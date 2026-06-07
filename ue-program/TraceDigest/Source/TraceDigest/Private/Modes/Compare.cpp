// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"
#include "Percentiles.h"

#include "ProfilingDebugging/MiscTrace.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/TimingProfiler.h"

using namespace TraceServices;

namespace TraceDigest
{

// Compact aggregate we compute per-timer per-side. Keeps just enough state for
// the cross-trace diff; we don't need full reservoir samples here because the
// digest of A and B already gives us percentiles.
struct FAggSide
{
	FString Name;
	uint64 Count = 0;
	double Total = 0.0;
	double Avg = 0.0;
	double P50 = 0.0;
	double P95 = 0.0;
	double P99 = 0.0;
	double Max = 0.0;
};

// Build a name -> aggregate map by reusing the same reservoir walk as digest.
// Note: we key by name (not TimerId), because TimerIds are not stable across
// traces — a fresh capture renumbers everything.
//
// All session reads happen under the ReadScope held in this function. Anything
// the caller needs after the scope releases (duration, etc.) is captured into
// out-params here, not read post-scope — that triggers an assert in
// AnalysisService::ReadAccessCheck.
static void BuildSide(const IAnalysisSession& Session,
                      const FString& Prefix,
                      TMap<FString, FAggSide>& OutBy_Name,
                      double& OutDurationMs)
{
	FAnalysisSessionReadScope Lock(Session);
	const ITimingProfilerProvider* Provider = ReadTimingProfilerProvider(Session);
	OutDurationMs = Session.GetDurationSeconds() * 1000.0;
	if (!Provider) return;

	const double EndSec = Session.GetDurationSeconds();

	TMap<uint32, FReservoir> ReservoirByTimer;
	ReservoirByTimer.Reserve(4096);

	Provider->EnumerateTimelines(
		[&](const ITimingProfilerProvider::Timeline& Timeline)
		{
			Timeline.EnumerateEvents(0.0, EndSec,
				[&](double StartTime, double EndTime, uint32, const FTimingProfilerEvent& Event) -> EEventEnumerate
				{
					const double DurationMs = (EndTime - StartTime) * 1000.0;
					FReservoir& R = ReservoirByTimer.FindOrAdd(Event.TimerIndex);
					R.Add(DurationMs);
					return EEventEnumerate::Continue;
				});
		});

	Provider->ReadTimers([&](const ITimingProfilerTimerReader& Reader)
	{
		for (TPair<uint32, FReservoir>& Pair : ReservoirByTimer)
		{
			const FTimingProfilerTimer* Timer = Reader.GetTimer(Pair.Key);
			if (!Timer || !Timer->Name) continue;
			if (Timer->Type != ETimingProfilerTimerType::CpuScope) continue;

			FString Name = Timer->Name;
			if (!Prefix.IsEmpty() && !Name.StartsWith(Prefix)) continue;

			// Merge into existing entry if multiple TimerIds share a display
			// name (rare but possible, e.g. metadata vs non-metadata variants).
			FAggSide& Agg = OutBy_Name.FindOrAdd(Name);
			Agg.Name = Name;
			Agg.Count += Pair.Value.GetCount();
			Agg.Total += Pair.Value.GetTotal();
			Agg.Max = FMath::Max(Agg.Max, Pair.Value.GetMax());

			double P50 = 0.0, P95 = 0.0, P99 = 0.0;
			Pair.Value.GetPercentiles(P50, P95, P99);
			// If we end up merging multiple TimerIds, the percentiles aren't
			// strictly correct (we'd need to re-sort across reservoirs) — but
			// keeping the max gives the closest single-number answer.
			Agg.P50 = FMath::Max(Agg.P50, P50);
			Agg.P95 = FMath::Max(Agg.P95, P95);
			Agg.P99 = FMath::Max(Agg.P99, P99);
			Agg.Avg = Agg.Count ? (Agg.Total / static_cast<double>(Agg.Count)) : 0.0;
		}
	});
}

void Modes::RunCompare(const IAnalysisSession& SessionA,
                       const IAnalysisSession& SessionB,
                       const FArgs& Args,
                       FJsonOut& Json)
{
	TMap<FString, FAggSide> A, B;
	double DurationAMs = 0.0, DurationBMs = 0.0;
	BuildSide(SessionA, Args.Prefix, A, DurationAMs);
	BuildSide(SessionB, Args.Prefix, B, DurationBMs);

	// Union of names — we want to surface events that exist on only one side
	// (those are the most interesting regressions: a new scope appearing or an
	// old one disappearing).
	TSet<FString> Names;
	for (const auto& Pair : A) Names.Add(Pair.Key);
	for (const auto& Pair : B) Names.Add(Pair.Key);

	struct FDiffRow
	{
		FString Name;
		FAggSide A_;
		FAggSide B_;
		double DeltaP95 = 0.0;
		double DeltaTotal = 0.0;
		bool OnlyInA = false;
		bool OnlyInB = false;
	};
	TArray<FDiffRow> Rows;
	Rows.Reserve(Names.Num());

	for (const FString& Name : Names)
	{
		FDiffRow Row;
		Row.Name = Name;
		if (const FAggSide* Sa = A.Find(Name)) { Row.A_ = *Sa; } else { Row.OnlyInB = true; }
		if (const FAggSide* Sb = B.Find(Name)) { Row.B_ = *Sb; } else { Row.OnlyInA = true; }
		Row.DeltaP95   = Row.B_.P95   - Row.A_.P95;
		Row.DeltaTotal = Row.B_.Total - Row.A_.Total;

		// Threshold filter: drop tiny deltas unless one side is missing
		// entirely (always interesting).
		if (Args.Threshold > 0.0 && !Row.OnlyInA && !Row.OnlyInB
			&& FMath::Abs(Row.DeltaP95) < Args.Threshold)
		{
			continue;
		}
		Rows.Add(MoveTemp(Row));
	}

	// Rank by |ΔP95| descending — the largest tail regressions first.
	Rows.Sort([](const FDiffRow& X, const FDiffRow& Y)
	{
		return FMath::Abs(X.DeltaP95) > FMath::Abs(Y.DeltaP95);
	});

	if (Args.Limit > 0 && Rows.Num() > Args.Limit)
	{
		Rows.SetNum(Args.Limit);
	}

	auto WriteSide = [&Json](const TCHAR* Key, const FAggSide& S)
	{
		Json.Key(Key);
		Json.BeginObject();
		Json.KeyInt(TEXT("count"),    static_cast<int64>(S.Count));
		Json.KeyNum(TEXT("total_ms"), S.Total);
		Json.KeyNum(TEXT("avg_ms"),   S.Avg);
		Json.KeyNum(TEXT("p50_ms"),   S.P50);
		Json.KeyNum(TEXT("p95_ms"),   S.P95);
		Json.KeyNum(TEXT("p99_ms"),   S.P99);
		Json.KeyNum(TEXT("max_ms"),   S.Max);
		Json.EndObject();
	};

	Json.BeginObject();
	Json.KeyStr(TEXT("file"),  Args.File);
	Json.KeyStr(TEXT("file2"), Args.File2);
	Json.KeyStr(TEXT("mode"),  FArgs::ModeName(Args.Mode));
	Json.KeyNum(TEXT("duration_a_ms"), DurationAMs);
	Json.KeyNum(TEXT("duration_b_ms"), DurationBMs);

	Json.Key(TEXT("events"));
	Json.BeginArray();
	for (const FDiffRow& Row : Rows)
	{
		Json.BeginObject();
		Json.KeyStr(TEXT("name"), Row.Name);
		Json.Key(TEXT("only_in_a")); Json.Bool(Row.OnlyInA);
		Json.Key(TEXT("only_in_b")); Json.Bool(Row.OnlyInB);
		Json.KeyNum(TEXT("delta_p95_ms"),   Row.DeltaP95);
		Json.KeyNum(TEXT("delta_total_ms"), Row.DeltaTotal);
		WriteSide(TEXT("a"), Row.A_);
		WriteSide(TEXT("b"), Row.B_);
		Json.EndObject();
	}
	Json.EndArray();
	Json.EndObject();
}

} // namespace TraceDigest

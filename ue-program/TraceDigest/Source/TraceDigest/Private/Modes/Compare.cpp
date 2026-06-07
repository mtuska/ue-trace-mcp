// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"
#include "Percentiles.h"
#include "ProviderReadScope.h"

#include "ProfilingDebugging/MiscTrace.h"
#include "TraceServices/Model/AllocationsProvider.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Counters.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Memory.h"
#include "TraceServices/Model/Regions.h"
#include "TraceServices/Model/TimingProfiler.h"

using namespace TraceServices;

namespace TraceDigest
{

// =============================================================================
// Event-like channels (cpu / gpu / region): same FAggSide row shape.
// =============================================================================

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

// Aggregate-by-name helper. Used by every event-like channel; the channel
// just chooses which Timing/Region timelines to walk.
static void IngestReservoir(const FString& Name,
                            FReservoir& Res,
                            TMap<FString, FAggSide>& OutByName)
{
	FAggSide& Agg = OutByName.FindOrAdd(Name);
	Agg.Name = Name;
	Agg.Count += Res.GetCount();
	Agg.Total += Res.GetTotal();
	Agg.Max = FMath::Max(Agg.Max, Res.GetMax());
	double P50 = 0.0, P95 = 0.0, P99 = 0.0;
	Res.GetPercentiles(P50, P95, P99);
	Agg.P50 = FMath::Max(Agg.P50, P50);
	Agg.P95 = FMath::Max(Agg.P95, P95);
	Agg.P99 = FMath::Max(Agg.P99, P99);
	Agg.Avg = Agg.Count ? (Agg.Total / static_cast<double>(Agg.Count)) : 0.0;
}

static void BuildCpuSide(const IAnalysisSession& Session, const FString& Prefix,
                         TMap<FString, FAggSide>& OutByName, double& OutDurationMs)
{
	FAnalysisSessionReadScope Lock(Session);
	const ITimingProfilerProvider* Provider = ReadTimingProfilerProvider(Session);
	OutDurationMs = Session.GetDurationSeconds() * 1000.0;
	if (!Provider) return;

	const double EndSec = Session.GetDurationSeconds();
	TMap<uint32, FReservoir> ByTimer;
	ByTimer.Reserve(4096);
	Provider->EnumerateTimelines(
		[&](const ITimingProfilerProvider::Timeline& Timeline)
		{
			Timeline.EnumerateEvents(0.0, EndSec,
				[&](double S, double E, uint32, const FTimingProfilerEvent& Ev) -> EEventEnumerate
				{
					ByTimer.FindOrAdd(Ev.TimerIndex).Add((E - S) * 1000.0);
					return EEventEnumerate::Continue;
				});
		});

	Provider->ReadTimers([&](const ITimingProfilerTimerReader& Reader)
	{
		for (TPair<uint32, FReservoir>& Pair : ByTimer)
		{
			const FTimingProfilerTimer* Timer = Reader.GetTimer(Pair.Key);
			if (!Timer || !Timer->Name) continue;
			if (Timer->Type != ETimingProfilerTimerType::CpuScope) continue;
			FString Name = Timer->Name;
			if (!Prefix.IsEmpty() && !Name.StartsWith(Prefix)) continue;
			IngestReservoir(Name, Pair.Value, OutByName);
		}
	});
}

static void BuildGpuSide(const IAnalysisSession& Session, const FString& Prefix,
                         TMap<FString, FAggSide>& OutByName, double& OutDurationMs)
{
	FAnalysisSessionReadScope Lock(Session);
	const ITimingProfilerProvider* Provider = ReadTimingProfilerProvider(Session);
	OutDurationMs = Session.GetDurationSeconds() * 1000.0;
	if (!Provider) return;

	const double EndSec = Session.GetDurationSeconds();
	TArray<uint32> Idxs;
	Provider->EnumerateGpuQueues([&](const FGpuQueueInfo& Q)
	{
		if (Q.TimelineIndex != ~0u) Idxs.Add(Q.TimelineIndex);
	});
	if (Idxs.Num() == 0)
	{
		uint32 Idx = ~0u;
		if (Provider->GetGpuTimelineIndex(Idx))  Idxs.Add(Idx);
		if (Provider->GetGpu2TimelineIndex(Idx)) Idxs.Add(Idx);
	}

	TMap<uint32, FReservoir> ByTimer;
	ByTimer.Reserve(2048);
	for (uint32 Idx : Idxs)
	{
		Provider->ReadTimeline(Idx, [&](const ITimingProfilerProvider::Timeline& Timeline)
		{
			Timeline.EnumerateEvents(0.0, EndSec,
				[&](double S, double E, uint32, const FTimingProfilerEvent& Ev) -> EEventEnumerate
				{
					ByTimer.FindOrAdd(Ev.TimerIndex).Add((E - S) * 1000.0);
					return EEventEnumerate::Continue;
				});
		});
	}

	Provider->ReadTimers([&](const ITimingProfilerTimerReader& Reader)
	{
		for (TPair<uint32, FReservoir>& Pair : ByTimer)
		{
			const FTimingProfilerTimer* Timer = Reader.GetTimer(Pair.Key);
			if (!Timer || !Timer->Name) continue;
			if (Timer->Type != ETimingProfilerTimerType::GpuScope) continue;
			FString Name = Timer->Name;
			if (!Prefix.IsEmpty() && !Name.StartsWith(Prefix)) continue;
			IngestReservoir(Name, Pair.Value, OutByName);
		}
	});
}

static void BuildRegionSide(const IAnalysisSession& Session, const FString& Prefix,
                            TMap<FString, FAggSide>& OutByName, double& OutDurationMs)
{
	FAnalysisSessionReadScope Lock(Session);
	const IRegionProvider& Provider = ReadRegionProvider(Session);
	FProviderReadScope ProviderLock(Provider);
	OutDurationMs = Session.GetDurationSeconds() * 1000.0;
	const double EndSec = Session.GetDurationSeconds();

	TMap<FString, FReservoir> ByName;
	ByName.Reserve(256);

	Provider.EnumerateTimelinesByCategory(
		[&](const IRegionTimeline& Timeline, const TCHAR*)
		{
			Timeline.EnumerateRegions(0.0, EndSec, [&](const FTimeRegion& R) -> bool
			{
				const FString Name = (R.Timer && R.Timer->Name) ? FString(R.Timer->Name) : FString();
				if (!Prefix.IsEmpty() && !Name.StartsWith(Prefix)) return true;
				const double End   = FMath::IsFinite(R.EndTime) ? FMath::Min(R.EndTime, EndSec) : EndSec;
				const double Begin = FMath::Max(R.BeginTime, 0.0);
				if (!FMath::IsFinite(End) || End < Begin) return true;
				ByName.FindOrAdd(Name).Add((End - Begin) * 1000.0);
				return true;
			});
		});

	for (TPair<FString, FReservoir>& Pair : ByName)
	{
		IngestReservoir(Pair.Key, Pair.Value, OutByName);
	}
}

// Diff two aggregated sides and emit the standard event-like compare row.
static void EmitEventDiff(const TMap<FString, FAggSide>& A,
                          const TMap<FString, FAggSide>& B,
                          const FArgs& Args,
                          FJsonOut& Json)
{
	TSet<FString> Names;
	for (const auto& Pair : A) Names.Add(Pair.Key);
	for (const auto& Pair : B) Names.Add(Pair.Key);

	struct FDiffRow
	{
		FString Name;
		FAggSide A_, B_;
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
		if (const FAggSide* Sa = A.Find(Name)) Row.A_ = *Sa; else Row.OnlyInB = true;
		if (const FAggSide* Sb = B.Find(Name)) Row.B_ = *Sb; else Row.OnlyInA = true;
		Row.DeltaP95   = Row.B_.P95   - Row.A_.P95;
		Row.DeltaTotal = Row.B_.Total - Row.A_.Total;
		if (Args.Threshold > 0.0 && !Row.OnlyInA && !Row.OnlyInB
			&& FMath::Abs(Row.DeltaP95) < Args.Threshold) continue;
		Rows.Add(MoveTemp(Row));
	}

	Rows.Sort([](const FDiffRow& X, const FDiffRow& Y)
	{
		return FMath::Abs(X.DeltaP95) > FMath::Abs(Y.DeltaP95);
	});
	if (Args.Limit > 0 && Rows.Num() > Args.Limit) Rows.SetNum(Args.Limit);

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

	Json.Key(TEXT("events"));
	Json.BeginArray();
	for (const FDiffRow& Row : Rows)
	{
		Json.BeginObject();
		Json.KeyStr (TEXT("name"),           Row.Name);
		Json.KeyBool(TEXT("only_in_a"),      Row.OnlyInA);
		Json.KeyBool(TEXT("only_in_b"),      Row.OnlyInB);
		Json.KeyNum (TEXT("delta_p95_ms"),   Row.DeltaP95);
		Json.KeyNum (TEXT("delta_total_ms"), Row.DeltaTotal);
		WriteSide(TEXT("a"), Row.A_);
		WriteSide(TEXT("b"), Row.B_);
		Json.EndObject();
	}
	Json.EndArray();
}

// =============================================================================
// Memory (LLM tags): compare per-tag peak bytes across both files.
// =============================================================================

static void BuildMemorySide(const IAnalysisSession& Session,
                            TMap<FString, int64>& OutPeakByTag, double& OutDurationMs)
{
	FAnalysisSessionReadScope Lock(Session);
	const IMemoryProvider* Provider = ReadMemoryProvider(Session);
	OutDurationMs = Session.GetDurationSeconds() * 1000.0;
	if (!Provider) return;
	FProviderReadScope ProviderLock(*Provider);

	const double EndSec = Session.GetDurationSeconds();
	// Default tracker (id 0) — most LLM-capable traces only run that one.
	const FMemoryTrackerId Tracker = 0;

	Provider->EnumerateTags([&](const FMemoryTagInfo& T)
	{
		int64 Peak = 0;
		Provider->EnumerateTagSamples(Tracker, T.Id, 0.0, EndSec,
			/*bIncludeRangeNeighbors*/ false,
			[&](double, double, const FMemoryTagSample& S)
			{
				if (S.Value > Peak) Peak = S.Value;
			});
		if (Peak > 0) OutPeakByTag.Add(T.Name, Peak);
	});
}

static void EmitMemoryDiff(const TMap<FString, int64>& A,
                            const TMap<FString, int64>& B,
                            const FArgs& Args, FJsonOut& Json)
{
	TSet<FString> Names;
	for (const auto& Pair : A) Names.Add(Pair.Key);
	for (const auto& Pair : B) Names.Add(Pair.Key);

	struct FRow { FString Name; int64 AP = 0, BP = 0, Delta = 0; bool OnlyInA = false, OnlyInB = false; };
	TArray<FRow> Rows;
	Rows.Reserve(Names.Num());
	for (const FString& Name : Names)
	{
		FRow R; R.Name = Name;
		if (const int64* P = A.Find(Name)) R.AP = *P; else R.OnlyInB = true;
		if (const int64* P = B.Find(Name)) R.BP = *P; else R.OnlyInA = true;
		R.Delta = R.BP - R.AP;
		Rows.Add(R);
	}
	Rows.Sort([](const FRow& X, const FRow& Y) { return FMath::Abs(X.Delta) > FMath::Abs(Y.Delta); });
	if (Args.Limit > 0 && Rows.Num() > Args.Limit) Rows.SetNum(Args.Limit);

	Json.Key(TEXT("events"));
	Json.BeginArray();
	for (const FRow& R : Rows)
	{
		Json.BeginObject();
		Json.KeyStr (TEXT("name"),              R.Name);
		Json.KeyInt (TEXT("a_peak_bytes"),      R.AP);
		Json.KeyInt (TEXT("b_peak_bytes"),      R.BP);
		Json.KeyInt (TEXT("delta_peak_bytes"),  R.Delta);
		Json.KeyBool(TEXT("only_in_a"),         R.OnlyInA);
		Json.KeyBool(TEXT("only_in_b"),         R.OnlyInB);
		Json.EndObject();
	}
	Json.EndArray();
}

// =============================================================================
// Counter: compare per-counter mean+max across both files.
// =============================================================================

struct FCounterAgg { uint64 Count = 0; double Sum = 0.0; double Max = 0.0; };

static void BuildCounterSide(const IAnalysisSession& Session,
                              TMap<FString, FCounterAgg>& OutByName, double& OutDurationMs)
{
	FAnalysisSessionReadScope Lock(Session);
	const ICounterProvider& Provider = ReadCounterProvider(Session);
	OutDurationMs = Session.GetDurationSeconds() * 1000.0;

	const double EndSec = Session.GetDurationSeconds();
	Provider.EnumerateCounters([&](uint32, const ICounter& C)
	{
		if (!C.GetName()) return;
		FString Name = C.GetName();
		FCounterAgg& Agg = OutByName.FindOrAdd(Name);
		if (C.IsFloatingPoint())
		{
			C.EnumerateFloatValues(0.0, EndSec, false,
				[&](double, double V)
				{
					++Agg.Count;
					Agg.Sum += V;
					if (V > Agg.Max) Agg.Max = V;
				});
		}
		else
		{
			C.EnumerateValues(0.0, EndSec, false,
				[&](double, int64 V)
				{
					const double Vd = static_cast<double>(V);
					++Agg.Count;
					Agg.Sum += Vd;
					if (Vd > Agg.Max) Agg.Max = Vd;
				});
		}
	});
}

static void EmitCounterDiff(const TMap<FString, FCounterAgg>& A,
                             const TMap<FString, FCounterAgg>& B,
                             const FArgs& Args, FJsonOut& Json)
{
	TSet<FString> Names;
	for (const auto& Pair : A) if (Pair.Value.Count > 0) Names.Add(Pair.Key);
	for (const auto& Pair : B) if (Pair.Value.Count > 0) Names.Add(Pair.Key);

	struct FRow
	{
		FString Name;
		double AMean = 0.0, BMean = 0.0, DeltaMean = 0.0;
		double AMax  = 0.0, BMax  = 0.0, DeltaMax  = 0.0;
		bool OnlyInA = false, OnlyInB = false;
	};
	TArray<FRow> Rows;
	Rows.Reserve(Names.Num());
	for (const FString& Name : Names)
	{
		FRow R; R.Name = Name;
		if (const FCounterAgg* P = A.Find(Name); P && P->Count > 0)
		{
			R.AMean = P->Sum / static_cast<double>(P->Count);
			R.AMax  = P->Max;
		}
		else R.OnlyInB = true;
		if (const FCounterAgg* P = B.Find(Name); P && P->Count > 0)
		{
			R.BMean = P->Sum / static_cast<double>(P->Count);
			R.BMax  = P->Max;
		}
		else R.OnlyInA = true;
		R.DeltaMean = R.BMean - R.AMean;
		R.DeltaMax  = R.BMax  - R.AMax;
		Rows.Add(R);
	}
	Rows.Sort([](const FRow& X, const FRow& Y) { return FMath::Abs(X.DeltaMax) > FMath::Abs(Y.DeltaMax); });
	if (Args.Limit > 0 && Rows.Num() > Args.Limit) Rows.SetNum(Args.Limit);

	Json.Key(TEXT("events"));
	Json.BeginArray();
	for (const FRow& R : Rows)
	{
		Json.BeginObject();
		Json.KeyStr (TEXT("name"),       R.Name);
		Json.KeyNum (TEXT("a_mean"),     R.AMean);
		Json.KeyNum (TEXT("b_mean"),     R.BMean);
		Json.KeyNum (TEXT("delta_mean"), R.DeltaMean);
		Json.KeyNum (TEXT("a_max"),      R.AMax);
		Json.KeyNum (TEXT("b_max"),      R.BMax);
		Json.KeyNum (TEXT("delta_max"),  R.DeltaMax);
		Json.KeyBool(TEXT("only_in_a"),  R.OnlyInA);
		Json.KeyBool(TEXT("only_in_b"),  R.OnlyInB);
		Json.EndObject();
	}
	Json.EndArray();
}

// =============================================================================
// Memalloc: compare aggregate metrics (peak total, max-live, alloc-events,
// free-events) across both files.
// =============================================================================

struct FAllocSummary
{
	double PeakTotal      = 0.0;
	double MaxLive        = 0.0;
	double AllocEvents    = 0.0;  // sum across all timeline points
	double FreeEvents     = 0.0;
};

static void BuildMemallocSide(const IAnalysisSession& Session, FAllocSummary& Out, double& OutDurationMs)
{
	FAnalysisSessionReadScope Lock(Session);
	const IAllocationsProvider* Provider = ReadAllocationsProvider(Session);
	OutDurationMs = Session.GetDurationSeconds() * 1000.0;
	if (!Provider) return;
	FProviderReadScope ProviderLock(*Provider);

	const int32 Points = Provider->GetTimelineNumPoints();
	if (Points <= 0) return;
	int32 Start = 0, End = Points - 1;
	Provider->GetTimelineIndexRange(0.0, Session.GetDurationSeconds(), Start, End);

	Provider->EnumerateTimeline(IAllocationsProvider::ETimelineU64::MaxTotalAllocatedMemory,
		Start, End,
		[&](double, double, uint64 V) { if ((double)V > Out.PeakTotal) Out.PeakTotal = (double)V; });
	Provider->EnumerateTimeline(IAllocationsProvider::ETimelineU32::MaxLiveAllocations,
		Start, End,
		[&](double, double, uint32 V) { if ((double)V > Out.MaxLive) Out.MaxLive = (double)V; });
	Provider->EnumerateTimeline(IAllocationsProvider::ETimelineU32::AllocEvents,
		Start, End,
		[&](double, double, uint32 V) { Out.AllocEvents += V; });
	Provider->EnumerateTimeline(IAllocationsProvider::ETimelineU32::FreeEvents,
		Start, End,
		[&](double, double, uint32 V) { Out.FreeEvents += V; });
}

static void EmitMemallocDiff(const FAllocSummary& A, const FAllocSummary& B, FJsonOut& Json)
{
	auto Row = [&Json](const TCHAR* Metric, double Av, double Bv)
	{
		Json.BeginObject();
		Json.KeyStr (TEXT("metric"),  FString(Metric));
		Json.KeyNum (TEXT("a_value"), Av);
		Json.KeyNum (TEXT("b_value"), Bv);
		Json.KeyNum (TEXT("delta"),   Bv - Av);
		Json.EndObject();
	};
	Json.Key(TEXT("events"));
	Json.BeginArray();
	Row(TEXT("peak_total_allocated_bytes"), A.PeakTotal,   B.PeakTotal);
	Row(TEXT("max_live_allocations"),       A.MaxLive,     B.MaxLive);
	Row(TEXT("alloc_events_total"),         A.AllocEvents, B.AllocEvents);
	Row(TEXT("free_events_total"),          A.FreeEvents,  B.FreeEvents);
	Json.EndArray();
}

// =============================================================================
// Top-level dispatch.
// =============================================================================

void Modes::RunCompare(const IAnalysisSession& SessionA,
                       const IAnalysisSession& SessionB,
                       const FArgs& Args,
                       FJsonOut& Json)
{
	const FString Channel = Args.Channel.IsEmpty() ? FString(TEXT("cpu")) : Args.Channel;
	double DurAMs = 0.0, DurBMs = 0.0;

	Json.BeginObject();
	Json.KeyStr(TEXT("file"),    Args.File);
	Json.KeyStr(TEXT("file2"),   Args.File2);
	Json.KeyStr(TEXT("mode"),    FArgs::ModeName(Args.Mode));
	Json.KeyStr(TEXT("channel"), Channel);

	if (Channel.Equals(TEXT("gpu"), ESearchCase::IgnoreCase))
	{
		TMap<FString, FAggSide> A, B;
		BuildGpuSide(SessionA, Args.Prefix, A, DurAMs);
		BuildGpuSide(SessionB, Args.Prefix, B, DurBMs);
		Json.KeyNum(TEXT("duration_a_ms"), DurAMs);
		Json.KeyNum(TEXT("duration_b_ms"), DurBMs);
		EmitEventDiff(A, B, Args, Json);
	}
	else if (Channel.Equals(TEXT("region"), ESearchCase::IgnoreCase))
	{
		TMap<FString, FAggSide> A, B;
		BuildRegionSide(SessionA, Args.Prefix, A, DurAMs);
		BuildRegionSide(SessionB, Args.Prefix, B, DurBMs);
		Json.KeyNum(TEXT("duration_a_ms"), DurAMs);
		Json.KeyNum(TEXT("duration_b_ms"), DurBMs);
		EmitEventDiff(A, B, Args, Json);
	}
	else if (Channel.Equals(TEXT("memory"), ESearchCase::IgnoreCase))
	{
		TMap<FString, int64> A, B;
		BuildMemorySide(SessionA, A, DurAMs);
		BuildMemorySide(SessionB, B, DurBMs);
		Json.KeyNum(TEXT("duration_a_ms"), DurAMs);
		Json.KeyNum(TEXT("duration_b_ms"), DurBMs);
		EmitMemoryDiff(A, B, Args, Json);
	}
	else if (Channel.Equals(TEXT("counter"), ESearchCase::IgnoreCase))
	{
		TMap<FString, FCounterAgg> A, B;
		BuildCounterSide(SessionA, A, DurAMs);
		BuildCounterSide(SessionB, B, DurBMs);
		Json.KeyNum(TEXT("duration_a_ms"), DurAMs);
		Json.KeyNum(TEXT("duration_b_ms"), DurBMs);
		EmitCounterDiff(A, B, Args, Json);
	}
	else if (Channel.Equals(TEXT("memalloc"), ESearchCase::IgnoreCase))
	{
		FAllocSummary A, B;
		BuildMemallocSide(SessionA, A, DurAMs);
		BuildMemallocSide(SessionB, B, DurBMs);
		Json.KeyNum(TEXT("duration_a_ms"), DurAMs);
		Json.KeyNum(TEXT("duration_b_ms"), DurBMs);
		EmitMemallocDiff(A, B, Json);
	}
	else  // default: cpu (unchanged v0.3/v0.4 shape)
	{
		TMap<FString, FAggSide> A, B;
		BuildCpuSide(SessionA, Args.Prefix, A, DurAMs);
		BuildCpuSide(SessionB, Args.Prefix, B, DurBMs);
		Json.KeyNum(TEXT("duration_a_ms"), DurAMs);
		Json.KeyNum(TEXT("duration_b_ms"), DurBMs);
		EmitEventDiff(A, B, Args, Json);
	}

	Json.EndObject();
}

} // namespace TraceDigest

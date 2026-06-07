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

// Composite "first question to ask" view. Combines:
//   * top-N events  (digest-style, but capped tighter — default 10)
//   * frame_stats   (min/avg/p50/p95/p99/max of Game-thread frame durations)
//   * slowest_frames (top-3 by duration with their idx)
//
// All produced in one editor invocation — saves the LLM from spawning the
// editor three times to ask "what's hot? how's the framerate? where are the
// spikes?". The events array reuses the digest schema verbatim.
void Modes::RunOverview(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const ITimingProfilerProvider* TimingProvider = ReadTimingProfilerProvider(Session);
	const IFrameProvider& Frames = ReadFrameProvider(Session);

	const double EndSec = Session.GetDurationSeconds();
	const uint64 GameCount = Frames.GetFrameCount(TraceFrameType_Game);

	// --- Frame stats ---
	FReservoir FrameDurations(static_cast<int32>(FMath::Max<uint64>(GameCount, 1)));
	TArray<TPair<uint64, double>> AllFrames;
	AllFrames.Reserve(static_cast<int32>(GameCount));

	Frames.EnumerateFrames(TraceFrameType_Game, 0, GameCount,
		[&](const TraceServices::FFrame& F)
		{
			const double Ms = (F.EndTime - F.StartTime) * 1000.0;
			FrameDurations.Add(Ms);
			AllFrames.Add({ F.Index, Ms });
		});

	double Fp50 = 0.0, Fp95 = 0.0, Fp99 = 0.0;
	FrameDurations.GetPercentiles(Fp50, Fp95, Fp99);

	// --- Slowest frames ---
	AllFrames.Sort([](const TPair<uint64, double>& A, const TPair<uint64, double>& B)
	{
		return A.Value > B.Value;
	});
	const int32 SlowestCount = FMath::Min(3, AllFrames.Num());

	// --- Top events (digest-style, but with the Overview's own limit) ---
	const int32 EventLimit = Args.Limit > 0 ? FMath::Min(Args.Limit, 10) : 10;
	TMap<uint32, FReservoir> ReservoirByTimer;
	ReservoirByTimer.Reserve(4096);

	if (TimingProvider)
	{
		TimingProvider->EnumerateTimelines(
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
	}

	struct FRow { FString Name; FReservoir* R = nullptr; };
	TArray<FRow> Rows;
	Rows.Reserve(ReservoirByTimer.Num());

	if (TimingProvider)
	{
		TimingProvider->ReadTimers([&](const ITimingProfilerTimerReader& Reader)
		{
			for (TPair<uint32, FReservoir>& Pair : ReservoirByTimer)
			{
				const FTimingProfilerTimer* Timer = Reader.GetTimer(Pair.Key);
				if (!Timer || !Timer->Name) continue;
				if (Timer->Type != ETimingProfilerTimerType::CpuScope) continue;

				FString Name = Timer->Name;
				if (!Args.Prefix.IsEmpty() && !Name.StartsWith(Args.Prefix)) continue;
				Rows.Add({ MoveTemp(Name), &Pair.Value });
			}
		});
	}

	Rows.Sort([](const FRow& A, const FRow& B) { return A.R->GetTotal() > B.R->GetTotal(); });
	if (Rows.Num() > EventLimit) Rows.SetNum(EventLimit);

	// --- Emit ---
	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyNum(TEXT("duration_ms"), EndSec * 1000.0);
	Json.KeyInt(TEXT("frame_count"), static_cast<int64>(GameCount));

	Json.Key(TEXT("frame_stats"));
	Json.BeginObject();
	Json.KeyNum(TEXT("min_ms"), FrameDurations.GetMin());
	Json.KeyNum(TEXT("avg_ms"), FrameDurations.GetAvg());
	Json.KeyNum(TEXT("p50_ms"), Fp50);
	Json.KeyNum(TEXT("p95_ms"), Fp95);
	Json.KeyNum(TEXT("p99_ms"), Fp99);
	Json.KeyNum(TEXT("max_ms"), FrameDurations.GetMax());
	Json.EndObject();

	Json.Key(TEXT("slowest_frames"));
	Json.BeginArray();
	for (int32 i = 0; i < SlowestCount; ++i)
	{
		Json.BeginObject();
		Json.KeyInt(TEXT("idx"),         static_cast<int64>(AllFrames[i].Key));
		Json.KeyNum(TEXT("duration_ms"), AllFrames[i].Value);
		Json.EndObject();
	}
	Json.EndArray();

	Json.Key(TEXT("events"));
	Json.BeginArray();
	for (FRow& Row : Rows)
	{
		double P50 = 0.0, P95 = 0.0, P99 = 0.0;
		Row.R->GetPercentiles(P50, P95, P99);

		Json.BeginObject();
		Json.KeyStr(TEXT("name"),     Row.Name);
		Json.KeyInt(TEXT("count"),    static_cast<int64>(Row.R->GetCount()));
		Json.KeyNum(TEXT("total_ms"), Row.R->GetTotal());
		Json.KeyNum(TEXT("avg_ms"),   Row.R->GetAvg());
		Json.KeyNum(TEXT("p50_ms"),   P50);
		Json.KeyNum(TEXT("p95_ms"),   P95);
		Json.KeyNum(TEXT("p99_ms"),   P99);
		Json.KeyNum(TEXT("max_ms"),   Row.R->GetMax());
		Json.EndObject();
	}
	Json.EndArray();
	Json.EndObject();
}

} // namespace TraceDigest

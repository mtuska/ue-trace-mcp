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

// One row in the digest output. We hold a pointer into the per-timer reservoir
// map so we don't copy the sample buffer; the map outlives this struct.
struct FDigestRow
{
	FString Name;
	uint32 TimerId = 0;
	FReservoir* Reservoir = nullptr;
};

static void EmitFrameCount(const IAnalysisSession& Session, FJsonOut& Json)
{
	// We expose Game-thread frames as the canonical frame count; Insights uses
	// the same default. Rendering frame count is available via mode=frames if
	// the caller cares.
	const IFrameProvider& Frames = ReadFrameProvider(Session);
	Json.KeyInt(TEXT("frame_count"), static_cast<int64>(Frames.GetFrameCount(TraceFrameType_Game)));
}

void Modes::RunDigest(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);

	const ITimingProfilerProvider* TimingProvider = ReadTimingProfilerProvider(Session);

	// Build TimerId -> reservoir of inclusive instance durations (ms).
	TMap<uint32, FReservoir> ReservoirByTimer;
	ReservoirByTimer.Reserve(4096);

	const double EndSec = Session.GetDurationSeconds();

	if (TimingProvider)
	{
		TimingProvider->EnumerateTimelines(
			[&ReservoirByTimer, EndSec](const ITimingProfilerProvider::Timeline& Timeline)
			{
				Timeline.EnumerateEvents(0.0, EndSec,
					[&ReservoirByTimer](double StartTime, double EndTime, uint32 /*Depth*/, const FTimingProfilerEvent& Event) -> EEventEnumerate
					{
						const double DurationMs = (EndTime - StartTime) * 1000.0;
						FReservoir& R = ReservoirByTimer.FindOrAdd(Event.TimerIndex);
						R.Add(DurationMs);
						return EEventEnumerate::Continue;
					});
			});
	}

	// Resolve timer names and apply prefix filter under the timer-reader lock.
	TArray<FDigestRow> Rows;
	Rows.Reserve(ReservoirByTimer.Num());

	if (TimingProvider)
	{
		TimingProvider->ReadTimers(
			[&](const ITimingProfilerTimerReader& Reader)
			{
				for (TPair<uint32, FReservoir>& Pair : ReservoirByTimer)
				{
					const FTimingProfilerTimer* Timer = Reader.GetTimer(Pair.Key);
					if (!Timer || !Timer->Name)
					{
						continue;
					}

					// CPU only for v1. GPU/Verse timelines are kept out of the
					// digest entirely until we have a story for differentiating
					// them in the JSON.
					if (Timer->Type != ETimingProfilerTimerType::CpuScope)
					{
						continue;
					}

					FString Name = Timer->Name;
					if (!Args.Prefix.IsEmpty() && !Name.StartsWith(Args.Prefix))
					{
						continue;
					}

					Rows.Add({ MoveTemp(Name), Pair.Key, &Pair.Value });
				}
			});
	}

	// Hot scopes first.
	Rows.Sort([](const FDigestRow& A, const FDigestRow& B)
	{
		return A.Reservoir->GetTotal() > B.Reservoir->GetTotal();
	});

	if (Args.Limit > 0 && Rows.Num() > Args.Limit)
	{
		Rows.SetNum(Args.Limit);
	}

	// Emit JSON.
	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyNum(TEXT("duration_ms"), EndSec * 1000.0);
	EmitFrameCount(Session, Json);

	Json.Key(TEXT("events"));
	Json.BeginArray();
	for (FDigestRow& Row : Rows)
	{
		double P50 = 0.0, P95 = 0.0, P99 = 0.0;
		Row.Reservoir->GetPercentiles(P50, P95, P99);

		Json.BeginObject();
		Json.KeyStr(TEXT("name"),     Row.Name);
		Json.KeyInt(TEXT("count"),    static_cast<int64>(Row.Reservoir->GetCount()));
		Json.KeyNum(TEXT("total_ms"), Row.Reservoir->GetTotal());
		Json.KeyNum(TEXT("avg_ms"),   Row.Reservoir->GetAvg());
		Json.KeyNum(TEXT("p50_ms"),   P50);
		Json.KeyNum(TEXT("p95_ms"),   P95);
		Json.KeyNum(TEXT("p99_ms"),   P99);
		Json.KeyNum(TEXT("max_ms"),   Row.Reservoir->GetMax());
		Json.EndObject();
	}
	Json.EndArray();
	Json.EndObject();
}

} // namespace TraceDigest

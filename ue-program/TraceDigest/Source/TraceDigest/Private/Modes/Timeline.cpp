// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"
#include "TimerLookup.h"
#include "ProviderReadScope.h"

#include "ProfilingDebugging/MiscTrace.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Regions.h"
#include "TraceServices/Model/TimingProfiler.h"

using namespace TraceServices;

namespace TraceDigest
{

namespace {

// Walk every CPU+GPU timeline and emit instances whose TimerIndex matches
// `TargetTimers`. For -channel=cpu we use EnumerateTimelines (which covers
// every CPU timeline plus GPU; events whose timer name matches *and* live
// on a CPU timeline naturally pass because GPU events on GPU timelines
// have different timer ids — they just happen to share the display name).
// For -channel=gpu we iterate only GPU queue timelines so we never see CPU
// instances.
static void EmitTimingInstances(const ITimingProfilerProvider& Timing,
                                 const TSet<uint32>& TargetTimers,
                                 const IFrameProvider& Frames,
                                 double WindowStart, double WindowEnd,
                                 bool bGpuOnly, FJsonOut& Json)
{
	const auto Walk = [&](const ITimingProfilerProvider::Timeline& Timeline)
	{
		Timeline.EnumerateEvents(WindowStart, WindowEnd,
			[&](double StartTime, double EndTime, uint32 /*Depth*/, const FTimingProfilerEvent& Event) -> EEventEnumerate
			{
				if (!TargetTimers.Contains(Event.TimerIndex)) return EEventEnumerate::Continue;
				const uint32 FrameIdx = Frames.GetFrameNumberForTimestamp(TraceFrameType_Game, StartTime);
				Json.BeginObject();
				Json.KeyInt(TEXT("frame_idx"),    static_cast<int64>(FrameIdx));
				Json.KeyNum(TEXT("start_ms"),     StartTime * 1000.0);
				Json.KeyNum(TEXT("duration_ms"), (EndTime - StartTime) * 1000.0);
				Json.EndObject();
				return EEventEnumerate::Continue;
			});
	};

	if (bGpuOnly)
	{
		TArray<uint32> Idxs;
		Timing.EnumerateGpuQueues([&](const FGpuQueueInfo& Q)
		{
			if (Q.TimelineIndex != ~0u) Idxs.Add(Q.TimelineIndex);
		});
		// Fallback: legacy Gpu1/Gpu2 timelines for traces that predate the
		// FGpuQueueInfo API.
		if (Idxs.Num() == 0)
		{
			uint32 Idx = ~0u;
			if (Timing.GetGpuTimelineIndex(Idx))  Idxs.Add(Idx);
			if (Timing.GetGpu2TimelineIndex(Idx)) Idxs.Add(Idx);
		}
		for (uint32 Idx : Idxs)
		{
			Timing.ReadTimeline(Idx, Walk);
		}
	}
	else
	{
		Timing.EnumerateTimelines(Walk);
	}
}

static void EmitRegionInstances(const IAnalysisSession& Session,
                                 const FString& EventName,
                                 const IFrameProvider& Frames,
                                 double WindowStart, double WindowEnd,
                                 FJsonOut& Json)
{
	const IRegionProvider& Provider = ReadRegionProvider(Session);
	FProviderReadScope ProviderLock(Provider);
	Provider.EnumerateTimelinesByCategory(
		[&](const IRegionTimeline& Timeline, const TCHAR*)
		{
			Timeline.EnumerateRegions(WindowStart, WindowEnd, [&](const FTimeRegion& R) -> bool
			{
				const FString Name = (R.Timer && R.Timer->Name) ? FString(R.Timer->Name) : FString();
				if (!Name.Equals(EventName, ESearchCase::IgnoreCase)) return true;
				// Clamp open-ended regions to the trace window so JSON numbers
				// stay finite (open regions default to EndTime=+inf).
				const double Begin = FMath::Max(R.BeginTime, WindowStart);
				const double End   = FMath::Min(R.EndTime,   WindowEnd);
				if (!FMath::IsFinite(End) || !FMath::IsFinite(Begin) || End < Begin) return true;
				const uint32 FrameIdx = Frames.GetFrameNumberForTimestamp(TraceFrameType_Game, Begin);
				Json.BeginObject();
				Json.KeyInt(TEXT("frame_idx"),    static_cast<int64>(FrameIdx));
				Json.KeyNum(TEXT("start_ms"),     Begin * 1000.0);
				Json.KeyNum(TEXT("duration_ms"), (End - Begin) * 1000.0);
				Json.EndObject();
				return true;
			});
		});
}

} // namespace

void Modes::RunTimeline(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const ITimingProfilerProvider* TimingProvider = ReadTimingProfilerProvider(Session);
	const IFrameProvider& Frames = ReadFrameProvider(Session);

	const double EndSec = Session.GetDurationSeconds();
	double WindowStart = 0.0;
	double WindowEnd = EndSec;
	if (Args.FrameRangeStart >= 0 && Args.FrameRangeEnd > Args.FrameRangeStart)
	{
		if (const TraceServices::FFrame* F0 = Frames.GetFrame(TraceFrameType_Game, Args.FrameRangeStart))
		{
			WindowStart = F0->StartTime;
		}
		if (const TraceServices::FFrame* F1 = Frames.GetFrame(TraceFrameType_Game, Args.FrameRangeEnd - 1))
		{
			WindowEnd = F1->EndTime;
		}
	}

	Json.BeginObject();
	Json.KeyStr(TEXT("file"),    Args.File);
	Json.KeyStr(TEXT("mode"),    FArgs::ModeName(Args.Mode));
	Json.KeyStr(TEXT("channel"), Args.Channel);
	Json.KeyStr(TEXT("event"),   Args.Event);
	Json.KeyNum(TEXT("duration_ms"), EndSec * 1000.0);
	Json.KeyInt(TEXT("frame_count"), static_cast<int64>(Frames.GetFrameCount(TraceFrameType_Game)));

	Json.Key(TEXT("events"));
	Json.BeginArray();

	if (Args.Channel.Equals(TEXT("region"), ESearchCase::IgnoreCase))
	{
		EmitRegionInstances(Session, Args.Event, Frames, WindowStart, WindowEnd, Json);
	}
	else
	{
		const bool bGpu = Args.Channel.Equals(TEXT("gpu"), ESearchCase::IgnoreCase);
		TSet<uint32> TargetTimers;
		if (TimingProvider)
		{
			FindTimerIdsByName(*TimingProvider, Args.Event, TargetTimers);
		}
		if (TimingProvider && TargetTimers.Num() > 0)
		{
			EmitTimingInstances(*TimingProvider, TargetTimers, Frames, WindowStart, WindowEnd, bGpu, Json);
		}
	}

	Json.EndArray();
	Json.EndObject();
}

} // namespace TraceDigest

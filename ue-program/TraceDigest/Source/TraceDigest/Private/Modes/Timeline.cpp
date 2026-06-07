// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"
#include "TimerLookup.h"

#include "ProfilingDebugging/MiscTrace.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/TimingProfiler.h"

using namespace TraceServices;

namespace TraceDigest
{

void Modes::RunTimeline(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const ITimingProfilerProvider* TimingProvider = ReadTimingProfilerProvider(Session);
	const IFrameProvider& Frames = ReadFrameProvider(Session);

	TSet<uint32> TargetTimers;
	if (TimingProvider)
	{
		FindTimerIdsByName(*TimingProvider, Args.Event, TargetTimers);
	}

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
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyStr(TEXT("event"), Args.Event);
	Json.KeyNum(TEXT("duration_ms"), EndSec * 1000.0);
	Json.KeyInt(TEXT("frame_count"), static_cast<int64>(Frames.GetFrameCount(TraceFrameType_Game)));

	Json.Key(TEXT("events"));
	Json.BeginArray();

	if (TimingProvider && TargetTimers.Num() > 0)
	{
		TimingProvider->EnumerateTimelines(
			[&](const ITimingProfilerProvider::Timeline& Timeline)
			{
				Timeline.EnumerateEvents(WindowStart, WindowEnd,
					[&](double StartTime, double EndTime, uint32 /*Depth*/, const FTimingProfilerEvent& Event) -> EEventEnumerate
					{
						if (!TargetTimers.Contains(Event.TimerIndex))
						{
							return EEventEnumerate::Continue;
						}

						const uint32 FrameIdx = Frames.GetFrameNumberForTimestamp(TraceFrameType_Game, StartTime);
						Json.BeginObject();
						Json.KeyInt(TEXT("frame_idx"),    static_cast<int64>(FrameIdx));
						Json.KeyNum(TEXT("start_ms"),     StartTime * 1000.0);
						Json.KeyNum(TEXT("duration_ms"), (EndTime - StartTime) * 1000.0);
						Json.EndObject();
						return EEventEnumerate::Continue;
					});
			});
	}

	Json.EndArray();
	Json.EndObject();
}

} // namespace TraceDigest

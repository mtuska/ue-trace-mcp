// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"

#include "ProfilingDebugging/MiscTrace.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"

using namespace TraceServices;

namespace TraceDigest
{

// Returns every CPU event that ran during a single Game-thread frame, sorted
// by duration descending and capped at Args.Limit. The use case is post-spike
// drill-down: trace_frames says frame 4203 was 47ms, this tool says *what was
// running* during those 47ms across every thread.
void Modes::RunFrame(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const ITimingProfilerProvider* TimingProvider = ReadTimingProfilerProvider(Session);
	const IFrameProvider& Frames = ReadFrameProvider(Session);
	const IThreadProvider& Threads = ReadThreadProvider(Session);

	const TraceServices::FFrame* Frame = Frames.GetFrame(TraceFrameType_Game, static_cast<uint64>(Args.FrameIndex));

	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyInt(TEXT("frame_idx"), static_cast<int64>(Args.FrameIndex));

	if (!Frame || !TimingProvider)
	{
		Json.KeyNum(TEXT("start_ms"), 0.0);
		Json.KeyNum(TEXT("end_ms"), 0.0);
		Json.KeyNum(TEXT("duration_ms"), 0.0);
		Json.Key(TEXT("events"));
		Json.BeginArray();
		Json.EndArray();
		Json.EndObject();
		return;
	}

	const double StartSec = Frame->StartTime;
	const double EndSec   = Frame->EndTime;

	Json.KeyNum(TEXT("start_ms"),    StartSec * 1000.0);
	Json.KeyNum(TEXT("end_ms"),      EndSec   * 1000.0);
	Json.KeyNum(TEXT("duration_ms"), (EndSec - StartSec) * 1000.0);

	// Map TimelineIndex -> thread display name. CPU timelines are addressed
	// by ThreadId via GetCpuThreadTimelineIndex; we invert that into a lookup.
	TMap<uint32, FString> TimelineToThread;
	Threads.EnumerateThreads([&](const FThreadInfo& Info)
	{
		uint32 TimelineIdx = 0;
		if (TimingProvider->GetCpuThreadTimelineIndex(Info.Id, TimelineIdx))
		{
			TimelineToThread.Add(TimelineIdx, Info.Name ? FString(Info.Name) : FString::Printf(TEXT("Thread_%u"), Info.Id));
		}
	});

	// We need timer names later; pull them all into a side map under the
	// timer-reader lock so we don't take it once per event.
	TMap<uint32, FString> TimerName;
	TimingProvider->ReadTimers([&](const ITimingProfilerTimerReader& Reader)
	{
		const uint32 N = Reader.GetTimerCount();
		for (uint32 I = 0; I < N; ++I)
		{
			const FTimingProfilerTimer* T = Reader.GetTimer(I);
			if (T && T->Name && T->Type == ETimingProfilerTimerType::CpuScope)
			{
				TimerName.Add(T->Id, T->Name);
			}
		}
	});

	struct FRow
	{
		FString Name;
		FString Thread;
		uint32 Depth;
		double StartMs;
		double DurationMs;
	};
	TArray<FRow> Rows;

	const uint32 TimelineCount = TimingProvider->GetTimelineCount();
	for (uint32 i = 0; i < TimelineCount; ++i)
	{
		TimingProvider->ReadTimeline(i, [&](const ITimingProfilerProvider::Timeline& Timeline)
		{
			const FString* ThreadName = TimelineToThread.Find(i);
			Timeline.EnumerateEvents(StartSec, EndSec,
				[&](double S, double E, uint32 Depth, const FTimingProfilerEvent& Event) -> EEventEnumerate
				{
					// Skip events that started before this frame (rare partial-overlap
					// from a long-running scope that began earlier).
					if (S < StartSec) return EEventEnumerate::Continue;

					const FString* Name = TimerName.Find(Event.TimerIndex);
					if (!Name) return EEventEnumerate::Continue;

					FRow R;
					R.Name       = *Name;
					R.Thread     = ThreadName ? *ThreadName : FString();
					R.Depth      = Depth;
					R.StartMs    = S * 1000.0;
					R.DurationMs = (E - S) * 1000.0;
					Rows.Add(MoveTemp(R));
					return EEventEnumerate::Continue;
				});
		});
	}

	// Longest events first — that's what the LLM cares about for spike analysis.
	Rows.Sort([](const FRow& A, const FRow& B) { return A.DurationMs > B.DurationMs; });
	if (Args.Limit > 0 && Rows.Num() > Args.Limit) Rows.SetNum(Args.Limit);

	Json.Key(TEXT("events"));
	Json.BeginArray();
	for (const FRow& R : Rows)
	{
		Json.BeginObject();
		Json.KeyStr(TEXT("name"),         R.Name);
		Json.KeyStr(TEXT("thread"),       R.Thread);
		Json.KeyInt(TEXT("depth"),        static_cast<int64>(R.Depth));
		Json.KeyNum(TEXT("start_ms"),     R.StartMs);
		Json.KeyNum(TEXT("duration_ms"),  R.DurationMs);
		Json.EndObject();
	}
	Json.EndArray();
	Json.EndObject();
}

} // namespace TraceDigest

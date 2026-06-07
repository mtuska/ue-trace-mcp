// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"

#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/TimingProfiler.h"

using namespace TraceServices;

namespace TraceDigest
{

// Dispatched via `-view=queues|timeline|fences`. All three pull from the
// same ITimingProfilerProvider that the CPU side already uses. GPU timelines
// reuse the same ITimeline<FTimingProfilerEvent> interface as CPU threads,
// just keyed by GPU-queue timeline index instead of CPU-thread timeline
// index.

static void RunGpuQueues(const ITimingProfilerProvider& Timing, const FArgs& Args, FJsonOut& Json, double EndSec)
{
	Json.KeyBool(TEXT("has_gpu"), Timing.HasGpuTiming());
	Json.KeyNum(TEXT("duration_ms"), EndSec * 1000.0);

	Json.Key(TEXT("queues"));
	Json.BeginArray();
	int32 NewStyleCount = 0;
	Timing.EnumerateGpuQueues([&](const FGpuQueueInfo& Q)
	{
		++NewStyleCount;
		Json.BeginObject();
		Json.KeyInt(TEXT("id"),                 static_cast<int64>(Q.Id));
		Json.KeyInt(TEXT("gpu"),                static_cast<int64>(Q.GPU));
		Json.KeyInt(TEXT("index"),              static_cast<int64>(Q.Index));
		Json.KeyInt(TEXT("type"),               static_cast<int64>(Q.Type));
		Json.KeyStr(TEXT("name"),               Q.Name ? FString(Q.Name) : FString());
		Json.KeyStr(TEXT("display_name"),       Q.GetDisplayName());
		Json.KeyInt(TEXT("timeline_index"),     static_cast<int64>(Q.TimelineIndex));
		Json.KeyInt(TEXT("work_timeline_index"), static_cast<int64>(Q.WorkTimelineIndex));
		Json.KeyStr(TEXT("api"),                TEXT("queue"));  // FGpuQueueInfo
		Json.EndObject();
	});

	// Legacy GPU-insights timelines (Gpu1/Gpu2). Older traces and a number of
	// in-the-wild Insights captures expose GPU work this way instead of via
	// FGpuQueueInfo. We surface them as pseudo-queues with id=0/1 so the
	// timeline / fences views can find them.
	if (NewStyleCount == 0)
	{
		uint32 Idx = ~0u;
		if (Timing.GetGpuTimelineIndex(Idx))
		{
			Json.BeginObject();
			Json.KeyInt(TEXT("id"),                 0);
			Json.KeyInt(TEXT("gpu"),                0);
			Json.KeyInt(TEXT("index"),              0);
			Json.KeyInt(TEXT("type"),               0);
			Json.KeyStr(TEXT("name"),               TEXT("Gpu1"));
			Json.KeyStr(TEXT("display_name"),       TEXT("Gpu1 (legacy)"));
			Json.KeyInt(TEXT("timeline_index"),     static_cast<int64>(Idx));
			Json.KeyInt(TEXT("work_timeline_index"), static_cast<int64>(Idx));
			Json.KeyStr(TEXT("api"),                TEXT("legacy"));
			Json.EndObject();
		}
		Idx = ~0u;
		if (Timing.GetGpu2TimelineIndex(Idx))
		{
			Json.BeginObject();
			Json.KeyInt(TEXT("id"),                 1);
			Json.KeyInt(TEXT("gpu"),                0);
			Json.KeyInt(TEXT("index"),              1);
			Json.KeyInt(TEXT("type"),               0);
			Json.KeyStr(TEXT("name"),               TEXT("Gpu2"));
			Json.KeyStr(TEXT("display_name"),       TEXT("Gpu2 (legacy)"));
			Json.KeyInt(TEXT("timeline_index"),     static_cast<int64>(Idx));
			Json.KeyInt(TEXT("work_timeline_index"), static_cast<int64>(Idx));
			Json.KeyStr(TEXT("api"),                TEXT("legacy"));
			Json.EndObject();
		}
	}
	Json.EndArray();
}

static void RunGpuFences(const ITimingProfilerProvider& Timing, const FArgs& Args, FJsonOut& Json, double EndSec)
{
	const int32 Limit = Args.Limit > 0 ? Args.Limit : 200;

	Json.KeyBool(TEXT("has_gpu"), Timing.HasGpuTiming());
	Json.KeyNum (TEXT("duration_ms"), EndSec * 1000.0);
	Json.KeyInt (TEXT("queue_filter"), static_cast<int64>(Args.Queue));  // -1 = all queues

	// Walk every queue (or just the one named via -queue=) and gather the
	// resolved fence pairs — i.e. signal-on-queue-A matched with wait-on-
	// queue-B for the same fence Value. Resolved pairs are the useful unit
	// for "find me cross-queue stalls".
	int32 Emitted = 0;
	int32 Total = 0;

	Json.Key(TEXT("events"));
	Json.BeginArray();

	TArray<uint32> QueueIds;
	Timing.EnumerateGpuQueues([&](const FGpuQueueInfo& Q)
	{
		QueueIds.Add(Q.Id);
	});

	for (uint32 QueueId : QueueIds)
	{
		if (Args.Queue >= 0 && static_cast<uint32>(Args.Queue) != QueueId) continue;
		Timing.EnumerateResolvedGpuFences(QueueId, 0.0, EndSec,
			[&](uint32 SignalQueueId, const FGpuSignalFence& Signal,
			    uint32 WaitQueueId,   const FGpuWaitFence&   Wait) -> EEnumerateResult
			{
				++Total;
				if (Emitted >= Limit) return EEnumerateResult::Continue;
				Json.BeginObject();
				Json.KeyInt(TEXT("signal_queue_id"), static_cast<int64>(SignalQueueId));
				Json.KeyNum(TEXT("signal_time_ms"), Signal.Timestamp * 1000.0);
				Json.KeyInt(TEXT("signal_value"),   static_cast<int64>(Signal.Value));
				Json.KeyInt(TEXT("wait_queue_id"),  static_cast<int64>(WaitQueueId));
				Json.KeyNum(TEXT("wait_time_ms"),   Wait.Timestamp * 1000.0);
				Json.KeyInt(TEXT("wait_value"),     static_cast<int64>(Wait.Value));
				Json.KeyNum(TEXT("stall_ms"),       (Wait.Timestamp - Signal.Timestamp) * 1000.0);
				Json.EndObject();
				++Emitted;
				return EEnumerateResult::Continue;
			});
	}
	Json.EndArray();

	Json.KeyInt (TEXT("total_in_window"), static_cast<int64>(Total));
	Json.KeyBool(TEXT("truncated"),        Total > Emitted);
}

static void RunGpuTimeline(const ITimingProfilerProvider& Timing, const FArgs& Args, FJsonOut& Json, double EndSec)
{
	const int32 Limit = Args.Limit > 0 ? Args.Limit : 200;

	Json.KeyBool(TEXT("has_gpu"), Timing.HasGpuTiming());
	Json.KeyNum (TEXT("duration_ms"), EndSec * 1000.0);
	Json.KeyInt (TEXT("queue_filter"), static_cast<int64>(Args.Queue));  // -1 = all queues

	// Per-queue digest: aggregate inclusive_ms per timer name across the
	// selected queues. Output shape matches CPU `trace_threads` top_timers:
	// callers walk each "queue" sub-object and read its `top_timers` array.
	struct FTimerAgg { uint64 Count = 0; double InclusiveMs = 0.0; };
	struct FQueueAgg
	{
		uint32 Id = 0;
		FString DisplayName;
		uint32 TimelineIndex = ~0;
		uint64 EventCount = 0;
		TMap<uint32 /*TimerIdx*/, FTimerAgg> PerTimer;
	};

	TArray<FQueueAgg> Queues;
	Timing.EnumerateGpuQueues([&](const FGpuQueueInfo& Q)
	{
		if (Args.Queue >= 0 && static_cast<uint32>(Args.Queue) != Q.Id) return;
		FQueueAgg Row;
		Row.Id = Q.Id;
		Row.DisplayName = Q.GetDisplayName();
		Row.TimelineIndex = Q.TimelineIndex;
		Queues.Add(MoveTemp(Row));
	});

	for (FQueueAgg& Q : Queues)
	{
		if (Q.TimelineIndex == ~0u) continue;
		Timing.ReadTimeline(Q.TimelineIndex,
			[&](const ITimingProfilerProvider::Timeline& Timeline)
			{
				Timeline.EnumerateEvents(0.0, EndSec,
					[&](double S, double E, uint32, const FTimingProfilerEvent& Event) -> EEventEnumerate
					{
						Q.EventCount += 1;
						FTimerAgg& T = Q.PerTimer.FindOrAdd(Event.TimerIndex);
						T.Count += 1;
						T.InclusiveMs += (E - S) * 1000.0;
						return EEventEnumerate::Continue;
					});
			});
	}

	// Resolve timer names so the JSON has readable rows.
	TMap<uint32, FString> TimerName;
	Timing.ReadTimers([&](const ITimingProfilerTimerReader& Reader)
	{
		for (const FQueueAgg& Q : Queues)
		{
			for (const TPair<uint32, FTimerAgg>& TP : Q.PerTimer)
			{
				if (TimerName.Contains(TP.Key)) continue;
				const FTimingProfilerTimer* Tmr = Reader.GetTimer(TP.Key);
				if (Tmr && Tmr->Name) TimerName.Add(TP.Key, FString(Tmr->Name));
			}
		}
	});

	Json.Key(TEXT("events"));
	Json.BeginArray();
	for (FQueueAgg& Q : Queues)
	{
		// Sort per-queue timers by inclusive_ms desc, take top-N.
		TArray<TPair<uint32, FTimerAgg>> Rows;
		Rows.Reserve(Q.PerTimer.Num());
		for (TPair<uint32, FTimerAgg>& TP : Q.PerTimer) Rows.Add(TP);
		Rows.Sort([](const TPair<uint32, FTimerAgg>& A, const TPair<uint32, FTimerAgg>& B)
		{
			return A.Value.InclusiveMs > B.Value.InclusiveMs;
		});
		const int32 TopCount = FMath::Min(Limit, Rows.Num());

		Json.BeginObject();
		Json.KeyInt(TEXT("queue_id"),      static_cast<int64>(Q.Id));
		Json.KeyStr(TEXT("queue_name"),    Q.DisplayName);
		Json.KeyInt(TEXT("event_count"),   static_cast<int64>(Q.EventCount));

		Json.Key(TEXT("top_timers"));
		Json.BeginArray();
		for (int32 i = 0; i < TopCount; ++i)
		{
			const FString* Name = TimerName.Find(Rows[i].Key);
			if (!Name) continue;
			Json.BeginObject();
			Json.KeyStr(TEXT("name"),         *Name);
			Json.KeyInt(TEXT("count"),        static_cast<int64>(Rows[i].Value.Count));
			Json.KeyNum(TEXT("inclusive_ms"), Rows[i].Value.InclusiveMs);
			Json.EndObject();
		}
		Json.EndArray();
		Json.EndObject();
	}
	Json.EndArray();
}

void Modes::RunGpu(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const ITimingProfilerProvider* Timing = ReadTimingProfilerProvider(Session);
	const double EndSec = Session.GetDurationSeconds();

	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyStr(TEXT("view"), Args.View.IsEmpty() ? FString(TEXT("queues")) : Args.View);

	if (!Timing)
	{
		Json.KeyBool(TEXT("has_gpu"), false);
		Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray();
		Json.EndObject();
		return;
	}

	if (Args.View.Equals(TEXT("timeline"), ESearchCase::IgnoreCase))
	{
		RunGpuTimeline(*Timing, Args, Json, EndSec);
	}
	else if (Args.View.Equals(TEXT("fences"), ESearchCase::IgnoreCase))
	{
		RunGpuFences(*Timing, Args, Json, EndSec);
	}
	else // default: queues
	{
		RunGpuQueues(*Timing, Args, Json, EndSec);
	}

	Json.EndObject();
}

} // namespace TraceDigest

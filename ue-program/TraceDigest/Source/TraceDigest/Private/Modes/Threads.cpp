// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"

#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"

using namespace TraceServices;

namespace TraceDigest
{

namespace {

struct FTimerAgg
{
	uint64 Count = 0;
	double InclusiveMs = 0.0;
};

struct FThreadAgg
{
	uint32 Id = 0;
	FString Name;
	uint64 EventCount = 0;
	double TotalDepth0Ms = 0.0;     // sum of root-depth events — rough "thread busy" proxy
	TMap<uint32 /*TimerId*/, FTimerAgg> PerTimer;
};

} // namespace

// Per-thread CPU breakdown. For each CPU timeline:
//   * event_count        total events emitted on the thread
//   * total_depth0_ms    sum of depth-0 (outermost) event durations — a
//                        reasonable proxy for "how busy was this thread"
//                        without paying the cost of true exclusive tracking
//   * events             top-K timers on this thread by inclusive_ms
//
// Per-timer rows show count + inclusive_ms; we deliberately omit exclusive_ms
// because computing it cleanly requires depth-aware bookkeeping at every
// event, which would dominate the trace walk.
void Modes::RunThreads(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const ITimingProfilerProvider* TimingProvider = ReadTimingProfilerProvider(Session);
	const IThreadProvider& Threads = ReadThreadProvider(Session);

	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyNum(TEXT("duration_ms"), Session.GetDurationSeconds() * 1000.0);

	if (!TimingProvider)
	{
		Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray();
		Json.EndObject();
		return;
	}

	// Build (Timeline index -> thread info) inversion so we don't have to
	// search threads for every event.
	TMap<uint32, FThreadAgg> ByTimeline;
	Threads.EnumerateThreads([&](const FThreadInfo& Info)
	{
		uint32 TimelineIdx = 0;
		if (TimingProvider->GetCpuThreadTimelineIndex(Info.Id, TimelineIdx))
		{
			FThreadAgg& Agg = ByTimeline.FindOrAdd(TimelineIdx);
			Agg.Id   = Info.Id;
			Agg.Name = Info.Name ? FString(Info.Name) : FString::Printf(TEXT("Thread_%u"), Info.Id);
		}
	});

	const double EndSec = Session.GetDurationSeconds();
	const uint32 TimelineCount = TimingProvider->GetTimelineCount();

	for (uint32 i = 0; i < TimelineCount; ++i)
	{
		FThreadAgg* Agg = ByTimeline.Find(i);
		if (!Agg) continue;  // skip GPU/Verse timelines

		TimingProvider->ReadTimeline(i, [&](const ITimingProfilerProvider::Timeline& Timeline)
		{
			Timeline.EnumerateEvents(0.0, EndSec,
				[&](double S, double E, uint32 Depth, const FTimingProfilerEvent& Event) -> EEventEnumerate
				{
					const double Ms = (E - S) * 1000.0;
					Agg->EventCount += 1;
					if (Depth == 0) Agg->TotalDepth0Ms += Ms;

					FTimerAgg& T = Agg->PerTimer.FindOrAdd(Event.TimerIndex);
					T.Count += 1;
					T.InclusiveMs += Ms;
					return EEventEnumerate::Continue;
				});
		});
	}

	// Resolve timer names once.
	TMap<uint32, FString> TimerName;
	TimingProvider->ReadTimers([&](const ITimingProfilerTimerReader& Reader)
	{
		for (TPair<uint32, FThreadAgg>& Pair : ByTimeline)
		{
			for (TPair<uint32, FTimerAgg>& TP : Pair.Value.PerTimer)
			{
				if (TimerName.Contains(TP.Key)) continue;
				const FTimingProfilerTimer* T = Reader.GetTimer(TP.Key);
				if (T && T->Name) TimerName.Add(TP.Key, FString(T->Name));
			}
		}
	});

	// Order threads by total_depth0_ms desc — busy threads first.
	TArray<FThreadAgg*> OrderedThreads;
	OrderedThreads.Reserve(ByTimeline.Num());
	for (TPair<uint32, FThreadAgg>& Pair : ByTimeline)
	{
		if (Pair.Value.EventCount > 0)
		{
			OrderedThreads.Add(&Pair.Value);
		}
	}
	OrderedThreads.Sort([](const FThreadAgg& A, const FThreadAgg& B)
	{
		return A.TotalDepth0Ms > B.TotalDepth0Ms;
	});
	if (Args.Limit > 0 && OrderedThreads.Num() > Args.Limit) OrderedThreads.SetNum(Args.Limit);

	// Per-thread, take top-N timers by inclusive_ms. Bounded so a busy thread
	// doesn't blow up the JSON.
	const int32 PerThreadTopN = 10;

	Json.Key(TEXT("events"));
	Json.BeginArray();
	for (FThreadAgg* Agg : OrderedThreads)
	{
		// Sort the per-timer map into a vec, take top N.
		TArray<TPair<uint32, FTimerAgg>> TimerRows;
		TimerRows.Reserve(Agg->PerTimer.Num());
		for (TPair<uint32, FTimerAgg>& TP : Agg->PerTimer) TimerRows.Add(TP);
		TimerRows.Sort([](const TPair<uint32, FTimerAgg>& A, const TPair<uint32, FTimerAgg>& B)
		{
			return A.Value.InclusiveMs > B.Value.InclusiveMs;
		});
		const int32 TopCount = FMath::Min(PerThreadTopN, TimerRows.Num());

		Json.BeginObject();
		Json.KeyInt(TEXT("thread_id"),       static_cast<int64>(Agg->Id));
		Json.KeyStr(TEXT("thread_name"),     Agg->Name);
		Json.KeyInt(TEXT("event_count"),     static_cast<int64>(Agg->EventCount));
		Json.KeyNum(TEXT("total_depth0_ms"), Agg->TotalDepth0Ms);

		Json.Key(TEXT("top_timers"));
		Json.BeginArray();
		for (int32 i = 0; i < TopCount; ++i)
		{
			const FString* Name = TimerName.Find(TimerRows[i].Key);
			if (!Name) continue;
			Json.BeginObject();
			Json.KeyStr(TEXT("name"),         *Name);
			Json.KeyInt(TEXT("count"),        static_cast<int64>(TimerRows[i].Value.Count));
			Json.KeyNum(TEXT("inclusive_ms"), TimerRows[i].Value.InclusiveMs);
			Json.EndObject();
		}
		Json.EndArray();
		Json.EndObject();
	}
	Json.EndArray();
	Json.EndObject();
}

} // namespace TraceDigest

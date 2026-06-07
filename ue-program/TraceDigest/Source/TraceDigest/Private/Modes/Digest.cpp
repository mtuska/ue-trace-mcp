// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"
#include "Percentiles.h"
#include "ProviderReadScope.h"

#include "ProfilingDebugging/MiscTrace.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Regions.h"
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

// Reservoirs keyed by FString (for the region path which has no stable
// integer id across the trace) or by uint32 (for the CPU/GPU timer-id path).
// Both feed the same aggregator and JSON emitter — we just key differently
// while collecting.

namespace {

static void RunCpuDigest(const ITimingProfilerProvider& Timing, const FArgs& Args, double EndSec, FJsonOut& Json)
{
	TMap<uint32, FReservoir> ReservoirByTimer;
	ReservoirByTimer.Reserve(4096);

	Timing.EnumerateTimelines(
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

	TArray<FDigestRow> Rows;
	Rows.Reserve(ReservoirByTimer.Num());
	Timing.ReadTimers([&](const ITimingProfilerTimerReader& Reader)
	{
		for (TPair<uint32, FReservoir>& Pair : ReservoirByTimer)
		{
			const FTimingProfilerTimer* Timer = Reader.GetTimer(Pair.Key);
			if (!Timer || !Timer->Name) continue;
			if (Timer->Type != ETimingProfilerTimerType::CpuScope) continue;

			FString Name = Timer->Name;
			if (!Args.Prefix.IsEmpty() && !Name.StartsWith(Args.Prefix)) continue;
			Rows.Add({ MoveTemp(Name), Pair.Key, &Pair.Value });
		}
	});

	Rows.Sort([](const FDigestRow& A, const FDigestRow& B)
	{
		return A.Reservoir->GetTotal() > B.Reservoir->GetTotal();
	});
	if (Args.Limit > 0 && Rows.Num() > Args.Limit) Rows.SetNum(Args.Limit);

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
}

// GPU digest: walk every GPU queue's timeline and aggregate by timer id,
// reusing the same reservoir → percentile pipeline. GPU events live in the
// same ITimingProfilerTimerReader as CPU but have Type=GpuScope; we filter
// for that. Supports both the new FGpuQueueInfo-based API and the legacy
// Gpu1/Gpu2 timelines (which a number of in-the-wild traces still use).
static void RunGpuDigest(const ITimingProfilerProvider& Timing, const FArgs& Args, double EndSec, FJsonOut& Json)
{
	TMap<uint32, FReservoir> ReservoirByTimer;
	ReservoirByTimer.Reserve(2048);

	TArray<uint32> QueueTimelineIdx;
	Timing.EnumerateGpuQueues([&](const FGpuQueueInfo& Q)
	{
		if (Q.TimelineIndex != ~0u) QueueTimelineIdx.Add(Q.TimelineIndex);
	});
	// Fallback: legacy GPU timelines.
	if (QueueTimelineIdx.Num() == 0)
	{
		uint32 Idx = ~0u;
		if (Timing.GetGpuTimelineIndex(Idx))  QueueTimelineIdx.Add(Idx);
		if (Timing.GetGpu2TimelineIndex(Idx)) QueueTimelineIdx.Add(Idx);
	}

	for (uint32 Idx : QueueTimelineIdx)
	{
		Timing.ReadTimeline(Idx, [&](const ITimingProfilerProvider::Timeline& Timeline)
		{
			Timeline.EnumerateEvents(0.0, EndSec,
				[&](double S, double E, uint32, const FTimingProfilerEvent& Event) -> EEventEnumerate
				{
					FReservoir& R = ReservoirByTimer.FindOrAdd(Event.TimerIndex);
					R.Add((E - S) * 1000.0);
					return EEventEnumerate::Continue;
				});
		});
	}

	TArray<FDigestRow> Rows;
	Rows.Reserve(ReservoirByTimer.Num());
	Timing.ReadTimers([&](const ITimingProfilerTimerReader& Reader)
	{
		for (TPair<uint32, FReservoir>& Pair : ReservoirByTimer)
		{
			const FTimingProfilerTimer* Timer = Reader.GetTimer(Pair.Key);
			if (!Timer || !Timer->Name) continue;
			if (Timer->Type != ETimingProfilerTimerType::GpuScope) continue;

			FString Name = Timer->Name;
			if (!Args.Prefix.IsEmpty() && !Name.StartsWith(Args.Prefix)) continue;
			Rows.Add({ MoveTemp(Name), Pair.Key, &Pair.Value });
		}
	});

	Rows.Sort([](const FDigestRow& A, const FDigestRow& B)
	{
		return A.Reservoir->GetTotal() > B.Reservoir->GetTotal();
	});
	if (Args.Limit > 0 && Rows.Num() > Args.Limit) Rows.SetNum(Args.Limit);

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
}

// Region digest: aggregate every FTimeRegion by timer name. Regions have
// no integer ids globally so we key by FString directly.
static void RunRegionDigest(const IAnalysisSession& Session, const FArgs& Args, double EndSec, FJsonOut& Json)
{
	const IRegionProvider& Provider = ReadRegionProvider(Session);
	FProviderReadScope ProviderLock(Provider);

	TMap<FString, FReservoir> ReservoirByName;
	ReservoirByName.Reserve(256);

	Provider.EnumerateTimelinesByCategory(
		[&](const IRegionTimeline& Timeline, const TCHAR*)
		{
			Timeline.EnumerateRegions(0.0, EndSec, [&](const FTimeRegion& R) -> bool
			{
				const FString Name = (R.Timer && R.Timer->Name) ? FString(R.Timer->Name) : FString();
				if (!Args.Prefix.IsEmpty() && !Name.StartsWith(Args.Prefix)) return true;
				// Open regions have EndTime = +inf; clamp to the trace's end so
				// they're treated as "ran from begin to capture end". Anything
				// past EndSec is also clamped.
				const double End = FMath::Min(R.EndTime, EndSec);
				const double Begin = FMath::Max(R.BeginTime, 0.0);
				if (!FMath::IsFinite(End) || !FMath::IsFinite(Begin) || End < Begin) return true;
				FReservoir& Res = ReservoirByName.FindOrAdd(Name);
				Res.Add((End - Begin) * 1000.0);
				return true;
			});
		});

	// Collect + sort.
	struct FRow { FString Name; FReservoir* R = nullptr; };
	TArray<FRow> Rows;
	Rows.Reserve(ReservoirByName.Num());
	for (TPair<FString, FReservoir>& Pair : ReservoirByName)
	{
		Rows.Add({ Pair.Key, &Pair.Value });
	}
	Rows.Sort([](const FRow& A, const FRow& B) { return A.R->GetTotal() > B.R->GetTotal(); });
	if (Args.Limit > 0 && Rows.Num() > Args.Limit) Rows.SetNum(Args.Limit);

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
}

} // namespace

// Channel-dispatched digest. Channel value comes from Args.Channel
// (default "cpu", validated by FArgs::Parse to cpu|gpu|region for this
// mode). Output schema is identical across channels — same {name, count,
// total_ms, avg_ms, p50, p95, p99, max_ms} rows.
void Modes::RunDigest(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const ITimingProfilerProvider* TimingProvider = ReadTimingProfilerProvider(Session);
	const double EndSec = Session.GetDurationSeconds();

	Json.BeginObject();
	Json.KeyStr(TEXT("file"),    Args.File);
	Json.KeyStr(TEXT("mode"),    FArgs::ModeName(Args.Mode));
	Json.KeyStr(TEXT("channel"), Args.Channel);
	Json.KeyNum(TEXT("duration_ms"), EndSec * 1000.0);
	EmitFrameCount(Session, Json);

	if (Args.Channel.Equals(TEXT("gpu"), ESearchCase::IgnoreCase))
	{
		if (TimingProvider) RunGpuDigest(*TimingProvider, Args, EndSec, Json);
		else                { Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray(); }
	}
	else if (Args.Channel.Equals(TEXT("region"), ESearchCase::IgnoreCase))
	{
		RunRegionDigest(Session, Args, EndSec, Json);
	}
	else
	{
		if (TimingProvider) RunCpuDigest(*TimingProvider, Args, EndSec, Json);
		else                { Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray(); }
	}

	Json.EndObject();
}

} // namespace TraceDigest

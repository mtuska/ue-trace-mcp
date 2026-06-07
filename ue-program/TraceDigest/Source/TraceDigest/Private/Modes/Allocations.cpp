// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"
#include "ProviderReadScope.h"
#include "SeriesBucket.h"

#include "HAL/PlatformProcess.h"
#include "ProfilingDebugging/MemoryTrace.h"  // HeapId typedef
#include "TraceServices/Model/AllocationsProvider.h"
#include "TraceServices/Model/AnalysisSession.h"

using namespace TraceServices;

namespace TraceDigest
{

// IAllocationsProvider has its own BeginRead/EndRead pair (FProviderReadScope)
// distinct from the session scope. It exposes:
//   * timeline — pre-aggregated time series (MaxTotalAllocatedMemory,
//     MinLiveAllocations, AllocEvents/sec, FreeEvents/sec, …). Cheap.
//   * heaps — root-heap tree (FHeapSpec). Tiny.
//   * query — async per-allocation queries by EQueryRule (active-at-T,
//     leaked, short-lived, …). 10^7–10^8 allocations per real trace
//     means we MUST cap and synthesise truncated. The async StartQuery/
//     PollQuery API is wrapped sync here so MCP tool calls stay
//     synchronous from the LLM's perspective.

static IAllocationsProvider::EQueryRule ParseRule(const FString& In, bool& bOk)
{
	using R = IAllocationsProvider::EQueryRule;
	bOk = true;
	// We don't know the full enum at compile time because UE only exposes
	// the names as comments. Stick to the four most useful ones; reject
	// the rest with a clear error.
	if (In.Equals(TEXT("aAf"),  ESearchCase::IgnoreCase)) return R::aAf;
	if (In.Equals(TEXT("afA"),  ESearchCase::IgnoreCase)) return R::afA;
	if (In.Equals(TEXT("Aaf"),  ESearchCase::IgnoreCase)) return R::Aaf;
	if (In.Equals(TEXT("aAfB"), ESearchCase::IgnoreCase)) return R::aAfB;
	bOk = false;
	return R::aAf;
}

static void RunTimelineView(const IAllocationsProvider& Provider, const FArgs& Args, FJsonOut& Json, double EndSec)
{
	// IAllocationsProvider's internal timelines have a FIXED number of
	// points spanning the trace; we don't need to rebucket. But for output
	// consistency we still emit through SeriesBucket so consumers see the
	// same {t_ms, min, max, avg, count} shape they get from counters and
	// memory samples.
	const int32 Points = Provider.GetTimelineNumPoints();
	int32 StartIdx = 0;
	int32 EndIdx   = Points - 1;
	Provider.GetTimelineIndexRange(0.0, EndSec, StartIdx, EndIdx);
	Json.KeyInt(TEXT("timeline_points"), static_cast<int64>(Points));
	Json.KeyInt(TEXT("range_start"),     static_cast<int64>(StartIdx));
	Json.KeyInt(TEXT("range_end"),       static_cast<int64>(EndIdx));

	// Helper to bucket each timeline into the user-requested Args.Buckets
	// count and emit a series array. We do the four most useful ones:
	// MaxTotalAllocatedMemory, MaxLiveAllocations, AllocEvents, FreeEvents.
	auto EmitU64 = [&](const TCHAR* Key, IAllocationsProvider::ETimelineU64 T)
	{
		FSeriesBuckets B(0.0, EndSec, Args.Buckets);
		Provider.EnumerateTimeline(T, StartIdx, EndIdx,
			[&](double Time, double /*Duration*/, uint64 Value)
			{
				B.Add(Time, static_cast<double>(Value));
			});
		Json.Key(Key);
		Json.BeginArray();
		for (int32 i = 0; i < B.Num(); ++i)
		{
			const FSeriesBuckets::FBucket& Bk = B.Buckets[i];
			if (Bk.Count == 0) continue;
			Json.BeginObject();
			Json.KeyNum(TEXT("t_ms"),  B.MidpointMs(i));
			Json.KeyNum(TEXT("min"),   Bk.Min);
			Json.KeyNum(TEXT("max"),   Bk.Max);
			Json.KeyNum(TEXT("avg"),   Bk.Sum / static_cast<double>(Bk.Count));
			Json.KeyInt(TEXT("count"), static_cast<int64>(Bk.Count));
			Json.EndObject();
		}
		Json.EndArray();
	};
	auto EmitU32 = [&](const TCHAR* Key, IAllocationsProvider::ETimelineU32 T)
	{
		FSeriesBuckets B(0.0, EndSec, Args.Buckets);
		Provider.EnumerateTimeline(T, StartIdx, EndIdx,
			[&](double Time, double /*Duration*/, uint32 Value)
			{
				B.Add(Time, static_cast<double>(Value));
			});
		Json.Key(Key);
		Json.BeginArray();
		for (int32 i = 0; i < B.Num(); ++i)
		{
			const FSeriesBuckets::FBucket& Bk = B.Buckets[i];
			if (Bk.Count == 0) continue;
			Json.BeginObject();
			Json.KeyNum(TEXT("t_ms"),  B.MidpointMs(i));
			Json.KeyNum(TEXT("min"),   Bk.Min);
			Json.KeyNum(TEXT("max"),   Bk.Max);
			Json.KeyNum(TEXT("avg"),   Bk.Sum / static_cast<double>(Bk.Count));
			Json.KeyInt(TEXT("count"), static_cast<int64>(Bk.Count));
			Json.EndObject();
		}
		Json.EndArray();
	};

	EmitU64(TEXT("max_total_allocated_memory"), IAllocationsProvider::ETimelineU64::MaxTotalAllocatedMemory);
	EmitU32(TEXT("max_live_allocations"),       IAllocationsProvider::ETimelineU32::MaxLiveAllocations);
	EmitU32(TEXT("alloc_events_per_point"),     IAllocationsProvider::ETimelineU32::AllocEvents);
	EmitU32(TEXT("free_events_per_point"),      IAllocationsProvider::ETimelineU32::FreeEvents);
}

// Recursive descent over the heap tree. Bounded to 1000 nodes total — UE's
// trees are tiny but I'd rather guard.
static void EmitHeapTree(const IAllocationsProvider& Provider, FJsonOut& Json)
{
	Json.Key(TEXT("heaps"));
	Json.BeginArray();
	int32 Emitted = 0;
	Provider.EnumerateRootHeaps([&](HeapId Id, const IAllocationsProvider::FHeapSpec& Root)
	{
		if (Emitted >= 1000) return;
		Json.BeginObject();
		Json.KeyInt(TEXT("id"),        static_cast<int64>(Id));
		Json.KeyInt(TEXT("parent_id"), -1);
		Json.KeyInt(TEXT("flags"),     static_cast<int64>(Root.Flags));
		Json.KeyStr(TEXT("name"),      Root.Name ? FString(Root.Name) : FString());
		Json.KeyBool(TEXT("is_root"),  true);
		Json.EndObject();
		++Emitted;
	});
	Provider.EnumerateHeaps([&](HeapId Id, const IAllocationsProvider::FHeapSpec& Spec)
	{
		if (Emitted >= 1000) return;
		Json.BeginObject();
		Json.KeyInt (TEXT("id"),        static_cast<int64>(Id));
		Json.KeyInt (TEXT("parent_id"), static_cast<int64>(Spec.Parent ? Spec.Parent->Id : 0));
		Json.KeyInt (TEXT("flags"),     static_cast<int64>(Spec.Flags));
		Json.KeyStr (TEXT("name"),      Spec.Name ? FString(Spec.Name) : FString());
		Json.KeyBool(TEXT("is_root"),   false);
		Json.EndObject();
		++Emitted;
	});
	Json.EndArray();
	Json.KeyInt(TEXT("heap_count"), static_cast<int64>(Emitted));
}

// Sync-wrap the async StartQuery → PollQuery loop. Returns once status is
// Done OR our wall-clock budget runs out (in which case CancelQuery and
// emit truncated:true with whatever partial rows we collected).
static void RunQueryView(const IAllocationsProvider& Provider, const FArgs& Args, FJsonOut& Json)
{
	bool bRuleOk = true;
	IAllocationsProvider::EQueryRule Rule = ParseRule(Args.Rule, bRuleOk);
	Json.KeyStr(TEXT("rule"), Args.Rule);
	Json.KeyNum(TEXT("time_a"), Args.TimeA);
	Json.KeyNum(TEXT("time_b"), Args.TimeB);
	Json.KeyInt(TEXT("query_timeout_ms"), static_cast<int64>(Args.QueryTimeoutMs));

	if (!bRuleOk)
	{
		Json.KeyBool(TEXT("ok"), false);
		Json.KeyStr (TEXT("error"), FString::Printf(TEXT("unknown -rule='%s' (supported: aAf|afA|Aaf|aAfB)"), *Args.Rule));
		Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray();
		return;
	}

	IAllocationsProvider::FQueryParams Params{};
	Params.Rule  = Rule;
	Params.TimeA = Args.TimeA >= 0.0 ? Args.TimeA : 0.0;
	Params.TimeB = Args.TimeB;
	Params.TimeC = 0.0;
	Params.TimeD = 0.0;

	const int32 Limit = Args.Limit > 0 ? Args.Limit : 200;
	const double TimeoutSec = static_cast<double>(Args.QueryTimeoutMs) / 1000.0;
	const double Deadline = FPlatformTime::Seconds() + TimeoutSec;

	IAllocationsProvider::FQueryHandle Handle = Provider.StartQuery(Params);

	int32 Emitted = 0;
	int32 TotalAvailable = 0;
	bool  bCompleted = false;

	Json.Key(TEXT("events"));
	Json.BeginArray();

	while (true)
	{
		const IAllocationsProvider::FQueryStatus Status = Provider.PollQuery(Handle);

		// Drain whatever batches are available right now.
		while (Status.Status == IAllocationsProvider::EQueryStatus::Available
			|| Status.Status == IAllocationsProvider::EQueryStatus::Done)
		{
			IAllocationsProvider::FQueryResult Result = Status.NextResult();
			if (!Result.IsValid() || Result->Num() == 0) break;
			const uint32 N = Result->Num();
			TotalAvailable += static_cast<int32>(N);
			for (uint32 i = 0; i < N; ++i)
			{
				const IAllocationsProvider::FAllocation* A = Result->Get(i);
				if (!A) continue;
				if (Emitted >= Limit) continue;  // keep counting Total but don't emit
				Json.BeginObject();
				Json.KeyInt(TEXT("address"),       static_cast<int64>(A->GetAddress()));
				Json.KeyInt(TEXT("size"),          static_cast<int64>(A->GetSize()));
				Json.KeyInt(TEXT("alignment"),     static_cast<int64>(A->GetAlignment()));
				Json.KeyNum(TEXT("start_ms"),      A->GetStartTime() * 1000.0);
				Json.KeyNum(TEXT("end_ms"),        A->GetEndTime() * 1000.0);
				Json.KeyInt(TEXT("alloc_thread"),  static_cast<int64>(A->GetAllocThreadId()));
				Json.KeyInt(TEXT("free_thread"),   static_cast<int64>(A->GetFreeThreadId()));
				Json.KeyInt(TEXT("alloc_callstack_id"), static_cast<int64>(A->GetAllocCallstackId()));
				Json.KeyInt(TEXT("free_callstack_id"),  static_cast<int64>(A->GetFreeCallstackId()));
				Json.KeyInt(TEXT("tag"),           static_cast<int64>(A->GetTag()));
				Json.KeyInt(TEXT("root_heap"),     static_cast<int64>(A->GetRootHeap()));
				Json.KeyBool(TEXT("is_heap"),      A->IsHeap());
				Json.KeyBool(TEXT("is_swap"),      A->IsSwap());
				Json.EndObject();
				++Emitted;
			}
		}

		if (Status.Status == IAllocationsProvider::EQueryStatus::Done)
		{
			bCompleted = true;
			break;
		}
		if (Status.Status == IAllocationsProvider::EQueryStatus::Working)
		{
			if (FPlatformTime::Seconds() >= Deadline)
			{
				Provider.CancelQuery(Handle);
				break;
			}
			FPlatformProcess::Sleep(0.025f);  // 25ms
			continue;
		}
		// Unknown status — bail rather than busy-loop.
		break;
	}

	Json.EndArray();
	Json.KeyBool(TEXT("completed"), bCompleted);
	Json.KeyInt (TEXT("total_available"), static_cast<int64>(TotalAvailable));
	Json.KeyBool(TEXT("truncated"), Emitted < TotalAvailable || !bCompleted);
}

void Modes::RunAllocations(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const IAllocationsProvider* Provider = ReadAllocationsProvider(Session);
	const double EndSec = Session.GetDurationSeconds();

	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyStr(TEXT("view"), Args.View.IsEmpty() ? FString(TEXT("timeline")) : Args.View);
	Json.KeyNum(TEXT("duration_ms"), EndSec * 1000.0);

	if (!Provider)
	{
		Json.KeyBool(TEXT("has_memalloc"), false);
		Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray();
		Json.EndObject();
		return;
	}

	FProviderReadScope ProviderLock(*Provider);
	Json.KeyBool(TEXT("has_memalloc"), true);

	if (Args.View.Equals(TEXT("heaps"), ESearchCase::IgnoreCase))
	{
		EmitHeapTree(*Provider, Json);
	}
	else if (Args.View.Equals(TEXT("query"), ESearchCase::IgnoreCase))
	{
		RunQueryView(*Provider, Args, Json);
	}
	else  // default: timeline
	{
		RunTimelineView(*Provider, Args, Json, EndSec);
	}

	Json.EndObject();
}

} // namespace TraceDigest

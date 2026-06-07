// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"
#include "TimerLookup.h"

#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/TimingProfiler.h"

using namespace TraceServices;

namespace TraceDigest
{

// Shared butterfly walker for the two directional views. `bCallers=true`
// expands the upward (incoming) tree; false expands the downward (outgoing)
// tree. The root node represents the target event itself; its direct children
// are the actual callers/callees we list.
static void RunButterfly(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json, bool bCallers)
{
	FAnalysisSessionReadScope Lock(Session);
	const ITimingProfilerProvider* TimingProvider = ReadTimingProfilerProvider(Session);

	Json.BeginObject();
	Json.KeyStr(TEXT("file"),    Args.File);
	Json.KeyStr(TEXT("mode"),    FArgs::ModeName(Args.Mode));
	Json.KeyStr(TEXT("channel"), Args.Channel);
	Json.KeyStr(TEXT("event"),   Args.Event);

	if (!TimingProvider)
	{
		Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray();
		Json.EndObject();
		return;
	}

	uint32 TimerId = 0;
	const bool bFound = FindFirstTimerIdByName(*TimingProvider, Args.Event, TimerId);

	if (!bFound)
	{
		Json.KeyInt(TEXT("target_count"), 0);
		Json.KeyNum(TEXT("target_inclusive_ms"), 0.0);
		Json.KeyNum(TEXT("target_exclusive_ms"), 0.0);
		Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray();
		Json.EndObject();
		return;
	}

	FCreateButterflyParams Params;
	Params.IntervalStart = 0.0;
	Params.IntervalEnd = Session.GetDurationSeconds();
	// Channel-aware filter: gpu selects every GPU queue and leaves the CPU
	// filter empty (default-constructed TFunction is treated as "no threads
	// included"); cpu selects every CPU thread and leaves the GPU filter
	// empty. v0.4.0 doesn't mix — caller picks one channel via -channel=.
	if (Args.Channel.Equals(TEXT("gpu"), ESearchCase::IgnoreCase))
	{
		Params.GpuQueueFilter = [](uint32) { return true; };
	}
	else
	{
		Params.CpuThreadFilter = [](uint32) { return true; };
	}

	ITimingProfilerButterfly* Butterfly = TimingProvider->CreateButterfly(Params);
	if (!Butterfly)
	{
		Json.KeyInt(TEXT("target_count"), 0);
		Json.KeyNum(TEXT("target_inclusive_ms"), 0.0);
		Json.KeyNum(TEXT("target_exclusive_ms"), 0.0);
		Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray();
		Json.EndObject();
		return;
	}

	const FTimingProfilerButterflyNode& Root = bCallers
		? Butterfly->GenerateCallersTree(TimerId)
		: Butterfly->GenerateCalleesTree(TimerId);

	Json.KeyInt(TEXT("target_count"), static_cast<int64>(Root.Count));
	Json.KeyNum(TEXT("target_inclusive_ms"), Root.InclusiveTime * 1000.0);
	Json.KeyNum(TEXT("target_exclusive_ms"), Root.ExclusiveTime * 1000.0);

	// Direct children = callers/callees one hop away from the target. We don't
	// recurse — the butterfly tree can balloon for deep call graphs and the
	// LLM can chase another hop by calling the same tool with a different
	// -event= argument.
	TArray<FTimingProfilerButterflyNode*> Children = Root.Children;
	Children.Sort([](const FTimingProfilerButterflyNode& A, const FTimingProfilerButterflyNode& B)
	{
		return A.InclusiveTime > B.InclusiveTime;
	});
	if (Args.Limit > 0 && Children.Num() > Args.Limit) Children.SetNum(Args.Limit);

	Json.Key(TEXT("events"));
	Json.BeginArray();
	for (const FTimingProfilerButterflyNode* Child : Children)
	{
		if (!Child || !Child->Timer || !Child->Timer->Name) continue;
		Json.BeginObject();
		Json.KeyStr(TEXT("name"),         FString(Child->Timer->Name));
		Json.KeyInt(TEXT("count"),        static_cast<int64>(Child->Count));
		Json.KeyNum(TEXT("inclusive_ms"), Child->InclusiveTime * 1000.0);
		Json.KeyNum(TEXT("exclusive_ms"), Child->ExclusiveTime * 1000.0);
		Json.EndObject();
	}
	Json.EndArray();
	Json.EndObject();

	delete Butterfly;
}

void Modes::RunCallers(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	RunButterfly(Session, Args, Json, /*bCallers=*/true);
}

void Modes::RunCallees(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	RunButterfly(Session, Args, Json, /*bCallers=*/false);
}

} // namespace TraceDigest

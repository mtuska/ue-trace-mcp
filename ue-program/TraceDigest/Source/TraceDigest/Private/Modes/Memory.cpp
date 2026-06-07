// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"
#include "ProviderReadScope.h"
#include "SeriesBucket.h"

#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Memory.h"

using namespace TraceServices;

namespace TraceDigest
{

// IMemoryProvider serves the LLM (Low-Level Memory) tracker data:
//   * trackers — N (≤8) memory trackers (Default, Platform, ...). Each
//     has a name; queries against a tracker need its id.
//   * tags — hierarchical LLM tag tree (Render, Mesh, Audio, …). Each tag
//     belongs to a tag set and has a parent_id (0 = root).
//   * samples — per-tag time series of `Value` int64s. We downsample via
//     SeriesBucket so the response stays O(buckets) regardless of how
//     dense the underlying sample stream is.
//
// IMemoryProvider has its own BeginRead/EndRead pair (FProviderReadScope)
// in addition to the session-wide FAnalysisSessionReadScope.

// Resolve a -tracker= argument to a tracker id. Accepts either a decimal
// integer or a (case-insensitive) name match.
static FMemoryTrackerId ResolveTrackerId(const IMemoryProvider& Provider, const FString& In)
{
	if (In.IsEmpty()) return FMemoryTrackerInfo::InvalidTrackerId;
	if (In.IsNumeric()) return static_cast<FMemoryTrackerId>(FCString::Atoi(*In));
	FMemoryTrackerId Found = FMemoryTrackerInfo::InvalidTrackerId;
	Provider.EnumerateTrackers([&](const FMemoryTrackerInfo& T)
	{
		if (Found == FMemoryTrackerInfo::InvalidTrackerId
			&& T.Name.Equals(In, ESearchCase::IgnoreCase))
		{
			Found = T.Id;
		}
	});
	return Found;
}

// Resolve a -tag= argument to a tag id.
static FMemoryTagId ResolveTagId(const IMemoryProvider& Provider, const FString& In)
{
	if (In.IsEmpty()) return FMemoryTagInfo::InvalidTagId;
	if (In.IsNumeric()) return static_cast<FMemoryTagId>(FCString::Atoi64(*In));
	FMemoryTagId Found = FMemoryTagInfo::InvalidTagId;
	Provider.EnumerateTags([&](const FMemoryTagInfo& T)
	{
		if (Found == FMemoryTagInfo::InvalidTagId
			&& T.Name.Equals(In, ESearchCase::IgnoreCase))
		{
			Found = T.Id;
		}
	});
	return Found;
}

static void RunTrackersView(const IMemoryProvider& Provider, FJsonOut& Json)
{
	Json.Key(TEXT("trackers"));
	Json.BeginArray();
	Provider.EnumerateTrackers([&](const FMemoryTrackerInfo& T)
	{
		Json.BeginObject();
		Json.KeyInt(TEXT("id"),   static_cast<int64>(T.Id));
		Json.KeyStr(TEXT("name"), T.Name);
		Json.EndObject();
	});
	Json.EndArray();

	Json.Key(TEXT("tag_sets"));
	Json.BeginArray();
	Provider.EnumerateTagSets([&](const FMemoryTagSetInfo& S)
	{
		Json.BeginObject();
		Json.KeyInt(TEXT("id"),   static_cast<int64>(S.Id));
		Json.KeyStr(TEXT("name"), S.Name);
		Json.EndObject();
	});
	Json.EndArray();
}

static void RunTagsView(const IMemoryProvider& Provider, const FArgs& Args, FJsonOut& Json)
{
	// We emit tags as a flat list with parent_id; the LLM can reconstruct
	// the tree if it wants. Optional -tracker= and -tag= filters: when
	// -tag= is set we emit only the matching tag + its ancestor chain (a
	// useful "explain this tag" shape); when -tracker= is set we emit
	// only tags that bit-flag the tracker. Both filters can be combined.
	FMemoryTrackerId Tracker = ResolveTrackerId(Provider, Args.Tracker);
	FMemoryTagId     FocusTag = ResolveTagId(Provider, Args.Tag);

	Json.KeyInt(TEXT("tracker_filter"), static_cast<int64>(Tracker));
	Json.KeyInt(TEXT("tag_filter"),     static_cast<int64>(FocusTag));

	// Snapshot every tag in a map for ancestor walks if -tag= was given.
	TMap<FMemoryTagId, FMemoryTagInfo> AllTags;
	Provider.EnumerateTags([&](const FMemoryTagInfo& T)
	{
		AllTags.Add(T.Id, T);
	});

	TSet<FMemoryTagId> KeepSet;
	if (FocusTag != FMemoryTagInfo::InvalidTagId)
	{
		FMemoryTagId Cur = FocusTag;
		while (Cur != FMemoryTagInfo::InvalidTagId)
		{
			if (KeepSet.Contains(Cur)) break;  // defensive against cycles
			KeepSet.Add(Cur);
			const FMemoryTagInfo* P = AllTags.Find(Cur);
			Cur = P ? P->ParentId : FMemoryTagInfo::InvalidTagId;
		}
	}

	Json.Key(TEXT("tags"));
	Json.BeginArray();
	for (const TPair<FMemoryTagId, FMemoryTagInfo>& Pair : AllTags)
	{
		const FMemoryTagInfo& T = Pair.Value;
		if (FocusTag != FMemoryTagInfo::InvalidTagId && !KeepSet.Contains(T.Id)) continue;
		if (Tracker != FMemoryTrackerInfo::InvalidTrackerId)
		{
			const uint64 Mask = 1ull << static_cast<uint64>(Tracker);
			if ((T.Trackers & Mask) == 0) continue;
		}
		Json.BeginObject();
		Json.KeyInt (TEXT("id"),        static_cast<int64>(T.Id));
		Json.KeyInt (TEXT("parent_id"), static_cast<int64>(T.ParentId));
		Json.KeyInt (TEXT("tag_set_id"), static_cast<int64>(T.TagSetId));
		Json.KeyInt (TEXT("trackers_bitmask"), static_cast<int64>(T.Trackers));
		Json.KeyStr (TEXT("name"),      T.Name);
		Json.EndObject();
	}
	Json.EndArray();
}

static void RunSamplesView(const IMemoryProvider& Provider, const FArgs& Args, double EndSec, FJsonOut& Json)
{
	FMemoryTrackerId Tracker = ResolveTrackerId(Provider, Args.Tracker);
	FMemoryTagId     TagId   = ResolveTagId(Provider, Args.Tag);

	// Default to tracker 0 (UE's "Default" tracker) when -tracker= wasn't
	// supplied — most users only have the one tracker active anyway.
	if (Tracker == FMemoryTrackerInfo::InvalidTrackerId) Tracker = 0;

	Json.KeyInt(TEXT("tracker_id"),  static_cast<int64>(Tracker));
	Json.KeyInt(TEXT("tag_id"),      static_cast<int64>(TagId));
	Json.KeyStr(TEXT("tag_arg"),     Args.Tag);
	Json.KeyInt(TEXT("buckets"),     static_cast<int64>(Args.Buckets));

	if (TagId == FMemoryTagInfo::InvalidTagId)
	{
		Json.KeyBool(TEXT("found"), false);
		Json.Key(TEXT("series")); Json.BeginArray(); Json.EndArray();
		return;
	}
	Json.KeyBool(TEXT("found"), true);
	Json.KeyInt(TEXT("total_samples"),
		static_cast<int64>(Provider.GetTagSampleCount(Tracker, TagId)));

	FSeriesBuckets Series(0.0, EndSec, Args.Buckets);
	Provider.EnumerateTagSamples(Tracker, TagId, 0.0, EndSec,
		/*bIncludeRangeNeighbors*/ false,
		[&](double Time, double /*Duration*/, const FMemoryTagSample& S)
		{
			Series.Add(Time, static_cast<double>(S.Value));
		});

	Json.Key(TEXT("series"));
	Json.BeginArray();
	for (int32 i = 0; i < Series.Num(); ++i)
	{
		const FSeriesBuckets::FBucket& B = Series.Buckets[i];
		if (B.Count == 0) continue;
		Json.BeginObject();
		Json.KeyNum(TEXT("t_ms"),     Series.MidpointMs(i));
		Json.KeyNum(TEXT("min"),      B.Min);
		Json.KeyNum(TEXT("max"),      B.Max);
		Json.KeyNum(TEXT("avg"),      B.Sum / static_cast<double>(B.Count));
		Json.KeyInt(TEXT("count"),    static_cast<int64>(B.Count));
		Json.EndObject();
	}
	Json.EndArray();
}

void Modes::RunMemory(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const IMemoryProvider* Provider = ReadMemoryProvider(Session);
	const double EndSec = Session.GetDurationSeconds();

	Json.BeginObject();
	Json.KeyStr(TEXT("file"),    Args.File);
	Json.KeyStr(TEXT("mode"),    FArgs::ModeName(Args.Mode));
	Json.KeyStr(TEXT("view"),    Args.View.IsEmpty() ? FString(TEXT("trackers")) : Args.View);
	Json.KeyNum(TEXT("duration_ms"), EndSec * 1000.0);

	if (!Provider)
	{
		Json.KeyBool(TEXT("has_memory"), false);
		Json.Key(TEXT("trackers")); Json.BeginArray(); Json.EndArray();
		Json.EndObject();
		return;
	}

	FProviderReadScope ProviderLock(*Provider);
	Json.KeyBool(TEXT("has_memory"), true);

	if (Args.View.Equals(TEXT("tags"), ESearchCase::IgnoreCase))
	{
		RunTagsView(*Provider, Args, Json);
	}
	else if (Args.View.Equals(TEXT("samples"), ESearchCase::IgnoreCase))
	{
		RunSamplesView(*Provider, Args, EndSec, Json);
	}
	else // default: trackers
	{
		RunTrackersView(*Provider, Json);
	}

	Json.EndObject();
}

} // namespace TraceDigest

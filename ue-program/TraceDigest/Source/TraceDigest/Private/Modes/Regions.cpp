// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"
#include "ProviderReadScope.h"

#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Regions.h"

using namespace TraceServices;

namespace TraceDigest
{

// Time-ordered list of `TRACE_BEGIN_REGION`/`TRACE_END_REGION` time spans.
// Filterable by -category=. Each row carries the region's depth so callers
// can reconstruct stacking if they want. Output shape mirrors trace_bookmarks
// (single flat events[] array) for LLM ergonomics — categories cross-cut via
// each row's `category` field.
void Modes::RunRegions(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const IRegionProvider& Provider = ReadRegionProvider(Session);
	FProviderReadScope ProviderLock(Provider);

	const double EndSec = Session.GetDurationSeconds();
	const int32 Limit = Args.Limit > 0 ? Args.Limit : 500;

	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyNum(TEXT("duration_ms"),    EndSec * 1000.0);
	Json.KeyInt(TEXT("total_in_window"), static_cast<int64>(Provider.GetRegionCount()));
	Json.KeyStr(TEXT("category_filter"), Args.Category);  // empty = all

	// Optionally list known categories so the LLM can see what filters exist.
	Json.Key(TEXT("categories"));
	Json.BeginArray();
	Provider.EnumerateCategories([&](const FRegionCategory& C) -> bool
	{
		Json.Str(C.Name ? FString(C.Name) : FString());
		return true;
	});
	Json.EndArray();

	int32 Emitted = 0;
	int32 Total   = 0;

	Json.Key(TEXT("events"));
	Json.BeginArray();
	Provider.EnumerateTimelinesByCategory(
		[&](const IRegionTimeline& Timeline, const TCHAR* CategoryName)
		{
			// -category filter: skip non-matching timelines wholesale.
			if (!Args.Category.IsEmpty())
			{
				const FString TheirCat = CategoryName ? FString(CategoryName) : FString();
				if (!TheirCat.Equals(Args.Category, ESearchCase::IgnoreCase))
				{
					return;
				}
			}
			Timeline.EnumerateRegions(0.0, EndSec, [&](const FTimeRegion& R) -> bool
			{
				++Total;
				if (Emitted >= Limit) return true;  // keep counting Total
				const FString TimerName = (R.Timer && R.Timer->Name) ? FString(R.Timer->Name) : FString();
				const FString Cat       = (R.Timer && R.Timer->Category && R.Timer->Category->Name)
					                      ? FString(R.Timer->Category->Name) : FString();
				// Open regions default to EndTime=+inf. Clamp to trace end so
				// the JSON stays valid; mark the row so callers can tell.
				const bool   bOpen = !FMath::IsFinite(R.EndTime);
				const double End   = bOpen ? EndSec : R.EndTime;

				Json.BeginObject();
				Json.KeyStr (TEXT("name"),        TimerName);
				Json.KeyStr (TEXT("category"),    Cat);
				Json.KeyNum (TEXT("begin_ms"),    R.BeginTime * 1000.0);
				Json.KeyNum (TEXT("end_ms"),      End * 1000.0);
				Json.KeyNum (TEXT("duration_ms"), (End - R.BeginTime) * 1000.0);
				Json.KeyInt (TEXT("depth"),       static_cast<int64>(R.Depth));
				Json.KeyInt (TEXT("id"),          static_cast<int64>(R.Id));
				Json.KeyBool(TEXT("open"),        bOpen);
				Json.EndObject();
				++Emitted;
				return true;
			});
		});
	Json.EndArray();

	Json.KeyBool(TEXT("truncated"), Total > Emitted);
	Json.EndObject();
}

} // namespace TraceDigest

// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"
#include "SeriesBucket.h"

#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Counters.h"

using namespace TraceServices;

namespace TraceDigest
{

// Dispatched on `-counter=<name>`:
//   no -counter: catalogue — every counter + metadata (name, group, type,
//                display hint, IsFloatingPoint).
//   with -counter: time-bucketed series of values for that counter, using
//                  SeriesBucket to keep response size O(buckets) regardless
//                  of underlying density.

static const TCHAR* DisplayHintName(ECounterDisplayHint H)
{
	switch (H)
	{
		case CounterDisplayHint_Memory: return TEXT("memory");
		default:                        return TEXT("none");
	}
}

static void EmitCatalogue(const ICounterProvider& Provider, FJsonOut& Json)
{
	Json.Key(TEXT("counters"));
	Json.BeginArray();
	Provider.EnumerateCounters([&](uint32 Id, const ICounter& Counter)
	{
		Json.BeginObject();
		Json.KeyInt (TEXT("id"),               static_cast<int64>(Id));
		Json.KeyStr (TEXT("name"),             Counter.GetName() ? FString(Counter.GetName()) : FString());
		Json.KeyStr (TEXT("group"),            Counter.GetGroup() ? FString(Counter.GetGroup()) : FString());
		Json.KeyStr (TEXT("description"),      Counter.GetDescription() ? FString(Counter.GetDescription()) : FString());
		Json.KeyBool(TEXT("is_float"),         Counter.IsFloatingPoint());
		Json.KeyBool(TEXT("reset_every_frame"),Counter.IsResetEveryFrame());
		Json.KeyStr (TEXT("display_hint"),     FString(DisplayHintName(Counter.GetDisplayHint())));
		Json.EndObject();
	});
	Json.EndArray();
}

static void EmitSeries(const ICounterProvider& Provider, const FArgs& Args, FJsonOut& Json, double EndSec)
{
	// Find the counter by exact name match (case-insensitive). EnumerateCounters
	// is O(n) but `n` is small (hundreds at most).
	const ICounter* Target = nullptr;
	uint32          TargetId = ~0u;
	bool            TargetIsFloat = false;
	Provider.EnumerateCounters([&](uint32 Id, const ICounter& C)
	{
		if (Target) return;
		const TCHAR* Name = C.GetName();
		if (Name && Args.Counter.Equals(Name, ESearchCase::IgnoreCase))
		{
			Target        = &C;
			TargetId      = Id;
			TargetIsFloat = C.IsFloatingPoint();
		}
	});

	Json.KeyStr(TEXT("counter"), Args.Counter);
	if (!Target)
	{
		Json.KeyBool(TEXT("found"), false);
		Json.Key(TEXT("series")); Json.BeginArray(); Json.EndArray();
		return;
	}
	Json.KeyBool(TEXT("found"),     true);
	Json.KeyInt (TEXT("counter_id"), static_cast<int64>(TargetId));
	Json.KeyBool(TEXT("is_float"),   TargetIsFloat);
	Json.KeyInt (TEXT("buckets"),    static_cast<int64>(Args.Buckets));

	FSeriesBuckets Series(0.0, EndSec, Args.Buckets);
	uint64 TotalSamples = 0;
	if (TargetIsFloat)
	{
		Target->EnumerateFloatValues(0.0, EndSec, /*bIncludeExternalBounds*/ true,
			[&](double Time, double Value)
			{
				Series.Add(Time, Value);
				++TotalSamples;
			});
	}
	else
	{
		Target->EnumerateValues(0.0, EndSec, /*bIncludeExternalBounds*/ true,
			[&](double Time, int64 Value)
			{
				Series.Add(Time, static_cast<double>(Value));
				++TotalSamples;
			});
	}
	Json.KeyInt(TEXT("total_samples"), static_cast<int64>(TotalSamples));

	Json.Key(TEXT("series"));
	Json.BeginArray();
	for (int32 i = 0; i < Series.Num(); ++i)
	{
		const FSeriesBuckets::FBucket& B = Series.Buckets[i];
		if (B.Count == 0) continue;  // gaps not emitted — keeps the array compact
		Json.BeginObject();
		Json.KeyNum (TEXT("t_ms"),     Series.MidpointMs(i));
		Json.KeyNum (TEXT("min"),      B.Min);
		Json.KeyNum (TEXT("max"),      B.Max);
		Json.KeyNum (TEXT("avg"),      B.Sum / static_cast<double>(B.Count));
		Json.KeyInt (TEXT("count"),    static_cast<int64>(B.Count));
		Json.EndObject();
	}
	Json.EndArray();
}

void Modes::RunCounters(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const ICounterProvider& Provider = ReadCounterProvider(Session);

	const double EndSec = Session.GetDurationSeconds();

	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyNum(TEXT("duration_ms"),  EndSec * 1000.0);
	Json.KeyInt(TEXT("counter_count"), static_cast<int64>(Provider.GetCounterCount()));

	if (Args.Counter.IsEmpty())
	{
		EmitCatalogue(Provider, Json);
	}
	else
	{
		EmitSeries(Provider, Args, Json, EndSec);
	}

	Json.EndObject();
}

} // namespace TraceDigest

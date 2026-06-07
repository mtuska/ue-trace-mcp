// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"
#include "Percentiles.h"
#include "ProviderReadScope.h"

#include "Dom/JsonObject.h"
#include "Logging/LogVerbosity.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Counters.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Log.h"
#include "TraceServices/Model/Regions.h"
#include "TraceServices/Model/TimingProfiler.h"

using namespace TraceServices;

namespace TraceDigest
{

// Escape hatch for cross-cutting questions the specific tools can't
// answer. Each intent takes a JSON params blob and joins two providers.
// Intents currently shipped:
//   * list                            — enumerate the registry
//   * frames_where_counter_exceeds    — IFrameProvider × ICounterProvider
//   * events_inside_region            — ITimingProfilerProvider × IRegionProvider
//   * logs_around_slow_frames         — IFrameProvider × ILogProvider
// New intents land here without churning the MCP schema — `intent` is
// z.string() on the TS side specifically so additions are non-breaking.

namespace {

static TSharedPtr<FJsonObject> ParseParams(const FString& In)
{
	if (In.IsEmpty()) return MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Out;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(In);
	if (!FJsonSerializer::Deserialize(Reader, Out) || !Out.IsValid())
	{
		return nullptr;
	}
	return Out;
}

static void WriteError(FJsonOut& Json, const FString& Message, const TArray<FString>& Supported = {})
{
	Json.KeyBool(TEXT("ok"), false);
	Json.KeyStr (TEXT("error"), Message);
	if (Supported.Num() > 0)
	{
		Json.Key(TEXT("supported"));
		Json.BeginArray();
		for (const FString& S : Supported) Json.Str(S);
		Json.EndArray();
	}
}

static void EmitListIntent(FJsonOut& Json)
{
	Json.KeyBool(TEXT("ok"), true);
	Json.Key(TEXT("intents"));
	Json.BeginArray();

	auto Emit = [&](const TCHAR* Name, const TCHAR* Desc, const TCHAR* Params)
	{
		Json.BeginObject();
		Json.KeyStr(TEXT("name"),        Name);
		Json.KeyStr(TEXT("description"), Desc);
		Json.KeyStr(TEXT("params"),      Params);
		Json.EndObject();
	};
	Emit(TEXT("list"),
		TEXT("Enumerate available intents + their params shapes."),
		TEXT("{}"));
	Emit(TEXT("frames_where_counter_exceeds"),
		TEXT("Find every Game-thread frame where a named counter's value crosses a threshold."),
		TEXT("{ counter: <name>, threshold: <number>, op?: \">\"|\">=\" }"));
	Emit(TEXT("events_inside_region"),
		TEXT("List CPU events whose start time falls inside any instance of a named region."),
		TEXT("{ region: <name>, event_prefix?: <string>, limit?: <int> }"));
	Emit(TEXT("logs_around_slow_frames"),
		TEXT("For every frame whose duration exceeds the percentile threshold, list log messages within a window around the frame's start."),
		TEXT("{ p_threshold?: 0.95, window_ms?: 50, verbosity?: <warning|error|...>, limit?: <int> }"));

	Json.EndArray();
}

static void EmitFramesWhereCounterExceeds(
	const IAnalysisSession& Session, const FJsonObject& Params, const FArgs& Args, FJsonOut& Json)
{
	const FString CounterName = Params.HasField(TEXT("counter")) ? Params.GetStringField(TEXT("counter")) : FString();
	if (CounterName.IsEmpty())
	{
		WriteError(Json, TEXT("missing required param 'counter'"));
		return;
	}
	const double Threshold = Params.HasField(TEXT("threshold")) ? Params.GetNumberField(TEXT("threshold")) : 0.0;
	const FString OpStr    = Params.HasField(TEXT("op")) ? Params.GetStringField(TEXT("op")) : FString(TEXT(">"));
	const bool bGreaterOrEqual = OpStr.Equals(TEXT(">="), ESearchCase::CaseSensitive);

	const ICounterProvider& Counters = ReadCounterProvider(Session);
	const IFrameProvider&   Frames   = ReadFrameProvider(Session);

	// Resolve counter by case-insensitive name match.
	const ICounter* Target = nullptr;
	Counters.EnumerateCounters([&](uint32, const ICounter& C)
	{
		if (Target) return;
		if (C.GetName() && CounterName.Equals(C.GetName(), ESearchCase::IgnoreCase)) Target = &C;
	});
	if (!Target)
	{
		WriteError(Json, FString::Printf(TEXT("counter '%s' not found"), *CounterName));
		return;
	}

	const double EndSec = Session.GetDurationSeconds();
	const int32  Limit  = Args.Limit > 0 ? Args.Limit : 200;

	// Walk values; on each crossing, look up containing frame and emit a row
	// for the FIRST sample in that frame (dedupe by frame index).
	TSet<uint32> EmittedFrames;
	int32 Emitted = 0;
	int32 Total   = 0;

	Json.KeyBool(TEXT("ok"), true);
	Json.KeyStr (TEXT("counter"), Target->GetName());
	Json.KeyNum (TEXT("threshold"), Threshold);
	Json.KeyStr (TEXT("op"), bGreaterOrEqual ? TEXT(">=") : TEXT(">"));

	auto EmitRow = [&](double Time, double Value)
	{
		++Total;
		if (Emitted >= Limit) return;
		const uint32 FrameIdx = Frames.GetFrameNumberForTimestamp(TraceFrameType_Game, Time);
		if (EmittedFrames.Contains(FrameIdx)) return;
		EmittedFrames.Add(FrameIdx);
		const TraceServices::FFrame* F = Frames.GetFrame(TraceFrameType_Game, FrameIdx);
		Json.BeginObject();
		Json.KeyInt(TEXT("frame_idx"),     static_cast<int64>(FrameIdx));
		Json.KeyNum(TEXT("time_ms"),       Time * 1000.0);
		Json.KeyNum(TEXT("value"),         Value);
		Json.KeyNum(TEXT("frame_start_ms"), F ? F->StartTime * 1000.0 : 0.0);
		Json.KeyNum(TEXT("frame_duration_ms"), F ? (F->EndTime - F->StartTime) * 1000.0 : 0.0);
		Json.EndObject();
		++Emitted;
	};

	Json.Key(TEXT("events"));
	Json.BeginArray();
	if (Target->IsFloatingPoint())
	{
		Target->EnumerateFloatValues(0.0, EndSec, /*bIncludeExternalBounds*/ false,
			[&](double Time, double Value)
			{
				if ((bGreaterOrEqual && Value >= Threshold) || (!bGreaterOrEqual && Value > Threshold))
				{
					EmitRow(Time, Value);
				}
			});
	}
	else
	{
		Target->EnumerateValues(0.0, EndSec, false,
			[&](double Time, int64 Value)
			{
				const double V = static_cast<double>(Value);
				if ((bGreaterOrEqual && V >= Threshold) || (!bGreaterOrEqual && V > Threshold))
				{
					EmitRow(Time, V);
				}
			});
	}
	Json.EndArray();
	Json.KeyInt (TEXT("total_crossings"), static_cast<int64>(Total));
	Json.KeyBool(TEXT("truncated"), Emitted < Total);
}

static void EmitEventsInsideRegion(
	const IAnalysisSession& Session, const FJsonObject& Params, const FArgs& Args, FJsonOut& Json)
{
	const FString RegionName = Params.HasField(TEXT("region")) ? Params.GetStringField(TEXT("region")) : FString();
	if (RegionName.IsEmpty())
	{
		WriteError(Json, TEXT("missing required param 'region'"));
		return;
	}
	const FString EventPrefix = Params.HasField(TEXT("event_prefix")) ? Params.GetStringField(TEXT("event_prefix")) : FString();
	int32 RequestedLimit = 0;
	if (Params.HasField(TEXT("limit"))) RequestedLimit = static_cast<int32>(Params.GetNumberField(TEXT("limit")));
	const int32 Limit = RequestedLimit > 0 ? RequestedLimit : (Args.Limit > 0 ? Args.Limit : 200);

	const IRegionProvider& Regions = ReadRegionProvider(Session);
	FProviderReadScope RegionsLock(Regions);
	const ITimingProfilerProvider* Timing = ReadTimingProfilerProvider(Session);
	if (!Timing)
	{
		WriteError(Json, TEXT("no timing profiler provider"));
		return;
	}

	const double EndSec = Session.GetDurationSeconds();

	// Collect every matching region instance.
	struct FInstance { double Begin; double End; uint64 Id; };
	TArray<FInstance> Instances;
	Regions.EnumerateTimelinesByCategory(
		[&](const IRegionTimeline& Timeline, const TCHAR*)
		{
			Timeline.EnumerateRegions(0.0, EndSec, [&](const FTimeRegion& R) -> bool
			{
				const FString Name = (R.Timer && R.Timer->Name) ? FString(R.Timer->Name) : FString();
				if (!Name.Equals(RegionName, ESearchCase::IgnoreCase)) return true;
				const double End = FMath::IsFinite(R.EndTime) ? FMath::Min(R.EndTime, EndSec) : EndSec;
				Instances.Add({ R.BeginTime, End, R.Id });
				return true;
			});
		});

	Json.KeyBool(TEXT("ok"), true);
	Json.KeyStr (TEXT("region"), RegionName);
	Json.KeyInt (TEXT("region_instance_count"), static_cast<int64>(Instances.Num()));

	if (Instances.Num() == 0)
	{
		Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray();
		Json.KeyInt (TEXT("total_events"), 0);
		Json.KeyBool(TEXT("truncated"), false);
		return;
	}

	int32 Emitted = 0;
	int32 Total   = 0;
	Json.Key(TEXT("events"));
	Json.BeginArray();
	Timing->EnumerateTimelines(
		[&](const ITimingProfilerProvider::Timeline& Timeline)
		{
			for (const FInstance& Inst : Instances)
			{
				Timeline.EnumerateEvents(Inst.Begin, Inst.End,
					[&](double S, double E, uint32, const FTimingProfilerEvent& Event) -> EEventEnumerate
					{
						++Total;
						if (Emitted >= Limit) return EEventEnumerate::Continue;
						// Resolve timer name; reject by prefix if requested.
						bool bKeep = true;
						FString TimerName;
						Timing->ReadTimers([&](const ITimingProfilerTimerReader& Reader)
						{
							if (const FTimingProfilerTimer* T = Reader.GetTimer(Event.TimerIndex))
							{
								TimerName = T->Name ? FString(T->Name) : FString();
							}
						});
						if (!EventPrefix.IsEmpty() && !TimerName.StartsWith(EventPrefix)) bKeep = false;
						if (!bKeep) return EEventEnumerate::Continue;

						Json.BeginObject();
						Json.KeyInt(TEXT("region_id"),               static_cast<int64>(Inst.Id));
						Json.KeyNum(TEXT("region_begin_ms"),         Inst.Begin * 1000.0);
						Json.KeyStr(TEXT("name"),                    TimerName);
						Json.KeyNum(TEXT("start_ms"),                S * 1000.0);
						Json.KeyNum(TEXT("duration_ms"),            (E - S) * 1000.0);
						Json.EndObject();
						++Emitted;
						return EEventEnumerate::Continue;
					});
			}
		});
	Json.EndArray();
	Json.KeyInt (TEXT("total_events"), static_cast<int64>(Total));
	Json.KeyBool(TEXT("truncated"), Emitted < Total);
}

static void EmitLogsAroundSlowFrames(
	const IAnalysisSession& Session, const FJsonObject& Params, const FArgs& Args, FJsonOut& Json)
{
	const double PThreshold = Params.HasField(TEXT("p_threshold")) ? Params.GetNumberField(TEXT("p_threshold")) : 0.95;
	const double WindowMs   = Params.HasField(TEXT("window_ms"))   ? Params.GetNumberField(TEXT("window_ms"))   : 50.0;
	int32 RequestedLimit = 0;
	if (Params.HasField(TEXT("limit"))) RequestedLimit = static_cast<int32>(Params.GetNumberField(TEXT("limit")));
	const int32 Limit = RequestedLimit > 0 ? RequestedLimit : (Args.Limit > 0 ? Args.Limit : 200);

	if (PThreshold <= 0.0 || PThreshold >= 1.0)
	{
		WriteError(Json, TEXT("p_threshold must be in (0, 1) — e.g. 0.95 for p95"));
		return;
	}

	const IFrameProvider& Frames = ReadFrameProvider(Session);
	const ILogProvider&   Logs   = ReadLogProvider(Session);

	const uint64 N = Frames.GetFrameCount(TraceFrameType_Game);
	if (N == 0)
	{
		Json.KeyBool(TEXT("ok"), true);
		Json.KeyNum (TEXT("p_threshold"), PThreshold);
		Json.KeyNum (TEXT("threshold_ms"), 0.0);
		Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray();
		Json.KeyInt(TEXT("slow_frame_count"), 0);
		return;
	}

	// Compute threshold via reservoir percentiles to keep memory bounded.
	FReservoir Frame(static_cast<int32>(FMath::Min<uint64>(N, 200000)));
	struct FFrameRow { uint64 Idx; double StartSec; double DurationMs; };
	TArray<FFrameRow> All;
	All.Reserve(static_cast<int32>(N));
	Frames.EnumerateFrames(TraceFrameType_Game, 0, N, [&](const TraceServices::FFrame& F)
	{
		const double Ms = (F.EndTime - F.StartTime) * 1000.0;
		Frame.Add(Ms);
		All.Add({ F.Index, F.StartTime, Ms });
	});

	// Resolve threshold via reservoir percentile. Reservoir uses random sampling
	// above the cap so this is approximate — fine for "spike detection".
	const double Threshold = Frame.GetPercentile(PThreshold);

	Json.KeyBool(TEXT("ok"), true);
	Json.KeyNum (TEXT("p_threshold"), PThreshold);
	Json.KeyNum (TEXT("threshold_ms"), Threshold);
	Json.KeyNum (TEXT("window_ms"),    WindowMs);

	int32 SlowFrameCount = 0;
	int32 Emitted        = 0;
	int32 TotalLogs      = 0;
	Json.Key(TEXT("events"));
	Json.BeginArray();
	for (const FFrameRow& Row : All)
	{
		const double DurationMs = Row.DurationMs;
		if (DurationMs < Threshold) continue;
		++SlowFrameCount;
		if (Emitted >= Limit) continue;

		const double StartSec = Row.StartSec;
		const double WinStartSec = FMath::Max(0.0, StartSec - WindowMs / 1000.0);
		const double WinEndSec   = StartSec + (DurationMs + WindowMs) / 1000.0;

		Logs.EnumerateMessages(WinStartSec, WinEndSec, [&](const FLogMessageInfo& Msg)
		{
			++TotalLogs;
			if (Emitted >= Limit) return;
			Json.BeginObject();
			Json.KeyInt(TEXT("frame_idx"),         static_cast<int64>(Row.Idx));
			Json.KeyNum(TEXT("frame_duration_ms"), DurationMs);
			Json.KeyNum(TEXT("log_time_ms"),       Msg.Time * 1000.0);
			Json.KeyStr(TEXT("category"),          Msg.Category && Msg.Category->Name ? FString(Msg.Category->Name) : FString());
			const TCHAR* VerbName =
				Msg.Verbosity == ELogVerbosity::Fatal   ? TEXT("fatal")   :
				Msg.Verbosity == ELogVerbosity::Error   ? TEXT("error")   :
				Msg.Verbosity == ELogVerbosity::Warning ? TEXT("warning") :
				Msg.Verbosity == ELogVerbosity::Display ? TEXT("display") :
				Msg.Verbosity == ELogVerbosity::Log     ? TEXT("log")     :
				TEXT("verbose");
			Json.KeyStr(TEXT("verbosity"),         FString(VerbName));
			Json.KeyStr(TEXT("message"),           Msg.Message ? FString(Msg.Message) : FString());
			Json.EndObject();
			++Emitted;
		});
	}
	Json.EndArray();
	Json.KeyInt (TEXT("slow_frame_count"), static_cast<int64>(SlowFrameCount));
	Json.KeyInt (TEXT("total_logs_in_windows"), static_cast<int64>(TotalLogs));
	Json.KeyBool(TEXT("truncated"), Emitted < TotalLogs);
}

} // namespace

void Modes::RunQuery(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);

	Json.BeginObject();
	Json.KeyStr(TEXT("file"),   Args.File);
	Json.KeyStr(TEXT("mode"),   FArgs::ModeName(Args.Mode));
	Json.KeyStr(TEXT("intent"), Args.Intent);

	if (Args.Intent.Equals(TEXT("list"), ESearchCase::IgnoreCase))
	{
		EmitListIntent(Json);
		Json.EndObject();
		return;
	}

	TSharedPtr<FJsonObject> Params = ParseParams(Args.Params);
	if (!Params.IsValid())
	{
		WriteError(Json,
			FString::Printf(TEXT("could not parse -params= as JSON: %s"), *Args.Params),
			{ TEXT("list"),
			  TEXT("frames_where_counter_exceeds"),
			  TEXT("events_inside_region"),
			  TEXT("logs_around_slow_frames") });
		Json.EndObject();
		return;
	}

	if (Args.Intent.Equals(TEXT("frames_where_counter_exceeds"), ESearchCase::IgnoreCase))
	{
		EmitFramesWhereCounterExceeds(Session, *Params, Args, Json);
	}
	else if (Args.Intent.Equals(TEXT("events_inside_region"), ESearchCase::IgnoreCase))
	{
		EmitEventsInsideRegion(Session, *Params, Args, Json);
	}
	else if (Args.Intent.Equals(TEXT("logs_around_slow_frames"), ESearchCase::IgnoreCase))
	{
		EmitLogsAroundSlowFrames(Session, *Params, Args, Json);
	}
	else
	{
		WriteError(Json,
			FString::Printf(TEXT("unknown intent '%s'"), *Args.Intent),
			{ TEXT("list"),
			  TEXT("frames_where_counter_exceeds"),
			  TEXT("events_inside_region"),
			  TEXT("logs_around_slow_frames") });
	}

	Json.EndObject();
}

} // namespace TraceDigest

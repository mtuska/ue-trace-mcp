// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"

#include "Logging/LogVerbosity.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Log.h"

using namespace TraceServices;

namespace TraceDigest
{

// Map a user-supplied -verbosity= name to UE's ELogVerbosity threshold.
// "all" / empty / unknown all collapse to "Verbose" (= include everything).
// The check is "row.verbosity <= threshold" because UE's enum is ordered
// with Fatal=1 most-severe through Verbose=7 least.
static ELogVerbosity::Type ParseVerbosityFloor(const FString& V)
{
	if (V.IsEmpty()) return ELogVerbosity::Verbose;
	if (V.Equals(TEXT("fatal"),    ESearchCase::IgnoreCase)) return ELogVerbosity::Fatal;
	if (V.Equals(TEXT("error"),    ESearchCase::IgnoreCase)) return ELogVerbosity::Error;
	if (V.Equals(TEXT("warning"),  ESearchCase::IgnoreCase)) return ELogVerbosity::Warning;
	if (V.Equals(TEXT("warn"),     ESearchCase::IgnoreCase)) return ELogVerbosity::Warning;
	if (V.Equals(TEXT("display"),  ESearchCase::IgnoreCase)) return ELogVerbosity::Display;
	if (V.Equals(TEXT("log"),      ESearchCase::IgnoreCase)) return ELogVerbosity::Log;
	if (V.Equals(TEXT("verbose"),  ESearchCase::IgnoreCase)) return ELogVerbosity::Verbose;
	if (V.Equals(TEXT("all"),      ESearchCase::IgnoreCase)) return ELogVerbosity::Verbose;
	return ELogVerbosity::Verbose;
}

static const TCHAR* VerbosityName(ELogVerbosity::Type V)
{
	switch (V)
	{
		case ELogVerbosity::Fatal:        return TEXT("fatal");
		case ELogVerbosity::Error:        return TEXT("error");
		case ELogVerbosity::Warning:      return TEXT("warning");
		case ELogVerbosity::Display:      return TEXT("display");
		case ELogVerbosity::Log:          return TEXT("log");
		case ELogVerbosity::Verbose:      return TEXT("verbose");
		case ELogVerbosity::VeryVerbose:  return TEXT("very_verbose");
		default:                          return TEXT("unknown");
	}
}

// Windowed log enumeration. Optional filters: verbosity floor, exact
// category, case-insensitive substring grep. Output is bounded by
// `Args.Limit` (default 500); `truncated` flags when more rows existed in
// the window than were emitted.
void Modes::RunLogs(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const ILogProvider& Provider = ReadLogProvider(Session);

	const double EndSec = Session.GetDurationSeconds();
	const int32 Limit = Args.Limit > 0 ? Args.Limit : 500;
	const ELogVerbosity::Type Floor = ParseVerbosityFloor(Args.Verbosity);
	const bool bGrep = !Args.Grep.IsEmpty();

	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyNum(TEXT("duration_ms"),    EndSec * 1000.0);
	Json.KeyInt(TEXT("total_in_window"), static_cast<int64>(Provider.GetMessageCount()));
	Json.KeyStr(TEXT("verbosity_floor"), VerbosityName(Floor));
	Json.KeyStr(TEXT("category_filter"), Args.Category);
	Json.KeyStr(TEXT("grep"),            Args.Grep);

	int32 Emitted = 0;
	int32 Total   = 0;

	Json.Key(TEXT("events"));
	Json.BeginArray();
	Provider.EnumerateMessages(0.0, EndSec, [&](const FLogMessageInfo& Msg)
	{
		// Verbosity floor — Verbosity values strictly above the floor are
		// less severe and get filtered (UE's enum has Fatal=1, Verbose=7).
		if (Msg.Verbosity > Floor) return;

		const FString Category = (Msg.Category && Msg.Category->Name)
		                         ? FString(Msg.Category->Name) : FString();
		if (!Args.Category.IsEmpty() && !Category.Equals(Args.Category, ESearchCase::IgnoreCase))
		{
			return;
		}
		const FString Message = Msg.Message ? FString(Msg.Message) : FString();
		if (bGrep && !Message.Contains(Args.Grep, ESearchCase::IgnoreCase)) return;

		++Total;
		if (Emitted >= Limit) return;

		Json.BeginObject();
		Json.KeyInt(TEXT("idx"),       static_cast<int64>(Msg.Index));
		Json.KeyNum(TEXT("time_ms"),   Msg.Time * 1000.0);
		Json.KeyStr(TEXT("category"),  Category);
		Json.KeyStr(TEXT("verbosity"), FString(VerbosityName(Msg.Verbosity)));
		Json.KeyStr(TEXT("message"),   Message);
		Json.KeyStr(TEXT("file"),      Msg.File ? FString(Msg.File) : FString());
		Json.KeyInt(TEXT("line"),      static_cast<int64>(Msg.Line));
		Json.EndObject();
		++Emitted;
	});
	Json.EndArray();

	Json.KeyBool(TEXT("truncated"), Total > Emitted);
	Json.EndObject();
}

} // namespace TraceDigest

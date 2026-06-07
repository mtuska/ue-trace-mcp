// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"

#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Channel.h"

using namespace TraceServices;

namespace TraceDigest
{

// Enumerate every trace channel known to this `.utrace` capture. Output is
// flat: {file, mode, duration_ms, channels: [{id, name, enabled, read_only}]}.
// Channel count is tiny (~10-20), so we never need pagination or filtering
// here. The MCP server uses this to know which other tools have data to
// surface — e.g. don't bother calling `trace_memalloc_*` against a capture
// whose `memalloc` channel was never recorded.
void Modes::RunChannels(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const IChannelProvider* Provider = ReadChannelProvider(Session);

	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyNum(TEXT("duration_ms"), Session.GetDurationSeconds() * 1000.0);

	Json.Key(TEXT("channels"));
	Json.BeginArray();
	if (Provider)
	{
		const TArray<FChannelEntry>& Entries = Provider->GetChannels();
		for (const FChannelEntry& C : Entries)
		{
			Json.BeginObject();
			Json.KeyInt (TEXT("id"),        static_cast<int64>(C.Id));
			Json.KeyStr (TEXT("name"),      C.Name);
			Json.KeyBool(TEXT("enabled"),   C.bIsEnabled);
			Json.KeyBool(TEXT("read_only"), C.bReadOnly);
			Json.EndObject();
		}
	}
	Json.EndArray();
	Json.EndObject();
}

} // namespace TraceDigest

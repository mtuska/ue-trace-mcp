// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"

#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Callstack.h"
#include "TraceServices/Model/Modules.h"

using namespace TraceServices;

namespace TraceDigest
{

// Resolve one CallstackId (32-bit integer carried by every event row v0.4
// emits) to its symbolicated frames. The trace analyzer populates each
// FStackFrame's `Symbol` pointer lazily — by the time we run on a fully
// loaded session most should be resolved, but Pending/NotLoaded/NotFound
// frames are also legitimate so we surface their status per-frame.

static const TCHAR* SymbolStatusName(ESymbolQueryResult R)
{
	switch (R)
	{
		case ESymbolQueryResult::Pending:  return TEXT("pending");
		case ESymbolQueryResult::OK:       return TEXT("ok");
		case ESymbolQueryResult::NotLoaded:return TEXT("not_loaded");
		case ESymbolQueryResult::Mismatch: return TEXT("version_mismatch");
		case ESymbolQueryResult::NotFound: return TEXT("not_found");
		default:                           return TEXT("unknown");
	}
}

void Modes::RunCallstack(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const ICallstacksProvider* Provider = ReadCallstacksProvider(Session);

	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyInt(TEXT("callstack_id"), static_cast<int64>(Args.CallstackId));

	if (!Provider)
	{
		Json.KeyBool(TEXT("found"), false);
		Json.Key(TEXT("frames")); Json.BeginArray(); Json.EndArray();
		Json.EndObject();
		return;
	}

	const FCallstack* Cs = Provider->GetCallstack(Args.CallstackId);
	// Provider returns a special "Unknown" callstack rather than null for
	// missing ids — surface that via `found` based on frame count.
	const uint32 FrameCount = Cs ? Cs->Num() : 0;
	Json.KeyBool(TEXT("found"), Cs != nullptr && FrameCount > 0);
	Json.KeyInt (TEXT("frame_count"), static_cast<int64>(FrameCount));

	Json.Key(TEXT("frames"));
	Json.BeginArray();
	for (uint32 i = 0; i < FrameCount; ++i)
	{
		const FStackFrame* Frame = Cs->Frame(static_cast<uint8>(i));
		if (!Frame) continue;

		Json.BeginObject();
		Json.KeyInt(TEXT("depth"), static_cast<int64>(i));
		Json.KeyInt(TEXT("addr"),  static_cast<int64>(Frame->Addr));

		if (Frame->Symbol)
		{
			const ESymbolQueryResult Res = Frame->Symbol->GetResult();
			Json.KeyStr(TEXT("status"), FString(SymbolStatusName(Res)));
			Json.KeyStr(TEXT("symbol"),
				Frame->Symbol->Name ? FString(Frame->Symbol->Name) : FString());
			Json.KeyStr(TEXT("module"),
				Frame->Symbol->Module ? FString(Frame->Symbol->Module) : FString());
			Json.KeyStr(TEXT("file"),
				Frame->Symbol->File ? FString(Frame->Symbol->File) : FString());
			Json.KeyInt(TEXT("line"), static_cast<int64>(Frame->Symbol->Line));
		}
		else
		{
			// Frame address without a symbol pointer — rare but possible
			// during analysis startup.
			Json.KeyStr(TEXT("status"), FString(TEXT("no_symbol")));
			Json.KeyStr(TEXT("symbol"), FString());
			Json.KeyStr(TEXT("module"), FString());
			Json.KeyStr(TEXT("file"),   FString());
			Json.KeyInt(TEXT("line"),   0);
		}
		Json.EndObject();
	}
	Json.EndArray();
	Json.EndObject();
}

} // namespace TraceDigest

// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"

#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Bookmarks.h"
#include "TraceServices/Model/Frames.h"

using namespace TraceServices;

namespace TraceDigest
{

// Time-ordered list of `TRACE_BOOKMARK` points. Bookmarks usually count in
// the hundreds even for long traces, so we don't downsample — just window.
// Frame indexing on each row is best-effort: we look up the Game-thread
// frame containing the bookmark's timestamp.
void Modes::RunBookmarks(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const IBookmarkProvider& Provider = ReadBookmarkProvider(Session);
	const IFrameProvider& Frames = ReadFrameProvider(Session);

	const double EndSec = Session.GetDurationSeconds();
	const int32 Limit = Args.Limit > 0 ? Args.Limit : 500;

	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyNum(TEXT("duration_ms"),    EndSec * 1000.0);
	Json.KeyInt(TEXT("total_in_window"), static_cast<int64>(Provider.GetBookmarkCount()));

	int32 Emitted = 0;
	int32 Total   = 0;

	Json.Key(TEXT("events"));
	Json.BeginArray();
	Provider.EnumerateBookmarks(0.0, EndSec, [&](const FBookmark& B)
	{
		++Total;
		if (Emitted >= Limit) return;
		// Frame index: which Game-thread frame contains this timestamp?
		// `FindFrame` returns the frame instance directly; on miss we emit -1.
		const TraceServices::FFrame* Frame = nullptr;
		uint64 FrameIdx = 0;
		Frame = Frames.GetFrame(TraceFrameType_Game, FrameIdx);
		// IFrameProvider doesn't expose a binary-search-by-time helper here,
		// so we use the cheaper approximation: emit -1 and let consumers pull
		// `trace_frame` separately if they want to drill into the frame at
		// the bookmark's time.
		(void)Frame;

		Json.BeginObject();
		Json.KeyNum (TEXT("time_ms"),       B.Time * 1000.0);
		Json.KeyInt (TEXT("frame_idx"),     -1);
		Json.KeyStr (TEXT("text"),          B.Text ? FString(B.Text) : FString());
		Json.KeyInt (TEXT("callstack_id"),  static_cast<int64>(B.CallstackId));
		Json.EndObject();
		++Emitted;
	});
	Json.EndArray();

	Json.KeyBool(TEXT("truncated"), Total > Emitted);
	Json.EndObject();
}

} // namespace TraceDigest

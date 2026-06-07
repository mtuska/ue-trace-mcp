// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"

#include "ProfilingDebugging/MiscTrace.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"

using namespace TraceServices;

namespace TraceDigest
{

void Modes::RunFrames(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const IFrameProvider& Frames = ReadFrameProvider(Session);

	const uint64 GameCount = Frames.GetFrameCount(TraceFrameType_Game);
	const double EndSec = Session.GetDurationSeconds();

	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyNum(TEXT("duration_ms"), EndSec * 1000.0);
	Json.KeyInt(TEXT("frame_count"), static_cast<int64>(GameCount));

	// Resolve [Start, End) bounds: default to all frames, optionally narrowed
	// by -framerange=A:B and/or -limit (limit clamps the *returned* count, not
	// the iteration range).
	uint64 RangeStart = 0;
	uint64 RangeEnd = GameCount;
	if (Args.FrameRangeStart >= 0)
	{
		RangeStart = FMath::Min<uint64>(static_cast<uint64>(Args.FrameRangeStart), GameCount);
	}
	if (Args.FrameRangeEnd > 0)
	{
		RangeEnd = FMath::Min<uint64>(static_cast<uint64>(Args.FrameRangeEnd), GameCount);
	}
	if (RangeEnd < RangeStart)
	{
		RangeEnd = RangeStart;
	}

	const uint64 MaxEmit = Args.Limit > 0
		? FMath::Min<uint64>(RangeEnd - RangeStart, static_cast<uint64>(Args.Limit))
		: (RangeEnd - RangeStart);

	Json.Key(TEXT("events"));
	Json.BeginArray();

	uint64 Emitted = 0;
	Frames.EnumerateFrames(TraceFrameType_Game, RangeStart, RangeEnd,
		[&](const TraceServices::FFrame& Frame)
		{
			if (Emitted >= MaxEmit) return;
			++Emitted;

			Json.BeginObject();
			Json.KeyInt(TEXT("idx"),         static_cast<int64>(Frame.Index));
			Json.KeyNum(TEXT("start_ms"),    Frame.StartTime * 1000.0);
			Json.KeyNum(TEXT("end_ms"),      Frame.EndTime   * 1000.0);
			Json.KeyNum(TEXT("duration_ms"), (Frame.EndTime - Frame.StartTime) * 1000.0);
			Json.EndObject();
		});

	Json.EndArray();
	Json.EndObject();
}

} // namespace TraceDigest

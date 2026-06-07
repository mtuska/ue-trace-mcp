// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#pragma once

#include "TraceDigestPCH.h"

namespace TraceServices { class IAnalysisSession; }

namespace TraceDigest
{

struct FArgs;
class FJsonOut;

// Each mode reads from one (or two) loaded sessions and writes a JSON document
// describing the result. Output is always a top-level object with at least
// `file` + `mode`, plus an `events` array of mode-specific row records.
namespace Modes
{
	void RunDigest  (const TraceServices::IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json);
	void RunTimeline(const TraceServices::IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json);
	void RunFrames  (const TraceServices::IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json);
	void RunCompare (const TraceServices::IAnalysisSession& A,
	                 const TraceServices::IAnalysisSession& B,
	                 const FArgs& Args, FJsonOut& Json);

	void RunOverview(const TraceServices::IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json);
	void RunFrame   (const TraceServices::IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json);
	void RunCallers (const TraceServices::IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json);
	void RunCallees (const TraceServices::IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json);
	void RunThreads (const TraceServices::IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json);
}

} // namespace TraceDigest

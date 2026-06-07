// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#pragma once

#include "TraceServices/Model/AnalysisSession.h"

namespace TraceDigest
{

// RAII wrapper around `IProvider::BeginRead/EndRead`. UE's analysis pipeline
// has a two-level locking model: an FAnalysisSessionReadScope locks the
// session as a whole, and each provider that owns its own data structures
// (regions, memory, allocations, …) additionally has its own per-provider
// lock that must be held while reading. ITimingProfilerProvider /
// IFrameProvider / IThreadProvider / IChannelProvider get along fine with
// just the session scope; providers backed by their own paged storage do
// not, and assert with "Trying to READ from provider outside of a READ
// scope" if a session-scope-only caller touches them.
struct FProviderReadScope
{
	const TraceServices::IProvider& Provider;
	explicit FProviderReadScope(const TraceServices::IProvider& InProvider) : Provider(InProvider)
	{
		Provider.BeginRead();
	}
	~FProviderReadScope()
	{
		Provider.EndRead();
	}
};

} // namespace TraceDigest

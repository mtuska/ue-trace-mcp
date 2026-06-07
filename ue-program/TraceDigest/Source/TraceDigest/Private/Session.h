// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#pragma once

#include "TraceDigestPCH.h"
#include "Templates/SharedPointer.h"

namespace TraceServices
{
	class IAnalysisService;
	class IAnalysisSession;
	class IModuleService;
}

namespace TraceDigest
{

// Owns the TraceServices analysis pipeline for one .utrace file.
// Loads the file synchronously; subsequent reads use the returned session
// under a FAnalysisSessionReadScope.
class FLoadedTrace
{
public:
	FLoadedTrace();
	~FLoadedTrace();

	// Load the trace, using a persistent on-disk analysis cache (default).
	bool Load(const FString& FilePath, FString& OutError);

	// Lower-level: optionally skip the persistent cache and run a clean parse.
	// Useful for debugging cache issues or for traces that change in-place at
	// the same path without an mtime bump.
	bool LoadEx(const FString& FilePath, bool bDisableCache, FString& OutError);

	TSharedPtr<const TraceServices::IAnalysisSession> GetSession() const { return Session; }

private:
	TSharedPtr<TraceServices::IAnalysisService> AnalysisService;
	TSharedPtr<TraceServices::IModuleService>   ModuleService;
	TSharedPtr<const TraceServices::IAnalysisSession> Session;
};

} // namespace TraceDigest

// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Session.h"
#include "Args.h"

#include "Hash/CityHash.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Trace/DataStream.h"

#include "TraceServices/AnalysisService.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/ModuleService.h"

namespace TraceDigest
{

// Minimal IInDataStream implementation that reads a .utrace from disk into the
// TraceServices analysis pipeline. Mirrors FAnalysisService::StartAnalysis'
// internal FFileDataStream, but is needed here because we have to call
// StartAnalysis ourselves to override the session name (and therefore the
// cache file path) — see notes below.
class FUtraceFileStream : public UE::Trace::IInDataStream
{
public:
	virtual int32 Read(void* Data, uint32 Size) override
	{
		if (Remaining <= 0) return 0;
		if (Size > Remaining) Size = static_cast<uint32>(Remaining);
		if (!Handle->Read(static_cast<uint8*>(Data), Size)) return 0;
		Remaining -= Size;
		return static_cast<int32>(Size);
	}

	TUniquePtr<IFileHandle> Handle;
	uint64 Remaining = 0;
};

// Build a per-trace cache file path. Hash key includes:
//   * absolute trace path (so two traces don't collide)
//   * size + mtime (mtime change invalidates by producing a different filename)
//
// Lives under `<ApplicationSettingsDir>/TraceDigest/Cache/<hash>.utdc`. On Linux
// that's `~/.config/Epic/TraceDigest/Cache/`. Old caches accumulate; we don't
// GC them — `rm -rf` if it grows.
static FString DeriveCachePath(const FString& TraceFile)
{
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

	const FString AbsPath  = FPaths::ConvertRelativePathToFull(TraceFile);
	const int64    Size    = PF.FileSize(*AbsPath);
	const FDateTime MTime  = PF.GetTimeStamp(*AbsPath);

	const FString Key = FString::Printf(TEXT("%s|%lld|%lld"),
	                                    *AbsPath, Size, MTime.GetTicks());
	const FTCHARToUTF8 KeyUtf8(*Key);
	const uint64 Hash = CityHash64(reinterpret_cast<const char*>(KeyUtf8.Get()),
	                               static_cast<uint64>(KeyUtf8.Length()));

	const FString CacheDir = FPaths::Combine(
		FPlatformProcess::ApplicationSettingsDir(),
		TEXT("TraceDigest"), TEXT("Cache"));
	PF.CreateDirectoryTree(*CacheDir);

	return FPaths::Combine(CacheDir, FString::Printf(TEXT("%016llx.utdc"), Hash));
}

FLoadedTrace::FLoadedTrace() = default;
FLoadedTrace::~FLoadedTrace() = default;

bool FLoadedTrace::Load(const FString& FilePath, FString& OutError)
{
	return LoadEx(FilePath, /*DisableCache=*/false, OutError);
}

bool FLoadedTrace::LoadEx(const FString& FilePath, bool bDisableCache, FString& OutError)
{
	if (!FPaths::FileExists(FilePath))
	{
		OutError = FString::Printf(TEXT("trace file not found: %s"), *FilePath);
		return false;
	}

	ITraceServicesModule& TraceModule = FModuleManager::LoadModuleChecked<ITraceServicesModule>("TraceServices");

	// Editor context: the engine already created singleton TraceServices on
	// startup, and Create* would assert on the duplicate. Get* returns the
	// existing instance there. Program context: nothing has touched TraceServices
	// yet, so Get* returns null and we have to create our own.
	ModuleService = TraceModule.GetModuleService();
	if (!ModuleService.IsValid())
	{
		ModuleService = TraceModule.CreateModuleService();
	}

	AnalysisService = TraceModule.GetAnalysisService();
	if (!AnalysisService.IsValid())
	{
		AnalysisService = TraceModule.CreateAnalysisService();
	}

	if (!AnalysisService.IsValid() || !ModuleService.IsValid())
	{
		OutError = TEXT("failed to obtain TraceServices analysis pipeline");
		return false;
	}

	// The session's `Name` doubles as the cache file path in FAnalysisCache.
	// FAnalysisService::Analyze passes the .utrace path as the name, which is
	// wrong: that file is opened as a cache and its contents read as cache
	// table-of-contents, fails the format check, silently goes transient. So
	// we replicate Analyze's internals here but pass our own derived cache
	// path as the session name.
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	IFileHandle* RawHandle = PF.OpenRead(*FilePath, /*bAllowWrite=*/true);
	if (!RawHandle)
	{
		OutError = FString::Printf(TEXT("could not open trace for read: %s"), *FilePath);
		return false;
	}

	const FString CachePath = bDisableCache ? FilePath : DeriveCachePath(FilePath);

	auto Stream = MakeUnique<FUtraceFileStream>();
	Stream->Handle    = TUniquePtr<IFileHandle>(RawHandle);
	Stream->Remaining = RawHandle->Size();

	// SessionId ~0 matches what FAnalysisService::StartAnalysis uses for file-
	// based traces (vs. live captures, which carry a real trace id).
	Session = AnalysisService->StartAnalysis(~uint32(0), *CachePath, MoveTemp(Stream));
	if (!Session.IsValid())
	{
		OutError = FString::Printf(TEXT("TraceServices failed to start analysis: %s"), *FilePath);
		return false;
	}

	Session->Wait();
	if (!Session->IsAnalysisComplete())
	{
		OutError = TEXT("analysis session did not reach completion");
		return false;
	}
	return true;
}

} // namespace TraceDigest

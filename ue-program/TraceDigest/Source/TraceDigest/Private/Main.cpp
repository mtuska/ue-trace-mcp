// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "RequiredProgramMainCPPInclude.h"

#include "Args.h"
#include "Daemon.h"
#include "Session.h"
#include "Modes.h"
#include "JsonOut.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "TraceServices/Model/AnalysisSession.h"

DEFINE_LOG_CATEGORY_STATIC(LogTraceDigest, Log, All);

IMPLEMENT_APPLICATION(TraceDigest, "TraceDigest");

static bool EmitJson(const TraceDigest::FJsonOut& Json, const FString& OutPath, FString& OutError)
{
	const FString Payload = Json.ToString() + LINE_TERMINATOR;
	if (OutPath.IsEmpty())
	{
		FPlatformMisc::LocalPrint(*Payload);
		return true;
	}
	if (!FFileHelper::SaveStringToFile(Payload, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("failed to write %s"), *OutPath);
		return false;
	}
	return true;
}

static int32 RunWithArgs(const TraceDigest::FArgs& Args)
{
	using namespace TraceDigest;

	FLoadedTrace TraceA;
	FString LoadErr;
	if (!TraceA.LoadEx(Args.File, Args.bDisableCache, LoadErr))
	{
		UE_LOG(LogTraceDigest, Error, TEXT("%s"), *LoadErr);
		return 2;
	}

	FJsonOut Json;

	switch (Args.Mode)
	{
		case EMode::Digest:   Modes::RunDigest  (*TraceA.GetSession(), Args, Json); break;
		case EMode::Timeline: Modes::RunTimeline(*TraceA.GetSession(), Args, Json); break;
		case EMode::Frames:   Modes::RunFrames  (*TraceA.GetSession(), Args, Json); break;
		case EMode::Overview: Modes::RunOverview(*TraceA.GetSession(), Args, Json); break;
		case EMode::Frame:    Modes::RunFrame   (*TraceA.GetSession(), Args, Json); break;
		case EMode::Callers:  Modes::RunCallers (*TraceA.GetSession(), Args, Json); break;
		case EMode::Callees:  Modes::RunCallees (*TraceA.GetSession(), Args, Json); break;
		case EMode::Threads:   Modes::RunThreads  (*TraceA.GetSession(), Args, Json); break;
		case EMode::Channels:  Modes::RunChannels (*TraceA.GetSession(), Args, Json); break;
		case EMode::Gpu:       Modes::RunGpu      (*TraceA.GetSession(), Args, Json); break;
		case EMode::Counters:  Modes::RunCounters (*TraceA.GetSession(), Args, Json); break;
		case EMode::Bookmarks: Modes::RunBookmarks(*TraceA.GetSession(), Args, Json); break;
		case EMode::Regions:   Modes::RunRegions  (*TraceA.GetSession(), Args, Json); break;
		case EMode::Logs:      Modes::RunLogs     (*TraceA.GetSession(), Args, Json); break;
		case EMode::Memory:    Modes::RunMemory     (*TraceA.GetSession(), Args, Json); break;
		case EMode::Allocations: Modes::RunAllocations(*TraceA.GetSession(), Args, Json); break;
		case EMode::Query:       Modes::RunQuery      (*TraceA.GetSession(), Args, Json); break;

		case EMode::Compare:
		{
			FLoadedTrace TraceB;
			if (!TraceB.LoadEx(Args.File2, Args.bDisableCache, LoadErr))
			{
				UE_LOG(LogTraceDigest, Error, TEXT("%s"), *LoadErr);
				return 2;
			}
			Modes::RunCompare(*TraceA.GetSession(), *TraceB.GetSession(), Args, Json);
			break;
		}
	}

	FString WriteErr;
	if (!EmitJson(Json, Args.OutPath, WriteErr))
	{
		UE_LOG(LogTraceDigest, Error, TEXT("%s"), *WriteErr);
		return 3;
	}
	return 0;
}

INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	FTaskTagScope Scope(ETaskTag::EGameThread);
	ON_SCOPE_EXIT
	{
		RequestEngineExit(TEXT("Exiting"));
		FEngineLoop::AppPreExit();
		FModuleManager::Get().UnloadModulesAtShutdown();
		FEngineLoop::AppExit();
	};

	// PreInit handles -trace= flags and engine-internal args. Our own flags
	// (-mode, -file, etc.) pass through and we parse them out below.
	if (const int32 PreInitResult = GEngineLoop.PreInit(ArgC, ArgV))
	{
		return PreInitResult;
	}

	// Reassemble user-visible command line for our own parser. PreInit already
	// stripped engine flags from FCommandLine::Get(), so what remains here is
	// the user's argv.
	FString CmdLine = FCommandLine::Get();

	TraceDigest::FArgs Args;
	FString ParseErr;
	if (!Args.Parse(*CmdLine, ParseErr))
	{
		UE_LOG(LogTraceDigest, Error, TEXT("%s"), *ParseErr);
		UE_LOG(LogTraceDigest, Display,
			TEXT("usage: TraceDigest -file=<path> [-mode=digest|timeline|frames|compare|overview|frame|callers|callees|threads] "
			     "[-file2=<path>] [-prefix=Zombie_] [-event=Name] [-frame=N] [-framerange=A:B] "
			     "[-limit=N] [-threshold=ms] [-out=<path>]"));
		return 1;
	}

	if (Args.bDaemonMode)
	{
		return TraceDigest::RunDaemon(Args);
	}
	return RunWithArgs(Args);
}

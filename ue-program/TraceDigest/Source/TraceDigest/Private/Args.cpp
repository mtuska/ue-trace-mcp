// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Args.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace TraceDigest
{

const TCHAR* FArgs::ModeName(EMode M)
{
	switch (M)
	{
		case EMode::Digest:   return TEXT("digest");
		case EMode::Timeline: return TEXT("timeline");
		case EMode::Frames:   return TEXT("frames");
		case EMode::Compare:  return TEXT("compare");
		case EMode::Overview: return TEXT("overview");
		case EMode::Frame:    return TEXT("frame");
		case EMode::Callers:  return TEXT("callers");
		case EMode::Callees:  return TEXT("callees");
		case EMode::Threads:   return TEXT("threads");
		case EMode::Channels:  return TEXT("channels");
		case EMode::Gpu:       return TEXT("gpu");
		case EMode::Counters:  return TEXT("counters");
		case EMode::Bookmarks: return TEXT("bookmarks");
		case EMode::Regions:   return TEXT("regions");
		case EMode::Logs:      return TEXT("logs");
	}
	return TEXT("digest");
}

bool FArgs::Parse(const TCHAR* CmdLine, FString& OutError)
{
	FString ModeStr;
	if (FParse::Value(CmdLine, TEXT("-mode="), ModeStr))
	{
		if      (ModeStr.Equals(TEXT("digest"),   ESearchCase::IgnoreCase)) { Mode = EMode::Digest;   }
		else if (ModeStr.Equals(TEXT("timeline"), ESearchCase::IgnoreCase)) { Mode = EMode::Timeline; }
		else if (ModeStr.Equals(TEXT("frames"),   ESearchCase::IgnoreCase)) { Mode = EMode::Frames;   }
		else if (ModeStr.Equals(TEXT("compare"),  ESearchCase::IgnoreCase)) { Mode = EMode::Compare;  }
		else if (ModeStr.Equals(TEXT("overview"), ESearchCase::IgnoreCase)) { Mode = EMode::Overview; }
		else if (ModeStr.Equals(TEXT("frame"),    ESearchCase::IgnoreCase)) { Mode = EMode::Frame;    }
		else if (ModeStr.Equals(TEXT("callers"),  ESearchCase::IgnoreCase)) { Mode = EMode::Callers;  }
		else if (ModeStr.Equals(TEXT("callees"),  ESearchCase::IgnoreCase)) { Mode = EMode::Callees;  }
		else if (ModeStr.Equals(TEXT("threads"),   ESearchCase::IgnoreCase)) { Mode = EMode::Threads;   }
		else if (ModeStr.Equals(TEXT("channels"),  ESearchCase::IgnoreCase)) { Mode = EMode::Channels;  }
		else if (ModeStr.Equals(TEXT("gpu"),       ESearchCase::IgnoreCase)) { Mode = EMode::Gpu;       }
		else if (ModeStr.Equals(TEXT("counters"),  ESearchCase::IgnoreCase)) { Mode = EMode::Counters;  }
		else if (ModeStr.Equals(TEXT("bookmarks"), ESearchCase::IgnoreCase)) { Mode = EMode::Bookmarks; }
		else if (ModeStr.Equals(TEXT("regions"),   ESearchCase::IgnoreCase)) { Mode = EMode::Regions;   }
		else if (ModeStr.Equals(TEXT("logs"),      ESearchCase::IgnoreCase)) { Mode = EMode::Logs;      }
		else
		{
			OutError = FString::Printf(TEXT("unknown -mode='%s'"), *ModeStr);
			return false;
		}
	}

	FParse::Value(CmdLine, TEXT("-file="), File);
	FParse::Value(CmdLine, TEXT("-file2="), File2);
	FParse::Value(CmdLine, TEXT("-prefix="), Prefix);
	FParse::Value(CmdLine, TEXT("-event="), Event);
	FParse::Value(CmdLine, TEXT("-out="), OutPath);
	FParse::Value(CmdLine, TEXT("-limit="), Limit);
	FParse::Value(CmdLine, TEXT("-threshold="), Threshold);
	FParse::Value(CmdLine, TEXT("-frame="), FrameIndex);

	// v0.4 channel-aware flags. All optional; modes that need them validate
	// post-parse below.
	FParse::Value(CmdLine, TEXT("-view="),       View);
	FParse::Value(CmdLine, TEXT("-counter="),    Counter);
	FParse::Value(CmdLine, TEXT("-category="),   Category);
	FParse::Value(CmdLine, TEXT("-verbosity="),  Verbosity);
	FParse::Value(CmdLine, TEXT("-grep="),       Grep);
	FParse::Value(CmdLine, TEXT("-buckets="),    Buckets);
	FParse::Value(CmdLine, TEXT("-queue="),      Queue);
	if (Buckets <= 0) Buckets = 256;

	if (FParse::Param(CmdLine, TEXT("nocache")))
	{
		bDisableCache = true;
	}

	if (FParse::Param(CmdLine, TEXT("daemon")))
	{
		bDaemonMode = true;
	}
	FParse::Value(CmdLine, TEXT("-socket="), SocketPath);
	FParse::Value(CmdLine, TEXT("-idle-timeout="), IdleTimeoutSec);
	if (IdleTimeoutSec <= 0) IdleTimeoutSec = 600;

	FString FrameRange;
	if (FParse::Value(CmdLine, TEXT("-framerange="), FrameRange))
	{
		FString Lhs, Rhs;
		if (FrameRange.Split(TEXT(":"), &Lhs, &Rhs))
		{
			FrameRangeStart = FCString::Atoi(*Lhs);
			FrameRangeEnd = FCString::Atoi(*Rhs);
		}
		else
		{
			OutError = FString::Printf(TEXT("invalid -framerange='%s' (expected A:B)"), *FrameRange);
			return false;
		}
	}

	if (File.IsEmpty())
	{
		OutError = TEXT("missing required -file=<path>");
		return false;
	}
	if (Mode == EMode::Compare && File2.IsEmpty())
	{
		OutError = TEXT("-mode=compare requires -file2=<path>");
		return false;
	}
	if ((Mode == EMode::Timeline || Mode == EMode::Callers || Mode == EMode::Callees) && Event.IsEmpty())
	{
		OutError = FString::Printf(TEXT("-mode=%s requires -event=<name>"), ModeName(Mode));
		return false;
	}
	if (Mode == EMode::Frame && FrameIndex < 0)
	{
		OutError = TEXT("-mode=frame requires -frame=<index>");
		return false;
	}
	if (Mode == EMode::Gpu)
	{
		if (View.IsEmpty()) View = TEXT("queues");
		if (!View.Equals(TEXT("queues"), ESearchCase::IgnoreCase)
			&& !View.Equals(TEXT("timeline"), ESearchCase::IgnoreCase)
			&& !View.Equals(TEXT("fences"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("-mode=gpu unknown -view='%s' (expected queues|timeline|fences)"), *View);
			return false;
		}
	}
	if (Mode == EMode::Counters && !Counter.IsEmpty())
	{
		// Series mode — counter name supplied implies the series view; no
		// extra validation here. Catalogue mode (no -counter=) has no
		// required args.
	}
	if (Limit <= 0)
	{
		Limit = 200;
	}
	return true;
}

} // namespace TraceDigest

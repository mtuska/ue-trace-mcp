// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#pragma once

#include "TraceDigestPCH.h"

namespace TraceDigest
{

enum class EMode : uint8
{
	Digest,    // per-event aggregate stats over whole trace
	Timeline,  // every instance of one event
	Frames,    // per-frame totals/breakdown
	Compare,   // diff two traces
	Overview,  // composite: top-N events + frame stats + slowest frames
	Frame,     // single-frame: every event that ran during one frame
	Callers,   // butterfly: who called this event
	Callees,   // butterfly: what this event called
	Threads,   // per-thread CPU breakdown
};

struct FArgs
{
	FString File;
	FString File2;        // -file2= for compare mode
	FString Prefix;       // -prefix=Zombie_ filter timer names
	FString Event;        // -event= for timeline/callers/callees mode
	FString OutPath;      // -out= optional output file; default = stdout
	EMode Mode = EMode::Digest;
	int32 Limit = 200;          // top-N rows
	int32 FrameRangeStart = -1; // -framerange=A:B (inclusive A, exclusive B)
	int32 FrameRangeEnd = -1;
	int32 FrameIndex = -1;      // -frame=N for single-frame mode
	double Threshold = 0.0;     // -threshold= for compare (ms delta floor)
	bool bDisableCache = false; // -nocache to skip the persistent analysis cache

	// Daemon mode (Program target only). The Program loads the trace once and
	// services repeated queries over a Unix socket until idle timeout or
	// SIGTERM. The plugin commandlet path ignores these flags.
	bool   bDaemonMode    = false; // -daemon
	FString SocketPath;            // -socket=/path/to/daemon.sock
	int32  IdleTimeoutSec = 600;   // -idle-timeout=<sec>; default 10 min

	bool Parse(const TCHAR* CmdLine, FString& OutError);

	static const TCHAR* ModeName(EMode M);
};

} // namespace TraceDigest

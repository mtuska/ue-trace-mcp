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
	Channels,  // enumerate trace channels present in the capture
	Gpu,       // GPU queues + per-queue timeline + fences (view-dispatched)
	Counters,  // counter catalogue + per-counter time series (view-dispatched)
	Bookmarks, // TRACE_BOOKMARK point markers
	Regions,   // TRACE_BEGIN/END_REGION time spans
	Logs,      // captured UE_LOG output, windowed + filtered
	Memory,    // LLM tag tree + per-tag time-bucketed samples (view-dispatched)
	Allocations, // Per-allocation tracking (timeline/heaps/query view-dispatched)
	Query,     // Intent-dispatched cross-cutting queries (trace_query)
	Callstack, // Resolve one CallstackId to symbolicated frames
	Modules,   // List discovered modules + symbol-resolution stats
	TaskList,  // Windowed enumeration of tasks with state filter
	TaskDrill, // Full info on one task (prerequisites/subsequents/parents/nested)
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

	// v0.4 channel-aware additions. Empty/zero defaults mean "not supplied"
	// — modes that need a value validate post-parse.
	FString Channel;       // -channel=cpu|gpu|region for agnostic verbs (digest/timeline/callers/callees/compare). Default cpu.
	FString View;          // -view=<name> for umbrella modes (gpu, counters, …)
	FString Counter;       // -counter=<name> for counter series queries
	FString Category;      // -category=<name> for regions/logs filtering
	FString Verbosity;     // -verbosity=<error|warn|display|log|verbose|all>
	FString Grep;          // -grep=<substr> for log message filtering
	FString Tracker;       // -tracker=<id|name> for memory views
	FString Tag;           // -tag=<id|name> for memory sample queries
	FString Rule;          // -rule=<name> for allocation queries (aAf|afA|Aaf|AafB|…)
	FString Intent;        // -intent=<name> for trace_query
	FString Params;        // -params=<json> opaque parameter blob for trace_query
	int32 Buckets = 256;   // -buckets=<N> bins for time-series downsampling
	int32 Queue = -1;      // -queue=<id> for per-queue GPU views
	double TimeA = -1.0;   // -timeA=<sec> query rule time anchor
	double TimeB = -1.0;   // -timeB=<sec> query rule time anchor
	int32 QueryTimeoutMs = 60000;  // -query-timeout-ms=<N> for sync-wrapped allocations queries

	// v0.5: callstack/symbolication
	uint32 CallstackId = 0;  // -callstack-id=<uint32> for trace_callstack lookup

	// v0.5: tasks
	uint64 TaskId = ~uint64(0);  // -task-id=<uint64> for trace_task_drill (TaskTrace::InvalidId default)
	FString State;               // -state=<Alive|Launched|Active|WaitingForPrerequisites|Queued|Executing|WaitingForNested|Completed>

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

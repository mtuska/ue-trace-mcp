# ue-trace-mcp

LLM-driven Unreal Engine `.utrace` profiling. Two pieces:

- **`ue-program/TraceDigest/`** — a standalone UE `Program` target (same shape as `UnrealPak`). Reads a `.utrace` via the engine's `TraceServices` module and writes structured JSON. Supports a long-lived daemon mode that keeps the parsed session resident across queries. Requires a source-built engine.
- **`mcp-server/`** — a TypeScript MCP server that wraps the binary. Spawns daemons on demand, caches output JSON, manages lifecycle. 11 tools.

A pure-TypeScript parser was considered and ruled out: the wire layer exists in `EpicGames.Tracing` (~800 LOC C#) but full semantic analysis (timing-scope reconstruction, percentiles per timer, frame/butterfly/thread providers) lives in `Engine/Source/Developer/TraceServices/` C++ with no port.

## Repo layout

```
ue-trace-mcp/
├── Makefile                        # build / link / test orchestration
├── ue-program/TraceDigest/         # standalone Program target
│   ├── README.md
│   └── Source/
│       ├── TraceDigest.Target.cs   # Type = Program
│       └── TraceDigest/
│           ├── TraceDigest.Build.cs
│           └── Private/
│               ├── Main.cpp                # INT32_MAIN entry — dispatch to daemon or one-shot
│               ├── TraceDigestPCH.h
│               ├── Args.{h,cpp}            # -mode -file -prefix -event -framerange -limit -threshold -out -nocache -daemon -socket -idle-timeout
│               ├── Session.{h,cpp}         # FLoadedTrace: opens .utrace via TraceServices, derives a sane cache file path
│               ├── Daemon.{h,cpp}          # Unix-socket server + request loop + idle timer
│               ├── Percentiles.h           # Reservoir sampling (cap 200k/timer)
│               ├── JsonOut.{h,cpp}         # Streaming JSON writer
│               ├── TimerLookup.h           # Resolve timer name -> TimerId(s)
│               ├── Modes.h
│               └── Modes/
│                   ├── Digest.cpp Timeline.cpp Frames.cpp Compare.cpp
│                   └── Overview.cpp Frame.cpp Butterfly.cpp Threads.cpp
└── mcp-server/                     # TypeScript MCP server
    ├── package.json
    ├── src/
    │   ├── index.ts                # stdio entrypoint + signal/EOF reaper
    │   ├── server.ts               # tool registration (11 tools)
    │   ├── digest.ts               # daemon-or-one-shot router
    │   ├── daemon.ts               # DaemonRegistry: spawn/track/query/reap
    │   ├── memory.ts               # /proc/meminfo + VmRSS + admission control
    │   ├── cache.ts                # output JSON LRU by (path, mtime, variant)
    │   ├── schemas.ts              # zod schemas
    │   ├── tools.ts                # per-tool dispatchers
    │   └── types.ts                # shape of every mode output
    └── tests/                      # vitest, no UE required (mock binary)
```

## Quick start

1. Symlink the program source into your engine and build it:

   ```bash
   make build-program UE_SOURCE=/abs/path/to/UnrealEngine
   # produces $UE_SOURCE/Engine/Binaries/Linux/TraceDigest (~300KB)
   ```

2. Build the MCP server:

   ```bash
   make build-mcp
   ```

3. Wire it into your MCP config (e.g. `<project>/.mcp.json`):

   ```jsonc
   {
     "mcpServers": {
       "ue-trace": {
         "command": "node",
         "args": ["/abs/path/to/ue-trace-mcp/mcp-server/dist/index.js"],
         "env": {
           "TRACE_DIGEST_BIN": "/abs/path/to/UnrealEngine/Engine/Binaries/Linux/TraceDigest"
         }
       }
     }
   }
   ```

4. Smoke-test:

   ```bash
   make smoke         FILE=/abs/path/to/foo.utrace   # direct binary
   make smoke-daemon  FILE=/abs/path/to/foo.utrace   # cold + warm timing through daemon
   ```

## Daemon mode

Each call doesn't re-parse the trace. The Program supports a long-lived daemon: load the trace once, then service queries over a Unix socket until idle timeout or reap.

Architecture:
- **One daemon process per `(absolute path × mtime)` trace.** Distinct traces get distinct daemons.
- **Lazy spawn** on first query. Subsequent queries on the same trace skip parse entirely (~10–200ms per call instead of ~1.7s).
- **Memory admission**: before spawning, MCP reads `/proc/meminfo`, estimates the trace's footprint (~4× file size), and refuses with a clear error if loading would leave less than `TRACE_MIN_FREE_MB` (default 1024 MiB) available. Also respects `TRACE_MAX_DAEMON_MB` per-daemon ceiling if set.
- **Idle self-kill** after `TRACE_IDLE_TIMEOUT_SEC` (default 600 = 10 min). Defense in depth — survives MCP crash.
- **LRU eviction** when more than `TRACE_MAX_DAEMONS` (default 5) traces are resident.
- **MCP shutdown reaper**: when the MCP server gets SIGTERM/SIGINT or its stdin closes, it terminates every spawned daemon with SIGTERM → SIGKILL grace window. No orphans.
- **Explicit unload**: `trace_unload({ file })` evicts a daemon immediately. `trace_status({})` lists current daemons with PIDs, uptime, idle time, estimated and actual RSS.

Environment overrides:
- `TRACE_DIGEST_BIN` — Program binary path (required)
- `TRACE_IDLE_TIMEOUT_SEC` — seconds before a daemon self-kills (default 600)
- `TRACE_MAX_DAEMONS` — max resident daemons (default 5)
- `TRACE_MIN_FREE_MB` — required free system memory after load (default 1024)
- `TRACE_MAX_DAEMON_MB` — per-daemon RSS ceiling (default -1 = disabled)

## Tool surface

Eleven MCP tools. Each one's `inputSchema` is a plain JSON-Schema object (no top-level anyOf — Claude Code drops those).

| tool | purpose |
| --- | --- |
| `trace_overview` | First-look summary: frame stats + 3 slowest frames + top-N events. Call this first. |
| `trace_digest` | Per-event aggregates: count, total_ms, P50/P95/P99, max_ms. Filters: `prefix`, `eventNames`. |
| `trace_timeline` | Every instance of one event with `{frame_idx, start_ms, duration_ms}`. |
| `trace_frames` | Per-frame durations, sorted by `idx` or `duration_ms`. |
| `trace_frame` | Drill into one frame: every CPU event with thread, depth, start_ms, duration_ms. |
| `trace_callers` | Butterfly upward: direct callers of an event, ranked by inclusive_ms. |
| `trace_callees` | Butterfly downward: direct callees of an event. |
| `trace_threads` | Per-thread CPU breakdown with top-10 timers per thread. |
| `trace_compare` | Diff two traces, ranked by \|Δ P95\|. Surfaces only-in-a / only-in-b events. |
| `trace_unload` | Evict the daemon resident for one trace. Frees memory immediately. |
| `trace_status` | Per-daemon PID/RSS/uptime + system memory. Use to check what's resident. |

Every mode emits its row payload under `events`.

## Analysis cache

TraceServices ships with an `IAnalysisCache` that persists analyzer state to disk between runs. We wire it up correctly (cache files land at `~/.config/Epic/TraceDigest/Cache/<hash>.utdc`, hashed on absolute trace path + size + mtime so changes invalidate cleanly), but **in current engine versions only the `ModuleProvider` (symbol resolution) actually feeds the cache**. The timing profiler, frames, threads, counters and so on parse from scratch each invocation. So for CPU-heavy traces, the cache is plumbing-only — no measurable cold-start win today, but a free win if Epic extends provider usage in the future, and no longer at risk of accidentally treating the `.utrace` itself as a cache file (which is what the default code path tries to do).

Pass `-nocache` to disable the cache lookup entirely.

The real fix for "the LLM keeps re-parsing the same trace" is **daemon mode** — keep a parsed `IAnalysisSession` resident across calls. That's the default behavior.

## Status

v0.3 scope:

- CPU events only (the `cpu` channel)
- File-based traces only (no live ingestion)
- Read-only
- True P50 / P95 / P99 via reservoir sampling (200k cap per timer)
- Daemon mode with memory budget, idle reap, MCP-shutdown teardown

Out of scope for v1: GPU/Verse timing, bookmarks/counters/regions, GUI integration, memory tracking, Windows daemon (POSIX-only socket code).

## Binary-engine path (removed)

Earlier versions shipped a project plugin with a `UCommandlet` that worked against binary/installed engines. It's been dropped — the Program path is ~6× faster (1.7s cold vs ~10s editor cold) and the daemon mode it enables is a much bigger win again. If you only have a binary engine, see git history at `v0.3` and revert; otherwise build the engine from source.

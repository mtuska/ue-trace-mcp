# ue-trace-mcp

LLM-driven Unreal Engine `.utrace` profiling. Two pieces:

- **`ue-program/TraceDigest/`** — a standalone UE `Program` target (same shape as `UnrealPak`). Reads a `.utrace` via the engine's `TraceServices` module and writes structured JSON. Supports a long-lived daemon mode that keeps the parsed session resident across queries. Requires a source-built engine.
- **`mcp-server/`** — a TypeScript MCP server that wraps the binary. Spawns daemons on demand, caches output JSON, manages lifecycle. **26 tools** spanning every UE trace category.

A pure-TypeScript parser was considered and ruled out: the wire layer exists in `EpicGames.Tracing` (~800 LOC C#) but full semantic analysis (timing-scope reconstruction, percentiles per timer, frame/butterfly/thread/region/counter/memory/allocation providers) lives in `Engine/Source/Developer/TraceServices/` C++ with no port.

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
│               ├── Args.{h,cpp}            # all -mode/-file/-channel/-view/... flags
│               ├── Session.{h,cpp}         # FLoadedTrace: opens .utrace via TraceServices, derives a sane cache file path
│               ├── Daemon.{h,cpp}          # Unix-socket server + accept loop + idle timer + LOADING_PROGRESS heartbeat
│               ├── Percentiles.h           # Reservoir sampling (cap 200k/timer) + GetPercentile helper
│               ├── JsonOut.{h,cpp}         # Streaming JSON writer
│               ├── TimerLookup.h           # Resolve timer name -> TimerId(s)
│               ├── ProviderReadScope.h     # RAII helper for per-provider BeginRead/EndRead pairs
│               ├── Modes.h
│               └── Modes/
│                   ├── SeriesBucket.h                     # time-bucketed downsampler (counter/memory/memalloc)
│                   ├── Digest.cpp Timeline.cpp Frames.cpp Compare.cpp     # cpu+gpu+region channels for the agnostic verbs
│                   ├── Overview.cpp Frame.cpp Butterfly.cpp Threads.cpp   # cpu drill-downs
│                   ├── Channels.cpp                                       # IChannelProvider
│                   ├── Gpu.cpp                                            # GPU queues/timeline/fences (incl. legacy fallback)
│                   ├── Counters.cpp Bookmarks.cpp Regions.cpp Logs.cpp    # the per-channel reads
│                   ├── Memory.cpp                                         # LLM tag tree + samples (IMemoryProvider)
│                   ├── Allocations.cpp                                    # per-alloc (IAllocationsProvider, sync-polled)
│                   └── Query.cpp                                          # trace_query intent dispatch
└── mcp-server/                     # TypeScript MCP server
    ├── package.json
    ├── src/
    │   ├── index.ts                # stdio entrypoint + signal/EOF reaper
    │   ├── server.ts               # tool registration (26 tools via McpServer)
    │   ├── digest.ts               # daemon-or-one-shot router + argv builder
    │   ├── daemon.ts               # DaemonRegistry: spawn/track/query/reap + LOADING_PROGRESS capture
    │   ├── binary.ts               # lazy GH-release download with SHA256 verify
    │   ├── memory.ts               # /proc/meminfo + VmRSS + admission control
    │   ├── cache.ts                # output JSON LRU by (path, mtime, variant)
    │   ├── schemas.ts              # zod schemas (per-tool input shapes)
    │   ├── tools.ts                # per-tool dispatchers
    │   └── types.ts                # shape of every mode output
    └── tests/                      # vitest, no UE required (mock binary)
```

## Quick start

Wire the MCP server into your `<project>/.mcp.json` — that's it. No prebuild, no binary management:

```jsonc
{
  "mcpServers": {
    "ue-trace": {
      "command": "npx",
      "args": ["-y", "@mtuska/ue-trace-mcp"]
    }
  }
}
```

On first invocation the package downloads the matching `TraceDigest` binary (~10 MB) from the GitHub release tagged at its own npm version into `~/.cache/ue-trace-mcp/<version>/`, verifies its SHA-256 against the manifest baked into the npm package, and reuses it afterward. Linux x64 and Windows x64 are supported out of the box; macOS isn't shipped yet (set `TRACE_DIGEST_BIN` to a locally-built binary).

Versions stay in lockstep automatically: `npx @mtuska/ue-trace-mcp@0.4.0` always pulls the `v0.4.0` release's `TraceDigest`.

### Dev / from-source path

If you're hacking on this repo or running against a locally-built binary:

```bash
make build-program UE_SOURCE=/abs/path/to/UnrealEngine
make build-mcp
```

Then point `.mcp.json` at the local server + binary:

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

`TRACE_DIGEST_BIN` overrides the lazy-download path entirely.

### Smoke-test

```bash
make smoke         FILE=/abs/path/to/foo.utrace   # direct binary
make smoke-daemon  FILE=/abs/path/to/foo.utrace   # cold + warm timing through daemon
```

## Daemon mode

Each call doesn't re-parse the trace. The Program supports a long-lived daemon: load the trace once, then service queries over a Unix socket until idle timeout or reap.

Architecture:
- **One daemon process per `(absolute path × mtime)` trace.** Distinct traces get distinct daemons.
- **Lazy spawn** on first query. Subsequent queries on the same trace skip parse entirely (~10–200ms per call instead of ~1.7s for ZombieProto-scale, or ~20s for the 1GB Editor capture).
- **Memory admission**: before spawning, MCP reads `/proc/meminfo`, estimates the trace's footprint (~4× file size; bump to ~8× when memalloc is captured), and refuses with a clear error if loading would leave less than `TRACE_MIN_FREE_MB` (default 1024 MiB) available. Also respects `TRACE_MAX_DAEMON_MB` per-daemon ceiling if set.
- **Load-progress visibility**: the binary emits `LOADING_PROGRESS: elapsed_ms=<N>` to stderr every ~500ms while it's parsing the trace; the MCP server captures those and exposes them via `trace_status`'s new `loading[]` array. The LLM can poll `trace_status` mid-load to tell a busy daemon apart from a stuck one (if `wall_elapsed_ms` keeps climbing while `last_progress_elapsed_ms` freezes, the binary is hung).
- **Idle self-kill** after `TRACE_IDLE_TIMEOUT_SEC` (default 600 = 10 min). Defense in depth — survives MCP crash.
- **LRU eviction** when more than `TRACE_MAX_DAEMONS` (default 5) traces are resident.
- **MCP shutdown reaper**: when the MCP server gets SIGTERM/SIGINT or its stdin closes, it terminates every spawned daemon with SIGTERM → SIGKILL grace window. No orphans.
- **Explicit unload**: `trace_unload({ file })` evicts a daemon immediately. `trace_status({})` lists current daemons with PIDs, uptime, idle time, estimated and actual RSS — plus any daemons mid-load.

Environment overrides:
- `TRACE_DIGEST_BIN` — Program binary path (overrides the lazy-download path)
- `TRACE_IDLE_TIMEOUT_SEC` — seconds before a daemon self-kills (default 600)
- `TRACE_MAX_DAEMONS` — max resident daemons (default 5)
- `TRACE_MIN_FREE_MB` — required free system memory after load (default 1024)
- `TRACE_MAX_DAEMON_MB` — per-daemon RSS ceiling (default -1 = disabled)
- `TRACE_SPAWN_TIMEOUT_SEC` — hard timeout from spawn to `DAEMON_READY` (default 600). Multi-GB traces may take longer than the previous 30s cap.

## Tool surface

26 MCP tools. Each one's `inputSchema` is a plain JSON-Schema object — Claude Code and other MCP clients accept them cleanly.

The naming convention separates **channel-agnostic verbs** (flat-named, take a `channel` param) from **channel-bound verbs** (`trace_<channel>_<purpose>`).

### Cross-channel / agnostic (6)

| tool | purpose |
| --- | --- |
| `trace_overview` | First-look snapshot. Top-level keys: `channels[]`, `cpu`, `memory`, `memalloc`. The `cpu` sub-block keeps the v0.3 shape verbatim (`frame_count`, `frame_stats`, `slowest_frames`, `events`) — only the path prefix changed. |
| `trace_channels` | Enumerate channels present in the capture with their enabled/read-only flags. Call this first so the LLM knows which other tools have data. |
| `trace_frame` | Drill into one frame: every CPU event that ran during it. Already cross-channel by intent. |
| `trace_query` | Intent-dispatched escape hatch. Pick an intent + params for cross-cutting questions the specific tools can't answer (see below). |
| `trace_unload` | Evict the daemon for one trace. Frees memory immediately. |
| `trace_status` | Per-daemon PID/RSS/uptime + system memory + `loading[]` for in-flight spawns. |

### Channel-agnostic verbs with `channel` param (6)

Default channel is `cpu` — every v0.3 call site works unchanged.

| tool | channel support |
| --- | --- |
| `trace_digest` | `cpu` (default), `gpu`, `region`. Per-event aggregates: count, total_ms, P50/P95/P99, max_ms. |
| `trace_timeline` | `cpu` (default), `gpu`, `region`. Every instance of one named event/region with `{frame_idx, start_ms, duration_ms}`. |
| `trace_callers` | `cpu` (default), `gpu`. Butterfly upward via `FCreateButterflyParams` — UE natively supports both filter types. |
| `trace_callees` | `cpu` (default), `gpu`. Butterfly downward. |
| `trace_compare` | `cpu` only in v0.4.0. Diff two traces, ranked by \|Δ P95\|. Non-cpu channels error with a clear message. |
| `trace_frames` | `frame_type: game` (default). Per-frame durations, sorted by `idx` or `duration_ms`. |

### Channel-bound (14)

| channel | tools |
| --- | --- |
| **cpu** | `trace_cpu_threads` — per-thread breakdown with top-10 timers (renamed from `trace_threads`) |
| **gpu** | `trace_gpu_queues` — GPU queue list (incl. legacy Gpu1/Gpu2 fallback); `trace_gpu_fences` — resolved cross-queue fence pairs with stall_ms |
| **counter** | `trace_counter_catalogue` — every counter + metadata; `trace_counter_series` — time-bucketed values for one counter |
| **bookmark** | `trace_bookmark_list` — `TRACE_BOOKMARK` annotations |
| **region** | `trace_region_list` — `TRACE_BEGIN/END_REGION` spans, optionally filtered by category. Carries a per-row `open` flag for spans that never closed. |
| **log** | `trace_log_messages` — windowed `UE_LOG` enumeration with verbosity floor / category / grep filters |
| **memory** (LLM tags) | `trace_memory_trackers` / `trace_memory_tags` / `trace_memory_samples` — tag tree + per-tag time series |
| **memalloc** (per-alloc) | `trace_memalloc_timeline` (aggregate stats), `trace_memalloc_heaps` (FHeapSpec tree), `trace_memalloc_query` (rule-based query, sync-wrapped) |

Every row payload sits under `events` (or `series` / `tags` / `heaps` where the shape isn't event-like — documented per-tool).

### `trace_query` intents

`trace_query` is the escape hatch for questions that need joining two providers. Call with `intent: "list"` to discover the registry; the LLM can self-describe what's available:

| intent | joins | params |
| --- | --- | --- |
| `frames_where_counter_exceeds` | frames × counters | `{ counter, threshold, op?: ">" \| ">=" }` |
| `events_inside_region` | regions × CPU timing | `{ region, event_prefix?, limit? }` |
| `logs_around_slow_frames` | frames × logs | `{ p_threshold?: 0.95, window_ms?: 50, limit? }` |

New intents land on the C++ side without forcing a TS schema bump — `QueryArgs.intent` is `z.string()`, not an enum.

## Analysis cache

TraceServices ships with an `IAnalysisCache` that persists analyzer state to disk between runs. We wire it up correctly (cache files land at `~/.config/Epic/TraceDigest/Cache/<hash>.utdc`, hashed on absolute trace path + size + mtime so changes invalidate cleanly), but **in current engine versions only the `ModuleProvider` (symbol resolution) actually feeds the cache**. The timing profiler, frames, threads, counters and so on parse from scratch each invocation. So for CPU-heavy traces, the cache is plumbing-only — no measurable cold-start win today, but a free win if Epic extends provider usage in the future, and no longer at risk of accidentally treating the `.utrace` itself as a cache file (which is what the default code path tries to do).

Pass `-nocache` to disable the cache lookup entirely.

The real fix for "the LLM keeps re-parsing the same trace" is **daemon mode** — keep a parsed `IAnalysisSession` resident across calls. That's the default behavior.

## Releases

Cutting a release:

```bash
# 1. Bump the version in mcp-server/package.json (e.g. 0.4.0)
# 2. Commit, then tag the same version with a `v` prefix:
git tag v0.4.0
git push origin v0.4.0
```

The `release` workflow takes over: it validates the tag matches `package.json`, builds the `TraceDigest` binary on Linux + Windows runners against UE 5.7, bakes a `binaries.json` manifest with per-platform SHA-256 hashes into the npm package, publishes the `@mtuska/ue-trace-mcp` npm package via Trusted Publishing (OIDC), and creates a GitHub release with both binaries attached as `TraceDigest-v0.4.0-ue5.7-{linux-x64,windows-x64}.{tar.gz,zip}`.

To dry-run the binary build without publishing, run the `build-program` workflow manually from the Actions tab (it accepts a `ue_branch` input — default `5.7`).

Required repo secret (Settings → Secrets and variables → Actions):

- **`UE_TOKEN`** — a GitHub Personal Access Token for an account that has accepted Epic's EULA on epicgames.com and joined the [EpicGames GitHub organisation](https://www.unrealengine.com/en-US/ue-on-github). Scope: `repo` (read). Without this the workflow can't clone `EpicGames/UnrealEngine`.

## Status

v0.4 scope:

- **Every UE trace category**: cpu, gpu (incl. legacy Gpu1/Gpu2 fallback), frames (game), regions, bookmarks, counters, logs, memory (LLM tags), memalloc (per-allocation)
- Channel-aware `trace_overview` with per-category sub-blocks
- `trace_query` intent dispatcher with 3 starter intents (frames↔counters, events↔regions, logs↔frames)
- Daemon mode with memory budget, idle reap, MCP-shutdown teardown
- **Load-progress visibility** via `trace_status.loading[]` for multi-GB captures
- True P50 / P95 / P99 via reservoir sampling (200k cap per timer)
- Lazy GH-release binary fetch with SHA-256 verification anchored in the npm-provenance-signed package
- File-based traces only (no live ingestion)

Out of scope for v0.4: CSV-Profiler captures (rarely-used UE feature); stack samples / sampled callstacks; task graph; net trace; Slate/asset/cook providers; non-CPU `trace_compare` (land in v0.5+); Windows daemon mode (POSIX-only socket code — Windows one-shot still works fine).

## Binary-engine path (removed)

Earlier versions shipped a project plugin with a `UCommandlet` that worked against binary/installed engines. It's been dropped — the Program path is ~6× faster (1.7s cold vs ~10s editor cold) and the daemon mode it enables is a much bigger win again. If you only have a binary engine, see git history at `v0.3` and revert; otherwise build the engine from source.

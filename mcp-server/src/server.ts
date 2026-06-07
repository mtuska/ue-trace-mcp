import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import type { Server } from "@modelcontextprotocol/sdk/server/index.js";
import type { ZodObject, ZodRawShape, ZodTypeAny } from "zod";

import { TraceCache } from "./cache.js";
import {
  doAssetExports,
  doAssetPackages,
  doAssetRequests,
  doBookmarkList,
  doCallees,
  doCallers,
  doCallstack,
  doChannels,
  doCompare,
  doCounterCatalogue,
  doCounterSeries,
  doCpuThreads,
  doDigest,
  doFrame,
  doFrames,
  doGpuFences,
  doGpuQueues,
  doLogMessages,
  doMemallocHeaps,
  doMemallocQuery,
  doMemallocTimeline,
  doQuery,
  doMemorySamples,
  doMemoryTags,
  doMemoryTrackers,
  doModules,
  doOverview,
  doTaskDrill,
  doTaskList,
  doRegionList,
  doStatus,
  doTimeline,
  doUnload,
  type ToolContext,
} from "./tools.js";
import {
  AssetExportsArgs,
  AssetPackagesArgs,
  AssetRequestsArgs,
  BookmarkListArgs,
  CallersArgs,
  CalleesArgs,
  CallstackArgs,
  ChannelsArgs,
  CompareArgs,
  CounterCatalogueArgs,
  CounterSeriesArgs,
  CpuThreadsArgs,
  DigestArgs,
  FrameArgs,
  FramesArgs,
  GpuFencesArgs,
  GpuQueuesArgs,
  LogMessagesArgs,
  MemallocHeapsArgs,
  MemallocQueryArgs,
  MemallocTimelineArgs,
  ModulesArgs,
  QueryArgs,
  TaskDrillArgs,
  TaskListArgs,
  MemorySamplesArgs,
  MemoryTagsArgs,
  MemoryTrackersArgs,
  OverviewArgs,
  RegionListArgs,
  StatusArgs,
  TimelineArgs,
  UnloadArgs,
} from "./schemas.js";
import type { DigestRunOptions } from "./digest.js";
import { DaemonRegistry } from "./daemon.js";

export interface ServerOptions {
  runOptions?: DigestRunOptions;
  cacheSize?: number;
  /** Set false to disable daemon mode entirely (always one-shot). Defaults to true when a binary path is configured. */
  enableDaemon?: boolean;
  /** Optional registry override; otherwise constructed from env + runOptions.binary. */
  daemons?: DaemonRegistry;
}

// Server-level instructions surfaced via the MCP `instructions` field on
// `initialize`. Clients that show a server description (including Claude
// Code, Claude Desktop) read this. Keep it terse — the per-tool
// descriptions carry the details; this is the orientation pass.
const SERVER_INSTRUCTIONS = `\
ue-trace-mcp wraps Unreal Engine .utrace captures. It surfaces every UE
trace category (cpu, gpu, frames, regions, bookmarks, counters, logs,
memory/LLM tags, per-allocation tracking) as MCP tools.

Suggested workflow for a fresh trace:
  1. trace_channels    — see which channels were actually captured.
  2. trace_overview    — first-look snapshot. CPU frame stats + slowest
                         frames + top events live under \`cpu\`; memory
                         and memalloc summaries sit alongside.
  3. Then drill down with the channel-specific tools or use
     trace_query (intent-dispatched) for cross-cutting questions.

Naming convention:
  * Flat names (trace_overview, trace_digest, …) when the verb is
    channel-agnostic — pass \`channel: "cpu" | "gpu" | "region"\` to
    redirect; default is cpu.
  * trace_<channel>_<purpose> when the verb is tied to one provider
    (trace_cpu_threads, trace_gpu_queues, trace_memory_tags, …).

Long loads: multi-GB traces can take minutes to parse. trace_status now
returns a \`loading[]\` array alongside \`daemons[]\`; each entry carries
\`wall_elapsed_ms\` (MCP-side clock) and \`last_progress_elapsed_ms\`
(the binary's own heartbeat, ~500ms cadence). If wall keeps climbing
but last_progress freezes, the load is stuck.

trace_query intents (use intent="list" to discover the registry):
  * frames_where_counter_exceeds — find frames where a counter crosses
    a threshold.
  * events_inside_region — list CPU events that ran inside named
    region instances.
  * logs_around_slow_frames — for every frame past a pX threshold,
    return logs in a window around it.
`;

// Tool definitions are kept as a list so we can register them in one loop.
// Each schema is a `z.object(...)` — McpServer.registerTool wants the raw
// shape (Record<string, ZodTypeAny>) so we pass `schema.shape` at
// registration time. We keep the generic loose at the list level (ZodTypeAny)
// so each entry's handler can accept its own narrowed arg type without the
// container forcing a single uniform shape.
interface ToolDef<S extends ZodTypeAny = ZodTypeAny> {
  name: string;
  description: string;
  schema: S;
  handler: (args: ReturnType<S["parse"]>, ctx: ToolContext) => Promise<unknown>;
}

export interface BuiltServer {
  /**
   * The underlying low-level Server (exposed through McpServer.server). The
   * stdio entrypoint uses `.connect(transport)` on this object; tests poke
   * into its `_requestHandlers` map.
   */
  server: Server;
  daemons: DaemonRegistry | null;
}

export function createServer(opts: ServerOptions = {}): BuiltServer {
  const cache = new TraceCache(opts.cacheSize ?? 5);
  const runOptions = opts.runOptions ?? {};

  // Daemon registry is set up only when we have a Program binary to spawn.
  // Editor-commandlet mode doesn't support daemon (we don't want to keep an
  // entire editor process resident per trace).
  const binary = runOptions.binary ?? process.env.TRACE_DIGEST_BIN;
  const enableDaemon = (opts.enableDaemon ?? true) && !!binary;

  const daemons: DaemonRegistry | null =
    opts.daemons ??
    (enableDaemon
      ? new DaemonRegistry({
          binary: binary!,
          idleTimeoutSec: Number(process.env.TRACE_IDLE_TIMEOUT_SEC ?? 600),
          maxDaemons: Number(process.env.TRACE_MAX_DAEMONS ?? 5),
        })
      : null);
  if (daemons) daemons.startReaper();

  // runOptions.daemons forwarded so digest.ts can route into the registry.
  const runOptionsWithDaemons: DigestRunOptions = daemons
    ? { ...runOptions, daemons }
    : runOptions;

  const ctx: ToolContext = {
    cache,
    runOptions: runOptionsWithDaemons,
    daemons: daemons ?? undefined,
  };

  const tools: ToolDef[] = [
    {
      name: "trace_overview",
      description:
        "First-look summary of a .utrace in one call: frame stats (min/avg/p50/p95/p99/max), " +
        "top 3 slowest frames, and top-N hottest events with their percentiles. " +
        "Use this before any other tool to orient on a new trace.",
      schema: OverviewArgs,
      handler: (a, c) => doOverview(a, c),
    },
    {
      name: "trace_digest",
      description:
        "Per-event aggregate stats: count, total_ms, avg_ms, true P50/P95/P99, max_ms. " +
        "Filters: prefix (timer-name prefix), eventNames (exact names).",
      schema: DigestArgs,
      handler: (a, c) => doDigest(a, c),
    },
    {
      name: "trace_timeline",
      description:
        "Every instance of one timer in the trace, with {frame_idx, start_ms, duration_ms}. " +
        "Use to investigate per-frame variance once digest has pointed at a suspect event.",
      schema: TimelineArgs,
      handler: (a, c) => doTimeline(a, c),
    },
    {
      name: "trace_frames",
      description:
        "Per-frame durations for Game-thread frames. Sort by 'idx' (default) or 'duration_ms'. " +
        "Use to find frame-rate spikes; combine with trace_frame to drill into a specific one.",
      schema: FramesArgs,
      handler: (a, c) => doFrames(a, c),
    },
    {
      name: "trace_frame",
      description:
        "Drill into a single Game-thread frame: every CPU event that ran during it, with thread + " +
        "depth + start_ms + duration_ms, sorted longest first. Pair with trace_frames to investigate spikes.",
      schema: FrameArgs,
      handler: (a, c) => doFrame(a, c),
    },
    {
      name: "trace_callers",
      description:
        "Butterfly view: direct callers of a timer, ranked by inclusive_ms. " +
        "Use to answer 'who is invoking <hot event>?'.",
      schema: CallersArgs,
      handler: (a, c) => doCallers(a, c),
    },
    {
      name: "trace_callees",
      description:
        "Butterfly view: direct callees of a timer, ranked by inclusive_ms. " +
        "Use to answer 'what does <hot event> spend its time on?'.",
      schema: CalleesArgs,
      handler: (a, c) => doCallees(a, c),
    },
    {
      name: "trace_cpu_threads",
      description:
        "Per-thread CPU breakdown: event_count, total_depth0_ms (rough 'thread busy' proxy), and " +
        "top-10 timers per thread. Use to locate which thread is doing the work or to spot parallelism gaps.",
      schema: CpuThreadsArgs,
      handler: (a, c) => doCpuThreads(a, c),
    },
    {
      name: "trace_compare",
      description:
        "Diff two .utrace captures, ranked by |Δ P95|. Surfaces events that exist on only one side " +
        "(only_in_a / only_in_b) — those are usually the most interesting regression signals.",
      schema: CompareArgs,
      handler: (a, c) => doCompare(a, c),
    },
    {
      name: "trace_unload",
      description:
        "Evict the daemon resident for one trace, freeing its memory immediately. " +
        "Use after you're done investigating a trace, or before loading a much bigger one. " +
        "No-op if no daemon is currently loaded for that file.",
      schema: UnloadArgs,
      handler: (a, c) => doUnload(a, c),
    },
    {
      name: "trace_status",
      description:
        "Report current daemon state: per-trace PID, uptime, idle time, estimated and actual RSS, " +
        "plus system memory available AND any daemons mid-load via `loading[]`. Multi-GB traces " +
        "can take minutes to parse; the LLM can poll this tool to tell a busy daemon apart from " +
        "a stuck one. Each `loading` row carries `wall_elapsed_ms` (server clock since spawn) and " +
        "`last_progress_elapsed_ms` (the binary's own self-reported heartbeat) — divergence " +
        "between the two flags a stuck process.",
      schema: StatusArgs,
      handler: (a, c) => doStatus(a, c),
    },
    {
      name: "trace_channels",
      description:
        "Enumerate the trace channels present in the .utrace capture (cpu, gpu, memory, memalloc, " +
        "counter, log, bookmark, region, …). Use to find out which other tools will return data — " +
        "e.g. don't bother calling trace_memalloc_* against a capture whose memalloc channel was " +
        "never recorded.",
      schema: ChannelsArgs,
      handler: (a, c) => doChannels(a, c),
    },
    {
      name: "trace_gpu_queues",
      description:
        "List GPU queues seen in the capture (one per GPU × queue index). Includes the timeline index " +
        "used by trace_digest({channel:'gpu'}) and trace_timeline({channel:'gpu'}).",
      schema: GpuQueuesArgs,
      handler: (a, c) => doGpuQueues(a, c),
    },
    {
      name: "trace_gpu_fences",
      description:
        "Resolved GPU cross-queue fence pairs (signal on queue A matched with wait on queue B). " +
        "Each row carries the stall duration; ranked by file order. Use to find cross-queue stalls.",
      schema: GpuFencesArgs,
      handler: (a, c) => doGpuFences(a, c),
    },
    {
      name: "trace_counter_catalogue",
      description:
        "Enumerate every TRACE_COUNTER_* known to the trace, with metadata (group, type, display hint). " +
        "Use to discover counter names before calling trace_counter_series.",
      schema: CounterCatalogueArgs,
      handler: (a, c) => doCounterCatalogue(a, c),
    },
    {
      name: "trace_counter_series",
      description:
        "Time-bucketed values for one named counter ({t_ms, min, max, avg, count} per bucket). " +
        "Default 256 buckets evenly distributed across the trace; the response is O(buckets) " +
        "regardless of underlying sample density.",
      schema: CounterSeriesArgs,
      handler: (a, c) => doCounterSeries(a, c),
    },
    {
      name: "trace_bookmark_list",
      description:
        "Time-ordered TRACE_BOOKMARK points with optional callstack id. Use to find named " +
        "annotations in the capture.",
      schema: BookmarkListArgs,
      handler: (a, c) => doBookmarkList(a, c),
    },
    {
      name: "trace_region_list",
      description:
        "TRACE_BEGIN/END_REGION spans, optionally filtered to one category. Each row carries depth " +
        "so consumers can reconstruct stacking. Returns a categories[] hint listing every known " +
        "category in the trace.",
      schema: RegionListArgs,
      handler: (a, c) => doRegionList(a, c),
    },
    {
      name: "trace_log_messages",
      description:
        "Windowed UE_LOG enumeration with verbosity floor + category + case-insensitive grep filters. " +
        "Default cap 500 messages; `truncated:true` flags when more rows existed in the window.",
      schema: LogMessagesArgs,
      handler: (a, c) => doLogMessages(a, c),
    },
    {
      name: "trace_memory_trackers",
      description:
        "List LLM memory trackers (Default, Platform, …) and tag sets registered for this trace. " +
        "Empty trackers list means the LLM channel wasn't captured in this run.",
      schema: MemoryTrackersArgs,
      handler: (a, c) => doMemoryTrackers(a, c),
    },
    {
      name: "trace_memory_tags",
      description:
        "Enumerate LLM tags as a flat list with parent_id for tree reconstruction. Optional " +
        "-tracker= filter restricts to tags exposed by one tracker; -tag= focuses on one tag + " +
        "its ancestor chain (useful for explaining a single LLM bucket).",
      schema: MemoryTagsArgs,
      handler: (a, c) => doMemoryTags(a, c),
    },
    {
      name: "trace_memory_samples",
      description:
        "Time-bucketed memory samples for one LLM tag from one tracker ({t_ms, min, max, avg, count} " +
        "per bucket). Default 256 buckets across the trace; default tracker is 0 (UE's 'Default'). " +
        "Use trace_memory_tags to discover tag ids/names.",
      schema: MemorySamplesArgs,
      handler: (a, c) => doMemorySamples(a, c),
    },
    {
      name: "trace_memalloc_timeline",
      description:
        "Aggregate allocation stats across the trace: max_total_allocated_memory, max_live_allocations, " +
        "alloc_events / free_events per timeline point. Each metric is time-bucketed into ~256 " +
        "(configurable) buckets so the response is O(buckets). Empty arrays mean the memalloc channel " +
        "wasn't captured.",
      schema: MemallocTimelineArgs,
      handler: (a, c) => doMemallocTimeline(a, c),
    },
    {
      name: "trace_memalloc_heaps",
      description:
        "Heap-spec tree (FHeapSpec). Small. Use to discover root heap ids that show up in trace_memalloc_query rows.",
      schema: MemallocHeapsArgs,
      handler: (a, c) => doMemallocHeaps(a, c),
    },
    {
      name: "trace_memalloc_query",
      description:
        "Rule-based query over individual allocations. The async StartQuery/PollQuery API is " +
        "sync-wrapped inside the binary with a per-call timeout (default 60s) so the MCP call stays " +
        "synchronous. Rules: aAf (active at A), afA (freed before A), Aaf (allocated after A), " +
        "aAfB (active at A, freed before B). Results capped at -limit (default 200) and `truncated` " +
        "is set when more rows existed in the query result set or the timeout fired before " +
        "completion.",
      schema: MemallocQueryArgs,
      handler: (a, c) => doMemallocQuery(a, c),
    },
    {
      name: "trace_query",
      description:
        "Cross-cutting escape hatch for questions the specific tools can't answer. Pick one intent " +
        "and pass its params. Call with intent='list' to discover available intents. Initial " +
        "intents: frames_where_counter_exceeds (find frames where a counter exceeds a threshold), " +
        "events_inside_region (list CPU events inside named region instances), " +
        "logs_around_slow_frames (logs in a window around each frame past a pX threshold).",
      schema: QueryArgs,
      handler: (a, c) => doQuery(a, c),
    },
    {
      name: "trace_callstack",
      description:
        "Resolve one callstack_id (carried by every CPU/GPU/allocation/bookmark event row) to its " +
        "symbolicated frames. Each frame returns {depth, addr, status, symbol, module, file, line}. " +
        "status is 'ok' / 'pending' / 'not_loaded' / 'version_mismatch' / 'not_found' / 'no_symbol' — " +
        "non-ok statuses usually mean the parent module wasn't loaded; check trace_modules.",
      schema: CallstackArgs,
      handler: (a, c) => doCallstack(a, c),
    },
    {
      name: "trace_modules",
      description:
        "List every discovered module with load status (loaded/pending/not_found/version_mismatch/…) " +
        "plus per-module symbol-resolution stats (discovered/cached/resolved/failed/available). " +
        "Pair with trace_callstack to diagnose why a frame's symbol came back unresolved.",
      schema: ModulesArgs,
      handler: (a, c) => doModules(a, c),
    },
    {
      name: "trace_task_list",
      description:
        "Windowed enumeration of UE task-graph tasks. Filter by `state` " +
        "(Alive/Launched/Active/WaitingForPrerequisites/Queued/Executing/WaitingForNested/Completed) " +
        "and time via frameRange. Each row carries id, debug_name, thread, key timestamps, and " +
        "prerequisite/subsequent counts. Production traces have 10⁶–10⁷ tasks — `truncated:true` " +
        "is normal; drill into specific ids with trace_task_drill.",
      schema: TaskListArgs,
      handler: (a, c) => doTaskList(a, c),
    },
    {
      name: "trace_task_drill",
      description:
        "Full FTaskInfo for one task id from trace_task_list: every timestamp (created/launched/" +
        "scheduled/started/finished/completed/destroyed), every thread id, and the four relation " +
        "arrays (prerequisites, subsequents, parent_tasks, nested_tasks). Use to investigate " +
        "scheduling stalls and dependency chains.",
      schema: TaskDrillArgs,
      handler: (a, c) => doTaskDrill(a, c),
    },
    {
      name: "trace_asset_packages",
      description:
        "Per-package load timings (FPackagesTableRow). Each row: name, total_serialized_size, " +
        "main_thread_ms vs async_loading_ms split, summary (header size, import/export counts, " +
        "priority), and imported_packages_count. Use to answer \"why is my map loading slowly?\". " +
        "Empty total_in_window means the loadtime channel wasn't captured.",
      schema: AssetPackagesArgs,
      handler: (a, c) => doAssetPackages(a, c),
    },
    {
      name: "trace_asset_requests",
      description:
        "Top-level load requests (FRequestsTableRow). Each row: id, name, start_ms, duration_ms, " +
        "package_count. A 'request' usually corresponds to a LoadPackage / async-load call site.",
      schema: AssetRequestsArgs,
      handler: (a, c) => doAssetRequests(a, c),
    },
    {
      name: "trace_asset_exports",
      description:
        "Per-export breakdown (FExportsTableRow). Each row: id, class, owning package, " +
        "serialized_size, main_thread_ms vs async_loading_ms split, and event_type " +
        "(create/serialize/postload). Lets you find which UObject classes dominate load time.",
      schema: AssetExportsArgs,
      handler: (a, c) => doAssetExports(a, c),
    },
  ];

  const mcp = new McpServer(
    { name: "ue-trace-mcp", version: "0.4.0-dev" },
    {
      capabilities: { tools: {} },
      instructions: SERVER_INSTRUCTIONS,
    },
  );

  // Register every tool with McpServer. The high-level API:
  //   * validates incoming args against the zod shape (we pass `schema.shape`,
  //     a ZodRawShape — McpServer wraps it in a ZodObject internally and
  //     uses zod-to-json-schema to publish a plain-object inputSchema)
  //   * passes the validated args to our handler
  //   * lets us return a regular CallToolResult; throws are caught and
  //     surfaced as isError responses
  for (const t of tools) {
    mcp.registerTool(
      t.name,
      {
        description: t.description,
        // Cast: at the container level we keep ToolDef<ZodTypeAny> so each
        // handler retains its narrowed arg type, but at registration time
        // every schema IS a ZodObject and we want its .shape.
        inputSchema: (t.schema as ZodObject<ZodRawShape>).shape,
      },
      async (args) => {
        try {
          const result = await t.handler(args as ReturnType<typeof t.schema.parse>, ctx);
          return { content: [{ type: "text" as const, text: JSON.stringify(result) }] };
        } catch (e) {
          const err = e as Error & { stderr?: string };
          return {
            isError: true,
            content: [
              {
                type: "text" as const,
                text: err.stderr
                  ? `${err.message}\n--- stderr ---\n${err.stderr}`
                  : err.message,
              },
            ],
          };
        }
      },
    );
  }

  return { server: mcp.server, daemons };
}

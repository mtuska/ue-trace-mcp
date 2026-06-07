import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";
import { zodToJsonSchema } from "zod-to-json-schema";
import type { ZodTypeAny } from "zod";

import { TraceCache } from "./cache.js";
import {
  doCallees,
  doCallers,
  doChannels,
  doCompare,
  doCpuThreads,
  doDigest,
  doFrame,
  doFrames,
  doOverview,
  doStatus,
  doTimeline,
  doUnload,
  type ToolContext,
} from "./tools.js";
import {
  CallersArgs,
  CalleesArgs,
  ChannelsArgs,
  CompareArgs,
  CpuThreadsArgs,
  DigestArgs,
  FrameArgs,
  FramesArgs,
  OverviewArgs,
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

interface ToolDef<S extends ZodTypeAny> {
  name: string;
  description: string;
  schema: S;
  handler: (args: ReturnType<S["parse"]>, ctx: ToolContext) => Promise<unknown>;
}

export interface BuiltServer {
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

  const tools: ToolDef<ZodTypeAny>[] = [
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
        "plus system memory available. Use to see what's resident and how much headroom you have.",
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
  ];

  const byName = new Map(tools.map((t) => [t.name, t] as const));

  const server = new Server(
    { name: "ue-trace-mcp", version: "0.4.0-dev" },
    { capabilities: { tools: {} } },
  );

  server.setRequestHandler(ListToolsRequestSchema, async () => ({
    tools: tools.map((t) => ({
      name: t.name,
      description: t.description,
      inputSchema: zodToJsonSchema(t.schema, { $refStrategy: "none" }) as Record<string, unknown>,
    })),
  }));

  server.setRequestHandler(CallToolRequestSchema, async (req) => {
    const tool = byName.get(req.params.name);
    if (!tool) {
      return {
        isError: true,
        content: [{ type: "text", text: `unknown tool: ${req.params.name}` }],
      };
    }

    const parsed = tool.schema.safeParse(req.params.arguments);
    if (!parsed.success) {
      return {
        isError: true,
        content: [{ type: "text", text: `invalid arguments: ${parsed.error.message}` }],
      };
    }

    try {
      const result = await tool.handler(parsed.data, ctx);
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (e) {
      const err = e as Error & { stderr?: string };
      return {
        isError: true,
        content: [
          {
            type: "text",
            text: err.stderr
              ? `${err.message}\n--- stderr ---\n${err.stderr}`
              : err.message,
          },
        ],
      };
    }
  });

  return { server, daemons };
}

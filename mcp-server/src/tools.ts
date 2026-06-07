import { runTraceDigest, type DigestRunOptions } from "./digest.js";
import { TraceCache, variantHash } from "./cache.js";
import type {
  BookmarkListArgsT,
  CallersArgsT,
  CalleesArgsT,
  ChannelsArgsT,
  CompareArgsT,
  CounterCatalogueArgsT,
  CounterSeriesArgsT,
  CpuThreadsArgsT,
  DigestArgsT,
  FrameArgsT,
  FramesArgsT,
  GpuFencesArgsT,
  GpuQueuesArgsT,
  LogMessagesArgsT,
  MemorySamplesArgsT,
  MemoryTagsArgsT,
  MemoryTrackersArgsT,
  OverviewArgsT,
  RegionListArgsT,
  StatusArgsT,
  TimelineArgsT,
  UnloadArgsT,
} from "./schemas.js";
import type {
  BookmarkListOutput,
  ButterflyOutput,
  ChannelsOutput,
  CompareOutput,
  CounterCatalogueOutput,
  CounterSeriesOutput,
  DigestOutput,
  FrameOutput,
  FrameEvent,
  FramesOutput,
  GpuFencesOutput,
  GpuQueuesOutput,
  LogMessagesOutput,
  MemorySamplesOutput,
  MemoryTagsOutput,
  MemoryTrackersOutput,
  OverviewOutput,
  RegionListOutput,
  StatusOutput,
  ThreadsOutput,
  TimelineOutput,
  UnloadOutput,
} from "./types.js";
import type { DaemonRegistry } from "./daemon.js";

export interface ToolContext {
  cache: TraceCache;
  runOptions: DigestRunOptions;
  daemons?: DaemonRegistry;
}

// Each public function corresponds to one MCP tool. Inputs are zod-validated
// by the server layer. Caching is keyed by (file × mtime × variant) so
// repeated identical calls skip the editor entirely.

export async function doDigest(args: DigestArgsT, ctx: ToolContext): Promise<DigestOutput> {
  const variant = variantHash("digest", { channel: args.channel, prefix: args.prefix, limit: args.limit });

  const cached = await ctx.cache.get(args.file, variant);
  const raw =
    (cached?.value as DigestOutput | undefined) ??
    (await (async () => {
      const out = (await runTraceDigest(
        { mode: "digest", file: args.file, channel: args.channel, prefix: args.prefix, limit: args.limit },
        ctx.runOptions,
      )) as DigestOutput;
      await ctx.cache.put(args.file, variant, out);
      return out;
    })());

  if (args.eventNames && args.eventNames.length > 0) {
    const keep = new Set(args.eventNames);
    return { ...raw, events: raw.events.filter((e) => keep.has(e.name)) };
  }
  return raw;
}

export async function doTimeline(args: TimelineArgsT, ctx: ToolContext): Promise<TimelineOutput> {
  const variant = variantHash("timeline", { channel: args.channel, event: args.event, frameRange: args.frameRange });
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as TimelineOutput;

  const out = (await runTraceDigest(
    { mode: "timeline", file: args.file, channel: args.channel, event: args.event, frameRange: args.frameRange },
    ctx.runOptions,
  )) as TimelineOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doFrames(args: FramesArgsT, ctx: ToolContext): Promise<FramesOutput> {
  const variant = variantHash("frames", { limit: args.limit, frameRange: args.frameRange });

  const cached = await ctx.cache.get(args.file, variant);
  const raw =
    (cached?.value as FramesOutput | undefined) ??
    (await (async () => {
      const out = (await runTraceDigest(
        { mode: "frames", file: args.file, limit: args.limit, frameRange: args.frameRange },
        ctx.runOptions,
      )) as FramesOutput;
      await ctx.cache.put(args.file, variant, out);
      return out;
    })());

  if (args.sortBy === "duration_ms") {
    return {
      ...raw,
      events: [...raw.events].sort(
        (a: FrameEvent, b: FrameEvent) => b.duration_ms - a.duration_ms,
      ),
    };
  }
  return raw;
}

export async function doCompare(args: CompareArgsT, ctx: ToolContext): Promise<CompareOutput> {
  const variant = variantHash("compare", {
    channel: args.channel,
    fileB: args.fileB,
    prefix: args.prefix,
    threshold: args.threshold,
    limit: args.limit,
  });

  const cached = await ctx.cache.get(args.fileA, variant);
  if (cached) return cached.value as CompareOutput;

  const out = (await runTraceDigest(
    {
      mode: "compare",
      file: args.fileA,
      file2: args.fileB,
      channel: args.channel,
      prefix: args.prefix,
      threshold: args.threshold,
      limit: args.limit,
    },
    ctx.runOptions,
  )) as CompareOutput;
  await ctx.cache.put(args.fileA, variant, out);
  return out;
}

// --- v0.2 ---

export async function doOverview(args: OverviewArgsT, ctx: ToolContext): Promise<OverviewOutput> {
  const variant = variantHash("overview", { prefix: args.prefix, limit: args.limit });
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as OverviewOutput;

  const out = (await runTraceDigest(
    { mode: "overview", file: args.file, prefix: args.prefix, limit: args.limit },
    ctx.runOptions,
  )) as OverviewOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doFrame(args: FrameArgsT, ctx: ToolContext): Promise<FrameOutput> {
  const variant = variantHash("frame", { frame: args.frame, limit: args.limit });
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as FrameOutput;

  const out = (await runTraceDigest(
    { mode: "frame", file: args.file, frame: args.frame, limit: args.limit },
    ctx.runOptions,
  )) as FrameOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doCallers(args: CallersArgsT, ctx: ToolContext): Promise<ButterflyOutput> {
  const variant = variantHash("callers", { channel: args.channel, event: args.event, limit: args.limit });
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as ButterflyOutput;

  const out = (await runTraceDigest(
    { mode: "callers", file: args.file, channel: args.channel, event: args.event, limit: args.limit },
    ctx.runOptions,
  )) as ButterflyOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doCallees(args: CalleesArgsT, ctx: ToolContext): Promise<ButterflyOutput> {
  const variant = variantHash("callees", { channel: args.channel, event: args.event, limit: args.limit });
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as ButterflyOutput;

  const out = (await runTraceDigest(
    { mode: "callees", file: args.file, channel: args.channel, event: args.event, limit: args.limit },
    ctx.runOptions,
  )) as ButterflyOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

// --- v0.4 channel-aware tools ---

export async function doChannels(args: ChannelsArgsT, ctx: ToolContext): Promise<ChannelsOutput> {
  const variant = variantHash("channels", {});
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as ChannelsOutput;

  const out = (await runTraceDigest(
    { mode: "channels", file: args.file },
    ctx.runOptions,
  )) as ChannelsOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doGpuQueues(args: GpuQueuesArgsT, ctx: ToolContext): Promise<GpuQueuesOutput> {
  const variant = variantHash("gpu_queues", {});
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as GpuQueuesOutput;

  const out = (await runTraceDigest(
    { mode: "gpu", file: args.file, view: "queues" },
    ctx.runOptions,
  )) as GpuQueuesOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doGpuFences(args: GpuFencesArgsT, ctx: ToolContext): Promise<GpuFencesOutput> {
  const variant = variantHash("gpu_fences", { queue: args.queue, limit: args.limit });
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as GpuFencesOutput;

  const out = (await runTraceDigest(
    { mode: "gpu", file: args.file, view: "fences", queue: args.queue, limit: args.limit },
    ctx.runOptions,
  )) as GpuFencesOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doCounterCatalogue(
  args: CounterCatalogueArgsT,
  ctx: ToolContext,
): Promise<CounterCatalogueOutput> {
  const variant = variantHash("counter_catalogue", {});
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as CounterCatalogueOutput;

  const out = (await runTraceDigest(
    { mode: "counters", file: args.file },
    ctx.runOptions,
  )) as CounterCatalogueOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doCounterSeries(
  args: CounterSeriesArgsT,
  ctx: ToolContext,
): Promise<CounterSeriesOutput> {
  const variant = variantHash("counter_series", { counter: args.counter, buckets: args.buckets });
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as CounterSeriesOutput;

  const out = (await runTraceDigest(
    { mode: "counters", file: args.file, counter: args.counter, buckets: args.buckets },
    ctx.runOptions,
  )) as CounterSeriesOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doBookmarkList(
  args: BookmarkListArgsT,
  ctx: ToolContext,
): Promise<BookmarkListOutput> {
  const variant = variantHash("bookmark_list", { limit: args.limit });
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as BookmarkListOutput;

  const out = (await runTraceDigest(
    { mode: "bookmarks", file: args.file, limit: args.limit },
    ctx.runOptions,
  )) as BookmarkListOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doRegionList(
  args: RegionListArgsT,
  ctx: ToolContext,
): Promise<RegionListOutput> {
  const variant = variantHash("region_list", { category: args.category, limit: args.limit });
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as RegionListOutput;

  const out = (await runTraceDigest(
    { mode: "regions", file: args.file, category: args.category, limit: args.limit },
    ctx.runOptions,
  )) as RegionListOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doLogMessages(
  args: LogMessagesArgsT,
  ctx: ToolContext,
): Promise<LogMessagesOutput> {
  const variant = variantHash("log_messages", {
    verbosity: args.verbosity,
    category: args.category,
    grep: args.grep,
    limit: args.limit,
  });
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as LogMessagesOutput;

  const out = (await runTraceDigest(
    {
      mode: "logs",
      file: args.file,
      verbosity: args.verbosity,
      category: args.category,
      grep: args.grep,
      limit: args.limit,
    },
    ctx.runOptions,
  )) as LogMessagesOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doMemoryTrackers(
  args: MemoryTrackersArgsT,
  ctx: ToolContext,
): Promise<MemoryTrackersOutput> {
  const variant = variantHash("memory_trackers", {});
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as MemoryTrackersOutput;

  const out = (await runTraceDigest(
    { mode: "memory", file: args.file, view: "trackers" },
    ctx.runOptions,
  )) as MemoryTrackersOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doMemoryTags(args: MemoryTagsArgsT, ctx: ToolContext): Promise<MemoryTagsOutput> {
  const variant = variantHash("memory_tags", { tracker: args.tracker, tag: args.tag });
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as MemoryTagsOutput;

  const out = (await runTraceDigest(
    { mode: "memory", file: args.file, view: "tags", tracker: args.tracker, tag: args.tag },
    ctx.runOptions,
  )) as MemoryTagsOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

export async function doMemorySamples(
  args: MemorySamplesArgsT,
  ctx: ToolContext,
): Promise<MemorySamplesOutput> {
  const variant = variantHash("memory_samples", {
    tracker: args.tracker,
    tag: args.tag,
    buckets: args.buckets,
  });
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as MemorySamplesOutput;

  const out = (await runTraceDigest(
    {
      mode: "memory",
      file: args.file,
      view: "samples",
      tracker: args.tracker,
      tag: args.tag,
      buckets: args.buckets,
    },
    ctx.runOptions,
  )) as MemorySamplesOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

// Was `doThreads` in v0.3 — renamed to match the new `trace_cpu_threads`
// tool name and to leave room for `trace_gpu_queues` etc. without ambiguity.
export async function doCpuThreads(args: CpuThreadsArgsT, ctx: ToolContext): Promise<ThreadsOutput> {
  const variant = variantHash("threads", { limit: args.limit });
  const cached = await ctx.cache.get(args.file, variant);
  if (cached) return cached.value as ThreadsOutput;

  const out = (await runTraceDigest(
    { mode: "threads", file: args.file, limit: args.limit },
    ctx.runOptions,
  )) as ThreadsOutput;
  await ctx.cache.put(args.file, variant, out);
  return out;
}

// --- Daemon control tools ---

export async function doUnload(args: UnloadArgsT, ctx: ToolContext): Promise<UnloadOutput> {
  if (!ctx.daemons) {
    return { file: args.file, was_loaded: false };
  }
  const was_loaded = await ctx.daemons.unload(args.file);
  return { file: args.file, was_loaded };
}

export async function doStatus(_args: StatusArgsT, ctx: ToolContext): Promise<StatusOutput> {
  if (!ctx.daemons) {
    return { system: { available_mb: -1, total_mb: -1 }, daemons: [] };
  }
  const sys = await ctx.daemons.systemMemory();
  const rows = await ctx.daemons.status();
  return {
    system: { available_mb: sys.availableMb, total_mb: sys.totalMb },
    daemons: rows,
  };
}

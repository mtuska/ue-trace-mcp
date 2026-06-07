import { z } from "zod";

// One zod schema per tool, each a top-level object so the emitted JSON
// Schema is `{type: "object", properties: ...}` — the shape every MCP client
// reliably accepts.

const file = z.string().describe("Absolute path to the .utrace file.");

// Channel param for tools whose verb is genuinely channel-agnostic.
// `trace_digest` and `trace_timeline` accept cpu+gpu+region;
// `trace_callers` and `trace_callees` accept cpu+gpu only (butterflies on
// regions have no meaningful semantics).
const channel = z
  .enum(["cpu", "gpu", "region"])
  .optional()
  .default("cpu")
  .describe(
    "Trace channel to operate on. cpu (default), gpu (queue timelines), or region (TRACE_BEGIN/END_REGION spans).",
  );

const butterflyChannel = z
  .enum(["cpu", "gpu"])
  .optional()
  .default("cpu")
  .describe(
    "Trace channel to butterfly into. cpu (default) or gpu. Regions have no caller/callee relationships.",
  );

// trace_compare ships cpu-only in v0.4.0; gpu/memory/etc. compares land in
// a later phase. The field exists in the schema so future widening is
// non-breaking; non-cpu values are rejected at parse time.
const compareChannel = z
  .enum(["cpu"])
  .optional()
  .default("cpu")
  .describe(
    "Trace channel to compare. Only 'cpu' is supported in v0.4.0; non-cpu channels error out.",
  );

const frameType = z
  .enum(["game"])
  .optional()
  .default("game")
  .describe(
    "Which frame stream to enumerate. v0.4.0 ships 'game' only; 'render' lands later.",
  );

export const DigestArgs = z.object({
  file,
  channel,
  prefix: z
    .string()
    .optional()
    .describe(
      "Restrict to timers whose display name starts with this prefix (e.g. 'Zombie_'). Empty/omitted = no filter.",
    ),
  eventNames: z
    .array(z.string())
    .optional()
    .describe(
      "Post-filter: keep only events whose exact name is in this list. Applied after prefix.",
    ),
  limit: z
    .number()
    .int()
    .positive()
    .optional()
    .describe("Top-N events by total_ms. Default 200."),
});
export type DigestArgsT = z.infer<typeof DigestArgs>;

export const TimelineArgs = z.object({
  file,
  channel,
  event: z
    .string()
    .describe("Exact timer name to enumerate (e.g. 'Zombie_StepLocomotion')."),
  frameRange: z
    .tuple([z.number().int().nonnegative(), z.number().int().positive()])
    .optional()
    .describe("Restrict to Game-thread frames [A, B)."),
});
export type TimelineArgsT = z.infer<typeof TimelineArgs>;

export const FramesArgs = z.object({
  file,
  frame_type: frameType,
  sortBy: z
    .enum(["idx", "duration_ms"])
    .optional()
    .describe("Default 'idx'. 'duration_ms' surfaces the slowest frames first."),
  limit: z.number().int().positive().optional().describe("Cap returned frames. Default 200."),
  frameRange: z
    .tuple([z.number().int().nonnegative(), z.number().int().positive()])
    .optional()
    .describe("Restrict to Game-thread frames [A, B)."),
});
export type FramesArgsT = z.infer<typeof FramesArgs>;

export const CompareArgs = z.object({
  fileA: z.string().describe("Absolute path to the baseline .utrace."),
  fileB: z.string().describe("Absolute path to the candidate .utrace."),
  channel: compareChannel,
  prefix: z.string().optional().describe("Restrict to timers whose name starts with this prefix."),
  threshold: z
    .number()
    .nonnegative()
    .optional()
    .describe(
      "Drop rows with |Δ P95| below this many ms. Events that exist on only one side are always kept.",
    ),
  limit: z.number().int().positive().optional().describe("Top-N ranked by |Δ P95|. Default 200."),
});
export type CompareArgsT = z.infer<typeof CompareArgs>;

// --- v0.2 ---

export const OverviewArgs = z.object({
  file,
  prefix: z.string().optional().describe("Restrict events list to timers with this prefix."),
  limit: z
    .number()
    .int()
    .positive()
    .optional()
    .describe("Cap on top-events list (also capped to 10 internally). Default 10."),
});
export type OverviewArgsT = z.infer<typeof OverviewArgs>;

export const FrameArgs = z.object({
  file,
  frame: z.number().int().nonnegative().describe("Game-thread frame index to drill into."),
  limit: z
    .number()
    .int()
    .positive()
    .optional()
    .describe("Top-N events within the frame, ordered by duration_ms. Default 200."),
});
export type FrameArgsT = z.infer<typeof FrameArgs>;

export const CallersArgs = z.object({
  file,
  channel: butterflyChannel,
  event: z.string().describe("Exact timer name to find callers for (e.g. 'Zombie_StepLocomotion')."),
  limit: z.number().int().positive().optional().describe("Top-N direct callers by inclusive time. Default 200."),
});
export type CallersArgsT = z.infer<typeof CallersArgs>;

export const CalleesArgs = z.object({
  file,
  channel: butterflyChannel,
  event: z.string().describe("Exact timer name to find callees for."),
  limit: z.number().int().positive().optional().describe("Top-N direct callees by inclusive time. Default 200."),
});
export type CalleesArgsT = z.infer<typeof CalleesArgs>;

// trace_cpu_threads — was trace_threads in v0.3. Renamed because threads are
// inherently a CPU concept (UE's IThreadProvider has no GPU equivalent).
export const CpuThreadsArgs = z.object({
  file,
  limit: z.number().int().positive().optional().describe("Top-N threads by total_depth0_ms. Default 200."),
});
export type CpuThreadsArgsT = z.infer<typeof CpuThreadsArgs>;

// --- Daemon control tools ---

export const UnloadArgs = z.object({
  file: z
    .string()
    .describe(
      "Absolute path to the .utrace whose daemon should be evicted. No-op if no daemon is currently loaded for it.",
    ),
});
export type UnloadArgsT = z.infer<typeof UnloadArgs>;

export const StatusArgs = z.object({}).describe("No arguments. Returns per-daemon stats and system memory.");
export type StatusArgsT = z.infer<typeof StatusArgs>;

// --- v0.4 channel-aware tools ---

export const ChannelsArgs = z.object({
  file,
});
export type ChannelsArgsT = z.infer<typeof ChannelsArgs>;

// trace_gpu_queues: list GPU queues + their timeline indices.
export const GpuQueuesArgs = z.object({
  file,
});
export type GpuQueuesArgsT = z.infer<typeof GpuQueuesArgs>;

// trace_gpu_fences: cross-queue signal/wait pairs ("stall" candidates).
export const GpuFencesArgs = z.object({
  file,
  queue: z
    .number()
    .int()
    .nonnegative()
    .optional()
    .describe("Restrict to fences signalled by this queue id. Default: all queues."),
  limit: z.number().int().positive().optional().describe("Top-N resolved fences. Default 200."),
});
export type GpuFencesArgsT = z.infer<typeof GpuFencesArgs>;

// trace_counter_catalogue: every counter + metadata. No filters; small.
export const CounterCatalogueArgs = z.object({
  file,
});
export type CounterCatalogueArgsT = z.infer<typeof CounterCatalogueArgs>;

// trace_counter_series: downsampled time series for a named counter.
export const CounterSeriesArgs = z.object({
  file,
  counter: z.string().describe("Exact (case-insensitive) counter name. Use trace_counter_catalogue to discover."),
  buckets: z
    .number()
    .int()
    .positive()
    .max(4096)
    .optional()
    .describe("Number of equal-width time buckets. Default 256."),
});
export type CounterSeriesArgsT = z.infer<typeof CounterSeriesArgs>;

// trace_bookmark_list: time-ordered TRACE_BOOKMARK points.
export const BookmarkListArgs = z.object({
  file,
  limit: z.number().int().positive().optional().describe("Cap returned bookmarks. Default 500."),
});
export type BookmarkListArgsT = z.infer<typeof BookmarkListArgs>;

// trace_region_list: TRACE_BEGIN/END_REGION spans (optionally one category).
export const RegionListArgs = z.object({
  file,
  category: z.string().optional().describe("Restrict to this exact category (case-insensitive)."),
  limit: z.number().int().positive().optional().describe("Cap returned regions. Default 500."),
});
export type RegionListArgsT = z.infer<typeof RegionListArgs>;

// trace_log_messages: windowed UE_LOG enumeration with filters.
export const LogMessagesArgs = z.object({
  file,
  verbosity: z
    .enum(["fatal", "error", "warning", "warn", "display", "log", "verbose", "all"])
    .optional()
    .describe(
      "Floor: messages strictly less severe than this are excluded. Default: verbose (=all).",
    ),
  category: z.string().optional().describe("Restrict to this exact category (case-insensitive)."),
  grep: z.string().optional().describe("Case-insensitive substring filter on the message body."),
  limit: z.number().int().positive().optional().describe("Cap returned messages. Default 500."),
});
export type LogMessagesArgsT = z.infer<typeof LogMessagesArgs>;

// trace_memory_* family (LLM tag tree). Three views, one umbrella mode on
// the binary side. The TS surface is split so each tool's params shape is
// tight — `tags` accepts optional tracker/tag focus; `samples` requires
// tag; `trackers` takes only the file.
export const MemoryTrackersArgs = z.object({
  file,
});
export type MemoryTrackersArgsT = z.infer<typeof MemoryTrackersArgs>;

export const MemoryTagsArgs = z.object({
  file,
  tracker: z.string().optional().describe("Restrict to tags exposed by this tracker (id or name)."),
  tag: z
    .string()
    .optional()
    .describe(
      "If supplied, emit only this tag and its ancestor chain (useful for explaining a single LLM bucket).",
    ),
});
export type MemoryTagsArgsT = z.infer<typeof MemoryTagsArgs>;

export const MemorySamplesArgs = z.object({
  file,
  tracker: z
    .string()
    .optional()
    .describe("Tracker id or name. Defaults to tracker 0 (UE's 'Default')."),
  tag: z.string().describe("Tag id or exact name. Use trace_memory_tags to discover."),
  buckets: z
    .number()
    .int()
    .positive()
    .max(4096)
    .optional()
    .describe("Number of equal-width time buckets. Default 256."),
});
export type MemorySamplesArgsT = z.infer<typeof MemorySamplesArgs>;

// trace_memalloc_* family — three umbrella views on IAllocationsProvider.

// trace_memalloc_timeline: aggregate stats over time (max total allocated
// memory, max live allocations, alloc/free events per timeline point).
export const MemallocTimelineArgs = z.object({
  file,
  buckets: z
    .number()
    .int()
    .positive()
    .max(4096)
    .optional()
    .describe("Number of equal-width time buckets. Default 256."),
});
export type MemallocTimelineArgsT = z.infer<typeof MemallocTimelineArgs>;

// trace_memalloc_heaps: heap-spec tree (FHeapSpec); tiny.
export const MemallocHeapsArgs = z.object({
  file,
});
export type MemallocHeapsArgsT = z.infer<typeof MemallocHeapsArgs>;

// trace_memalloc_query: rule-based allocation query (active-at-T, leaked,
// short-lived, …). Sync-polled inside the binary; LLM gets the
// completed/truncated/events shape back.
export const MemallocQueryArgs = z.object({
  file,
  rule: z
    .enum(["aAf", "afA", "Aaf", "aAfB"])
    .describe(
      "Allocation query rule. aAf=active at A; afA=allocated+freed before A; Aaf=allocated after A; aAfB=active at A, freed before B (long-living window).",
    ),
  timeA: z
    .number()
    .nonnegative()
    .optional()
    .describe("Time anchor A in seconds. Required for every rule; default 0."),
  timeB: z
    .number()
    .nonnegative()
    .optional()
    .describe("Time anchor B in seconds. Required for two-anchor rules (aAfB)."),
  limit: z
    .number()
    .int()
    .positive()
    .optional()
    .describe("Cap returned rows. Default 200. `truncated:true` flags overrun."),
  query_timeout_ms: z
    .number()
    .int()
    .positive()
    .optional()
    .describe(
      "How long to wait for the async query before cancelling. Default 60000 (60s). The C++ side polls every 25ms.",
    ),
});
export type MemallocQueryArgsT = z.infer<typeof MemallocQueryArgs>;

// trace_callstack — resolve one CallstackId to symbolicated frames. Every
// event-emitting tool's row carries a callstack_id; pass it here to see
// function/module/file/line.
export const CallstackArgs = z.object({
  file,
  id: z
    .number()
    .int()
    .positive()
    .describe("Callstack id from any v0.4 event row. Id 0 is reserved for the empty callstack."),
});
export type CallstackArgsT = z.infer<typeof CallstackArgs>;

// trace_modules — list discovered modules with load status + symbol stats.
// Pair with trace_callstack when a frame returns status:"not_loaded" or
// "version_mismatch" — checking modules tells you whether the parent
// module was loaded at all.
export const ModulesArgs = z.object({
  file,
});
export type ModulesArgsT = z.infer<typeof ModulesArgs>;

// trace_task_list — windowed enumeration of UE task-graph tasks. State
// filter mirrors UE's ETaskEnumerationOption. Production traces can have
// 10⁶–10⁷ tasks so windowing via frameRange + cap via limit is the norm.
export const TaskListArgs = z.object({
  file,
  state: z
    .enum([
      "Alive",
      "Launched",
      "Active",
      "WaitingForPrerequisites",
      "Queued",
      "Executing",
      "WaitingForNested",
      "Completed",
    ])
    .optional()
    .describe("Filter tasks by state. Default 'Alive' (= every task seen in the window)."),
  frameRange: z
    .tuple([z.number().int().nonnegative(), z.number().int().positive()])
    .optional()
    .describe("Restrict to Game-thread frames [A, B). Default: full trace duration."),
  limit: z.number().int().positive().optional().describe("Cap returned tasks. Default 500."),
});
export type TaskListArgsT = z.infer<typeof TaskListArgs>;

// trace_task_drill — full info on one task: every timestamp, every
// thread id, plus the four relation arrays (prerequisites, subsequents,
// parent_tasks, nested_tasks). Use after task_list to investigate a
// specific task's lifecycle and dependencies.
export const TaskDrillArgs = z.object({
  file,
  task_id: z.number().int().nonnegative().describe("TaskTrace::FId from a task_list row."),
});
export type TaskDrillArgsT = z.infer<typeof TaskDrillArgs>;

// trace_query — intent-dispatched escape hatch. `intent` is z.string() (not
// an enum) so the C++ side can add new intents without forcing a schema
// bump. Use intent="list" to discover the registry.
export const QueryArgs = z.object({
  file,
  intent: z
    .string()
    .describe(
      "Intent name. Use intent='list' to discover available intents and their params shapes.",
    ),
  params: z
    .record(z.unknown())
    .optional()
    .describe(
      "Opaque parameter object forwarded to the intent. Shape is intent-specific; the C++ side parses it as JSON.",
    ),
});
export type QueryArgsT = z.infer<typeof QueryArgs>;

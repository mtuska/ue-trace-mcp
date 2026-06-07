// Shapes mirroring what the TraceDigest UE binary writes. Keep these in
// lock-step with ue-plugin/TraceDigest/Source/.../Modes/*.cpp.
//
// Every mode's row payload is exposed under the `events` key. Per-row schemas
// differ — that's why each mode has its own *Output type.

export interface DigestEvent {
  name: string;
  count: number;
  total_ms: number;
  avg_ms: number;
  p50_ms: number;
  p95_ms: number;
  p99_ms: number;
  max_ms: number;
}

export interface DigestOutput {
  file: string;
  mode: "digest";
  duration_ms: number;
  frame_count: number;
  events: DigestEvent[];
}

export interface TimelineEvent {
  frame_idx: number;
  start_ms: number;
  duration_ms: number;
}

export interface TimelineOutput {
  file: string;
  mode: "timeline";
  event: string;
  duration_ms: number;
  frame_count: number;
  events: TimelineEvent[];
}

export interface FrameEvent {
  idx: number;
  start_ms: number;
  end_ms: number;
  duration_ms: number;
}

export interface FramesOutput {
  file: string;
  mode: "frames";
  duration_ms: number;
  frame_count: number;
  events: FrameEvent[];
}

export interface CompareSide {
  count: number;
  total_ms: number;
  avg_ms: number;
  p50_ms: number;
  p95_ms: number;
  p99_ms: number;
  max_ms: number;
}

/** cpu/gpu/region — event-aggregate diff row. */
export interface CompareEvent {
  name: string;
  only_in_a: boolean;
  only_in_b: boolean;
  delta_p95_ms: number;
  delta_total_ms: number;
  a: CompareSide;
  b: CompareSide;
}

/** memory — per-tag peak-bytes diff row. */
export interface CompareMemoryRow {
  name: string;
  a_peak_bytes: number;
  b_peak_bytes: number;
  delta_peak_bytes: number;
  only_in_a: boolean;
  only_in_b: boolean;
}

/** counter — per-counter mean+max diff row. */
export interface CompareCounterRow {
  name: string;
  a_mean: number;
  b_mean: number;
  delta_mean: number;
  a_max: number;
  b_max: number;
  delta_max: number;
  only_in_a: boolean;
  only_in_b: boolean;
}

/** memalloc — fixed 4-metric diff (per session, not per-name). */
export interface CompareMemallocRow {
  /** "peak_total_allocated_bytes" | "max_live_allocations" | "alloc_events_total" | "free_events_total" */
  metric: string;
  a_value: number;
  b_value: number;
  delta: number;
}

export interface CompareOutput {
  file: string;
  file2: string;
  mode: "compare";
  channel: "cpu" | "gpu" | "region" | "memory" | "memalloc" | "counter";
  duration_a_ms: number;
  duration_b_ms: number;
  /** Row shape is channel-specific — see the per-channel types above. */
  events: Array<CompareEvent | CompareMemoryRow | CompareCounterRow | CompareMemallocRow>;
}

// --- v0.2 ---

export interface FrameStats {
  min_ms: number;
  avg_ms: number;
  p50_ms: number;
  p95_ms: number;
  p99_ms: number;
  max_ms: number;
}

// v0.4 channel-aware shape. Top-level has only session-wide fields; every
// category gets its own sub-object. CPU keeps the v0.3 keys verbatim so
// consumers only need to prefix their paths with `cpu.`.
export interface OverviewCpu {
  frame_count: number;
  frame_stats: FrameStats;
  slowest_frames: Array<{ idx: number; duration_ms: number }>;
  events: DigestEvent[];
}

export interface OverviewMemory {
  tracker_count: number;
  tag_set_count: number;
  tag_count: number;
}

export interface OverviewMemalloc {
  timeline_points: number;
  peak_bytes: number;
  alloc_event_total: number;
  free_event_total: number;
}

export interface OverviewOutput {
  file: string;
  mode: "overview";
  duration_ms: number;
  /** Channel names present in the trace (regardless of enabled state). */
  channels: string[];
  cpu?: OverviewCpu;
  memory?: OverviewMemory;
  memalloc?: OverviewMemalloc;
  // gpu, counters, logs, bookmarks, regions sub-blocks land in
  // subsequent passes as those provider summaries come online.
}

export interface FrameDrillEvent {
  name: string;
  thread: string;
  depth: number;
  start_ms: number;
  duration_ms: number;
}

export interface FrameOutput {
  file: string;
  mode: "frame";
  frame_idx: number;
  start_ms: number;
  end_ms: number;
  duration_ms: number;
  events: FrameDrillEvent[];
}

export interface ButterflyEvent {
  name: string;
  count: number;
  inclusive_ms: number;
  exclusive_ms: number;
}

export interface ButterflyOutput {
  file: string;
  mode: "callers" | "callees";
  event: string;
  target_count: number;
  target_inclusive_ms: number;
  target_exclusive_ms: number;
  events: ButterflyEvent[];
}

export interface ThreadTopTimer {
  name: string;
  count: number;
  inclusive_ms: number;
}

export interface ThreadEvent {
  thread_id: number;
  thread_name: string;
  event_count: number;
  total_depth0_ms: number;
  top_timers: ThreadTopTimer[];
}

export interface ThreadsOutput {
  file: string;
  mode: "threads";
  duration_ms: number;
  events: ThreadEvent[];
}

// --- v0.4 channel-aware tools ---

export interface ChannelEntry {
  id: number;
  name: string;
  enabled: boolean;
  read_only: boolean;
}

export interface ChannelsOutput {
  file: string;
  mode: "channels";
  duration_ms: number;
  channels: ChannelEntry[];
}

// GPU queues — mirror of FGpuQueueInfo.
export interface GpuQueue {
  id: number;
  gpu: number;
  index: number;
  type: number;
  name: string;
  display_name: string;
  timeline_index: number;
  work_timeline_index: number;
}

export interface GpuQueuesOutput {
  file: string;
  mode: "gpu";
  view: "queues";
  has_gpu: boolean;
  duration_ms: number;
  queues: GpuQueue[];
}

// GPU resolved fence pairs — cross-queue stall analysis.
export interface GpuFenceEvent {
  signal_queue_id: number;
  signal_time_ms: number;
  signal_value: number;
  wait_queue_id: number;
  wait_time_ms: number;
  wait_value: number;
  stall_ms: number;
}

export interface GpuFencesOutput {
  file: string;
  mode: "gpu";
  view: "fences";
  has_gpu: boolean;
  duration_ms: number;
  queue_filter: number;
  total_in_window: number;
  truncated: boolean;
  events: GpuFenceEvent[];
}

// Counter catalogue — metadata only.
export interface CounterMeta {
  id: number;
  name: string;
  group: string;
  description: string;
  is_float: boolean;
  reset_every_frame: boolean;
  display_hint: "memory" | "none";
}

export interface CounterCatalogueOutput {
  file: string;
  mode: "counters";
  duration_ms: number;
  counter_count: number;
  counters: CounterMeta[];
}

// Counter series — time-bucketed values.
export interface CounterSeriesBucket {
  t_ms: number;
  min: number;
  max: number;
  avg: number;
  count: number;
}

export interface CounterSeriesOutput {
  file: string;
  mode: "counters";
  duration_ms: number;
  counter_count: number;
  counter: string;
  found: boolean;
  counter_id?: number;
  is_float?: boolean;
  buckets?: number;
  total_samples?: number;
  series: CounterSeriesBucket[];
}

export interface BookmarkEvent {
  time_ms: number;
  frame_idx: number;
  text: string;
  callstack_id: number;
}

export interface BookmarkListOutput {
  file: string;
  mode: "bookmarks";
  duration_ms: number;
  total_in_window: number;
  truncated: boolean;
  events: BookmarkEvent[];
}

export interface RegionEvent {
  name: string;
  category: string;
  begin_ms: number;
  end_ms: number;
  duration_ms: number;
  depth: number;
  id: number;
  /** True when the region never received a TRACE_END_REGION; `end_ms` is clamped to the trace end. */
  open: boolean;
}

export interface RegionListOutput {
  file: string;
  mode: "regions";
  duration_ms: number;
  total_in_window: number;
  category_filter: string;
  categories: string[];
  truncated: boolean;
  events: RegionEvent[];
}

export interface LogEvent {
  idx: number;
  time_ms: number;
  category: string;
  verbosity: string;
  message: string;
  file: string;
  line: number;
}

export interface LogMessagesOutput {
  file: string;
  mode: "logs";
  duration_ms: number;
  total_in_window: number;
  verbosity_floor: string;
  category_filter: string;
  grep: string;
  truncated: boolean;
  events: LogEvent[];
}

// trace_memory_* family — three umbrella views on IMemoryProvider.

export interface MemoryTracker {
  id: number;
  name: string;
}

export interface MemoryTagSet {
  id: number;
  name: string;
}

export interface MemoryTrackersOutput {
  file: string;
  mode: "memory";
  view: "trackers";
  duration_ms: number;
  has_memory: boolean;
  trackers: MemoryTracker[];
  tag_sets: MemoryTagSet[];
}

export interface MemoryTagRow {
  id: number;
  parent_id: number;
  tag_set_id: number;
  trackers_bitmask: number;
  name: string;
}

export interface MemoryTagsOutput {
  file: string;
  mode: "memory";
  view: "tags";
  duration_ms: number;
  has_memory: boolean;
  tracker_filter: number;
  tag_filter: number;
  tags: MemoryTagRow[];
}

export interface MemorySampleBucket {
  t_ms: number;
  min: number;
  max: number;
  avg: number;
  count: number;
}

export interface MemorySamplesOutput {
  file: string;
  mode: "memory";
  view: "samples";
  duration_ms: number;
  has_memory: boolean;
  tracker_id: number;
  tag_id: number;
  tag_arg: string;
  buckets: number;
  found: boolean;
  total_samples?: number;
  series: MemorySampleBucket[];
}

// trace_memalloc_* family — three umbrella views on IAllocationsProvider.

export interface AllocBucket {
  t_ms: number;
  min: number;
  max: number;
  avg: number;
  count: number;
}

export interface MemallocTimelineOutput {
  file: string;
  mode: "allocations";
  view: "timeline";
  duration_ms: number;
  has_memalloc: boolean;
  timeline_points?: number;
  range_start?: number;
  range_end?: number;
  max_total_allocated_memory?: AllocBucket[];
  max_live_allocations?: AllocBucket[];
  alloc_events_per_point?: AllocBucket[];
  free_events_per_point?: AllocBucket[];
}

export interface MemallocHeap {
  id: number;
  parent_id: number;
  flags: number;
  name: string;
  is_root: boolean;
}

export interface MemallocHeapsOutput {
  file: string;
  mode: "allocations";
  view: "heaps";
  duration_ms: number;
  has_memalloc: boolean;
  heaps: MemallocHeap[];
  heap_count: number;
}

export interface AllocationRow {
  address: number;
  size: number;
  alignment: number;
  start_ms: number;
  end_ms: number;
  alloc_thread: number;
  free_thread: number;
  alloc_callstack_id: number;
  free_callstack_id: number;
  tag: number;
  root_heap: number;
  is_heap: boolean;
  is_swap: boolean;
}

export interface MemallocQueryOutput {
  file: string;
  mode: "allocations";
  view: "query";
  duration_ms: number;
  has_memalloc: boolean;
  rule: string;
  time_a: number;
  time_b: number;
  query_timeout_ms: number;
  completed: boolean;
  total_available: number;
  truncated: boolean;
  events: AllocationRow[];
  /** Only present on a parse failure. */
  ok?: boolean;
  error?: string;
}

// trace_callstack — symbolicated frames for one CallstackId.
export interface CallstackFrame {
  depth: number;
  addr: number;
  /** "pending" | "ok" | "not_loaded" | "version_mismatch" | "not_found" | "no_symbol" | "unknown". */
  status: string;
  symbol: string;
  module: string;
  file: string;
  line: number;
}

export interface CallstackOutput {
  file: string;
  mode: "callstack";
  callstack_id: number;
  found: boolean;
  frame_count: number;
  frames: CallstackFrame[];
}

// trace_modules — discovered modules + per-module symbol stats.
export interface ModuleSymbolStats {
  discovered: number;
  cached: number;
  resolved: number;
  failed: number;
  available: number;
}

export interface ModuleRow {
  name: string;
  full_name: string;
  base: number;
  size: number;
  unloaded: boolean;
  status: string;
  status_message: string;
  symbol_stats: ModuleSymbolStats;
}

export interface ModulesOutput {
  file: string;
  mode: "modules";
  module_count: number;
  finished_resolving?: boolean;
  totals?: {
    modules_discovered: number;
    modules_loaded: number;
    modules_failed: number;
    symbols_discovered: number;
    symbols_cached: number;
    symbols_resolved: number;
    symbols_failed: number;
  };
  modules: ModuleRow[];
}

// trace_task_list — windowed task-graph enumeration.
export interface TaskListRow {
  id: number;
  debug_name: string;
  tracked: boolean;
  thread_to_execute: number;
  created_ms: number;
  started_ms: number;
  finished_ms: number;
  completed_ms: number;
  prerequisite_count: number;
  subsequent_count: number;
}

export interface TaskListOutput {
  file: string;
  mode: "task_list";
  state: string;
  window_start_ms: number;
  window_end_ms: number;
  total_tasks_in_trace?: number;
  total_in_window: number;
  truncated: boolean;
  events: TaskListRow[];
}

// trace_task_drill — full FTaskInfo for one task, plus its relation arrays.
export interface TaskRelation {
  id: number;
  time_ms: number;
  thread_id: number;
}

export interface TaskDrillOutput {
  file: string;
  mode: "task_drill";
  task_id: number;
  found: boolean;
  debug_name?: string;
  tracked?: boolean;
  thread_to_execute?: number;
  task_size?: number;
  timestamps?: {
    created_ms: number;
    launched_ms: number;
    scheduled_ms: number;
    started_ms: number;
    finished_ms: number;
    completed_ms: number;
    destroyed_ms: number;
  };
  threads?: {
    created: number;
    launched: number;
    scheduled: number;
    started: number;
    completed: number;
    destroyed: number;
  };
  prerequisites?: TaskRelation[];
  subsequents?: TaskRelation[];
  parent_tasks?: TaskRelation[];
  nested_tasks?: TaskRelation[];
}

// trace_asset_* family — pre-aggregated load-time tables.
export interface AssetPackageRow {
  id: number;
  name: string;
  total_serialized_size: number;
  serialized_header_size: number;
  serialized_exports_count: number;
  serialized_exports_size: number;
  main_thread_ms: number;
  async_loading_ms: number;
  summary: {
    total_header_size: number;
    import_count: number;
    export_count: number;
    priority: number;
  };
  imported_packages_count: number;
  request_id: number;
}

export interface AssetRequestRow {
  id: number;
  name: string;
  start_ms: number;
  duration_ms: number;
  package_count: number;
}

export interface AssetExportRow {
  id: number;
  class: string;
  package_name: string;
  package_id: number;
  serialized_size: number;
  main_thread_ms: number;
  async_loading_ms: number;
  /** "create" | "serialize" | "postload" | "none" */
  event_type: string;
}

export interface AssetOutputBase<View extends string, Row> {
  file: string;
  mode: "asset";
  view: View;
  window_start_ms: number;
  window_end_ms: number;
  has_load_time_data: boolean;
  total_in_window: number;
  truncated: boolean;
  events: Row[];
}

export type AssetPackagesOutput = AssetOutputBase<"packages", AssetPackageRow>;
export type AssetRequestsOutput = AssetOutputBase<"requests", AssetRequestRow>;
export type AssetExportsOutput  = AssetOutputBase<"exports",  AssetExportRow>;

// trace_query — open-ended envelope. The `events` array shape varies per
// intent; intent-specific top-level fields land alongside it. We type the
// rows as `Record<string, unknown>` because of that variability.
export interface QueryOutput {
  file: string;
  mode: "query";
  intent: string;
  ok: boolean;
  /** Set when the intent failed parse or wasn't recognised. */
  error?: string;
  /** Set on unknown-intent errors. */
  supported?: string[];
  /** Set when intent === "list". */
  intents?: Array<{ name: string; description: string; params: string }>;
  /** Set by every non-list intent that returns rows. */
  events?: Array<Record<string, unknown>>;
  /** Any other intent-specific top-level fields. */
  [k: string]: unknown;
}

// --- Daemon control responses ---

export interface UnloadOutput {
  file: string;
  was_loaded: boolean;
}

export interface StatusDaemonRow {
  file: string;
  pid: number;
  socket: string;
  uptime_ms: number;
  idle_ms: number;
  estimate_mb: number;
  rss_mb: number;
  requests_served: number;
}

/** Daemon that's been spawned but hasn't finished parsing its trace yet. */
export interface StatusLoadingRow {
  file: string;
  pid: number | null;
  /** Wall-clock ms since the MCP server spawned the binary. */
  wall_elapsed_ms: number;
  /**
   * Most recent `elapsed_ms` value reported by the binary itself via its
   * `LOADING_PROGRESS:` heartbeat (emitted every ~500ms during LoadEx).
   * `null` until the first line arrives; a value that stops advancing
   * while `wall_elapsed_ms` keeps climbing is a stuck load.
   */
  last_progress_elapsed_ms: number | null;
}

export interface StatusOutput {
  system: { available_mb: number; total_mb: number };
  daemons: StatusDaemonRow[];
  loading: StatusLoadingRow[];
}

export type AnyDigestOutput =
  | DigestOutput
  | TimelineOutput
  | FramesOutput
  | CompareOutput
  | OverviewOutput
  | FrameOutput
  | ButterflyOutput
  | ThreadsOutput
  | ChannelsOutput
  | GpuQueuesOutput
  | GpuFencesOutput
  | CounterCatalogueOutput
  | CounterSeriesOutput
  | BookmarkListOutput
  | RegionListOutput
  | LogMessagesOutput
  | MemoryTrackersOutput
  | MemoryTagsOutput
  | MemorySamplesOutput
  | MemallocTimelineOutput
  | MemallocHeapsOutput
  | MemallocQueryOutput
  | QueryOutput
  | CallstackOutput
  | ModulesOutput
  | TaskListOutput
  | TaskDrillOutput
  | AssetPackagesOutput
  | AssetRequestsOutput
  | AssetExportsOutput;

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

export interface CompareEvent {
  name: string;
  only_in_a: boolean;
  only_in_b: boolean;
  delta_p95_ms: number;
  delta_total_ms: number;
  a: CompareSide;
  b: CompareSide;
}

export interface CompareOutput {
  file: string;
  file2: string;
  mode: "compare";
  duration_a_ms: number;
  duration_b_ms: number;
  events: CompareEvent[];
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

export interface OverviewOutput {
  file: string;
  mode: "overview";
  duration_ms: number;
  /** Channel names present in the trace (regardless of enabled state). */
  channels: string[];
  cpu?: OverviewCpu;
  // gpu, memory, memalloc, counters, logs, bookmarks, regions land in
  // subsequent passes as those provider blocks come online.
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

export interface StatusOutput {
  system: { available_mb: number; total_mb: number };
  daemons: StatusDaemonRow[];
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
  | MemorySamplesOutput;

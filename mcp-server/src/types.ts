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

export interface OverviewOutput {
  file: string;
  mode: "overview";
  duration_ms: number;
  frame_count: number;
  frame_stats: FrameStats;
  slowest_frames: Array<{ idx: number; duration_ms: number }>;
  events: DigestEvent[];
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
  | ChannelsOutput;

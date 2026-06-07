import { z } from "zod";

// One zod schema per tool, each a top-level object so the emitted JSON
// Schema is `{type: "object", properties: ...}` — the shape every MCP client
// reliably accepts.

const file = z.string().describe("Absolute path to the .utrace file.");

export const DigestArgs = z.object({
  file,
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
  event: z.string().describe("Exact timer name to find callers for (e.g. 'Zombie_StepLocomotion')."),
  limit: z.number().int().positive().optional().describe("Top-N direct callers by inclusive time. Default 200."),
});
export type CallersArgsT = z.infer<typeof CallersArgs>;

export const CalleesArgs = z.object({
  file,
  event: z.string().describe("Exact timer name to find callees for."),
  limit: z.number().int().positive().optional().describe("Top-N direct callees by inclusive time. Default 200."),
});
export type CalleesArgsT = z.infer<typeof CalleesArgs>;

export const ThreadsArgs = z.object({
  file,
  limit: z.number().int().positive().optional().describe("Top-N threads by total_depth0_ms. Default 200."),
});
export type ThreadsArgsT = z.infer<typeof ThreadsArgs>;

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

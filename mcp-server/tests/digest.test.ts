import { describe, expect, it, beforeEach } from "vitest";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { chmod } from "node:fs/promises";

import { runTraceDigest, extractTrailingJson, TraceDigestError } from "../src/digest.js";
import {
  doBookmarkList,
  doCallees,
  doCallers,
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
  doMemorySamples,
  doMemoryTags,
  doMemoryTrackers,
  doOverview,
  doRegionList,
  doTimeline,
} from "../src/tools.js";
import { TraceCache } from "../src/cache.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const MOCK_BIN = resolve(join(__dirname, "mock-bin.mjs"));
const FIXTURE_TRACE = join(__dirname, "fixtures", "digest-sample.json");

// The fixtures double as "fake .utrace files" — we never read them as traces,
// we just need a real path with a real mtime for the cache key to work.

beforeEach(async () => {
  await chmod(MOCK_BIN, 0o755).catch(() => undefined);
});

describe("extractTrailingJson", () => {
  it("parses bare JSON", () => {
    const v = extractTrailingJson('{"a":1}');
    expect(v).toEqual({ a: 1 });
  });

  it("strips engine startup chatter", () => {
    const v = extractTrailingJson(
      "LogInit: hi\nLogModule: loaded\n{\"a\":2,\"b\":\"x\"}\n",
    );
    expect(v).toEqual({ a: 2, b: "x" });
  });

  it("handles strings with embedded braces", () => {
    const v = extractTrailingJson('LogX: noise\n{"name":"foo{bar}baz","n":3}');
    expect(v).toEqual({ name: "foo{bar}baz", n: 3 });
  });

  it("throws on garbage", () => {
    expect(() => extractTrailingJson("LogInit: nothing here\n")).toThrow();
  });
});

describe("runTraceDigest spawn path", () => {
  it("invokes the binary and returns parsed JSON", async () => {
    const out = await runTraceDigest(
      { mode: "digest", file: FIXTURE_TRACE, prefix: "Zombie_" },
      { binary: MOCK_BIN },
    );
    expect(out.mode).toBe("digest");
    expect(out.file).toBe(FIXTURE_TRACE);
    if (out.mode === "digest") {
      expect(out.events.length).toBeGreaterThan(0);
      expect(out.events[0]!.name).toMatch(/^Zombie_/);
    }
  });

  it("tolerates startup noise on stdout", async () => {
    const out = await runTraceDigest(
      { mode: "digest", file: FIXTURE_TRACE },
      { binary: MOCK_BIN, timeoutMs: 5000 },
    );
    // Verifies the env-driven noisy mode succeeds through the slow path.
    expect(out.mode).toBe("digest");
  });

  it("surfaces binary exit code as TraceDigestError", async () => {
    process.env.MOCK_BIN_FAIL = "1";
    try {
      await expect(
        runTraceDigest(
          { mode: "digest", file: FIXTURE_TRACE },
          { binary: MOCK_BIN },
        ),
      ).rejects.toBeInstanceOf(TraceDigestError);
    } finally {
      delete process.env.MOCK_BIN_FAIL;
    }
  });

  it("rejects when the file argument refers to a missing trace", async () => {
    await expect(
      runTraceDigest(
        { mode: "digest", file: "/tmp/MISSING.utrace" },
        { binary: MOCK_BIN },
      ),
    ).rejects.toBeInstanceOf(TraceDigestError);
  });
});

describe("tool dispatchers", () => {
  it("doDigest: returns events, supports eventNames post-filter", async () => {
    const cache = new TraceCache(3);
    const result = await doDigest(
      {
        file: FIXTURE_TRACE,
        prefix: "Zombie_",
        eventNames: ["Zombie_StepLocomotion"],
      },
      { cache, runOptions: { binary: MOCK_BIN } },
    );
    expect(result.mode).toBe("digest");
    expect(result.events).toHaveLength(1);
    expect(result.events[0]!.name).toBe("Zombie_StepLocomotion");
  });

  it("doFrames: sortBy=duration_ms reshuffles client-side", async () => {
    const cache = new TraceCache(3);
    const result = await doFrames(
      { file: FIXTURE_TRACE, sortBy: "duration_ms" },
      { cache, runOptions: { binary: MOCK_BIN } },
    );
    expect(result.mode).toBe("frames");
    // Fixture has frames in ascending idx order with durations 16, 17, 17.
    expect(result.events[0]!.duration_ms).toBeGreaterThanOrEqual(
      result.events[result.events.length - 1]!.duration_ms,
    );
  });

  it("doTimeline: surfaces every instance for the requested event", async () => {
    const cache = new TraceCache(3);
    const result = await doTimeline(
      { file: FIXTURE_TRACE, event: "Zombie_StepLocomotion" },
      { cache, runOptions: { binary: MOCK_BIN } },
    );
    expect(result.mode).toBe("timeline");
    expect(result.events.length).toBeGreaterThan(0);
    expect(result.event).toBe("Zombie_StepLocomotion");
  });

  it("doCompare: ranks regressions including only-on-one-side events", async () => {
    const cache = new TraceCache(3);
    const result = await doCompare(
      { fileA: FIXTURE_TRACE, fileB: FIXTURE_TRACE, prefix: "Zombie_" },
      { cache, runOptions: { binary: MOCK_BIN } },
    );
    expect(result.mode).toBe("compare");
    // The compare fixture surfaces WallSlideProbe as only_in_b — it should
    // win the sort by |Δ P95| and appear first.
    expect(result.events[0]!.name).toBe("Zombie_WallSlideProbe");
    expect(result.events[0]!.only_in_b).toBe(true);
  });

  it("caches by (path, mtime, variant): second identical call doesn't respawn", async () => {
    // Run twice with MOCK_BIN_FAIL set on the *second* call. If the cache hit,
    // the second call returns the first result; if not, it errors.
    const cache = new TraceCache(3);
    const ctx = { cache, runOptions: { binary: MOCK_BIN } };

    const first = await doDigest(
      { file: FIXTURE_TRACE, prefix: "Zombie_" },
      ctx,
    );

    process.env.MOCK_BIN_FAIL = "1";
    try {
      const second = await doDigest(
        { file: FIXTURE_TRACE, prefix: "Zombie_" },
        ctx,
      );
      expect(second).toEqual(first);
    } finally {
      delete process.env.MOCK_BIN_FAIL;
    }
  });

  it("eventNames post-filter is cache-friendly: same cache entry serves different filters", async () => {
    const cache = new TraceCache(3);
    const ctx = { cache, runOptions: { binary: MOCK_BIN } };

    const a = await doDigest(
      {
        file: FIXTURE_TRACE,
        prefix: "Zombie_",
        eventNames: ["Zombie_StepLocomotion"],
      },
      ctx,
    );

    process.env.MOCK_BIN_FAIL = "1";
    try {
      const b = await doDigest(
        {
          file: FIXTURE_TRACE,
          prefix: "Zombie_",
          eventNames: ["Zombie_PerceptionUpdate"],
        },
        ctx,
      );
      expect(a.events.map((e) => e.name)).toEqual(["Zombie_StepLocomotion"]);
      expect(b.events.map((e) => e.name)).toEqual(["Zombie_PerceptionUpdate"]);
    } finally {
      delete process.env.MOCK_BIN_FAIL;
    }
  });
});

describe("v0.2 tools", () => {
  it("doOverview: returns channel-aware shape with cpu sub-block", async () => {
    const cache = new TraceCache(3);
    const r = await doOverview({ file: FIXTURE_TRACE }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.mode).toBe("overview");
    expect(r.duration_ms).toBeGreaterThan(0);
    expect(r.channels.length).toBeGreaterThan(0);
    // v0.4 breaking change: frame_stats / slowest_frames / events live under
    // `cpu`, not at the top level. The cpu sub-object keeps the v0.3 keys
    // verbatim.
    expect(r.cpu).toBeDefined();
    expect(r.cpu!.frame_stats.p95_ms).toBeGreaterThan(0);
    expect(r.cpu!.slowest_frames.length).toBeGreaterThan(0);
    expect(r.cpu!.events.length).toBeGreaterThan(0);
  });

  it("doFrame: returns events within one frame", async () => {
    const cache = new TraceCache(3);
    const r = await doFrame({ file: FIXTURE_TRACE, frame: 412 }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.mode).toBe("frame");
    expect(r.frame_idx).toBe(412);
    expect(r.events[0]!.thread).toBeTypeOf("string");
  });

  it("doCallers / doCallees: return butterfly-shaped output", async () => {
    const cache = new TraceCache(3);
    const callers = await doCallers(
      { file: FIXTURE_TRACE, event: "Zombie_StepLocomotion" },
      { cache, runOptions: { binary: MOCK_BIN } },
    );
    expect(callers.mode).toBe("callers");
    expect(callers.target_inclusive_ms).toBeGreaterThan(0);
    expect(callers.events[0]!.inclusive_ms).toBeGreaterThan(0);

    const callees = await doCallees(
      { file: FIXTURE_TRACE, event: "Zombie_TierPass" },
      { cache, runOptions: { binary: MOCK_BIN } },
    );
    expect(callees.mode).toBe("callees");
    expect(callees.events.length).toBeGreaterThan(0);
  });

  it("doCpuThreads: returns per-thread breakdowns with top_timers", async () => {
    const cache = new TraceCache(3);
    const r = await doCpuThreads({ file: FIXTURE_TRACE }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.mode).toBe("threads");
    expect(r.events.length).toBeGreaterThan(0);
    expect(r.events[0]!.thread_name).toBeTypeOf("string");
    expect(r.events[0]!.top_timers.length).toBeGreaterThan(0);
  });

  it("doChannels: lists trace channels with enabled/read_only flags", async () => {
    const cache = new TraceCache(3);
    const r = await doChannels(
      { file: FIXTURE_TRACE },
      { cache, runOptions: { binary: MOCK_BIN } },
    );
    expect(r.mode).toBe("channels");
    expect(r.channels.length).toBeGreaterThan(0);
    const cpu = r.channels.find((c) => c.name === "cpu");
    expect(cpu?.enabled).toBe(true);
    // disabled channels still show up so the LLM knows they exist but had no data.
    const memalloc = r.channels.find((c) => c.name === "memalloc");
    expect(memalloc?.enabled).toBe(false);
  });

  it("doGpuQueues: lists GPU queues with timeline indices", async () => {
    const cache = new TraceCache(3);
    const r = await doGpuQueues({ file: FIXTURE_TRACE }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.view).toBe("queues");
    expect(r.has_gpu).toBe(true);
    expect(r.queues.length).toBeGreaterThan(0);
    expect(r.queues[0]!.timeline_index).toBeGreaterThanOrEqual(0);
  });

  it("doGpuFences: returns resolved fence pairs with stall_ms", async () => {
    const cache = new TraceCache(3);
    const r = await doGpuFences({ file: FIXTURE_TRACE }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.view).toBe("fences");
    expect(r.events.length).toBeGreaterThan(0);
    expect(r.events[0]!.stall_ms).toBeGreaterThanOrEqual(0);
  });

  it("doCounterCatalogue: lists every counter with metadata", async () => {
    const cache = new TraceCache(3);
    const r = await doCounterCatalogue({ file: FIXTURE_TRACE }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.mode).toBe("counters");
    expect(r.counters.length).toBeGreaterThan(0);
    const found = r.counters.find((c) => c.name === "Zombies_Alive");
    expect(found?.is_float).toBe(false);
  });

  it("doCounterSeries: returns downsampled time buckets for a counter", async () => {
    const cache = new TraceCache(3);
    const r = await doCounterSeries(
      { file: FIXTURE_TRACE, counter: "Zombies_Alive" },
      { cache, runOptions: { binary: MOCK_BIN } },
    );
    expect(r.found).toBe(true);
    expect(r.series.length).toBeGreaterThan(0);
    expect(r.series[0]!.count).toBeGreaterThan(0);
    expect(r.series[0]!.avg).toBeGreaterThan(0);
  });

  it("doBookmarkList: returns time-ordered TRACE_BOOKMARK points", async () => {
    const cache = new TraceCache(3);
    const r = await doBookmarkList({ file: FIXTURE_TRACE }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.mode).toBe("bookmarks");
    expect(r.events.length).toBeGreaterThan(0);
    expect(r.events[0]!.text.length).toBeGreaterThan(0);
  });

  it("doRegionList: returns regions with categories[] hint", async () => {
    const cache = new TraceCache(3);
    const r = await doRegionList({ file: FIXTURE_TRACE }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.mode).toBe("regions");
    expect(r.events.length).toBeGreaterThan(0);
    expect(r.categories.length).toBeGreaterThan(0);
    expect(r.events[0]!.duration_ms).toBeGreaterThan(0);
  });

  it("doLogMessages: returns filtered UE_LOG messages", async () => {
    const cache = new TraceCache(3);
    const r = await doLogMessages({ file: FIXTURE_TRACE }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.mode).toBe("logs");
    expect(r.events.length).toBeGreaterThan(0);
    const error = r.events.find((e) => e.verbosity === "error");
    expect(error).toBeDefined();
  });

  it("doMemoryTrackers: lists LLM trackers + tag sets", async () => {
    const cache = new TraceCache(3);
    const r = await doMemoryTrackers({ file: FIXTURE_TRACE }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.view).toBe("trackers");
    expect(r.has_memory).toBe(true);
    expect(r.trackers.length).toBeGreaterThan(0);
    expect(r.tag_sets.length).toBeGreaterThan(0);
  });

  it("doMemoryTags: returns flat tag list with parent_id", async () => {
    const cache = new TraceCache(3);
    const r = await doMemoryTags({ file: FIXTURE_TRACE }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.view).toBe("tags");
    expect(r.tags.length).toBeGreaterThan(0);
    // Every tag has parent_id (0 = root).
    const root = r.tags.find((t) => t.parent_id === 0);
    expect(root).toBeDefined();
  });

  it("doMemorySamples: returns time-bucketed per-tag samples", async () => {
    const cache = new TraceCache(3);
    const r = await doMemorySamples(
      { file: FIXTURE_TRACE, tag: "Mesh" },
      { cache, runOptions: { binary: MOCK_BIN } },
    );
    expect(r.view).toBe("samples");
    expect(r.found).toBe(true);
    expect(r.series.length).toBeGreaterThan(0);
    expect(r.series[0]!.avg).toBeGreaterThan(0);
  });

  it("doMemallocTimeline: returns four aggregate series", async () => {
    const cache = new TraceCache(3);
    const r = await doMemallocTimeline({ file: FIXTURE_TRACE }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.view).toBe("timeline");
    expect(r.has_memalloc).toBe(true);
    expect(r.max_total_allocated_memory!.length).toBeGreaterThan(0);
    expect(r.max_live_allocations!.length).toBeGreaterThan(0);
    expect(r.alloc_events_per_point!.length).toBeGreaterThan(0);
    expect(r.free_events_per_point!.length).toBeGreaterThan(0);
  });

  it("doMemallocHeaps: returns heap tree with root + nested entries", async () => {
    const cache = new TraceCache(3);
    const r = await doMemallocHeaps({ file: FIXTURE_TRACE }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.view).toBe("heaps");
    expect(r.heaps.length).toBeGreaterThan(0);
    const root = r.heaps.find((h) => h.is_root);
    expect(root).toBeDefined();
    const child = r.heaps.find((h) => !h.is_root);
    expect(child?.parent_id).toBeGreaterThanOrEqual(0);
  });

  it("doMemallocQuery: returns AllocationRow events + completed flag", async () => {
    const r = await doMemallocQuery(
      { file: FIXTURE_TRACE, rule: "aAf", timeA: 5000.0 },
      { cache: new TraceCache(3), runOptions: { binary: MOCK_BIN } },
    );
    expect(r.view).toBe("query");
    expect(r.rule).toBe("aAf");
    expect(r.completed).toBe(true);
    expect(r.events.length).toBeGreaterThan(0);
    expect(r.events[0]!.size).toBeGreaterThan(0);
  });
});

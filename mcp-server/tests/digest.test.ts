import { describe, expect, it, beforeEach } from "vitest";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { chmod } from "node:fs/promises";

import { runTraceDigest, extractTrailingJson, TraceDigestError } from "../src/digest.js";
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
  it("doOverview: returns frame_stats, slowest_frames, and top events", async () => {
    const cache = new TraceCache(3);
    const r = await doOverview({ file: FIXTURE_TRACE }, { cache, runOptions: { binary: MOCK_BIN } });
    expect(r.mode).toBe("overview");
    expect(r.frame_stats.p95_ms).toBeGreaterThan(0);
    expect(r.slowest_frames.length).toBeGreaterThan(0);
    expect(r.events.length).toBeGreaterThan(0);
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
});

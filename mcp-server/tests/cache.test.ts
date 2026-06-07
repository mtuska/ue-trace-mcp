import { describe, expect, it } from "vitest";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { utimes } from "node:fs/promises";

import { TraceCache, variantHash } from "../src/cache.js";
import type { DigestOutput } from "../src/types.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const FIXTURE = join(__dirname, "fixtures", "digest-sample.json");

function makeValue(name: string): DigestOutput {
  return {
    file: name,
    mode: "digest",
    duration_ms: 1.0,
    frame_count: 1,
    events: [],
  };
}

describe("variantHash", () => {
  it("is stable across argument insertion order", () => {
    const a = variantHash("digest", { prefix: "Zombie_", limit: 200 });
    const b = variantHash("digest", { limit: 200, prefix: "Zombie_" });
    expect(a).toBe(b);
  });

  it("excludes file/fileA/fileB (those are part of the cache key separately)", () => {
    const a = variantHash("digest", { file: "/a", prefix: "X" });
    const b = variantHash("digest", { file: "/b", prefix: "X" });
    expect(a).toBe(b);
  });

  it("differentiates different variants", () => {
    const a = variantHash("digest", { prefix: "Zombie_" });
    const b = variantHash("digest", { prefix: "Player_" });
    expect(a).not.toBe(b);
  });
});

describe("TraceCache", () => {
  it("hits on (path, mtime, variant) repeat", async () => {
    const c = new TraceCache(3);
    await c.put(FIXTURE, "v1", makeValue("first"));
    const hit = await c.get(FIXTURE, "v1");
    expect(hit?.value.file).toBe("first");
  });

  it("misses on variant change", async () => {
    const c = new TraceCache(3);
    await c.put(FIXTURE, "v1", makeValue("first"));
    const miss = await c.get(FIXTURE, "v2");
    expect(miss).toBeNull();
  });

  it("misses after mtime bumps (touch invalidates)", async () => {
    const c = new TraceCache(3);
    await c.put(FIXTURE, "v1", makeValue("first"));

    // Touch the fixture forward by 5 seconds — simulates a re-captured trace.
    const future = new Date(Date.now() + 5000);
    await utimes(FIXTURE, future, future);

    const miss = await c.get(FIXTURE, "v1");
    expect(miss).toBeNull();
  });

  it("evicts LRU when over capacity", async () => {
    const c = new TraceCache(2);
    await c.put(FIXTURE, "v1", makeValue("a"));
    await c.put(FIXTURE, "v2", makeValue("b"));
    await c.put(FIXTURE, "v3", makeValue("c"));
    expect(c.size()).toBe(2);
    expect(await c.get(FIXTURE, "v1")).toBeNull(); // evicted
    expect((await c.get(FIXTURE, "v3"))?.value.file).toBe("c");
  });

  it("get bumps LRU order", async () => {
    const c = new TraceCache(2);
    await c.put(FIXTURE, "v1", makeValue("a"));
    await c.put(FIXTURE, "v2", makeValue("b"));
    // Touch v1 — it should now be most-recently-used.
    await c.get(FIXTURE, "v1");
    await c.put(FIXTURE, "v3", makeValue("c"));
    // v2 was the LRU; expect it evicted.
    expect(await c.get(FIXTURE, "v2")).toBeNull();
    expect((await c.get(FIXTURE, "v1"))?.value.file).toBe("a");
    expect((await c.get(FIXTURE, "v3"))?.value.file).toBe("c");
  });
});

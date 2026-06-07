import { afterEach, describe, expect, it } from "vitest";

import { cacheRoot, detectPlatform, ensureBinary } from "../src/binary.js";

describe("detectPlatform", () => {
  it("maps linux x64", () => {
    expect(detectPlatform("linux", "x64")).toEqual({
      slug: "linux-x64",
      ext: "tar.gz",
      binaryName: "TraceDigest",
    });
  });

  it("maps windows x64", () => {
    expect(detectPlatform("win32", "x64")).toEqual({
      slug: "windows-x64",
      ext: "zip",
      binaryName: "TraceDigest.exe",
    });
  });

  it("throws a friendly error on macOS until we ship binaries for it", () => {
    expect(() => detectPlatform("darwin", "arm64")).toThrow(/macOS isn't shipped yet/);
  });

  it("throws on truly unsupported (platform, arch) combos", () => {
    expect(() => detectPlatform("freebsd" as NodeJS.Platform, "x64")).toThrow(/unsupported platform/);
  });

  it("rejects 32-bit linux", () => {
    expect(() => detectPlatform("linux", "ia32")).toThrow(/unsupported platform/);
  });
});

describe("cacheRoot", () => {
  const origCache = process.env.UE_TRACE_CACHE_DIR;
  afterEach(() => {
    if (origCache === undefined) delete process.env.UE_TRACE_CACHE_DIR;
    else process.env.UE_TRACE_CACHE_DIR = origCache;
  });

  it("honours UE_TRACE_CACHE_DIR override", () => {
    process.env.UE_TRACE_CACHE_DIR = "/tmp/custom-cache";
    expect(cacheRoot()).toBe("/tmp/custom-cache");
  });

  it("defaults under the user home dir when unset", () => {
    delete process.env.UE_TRACE_CACHE_DIR;
    expect(cacheRoot()).toMatch(/\.cache\/ue-trace-mcp$/);
  });
});

describe("ensureBinary", () => {
  const origBin = process.env.TRACE_DIGEST_BIN;
  afterEach(() => {
    if (origBin === undefined) delete process.env.TRACE_DIGEST_BIN;
    else process.env.TRACE_DIGEST_BIN = origBin;
  });

  it("short-circuits to TRACE_DIGEST_BIN if set, never hitting the network", async () => {
    process.env.TRACE_DIGEST_BIN = "/path/to/dev-build/TraceDigest";
    // If this tried to hit the network it would hang or fail; finishing
    // synchronously and returning the env value proves the short-circuit.
    expect(await ensureBinary()).toBe("/path/to/dev-build/TraceDigest");
  });
});

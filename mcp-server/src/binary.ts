import { spawn } from "node:child_process";
import { createWriteStream } from "node:fs";
import { chmod, mkdir, readFile, stat, unlink } from "node:fs/promises";
import { arch, homedir, platform } from "node:os";
import { dirname, join } from "node:path";
import { Readable } from "node:stream";
import { pipeline } from "node:stream/promises";
import { fileURLToPath } from "node:url";

/**
 * Resolve a usable TraceDigest binary path. On first run for a given package
 * version, downloads the matching binary from GitHub Releases into a per-user
 * cache directory. Subsequent runs hit the cache.
 *
 * Resolution order:
 *   1. `TRACE_DIGEST_BIN` env var if set — escape hatch for devs running a
 *      locally-built binary, or for users on platforms we don't ship yet.
 *   2. Cached binary at `$UE_TRACE_CACHE_DIR/<version>/<binary>` (default
 *      cache root: `~/.cache/ue-trace-mcp/`).
 *   3. Download + extract from
 *      `https://github.com/mtuska/ue-trace-mcp/releases/download/v<version>/
 *       TraceDigest-v<version>-ue<UE_BRANCH>-<slug>.<ext>`
 *
 * The npm package version IS the source of truth for which binary to fetch —
 * version-locking the TS server to its matching native artifact happens
 * automatically because `npx @mtuska/ue-trace-mcp@0.3.1` resolves both pieces
 * from the same `0.3.1` tag.
 */

// Engine branch the binaries were built against. Update this in lockstep with
// .github/workflows/release.yml's `ue_branch` input whenever we move to a new
// UE version — the filename in the GH release embeds this.
const UE_BRANCH = "5.7";

// Owner/repo for the releases. Kept here rather than read from package.json
// `repository.url` to avoid parsing git URL syntax at runtime.
const GH_RELEASE_BASE = "https://github.com/mtuska/ue-trace-mcp/releases/download";

interface PlatformInfo {
  /** Matches the slug in the release artifact filename. */
  slug: "linux-x64" | "windows-x64";
  ext: "tar.gz" | "zip";
  binaryName: "TraceDigest" | "TraceDigest.exe";
}

export class BinaryResolveError extends Error {
  constructor(message: string, public readonly hint?: string) {
    super(message);
    this.name = "BinaryResolveError";
  }
}

export function detectPlatform(
  p: NodeJS.Platform = platform(),
  a: string = arch(),
): PlatformInfo {
  if (p === "linux" && a === "x64") {
    return { slug: "linux-x64", ext: "tar.gz", binaryName: "TraceDigest" };
  }
  if (p === "win32" && a === "x64") {
    return { slug: "windows-x64", ext: "zip", binaryName: "TraceDigest.exe" };
  }
  if (p === "darwin") {
    throw new BinaryResolveError(
      `macOS isn't shipped yet`,
      `Set TRACE_DIGEST_BIN to a locally-built TraceDigest binary, or open an issue at https://github.com/mtuska/ue-trace-mcp/issues.`,
    );
  }
  throw new BinaryResolveError(
    `unsupported platform: ${p}-${a}`,
    `Set TRACE_DIGEST_BIN to a locally-built TraceDigest binary.`,
  );
}

export function cacheRoot(): string {
  return process.env.UE_TRACE_CACHE_DIR ?? join(homedir(), ".cache", "ue-trace-mcp");
}

/** Reads the version from this package's own package.json. */
let cachedVersion: string | undefined;
export async function packageVersion(): Promise<string> {
  if (cachedVersion) return cachedVersion;
  // dist/binary.js is at <pkg>/dist/binary.js, package.json at <pkg>/package.json.
  // When running from src via tsx, src/binary.ts is at <pkg>/src/binary.ts.
  const here = dirname(fileURLToPath(import.meta.url));
  for (const rel of ["..", "../.."]) {
    try {
      const raw = await readFile(join(here, rel, "package.json"), "utf8");
      const pkg = JSON.parse(raw) as { name?: string; version?: string };
      if (pkg.name?.includes("ue-trace-mcp") && pkg.version) {
        cachedVersion = pkg.version;
        return pkg.version;
      }
    } catch {
      /* try next */
    }
  }
  throw new BinaryResolveError("could not read package.json to determine version");
}

async function exists(p: string): Promise<boolean> {
  try {
    await stat(p);
    return true;
  } catch {
    return false;
  }
}

/**
 * Download a single URL to disk. We don't stream into the extractor directly
 * because the Windows path needs a real seekable file for `tar -xf <zip>` to
 * work, and the size is small enough that an intermediate file is fine.
 */
async function downloadFile(url: string, dest: string): Promise<void> {
  const res = await fetch(url, { redirect: "follow" });
  if (!res.ok || !res.body) {
    throw new BinaryResolveError(
      `download failed: HTTP ${res.status} ${res.statusText}`,
      `URL: ${url}`,
    );
  }
  // Web ReadableStream → Node Readable for pipeline().
  await pipeline(Readable.fromWeb(res.body as never), createWriteStream(dest));
}

/**
 * Extract a tarball or zip into a directory. Uses the system `tar` — bsdtar on
 * Windows 10+/macOS handles both formats; GNU tar on Linux handles tar.gz
 * (which is all we ship on Linux). Avoids a node-side extraction dep.
 */
async function extract(archive: string, dir: string, ext: "tar.gz" | "zip"): Promise<void> {
  const args =
    ext === "zip"
      ? ["-xf", archive, "-C", dir]
      : ["-xzf", archive, "-C", dir];
  await new Promise<void>((resolve, reject) => {
    const child = spawn("tar", args, { stdio: ["ignore", "pipe", "pipe"] });
    let stderr = "";
    child.stderr?.on("data", (b: Buffer) => {
      stderr += b.toString("utf8");
    });
    child.once("error", (e) => reject(new BinaryResolveError(`tar spawn failed: ${e.message}`)));
    child.once("close", (code) => {
      if (code === 0) resolve();
      else reject(new BinaryResolveError(`tar exited ${code}`, stderr.trim()));
    });
  });
}

export interface EnsureBinaryOptions {
  /** Override the version (default: read from package.json). */
  version?: string;
  /** Suppress the "downloading…" stderr line. */
  quiet?: boolean;
}

export async function ensureBinary(opts: EnsureBinaryOptions = {}): Promise<string> {
  // 1. Explicit override — always wins. Used by devs against a local build, by
  // tests against a mock binary, and by users on unsupported platforms.
  if (process.env.TRACE_DIGEST_BIN) {
    return process.env.TRACE_DIGEST_BIN;
  }

  const info = detectPlatform();
  const version = opts.version ?? (await packageVersion());
  const dir = join(cacheRoot(), version);
  const binPath = join(dir, info.binaryName);

  // 2. Cache hit.
  if (await exists(binPath)) {
    return binPath;
  }

  // 3. Download + extract.
  await mkdir(dir, { recursive: true });
  const archiveName = `TraceDigest-v${version}-ue${UE_BRANCH}-${info.slug}.${info.ext}`;
  const url = `${GH_RELEASE_BASE}/v${version}/${archiveName}`;
  const archivePath = join(dir, archiveName);

  if (!opts.quiet) {
    process.stderr.write(`ue-trace-mcp: fetching TraceDigest v${version} for ${info.slug}…\n`);
  }

  try {
    await downloadFile(url, archivePath);
    await extract(archivePath, dir, info.ext);
  } finally {
    // Clean up the archive whether or not extract succeeded. A failed extract
    // leaves us in a recoverable state for the next run.
    await unlink(archivePath).catch(() => undefined);
  }

  if (!(await exists(binPath))) {
    throw new BinaryResolveError(
      `binary not found at ${binPath} after extracting ${archiveName}`,
      `The release archive may have an unexpected layout. Set TRACE_DIGEST_BIN to bypass.`,
    );
  }

  // chmod +x for POSIX (no-op on Windows where the extension determines execability).
  await chmod(binPath, 0o755).catch(() => undefined);

  if (!opts.quiet) {
    process.stderr.write(`ue-trace-mcp: cached at ${binPath}\n`);
  }
  return binPath;
}

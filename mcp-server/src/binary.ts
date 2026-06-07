import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { createReadStream, createWriteStream } from "node:fs";
import { chmod, mkdir, readFile, stat, unlink } from "node:fs/promises";
import { arch, homedir, platform } from "node:os";
import { dirname, isAbsolute, join, relative, resolve as resolvePath } from "node:path";
import { Readable } from "node:stream";
import { pipeline } from "node:stream/promises";
import { fileURLToPath } from "node:url";

/**
 * Resolve a usable TraceDigest binary path. On first run for a given package
 * version, downloads the matching binary from GitHub Releases into a per-user
 * cache directory after verifying its SHA-256 against a manifest baked into
 * this npm package at publish time.
 *
 * Trust chain:
 *   npm package (signed via --provenance) → binaries.json (in the package) →
 *   GitHub release archive (SHA-256 verified against binaries.json)
 *
 * Note this is stronger than fetching a sibling SHA256SUMS from the release:
 * because the manifest lives inside the npm-provenance-signed package, an
 * attacker who could swap both the archive and a sibling SUMS file on the
 * GitHub side would still fail the local hash check.
 *
 * Resolution order:
 *   1. `TRACE_DIGEST_BIN` env var if set — escape hatch for devs running a
 *      locally-built binary, or for users on platforms we don't ship yet.
 *   2. Cached binary at `$UE_TRACE_CACHE_DIR/<version>/<binary>` (default
 *      cache root: `~/.cache/ue-trace-mcp/`).
 *   3. Download + checksum-verify + extract.
 */

// Owner/repo for the releases. Kept here rather than read from package.json
// `repository.url` to avoid parsing git URL syntax at runtime.
const GH_RELEASE_BASE = "https://github.com/mtuska/ue-trace-mcp/releases/download";

// Hosts the download path is permitted to land on (after following redirects).
// GitHub re-routes release downloads via objects.githubusercontent.com; both
// have to be on the allowlist. Anything else after a redirect means a CDN swap
// or an outright DNS hijack and we refuse to extract.
const ALLOWED_DOWNLOAD_HOSTS = new Set([
  "github.com",
  "objects.githubusercontent.com",
  "release-assets.githubusercontent.com",
]);

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

/** Walks up from this module's URL looking for a package root. */
async function readPackageFile(name: "package.json" | "binaries.json"): Promise<string | null> {
  // dist/binary.js is at <pkg>/dist/binary.js; src/binary.ts is at <pkg>/src/binary.ts.
  const here = dirname(fileURLToPath(import.meta.url));
  for (const rel of ["..", "../.."]) {
    try {
      return await readFile(join(here, rel, name), "utf8");
    } catch {
      /* try next */
    }
  }
  return null;
}

/** Reads the version from this package's own package.json. */
let cachedVersion: string | undefined;
export async function packageVersion(): Promise<string> {
  if (cachedVersion) return cachedVersion;
  const raw = await readPackageFile("package.json");
  if (raw) {
    const pkg = JSON.parse(raw) as { name?: string; version?: string };
    if (pkg.name?.includes("ue-trace-mcp") && pkg.version) {
      cachedVersion = pkg.version;
      return pkg.version;
    }
  }
  throw new BinaryResolveError("could not read package.json to determine version");
}

/**
 * Shape of binaries.json — the integrity manifest baked into the published
 * npm package by .github/workflows/release.yml. Maps each shipped platform
 * slug to its release-archive filename and expected SHA-256.
 */
export interface BinariesManifest {
  version: string;
  binaries: Record<
    "linux-x64" | "windows-x64",
    { filename: string; sha256: string } | undefined
  >;
}

let cachedManifest: BinariesManifest | undefined;
export async function readBinariesManifest(): Promise<BinariesManifest> {
  if (cachedManifest) return cachedManifest;
  const raw = await readPackageFile("binaries.json");
  if (!raw) {
    throw new BinaryResolveError(
      "binaries.json is missing from this package",
      "This usually means you're running from a source checkout. " +
        "Either build TraceDigest locally and set TRACE_DIGEST_BIN, or install " +
        "the published @mtuska/ue-trace-mcp package which ships the manifest.",
    );
  }
  cachedManifest = JSON.parse(raw) as BinariesManifest;
  return cachedManifest;
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
 * Download a single URL. Refuses redirects that escape the GitHub-hosted set
 * (no MITM via DNS hijack to a third-party host), and returns the response
 * body. The caller decides whether to stream-to-disk or buffer-in-memory.
 */
async function fetchAllowed(url: string): Promise<Response> {
  // `redirect: "manual"` would let us audit hops one at a time, but Node's
  // fetch surfaces them as 3xx responses with `Location`. We do that walk
  // ourselves so the final URL's host is auditable.
  let current = url;
  for (let hop = 0; hop < 5; hop++) {
    const u = new URL(current);
    if (!ALLOWED_DOWNLOAD_HOSTS.has(u.hostname)) {
      throw new BinaryResolveError(
        `download host not allowlisted: ${u.hostname}`,
        `Allowed: ${[...ALLOWED_DOWNLOAD_HOSTS].join(", ")}. URL: ${current}`,
      );
    }
    const res = await fetch(current, { redirect: "manual" });
    if (res.status >= 300 && res.status < 400) {
      const next = res.headers.get("location");
      if (!next) {
        throw new BinaryResolveError(`redirect ${res.status} without Location header`);
      }
      // Resolve relative redirects against the current URL.
      current = new URL(next, current).toString();
      continue;
    }
    if (!res.ok || !res.body) {
      throw new BinaryResolveError(
        `download failed: HTTP ${res.status} ${res.statusText}`,
        `URL: ${current}`,
      );
    }
    return res;
  }
  throw new BinaryResolveError(`too many redirects fetching ${url}`);
}

async function downloadFile(url: string, dest: string): Promise<void> {
  const res = await fetchAllowed(url);
  await pipeline(Readable.fromWeb(res.body as never), createWriteStream(dest));
}

async function sha256OfFile(path: string): Promise<string> {
  const hash = createHash("sha256");
  await pipeline(createReadStream(path), hash);
  return hash.digest("hex");
}

/**
 * Extract a tarball or zip into `dir`, then defensively verify nothing
 * escaped. Uses the system `tar` (bsdtar on Windows 10+/macOS, GNU tar on
 * Linux) so we don't ship a node-side archive parser. After extraction every
 * surviving file path must resolve inside `dir`; anything else (a malicious
 * archive with `..` traversal or absolute paths) gets the cache dir wiped.
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

/**
 * Walk `dir` after extraction and reject any entry that isn't a regular file
 * resolving inside `dir`. Symlinks (which `tar` happily creates for entries
 * pointing outside the cwd) are forbidden — we don't ship any, so their
 * presence indicates a malicious archive.
 */
async function assertContainedTree(dir: string): Promise<void> {
  const { lstat, readdir } = await import("node:fs/promises");
  const rootReal = resolvePath(dir);
  async function walk(p: string): Promise<void> {
    const st = await lstat(p);
    if (st.isSymbolicLink()) {
      throw new BinaryResolveError(
        `refusing extracted symlink: ${p}`,
        `The release archive contains a symbolic link, which our release pipeline never emits. Treating as tampered.`,
      );
    }
    const real = resolvePath(p);
    const rel = relative(rootReal, real);
    if (rel.startsWith("..") || isAbsolute(rel)) {
      throw new BinaryResolveError(`extracted path escapes cache dir: ${p}`);
    }
    if (st.isDirectory()) {
      for (const entry of await readdir(p)) {
        await walk(join(p, entry));
      }
    } else if (!st.isFile()) {
      throw new BinaryResolveError(`refusing non-regular file in archive: ${p}`);
    }
  }
  await walk(rootReal);
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

  // 3. Download + checksum-verify + extract. Filename and expected hash come
  // from the manifest baked into this npm package — NOT from a sibling file
  // on the GitHub release. That puts the integrity anchor inside the
  // provenance-signed package boundary.
  const manifest = await readBinariesManifest();
  const entry = manifest.binaries[info.slug];
  if (!entry) {
    throw new BinaryResolveError(
      `binaries.json has no entry for ${info.slug}`,
      "This package was built without a binary for your platform. Set TRACE_DIGEST_BIN to a locally-built TraceDigest.",
    );
  }
  if (manifest.version !== version) {
    // Sanity check: package.json and binaries.json should always agree.
    throw new BinaryResolveError(
      `package.json version (${version}) does not match binaries.json version (${manifest.version})`,
      "The npm package is internally inconsistent. Reinstall or report this.",
    );
  }

  await mkdir(dir, { recursive: true, mode: 0o755 });
  const archiveName = entry.filename;
  const archiveUrl = `${GH_RELEASE_BASE}/v${version}/${archiveName}`;
  const archivePath = join(dir, archiveName);

  if (!opts.quiet) {
    process.stderr.write(`ue-trace-mcp: fetching TraceDigest v${version} for ${info.slug}…\n`);
  }

  try {
    await downloadFile(archiveUrl, archivePath);

    const actual = await sha256OfFile(archivePath);
    if (actual !== entry.sha256) {
      throw new BinaryResolveError(
        `SHA-256 mismatch for ${archiveName}`,
        `expected ${entry.sha256}, got ${actual}. Possible MITM, CDN tampering, or corrupted download. Refusing to extract.`,
      );
    }

    await extract(archivePath, dir, info.ext);
    await assertContainedTree(dir);
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
    process.stderr.write(`ue-trace-mcp: verified + cached at ${binPath}\n`);
  }
  return binPath;
}

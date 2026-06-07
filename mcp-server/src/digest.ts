import { spawn } from "node:child_process";
import { access, constants } from "node:fs/promises";
import { resolve as resolvePath } from "node:path";

import type { AnyDigestOutput } from "./types.js";
import { DaemonRegistry } from "./daemon.js";

export interface DigestRunOptions {
  /**
   * Daemon registry. When present, queries route through a long-lived daemon
   * keyed by `(absPath × mtime)`. Cold call still pays the parse; subsequent
   * queries on the same trace skip it. This is the default for production.
   */
  daemons?: DaemonRegistry;

  /**
   * Path to the TraceDigest Program binary. Required either way: the daemon
   * registry needs it to spawn, and the one-shot fallback exec's it directly.
   * Tests stub this with mock-bin.mjs.
   */
  binary?: string;

  /** Cap on stdout we'll buffer in one-shot mode. Default 256 MiB. */
  maxStdoutBytes?: number;
  /** One-shot process timeout. Default 60s. */
  timeoutMs?: number;
}

export interface DigestArgsRaw {
  file: string;
  file2?: string;
  mode:
    | "digest"
    | "timeline"
    | "frames"
    | "compare"
    | "overview"
    | "frame"
    | "callers"
    | "callees"
    | "threads";
  prefix?: string;
  event?: string;
  limit?: number;
  threshold?: number;
  frameRange?: [number, number];
  frame?: number;
}

export class TraceDigestError extends Error {
  constructor(
    message: string,
    public readonly exitCode: number | null,
    public readonly stderr: string,
  ) {
    super(message);
    this.name = "TraceDigestError";
  }
}

function resolveBinary(opts: DigestRunOptions): string {
  const bin = opts.binary ?? process.env.TRACE_DIGEST_BIN;
  if (!bin) {
    throw new TraceDigestError(
      "no runner configured: set TRACE_DIGEST_BIN (path to the built TraceDigest Program)",
      null,
      "",
    );
  }
  return bin;
}

function buildBinaryArgv(args: DigestArgsRaw, outPath?: string): string[] {
  // Order doesn't matter to FParse, but stable order makes tests deterministic.
  const argv: string[] = [`-mode=${args.mode}`, `-file=${args.file}`];
  if (args.file2) argv.push(`-file2=${args.file2}`);
  if (args.prefix) argv.push(`-prefix=${args.prefix}`);
  if (args.event) argv.push(`-event=${args.event}`);
  if (args.limit !== undefined) argv.push(`-limit=${args.limit}`);
  if (args.threshold !== undefined) argv.push(`-threshold=${args.threshold}`);
  if (args.frameRange) {
    const [a, b] = args.frameRange;
    argv.push(`-framerange=${a}:${b}`);
  }
  if (args.frame !== undefined) argv.push(`-frame=${args.frame}`);
  if (outPath) argv.push(`-out=${outPath}`);
  return argv;
}

/**
 * Run TraceDigest and return its JSON output. Routes through the daemon
 * registry when one is configured; falls back to a one-shot spawn (the path
 * tests use with mock-bin.mjs) when not.
 */
export async function runTraceDigest(
  args: DigestArgsRaw,
  opts: DigestRunOptions = {},
): Promise<AnyDigestOutput> {
  if (opts.daemons) {
    return runViaDaemon(args, opts.daemons);
  }
  const binary = resolveBinary(opts);
  await assertExecutable(binary);
  return runBinary(binary, args, opts);
}

async function runViaDaemon(
  args: DigestArgsRaw,
  registry: DaemonRegistry,
): Promise<AnyDigestOutput> {
  // Build the cmdline the daemon's FArgs::Parse expects. -file/-daemon/-socket
  // are owned by the daemon; we send only the query shape.
  const parts: string[] = [`-mode=${args.mode}`];
  if (args.file2) parts.push(`-file2=${args.file2}`);
  if (args.prefix) parts.push(`-prefix=${args.prefix}`);
  if (args.event) parts.push(`-event=${args.event}`);
  if (args.limit !== undefined) parts.push(`-limit=${args.limit}`);
  if (args.threshold !== undefined) parts.push(`-threshold=${args.threshold}`);
  if (args.frame !== undefined) parts.push(`-frame=${args.frame}`);
  if (args.frameRange) parts.push(`-framerange=${args.frameRange[0]}:${args.frameRange[1]}`);

  const data = await registry.query(args.file, parts.join(" "));
  return data as AnyDigestOutput;
}

async function runBinary(
  binary: string,
  args: DigestArgsRaw,
  opts: DigestRunOptions,
): Promise<AnyDigestOutput> {
  const maxStdout = opts.maxStdoutBytes ?? 256 * 1024 * 1024;
  const timeout = opts.timeoutMs ?? 60_000;
  const argv = buildBinaryArgv(args);

  return new Promise<AnyDigestOutput>((resolve, reject) => {
    const child = spawn(binary, argv, { stdio: ["ignore", "pipe", "pipe"] });

    const stdoutChunks: Buffer[] = [];
    let stdoutLen = 0;
    let stderr = "";
    let killedForOverflow = false;
    let killedForTimeout = false;

    const timer = setTimeout(() => {
      killedForTimeout = true;
      child.kill("SIGKILL");
    }, timeout);

    child.stdout.on("data", (chunk: Buffer) => {
      stdoutLen += chunk.byteLength;
      if (stdoutLen > maxStdout) {
        killedForOverflow = true;
        child.kill("SIGKILL");
        return;
      }
      stdoutChunks.push(chunk);
    });

    child.stderr.on("data", (chunk: Buffer) => {
      stderr += chunk.toString("utf8");
    });

    child.on("error", (err) => {
      clearTimeout(timer);
      reject(new TraceDigestError(`failed to spawn ${binary}: ${err.message}`, null, stderr));
    });

    child.on("close", (code) => {
      clearTimeout(timer);

      if (killedForTimeout)
        return reject(new TraceDigestError(`TraceDigest timed out after ${timeout}ms`, code, stderr));
      if (killedForOverflow)
        return reject(new TraceDigestError(`TraceDigest stdout exceeded ${maxStdout} bytes`, code, stderr));
      if (code !== 0)
        return reject(new TraceDigestError(`TraceDigest exited with code ${code}`, code, stderr));

      const stdout = Buffer.concat(stdoutChunks).toString("utf8");
      try {
        const parsed = extractTrailingJson(stdout);
        resolve(parsed as AnyDigestOutput);
      } catch (e) {
        reject(
          new TraceDigestError(
            `TraceDigest output was not parseable JSON: ${(e as Error).message}`,
            code,
            stderr + "\n--- stdout tail ---\n" + stdout.slice(-2048),
          ),
        );
      }
    });
  });
}

/**
 * Pull the last balanced top-level JSON object out of a string. UE startup
 * output can prepend log lines to stdout in one-shot mode; we scan backward
 * from the last `}` to find the matching `{` and parse that slice.
 */
export function extractTrailingJson(stdout: string): unknown {
  const trimmed = stdout.trim();
  if (trimmed.startsWith("{") && trimmed.endsWith("}")) {
    try {
      return JSON.parse(trimmed);
    } catch {
      /* fall through */
    }
  }

  const end = trimmed.lastIndexOf("}");
  if (end === -1) throw new Error("no closing brace found in stdout");

  let depth = 0;
  let inString = false;
  let escape = false;

  for (let i = end; i >= 0; i--) {
    const c = trimmed[i];
    if (escape) { escape = false; continue; }
    if (inString) {
      if (c === "\\") escape = true;
      else if (c === '"') inString = false;
      continue;
    }
    if (c === '"') { inString = true; continue; }
    if (c === "}") depth++;
    else if (c === "{") {
      depth--;
      if (depth === 0) return JSON.parse(trimmed.slice(i, end + 1));
    }
  }
  throw new Error("could not balance JSON braces in stdout");
}

async function assertExecutable(path: string): Promise<void> {
  if (!path.includes("/") && !path.includes("\\")) return;
  const abs = resolvePath(path);
  try {
    await access(abs, constants.X_OK);
  } catch (e) {
    throw new TraceDigestError(
      `runner not found or not executable: ${abs}`,
      null,
      (e as Error).message,
    );
  }
}

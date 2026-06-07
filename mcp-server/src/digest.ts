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
    | "threads"
    | "channels"
    | "gpu"
    | "counters"
    | "bookmarks"
    | "regions"
    | "logs"
    | "memory"
    | "allocations"
    | "query"
    | "callstack"
    | "modules"
    | "task_list"
    | "task_drill";
  prefix?: string;
  event?: string;
  limit?: number;
  threshold?: number;
  frameRange?: [number, number];
  frame?: number;

  // v0.4 channel-aware flags.
  channel?: string;
  view?: string;
  counter?: string;
  category?: string;
  verbosity?: string;
  grep?: string;
  tracker?: string;
  tag?: string;
  rule?: string;
  timeA?: number;
  timeB?: number;
  buckets?: number;
  queue?: number;
  queryTimeoutMs?: number;
  intent?: string;
  params?: string;  // JSON string forwarded to C++ for parse
  callstackId?: number;
  taskId?: number;
  state?: string;
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

  // v0.4 channel-aware flags.
  if (args.channel) argv.push(`-channel=${args.channel}`);
  if (args.view) argv.push(`-view=${args.view}`);
  if (args.counter) argv.push(`-counter=${args.counter}`);
  if (args.category) argv.push(`-category=${args.category}`);
  if (args.verbosity) argv.push(`-verbosity=${args.verbosity}`);
  if (args.grep) argv.push(`-grep=${args.grep}`);
  if (args.tracker) argv.push(`-tracker=${args.tracker}`);
  if (args.tag) argv.push(`-tag=${args.tag}`);
  if (args.rule) argv.push(`-rule=${args.rule}`);
  if (args.timeA !== undefined) argv.push(`-timeA=${args.timeA}`);
  if (args.timeB !== undefined) argv.push(`-timeB=${args.timeB}`);
  if (args.buckets !== undefined) argv.push(`-buckets=${args.buckets}`);
  if (args.queue !== undefined) argv.push(`-queue=${args.queue}`);
  if (args.queryTimeoutMs !== undefined) argv.push(`-query-timeout-ms=${args.queryTimeoutMs}`);
  if (args.intent) argv.push(`-intent=${args.intent}`);
  if (args.params) {
    // Base64-encode the JSON params blob. UE's FParse::Value tokenizes on
    // commas in the raw value (and additionally on quotes/spaces depending
    // on shell), so passing JSON directly is unreliable. Base64 dodges all
    // of that for one extra char-class on each side.
    argv.push(`-params-b64=${Buffer.from(args.params, "utf8").toString("base64")}`);
  }
  if (args.callstackId !== undefined) argv.push(`-callstack-id=${args.callstackId}`);
  if (args.taskId !== undefined) argv.push(`-task-id=${args.taskId}`);
  if (args.state) argv.push(`-state=${args.state}`);

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
  if (args.channel) parts.push(`-channel=${args.channel}`);
  if (args.view) parts.push(`-view=${args.view}`);
  if (args.counter) parts.push(`-counter=${args.counter}`);
  if (args.category) parts.push(`-category=${args.category}`);
  if (args.verbosity) parts.push(`-verbosity=${args.verbosity}`);
  if (args.grep) parts.push(`-grep=${args.grep}`);
  if (args.tracker) parts.push(`-tracker=${args.tracker}`);
  if (args.tag) parts.push(`-tag=${args.tag}`);
  if (args.rule) parts.push(`-rule=${args.rule}`);
  if (args.timeA !== undefined) parts.push(`-timeA=${args.timeA}`);
  if (args.timeB !== undefined) parts.push(`-timeB=${args.timeB}`);
  if (args.buckets !== undefined) parts.push(`-buckets=${args.buckets}`);
  if (args.queue !== undefined) parts.push(`-queue=${args.queue}`);
  if (args.queryTimeoutMs !== undefined) parts.push(`-query-timeout-ms=${args.queryTimeoutMs}`);
  if (args.intent) parts.push(`-intent=${args.intent}`);
  if (args.params) {
    parts.push(`-params-b64=${Buffer.from(args.params, "utf8").toString("base64")}`);
  }
  if (args.callstackId !== undefined) parts.push(`-callstack-id=${args.callstackId}`);
  if (args.taskId !== undefined) parts.push(`-task-id=${args.taskId}`);
  if (args.state) parts.push(`-state=${args.state}`);

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

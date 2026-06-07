import { spawn, ChildProcess } from "node:child_process";
import { createConnection } from "node:net";
import { mkdir, rm, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import { resolve as resolvePath, join } from "node:path";
import { randomBytes } from "node:crypto";

import {
  checkAdmission,
  readBudgetFromEnv,
  readProcessRssMb,
  readSystemMemory,
  type MemoryBudget,
} from "./memory.js";

// One daemon per (absPath × mtime). Each daemon holds one parsed trace; the
// MCP server owns its lifecycle. Communication is length-prefixed UTF-8 over
// a Unix domain socket. Request payload is the same cmdline FArgs::Parse takes
// in the one-shot binary; response is `{ok, data|error}` with `data` carrying
// the same JSON shape every mode emits.

export interface DaemonHandle {
  key: string;
  file: string;
  absPath: string;
  mtimeMs: number;
  pid: number;
  socket: string;
  child: ChildProcess;
  spawnedAt: number;
  lastUsedAt: number;
  estimateMb: number;
  requestsServed: number;
}

export interface DaemonStatus {
  file: string;
  pid: number;
  socket: string;
  uptime_ms: number;
  idle_ms: number;
  estimate_mb: number;
  rss_mb: number;
  requests_served: number;
}

export interface DaemonOptions {
  /** Path to the TraceDigest Program binary. Required for daemon mode. */
  binary: string;
  /** Daemon idle timeout in seconds. Default 600. */
  idleTimeoutSec?: number;
  /** Sweep interval for idle-reaper. Default 30s. */
  reapIntervalMs?: number;
  /** Max resident daemons (LRU eviction beyond this). Default 5. */
  maxDaemons?: number;
  /** Memory budget. Defaults from env (TRACE_MIN_FREE_MB, TRACE_MAX_DAEMON_MB). */
  budget?: MemoryBudget;
  /** Where to keep sockets. Default /tmp/ue-trace-mcp-<uid>/. */
  socketDir?: string;
}

export class DaemonError extends Error {
  constructor(message: string, public readonly code: string) {
    super(message);
    this.name = "DaemonError";
  }
}

const READY_TOKEN = "DAEMON_READY";

export class DaemonRegistry {
  private readonly daemons = new Map<string, DaemonHandle>();
  private readonly opts: Required<Omit<DaemonOptions, "budget">> & { budget: MemoryBudget };
  private reapTimer: NodeJS.Timeout | null = null;
  private isShuttingDown = false;

  constructor(opts: DaemonOptions) {
    this.opts = {
      binary: opts.binary,
      idleTimeoutSec: opts.idleTimeoutSec ?? 600,
      reapIntervalMs: opts.reapIntervalMs ?? 30_000,
      maxDaemons: opts.maxDaemons ?? 5,
      budget: opts.budget ?? readBudgetFromEnv(),
      socketDir: opts.socketDir ?? join(tmpdir(), `ue-trace-mcp-${process.getuid?.() ?? "u"}`),
    };
  }

  /** Start the background idle-reap timer. Idempotent. */
  startReaper(): void {
    if (this.reapTimer) return;
    this.reapTimer = setInterval(() => {
      void this.reapIdle();
    }, this.opts.reapIntervalMs);
    // Allow process to exit even if our timer is pending.
    if (typeof this.reapTimer.unref === "function") this.reapTimer.unref();
  }

  /** Stop reaper and kill every spawned daemon. Safe to call multiple times. */
  async shutdown(): Promise<void> {
    if (this.isShuttingDown) return;
    this.isShuttingDown = true;
    if (this.reapTimer) {
      clearInterval(this.reapTimer);
      this.reapTimer = null;
    }
    const handles = [...this.daemons.values()];
    this.daemons.clear();
    await Promise.all(handles.map((h) => this.terminate(h, "mcp-shutdown")));
  }

  /** Run a query: route to existing daemon if warm, else spawn. */
  async query(file: string, cmdline: string): Promise<unknown> {
    const handle = await this.getOrSpawn(file);
    return this.sendQuery(handle, cmdline);
  }

  /** Send a control command (":ping", ":status", ":shutdown") to a daemon. */
  async control(file: string, cmd: ":ping" | ":status" | ":shutdown"): Promise<unknown> {
    const handle = this.daemons.get(await this.deriveKey(file));
    if (!handle) {
      throw new DaemonError(`no daemon running for ${file}`, "not-running");
    }
    return this.send(handle, cmd);
  }

  /**
   * Explicit unload: terminate the daemon for the given file. Returns true
   * if a daemon was running and we killed it; false if nothing was loaded.
   */
  async unload(file: string): Promise<boolean> {
    const key = await this.deriveKey(file);
    const handle = this.daemons.get(key);
    if (!handle) return false;
    this.daemons.delete(key);
    await this.terminate(handle, "explicit-unload");
    return true;
  }

  /** List currently-resident daemons with per-daemon status. */
  async status(): Promise<DaemonStatus[]> {
    const out: DaemonStatus[] = [];
    const now = Date.now();
    for (const h of this.daemons.values()) {
      const rssMb = await readProcessRssMb(h.pid);
      out.push({
        file: h.file,
        pid: h.pid,
        socket: h.socket,
        uptime_ms: now - h.spawnedAt,
        idle_ms: now - h.lastUsedAt,
        estimate_mb: h.estimateMb,
        rss_mb: rssMb,
        requests_served: h.requestsServed,
      });
    }
    return out;
  }

  /** Read system memory snapshot. Exposed for the status tool. */
  async systemMemory() {
    return readSystemMemory();
  }

  // ---------------------------------------------------------------------
  // Internals
  // ---------------------------------------------------------------------

  private async deriveKey(file: string): Promise<string> {
    const abs = resolvePath(file);
    const s = await stat(abs).catch(() => null);
    if (!s) throw new DaemonError(`trace file not found: ${abs}`, "not-found");
    return `${abs}#${s.mtimeMs}`;
  }

  private async getOrSpawn(file: string): Promise<DaemonHandle> {
    const abs = resolvePath(file);
    const s = await stat(abs).catch(() => null);
    if (!s) throw new DaemonError(`trace file not found: ${abs}`, "not-found");
    const key = `${abs}#${s.mtimeMs}`;

    const existing = this.daemons.get(key);
    if (existing) return existing;

    // Stale daemon for the same path but old mtime: kill before spawning new.
    for (const [k, h] of this.daemons) {
      if (h.absPath === abs && k !== key) {
        this.daemons.delete(k);
        await this.terminate(h, "mtime-changed");
      }
    }

    return this.spawn(abs, s.mtimeMs, key);
  }

  private async spawn(absPath: string, mtimeMs: number, key: string): Promise<DaemonHandle> {
    // Memory admission. Refuses with a clean error before we burn ~1s spawning
    // a daemon we'd then have to kill.
    const admission = await checkAdmission(absPath, this.opts.budget);
    if (!admission.ok) {
      throw new DaemonError(
        `refusing to load ${absPath}: ${admission.reason}`,
        "memory-budget",
      );
    }

    // Make room first if we're already at maxDaemons. Evict by lastUsedAt.
    while (this.daemons.size >= this.opts.maxDaemons) {
      const victim = this.pickLruVictim();
      if (!victim) break;
      this.daemons.delete(victim.key);
      await this.terminate(victim, "lru-evict");
    }

    await mkdir(this.opts.socketDir, { recursive: true, mode: 0o700 });
    const socket = join(
      this.opts.socketDir,
      `${randomBytes(8).toString("hex")}.sock`,
    );

    const argv = [
      "-daemon",
      `-file=${absPath}`,
      `-socket=${socket}`,
      `-idle-timeout=${this.opts.idleTimeoutSec}`,
    ];

    const child = spawn(this.opts.binary, argv, {
      stdio: ["ignore", "pipe", "pipe"],
      detached: false, // we own lifecycle; node tracks the child
    });

    // Wait until the daemon writes DAEMON_READY to stdout. UE startup output
    // (log lines, banner) interleaves; we just scan the buffer for the token.
    const readyPromise = new Promise<void>((resolve, reject) => {
      let buf = "";
      const onData = (b: Buffer) => {
        buf += b.toString("utf8");
        if (buf.includes(READY_TOKEN)) {
          child.stdout?.off("data", onData);
          resolve();
        }
      };
      child.stdout?.on("data", onData);
      child.once("exit", (code) =>
        reject(new DaemonError(`daemon exited before ready (code=${code})`, "spawn-failed")),
      );
      // 30s safety net. A cold parse on a multi-GB trace can take a while.
      setTimeout(() => reject(new DaemonError(`daemon spawn timed out`, "spawn-timeout")), 30_000);
    });

    try {
      await readyPromise;
    } catch (e) {
      try { child.kill("SIGKILL"); } catch {}
      throw e;
    }

    const handle: DaemonHandle = {
      key,
      file: absPath,
      absPath,
      mtimeMs,
      pid: child.pid!,
      socket,
      child,
      spawnedAt: Date.now(),
      lastUsedAt: Date.now(),
      estimateMb: admission.estimateMb,
      requestsServed: 0,
    };

    // If the daemon exits unexpectedly (idle timeout, crash), drop the entry so
    // a later query respawns instead of trying to dial a dead socket.
    child.once("exit", () => {
      if (this.daemons.get(key) === handle) {
        this.daemons.delete(key);
      }
    });

    this.daemons.set(key, handle);
    return handle;
  }

  private pickLruVictim(): DaemonHandle | undefined {
    let oldest: DaemonHandle | undefined;
    for (const h of this.daemons.values()) {
      if (!oldest || h.lastUsedAt < oldest.lastUsedAt) oldest = h;
    }
    return oldest;
  }

  private async sendQuery(handle: DaemonHandle, cmdline: string): Promise<unknown> {
    const resp = await this.send(handle, cmdline);
    handle.requestsServed += 1;
    return resp;
  }

  /** Send one length-prefixed payload, read one length-prefixed reply. */
  private send(handle: DaemonHandle, payload: string): Promise<unknown> {
    return new Promise((resolve, reject) => {
      const sock = createConnection({ path: handle.socket });
      let phase: "len" | "body" = "len";
      let needed = 4;
      const chunks: Buffer[] = [];
      let received = 0;
      let bodyLen = 0;

      const onError = (err: Error) =>
        reject(new DaemonError(`daemon ${handle.pid}: ${err.message}`, "ipc"));

      sock.on("error", onError);
      sock.on("data", (chunk: Buffer) => {
        chunks.push(chunk);
        received += chunk.byteLength;
        // Frame parsing loop: we may have header + body in one packet.
        while (true) {
          if (phase === "len" && received >= 4) {
            const merged = Buffer.concat(chunks);
            bodyLen = merged.readUInt32BE(0);
            chunks.length = 0;
            chunks.push(merged.subarray(4));
            received -= 4;
            phase = "body";
            needed = bodyLen;
          }
          if (phase === "body" && received >= needed) {
            const merged = Buffer.concat(chunks).subarray(0, needed);
            sock.end();
            try {
              const env = JSON.parse(merged.toString("utf8"));
              if (env.ok) resolve(env.data);
              else reject(new DaemonError(String(env.error ?? "unknown daemon error"), "remote"));
            } catch (e) {
              reject(new DaemonError(`bad JSON from daemon: ${(e as Error).message}`, "bad-json"));
            }
            return;
          }
          break;
        }
      });
      sock.on("connect", () => {
        const body = Buffer.from(payload, "utf8");
        const hdr = Buffer.alloc(4);
        hdr.writeUInt32BE(body.byteLength, 0);
        sock.write(Buffer.concat([hdr, body]));
        handle.lastUsedAt = Date.now();
      });
    });
  }

  private async reapIdle(): Promise<void> {
    if (this.isShuttingDown) return;
    const now = Date.now();
    const expiredMs = this.opts.idleTimeoutSec * 1000;
    const victims: DaemonHandle[] = [];
    for (const [key, h] of this.daemons) {
      if (now - h.lastUsedAt > expiredMs) {
        victims.push(h);
        this.daemons.delete(key);
      }
    }
    await Promise.all(victims.map((v) => this.terminate(v, "idle-reap")));
  }

  /** Politely SIGTERM, then SIGKILL after a grace period if still alive. */
  private async terminate(handle: DaemonHandle, _reason: string): Promise<void> {
    return new Promise<void>((resolve) => {
      if (handle.child.exitCode !== null) {
        void this.cleanupSocket(handle);
        return resolve();
      }
      const killTimer = setTimeout(() => {
        try { handle.child.kill("SIGKILL"); } catch {}
      }, 1500);
      handle.child.once("exit", () => {
        clearTimeout(killTimer);
        void this.cleanupSocket(handle).then(() => resolve());
      });
      try { handle.child.kill("SIGTERM"); } catch {
        clearTimeout(killTimer);
        void this.cleanupSocket(handle).then(() => resolve());
      }
    });
  }

  private async cleanupSocket(handle: DaemonHandle): Promise<void> {
    await rm(handle.socket, { force: true }).catch(() => undefined);
    await rm(`${handle.socket}.pid`, { force: true }).catch(() => undefined);
  }
}

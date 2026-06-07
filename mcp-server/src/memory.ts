import { readFile, stat } from "node:fs/promises";

// Memory-awareness helpers. Linux-first. Falls back to permissive limits on
// other platforms so we don't accidentally block valid loads.

export interface SystemMemory {
  /** MemAvailable from /proc/meminfo, in MiB. -1 if unknown. */
  availableMb: number;
  /** Total system memory in MiB. -1 if unknown. */
  totalMb: number;
}

export async function readSystemMemory(): Promise<SystemMemory> {
  if (process.platform !== "linux") {
    return { availableMb: -1, totalMb: -1 };
  }
  try {
    const raw = await readFile("/proc/meminfo", "utf8");
    const grab = (key: string): number => {
      const m = raw.match(new RegExp(`^${key}:\\s+(\\d+)\\s+kB`, "m"));
      return m ? Math.round(Number(m[1]) / 1024) : -1;
    };
    return { availableMb: grab("MemAvailable"), totalMb: grab("MemTotal") };
  } catch {
    return { availableMb: -1, totalMb: -1 };
  }
}

/** RSS of one process via /proc/<pid>/status, in MiB. -1 if unknown. */
export async function readProcessRssMb(pid: number): Promise<number> {
  if (process.platform !== "linux") return -1;
  try {
    const raw = await readFile(`/proc/${pid}/status`, "utf8");
    const m = raw.match(/^VmRSS:\s+(\d+)\s+kB/m);
    return m ? Math.round(Number(m[1]) / 1024) : -1;
  } catch {
    return -1;
  }
}

/**
 * Heuristic estimate for the resident-set cost of parsing one .utrace.
 *
 * TraceServices keeps timer instances, frames, threads, etc. as in-memory
 * arrays. Observed ratio on ZombieProto: ~3.5× the on-disk file size for a
 * trace dominated by CPU scopes. We bump it to 4× to leave slack, and clamp
 * to a 64MB floor for tiny traces (engine init itself costs that much).
 */
export async function estimateTraceMemoryMb(file: string): Promise<number> {
  try {
    const s = await stat(file);
    const fileMb = Math.round(s.size / (1024 * 1024));
    return Math.max(64, Math.round(fileMb * 4));
  } catch {
    return -1;
  }
}

export interface MemoryBudget {
  /** Keep at least this much free after loading. Default 1024 MiB. */
  minFreeMb: number;
  /** Hard ceiling per daemon. Refuse if estimate exceeds. Default disabled (-1). */
  maxDaemonMb: number;
}

export function readBudgetFromEnv(): MemoryBudget {
  return {
    minFreeMb: Number(process.env.TRACE_MIN_FREE_MB ?? 1024),
    maxDaemonMb: Number(process.env.TRACE_MAX_DAEMON_MB ?? -1),
  };
}

export interface AdmissionResult {
  ok: boolean;
  reason?: string;
  estimateMb: number;
  availableMb: number;
}

/**
 * Decide whether spawning a daemon for `file` would push the system below
 * the headroom threshold. Conservative: if either reading is unavailable
 * (e.g. macOS), we allow the load — the user accepts the risk on platforms
 * where we can't introspect.
 */
export async function checkAdmission(
  file: string,
  budget: MemoryBudget,
): Promise<AdmissionResult> {
  const estimateMb = await estimateTraceMemoryMb(file);
  const sys = await readSystemMemory();

  if (estimateMb < 0 || sys.availableMb < 0) {
    return { ok: true, estimateMb, availableMb: sys.availableMb };
  }

  if (budget.maxDaemonMb > 0 && estimateMb > budget.maxDaemonMb) {
    return {
      ok: false,
      estimateMb,
      availableMb: sys.availableMb,
      reason: `estimated ${estimateMb}MiB exceeds max-daemon ${budget.maxDaemonMb}MiB (env TRACE_MAX_DAEMON_MB)`,
    };
  }

  const projectedAvailable = sys.availableMb - estimateMb;
  if (projectedAvailable < budget.minFreeMb) {
    return {
      ok: false,
      estimateMb,
      availableMb: sys.availableMb,
      reason: `loading would leave ${projectedAvailable}MiB free (need ${budget.minFreeMb}MiB; env TRACE_MIN_FREE_MB)`,
    };
  }

  return { ok: true, estimateMb, availableMb: sys.availableMb };
}

import { readFile, writeFile, mkdir } from "node:fs/promises";
import { dirname, isAbsolute, resolve } from "node:path";

import { packageVersion } from "./binary.js";

// `npx -y @mtuska/ue-trace-mcp install` writes (or merges) an entry into
// `.mcp.json` in the current directory so the MCP client picks the server
// up on next launch. Designed to be safe to run repeatedly: existing
// non-conflicting entries are preserved; an existing entry under the same
// key is only overwritten with `--force`.

export interface InstallOptions {
  /** Path to the config file. Default: `.mcp.json` in cwd. */
  configPath?: string;
  /** MCP server key under `mcpServers`. Default: "ue-trace". */
  name?: string;
  /** Overwrite an existing entry with the same key. Default: refuse. */
  force?: boolean;
  /** Print the resulting file without writing it. */
  dryRun?: boolean;
  /** Pin to the installer's own version (e.g. `@mtuska/ue-trace-mcp@0.5.1`) instead of floating to latest. */
  pin?: boolean;
}

export type InstallStatus = "added" | "updated" | "unchanged" | "would-write" | "exists-keep";

export interface InstallResult {
  status: InstallStatus;
  configPath: string;
  entryName: string;
  packageSpec: string;
  /** The full JSON content that was written (or would be written, in dry-run). */
  finalConfig: { mcpServers: Record<string, McpServerEntry>; [k: string]: unknown };
}

interface McpServerEntry {
  command: string;
  args?: string[];
  env?: Record<string, string>;
  [k: string]: unknown;
}

interface McpConfigFile {
  mcpServers?: Record<string, McpServerEntry>;
  [k: string]: unknown;
}

const PKG_NAME = "@mtuska/ue-trace-mcp";

function parseArgs(argv: readonly string[]): InstallOptions & { help: boolean } {
  const opts: InstallOptions & { help: boolean } = { help: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    switch (a) {
      case "--help":
      case "-h":
        opts.help = true;
        break;
      case "--force":
      case "-f":
        opts.force = true;
        break;
      case "--dry-run":
      case "-n":
        opts.dryRun = true;
        break;
      case "--pin":
        opts.pin = true;
        break;
      case "--name":
        opts.name = argv[++i];
        break;
      case "--config":
        opts.configPath = argv[++i];
        break;
      default:
        if (a && a.startsWith("--name=")) opts.name = a.slice("--name=".length);
        else if (a && a.startsWith("--config=")) opts.configPath = a.slice("--config=".length);
        else throw new Error(`unknown install flag: ${a}`);
    }
  }
  return opts;
}

const HELP = `\
Usage: npx -y @mtuska/ue-trace-mcp install [options]

Adds an entry to .mcp.json in the current directory so the MCP client
picks up the ue-trace-mcp server on next launch.

Options:
  --name <key>     Server key under mcpServers (default: ue-trace).
  --config <path>  Path to the config file (default: ./.mcp.json).
  --pin            Pin to this installer's exact version on npm
                   (e.g. @mtuska/ue-trace-mcp@0.5.1) instead of floating
                   to latest.
  --force, -f      Overwrite an existing entry under the same key.
  --dry-run, -n    Print what would be written; don't write the file.
  --help, -h       Show this help.

Examples:
  npx -y @mtuska/ue-trace-mcp install
  npx -y @mtuska/ue-trace-mcp install --pin
  npx -y @mtuska/ue-trace-mcp install --name ue-trace-prod --config ./tools/.mcp.json
`;

/**
 * Run the install command. Returns an exit code; the caller is responsible
 * for `process.exit()`.
 */
export async function runInstall(argv: readonly string[]): Promise<number> {
  let opts: InstallOptions & { help: boolean };
  try {
    opts = parseArgs(argv);
  } catch (e) {
    process.stderr.write(`${(e as Error).message}\n\n${HELP}`);
    return 2;
  }
  if (opts.help) {
    process.stdout.write(HELP);
    return 0;
  }

  const result = await install(opts);
  const verb =
    result.status === "added"        ? "Added"        :
    result.status === "updated"      ? "Updated"      :
    result.status === "unchanged"    ? "Unchanged"    :
    result.status === "exists-keep"  ? "Kept existing":
                                       "Would write";
  process.stdout.write(
    `${verb} mcpServers["${result.entryName}"] -> ${result.packageSpec}\n` +
    `  config: ${result.configPath}\n`,
  );
  if (opts.dryRun) {
    process.stdout.write(`\n${JSON.stringify(result.finalConfig, null, 2)}\n`);
  }
  if (result.status === "exists-keep") {
    process.stderr.write(
      `note: an entry under "${result.entryName}" already exists. ` +
      `Re-run with --force to overwrite, or pick a different --name.\n`,
    );
    return 1;
  }
  return 0;
}

export async function install(opts: InstallOptions = {}): Promise<InstallResult> {
  const name = opts.name ?? "ue-trace";
  const cwd = process.cwd();
  const configPath = opts.configPath
    ? (isAbsolute(opts.configPath) ? opts.configPath : resolve(cwd, opts.configPath))
    : resolve(cwd, ".mcp.json");

  const packageSpec = opts.pin
    ? `${PKG_NAME}@${await packageVersion()}`
    : PKG_NAME;

  const existing = await readConfig(configPath);
  const desired: McpServerEntry = {
    command: "npx",
    args: ["-y", packageSpec],
  };

  const mcpServers = { ...(existing.mcpServers ?? {}) };
  const existingEntry = mcpServers[name];

  let status: InstallStatus;
  if (!existingEntry) {
    mcpServers[name] = desired;
    status = opts.dryRun ? "would-write" : "added";
  } else if (entriesEqual(existingEntry, desired)) {
    status = "unchanged";
  } else if (opts.force) {
    mcpServers[name] = desired;
    status = opts.dryRun ? "would-write" : "updated";
  } else {
    // Keep the user's existing entry; flag the conflict.
    status = "exists-keep";
  }

  const finalConfig = { ...existing, mcpServers };

  if (!opts.dryRun && (status === "added" || status === "updated")) {
    await mkdir(dirname(configPath), { recursive: true });
    await writeFile(
      configPath,
      JSON.stringify(finalConfig, null, 2) + "\n",
      "utf8",
    );
  }

  return { status, configPath, entryName: name, packageSpec, finalConfig };
}

async function readConfig(path: string): Promise<McpConfigFile> {
  try {
    const raw = await readFile(path, "utf8");
    if (!raw.trim()) return {};
    const parsed = JSON.parse(raw) as McpConfigFile;
    if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
      throw new Error(`expected a JSON object at the root of ${path}`);
    }
    return parsed;
  } catch (e) {
    const err = e as NodeJS.ErrnoException;
    if (err.code === "ENOENT") return {};
    if (e instanceof SyntaxError) {
      throw new Error(`could not parse ${path} as JSON: ${e.message}`);
    }
    throw e;
  }
}

function entriesEqual(a: McpServerEntry, b: McpServerEntry): boolean {
  if (a.command !== b.command) return false;
  const aArgs = a.args ?? [];
  const bArgs = b.args ?? [];
  if (aArgs.length !== bArgs.length) return false;
  for (let i = 0; i < aArgs.length; i++) {
    if (aArgs[i] !== bArgs[i]) return false;
  }
  return true;
}

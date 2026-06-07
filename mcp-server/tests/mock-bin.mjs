#!/usr/bin/env node
// Mock TraceDigest binary: parses -mode= and -file= from argv, then prints a
// matching fixture JSON to stdout. Used by tests to exercise the spawn+parse
// path without depending on UE.

import { readFile, stat } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const __dirname = dirname(fileURLToPath(import.meta.url));
const fixturesDir = join(__dirname, "fixtures");

function getArg(name, argv) {
  const prefix = `-${name}=`;
  for (const a of argv) {
    if (a.startsWith(prefix)) return a.slice(prefix.length);
  }
  return undefined;
}

async function main() {
  const argv = process.argv.slice(2);
  const mode = getArg("mode", argv) ?? "digest";
  const file = getArg("file", argv) ?? "/fake/zombieproto.utrace";

  if (file.includes("MISSING")) {
    process.stderr.write(`TraceDigest: file not found: ${file}\n`);
    process.exit(2);
  }
  if (process.env.MOCK_BIN_FAIL === "1") {
    process.stderr.write("TraceDigest: simulated failure\n");
    process.exit(2);
  }
  if (process.env.MOCK_BIN_NOISY === "1") {
    // Simulate the engine startup banner that may sneak onto stdout, to
    // exercise extractTrailingJson's slow path.
    process.stdout.write("LogInit: Trace Insights stub\n");
    process.stdout.write("LogModuleManager: Loaded TraceServices\n");
  }

  // Resolve fixture path. For umbrella modes (gpu, counters, memory, …) we
  // try the more-specific `${mode}-${view}-sample.json` first so each view
  // can have its own canned output, falling back to the bare `${mode}-…`.
  // Counters has a special case: presence of `-counter=` implies the
  // "series" view; absence implies the "catalogue" view.
  const view = getArg("view", argv) ?? (mode === "counters" && getArg("counter", argv) ? "series" : mode === "counters" ? "catalogue" : undefined);
  const candidates = view
    ? [`${mode}-${view}-sample.json`, `${mode}-sample.json`]
    : [`${mode}-sample.json`];

  let payload;
  let fixturePath;
  for (const c of candidates) {
    try {
      fixturePath = join(fixturesDir, c);
      payload = await readFile(fixturePath, "utf8");
      break;
    } catch {
      /* try next */
    }
  }
  if (payload === undefined) {
    process.stderr.write(
      `mock-bin: no fixture for mode=${mode}${view ? ` view=${view}` : ""}; tried [${candidates.join(", ")}]\n`,
    );
    process.exit(2);
  }
  // Override the file field so the test can assert it tracked the argv.
  const obj = JSON.parse(payload);
  obj.file = file;
  if (mode === "compare") {
    const file2 = getArg("file2", argv);
    if (file2) obj.file2 = file2;
  }
  process.stdout.write(JSON.stringify(obj) + "\n");
}

main().catch((e) => {
  process.stderr.write(`mock-bin error: ${e.message}\n`);
  process.exit(1);
});

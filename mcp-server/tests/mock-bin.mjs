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

  const fixturePath = join(fixturesDir, `${mode}-sample.json`);
  let payload;
  try {
    payload = await readFile(fixturePath, "utf8");
  } catch (e) {
    process.stderr.write(`mock-bin: no fixture for mode=${mode}: ${e.message}\n`);
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

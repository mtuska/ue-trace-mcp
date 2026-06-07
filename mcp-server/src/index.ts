#!/usr/bin/env node
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";

import { createServer } from "./server.js";

async function main(): Promise<void> {
  const { server, daemons } = createServer({
    runOptions: {
      binary: process.env.TRACE_DIGEST_BIN,
    },
    cacheSize: Number(process.env.TRACE_CACHE_SIZE ?? 5),
  });

  // MCP shutdown reaper: when the client disconnects or we get a signal, kill
  // every daemon we spawned. Daemons also self-time-out as defense in depth,
  // but reaping eagerly here frees memory immediately and avoids orphan
  // processes when the user restarts Claude Code.
  let shuttingDown = false;
  const shutdown = async (reason: string): Promise<void> => {
    if (shuttingDown) return;
    shuttingDown = true;
    process.stderr.write(`ue-trace-mcp: shutting down (${reason})\n`);
    if (daemons) {
      try {
        await daemons.shutdown();
      } catch (e) {
        process.stderr.write(`ue-trace-mcp: reaper error: ${(e as Error).message}\n`);
      }
    }
  };

  for (const signal of ["SIGTERM", "SIGINT", "SIGHUP"] as const) {
    process.on(signal, () => {
      void shutdown(signal).then(() => process.exit(0));
    });
  }
  process.on("beforeExit", () => {
    void shutdown("beforeExit");
  });

  const transport = new StdioServerTransport();
  // When the MCP client closes stdin, the transport calls onclose. The MCP SDK
  // doesn't expose a direct hook for that, so we also hook stdin EOF.
  process.stdin.on("close", () => {
    void shutdown("stdin-close").then(() => process.exit(0));
  });

  await server.connect(transport);
}

main().catch((err) => {
  process.stderr.write(`ue-trace-mcp: fatal: ${(err as Error).message}\n`);
  process.exit(1);
});

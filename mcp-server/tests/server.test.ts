import { describe, expect, it } from "vitest";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { createServer } from "../src/server.js";
import type { Server } from "@modelcontextprotocol/sdk/server/index.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const MOCK_BIN = resolve(join(__dirname, "mock-bin.mjs"));
const FIXTURE = join(__dirname, "fixtures", "digest-sample.json");

interface InternalServer {
  // @ts-expect-error - private accessor
  _requestHandlers: Map<string, (req: unknown) => unknown>;
}

async function callHandler(server: Server, schema: { shape: { method: { value: string } } }, params: unknown) {
  const internal = server as unknown as InternalServer;
  const handler = internal._requestHandlers.get(schema.shape.method.value);
  if (!handler) throw new Error(`no handler for method ${schema.shape.method.value}`);
  return handler({ method: schema.shape.method.value, params });
}

describe("MCP server wiring", () => {
  it("lists every tool with a plain-object inputSchema (no top-level anyOf)", async () => {
    const { server } = createServer({ runOptions: { binary: MOCK_BIN }, enableDaemon: false });
    const res = (await callHandler(server, ListToolsRequestSchema, {})) as {
      tools: { name: string; inputSchema: Record<string, unknown> }[];
    };
    expect(res.tools.map((t) => t.name).sort()).toEqual([
      "trace_bookmark_list",
      "trace_callees",
      "trace_callers",
      "trace_channels",
      "trace_compare",
      "trace_counter_catalogue",
      "trace_counter_series",
      "trace_cpu_threads",
      "trace_digest",
      "trace_frame",
      "trace_frames",
      "trace_gpu_fences",
      "trace_gpu_queues",
      "trace_log_messages",
      "trace_memalloc_heaps",
      "trace_memalloc_query",
      "trace_memalloc_timeline",
      "trace_memory_samples",
      "trace_memory_tags",
      "trace_memory_trackers",
      "trace_overview",
      "trace_query",
      "trace_region_list",
      "trace_status",
      "trace_timeline",
      "trace_unload",
    ]);
    // Critically: each inputSchema must be a plain object schema. Clients
    // (Claude Code among them) silently drop tools with root-level anyOf.
    for (const t of res.tools) {
      expect(t.inputSchema.type).toBe("object");
      expect(t.inputSchema.anyOf).toBeUndefined();
      expect(t.inputSchema.oneOf).toBeUndefined();
    }
  });

  it("trace_digest: returns JSON content", async () => {
    const { server } = createServer({ runOptions: { binary: MOCK_BIN }, enableDaemon: false });
    const res = (await callHandler(server, CallToolRequestSchema, {
      name: "trace_digest",
      arguments: { file: FIXTURE, prefix: "Zombie_" },
    })) as { content: { type: string; text: string }[]; isError?: boolean };
    expect(res.isError).toBeFalsy();
    const payload = JSON.parse(res.content[0]!.text);
    expect(payload.mode).toBe("digest");
    expect(payload.events.length).toBeGreaterThan(0);
  });

  it("trace_compare: surfaces only-in-b regressions", async () => {
    const { server } = createServer({ runOptions: { binary: MOCK_BIN }, enableDaemon: false });
    const res = (await callHandler(server, CallToolRequestSchema, {
      name: "trace_compare",
      arguments: { fileA: FIXTURE, fileB: FIXTURE, prefix: "Zombie_" },
    })) as { content: { type: string; text: string }[]; isError?: boolean };
    expect(res.isError).toBeFalsy();
    const payload = JSON.parse(res.content[0]!.text);
    expect(payload.mode).toBe("compare");
    expect(payload.events[0].name).toBe("Zombie_WallSlideProbe");
  });

  it("invalid args return isError result, not a thrown exception", async () => {
    const { server } = createServer({ runOptions: { binary: MOCK_BIN }, enableDaemon: false });
    const res = (await callHandler(server, CallToolRequestSchema, {
      name: "trace_digest",
      arguments: { /* missing file */ },
    })) as { content: { type: string; text: string }[]; isError?: boolean };
    expect(res.isError).toBe(true);
    expect(res.content[0]!.text).toContain("invalid arguments");
  });

  it("unknown tool returns isError", async () => {
    const { server } = createServer({ runOptions: { binary: MOCK_BIN }, enableDaemon: false });
    const res = (await callHandler(server, CallToolRequestSchema, {
      name: "not-a-real-tool",
      arguments: {},
    })) as { content: { type: string; text: string }[]; isError?: boolean };
    expect(res.isError).toBe(true);
    expect(res.content[0]!.text).toContain("unknown tool");
  });
});

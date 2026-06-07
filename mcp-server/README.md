# mcp-server

TypeScript MCP server that exposes the `TraceDigest` UE binary as LLM-queryable
tools. Mirrors the action-keyed tool pattern (one outer tool `trace`, many
inner actions: `digest`, `timeline`, `frames`, `compare`).

## Setup

```bash
npm install
npm run build
```

Point the server at your built `TraceDigest` binary via `TRACE_DIGEST_BIN`:

```bash
export TRACE_DIGEST_BIN=/path/to/UE/Engine/Binaries/Linux/TraceDigest
npm start
```

## MCP client configuration

```jsonc
{
  "mcpServers": {
    "ue-trace": {
      "command": "node",
      "args": ["/path/to/ue-trace-mcp/mcp-server/dist/index.js"],
      "env": {
        "TRACE_DIGEST_BIN": "/path/to/UE/Engine/Binaries/Linux/TraceDigest"
      }
    }
  }
}
```

## Tool surface

One tool: `trace`. The `action` field discriminates:

```jsonc
// per-event aggregate stats
{ "action": "digest", "file": "/abs/foo.utrace", "prefix": "Zombie_", "limit": 200 }

// every instance of one event
{ "action": "timeline", "file": "/abs/foo.utrace", "event": "Zombie_StepLocomotion", "frameRange": [100, 200] }

// per-frame totals
{ "action": "frames", "file": "/abs/foo.utrace", "sortBy": "duration_ms", "limit": 50 }

// rank events by P95 delta across two traces
{ "action": "compare", "fileA": "/abs/a.utrace", "fileB": "/abs/b.utrace", "prefix": "Zombie_", "threshold": 0.05 }
```

## Caching

Parsed-trace JSON is cached in memory under `(absolute_path, mtime_ms, variant)`.
Repeated identical tool calls (same file, same args) skip the binary entirely.
Cache size: 5 by default; tune via `TRACE_CACHE_SIZE`.

When a `.utrace` file is overwritten (mtime bumps), all cache entries for that
file invalidate automatically on the next lookup.

## Dev

```bash
npm run dev          # tsx, hot-reload
npm test             # vitest, in-process, no UE required
npm run test:watch
```

Tests stub the binary with `tests/mock-bin.mjs` so they run anywhere Node 18+
is installed.

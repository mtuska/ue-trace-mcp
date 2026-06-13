import { describe, expect, it, beforeEach } from "vitest";
import { mkdtemp, readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";

import { install } from "../src/install.js";

async function tmp() {
  return await mkdtemp(join(tmpdir(), "ue-trace-mcp-install-"));
}

describe("install", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await tmp();
  });

  it("creates a fresh .mcp.json with the ue-trace entry", async () => {
    const configPath = join(dir, ".mcp.json");
    const r = await install({ configPath });

    expect(r.status).toBe("added");
    expect(r.entryName).toBe("ue-trace");
    expect(r.packageSpec).toBe("@mtuska/ue-trace-mcp");

    const written = JSON.parse(await readFile(configPath, "utf8"));
    expect(written.mcpServers["ue-trace"]).toEqual({
      command: "npx",
      args: ["-y", "@mtuska/ue-trace-mcp"],
    });
  });

  it("preserves unrelated entries when adding", async () => {
    const configPath = join(dir, ".mcp.json");
    await writeFile(
      configPath,
      JSON.stringify({
        mcpServers: {
          "other-mcp": { command: "node", args: ["/abs/other.js"] },
        },
      }),
      "utf8",
    );

    await install({ configPath });
    const written = JSON.parse(await readFile(configPath, "utf8"));
    expect(written.mcpServers["other-mcp"]).toEqual({
      command: "node",
      args: ["/abs/other.js"],
    });
    expect(written.mcpServers["ue-trace"].command).toBe("npx");
  });

  it("returns `unchanged` when the entry is already correct", async () => {
    const configPath = join(dir, ".mcp.json");
    await install({ configPath });  // first run adds it
    const r2 = await install({ configPath }); // second run is idempotent
    expect(r2.status).toBe("unchanged");
  });

  it("returns `exists-keep` without force when a different entry occupies the key", async () => {
    const configPath = join(dir, ".mcp.json");
    await writeFile(
      configPath,
      JSON.stringify({
        mcpServers: {
          "ue-trace": { command: "node", args: ["/abs/local/dist/index.js"] },
        },
      }),
      "utf8",
    );

    const r = await install({ configPath });
    expect(r.status).toBe("exists-keep");
    // File untouched.
    const written = JSON.parse(await readFile(configPath, "utf8"));
    expect(written.mcpServers["ue-trace"].command).toBe("node");
  });

  it("--force overwrites a conflicting entry", async () => {
    const configPath = join(dir, ".mcp.json");
    await writeFile(
      configPath,
      JSON.stringify({
        mcpServers: {
          "ue-trace": { command: "node", args: ["/abs/local/dist/index.js"] },
        },
      }),
      "utf8",
    );

    const r = await install({ configPath, force: true });
    expect(r.status).toBe("updated");
    const written = JSON.parse(await readFile(configPath, "utf8"));
    expect(written.mcpServers["ue-trace"]).toEqual({
      command: "npx",
      args: ["-y", "@mtuska/ue-trace-mcp"],
    });
  });

  it("--name lets the caller pick the mcpServers key", async () => {
    const configPath = join(dir, ".mcp.json");
    const r = await install({ configPath, name: "ue-trace-prod" });
    expect(r.entryName).toBe("ue-trace-prod");
    const written = JSON.parse(await readFile(configPath, "utf8"));
    expect(written.mcpServers["ue-trace-prod"]).toBeDefined();
    expect(written.mcpServers["ue-trace"]).toBeUndefined();
  });

  it("--dry-run does not write the file", async () => {
    const configPath = join(dir, ".mcp.json");
    const r = await install({ configPath, dryRun: true });
    expect(r.status).toBe("would-write");
    await expect(readFile(configPath, "utf8")).rejects.toThrow(/ENOENT/);
  });

  it("--pin appends the installer's own version to the package spec", async () => {
    const configPath = join(dir, ".mcp.json");
    const r = await install({ configPath, pin: true });
    expect(r.packageSpec).toMatch(/^@mtuska\/ue-trace-mcp@\d+\.\d+\.\d+/);
    const written = JSON.parse(await readFile(configPath, "utf8"));
    expect(written.mcpServers["ue-trace"].args[1]).toBe(r.packageSpec);
  });

  it("refuses an existing-but-malformed config with a clear error", async () => {
    const configPath = join(dir, ".mcp.json");
    await writeFile(configPath, "{ not valid json", "utf8");
    await expect(install({ configPath })).rejects.toThrow(/could not parse/);
  });
});

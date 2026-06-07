# ue-trace-mcp — build / link / test orchestration.
#
# Defaults assume the layout this repo was developed against. Override on the
# command line as needed:
#
#     make build-program UE_SOURCE=/elsewhere/UnrealEngine
#     make smoke         FILE=/path/to/foo.utrace

# ---------------------------------------------------------------------------
# Configurable paths
# ---------------------------------------------------------------------------

# Source-built engine. Used for the standalone Program target.
UE_SOURCE ?= /var/home/mtuska/Workspace/UnrealEngine

# ---------------------------------------------------------------------------
# Derived
# ---------------------------------------------------------------------------

REPO         := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PROGRAM_SRC  := $(REPO)/ue-program/TraceDigest
PROGRAM_BIN  := $(UE_SOURCE)/Engine/Binaries/Linux/TraceDigest
PROGRAM_LINK := $(UE_SOURCE)/Engine/Source/Programs/TraceDigest

# ---------------------------------------------------------------------------
# Phony targets
# ---------------------------------------------------------------------------

.PHONY: help build build-program build-mcp \
        link-program unlink-program \
        test smoke smoke-mcp smoke-daemon \
        clean clean-program clean-mcp \
        reap-daemons show

.DEFAULT_GOAL := help

help:
	@echo "ue-trace-mcp — Makefile targets:"
	@echo ""
	@echo "  build              build both program + mcp-server"
	@echo "  build-program      standalone TraceDigest binary (needs source engine)"
	@echo "  build-mcp          npm install + tsc for the MCP server"
	@echo ""
	@echo "  link-program       symlink ue-program into \$$UE_SOURCE/Engine/Source/Programs"
	@echo "  unlink-program     remove engine-side symlink"
	@echo ""
	@echo "  test               vitest suite (no UE required)"
	@echo "  smoke FILE=path    direct binary smoke test against a .utrace"
	@echo "  smoke-mcp FILE=p   end-to-end smoke through the MCP server"
	@echo "  smoke-daemon FILE=p  cold+warm timing through the daemon path"
	@echo "  reap-daemons       kill any orphaned daemons from prior MCP sessions"
	@echo ""
	@echo "  clean              clean MCP dist and UE intermediates"
	@echo "  show               print resolved paths and exit"
	@echo ""
	@echo "Variables (override with VAR=value):"
	@echo "  UE_SOURCE = $(UE_SOURCE)"

show:
	@echo "REPO         = $(REPO)"
	@echo "UE_SOURCE    = $(UE_SOURCE)"
	@echo "PROGRAM_SRC  = $(PROGRAM_SRC)"
	@echo "PROGRAM_BIN  = $(PROGRAM_BIN)"
	@echo "PROGRAM_LINK = $(PROGRAM_LINK)"

build: build-program build-mcp

# ---------------------------------------------------------------------------
# Program target (source engine)
# ---------------------------------------------------------------------------

# Idempotent: only creates the symlink if missing.
link-program:
	@test -e $(UE_SOURCE)/Engine/Source/Programs || \
		{ echo "ERR: $(UE_SOURCE) does not look like a UE source tree"; exit 1; }
	@if [ ! -L $(PROGRAM_LINK) ]; then \
		ln -s $(PROGRAM_SRC) $(PROGRAM_LINK); \
		echo "linked $(PROGRAM_LINK) -> $(PROGRAM_SRC)"; \
	else \
		echo "already linked: $(PROGRAM_LINK)"; \
	fi

unlink-program:
	@if [ -L $(PROGRAM_LINK) ]; then \
		rm $(PROGRAM_LINK); \
		echo "removed $(PROGRAM_LINK)"; \
	else \
		echo "no symlink at $(PROGRAM_LINK)"; \
	fi

build-program: link-program
	cd $(UE_SOURCE) && Engine/Build/BatchFiles/Linux/Build.sh TraceDigest Linux Development
	@test -x $(PROGRAM_BIN) && echo "OK $(PROGRAM_BIN)"

# ---------------------------------------------------------------------------
# MCP server
# ---------------------------------------------------------------------------

build-mcp:
	cd mcp-server && npm install && npm run build

test:
	cd mcp-server && npm test

# ---------------------------------------------------------------------------
# Smoke tests
# ---------------------------------------------------------------------------

smoke:
	@test -n "$(FILE)" || { echo "usage: make smoke FILE=/abs/path/to.utrace"; exit 1; }
	@test -x $(PROGRAM_BIN) || { echo "ERR: $(PROGRAM_BIN) not built; run 'make build-program'"; exit 1; }
	@$(PROGRAM_BIN) -file=$(FILE) -mode=overview -limit=5 -out=/tmp/td-smoke.json 2>/dev/null
	@python3 -m json.tool /tmp/td-smoke.json | head -30

# End-to-end through the MCP server's one-shot path (daemon disabled).
smoke-mcp: build-mcp
	@test -n "$(FILE)" || { echo "usage: make smoke-mcp FILE=/abs/path/to.utrace"; exit 1; }
	@test -x $(PROGRAM_BIN) || { echo "ERR: $(PROGRAM_BIN) not built"; exit 1; }
	@cd mcp-server && TRACE_DIGEST_BIN=$(PROGRAM_BIN) node --input-type=module -e "$$MCP_SMOKE"

# Cold + warm timing via the daemon path. Useful for measuring speedup.
smoke-daemon: build-mcp
	@test -n "$(FILE)" || { echo "usage: make smoke-daemon FILE=/abs/path/to.utrace"; exit 1; }
	@test -x $(PROGRAM_BIN) || { echo "ERR: $(PROGRAM_BIN) not built"; exit 1; }
	@cd mcp-server && TRACE_DIGEST_BIN=$(PROGRAM_BIN) node --input-type=module -e "$$DAEMON_SMOKE"

# Reap orphan daemons left by a crashed MCP. Safe to run at any time.
reap-daemons:
	@for pid in $$(cat /tmp/ue-trace-mcp-$$(id -u)/*.sock.pid 2>/dev/null); do \
		echo "killing daemon pid=$$pid"; kill -TERM $$pid 2>/dev/null || true; \
	done; \
	rm -rf /tmp/ue-trace-mcp-$$(id -u) 2>/dev/null; \
	echo "ok"

# Inline JS for smoke targets. Defined here so the cd ... && node block above
# stays readable; multi-line shell quoting inside `make` recipes is unforgiving.

define MCP_SMOKE
import { doOverview } from "./dist/tools.js";
import { TraceCache } from "./dist/cache.js";
const t0 = Date.now();
const r = await doOverview({ file: "$(FILE)", limit: 5 }, { cache: new TraceCache(1), runOptions: { binary: process.env.TRACE_DIGEST_BIN } });
console.log("elapsed:", Date.now() - t0, "ms");
console.log("frames=" + r.frame_count, "p95=" + r.frame_stats.p95_ms.toFixed(2) + "ms");
console.log("top:", r.events.map(e => e.name).join(", "));
endef
export MCP_SMOKE

define DAEMON_SMOKE
import { DaemonRegistry } from "./dist/daemon.js";
import { doOverview, doDigest, doStatus, doUnload } from "./dist/tools.js";
import { TraceCache } from "./dist/cache.js";
const reg = new DaemonRegistry({ binary: process.env.TRACE_DIGEST_BIN, idleTimeoutSec: 60, maxDaemons: 3 });
const ctx = { cache: new TraceCache(5), runOptions: { binary: process.env.TRACE_DIGEST_BIN, daemons: reg }, daemons: reg };
const t1 = Date.now();
const cold = await doOverview({ file: "$(FILE)", limit: 3 }, ctx);
console.log("cold (spawn+parse+query):", Date.now()-t1, "ms; top:", cold.events.map(e=>e.name).join(", "));
const t2 = Date.now();
await doDigest({ file: "$(FILE)", limit: 3 }, ctx);
console.log("warm digest:", Date.now()-t2, "ms");
const st = await doStatus({}, ctx);
console.log("status:", st.daemons.map(d=>({pid:d.pid,rss:d.rss_mb+"MiB",served:d.requests_served})), "system_free:", st.system.available_mb + "MiB");
await doUnload({ file: "$(FILE)" }, ctx);
await reg.shutdown();
endef
export DAEMON_SMOKE

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------

clean: clean-mcp clean-program

clean-mcp:
	rm -rf mcp-server/dist mcp-server/coverage

clean-program:
	rm -rf $(UE_SOURCE)/Engine/Intermediate/Build/Linux/TraceDigest

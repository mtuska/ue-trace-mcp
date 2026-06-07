# TraceDigest (Program target)

A standalone UE `Program` target — same target type as `UnrealPak`, `UnrealLightmass`, `BlankProgram`. Reads a `.utrace` file via the engine's `TraceServices` module and writes structured JSON to stdout or `-out=<file>`. No editor, no project plugin.

**Requires a source-built engine.** Binary/installed engines have a pre-compiled `UE5ProgramRules.dll` that can't pick up new Program targets — for those, use the [`ue-plugin/TraceDigest/`](../../ue-plugin/TraceDigest/) commandlet instead.

## What's here

```
ue-program/TraceDigest/
├── README.md
├── Source/
│   ├── TraceDigest.Target.cs        # Program type, modular link
│   └── TraceDigest/
│       ├── TraceDigest.Build.cs     # Core / CoreUObject / TraceServices / TraceAnalysis / Projects
│       └── Private/
│           ├── Main.cpp             # INT32_MAIN_INT32_ARGC_TCHAR_ARGV entry — this file is owned here
│           └── (symlinks to ../../../../ue-plugin/.../Private/)
│               Args.{h,cpp}, Session.{h,cpp}, Percentiles.h,
│               JsonOut.{h,cpp}, TimerLookup.h, Modes.h,
│               Modes/{Digest,Timeline,Frames,Compare,Overview,Frame,Butterfly,Threads}.cpp,
│               TraceDigestPCH.h
```

The shared sources live in `ue-plugin/`. Both the commandlet (plugin) and the Program target compile the same mode logic. Only the entry point differs:
- Plugin: `TraceDigestCommandlet::Main(FString)` invoked by `UnrealEditor-Cmd -run=TraceDigest`
- Program: `int main(argc, argv)` invoked directly as a standalone binary

## Build

```bash
# Symlink into the source engine (one time)
ln -s /abs/path/to/ue-trace-mcp/ue-program/TraceDigest \
      /abs/path/to/UnrealEngine/Engine/Source/Programs/TraceDigest

# Build
cd /abs/path/to/UnrealEngine
Engine/Build/BatchFiles/Linux/Build.sh TraceDigest Linux Development

# Binary lands at:
# /abs/path/to/UnrealEngine/Engine/Binaries/Linux/TraceDigest
```

## Run

```bash
$UE_ROOT/Engine/Binaries/Linux/TraceDigest \
  -file=/path/to/foo.utrace \
  -mode=digest \
  -prefix=Zombie_ \
  -limit=20
```

All flags match the commandlet path:

| `-mode=` | required extra args | output |
| --- | --- | --- |
| `digest` (default) | — | per-event aggregate stats |
| `overview` | — | composite: frame_stats + slowest_frames + top events |
| `timeline` | `-event=<name>` | every instance of one event |
| `frames` | — | per-frame totals |
| `frame` | `-frame=<idx>` | every event during one Game-thread frame |
| `callers` | `-event=<name>` | direct callers (butterfly) |
| `callees` | `-event=<name>` | direct callees (butterfly) |
| `threads` | — | per-thread CPU breakdown |
| `compare` | `-file2=<path>` | diff two traces, ranked by P95 delta |

Optional: `-prefix=`, `-limit=`, `-event=`, `-frame=`, `-framerange=A:B`, `-threshold=`, `-out=`.

## Performance vs the commandlet path

Real numbers against an 18,260-frame ZombieProto trace (~280MB `.utrace`):

| path | cold start | notes |
| --- | --- | --- |
| Program (this) | **~1.7s** | links only TraceServices + TraceAnalysis + Core/UObject |
| Plugin commandlet | ~10s | drags in the entire editor before the commandlet runs |

The Program path is the default for source-engine users and the answer if you're building this into CI or chaining many tool calls from an LLM.

## MCP wiring

Point the server at the Program binary via `TRACE_DIGEST_BIN`:

```jsonc
{
  "mcpServers": {
    "ue-trace": {
      "command": "node",
      "args": ["/abs/path/to/ue-trace-mcp/mcp-server/dist/index.js"],
      "env": {
        "TRACE_DIGEST_BIN": "/abs/path/to/UnrealEngine/Engine/Binaries/Linux/TraceDigest"
      }
    }
  }
}
```

The server auto-selects: `TRACE_DIGEST_BIN` if set, else falls back to `UE_EDITOR_CMD` + `UE_PROJECT` for the commandlet path. Both produce identical JSON.

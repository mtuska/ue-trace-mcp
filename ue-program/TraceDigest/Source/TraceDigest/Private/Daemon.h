// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#pragma once

#include "TraceDigestPCH.h"

namespace TraceDigest
{
struct FArgs;

// Long-lived daemon: load the trace once, then service queries over a Unix
// domain socket until idle timeout or SIGTERM. Returns the process exit code.
// POSIX-only; on non-POSIX platforms returns -1 immediately.
int RunDaemon(const FArgs& BootArgs);
}

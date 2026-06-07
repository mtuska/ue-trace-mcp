// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Daemon.h"

#include "Args.h"
#include "JsonOut.h"
#include "Modes.h"
#include "Session.h"

#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "TraceServices/Model/AnalysisSession.h"

DEFINE_LOG_CATEGORY_STATIC(LogTraceDaemon, Log, All);

#if PLATFORM_LINUX || PLATFORM_MAC
#	include <arpa/inet.h>
#	include <errno.h>
#	include <poll.h>
#	include <signal.h>
#	include <string.h>
#	include <sys/socket.h>
#	include <sys/stat.h>
#	include <sys/types.h>
#	include <sys/un.h>
#	include <unistd.h>
#endif

namespace TraceDigest
{

#if !(PLATFORM_LINUX || PLATFORM_MAC)
int RunDaemon(const FArgs&)
{
	UE_LOG(LogTraceDaemon, Error, TEXT("daemon mode only supported on POSIX platforms"));
	return -1;
}
#else

static volatile sig_atomic_t gShutdownRequested = 0;
static void OnSignal(int /*signo*/) { gShutdownRequested = 1; }

// Per-request I/O timeout. A slow / silent same-user attacker connecting to
// the socket would otherwise block the single-threaded accept loop forever
// (`ReadAll` retried EINTR unconditionally, so SIGTERM couldn't unblock it).
// 5 seconds is far longer than any healthy MCP client query takes; anything
// past that is treated as a hung peer and dropped.
static constexpr int kPerRequestTimeoutMs = 5000;

// poll(2) once on `fd` for POLLIN with the given remaining-budget. Returns:
//   1  — data ready
//   0  — timed out
//  -1  — error / shutdown requested (caller should bail)
static int PollOnceForRead(int fd, int TimeoutMs)
{
	pollfd pfd{};
	pfd.fd = fd;
	pfd.events = POLLIN;
	for (;;)
	{
		const int rc = ::poll(&pfd, 1, TimeoutMs);
		if (rc >= 0) return rc;
		if (errno == EINTR)
		{
			if (gShutdownRequested) return -1;
			continue;
		}
		return -1;
	}
}

// Read exactly `n` bytes from a connected socket within a wall-clock deadline.
// Returns the number of bytes read on success, 0 on clean EOF, -1 on timeout
// or shutdown or hard I/O error. The `poll()` wrapper makes us responsive to
// SIGTERM/SIGINT even while a peer is sending its bytes slowly.
static ssize_t ReadAllTimed(int fd, void* buf, size_t n, int TimeoutMs)
{
	char* p = static_cast<char*>(buf);
	size_t remaining = n;
	const double Deadline = FPlatformTime::Seconds() + (TimeoutMs / 1000.0);
	while (remaining > 0)
	{
		const int RemainingMs = static_cast<int>(
			FMath::Max(0.0, (Deadline - FPlatformTime::Seconds()) * 1000.0));
		const int Ready = PollOnceForRead(fd, RemainingMs);
		if (Ready <= 0) return -1; // 0 = timeout, -1 = error/shutdown

		const ssize_t r = ::read(fd, p, remaining);
		if (r < 0)
		{
			if (errno == EINTR)
			{
				if (gShutdownRequested) return -1;
				continue;
			}
			return -1;
		}
		if (r == 0) return 0; // peer closed mid-frame
		remaining -= r;
		p += r;
	}
	return n;
}

static ssize_t WriteAll(int fd, const void* buf, size_t n)
{
	const char* p = static_cast<const char*>(buf);
	size_t remaining = n;
	while (remaining > 0)
	{
		const ssize_t w = ::write(fd, p, remaining);
		if (w < 0)
		{
			if (errno == EINTR)
			{
				if (gShutdownRequested) return -1;
				continue;
			}
			return -1;
		}
		remaining -= w;
		p += w;
	}
	return n;
}

static bool RecvFramed(int fd, FString& Out, uint32 MaxLen = 1u * 1024u * 1024u)
{
	uint32 LenBE = 0;
	if (ReadAllTimed(fd, &LenBE, 4, kPerRequestTimeoutMs) != 4) return false;
	const uint32 Len = ntohl(LenBE);
	if (Len > MaxLen) return false;
	if (Len == 0) { Out.Empty(); return true; }

	TArray<char> Buf;
	Buf.SetNumUninitialized(static_cast<int32>(Len) + 1);
	if (ReadAllTimed(fd, Buf.GetData(), Len, kPerRequestTimeoutMs) != static_cast<ssize_t>(Len))
	{
		return false;
	}
	Buf[Len] = '\0';
	Out = FString(UTF8_TO_TCHAR(Buf.GetData()));
	return true;
}

static bool SendFramed(int fd, const FString& Payload)
{
	const FTCHARToUTF8 Utf8(*Payload);
	const uint32 Len = static_cast<uint32>(Utf8.Length());
	const uint32 LenBE = htonl(Len);
	if (WriteAll(fd, &LenBE, 4) != 4) return false;
	if (Len > 0 && WriteAll(fd, Utf8.Get(), Len) != static_cast<ssize_t>(Len)) return false;
	return true;
}

// Minimal JSON string escape for inline-in-response paths. We trust the mode
// output to be well-formed JSON already; this is only used for error/status
// strings we compose ourselves.
static FString EscapeJsonString(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len());
	for (TCHAR c : In)
	{
		switch (c)
		{
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\n'): Out += TEXT("\\n");  break;
			case TEXT('\r'): Out += TEXT("\\r");  break;
			case TEXT('\t'): Out += TEXT("\\t");  break;
			default:
				if (c < 0x20) Out += FString::Printf(TEXT("\\u%04x"), static_cast<int>(c));
				else          Out.AppendChar(c);
		}
	}
	return Out;
}

static FString BuildOkResponse(const FString& EmbeddedDataJson)
{
	return TEXT("{\"ok\":true,\"data\":") + EmbeddedDataJson + TEXT("}");
}

static FString BuildErrResponse(const FString& Message)
{
	return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *EscapeJsonString(Message));
}

struct FDaemonState
{
	FLoadedTrace Trace;
	FString BootFile;
	double  StartedAt    = 0.0;
	double  LastUsedAt   = 0.0;
	double  LoadElapsedMs = 0.0;
	uint64  RequestsServed = 0;
};

static FString HandleStatus(const FDaemonState& State)
{
	const double Now = FPlatformTime::Seconds();
	return FString::Printf(
		TEXT("{\"file\":\"%s\",\"uptime_ms\":%.3f,\"idle_ms\":%.3f,\"load_ms\":%.3f,\"requests_served\":%llu}"),
		*EscapeJsonString(State.BootFile),
		(Now - State.StartedAt) * 1000.0,
		(Now - State.LastUsedAt) * 1000.0,
		State.LoadElapsedMs,
		static_cast<unsigned long long>(State.RequestsServed));
}

// Run one query against the cached session. Returns a complete response payload.
static FString HandleQuery(FDaemonState& State, const FString& CmdLine)
{
	// FArgs::Parse rejects requests without -file= because one-shot mode
	// needs it. The daemon already knows the file, so we pre-pend it so the
	// validator is happy. If the client also passes -file= it gets overwritten
	// by ours; we treat the daemon's bound file as authoritative.
	const FString FullCmdLine = FString::Printf(TEXT("-file=\"%s\" "), *State.BootFile) + CmdLine;

	FArgs Args;
	FString ParseErr;
	if (!Args.Parse(*FullCmdLine, ParseErr))
	{
		return BuildErrResponse(ParseErr);
	}
	Args.File = State.BootFile; // belt-and-braces: client cannot override

	const TraceServices::IAnalysisSession& Session = *State.Trace.GetSession();
	FJsonOut Json;

	switch (Args.Mode)
	{
		case EMode::Digest:   Modes::RunDigest  (Session, Args, Json); break;
		case EMode::Timeline: Modes::RunTimeline(Session, Args, Json); break;
		case EMode::Frames:   Modes::RunFrames  (Session, Args, Json); break;
		case EMode::Overview: Modes::RunOverview(Session, Args, Json); break;
		case EMode::Frame:    Modes::RunFrame   (Session, Args, Json); break;
		case EMode::Callers:  Modes::RunCallers (Session, Args, Json); break;
		case EMode::Callees:  Modes::RunCallees (Session, Args, Json); break;
		case EMode::Threads:   Modes::RunThreads  (Session, Args, Json); break;
		case EMode::Channels:  Modes::RunChannels (Session, Args, Json); break;
		case EMode::Gpu:       Modes::RunGpu      (Session, Args, Json); break;
		case EMode::Counters:  Modes::RunCounters (Session, Args, Json); break;
		case EMode::Bookmarks: Modes::RunBookmarks(Session, Args, Json); break;
		case EMode::Regions:   Modes::RunRegions  (Session, Args, Json); break;
		case EMode::Logs:      Modes::RunLogs     (Session, Args, Json); break;
		case EMode::Memory:    Modes::RunMemory     (Session, Args, Json); break;
		case EMode::Allocations: Modes::RunAllocations(Session, Args, Json); break;
		case EMode::Query:       Modes::RunQuery      (Session, Args, Json); break;
		case EMode::Compare:
		{
			// Refused in daemon mode: -file2= would be a client-controlled,
			// arbitrary-filesystem-path open inside this long-lived process,
			// breaking the daemon's "trace is fixed at boot" invariant and
			// giving a same-user attacker who reached the socket a way to make
			// us OpenRead anything the daemon user can read. Compare is only
			// available via the one-shot path (where the launcher controls
			// both file arguments).
			return BuildErrResponse(
				TEXT("compare is not available in daemon mode; "
				     "invoke TraceDigest one-shot with -mode=compare -file=… -file2=…"));
		}
	}

	return BuildOkResponse(Json.ToString());
}

int RunDaemon(const FArgs& BootArgs)
{
	if (BootArgs.SocketPath.IsEmpty())
	{
		UE_LOG(LogTraceDaemon, Error, TEXT("missing -socket=<path>"));
		return 1;
	}

	FDaemonState State;
	State.BootFile  = BootArgs.File;
	State.StartedAt = FPlatformTime::Seconds();

	// Parse the trace once. This is the expensive call; everything afterwards
	// reuses the session.
	{
		const double T0 = FPlatformTime::Seconds();
		FString LoadErr;
		if (!State.Trace.LoadEx(BootArgs.File, BootArgs.bDisableCache, LoadErr))
		{
			UE_LOG(LogTraceDaemon, Error, TEXT("load: %s"), *LoadErr);
			return 2;
		}
		State.LoadElapsedMs = (FPlatformTime::Seconds() - T0) * 1000.0;
	}
	State.LastUsedAt = FPlatformTime::Seconds();

	// Bind a Unix domain socket at -socket=<path>. Cleanup any stale entry
	// from a prior crashed daemon before binding.
	const FTCHARToUTF8 SockPathUtf8(*BootArgs.SocketPath);
	if (SockPathUtf8.Length() >= static_cast<int32>(sizeof(sockaddr_un().sun_path)) - 1)
	{
		UE_LOG(LogTraceDaemon, Error, TEXT("socket path too long for AF_UNIX (max ~107 chars)"));
		return 3;
	}
	::unlink(SockPathUtf8.Get());

	const int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv < 0)
	{
		UE_LOG(LogTraceDaemon, Error, TEXT("socket(): %s"), UTF8_TO_TCHAR(strerror(errno)));
		return 4;
	}

	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SockPathUtf8.Get(), sizeof(addr.sun_path) - 1);
	if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
	{
		UE_LOG(LogTraceDaemon, Error, TEXT("bind %s: %s"), *BootArgs.SocketPath, UTF8_TO_TCHAR(strerror(errno)));
		::close(srv);
		return 5;
	}
	::chmod(SockPathUtf8.Get(), 0600);
	if (::listen(srv, 4) < 0)
	{
		UE_LOG(LogTraceDaemon, Error, TEXT("listen: %s"), UTF8_TO_TCHAR(strerror(errno)));
		::close(srv);
		::unlink(SockPathUtf8.Get());
		return 6;
	}

	const FString PidPath = BootArgs.SocketPath + TEXT(".pid");
	FFileHelper::SaveStringToFile(FString::Printf(TEXT("%d\n"), static_cast<int>(getpid())), *PidPath);

	::signal(SIGTERM, OnSignal);
	::signal(SIGINT,  OnSignal);
	::signal(SIGPIPE, SIG_IGN);

	UE_LOG(LogTraceDaemon, Display,
		TEXT("daemon ready trace=%s socket=%s load_ms=%.1f pid=%d idle_timeout_sec=%d"),
		*BootArgs.File, *BootArgs.SocketPath, State.LoadElapsedMs, static_cast<int>(getpid()),
		BootArgs.IdleTimeoutSec);

	// Tell the spawner we're listening. The MCP-side waits for this token on
	// stdout before it considers the daemon up.
	{
		static const char kReady[] = "DAEMON_READY\n";
		const ssize_t _ = ::write(STDOUT_FILENO, kReady, sizeof(kReady) - 1);
		(void)_;
	}

	while (!gShutdownRequested)
	{
		const double Now       = FPlatformTime::Seconds();
		const double IdleFor   = Now - State.LastUsedAt;
		const double Remaining = static_cast<double>(BootArgs.IdleTimeoutSec) - IdleFor;
		if (Remaining <= 0.0)
		{
			UE_LOG(LogTraceDaemon, Display, TEXT("idle timeout (%ds) — exiting"), BootArgs.IdleTimeoutSec);
			break;
		}

		pollfd pfd{};
		pfd.fd = srv;
		pfd.events = POLLIN;
		const int TimeoutMs = static_cast<int>(FMath::Min<double>(Remaining * 1000.0, 60000.0)) + 1;
		const int rc = ::poll(&pfd, 1, TimeoutMs);
		if (rc < 0)
		{
			if (errno == EINTR) continue;
			UE_LOG(LogTraceDaemon, Error, TEXT("poll: %s"), UTF8_TO_TCHAR(strerror(errno)));
			break;
		}
		if (rc == 0) continue; // wake to re-check idle

		const int conn = ::accept(srv, nullptr, nullptr);
		if (conn < 0)
		{
			if (errno == EINTR) continue;
			UE_LOG(LogTraceDaemon, Warning, TEXT("accept: %s"), UTF8_TO_TCHAR(strerror(errno)));
			continue;
		}

		FString Request;
		if (!RecvFramed(conn, Request))
		{
			::close(conn);
			continue;
		}

		FString Response;
		bool bExitAfter = false;
		if (Request.StartsWith(TEXT(":")))
		{
			if (Request == TEXT(":ping"))
			{
				Response = BuildOkResponse(TEXT("\"pong\""));
			}
			else if (Request == TEXT(":status"))
			{
				Response = BuildOkResponse(HandleStatus(State));
			}
			else if (Request == TEXT(":shutdown"))
			{
				Response = BuildOkResponse(TEXT("\"shutting down\""));
				bExitAfter = true;
			}
			else
			{
				Response = BuildErrResponse(FString::Printf(TEXT("unknown control: %s"), *Request));
			}
		}
		else
		{
			Response = HandleQuery(State, Request);
			State.RequestsServed += 1;
		}

		SendFramed(conn, Response);
		::close(conn);

		State.LastUsedAt = FPlatformTime::Seconds();
		if (bExitAfter) break;
	}

	UE_LOG(LogTraceDaemon, Display, TEXT("daemon exiting cleanly"));
	::close(srv);
	::unlink(SockPathUtf8.Get());
	{
		const FTCHARToUTF8 PidPathUtf8(*PidPath);
		::unlink(PidPathUtf8.Get());
	}
	return 0;
}

#endif // POSIX

} // namespace TraceDigest

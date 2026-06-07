// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"

#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Modules.h"

#include <atomic>  // std::memory_order_acquire — Modules.h pulls in <atomic> transitively, but make the dependency explicit so a future Modules.h refactor can't break Windows builds silently.

using namespace TraceServices;

namespace TraceDigest
{

// List every discovered module with its load status + symbol-resolution
// stats. Output is small (~10–100 rows on a typical trace); no windowing
// or limit needed. Pair with trace_callstack — when a callstack frame's
// status is "not_loaded" / "version_mismatch", checking trace_modules
// reveals whether the parent module was even loaded successfully.

static const TCHAR* ModuleStatusName(EModuleStatus S)
{
	switch (S)
	{
		case EModuleStatus::Discovered:      return TEXT("discovered");
		case EModuleStatus::Pending:         return TEXT("pending");
		case EModuleStatus::Loaded:          return TEXT("loaded");
		case EModuleStatus::VersionMismatch: return TEXT("version_mismatch");
		case EModuleStatus::NotFound:        return TEXT("not_found");
		case EModuleStatus::Failed:          return TEXT("failed");
		default:                             return TEXT("unknown");
	}
}

void Modes::RunModules(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const IModuleProvider* Provider = ReadModuleProvider(Session);

	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));

	if (!Provider)
	{
		Json.KeyInt(TEXT("module_count"), 0);
		Json.Key(TEXT("modules")); Json.BeginArray(); Json.EndArray();
		Json.EndObject();
		return;
	}

	Json.KeyInt (TEXT("module_count"), static_cast<int64>(Provider->GetNumModules()));
	Json.KeyBool(TEXT("finished_resolving"), Provider->HasFinishedResolving());

	IModuleProvider::FStats Stats{};
	Provider->GetStats(&Stats);
	Json.Key(TEXT("totals"));
	Json.BeginObject();
	Json.KeyInt(TEXT("modules_discovered"), static_cast<int64>(Stats.ModulesDiscovered));
	Json.KeyInt(TEXT("modules_loaded"),     static_cast<int64>(Stats.ModulesLoaded));
	Json.KeyInt(TEXT("modules_failed"),     static_cast<int64>(Stats.ModulesFailed));
	Json.KeyInt(TEXT("symbols_discovered"),static_cast<int64>(Stats.SymbolsDiscovered));
	Json.KeyInt(TEXT("symbols_cached"),    static_cast<int64>(Stats.SymbolsCached));
	Json.KeyInt(TEXT("symbols_resolved"),  static_cast<int64>(Stats.SymbolsResolved));
	Json.KeyInt(TEXT("symbols_failed"),    static_cast<int64>(Stats.SymbolsFailed));
	Json.EndObject();

	Json.Key(TEXT("modules"));
	Json.BeginArray();
	Provider->EnumerateModules(0, [&](const FModule& M)
	{
		Json.BeginObject();
		Json.KeyStr(TEXT("name"),      M.Name ? FString(M.Name) : FString());
		Json.KeyStr(TEXT("full_name"), M.FullName ? FString(M.FullName) : FString());
		Json.KeyInt(TEXT("base"),      static_cast<int64>(M.Base));
		Json.KeyInt(TEXT("size"),      static_cast<int64>(M.Size));
		Json.KeyBool(TEXT("unloaded"), M.bIsUnloaded.load(std::memory_order_acquire));
		Json.KeyStr(TEXT("status"),
			FString(ModuleStatusName(M.Status.load(std::memory_order_acquire))));
		Json.KeyStr(TEXT("status_message"),
			M.StatusMessage ? FString(M.StatusMessage) : FString());

		Json.Key(TEXT("symbol_stats"));
		Json.BeginObject();
		Json.KeyInt(TEXT("discovered"), static_cast<int64>(M.Stats.Discovered.load(std::memory_order_acquire)));
		Json.KeyInt(TEXT("cached"),     static_cast<int64>(M.Stats.Cached.load(std::memory_order_acquire)));
		Json.KeyInt(TEXT("resolved"),   static_cast<int64>(M.Stats.Resolved.load(std::memory_order_acquire)));
		Json.KeyInt(TEXT("failed"),     static_cast<int64>(M.Stats.Failed.load(std::memory_order_acquire)));
		Json.KeyInt(TEXT("available"),  static_cast<int64>(M.Stats.Available.load(std::memory_order_acquire)));
		Json.EndObject();

		Json.EndObject();
	});
	Json.EndArray();
	Json.EndObject();
}

} // namespace TraceDigest

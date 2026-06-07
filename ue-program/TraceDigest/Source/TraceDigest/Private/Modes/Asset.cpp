// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"

#include "ProfilingDebugging/MiscTrace.h"
#include "Templates/UniquePtr.h"
#include "TraceServices/Containers/Tables.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/LoadTimeProfiler.h"

using namespace TraceServices;

namespace TraceDigest
{

// ILoadTimeProfilerProvider exposes three pre-aggregated `ITable<…>`
// factory methods (CreatePackageDetails / CreateExportDetails /
// CreateRequests). Each call returns a freshly-materialised table that
// the caller owns — we wrap it in TUniquePtr so the deletion is
// exception-safe. Cardinality is small (10²–10⁴ rows) so no further
// downsampling.
//
// One C++ mode file backs three TS-side tools (trace_asset_packages /
// trace_asset_requests / trace_asset_exports) via the `-view=` umbrella.

static const TCHAR* EventTypeName(ELoadTimeProfilerObjectEventType T)
{
	switch (T)
	{
		case LoadTimeProfilerObjectEventType_Create:    return TEXT("create");
		case LoadTimeProfilerObjectEventType_Serialize: return TEXT("serialize");
		case LoadTimeProfilerObjectEventType_PostLoad:  return TEXT("postload");
		case LoadTimeProfilerObjectEventType_None:      return TEXT("none");
		default:                                        return TEXT("unknown");
	}
}

// Resolve frame-range window to seconds; default = full trace.
static void ResolveWindow(const IAnalysisSession& Session, const FArgs& Args,
                          double& OutStartSec, double& OutEndSec)
{
	OutStartSec = 0.0;
	OutEndSec   = Session.GetDurationSeconds();
	if (Args.FrameRangeStart < 0 || Args.FrameRangeEnd <= Args.FrameRangeStart) return;
	const IFrameProvider& Frames = ReadFrameProvider(Session);
	if (const TraceServices::FFrame* F0 = Frames.GetFrame(TraceFrameType_Game, Args.FrameRangeStart))
		OutStartSec = F0->StartTime;
	if (const TraceServices::FFrame* F1 = Frames.GetFrame(TraceFrameType_Game, Args.FrameRangeEnd - 1))
		OutEndSec = F1->EndTime;
}

static void EmitPackages(const ILoadTimeProfilerProvider& Provider,
                         double StartSec, double EndSec, int32 Limit, FJsonOut& Json)
{
	TUniquePtr<ITable<FPackagesTableRow>> Table(
		Provider.CreatePackageDetailsTable(StartSec, EndSec));

	int32 Total = 0;
	int32 Emitted = 0;
	Json.KeyInt(TEXT("total_in_window"), Table ? static_cast<int64>(Table->GetRowCount()) : 0);

	Json.Key(TEXT("events"));
	Json.BeginArray();
	if (Table)
	{
		TUniquePtr<ITableReader<FPackagesTableRow>> Reader(Table->CreateReader());
		while (Reader && Reader->IsValid())
		{
			++Total;
			if (Emitted >= Limit) { Reader->NextRow(); continue; }
			const FPackagesTableRow* Row = Reader->GetCurrentRow();
			if (!Row || !Row->PackageInfo) { Reader->NextRow(); continue; }
			const FPackageInfo* P = Row->PackageInfo;

			Json.BeginObject();
			Json.KeyInt(TEXT("id"),                   static_cast<int64>(P->Id));
			Json.KeyStr(TEXT("name"),                 P->Name ? FString(P->Name) : FString());
			Json.KeyInt(TEXT("total_serialized_size"),     static_cast<int64>(Row->TotalSerializedSize));
			Json.KeyInt(TEXT("serialized_header_size"),    static_cast<int64>(Row->SerializedHeaderSize));
			Json.KeyInt(TEXT("serialized_exports_count"),  static_cast<int64>(Row->SerializedExportsCount));
			Json.KeyInt(TEXT("serialized_exports_size"),   static_cast<int64>(Row->SerializedExportsSize));
			Json.KeyNum(TEXT("main_thread_ms"),       Row->MainThreadTime         * 1000.0);
			Json.KeyNum(TEXT("async_loading_ms"),     Row->AsyncLoadingThreadTime * 1000.0);

			// Lightweight summary so the LLM doesn't need a second call for
			// per-package shape.
			Json.Key(TEXT("summary"));
			Json.BeginObject();
			Json.KeyInt(TEXT("total_header_size"), static_cast<int64>(P->Summary.TotalHeaderSize));
			Json.KeyInt(TEXT("import_count"),      static_cast<int64>(P->Summary.ImportCount));
			Json.KeyInt(TEXT("export_count"),      static_cast<int64>(P->Summary.ExportCount));
			Json.KeyInt(TEXT("priority"),          static_cast<int64>(P->Summary.Priority));
			Json.EndObject();

			Json.KeyInt(TEXT("imported_packages_count"), static_cast<int64>(P->ImportedPackages.Num()));
			Json.KeyInt(TEXT("request_id"),              static_cast<int64>(P->RequestId));
			Json.EndObject();
			++Emitted;
			Reader->NextRow();
		}
	}
	Json.EndArray();
	Json.KeyBool(TEXT("truncated"), Emitted < Total);
}

static void EmitRequests(const ILoadTimeProfilerProvider& Provider,
                         double StartSec, double EndSec, int32 Limit, FJsonOut& Json)
{
	TUniquePtr<ITable<FRequestsTableRow>> Table(
		Provider.CreateRequestsTable(StartSec, EndSec));

	int32 Total = 0;
	int32 Emitted = 0;
	Json.KeyInt(TEXT("total_in_window"), Table ? static_cast<int64>(Table->GetRowCount()) : 0);

	Json.Key(TEXT("events"));
	Json.BeginArray();
	if (Table)
	{
		TUniquePtr<ITableReader<FRequestsTableRow>> Reader(Table->CreateReader());
		while (Reader && Reader->IsValid())
		{
			++Total;
			if (Emitted >= Limit) { Reader->NextRow(); continue; }
			const FRequestsTableRow* Row = Reader->GetCurrentRow();
			if (!Row) { Reader->NextRow(); continue; }

			Json.BeginObject();
			Json.KeyInt(TEXT("id"),            static_cast<int64>(Row->Id));
			Json.KeyStr(TEXT("name"),          Row->Name ? FString(Row->Name) : FString());
			Json.KeyNum(TEXT("start_ms"),      Row->StartTime * 1000.0);
			Json.KeyNum(TEXT("duration_ms"),   Row->Duration  * 1000.0);
			Json.KeyInt(TEXT("package_count"), static_cast<int64>(Row->Packages.Num()));
			Json.EndObject();
			++Emitted;
			Reader->NextRow();
		}
	}
	Json.EndArray();
	Json.KeyBool(TEXT("truncated"), Emitted < Total);
}

static void EmitExports(const ILoadTimeProfilerProvider& Provider,
                        double StartSec, double EndSec, int32 Limit, FJsonOut& Json)
{
	TUniquePtr<ITable<FExportsTableRow>> Table(
		Provider.CreateExportDetailsTable(StartSec, EndSec));

	int32 Total = 0;
	int32 Emitted = 0;
	Json.KeyInt(TEXT("total_in_window"), Table ? static_cast<int64>(Table->GetRowCount()) : 0);

	Json.Key(TEXT("events"));
	Json.BeginArray();
	if (Table)
	{
		TUniquePtr<ITableReader<FExportsTableRow>> Reader(Table->CreateReader());
		while (Reader && Reader->IsValid())
		{
			++Total;
			if (Emitted >= Limit) { Reader->NextRow(); continue; }
			const FExportsTableRow* Row = Reader->GetCurrentRow();
			if (!Row || !Row->ExportInfo) { Reader->NextRow(); continue; }
			const FPackageExportInfo* E = Row->ExportInfo;

			Json.BeginObject();
			Json.KeyInt(TEXT("id"),               static_cast<int64>(E->Id));
			Json.KeyStr(TEXT("class"),            (E->Class && E->Class->Name) ? FString(E->Class->Name) : FString());
			Json.KeyStr(TEXT("package_name"),     (E->Package && E->Package->Name) ? FString(E->Package->Name) : FString());
			Json.KeyInt(TEXT("package_id"),       E->Package ? static_cast<int64>(E->Package->Id) : 0);
			Json.KeyInt(TEXT("serialized_size"),  static_cast<int64>(Row->SerializedSize));
			Json.KeyNum(TEXT("main_thread_ms"),   Row->MainThreadTime * 1000.0);
			Json.KeyNum(TEXT("async_loading_ms"), Row->AsyncLoadingThreadTime * 1000.0);
			Json.KeyStr(TEXT("event_type"),       FString(EventTypeName(Row->EventType)));
			Json.EndObject();
			++Emitted;
			Reader->NextRow();
		}
	}
	Json.EndArray();
	Json.KeyBool(TEXT("truncated"), Emitted < Total);
}

void Modes::RunAsset(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const ILoadTimeProfilerProvider* Provider = ReadLoadTimeProfilerProvider(Session);

	double StartSec = 0.0, EndSec = 0.0;
	ResolveWindow(Session, Args, StartSec, EndSec);
	const int32 Limit = Args.Limit > 0 ? Args.Limit : 200;
	const FString View = Args.View.IsEmpty() ? FString(TEXT("packages")) : Args.View;

	Json.BeginObject();
	Json.KeyStr(TEXT("file"),  Args.File);
	Json.KeyStr(TEXT("mode"),  FArgs::ModeName(Args.Mode));
	Json.KeyStr(TEXT("view"),  View);
	Json.KeyNum(TEXT("window_start_ms"), StartSec * 1000.0);
	Json.KeyNum(TEXT("window_end_ms"),   EndSec   * 1000.0);

	if (!Provider)
	{
		Json.KeyBool(TEXT("has_load_time_data"), false);
		Json.KeyInt (TEXT("total_in_window"),    0);
		Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray();
		Json.KeyBool(TEXT("truncated"), false);
		Json.EndObject();
		return;
	}
	Json.KeyBool(TEXT("has_load_time_data"), true);

	if (View.Equals(TEXT("requests"), ESearchCase::IgnoreCase))
	{
		EmitRequests(*Provider, StartSec, EndSec, Limit, Json);
	}
	else if (View.Equals(TEXT("exports"), ESearchCase::IgnoreCase))
	{
		EmitExports(*Provider, StartSec, EndSec, Limit, Json);
	}
	else  // default: packages
	{
		EmitPackages(*Provider, StartSec, EndSec, Limit, Json);
	}

	Json.EndObject();
}

} // namespace TraceDigest

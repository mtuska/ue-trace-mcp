// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"

#include "ProfilingDebugging/MiscTrace.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/TasksProfiler.h"

using namespace TraceServices;

namespace TraceDigest
{

// ITasksProvider exposes UE's task graph: every TGraphTask + its timestamps
// (Created/Launched/Scheduled/Started/Finished/Completed/Destroyed) and the
// Prerequisites/Subsequents/ParentTasks/NestedTasks dependency arrays.
//
// Two tools share this file:
//   * trace_task_list — windowed enumeration with state filter, capped at
//     -limit (default 500). Production traces have 10^6-10^7 tasks — full
//     enumeration without windowing is infeasible.
//   * trace_task_drill — full FTaskInfo for one task id, plus its four
//     relation arrays.

static ETaskEnumerationOption ParseEnumOption(const FString& In)
{
	if (In.IsEmpty()) return ETaskEnumerationOption::Alive;
	if (In.Equals(TEXT("Alive"),                  ESearchCase::IgnoreCase)) return ETaskEnumerationOption::Alive;
	if (In.Equals(TEXT("Launched"),               ESearchCase::IgnoreCase)) return ETaskEnumerationOption::Launched;
	if (In.Equals(TEXT("Active"),                 ESearchCase::IgnoreCase)) return ETaskEnumerationOption::Active;
	if (In.Equals(TEXT("WaitingForPrerequisites"),ESearchCase::IgnoreCase)) return ETaskEnumerationOption::WaitingForPrerequisites;
	if (In.Equals(TEXT("Queued"),                 ESearchCase::IgnoreCase)) return ETaskEnumerationOption::Queued;
	if (In.Equals(TEXT("Executing"),              ESearchCase::IgnoreCase)) return ETaskEnumerationOption::Executing;
	if (In.Equals(TEXT("WaitingForNested"),       ESearchCase::IgnoreCase)) return ETaskEnumerationOption::WaitingForNested;
	if (In.Equals(TEXT("Completed"),              ESearchCase::IgnoreCase)) return ETaskEnumerationOption::Completed;
	return ETaskEnumerationOption::Alive;
}

static const TCHAR* EnumOptionName(ETaskEnumerationOption O)
{
	switch (O)
	{
		case ETaskEnumerationOption::Alive:                   return TEXT("Alive");
		case ETaskEnumerationOption::Launched:                return TEXT("Launched");
		case ETaskEnumerationOption::Active:                  return TEXT("Active");
		case ETaskEnumerationOption::WaitingForPrerequisites: return TEXT("WaitingForPrerequisites");
		case ETaskEnumerationOption::Queued:                  return TEXT("Queued");
		case ETaskEnumerationOption::Executing:               return TEXT("Executing");
		case ETaskEnumerationOption::WaitingForNested:        return TEXT("WaitingForNested");
		case ETaskEnumerationOption::Completed:               return TEXT("Completed");
		default:                                              return TEXT("unknown");
	}
}

// Resolve [-framerange=A:B] to seconds via IFrameProvider, just like the
// other modes that accept frame windows. Default = full trace.
static void ResolveTimeWindow(const IAnalysisSession& Session, const FArgs& Args,
                              double& OutStartSec, double& OutEndSec)
{
	OutStartSec = 0.0;
	OutEndSec   = Session.GetDurationSeconds();
	if (Args.FrameRangeStart < 0 || Args.FrameRangeEnd <= Args.FrameRangeStart) return;

	const IFrameProvider& Frames = ReadFrameProvider(Session);
	if (const TraceServices::FFrame* F0 = Frames.GetFrame(TraceFrameType_Game, Args.FrameRangeStart))
	{
		OutStartSec = F0->StartTime;
	}
	if (const TraceServices::FFrame* F1 = Frames.GetFrame(TraceFrameType_Game, Args.FrameRangeEnd - 1))
	{
		OutEndSec = F1->EndTime;
	}
}

void Modes::RunTaskList(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const ITasksProvider* Provider = ReadTasksProvider(Session);

	const ETaskEnumerationOption Option = ParseEnumOption(Args.State);
	const int32 Limit = Args.Limit > 0 ? Args.Limit : 500;
	double StartSec = 0.0, EndSec = 0.0;
	ResolveTimeWindow(Session, Args, StartSec, EndSec);

	Json.BeginObject();
	Json.KeyStr(TEXT("file"),  Args.File);
	Json.KeyStr(TEXT("mode"),  FArgs::ModeName(Args.Mode));
	Json.KeyStr(TEXT("state"), FString(EnumOptionName(Option)));
	Json.KeyNum(TEXT("window_start_ms"), StartSec * 1000.0);
	Json.KeyNum(TEXT("window_end_ms"),   EndSec   * 1000.0);

	if (!Provider)
	{
		Json.KeyInt(TEXT("total_in_window"), 0);
		Json.KeyBool(TEXT("truncated"), false);
		Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray();
		Json.EndObject();
		return;
	}

	Json.KeyInt(TEXT("total_tasks_in_trace"), Provider->GetNumTasks());

	int32 Emitted = 0;
	int32 Total = 0;

	Json.Key(TEXT("events"));
	Json.BeginArray();
	Provider->EnumerateTasks(StartSec, EndSec, Option,
		[&](const FTaskInfo& T) -> ETaskEnumerationResult
		{
			++Total;
			if (Emitted >= Limit) return ETaskEnumerationResult::Continue;

			// Surface the most useful fields without dumping every
			// timestamp — drill into one task via task_drill if needed.
			Json.BeginObject();
			Json.KeyInt(TEXT("id"),                 static_cast<int64>(T.Id));
			Json.KeyStr(TEXT("debug_name"),         T.DebugName ? FString(T.DebugName) : FString());
			Json.KeyBool(TEXT("tracked"),           T.bTracked);
			Json.KeyInt(TEXT("thread_to_execute"),  static_cast<int64>(T.ThreadToExecuteOn));
			Json.KeyNum(TEXT("created_ms"),         T.CreatedTimestamp   * 1000.0);
			Json.KeyNum(TEXT("started_ms"),         T.StartedTimestamp   * 1000.0);
			Json.KeyNum(TEXT("finished_ms"),        T.FinishedTimestamp  * 1000.0);
			Json.KeyNum(TEXT("completed_ms"),       T.CompletedTimestamp * 1000.0);
			Json.KeyInt(TEXT("prerequisite_count"), T.Prerequisites.Num());
			Json.KeyInt(TEXT("subsequent_count"),   T.Subsequents.Num());
			Json.EndObject();
			++Emitted;
			return ETaskEnumerationResult::Continue;
		});
	Json.EndArray();

	Json.KeyInt (TEXT("total_in_window"), Total);
	Json.KeyBool(TEXT("truncated"),       Emitted < Total);
	Json.EndObject();
}

// Emit one FRelationInfo array as `[{id, time_ms, thread_id}, ...]`.
static void EmitRelations(const TCHAR* Key,
                          const TArray<FTaskInfo::FRelationInfo>& Rels,
                          FJsonOut& Json)
{
	Json.Key(Key);
	Json.BeginArray();
	for (const FTaskInfo::FRelationInfo& R : Rels)
	{
		Json.BeginObject();
		Json.KeyInt(TEXT("id"),        static_cast<int64>(R.RelativeId));
		Json.KeyNum(TEXT("time_ms"),   R.Timestamp * 1000.0);
		Json.KeyInt(TEXT("thread_id"), static_cast<int64>(R.ThreadId));
		Json.EndObject();
	}
	Json.EndArray();
}

void Modes::RunTaskDrill(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const ITasksProvider* Provider = ReadTasksProvider(Session);

	Json.BeginObject();
	Json.KeyStr(TEXT("file"),    Args.File);
	Json.KeyStr(TEXT("mode"),    FArgs::ModeName(Args.Mode));
	Json.KeyInt(TEXT("task_id"), static_cast<int64>(Args.TaskId));

	if (!Provider)
	{
		Json.KeyBool(TEXT("found"), false);
		Json.EndObject();
		return;
	}

	const FTaskInfo* T = Provider->TryGetTask(Args.TaskId);
	if (!T)
	{
		Json.KeyBool(TEXT("found"), false);
		Json.EndObject();
		return;
	}

	Json.KeyBool(TEXT("found"),             true);
	Json.KeyStr (TEXT("debug_name"),        T->DebugName ? FString(T->DebugName) : FString());
	Json.KeyBool(TEXT("tracked"),           T->bTracked);
	Json.KeyInt (TEXT("thread_to_execute"), static_cast<int64>(T->ThreadToExecuteOn));
	Json.KeyInt (TEXT("task_size"),         static_cast<int64>(T->TaskSize));

	Json.Key(TEXT("timestamps"));
	Json.BeginObject();
	Json.KeyNum(TEXT("created_ms"),   T->CreatedTimestamp   * 1000.0);
	Json.KeyNum(TEXT("launched_ms"),  T->LaunchedTimestamp  * 1000.0);
	Json.KeyNum(TEXT("scheduled_ms"), T->ScheduledTimestamp * 1000.0);
	Json.KeyNum(TEXT("started_ms"),   T->StartedTimestamp   * 1000.0);
	Json.KeyNum(TEXT("finished_ms"),  T->FinishedTimestamp  * 1000.0);
	Json.KeyNum(TEXT("completed_ms"), T->CompletedTimestamp * 1000.0);
	Json.KeyNum(TEXT("destroyed_ms"), T->DestroyedTimestamp * 1000.0);
	Json.EndObject();

	Json.Key(TEXT("threads"));
	Json.BeginObject();
	Json.KeyInt(TEXT("created"),   static_cast<int64>(T->CreatedThreadId));
	Json.KeyInt(TEXT("launched"),  static_cast<int64>(T->LaunchedThreadId));
	Json.KeyInt(TEXT("scheduled"), static_cast<int64>(T->ScheduledThreadId));
	Json.KeyInt(TEXT("started"),   static_cast<int64>(T->StartedThreadId));
	Json.KeyInt(TEXT("completed"), static_cast<int64>(T->CompletedThreadId));
	Json.KeyInt(TEXT("destroyed"), static_cast<int64>(T->DestroyedThreadId));
	Json.EndObject();

	EmitRelations(TEXT("prerequisites"), T->Prerequisites, Json);
	EmitRelations(TEXT("subsequents"),   T->Subsequents,   Json);
	EmitRelations(TEXT("parent_tasks"),  T->ParentTasks,   Json);
	EmitRelations(TEXT("nested_tasks"),  T->NestedTasks,   Json);

	Json.EndObject();
}

} // namespace TraceDigest

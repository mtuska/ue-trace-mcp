// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#pragma once

#include "TraceDigestPCH.h"

#include "TraceServices/Model/TimingProfiler.h"

namespace TraceDigest
{

// Find every CPU TimerId whose display name exactly matches `EventName`. The
// same name can show up with multiple TimerIds (e.g. metadata vs non-metadata
// variants of the same scope), so this returns a set.
inline void FindTimerIdsByName(const TraceServices::ITimingProfilerProvider& Provider,
                               const FString& EventName,
                               TSet<uint32>& OutTimerIds)
{
	using namespace TraceServices;
	Provider.ReadTimers([&](const ITimingProfilerTimerReader& Reader)
	{
		const uint32 N = Reader.GetTimerCount();
		for (uint32 I = 0; I < N; ++I)
		{
			const FTimingProfilerTimer* Timer = Reader.GetTimer(I);
			if (Timer && Timer->Name && Timer->Type == ETimingProfilerTimerType::CpuScope
				&& EventName.Equals(Timer->Name))
			{
				OutTimerIds.Add(Timer->Id);
			}
		}
	});
}

// Pick a single canonical TimerId for a name (the first one we find). Used by
// the butterfly modes which take exactly one TimerId.
inline bool FindFirstTimerIdByName(const TraceServices::ITimingProfilerProvider& Provider,
                                   const FString& EventName,
                                   uint32& OutTimerId)
{
	TSet<uint32> Ids;
	FindTimerIdsByName(Provider, EventName, Ids);
	if (Ids.Num() == 0) return false;
	OutTimerId = *Ids.CreateConstIterator();
	return true;
}

} // namespace TraceDigest

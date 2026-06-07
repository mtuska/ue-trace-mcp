// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#pragma once

#include "TraceDigestPCH.h"

namespace TraceDigest
{

// Time-bucketed downsampler used by every per-time-series tool (counter
// series, memory tag samples, allocations timeline). Splits a [start, end]
// window into N equal-width buckets; each bucket accumulates min/max/avg/
// count without retaining individual samples, so memory stays O(N) no
// matter how dense the source stream.
//
// Usage:
//   FSeriesBuckets B(start_sec, end_sec, 256);
//   for each (t_sec, value): B.Add(t_sec, value);
//   for (int32 i = 0; i < B.Num(); ++i) {
//     if (B.Buckets[i].Count == 0) continue;  // gaps are not emitted
//     emit { t_ms: B.MidpointMs(i), min: B.Buckets[i].Min, ... }
//   }
class FSeriesBuckets
{
public:
	struct FBucket
	{
		double Min = 0.0;
		double Max = 0.0;
		double Sum = 0.0;
		uint64 Count = 0;
	};

	FSeriesBuckets(double InStartSec, double InEndSec, int32 InNumBuckets)
		: StartSec(InStartSec)
		, EndSec(FMath::Max(InEndSec, InStartSec + 1e-9))  // guarantee non-zero width
		, BucketWidthSec((EndSec - StartSec) / FMath::Max(1, InNumBuckets))
	{
		Buckets.SetNum(FMath::Max(1, InNumBuckets));
	}

	void Add(double TimeSec, double Value)
	{
		if (TimeSec < StartSec || TimeSec > EndSec) return;
		int32 Idx = static_cast<int32>((TimeSec - StartSec) / BucketWidthSec);
		if (Idx < 0) Idx = 0;
		if (Idx >= Buckets.Num()) Idx = Buckets.Num() - 1;
		FBucket& B = Buckets[Idx];
		if (B.Count == 0)
		{
			B.Min = B.Max = Value;
		}
		else
		{
			B.Min = FMath::Min(B.Min, Value);
			B.Max = FMath::Max(B.Max, Value);
		}
		B.Sum += Value;
		B.Count += 1;
	}

	int32  Num()        const { return Buckets.Num(); }
	double MidpointMs(int32 Idx) const
	{
		return (StartSec + (Idx + 0.5) * BucketWidthSec) * 1000.0;
	}
	double BucketWidthMs() const { return BucketWidthSec * 1000.0; }

	TArray<FBucket> Buckets;

private:
	double StartSec;
	double EndSec;
	double BucketWidthSec;
};

} // namespace TraceDigest

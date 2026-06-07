// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#pragma once

#include "TraceDigestPCH.h"

#include <algorithm>

namespace TraceDigest
{

// Reservoir sampler for per-timer durations.
// Below the cap we keep every sample (exact percentiles). Above the cap we
// switch to Vitter's Algorithm R so the kept array is a uniform random sample
// of the full stream — percentiles converge to the true value at sqrt(n) rate.
//
// Why a cap: a 17s trace at 60 fps with 100k zombies and one scope per zombie
// per frame easily hits 100M samples. 8 bytes/sample = 800MB. We don't want
// that. 200k samples per timer is ~1.6MB and gives < 0.5% error on P95/P99.
class FReservoir
{
public:
	explicit FReservoir(int32 InCap = 200000)
		: Cap(InCap)
	{
	}

	FORCEINLINE void Add(double Value)
	{
		++Seen;
		++Count;
		Sum += Value;
		Min = Value < Min ? Value : Min;
		Max = Value > Max ? Value : Max;

		if (Samples.Num() < Cap)
		{
			Samples.Add(Value);
		}
		else
		{
			// Algorithm R: replace a random index with probability Cap/Seen.
			const int64 Idx = FMath::RandRange(0, static_cast<int32>(Seen - 1));
			if (Idx < Cap)
			{
				Samples[Idx] = Value;
			}
		}
	}

	uint64 GetCount() const { return Count; }
	double GetTotal() const { return Sum; }
	double GetMin() const { return Count ? Min : 0.0; }
	double GetMax() const { return Count ? Max : 0.0; }
	double GetAvg() const { return Count ? (Sum / static_cast<double>(Count)) : 0.0; }

	// Returns P50, P95, P99 in one pass after sorting. Mutates the sample array.
	void GetPercentiles(double& OutP50, double& OutP95, double& OutP99)
	{
		if (Samples.Num() == 0)
		{
			OutP50 = OutP95 = OutP99 = 0.0;
			return;
		}
		std::sort(Samples.GetData(), Samples.GetData() + Samples.Num());
		OutP50 = PercentileSorted(0.50);
		OutP95 = PercentileSorted(0.95);
		OutP99 = PercentileSorted(0.99);
	}

private:
	double PercentileSorted(double P) const
	{
		// Linear interpolation between adjacent samples (matches numpy default).
		const double Rank = P * static_cast<double>(Samples.Num() - 1);
		const int32 Lo = FMath::FloorToInt32(static_cast<float>(Rank));
		const int32 Hi = FMath::Min(Lo + 1, Samples.Num() - 1);
		const double Frac = Rank - static_cast<double>(Lo);
		return Samples[Lo] * (1.0 - Frac) + Samples[Hi] * Frac;
	}

	int32 Cap = 200000;
	uint64 Count = 0;       // total samples seen (capped)
	uint64 Seen  = 0;       // total samples observed (uncapped, for reservoir)
	double Sum = 0.0;
	double Min = TNumericLimits<double>::Max();
	double Max = -TNumericLimits<double>::Max();
	TArray<double> Samples;
};

} // namespace TraceDigest

#include "Benchmark/AvidScriptBenchmarkStatistics.h"

namespace
{
int32 CalculateNearestRankIndex(const int32 SampleCount, const double Quantile)
{
	return FMath::Clamp(
		FMath::CeilToInt(static_cast<double>(SampleCount) * Quantile) - 1,
		0,
		SampleCount - 1);
}
} // namespace

FAvidScriptBenchmarkStats CalculateAvidScriptBenchmarkStats(TArray<double> Samples)
{
	FAvidScriptBenchmarkStats Stats;
	Stats.Count = Samples.Num();
	if (Samples.IsEmpty())
	{
		return Stats;
	}

	Samples.Sort();
	double TotalMs = 0.0;
	for (const double Sample : Samples)
	{
		TotalMs += Sample;
	}

	const int32 LastIndex = Samples.Num() - 1;
	Stats.MinMs = Samples[0];
	Stats.MaxMs = Samples[LastIndex];
	Stats.AvgMs = TotalMs / static_cast<double>(Samples.Num());
	Stats.P50Ms = Samples[CalculateNearestRankIndex(Samples.Num(), 0.50)];
	Stats.P95Ms = Samples[CalculateNearestRankIndex(Samples.Num(), 0.95)];
	return Stats;
}

#include "vda5050_fleet_adapter_full_control/util/metrics.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::util {

namespace {

// Buckets per doubling of the duration.
constexpr double kBucketsPerOctave = 8.0;

}  // namespace

std::size_t Histogram::bucket_of(double microseconds)
{
    if (!(microseconds >= 1.0))
    {
        return 0;
    }
    const double index = std::floor(kBucketsPerOctave * std::log2(microseconds)) + 1.0;
    return static_cast<std::size_t>(std::min(index, static_cast<double>(kBuckets - 1)));
}

double Histogram::upper_edge_us(std::size_t bucket)
{
    return std::exp2(static_cast<double>(bucket) / kBucketsPerOctave);
}

void Histogram::record(std::chrono::nanoseconds duration)
{
    const std::uint64_t ns = duration.count() > 0 ? static_cast<std::uint64_t>(duration.count()) : 0;
    _buckets[bucket_of(static_cast<double>(ns) / 1000.0)].fetch_add(1, std::memory_order_relaxed);
    _sum_ns.fetch_add(ns, std::memory_order_relaxed);
    std::uint64_t seen = _max_ns.load(std::memory_order_relaxed);
    while (ns > seen && !_max_ns.compare_exchange_weak(seen, ns, std::memory_order_relaxed))
    {
    }
}

nlohmann::json to_json(const Distribution &distribution)
{
    const auto rounded = [](double value) { return std::round(value * 1000.0) / 1000.0; };
    return {{"count", distribution.count},
            {"mean", rounded(distribution.mean_us)},
            {"p50", rounded(distribution.p50_us)},
            {"p99", rounded(distribution.p99_us)},
            {"max", rounded(distribution.max_us)}};
}

Distribution Histogram::take()
{
    std::array<std::uint64_t, kBuckets> counts{};
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < kBuckets; ++i)
    {
        counts[i] = _buckets[i].exchange(0, std::memory_order_relaxed);
        total += counts[i];
    }
    const std::uint64_t sum_ns = _sum_ns.exchange(0, std::memory_order_relaxed);
    const std::uint64_t max_ns = _max_ns.exchange(0, std::memory_order_relaxed);

    Distribution result;
    result.count = total;
    if (total == 0)
    {
        return result;
    }
    result.mean_us = static_cast<double>(sum_ns) / 1000.0 / static_cast<double>(total);
    result.max_us = static_cast<double>(max_ns) / 1000.0;

    // The upper edge of the bucket holding the value at `fraction` of the sorted durations.
    const auto percentile = [&](double fraction)
    {
        const auto rank = static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(total)));
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i)
        {
            seen += counts[i];
            if (seen >= std::max<std::uint64_t>(rank, 1))
            {
                return upper_edge_us(i);
            }
        }
        return upper_edge_us(kBuckets - 1);
    };
    result.p50_us = std::min(percentile(0.50), result.max_us);
    result.p99_us = std::min(percentile(0.99), result.max_us);
    return result;
}

ScopedTimer::~ScopedTimer()
{
    if (_target)
    {
        _target->record(std::chrono::steady_clock::now() - _start);
    }
}

}  // namespace vda5050_fleet_adapter_full_control::util

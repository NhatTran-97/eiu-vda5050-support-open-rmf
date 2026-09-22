#ifndef METRICS_HPP
#define METRICS_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <nlohmann/json_fwd.hpp>

namespace vda5050_fleet_adapter_full_control::util {

// A count that only grows; any thread may add to it or read it.
class Counter
{
public:
    void add(std::uint64_t amount = 1) { _value.fetch_add(amount, std::memory_order_relaxed); }
    std::uint64_t value() const { return _value.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> _value{0};
};

// What a Histogram recorded, with durations in microseconds.
struct Distribution
{
    std::uint64_t count = 0;
    double mean_us = 0.0;
    double p50_us = 0.0;
    double p99_us = 0.0;
    double max_us = 0.0;
};

// JSON object with count, mean, p50, p99 and max in microseconds, rounded to 0.001.
nlohmann::json to_json(const Distribution &distribution);

// Durations in buckets growing about 9% each, from 1 microsecond to a few seconds. Lock free, so threads can share one.
class Histogram
{
public:
    // Add one duration; a negative one counts as zero.
    void record(std::chrono::nanoseconds duration);

    // What was recorded since the previous call, which starts a new interval; records made meanwhile may land in either.
    // A percentile is the upper edge of its bucket, so it is at most 9% high.
    Distribution take();

    // The bucket a duration of `microseconds` falls in, and the upper edge of a bucket, in microseconds.
    static std::size_t bucket_of(double microseconds);
    static double upper_edge_us(std::size_t bucket);

    static constexpr std::size_t kBuckets = 192;

private:
    std::array<std::atomic<std::uint64_t>, kBuckets> _buckets{};
    std::atomic<std::uint64_t> _sum_ns{0};
    std::atomic<std::uint64_t> _max_ns{0};
};

// Records the time from its creation until it is destroyed into a histogram chosen along the way.
class ScopedTimer
{
public:
    explicit ScopedTimer(Histogram *target) : _target(target), _start(std::chrono::steady_clock::now()) {}
    ~ScopedTimer();
    ScopedTimer(const ScopedTimer &) = delete;
    ScopedTimer &operator=(const ScopedTimer &) = delete;

    // Record into `target` instead; nullptr records nothing.
    void retarget(Histogram *target) { _target = target; }

private:
    Histogram *_target;
    std::chrono::steady_clock::time_point _start;
};

}  // namespace vda5050_fleet_adapter_full_control::util

#endif  // METRICS_HPP

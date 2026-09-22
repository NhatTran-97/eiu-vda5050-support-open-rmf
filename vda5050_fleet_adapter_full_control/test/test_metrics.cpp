#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "vda5050_fleet_adapter_full_control/util/metrics.hpp"

using namespace vda5050_fleet_adapter_full_control::util;
using std::chrono::microseconds;
using std::chrono::nanoseconds;

TEST(CounterTest, StartsAtZeroAndAddsUp)
{
    Counter counter;
    EXPECT_EQ(counter.value(), 0u);
    counter.add();
    counter.add(4);
    EXPECT_EQ(counter.value(), 5u);
}

TEST(CounterTest, ThreadsDoNotLoseCounts)
{
    Counter counter;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&counter] { for (int i = 0; i < 25000; ++i) { counter.add(); } });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }
    EXPECT_EQ(counter.value(), 100000u);
}

TEST(HistogramTest, BucketsGrowWithTheDuration)
{
    EXPECT_EQ(Histogram::bucket_of(0.0), 0u);
    EXPECT_EQ(Histogram::bucket_of(0.5), 0u);
    EXPECT_EQ(Histogram::bucket_of(1.0), 1u);
    std::size_t previous = 0;
    for (double us = 1.0; us < 1e7; us *= 1.05)
    {
        const std::size_t bucket = Histogram::bucket_of(us);
        EXPECT_GE(bucket, previous) << us;
        EXPECT_GE(Histogram::upper_edge_us(bucket), us * 0.999) << us;
        previous = bucket;
    }
    EXPECT_EQ(Histogram::bucket_of(1e12), Histogram::kBuckets - 1);
}

TEST(HistogramTest, AnEmptyIntervalReportsNothing)
{
    Histogram histogram;
    const Distribution result = histogram.take();
    EXPECT_EQ(result.count, 0u);
    EXPECT_EQ(result.max_us, 0.0);
    EXPECT_EQ(result.p99_us, 0.0);
}

TEST(HistogramTest, CountMeanAndMaximumAreExact)
{
    Histogram histogram;
    for (int us : {10, 20, 30, 40})
    {
        histogram.record(microseconds(us));
    }
    const Distribution result = histogram.take();
    EXPECT_EQ(result.count, 4u);
    EXPECT_DOUBLE_EQ(result.mean_us, 25.0);
    EXPECT_DOUBLE_EQ(result.max_us, 40.0);
}

TEST(HistogramTest, PercentilesAreWithinOneBucketOfTheTrueValue)
{
    Histogram histogram;
    for (int us = 1; us <= 1000; ++us)
    {
        histogram.record(microseconds(us));
    }
    const Distribution result = histogram.take();
    EXPECT_GE(result.p50_us, 500.0);
    EXPECT_LE(result.p50_us, 500.0 * 1.10);
    EXPECT_GE(result.p99_us, 990.0);
    EXPECT_LE(result.p99_us, 1000.0);
}

TEST(HistogramTest, ALongDurationShowsInTheMaximumButOnlyMovesTheP99WhenItIsOnePercent)
{
    Histogram once;
    for (int i = 0; i < 99; ++i)
    {
        once.record(microseconds(10));
    }
    once.record(microseconds(5000));
    const Distribution single = once.take();
    EXPECT_DOUBLE_EQ(single.max_us, 5000.0);
    EXPECT_LT(single.p99_us, 12.0);

    Histogram twice;
    for (int i = 0; i < 98; ++i)
    {
        twice.record(microseconds(10));
    }
    twice.record(microseconds(5000));
    twice.record(microseconds(5000));
    EXPECT_GE(twice.take().p99_us, 4000.0);
}

TEST(HistogramTest, AValueIsNeverReportedAboveTheMaximum)
{
    Histogram histogram;
    histogram.record(microseconds(100));
    const Distribution result = histogram.take();
    EXPECT_DOUBLE_EQ(result.p50_us, 100.0);
    EXPECT_DOUBLE_EQ(result.p99_us, 100.0);
}

TEST(HistogramTest, TakeStartsANewInterval)
{
    Histogram histogram;
    histogram.record(microseconds(500));
    EXPECT_EQ(histogram.take().count, 1u);
    EXPECT_EQ(histogram.take().count, 0u);
    histogram.record(microseconds(7));
    const Distribution second = histogram.take();
    EXPECT_EQ(second.count, 1u);
    EXPECT_DOUBLE_EQ(second.max_us, 7.0);
}

TEST(HistogramTest, ANegativeDurationCountsAsZero)
{
    Histogram histogram;
    histogram.record(nanoseconds(-5));
    const Distribution result = histogram.take();
    EXPECT_EQ(result.count, 1u);
    EXPECT_EQ(result.max_us, 0.0);
}

TEST(HistogramTest, ThreadsDoNotLoseRecords)
{
    Histogram histogram;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&histogram] { for (int i = 0; i < 20000; ++i) { histogram.record(microseconds(i % 50)); } });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }
    const Distribution result = histogram.take();
    EXPECT_EQ(result.count, 80000u);
    EXPECT_DOUBLE_EQ(result.max_us, 49.0);
}

TEST(ScopedTimerTest, RecordsOnceIntoTheChosenHistogram)
{
    Histogram first;
    Histogram second;
    {
        ScopedTimer timer(&first);
        timer.retarget(&second);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(first.take().count, 0u);
    const Distribution result = second.take();
    EXPECT_EQ(result.count, 1u);
    EXPECT_GE(result.max_us, 2000.0);
}

TEST(ScopedTimerTest, ANullTargetRecordsNothing)
{
    Histogram histogram;
    {
        ScopedTimer timer(&histogram);
        timer.retarget(nullptr);
    }
    EXPECT_EQ(histogram.take().count, 0u);
}

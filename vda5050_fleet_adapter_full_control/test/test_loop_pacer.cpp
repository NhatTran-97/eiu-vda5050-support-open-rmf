#include <gtest/gtest.h>

#include <chrono>

#include "vda5050_fleet_adapter_full_control/util/loop_pacer.hpp"

using vda5050_fleet_adapter_full_control::util::LoopPacer;
using std::chrono::milliseconds;

namespace {
const std::chrono::steady_clock::time_point kStart{std::chrono::hours(1)};
}  // namespace

TEST(LoopPacerTest, PassesStartOnAFixedGridWhateverTheyTake)
{
    LoopPacer pacer(kStart, milliseconds(100));
    EXPECT_EQ(pacer.next(kStart + milliseconds(10)), kStart + milliseconds(100));
    EXPECT_EQ(pacer.next(kStart + milliseconds(100) + milliseconds(40)), kStart + milliseconds(200));
    EXPECT_EQ(pacer.next(kStart + milliseconds(200) + milliseconds(1)), kStart + milliseconds(300));
    EXPECT_EQ(pacer.overruns(), 0u);
}

TEST(LoopPacerTest, AThousandPassesDoNotDrift)
{
    LoopPacer pacer(kStart, milliseconds(100));
    auto wake = kStart;
    for (int i = 0; i < 1000; ++i)
    {
        wake = pacer.next(wake + milliseconds(35));
    }
    EXPECT_EQ(wake, kStart + milliseconds(100) * 1000);
}

TEST(LoopPacerTest, APassThatEndsExactlyAtTheNextSlotIsNotAnOverrun)
{
    LoopPacer pacer(kStart, milliseconds(100));
    EXPECT_EQ(pacer.next(kStart + milliseconds(100)), kStart + milliseconds(100));
    EXPECT_EQ(pacer.overruns(), 0u);
}

TEST(LoopPacerTest, AnOverrunSkipsTheMissedSlots)
{
    LoopPacer pacer(kStart, milliseconds(100));
    EXPECT_EQ(pacer.next(kStart + milliseconds(250)), kStart + milliseconds(300));
    EXPECT_EQ(pacer.overruns(), 1u);
}

TEST(LoopPacerTest, JustPastTheNextSlotSkipsOnlyThatSlot)
{
    LoopPacer pacer(kStart, milliseconds(100));
    EXPECT_EQ(pacer.next(kStart + milliseconds(101)), kStart + milliseconds(200));
    EXPECT_EQ(pacer.overruns(), 1u);
}

TEST(LoopPacerTest, TheGridSurvivesAnOverrun)
{
    LoopPacer pacer(kStart, milliseconds(100));
    EXPECT_EQ(pacer.next(kStart + milliseconds(730)), kStart + milliseconds(800));
    EXPECT_EQ(pacer.next(kStart + milliseconds(810)), kStart + milliseconds(900));
    EXPECT_EQ(pacer.next(kStart + milliseconds(915)), kStart + milliseconds(1000));
    EXPECT_EQ(pacer.overruns(), 1u);
}

TEST(LoopPacerTest, EveryOverrunIsCounted)
{
    LoopPacer pacer(kStart, milliseconds(100));
    pacer.next(kStart + milliseconds(150));
    pacer.next(kStart + milliseconds(420));
    pacer.next(kStart + milliseconds(430));
    EXPECT_EQ(pacer.overruns(), 2u);
}

TEST(LoopPacerTest, APeriodThatIsNotPositiveBecomesOneMillisecond)
{
    LoopPacer zero(kStart, milliseconds(0));
    EXPECT_EQ(zero.next(kStart), kStart + milliseconds(1));
    LoopPacer negative(kStart, milliseconds(-5));
    EXPECT_EQ(negative.next(kStart), kStart + milliseconds(1));
}

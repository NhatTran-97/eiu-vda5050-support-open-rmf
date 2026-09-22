#include <gtest/gtest.h>

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#include <rclcpp/logger.hpp>

#include "vda5050_fleet_adapter_full_control/core/config.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/state_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/state_sequence.hpp"

using namespace vda5050_fleet_adapter_full_control;
using vda5050::parse_timestamp_ms;
using vda5050::StateSequence;

TEST(TimestampTest, ParsesUtcTimestampsToMilliseconds)
{
    EXPECT_EQ(parse_timestamp_ms("1970-01-01T00:00:00.000Z"), 0);
    EXPECT_EQ(parse_timestamp_ms("2026-09-20T10:00:00.123Z"), 1789898400123);
    EXPECT_EQ(parse_timestamp_ms("2000-02-29T23:59:59.999Z"), 951868799999);
    EXPECT_EQ(parse_timestamp_ms("2100-03-01T00:00:00.000Z"), 4107542400000);
    EXPECT_EQ(parse_timestamp_ms("2024-12-31T23:59:59.000Z"), 1735689599000);
}

TEST(TimestampTest, FractionOfAnyLengthIsAccepted)
{
    EXPECT_EQ(parse_timestamp_ms("2026-09-20T10:00:00Z"), 1789898400000);
    EXPECT_EQ(parse_timestamp_ms("2026-09-20T10:00:00.1Z"), 1789898400100);
    EXPECT_EQ(parse_timestamp_ms("2026-09-20T10:00:00.12Z"), 1789898400120);
    EXPECT_EQ(parse_timestamp_ms("2026-09-20T10:00:00.123456Z"), 1789898400123);
}

TEST(TimestampTest, LaterTimestampsCompareGreater)
{
    EXPECT_LT(*parse_timestamp_ms("2026-09-20T10:00:00.999Z"), *parse_timestamp_ms("2026-09-20T10:00:01.000Z"));
    EXPECT_LT(*parse_timestamp_ms("2026-09-20T23:59:59.999Z"), *parse_timestamp_ms("2026-09-21T00:00:00.000Z"));
    EXPECT_LT(*parse_timestamp_ms("2026-12-31T23:59:59.999Z"), *parse_timestamp_ms("2027-01-01T00:00:00.000Z"));
}

TEST(TimestampTest, AnyOtherFormIsRejected)
{
    for (const char *text : {"", "garbage", "2026-09-20T10:00:00", "2026-09-20T10:00:00.123", "2026-09-20T10:00:00+01:00",
                             "2026-09-20 10:00:00.123Z", "2026-09-20t10:00:00.123z", "2026-13-20T10:00:00.000Z",
                             "2026-00-20T10:00:00.000Z", "2026-09-32T10:00:00.000Z", "2026-09-20T24:00:00.000Z",
                             "2026-09-20T10:60:00.000Z", "2026-09-20T10:00:00.Z", "2026-09-20T10:00:00.123ZZ",
                             "1969-12-31T23:59:59.000Z", "20260920T100000Z", "2026-09-20T10:00:0xZ"})
    {
        EXPECT_FALSE(parse_timestamp_ms(text).has_value()) << text;
    }
}

namespace {

nlohmann::json minimal_state(const nlohmann::json &header)
{
    using nlohmann::json;
    json state = {{"orderId", ""}, {"lastNodeId", "n0"}, {"driving", false}, {"nodeStates", json::array()},
                  {"edgeStates", json::array()}, {"actionStates", json::array()}, {"errors", json::array()},
                  {"operatingMode", "AUTOMATIC"}, {"safetyState", {{"eStop", "NONE"}, {"fieldViolation", false}}},
                  {"batteryState", {{"batteryCharge", 80.0}}}};
    state.update(header);
    return state;
}

}  // namespace

TEST(ParsedStateHeaderTest, ReadsHeaderIdAndTimestamp)
{
    const vda5050::ParsedState state(minimal_state({{"headerId", 42}, {"timestamp", "2026-09-20T10:00:00.123Z"}}));
    EXPECT_EQ(state.header_id, 42u);
    EXPECT_EQ(state.timestamp_ms, 1789898400123);
}

TEST(ParsedStateHeaderTest, AMissingOrMalformedHeaderIsLeftUnset)
{
    const vda5050::ParsedState missing(minimal_state(nlohmann::json::object()));
    EXPECT_FALSE(missing.header_id.has_value());
    EXPECT_FALSE(missing.timestamp_ms.has_value());

    for (const nlohmann::json &id : {nlohmann::json("7"), nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json(nullptr),
                                     nlohmann::json(static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) + 1)})
    {
        const vda5050::ParsedState state(minimal_state({{"headerId", id}, {"timestamp", 5}}));
        EXPECT_FALSE(state.header_id.has_value()) << id;
        EXPECT_FALSE(state.timestamp_ms.has_value());
    }
}

namespace {
constexpr std::int64_t kSecond = 1000;
constexpr std::int64_t kT0 = 1789898400000;
}  // namespace

TEST(StateSequenceTest, TheFirstMessageAndRisingIdsAreAccepted)
{
    StateSequence sequence;
    EXPECT_FALSE(sequence.is_stale(10, kT0, 3));
    EXPECT_FALSE(sequence.is_stale(11, kT0 + kSecond, 3));
    EXPECT_FALSE(sequence.is_stale(15, kT0 + 2 * kSecond, 3));
    EXPECT_EQ(sequence.last_header_id(), 15u);
}

TEST(StateSequenceTest, ADuplicateIsStale)
{
    StateSequence sequence;
    EXPECT_FALSE(sequence.is_stale(10, kT0, 3));
    EXPECT_TRUE(sequence.is_stale(10, kT0, 3));
    EXPECT_EQ(sequence.last_header_id(), 10u);
}

TEST(StateSequenceTest, AnOlderMessageIsStaleAndDoesNotMoveTheBaseline)
{
    StateSequence sequence;
    EXPECT_FALSE(sequence.is_stale(10, kT0, 3));
    EXPECT_FALSE(sequence.is_stale(11, kT0 + kSecond, 3));
    EXPECT_TRUE(sequence.is_stale(9, kT0 - kSecond, 3));
    EXPECT_TRUE(sequence.is_stale(10, kT0, 3));
    EXPECT_EQ(sequence.last_header_id(), 11u);
    EXPECT_FALSE(sequence.is_stale(12, kT0 + 2 * kSecond, 3));
}

TEST(StateSequenceTest, ALaterTimestampAcceptsALowerId)
{
    // A sender that restarted, or that does not count its messages, is not treated as stale.
    StateSequence sequence;
    EXPECT_FALSE(sequence.is_stale(500, kT0, 3));
    EXPECT_FALSE(sequence.is_stale(1, kT0 + kSecond, 3));
    EXPECT_FALSE(sequence.is_stale(1, kT0 + 2 * kSecond, 3));
    EXPECT_EQ(sequence.last_header_id(), 1u);
}

TEST(StateSequenceTest, ARisingIdAcceptsAnEarlierTimestamp)
{
    StateSequence sequence;
    EXPECT_FALSE(sequence.is_stale(10, kT0, 3));
    EXPECT_FALSE(sequence.is_stale(11, kT0 - 3600 * kSecond, 3));
}

TEST(StateSequenceTest, WithoutAHeaderIdOrATimestampNothingIsStale)
{
    StateSequence sequence;
    EXPECT_FALSE(sequence.is_stale(10, kT0, 3));
    EXPECT_FALSE(sequence.is_stale(std::nullopt, kT0 - kSecond, 3));
    EXPECT_EQ(sequence.last_header_id(), 10u);
    EXPECT_FALSE(sequence.is_stale(10, std::nullopt, 3));
    EXPECT_FALSE(sequence.is_stale(10, std::nullopt, 3));
    EXPECT_FALSE(sequence.is_stale(9, std::nullopt, 3));
}

TEST(StateSequenceTest, ASenderThatNeverCountsIsNeverLockedOut)
{
    // Constant id with timestamps that do not advance: only the streak limit keeps messages coming.
    StateSequence sequence;
    int accepted = 0;
    for (int i = 0; i < 40; ++i)
    {
        accepted += sequence.is_stale(1, kT0, 3) ? 0 : 1;
    }
    EXPECT_EQ(accepted, 10);
}

TEST(StateSequenceTest, AfterTheStreakLimitTheNextMessageStartsANewSequence)
{
    StateSequence sequence;
    EXPECT_FALSE(sequence.is_stale(50, kT0, 3));
    EXPECT_TRUE(sequence.is_stale(4, kT0 - 10 * kSecond, 3));
    EXPECT_TRUE(sequence.is_stale(5, kT0 - 9 * kSecond, 3));
    EXPECT_TRUE(sequence.is_stale(6, kT0 - 8 * kSecond, 3));
    EXPECT_FALSE(sequence.is_stale(7, kT0 - 7 * kSecond, 3));
    EXPECT_EQ(sequence.last_header_id(), 7u);
    EXPECT_FALSE(sequence.is_stale(8, kT0 - 6 * kSecond, 3));
}

TEST(StateSequenceTest, AnAcceptedMessageClearsTheStreak)
{
    StateSequence sequence;
    EXPECT_FALSE(sequence.is_stale(10, kT0, 2));
    EXPECT_TRUE(sequence.is_stale(9, kT0 - kSecond, 2));
    EXPECT_TRUE(sequence.is_stale(9, kT0 - kSecond, 2));
    EXPECT_FALSE(sequence.is_stale(11, kT0 + kSecond, 2));
    EXPECT_TRUE(sequence.is_stale(10, kT0, 2));
    EXPECT_TRUE(sequence.is_stale(10, kT0, 2));
}

TEST(StateSequenceTest, ALimitOfZeroAcceptsEverything)
{
    StateSequence sequence;
    EXPECT_FALSE(sequence.is_stale(10, kT0, 0));
    EXPECT_FALSE(sequence.is_stale(10, kT0, 0));
    EXPECT_FALSE(sequence.is_stale(1, kT0 - kSecond, 0));
}

TEST(StateSequenceTest, ResetForgetsTheLastMessage)
{
    StateSequence sequence;
    EXPECT_FALSE(sequence.is_stale(100, kT0, 3));
    EXPECT_TRUE(sequence.is_stale(1, kT0 - kSecond, 3));
    sequence.reset();
    EXPECT_FALSE(sequence.last_header_id().has_value());
    EXPECT_FALSE(sequence.is_stale(1, kT0 - kSecond, 3));
}

TEST(StateSequenceTest, TheIdWrappingAroundIsAcceptedByItsLaterTimestamp)
{
    StateSequence sequence;
    EXPECT_FALSE(sequence.is_stale(std::numeric_limits<std::uint32_t>::max(), kT0, 3));
    EXPECT_FALSE(sequence.is_stale(0, kT0 + kSecond, 3));
}

namespace {

// Write a config file whose vda5050 section ends with `extra` and return its path.
std::string write_config(const std::string &extra)
{
    const std::string path = ::testing::TempDir() + "state_sequence_config_test.yaml";
    std::ofstream(path) << "vda5050:\n  interface_name: test\n  mqtt:\n    host: localhost\n    port: 1883\n" << extra;
    return path;
}

}  // namespace

TEST(StaleStateConfigTest, DefaultsToTheSequenceDefault)
{
    EXPECT_EQ(core::Config(write_config("")).stale_state_streak(), StateSequence::kDefaultStreakLimit);
}

TEST(StaleStateConfigTest, TheKeyIsReadAndValidated)
{
    EXPECT_EQ(core::Config(write_config("  stale_state_streak: 7\n")).stale_state_streak(), 7);
    EXPECT_EQ(core::Config(write_config("  stale_state_streak: 0\n")).stale_state_streak(), 0);
    EXPECT_EQ(core::Config(write_config("  stale_state_streak: null\n")).stale_state_streak(), StateSequence::kDefaultStreakLimit);
    for (const char *bad : {"-1", "101", "2.5", "often"})
    {
        try
        {
            core::Config config(write_config(std::string("  stale_state_streak: ") + bad + "\n"));
            FAIL() << bad;
        }
        catch (const std::runtime_error &e)
        {
            EXPECT_NE(std::string(e.what()).find("vda5050.stale_state_streak"), std::string::npos) << bad;
        }
    }
}

namespace {

nlohmann::json state_at(int header, const std::string &timestamp, double x, double y)
{
    nlohmann::json state = minimal_state({{"headerId", header}, {"timestamp", timestamp}});
    state["agvPosition"] = {{"x", x}, {"y", y}, {"theta", 0.0}, {"mapId", "map"}, {"positionInitialized", true}};
    return state;
}

// A connector that is never started: messages are fed in directly.
class FreshnessTest : public ::testing::Test
{
protected:
    FreshnessTest() : connector(rclcpp::get_logger("test"), "tcp://localhost:1", "AMR")
    {
        connector.add_robot("tb3_1", "ROBOTIS", "0001", rmf::Transform());
    }

    void feed(const std::string &leaf, const nlohmann::json &payload)
    {
        connector.handle_message("AMR/v2/ROBOTIS/0001/" + leaf, payload.dump());
    }

    // x of the robot's reported position, or -1 without one.
    double x()
    {
        const auto data = connector.get_data("tb3_1");
        return data ? data->position[0] : -1.0;
    }

    rmf::Connector connector;
};

}  // namespace

TEST_F(FreshnessTest, AnOlderStateDoesNotReplaceANewerOne)
{
    feed("state", state_at(10, "2026-09-20T10:00:10.000Z", 5, 5));
    feed("state", state_at(9, "2026-09-20T10:00:09.500Z", 1, 1));
    EXPECT_DOUBLE_EQ(x(), 5.0);
    feed("state", state_at(11, "2026-09-20T10:00:11.000Z", 6, 6));
    EXPECT_DOUBLE_EQ(x(), 6.0);
}

TEST_F(FreshnessTest, ADuplicateStateChangesNothing)
{
    feed("state", state_at(10, "2026-09-20T10:00:10.000Z", 5, 5));
    feed("state", state_at(10, "2026-09-20T10:00:10.000Z", 9, 9));
    EXPECT_DOUBLE_EQ(x(), 5.0);
}

TEST_F(FreshnessTest, ConnectionOnlineStartsANewSequence)
{
    feed("state", state_at(500, "2026-09-20T10:00:10.000Z", 5, 5));
    feed("state", state_at(1, "2026-09-20T09:00:00.000Z", 1, 1));
    EXPECT_DOUBLE_EQ(x(), 5.0);
    feed("connection", {{"connectionState", "ONLINE"}});
    feed("state", state_at(1, "2026-09-20T09:00:00.000Z", 2, 2));
    EXPECT_DOUBLE_EQ(x(), 2.0);
    feed("state", state_at(2, "2026-09-20T09:00:01.000Z", 3, 3));
    EXPECT_DOUBLE_EQ(x(), 3.0);
}

TEST_F(FreshnessTest, ARestartedSenderRecoversAfterTheStreakLimit)
{
    feed("state", state_at(500, "2026-09-20T10:00:10.000Z", 5, 5));
    for (int i = 1; i <= 3; ++i)
    {
        feed("state", state_at(i, "2026-09-20T09:00:0" + std::to_string(i) + ".000Z", 1, 1));
        EXPECT_DOUBLE_EQ(x(), 5.0) << i;
    }
    feed("state", state_at(4, "2026-09-20T09:00:04.000Z", 2, 2));
    EXPECT_DOUBLE_EQ(x(), 2.0);
}

TEST_F(FreshnessTest, AStreakLimitOfZeroKeepsEveryState)
{
    connector.set_stale_state_streak(0);
    feed("state", state_at(10, "2026-09-20T10:00:10.000Z", 5, 5));
    feed("state", state_at(9, "2026-09-20T10:00:09.000Z", 1, 1));
    EXPECT_DOUBLE_EQ(x(), 1.0);
}

TEST_F(FreshnessTest, StatesWithoutAHeaderAreNeverDropped)
{
    for (int i = 1; i <= 8; ++i)
    {
        nlohmann::json state = minimal_state(nlohmann::json::object());
        state["agvPosition"] = {{"x", 1.0 * i}, {"y", 0.0}, {"theta", 0.0}, {"mapId", "map"}, {"positionInitialized", true}};
        feed("state", state);
        EXPECT_DOUBLE_EQ(x(), 1.0 * i) << i;
    }
}

// Order acknowledgement through the AGV state, idempotent resend (VDA5050 6.6.4.3) and
// cancellation of active orders this adapter did not send. Needs VDA5050_TEST_BROKER.

#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <mqtt/async_client.h>
#include <nlohmann/json.hpp>
#include <rclcpp/logging.hpp>

#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"

namespace {

using namespace std::chrono_literals;
using vda5050_fleet_adapter_full_control::rmf::CommandStatus;
using vda5050_fleet_adapter_full_control::rmf::Connector;
using vda5050_fleet_adapter_full_control::rmf::LinkPolicy;
using vda5050_fleet_adapter_full_control::rmf::Transform;
using vda5050_fleet_adapter_full_control::vda5050::CancelPolicy;
using json = nlohmann::json;

const char *kInterface = "RC";
const char *kMap = "L1";

std::optional<std::string> broker_url()
{
    const char *url = std::getenv("VDA5050_TEST_BROKER");
    return url && *url ? std::optional<std::string>(url) : std::nullopt;
}

// Order and command messages the adapter publishes for robot M/S1 (state and factsheet requests excluded).
class Capture : public virtual mqtt::callback
{
public:
    explicit Capture(const std::string &url)
        : _client(url, "recovery_capture_" + std::to_string(::getpid()) + "_" + std::to_string(++_instances))
    {
        _client.set_callback(*this);
    }
    ~Capture() override
    {
        try
        {
            _client.disconnect()->wait_for(1s);
        }
        catch (...)
        {
        }
    }

    bool start(const std::string &serial)
    {
        mqtt::connect_options options;
        options.set_clean_session(true);
        return _client.connect(options)->wait_for(5s) &&
               _client.subscribe(std::string(kInterface) + "/v2/M/" + serial + "/+", 1)->wait_for(5s);
    }

    void message_arrived(mqtt::const_message_ptr msg) override
    {
        const std::string topic = msg->get_topic();
        const std::string leaf = topic.substr(topic.rfind('/') + 1);
        if (leaf != "order" && leaf != "instantActions")
        {
            return;
        }
        const auto payload = json::parse(msg->get_payload_str(), nullptr, false);
        const std::string kind = leaf == "order" ? "order" : payload["actions"][0].value("actionType", std::string{"?"});
        if (kind == "stateRequest" || kind == "factsheetRequest")
        {
            return;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _messages.push_back(payload);
        _kinds.push_back(kind);
    }

    std::vector<std::string> kinds()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _kinds;
    }
    std::vector<json> messages()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _messages;
    }

private:
    static inline int _instances = 0;
    mqtt::async_client _client;
    std::mutex _mutex;
    std::vector<json> _messages;
    std::vector<std::string> _kinds;
};

json agv_state(int header, const std::string &order_id, int update_id, bool active, const json &errors = json::array())
{
    json node_states = json::array();
    if (active)
    {
        node_states.push_back({{"nodeId", "B"}, {"sequenceId", 2}, {"released", true}});
    }
    return {{"headerId", header},
            {"orderId", order_id},
            {"orderUpdateId", update_id},
            {"lastNodeId", active ? "A" : ""},
            {"lastNodeSequenceId", 0},
            {"driving", active},
            {"nodeStates", node_states},
            {"edgeStates", json::array()},
            {"actionStates", json::array()},
            {"errors", errors},
            {"operatingMode", "AUTOMATIC"},
            {"safetyState", {{"eStop", "NONE"}, {"fieldViolation", false}}},
            {"batteryState", {{"batteryCharge", 80.0}}},
            {"maps", json::array({{{"mapId", kMap}, {"mapStatus", "ENABLED"}}})},
            {"agvPosition", {{"x", 0.0}, {"y", 0.0}, {"theta", 0.0}, {"mapId", kMap}, {"positionInitialized", true}}}};
}

std::vector<Connector::RoutePoint> route()
{
    return {{"B", 2.0, 0.0, 0.0, std::nullopt}, {"C", 4.0, 0.0, 0.0, std::nullopt}};
}

std::string parameter(const json &instant_actions, const std::string &key)
{
    for (const auto &p : instant_actions["actions"][0].value("actionParameters", json::array()))
    {
        if (p.value("key", std::string{}) == key) return p.value("value", std::string{});
    }
    return {};
}

}  // namespace

class OrderRecoveryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto url = broker_url();
        if (!url)
        {
            GTEST_SKIP() << "set VDA5050_TEST_BROKER to a private broker to run these tests";
        }
        static int counter = 0;
        serial_ = "S" + std::to_string(::getpid()) + "_" + std::to_string(++counter);
        policy_.order_ack_timeout_s = 0.3;
        policy_.order_resend_attempts = 2;
        connector_ = std::make_unique<Connector>(rclcpp::get_logger("test"), *url, kInterface);
        capture_ = std::make_unique<Capture>(*url);
        ASSERT_TRUE(capture_->start(serial_));
    }

    void TearDown() override
    {
        capture_.reset();
        if (connector_) connector_->shutdown();
    }

    // Start the connector with policy_ and report an AGV state.
    void start(const json &first_state)
    {
        connector_->set_link_policy(policy_);
        connector_->add_robot("r1", "M", serial_, Transform());
        connector_->start();
        for (int i = 0; i < 100 && !connector_->metrics()["mqtt"]["connected"].get<bool>(); ++i)
        {
            std::this_thread::sleep_for(50ms);
        }
        ASSERT_TRUE(connector_->metrics()["mqtt"]["connected"].get<bool>());
        std::this_thread::sleep_for(200ms);
        feed(first_state);
    }

    void feed(json state)
    {
        state["headerId"] = ++header_;
        connector_->handle_message(std::string(kInterface) + "/v2/M/" + serial_ + "/state", state.dump());
    }

    // Call poll() like the update loop does, for `duration`.
    void poll_for(std::chrono::milliseconds duration)
    {
        const auto until = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < until)
        {
            connector_->poll("r1");
            std::this_thread::sleep_for(20ms);
        }
        std::this_thread::sleep_for(200ms);
    }

    std::string serial_;
    int header_ = 0;
    LinkPolicy policy_;
    std::unique_ptr<Connector> connector_;
    std::unique_ptr<Capture> capture_;
};

TEST_F(OrderRecoveryTest, UnacknowledgedOrderIsSentAgainUnchanged)
{
    start(agv_state(0, "", 0, false));
    const auto result = connector_->navigate_route("r1", route(), kMap);
    ASSERT_EQ(result.status, CommandStatus::queued);
    poll_for(1500ms);

    const auto messages = capture_->messages();
    ASSERT_EQ(capture_->kinds(), (std::vector<std::string>{"order", "order", "order"}));
    for (const auto &m : messages)
    {
        EXPECT_EQ(m["orderId"], result.order_id);
        EXPECT_EQ(m["orderUpdateId"], 0);
        EXPECT_EQ(m["nodes"], messages[0]["nodes"]);
        EXPECT_EQ(m["edges"], messages[0]["edges"]);
    }
    EXPECT_LT(messages[0]["headerId"].get<int>(), messages[1]["headerId"].get<int>());
    EXPECT_LT(messages[1]["headerId"].get<int>(), messages[2]["headerId"].get<int>());
}

TEST_F(OrderRecoveryTest, AcknowledgedOrderIsNotSentAgain)
{
    start(agv_state(0, "", 0, false));
    const auto result = connector_->navigate_route("r1", route(), kMap);
    feed(agv_state(0, result.order_id, 0, true));
    poll_for(1000ms);
    EXPECT_EQ(capture_->kinds(), std::vector<std::string>{"order"});
}

TEST_F(OrderRecoveryTest, RefusedOrderIsNotSentAgain)
{
    start(agv_state(0, "", 0, false));
    const auto result = connector_->navigate_route("r1", route(), kMap);
    feed(agv_state(0, "", 0, false, json::array({{{"errorType", "orderError"}, {"errorLevel", "WARNING"},
        {"errorReferences", json::array({{{"referenceKey", "orderId"}, {"referenceValue", result.order_id}}})}}})));
    poll_for(1000ms);
    EXPECT_EQ(capture_->kinds(), std::vector<std::string>{"order"});
}

TEST_F(OrderRecoveryTest, ResendingCanBeTurnedOff)
{
    policy_.order_resend_attempts = 0;
    start(agv_state(0, "", 0, false));
    connector_->navigate_route("r1", route(), kMap);
    poll_for(1000ms);
    EXPECT_EQ(capture_->kinds(), std::vector<std::string>{"order"});
}

TEST_F(OrderRecoveryTest, UpdateIsAcknowledgedByItsUpdateId)
{
    start(agv_state(0, "", 0, false));
    const auto result = connector_->navigate_route("r1", route(), kMap, 1);
    feed(agv_state(0, result.order_id, 0, true));
    ASSERT_EQ(connector_->release_more("r1", result.order_id, 2), CommandStatus::queued);
    feed(agv_state(0, result.order_id, 0, true));
    poll_for(500ms);

    auto kinds = capture_->kinds();
    ASSERT_GE(kinds.size(), 3u);
    const auto resent = capture_->messages()[2];
    EXPECT_EQ(resent["orderId"], result.order_id);
    EXPECT_EQ(resent["orderUpdateId"], 1);

    feed(agv_state(0, result.order_id, 1, true));
    const auto sent = capture_->kinds().size();
    poll_for(1000ms);
    EXPECT_EQ(capture_->kinds().size(), sent);
}

TEST_F(OrderRecoveryTest, UnknownActiveOrderIsCancelledWithItsOrderId)
{
    start(agv_state(0, "left-by-previous-run", 3, true));
    poll_for(500ms);
    ASSERT_EQ(capture_->kinds(), std::vector<std::string>{"cancelOrder"});
    EXPECT_EQ(parameter(capture_->messages()[0], "orderId"), "left-by-previous-run");

    poll_for(500ms);
    EXPECT_EQ(capture_->kinds().size(), 1u);
}

TEST_F(OrderRecoveryTest, UnknownOrderIsCancelledBeforeANewOrder)
{
    start(agv_state(0, "left-by-previous-run", 3, true));
    const auto result = connector_->navigate_route("r1", route(), kMap);
    ASSERT_EQ(result.status, CommandStatus::queued);
    std::this_thread::sleep_for(300ms);
    ASSERT_EQ(capture_->kinds(), (std::vector<std::string>{"cancelOrder", "order"}));
    EXPECT_EQ(parameter(capture_->messages()[0], "orderId"), "left-by-previous-run");
    EXPECT_EQ(capture_->messages()[1]["orderId"], result.order_id);
}

TEST_F(OrderRecoveryTest, OwnOrdersAreNotTreatedAsUnknown)
{
    start(agv_state(0, "", 0, false));
    const auto first = connector_->navigate_route("r1", route(), kMap);
    feed(agv_state(0, first.order_id, 0, true));
    ASSERT_EQ(connector_->stop("r1"), CommandStatus::queued);
    const auto second = connector_->navigate_route("r1", route(), kMap);
    feed(agv_state(0, first.order_id, 0, true));
    feed(agv_state(0, second.order_id, 0, true));
    poll_for(500ms);
    EXPECT_EQ(capture_->kinds(), (std::vector<std::string>{"order", "cancelOrder", "order"}));
    EXPECT_EQ(parameter(capture_->messages()[1], "orderId"), "");
}

TEST_F(OrderRecoveryTest, CancelledOwnOrderIsNotTreatedAsUnknownWithoutCancelTracking)
{
    connector_->set_cancel_policy(CancelPolicy{std::chrono::seconds(0), 1});
    start(agv_state(0, "", 0, false));
    const auto first = connector_->navigate_route("r1", route(), kMap);
    feed(agv_state(0, first.order_id, 0, true));
    ASSERT_EQ(connector_->stop("r1"), CommandStatus::queued);
    feed(agv_state(0, first.order_id, 0, true));
    poll_for(500ms);
    EXPECT_EQ(capture_->kinds(), (std::vector<std::string>{"order", "cancelOrder"}));
}

TEST_F(OrderRecoveryTest, UnknownOrdersAreKeptWhenCancellingIsOff)
{
    policy_.cancel_unknown_orders = false;
    start(agv_state(0, "left-by-previous-run", 3, true));
    poll_for(500ms);
    EXPECT_TRUE(capture_->kinds().empty());
}

TEST_F(OrderRecoveryTest, IdleAgvIsLeftAlone)
{
    start(agv_state(0, "finished-order", 3, false));
    poll_for(500ms);
    EXPECT_TRUE(capture_->kinds().empty());
}

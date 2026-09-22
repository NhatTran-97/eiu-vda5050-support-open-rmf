#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "vda5050_fleet_adapter_full_control/mqtt/mqtt_client.hpp"

using vda5050_fleet_adapter_full_control::mqtt::MqttClient;
using vda5050_fleet_adapter_full_control::mqtt::MqttOptions;

namespace {

// A client that never connects: messages are handed to it as Paho would.
class PayloadLimitTest : public ::testing::Test
{
protected:
    void make(std::size_t limit)
    {
        MqttOptions options;
        options.max_payload_bytes = limit;
        client = std::make_unique<MqttClient>("tcp://localhost:1", "payload-limit-test", std::nullopt, std::nullopt, options);
        client->set_on_message([this](const std::string &topic, const std::string &payload) { received.emplace_back(topic, payload.size()); });
        client->set_on_error([this](const std::string &context, const std::string &what) { errors.emplace_back(context, what); });
    }

    void deliver(const std::string &topic, std::size_t size)
    {
        ::mqtt::callback &callback = *client;
        callback.message_arrived(::mqtt::make_message(topic, std::string(size, 'x')));
    }

    std::unique_ptr<MqttClient> client;
    std::vector<std::pair<std::string, std::size_t>> received;
    std::vector<std::pair<std::string, std::string>> errors;
};

}  // namespace

TEST_F(PayloadLimitTest, TheDefaultLimitIsOneMebibyte)
{
    EXPECT_EQ(MqttOptions{}.max_payload_bytes, 1024u * 1024u);
}

TEST_F(PayloadLimitTest, APayloadUpToTheLimitIsDelivered)
{
    make(1000);
    deliver("uagv/v2/ACME/S1/state", 0);
    deliver("uagv/v2/ACME/S1/state", 999);
    deliver("uagv/v2/ACME/S1/state", 1000);
    ASSERT_EQ(received.size(), 3u);
    EXPECT_EQ(received[2].second, 1000u);
    EXPECT_TRUE(errors.empty());
}

TEST_F(PayloadLimitTest, ALargerPayloadIsDroppedAndReported)
{
    make(1000);
    deliver("uagv/v2/ACME/S1/state", 1001);
    EXPECT_TRUE(received.empty());
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors[0].first.find("uagv/v2/ACME/S1/state"), std::string::npos) << errors[0].first;
    EXPECT_NE(errors[0].second.find("1001"), std::string::npos) << errors[0].second;
    EXPECT_NE(errors[0].second.find("1000"), std::string::npos) << errors[0].second;
}

TEST_F(PayloadLimitTest, AnOversizedMessageDoesNotStopTheNextOne)
{
    make(1000);
    deliver("uagv/v2/ACME/S1/state", 5000);
    deliver("uagv/v2/ACME/S1/state", 10);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].second, 10u);
    EXPECT_EQ(errors.size(), 1u);
}

TEST_F(PayloadLimitTest, NothingIsDeliveredAfterShutdown)
{
    make(1000);
    client->shutdown();
    deliver("uagv/v2/ACME/S1/state", 10);
    deliver("uagv/v2/ACME/S1/state", 5000);
    EXPECT_TRUE(received.empty());
    EXPECT_TRUE(errors.empty());
}

TEST(TlsClientTest, AClientWithTlsSettingsIsConstructedWithoutConnecting)
{
    MqttOptions options;
    options.tls.enabled = true;
    options.tls.ca_file = "/nonexistent/ca.pem";
    options.tls.client_cert = "/nonexistent/client.pem";
    options.tls.client_key = "/nonexistent/client.key";
    options.tls.verify_hostname = false;
    MqttClient client("ssl://localhost:1", "tls-test", std::string("user"), std::string("pass"), options);
    EXPECT_FALSE(client.is_connected());
}

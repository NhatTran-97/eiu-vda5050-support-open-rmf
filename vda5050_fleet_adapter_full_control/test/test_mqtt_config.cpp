#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "vda5050_fleet_adapter_full_control/core/config.hpp"

using namespace vda5050_fleet_adapter_full_control::core;
using std::chrono::seconds;

namespace {

// Write a config file whose mqtt section ends with `extra` and return its path.
std::string write_config(const std::string &extra)
{
    const std::string path = ::testing::TempDir() + "mqtt_config_test.yaml";
    std::ofstream(path) << "vda5050:\n  interface_name: test\n  mqtt:\n    host: localhost\n    port: 1883\n" << extra;
    return path;
}

// Whether loading the config fails with an error that names `text`.
bool rejected_naming(const std::string &extra, const std::string &text)
{
    try
    {
        Config config(write_config(extra));
    }
    catch (const std::runtime_error &e)
    {
        return std::string(e.what()).find(text) != std::string::npos;
    }
    return false;
}

}  // namespace

TEST(MqttConfigTest, DefaultsApplyWhenTheKeysAreAbsent)
{
    const Config config(write_config(""));
    const vda5050_fleet_adapter_full_control::mqtt::MqttOptions defaults;
    EXPECT_EQ(config.mqtt().options.connect_timeout, defaults.connect_timeout);
    EXPECT_EQ(config.mqtt().options.retry_min, defaults.retry_min);
    EXPECT_EQ(config.mqtt().options.retry_max, defaults.retry_max);
    EXPECT_EQ(config.mqtt().options.keep_alive, defaults.keep_alive);
    EXPECT_EQ(defaults.keep_alive, seconds(60));
}

TEST(MqttConfigTest, EveryKeyIsRead)
{
    const Config config(write_config("    connect_timeout_s: 5\n    reconnect_min_s: 2\n    reconnect_max_s: 9\n    keep_alive_s: 20\n"));
    EXPECT_EQ(config.mqtt().options.connect_timeout, seconds(5));
    EXPECT_EQ(config.mqtt().options.retry_min, seconds(2));
    EXPECT_EQ(config.mqtt().options.retry_max, seconds(9));
    EXPECT_EQ(config.mqtt().options.keep_alive, seconds(20));
}

TEST(MqttConfigTest, ANullValueKeepsTheDefault)
{
    const Config config(write_config("    connect_timeout_s: null\n"));
    const vda5050_fleet_adapter_full_control::mqtt::MqttOptions defaults;
    EXPECT_EQ(config.mqtt().options.connect_timeout, defaults.connect_timeout);
}

TEST(MqttConfigTest, ValuesOutOfRangeAreRejectedAtStartup)
{
    const std::vector<std::pair<std::string, std::string>> bad = {
        {"connect_timeout_s: 0", "connect_timeout_s"}, {"connect_timeout_s: 121", "connect_timeout_s"},
        {"connect_timeout_s: 2.5", "connect_timeout_s"}, {"connect_timeout_s: soon", "connect_timeout_s"},
        {"reconnect_min_s: 0", "reconnect_min_s"},     {"reconnect_min_s: 61", "reconnect_min_s"},
        {"reconnect_max_s: 0", "reconnect_max_s"},     {"reconnect_max_s: 601", "reconnect_max_s"},
        {"keep_alive_s: 0", "keep_alive_s"},           {"keep_alive_s: 3601", "keep_alive_s"},
    };
    for (const auto &[line, key] : bad)
    {
        EXPECT_TRUE(rejected_naming("    " + line + "\n", "vda5050.mqtt." + key)) << line;
    }
}

TEST(MqttConfigTest, TheMaximumPayloadIsReadAndValidated)
{
    const vda5050_fleet_adapter_full_control::mqtt::MqttOptions defaults;
    EXPECT_EQ(Config(write_config("")).mqtt().options.max_payload_bytes, defaults.max_payload_bytes);
    EXPECT_EQ(Config(write_config("    max_payload_bytes: 65536\n")).mqtt().options.max_payload_bytes, 65536u);
    EXPECT_EQ(Config(write_config("    max_payload_bytes: null\n")).mqtt().options.max_payload_bytes, defaults.max_payload_bytes);
    for (const char *bad : {"1023", "268435457", "0", "-1", "big", "2.5"})
    {
        EXPECT_TRUE(rejected_naming(std::string("    max_payload_bytes: ") + bad + "\n", "vda5050.mqtt.max_payload_bytes")) << bad;
    }
}

TEST(MqttConfigTest, TheMaximumWaitMayNotBeBelowTheMinimum)
{
    EXPECT_TRUE(rejected_naming("    reconnect_min_s: 10\n    reconnect_max_s: 5\n", "reconnect_max_s"));
    EXPECT_NO_THROW(Config(write_config("    reconnect_min_s: 5\n    reconnect_max_s: 5\n")));
}

namespace {

// Write a config file whose vda5050 section is `body` next to some certificate files; return its path.
std::string write_tls_config(const std::string &body)
{
    const std::string directory = ::testing::TempDir();
    for (const char *name : {"ca.pem", "client.pem", "client.key"})
    {
        std::ofstream(directory + name) << "placeholder\n";
    }
    const std::string path = directory + "tls_config_test.yaml";
    std::ofstream(path) << "vda5050:\n  interface_name: test\n" << body;
    return path;
}

}  // namespace

TEST(TlsConfigTest, TlsIsOffByDefault)
{
    const Config config(write_tls_config("  mqtt:\n    host: broker\n"));
    EXPECT_FALSE(config.mqtt().options.tls.enabled);
    EXPECT_EQ(config.mqtt().broker_url, "tcp://broker:1883");
}

TEST(TlsConfigTest, EnablingTlsSelectsTheSslSchemeAndItsDefaultPort)
{
    const Config config(write_tls_config("  mqtt:\n    host: broker\n    tls:\n      enabled: true\n"));
    EXPECT_TRUE(config.mqtt().options.tls.enabled);
    EXPECT_EQ(config.mqtt().broker_url, "ssl://broker:8883");
    EXPECT_TRUE(config.mqtt().options.tls.verify_hostname);
    EXPECT_TRUE(config.mqtt().options.tls.ca_file.empty());

    const Config explicit_port(write_tls_config("  mqtt:\n    host: broker\n    port: 1884\n    tls:\n      enabled: true\n"));
    EXPECT_EQ(explicit_port.mqtt().broker_url, "ssl://broker:1884");
}

TEST(TlsConfigTest, CertificateFilesAreResolvedNextToTheConfigFile)
{
    const Config config(write_tls_config("  mqtt:\n    tls:\n      enabled: true\n      ca_file: ca.pem\n"
                                          "      client_cert: client.pem\n      client_key: client.key\n"
                                          "      verify_hostname: false\n"));
    const std::string directory = ::testing::TempDir();
    EXPECT_EQ(config.mqtt().options.tls.ca_file, directory + "ca.pem");
    EXPECT_EQ(config.mqtt().options.tls.client_cert, directory + "client.pem");
    EXPECT_EQ(config.mqtt().options.tls.client_key, directory + "client.key");
    EXPECT_FALSE(config.mqtt().options.tls.verify_hostname);

    const Config absolute(write_tls_config("  mqtt:\n    tls:\n      enabled: true\n      ca_file: " + directory + "ca.pem\n"));
    EXPECT_EQ(absolute.mqtt().options.tls.ca_file, directory + "ca.pem");
}

TEST(TlsConfigTest, AMissingOrUnusableFileIsRejectedByName)
{
    const auto rejected = [](const std::string &tls_lines, const std::string &key)
    {
        try
        {
            Config config(write_tls_config("  mqtt:\n    tls:\n      enabled: true\n" + tls_lines));
        }
        catch (const std::runtime_error &e)
        {
            return std::string(e.what()).find("vda5050.mqtt.tls." + key) != std::string::npos;
        }
        return false;
    };
    EXPECT_TRUE(rejected("      ca_file: no_such_file.pem\n", "ca_file"));
    EXPECT_TRUE(rejected("      ca_file: .\n", "ca_file"));
    EXPECT_TRUE(rejected("      client_cert: client.pem\n      client_key: no_such.key\n", "client_key"));
    EXPECT_TRUE(rejected("      client_cert: client.pem\n", "client_cert"));
    EXPECT_TRUE(rejected("      client_key: client.key\n", "client_cert"));
    EXPECT_TRUE(rejected("      ca_file: [a, b]\n", "ca_file"));
    EXPECT_TRUE(rejected("      verify_hostname: sometimes\n", "verify_hostname"));
}

TEST(TlsConfigTest, TheBlockIsCheckedOnlyWhenTlsIsEnabled)
{
    const Config config(write_tls_config("  mqtt:\n    tls:\n      enabled: false\n      ca_file: no_such_file.pem\n"));
    EXPECT_FALSE(config.mqtt().options.tls.enabled);
    EXPECT_THROW(Config(write_tls_config("  mqtt:\n    tls: 5\n")), std::runtime_error);
    EXPECT_THROW(Config(write_tls_config("  mqtt:\n    tls:\n      enabled: maybe\n")), std::runtime_error);
}

TEST(CredentialsConfigTest, ValuesMayReadTheEnvironment)
{
    ::setenv("VFA_TEST_USER", "fleet", 1);
    ::setenv("VFA_TEST_SECRET", "s3cret", 1);
    const Config config(write_tls_config("  mqtt:\n    username: ${VFA_TEST_USER}\n    password: \"pre-${VFA_TEST_SECRET}-post\"\n"));
    EXPECT_EQ(config.mqtt().username, "fleet");
    EXPECT_EQ(config.mqtt().password, "pre-s3cret-post");
    const Config plain(write_tls_config("  mqtt:\n    username: robot\n    password: null\n"));
    EXPECT_EQ(plain.mqtt().username, "robot");
    EXPECT_FALSE(plain.mqtt().password.has_value());
}

TEST(CredentialsConfigTest, AnUnsetVariableOrABrokenReferenceIsRejectedByName)
{
    ::unsetenv("VFA_TEST_UNSET");
    for (const char *bad : {"${VFA_TEST_UNSET}", "abc${VFA_TEST_UNSET", "${}"})
    {
        try
        {
            Config config(write_tls_config(std::string("  mqtt:\n    password: \"") + bad + "\"\n"));
            FAIL() << bad;
        }
        catch (const std::runtime_error &e)
        {
            EXPECT_NE(std::string(e.what()).find("vda5050.mqtt.password"), std::string::npos) << bad << ": " << e.what();
        }
    }
}

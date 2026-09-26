#ifndef MQTT_OPTIONS_HPP
#define MQTT_OPTIONS_HPP

#include <chrono>
#include <cstddef>
#include <string>

namespace vda5050_fleet_adapter_full_control::mqtt {

// Transport security of the connection to the MQTT broker.
struct TlsOptions
{
    bool enabled = false;
    // PEM file with the CA certificates that sign the broker's certificate; empty uses the system's.
    std::string ca_file;
    // PEM files with the client's certificate and private key, for a broker that asks for one.
    std::string client_cert;
    std::string client_key;
    // Check that the broker's certificate names the host being connected to.
    bool verify_hostname = true;
};

// Settings of the connection to the MQTT broker.
struct MqttOptions
{
    // Time one connect attempt may take.
    std::chrono::seconds connect_timeout{10};
    // Interval of the keep-alive packets that let the broker and the adapter notice a dead connection.
    std::chrono::seconds keep_alive{60};
    // Wait before the first reconnect attempt; the wait doubles up to retry_max.
    std::chrono::seconds retry_min{1};
    std::chrono::seconds retry_max{30};
    // Largest message payload handled, in bytes; a larger message is dropped unread.
    std::size_t max_payload_bytes = std::size_t{1024} * 1024;
    // QoS of all topics except connection (always 1).
    int qos = 1;
    TlsOptions tls;
};

}  // namespace vda5050_fleet_adapter_full_control::mqtt

#endif  // MQTT_OPTIONS_HPP

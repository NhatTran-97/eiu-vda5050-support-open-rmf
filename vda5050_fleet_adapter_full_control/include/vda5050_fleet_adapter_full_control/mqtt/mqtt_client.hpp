#ifndef MQTT_CLIENT_HPP
#define MQTT_CLIENT_HPP

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <mqtt/async_client.h>

#include "vda5050_fleet_adapter_full_control/mqtt/mqtt_options.hpp"
#include "vda5050_fleet_adapter_full_control/util/metrics.hpp"

namespace vda5050_fleet_adapter_full_control::mqtt {

class MqttClient : public virtual ::mqtt::callback
{
public:
    using MessageCallback = std::function<void(const std::string &topic, const std::string &payload)>;
    using ConnectedCallback = std::function<void()>;
    using ConnectionLostCallback = std::function<void(const std::string &cause)>;
    using ErrorCallback = std::function<void(const std::string &context, const std::string &what)>;

    MqttClient(const std::string &broker_url, const std::string &client_id, std::optional<std::string> username = std::nullopt, std::optional<std::string> password = std::nullopt,
               const MqttOptions &options = {});

    ~MqttClient() override;

    // Set before connecting. The connected callback may use this client; the others run on Paho's thread and must not.
    void set_on_message(MessageCallback cb);
    void set_on_connected(ConnectedCallback cb);
    void set_on_connection_lost(ConnectionLostCallback cb);
    void set_on_error(ErrorCallback cb);

    // Connects without blocking; the connected and error callbacks report the outcome.
    void connect(void);
    void shutdown(void);
    bool is_connected() const;

    // Return whether Paho queued the publish request, without implying delivery.
    bool publish(const std::string &topic, const std::string &payload, int qos = 1);

    // Return whether Paho queued the subscription; restore it after reconnecting.
    bool subscribe(const std::string &topic, int qos = 1);

    // What happened on the connection since this client was created.
    struct Stats
    {
        std::uint64_t connects = 0;
        std::uint64_t connections_lost = 0;
        std::uint64_t oversize_dropped = 0;
        std::uint64_t errors = 0;
    };
    Stats stats() const;

private:
    // Reports a failed connect attempt through the error callback.
    class ConnectListener : public ::mqtt::iaction_listener
    {
    public:
        explicit ConnectListener(MqttClient &owner) : _owner(owner) {}
        void on_failure(const ::mqtt::token &token) override;
        void on_success(const ::mqtt::token &) override {}

    private:
        MqttClient &_owner;
    };

    void report_error(const std::string &context, const std::string &what) noexcept;
    void resubscribe_all();
    void run_connected_worker();
    void connected(const std::string &cause) override;
    void connection_lost(const std::string &cause) override;
    void message_arrived(::mqtt::const_message_ptr msg) override;

    std::map<std::string, int> _subscriptions;
    std::mutex _subs_mutex;
    ConnectListener _connect_listener{*this};
    std::shared_ptr<::mqtt::async_client> _client;
    ::mqtt::connect_options _conn_opts;
    ConnectedCallback _on_connected;
    ConnectionLostCallback _on_connection_lost;
    MessageCallback _on_message;
    ErrorCallback _on_error;
    std::atomic<bool> _shutdown{false};
    std::size_t _max_payload_bytes;
    bool _tls_enabled = false;
    util::Counter _connects;
    util::Counter _connections_lost;
    util::Counter _oversize_dropped;
    util::Counter _errors;

    std::mutex _connected_mutex;
    std::condition_variable _connected_cv;
    bool _connected_pending = false;
    std::thread _connected_worker;
};

}  // namespace vda5050_fleet_adapter_full_control::mqtt

#endif  // MQTT_CLIENT_HPP

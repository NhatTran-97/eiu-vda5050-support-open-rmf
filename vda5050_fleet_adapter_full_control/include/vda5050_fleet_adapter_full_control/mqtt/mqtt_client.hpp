#ifndef MQTT_CLIENT_HPP
#define MQTT_CLIENT_HPP

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <atomic>
#include <map>
#include <mutex>
#include <mqtt/async_client.h>

namespace vda5050_fleet_adapter_full_control::mqtt {

class MqttClient : public virtual ::mqtt::callback
{
public:
    using MessageCallback = std::function<void(const std::string &topic, const std::string &payload)>;
    using ConnectedCallback = std::function<void()>;
    using ConnectionLostCallback = std::function<void(const std::string &cause)>;
    using ErrorCallback = std::function<void(const std::string &context, const std::string &what)>;

    MqttClient(std::string broker_url, std::string client_id,
               std::optional<std::string> username = std::nullopt,
               std::optional<std::string> password = std::nullopt);

    ~MqttClient() override;

    // Call these only before the first connect() -- Paho invokes callbacks on
    // its own internal thread, so reassigning them while connected races with
    // that thread.
    void set_on_message(MessageCallback cb);
    void set_on_connected(ConnectedCallback cb);
    void set_on_connection_lost(ConnectionLostCallback cb);
    void set_on_error(ErrorCallback cb);

    void connect(void);
    void shutdown(void);
    bool is_connected() const;

    // true: request handed off to Paho (no wait on the delivery token, so
    // this confirms neither broker receipt nor robot execution).
    // false: offline, or the underlying publish() call threw -- message is
    // dropped, no automatic retry.
    bool publish(const std::string &topic, const std::string &payload, int qos = 1);

    // true: subscribe request initiated (no wait on the token, so this is
    // not a broker-accepted confirmation).
    // false: offline, or the underlying subscribe() call threw. Either way
    // the topic is still stored and will be retried in connected().
    bool subscribe(const std::string &topic, int qos = 1);

private:
    void report_error(const std::string &context, const std::string &what) noexcept;
    void resubscribe_all();
    void connected(const std::string &cause) override;
    void connection_lost(const std::string &cause) override;
    void message_arrived(::mqtt::const_message_ptr msg) override;

    std::map<std::string, int> _subscriptions;
    std::mutex _subs_mutex;
    std::shared_ptr<::mqtt::async_client> _client;
    ::mqtt::connect_options _conn_opts;
    ConnectedCallback _on_connected;
    ConnectionLostCallback _on_connection_lost;
    MessageCallback _on_message;
    ErrorCallback _on_error;
    std::atomic<bool> _shutdown{false};
};

}  // namespace vda5050_fleet_adapter_full_control::mqtt

#endif  // MQTT_CLIENT_HPP

#ifndef STATE_SEQUENCE_HPP
#define STATE_SEQUENCE_HPP

#include <cstdint>
#include <optional>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// Recognizes state messages that duplicate or precede the last accepted one.
class StateSequence
{
public:
    // Consecutive stale messages tolerated by default before the sender is treated as restarted.
    static constexpr int kDefaultStreakLimit = 3;

    // Whether to drop the message: its headerId is not above the last accepted one and its timestamp is not later.
    // Never true without a headerId or timestamps. After `streak_limit` stale messages in a row the next one starts a new sequence; 0 accepts all.
    bool is_stale(std::optional<std::uint32_t> header_id, std::optional<std::int64_t> timestamp_ms, int streak_limit);

    // Forget the last accepted message.
    void reset();

    // headerId of the last accepted message.
    std::optional<std::uint32_t> last_header_id() const { return _header_id; }

private:
    std::optional<std::uint32_t> _header_id;
    std::optional<std::int64_t> _timestamp_ms;
    int _stale_streak = 0;
};

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // STATE_SEQUENCE_HPP

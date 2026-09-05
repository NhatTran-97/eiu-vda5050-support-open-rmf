#pragma once

#include <string>

namespace tb3_vda5050_bridge {

enum class BridgeMode {
  IDLE,
  DISPATCHING,
  NAVIGATING,
  WAITING_FOR_RELEASE,
  PAUSED,
  FAULTED
};

struct BridgeStatus {
  BridgeMode mode{BridgeMode::IDLE};
  bool driving{false};
  bool paused{false};
};

/**
 * @brief Manages bridge mode transitions and driving/paused state derivation.
 *
 * Centralizes all state transitions (IDLE, DISPATCHING, NAVIGATING, WAITING_FOR_RELEASE,
 * PAUSED, FAULTED) and derives the driving/paused flags from the current mode to prevent
 * contradictory states. Intentionally narrow in scope: owns only mode transitions, not
 * order lifecycle or action blocking (delegated to OrderSession and other managers).
 *
 * Key responsibilities:
 *  - Manage 6-mode state machine (IDLE, DISPATCHING, NAVIGATING, WAITING_FOR_RELEASE, PAUSED, FAULTED)
 *  - Derive driving/paused flags from mode for publication
 *  - Guard against contradictory state combinations
 */
class BridgeStateMachine {
public:
  BridgeStateMachine() = default;

  // ── State transitions ──────────────────────────────────────────────────────

  /**
   * @brief Transition to DISPATCHING when a new order is received.
   */
  void on_order_started();

  /**
   * @brief Transition to DISPATCHING when planning or retrying dispatch.
   */
  void on_dispatching();

  /**
   * @brief Transition to NAVIGATING when Nav2 accepts a goal.
   */
  void on_navigation_active();

  /**
   * @brief Transition to WAITING_FOR_RELEASE when next node is not yet released.
   */
  void on_waiting_for_release();

  /**
   * @brief Transition to PAUSED on pause request.
   */
  void on_pause_requested();

  /**
   * @brief Transition back to DISPATCHING on resume from pause.
   */
  void on_resume_requested();

  /**
   * @brief Transition to IDLE on cancel or order completion.
   */
  void on_cancel_requested();

  /**
   * @brief Transition to FAULTED when navigation fails and cannot recover.
   */
  void on_navigation_failed();

  /**
   * @brief Transition to IDLE when all route work is complete.
   */
  void on_all_work_completed();

  // ── State queries ──────────────────────────────────────────────────────────

  /**
   * @brief Get current BridgeStatus (mode + derived driving/paused flags).
   * @return BridgeStatus struct with mode and flags.
   */
  BridgeStatus status() const;

  /**
   * @brief Get current bridge mode.
   * @return Current BridgeMode enum value.
   */
  BridgeMode mode() const { return mode_; }

  /**
   * @brief Check if bridge is in PAUSED mode.
   * @return true if mode == PAUSED.
   */
  bool is_paused() const { return mode_ == BridgeMode::PAUSED; }

private:
  // ── Internal helpers ───────────────────────────────────────────────────────

  // Convert BridgeMode (mode) to BridgeStatus with appropriate driving/paused flags.
  static BridgeStatus status_from_mode(BridgeMode mode);

  // ── State ──────────────────────────────────────────────────────────────────

  BridgeMode mode_{BridgeMode::IDLE};
};

// Convert BridgeMode (mode) to string representation for logging.
std::string to_string(BridgeMode mode);

}  // namespace tb3_vda5050_bridge

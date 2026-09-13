"""Pure VDA5050 'state' topic parsing -- no Qt, no I/O.

Mirrors vda5050_fleet_adapter_full_control's ParsedState (see
src/vda5050/state_handler.cpp in that package) so the UI reads the same
fields the fleet adapter itself acts on, without re-deriving field names.
"""

from dataclasses import dataclass, field, asdict
from math import hypot


@dataclass
class SafetyState:
    e_stop: str = "NONE"
    field_violation: bool = False

    @property
    def triggered(self) -> bool:
        return self.field_violation or (self.e_stop not in ("", "NONE"))


@dataclass
class RobotState:
    order_id: str = ""
    order_update_id: int | None = None
    last_node_id: str = ""
    last_node_sequence_id: int | None = None
    driving: bool = False
    paused: bool = False
    new_base_request: bool = False
    distance_since_last_node: float | None = None
    operating_mode: str = "AUTOMATIC"

    battery_soc: float | None = None   # fraction 0..1
    charging: bool = False
    speed: float = 0.0                 # m/s, translational

    safety: SafetyState = field(default_factory=SafetyState)
    fatal_error: str = ""

    errors: list = field(default_factory=list)
    information: list = field(default_factory=list)
    loads: list = field(default_factory=list)
    maps: list = field(default_factory=list)
    node_states: list = field(default_factory=list)
    edge_states: list = field(default_factory=list)
    action_states: list = field(default_factory=list)

    localization_score: float | None = None
    map_id: str = ""
    # False means the fleet adapter has no usable pose for this robot: any
    # goal -- a new dispatch or the finishing_request return-to-charger --
    # fails path planning until the operator re-localizes it.
    position_initialized: bool = False

    @property
    def operable(self) -> bool:
        return self.operating_mode in ("AUTOMATIC", "SEMIAUTOMATIC")

    @property
    def ready_for_orders(self) -> bool:
        return (self.operable and not self.safety.triggered and not self.fatal_error
                and not self.paused and self.position_initialized)

    def action_status(self, action_id: str):
        for a in self.action_states:
            if isinstance(a, dict) and a.get("actionId") == action_id:
                return a.get("actionStatus")
        return None

    def to_dict(self) -> dict:
        d = asdict(self)
        d["safety"]["triggered"] = self.safety.triggered
        d["operable"] = self.operable
        d["ready_for_orders"] = self.ready_for_orders
        return d


def _first_fatal_error(errors: list) -> str:
    for e in errors:
        if not isinstance(e, dict) or e.get("errorLevel") != "FATAL":
            continue
        return e.get("errorType") or "(unnamed FATAL error)"
    return ""


def parse_velocity(raw: dict) -> float:
    """state.velocity and visualization.velocity share one schema."""
    v = raw.get("velocity")
    if not isinstance(v, dict):
        return 0.0
    return hypot(v.get("vx", 0.0), v.get("vy", 0.0))


def parse_state(raw: dict) -> RobotState:
    s = RobotState()

    pos = raw.get("agvPosition")
    if isinstance(pos, dict):
        s.localization_score = pos.get("localizationScore")
        s.map_id = pos.get("mapId", "")
        # Mirrors the adapter's own has_position(): a pose isn't usable unless
        # x/y/theta are all reported too, not just the positionInitialized flag.
        has_xytheta = all(pos.get(k) is not None for k in ("x", "y", "theta"))
        s.position_initialized = bool(pos.get("positionInitialized", False)) and has_xytheta

    battery = raw.get("batteryState")
    if isinstance(battery, dict):
        charge = battery.get("batteryCharge")
        if charge is not None:
            s.battery_soc = charge / 100.0
        s.charging = bool(battery.get("charging", False))

    s.speed = parse_velocity(raw)

    safety = raw.get("safetyState")
    if isinstance(safety, dict):
        s.safety = SafetyState(
            e_stop=safety.get("eStop", "NONE"),
            field_violation=bool(safety.get("fieldViolation", False)),
        )

    s.order_id = raw.get("orderId", "")
    s.order_update_id = raw.get("orderUpdateId")
    s.last_node_id = raw.get("lastNodeId", "")
    s.last_node_sequence_id = raw.get("lastNodeSequenceId")
    s.driving = bool(raw.get("driving", False))
    s.paused = bool(raw.get("paused", False))
    s.new_base_request = bool(raw.get("newBaseRequest", False))
    s.distance_since_last_node = raw.get("distanceSinceLastNode")
    s.operating_mode = raw.get("operatingMode", "AUTOMATIC")

    s.node_states = raw.get("nodeStates") or []
    s.edge_states = raw.get("edgeStates") or []
    s.action_states = raw.get("actionStates") or []
    s.errors = raw.get("errors") or []
    s.information = raw.get("information") or []
    s.loads = raw.get("loads") or []
    s.maps = raw.get("maps") or []
    s.fatal_error = _first_fatal_error(s.errors)

    return s

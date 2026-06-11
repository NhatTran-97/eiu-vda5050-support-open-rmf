Header      header

string      order_id
uint32      order_update_id
string      zone_set_id
string      last_node_id
uint32      last_node_sequence_id

NodeState[]    node_states
EdgeState[]    edge_states

AgvPosition    agv_position
bool           agv_position_set

Velocity       velocity
bool           velocity_set

Load[]         loads

bool           driving
bool           paused
bool           new_base_request
float64        distance_since_last_node  # meters

ActionState[]  action_states

BatteryState   battery_state

string         operating_mode   # AUTOMATIC | SEMIAUTOMATIC | MANUAL | SERVICE | TEACHIN

Error[]        errors
Info[]         information

SafetyState    safety_state

MapInfo[]      maps

string      edge_id
uint32      sequence_id
string      edge_description
bool        released
string      start_node_id
string      end_node_id
float64     max_speed           # optional; -1 = not set
float64     max_height          # optional; -1 = not set
float64     min_height          # optional; -1 = not set
float64     orientation         # optional; NaN = not set
string      orientation_type    # GLOBAL | TANGENTIAL
string      direction           # optional
bool        rotation_allowed
bool        rotation_allowed_set
float64     max_rotation_speed  # optional; -1 = not set
Trajectory  trajectory
bool        trajectory_set
float64     length              # optional; -1 = not set
Corridor    corridor
bool        corridor_set
Action[]    actions

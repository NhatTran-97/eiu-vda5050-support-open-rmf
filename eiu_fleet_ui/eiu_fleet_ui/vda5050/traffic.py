"""Summarize VDA5050 order/instantActions/state messages for the traffic log."""

from .state import RobotState


def order_actions(order: dict) -> dict:
    """Map actionId -> blockingType for every action inside an order's nodes and edges."""
    out = {}
    for group in list(order.get("nodes") or []) + list(order.get("edges") or []):
        if not isinstance(group, dict):
            continue
        for a in group.get("actions") or []:
            if isinstance(a, dict) and a.get("actionId"):
                out[a["actionId"]] = a.get("blockingType", "")
    return out


def instant_actions(msg: dict) -> dict:
    """Map actionId -> blockingType for an instantActions message."""
    return {a["actionId"]: a.get("blockingType", "")
            for a in msg.get("actions") or []
            if isinstance(a, dict) and a.get("actionId")}


def order_detail(order: dict) -> dict:
    """Trimmed order for the node-details view: node poses, actions and the edges between them."""
    nodes = []
    for n in order.get("nodes") or []:
        if not isinstance(n, dict):
            continue
        pos = n.get("nodePosition") or {}
        nodes.append({
            "nodeId": n.get("nodeId", ""), "sequenceId": n.get("sequenceId"),
            "x": pos.get("x"), "y": pos.get("y"), "theta": pos.get("theta"),
            "actions": [{"actionId": a.get("actionId", ""), "actionType": a.get("actionType", ""),
                         "blockingType": a.get("blockingType", "")}
                        for a in n.get("actions") or [] if isinstance(a, dict)]})
    edges = [{"sequenceId": e.get("sequenceId"),
              "startNodeId": e.get("startNodeId", ""), "endNodeId": e.get("endNodeId", ""),
              "maxSpeed": e.get("maxSpeed"), "orientation": e.get("orientation")}
             for e in order.get("edges") or [] if isinstance(e, dict)]
    return {"orderId": order.get("orderId", ""), "nodes": nodes, "edges": edges}


def short_id(value) -> str:
    """Shorten UUID-style ids."""
    text = str(value)
    return f"{text[:8]}…{text[-4:]}" if len(text) > 20 else text


def order_summary(order: dict) -> str:
    """Route, update id, horizon and the shortened order id."""
    nodes = [n for n in order.get("nodes") or [] if isinstance(n, dict)]
    ids = [str(n.get("nodeId", "?")) for n in nodes]
    route = "(no nodes)" if not ids else ids[0] if len(ids) == 1 else f"{ids[0]} → {ids[-1]}"
    parts = [route, f"updateId {order.get('orderUpdateId', '?')}", f"{len(nodes)} node" + ("" if len(nodes) == 1 else "s")]
    released = sum(1 for n in nodes if n.get("released"))
    if released < len(nodes):
        parts.append(f"released {released}/{len(nodes)}")
    n_actions = len(order_actions(order))
    if n_actions:
        parts.append(f"{n_actions} action(s)")
    parts.append(short_id(order.get("orderId", "?")))
    return " · ".join(parts)


def instant_summary(msg: dict) -> str:
    """Action types with their blocking type, e.g. 'cancelOrder · HARD'."""
    actions = []
    for a in msg.get("actions") or []:
        if not isinstance(a, dict):
            continue
        kind = str(a.get("actionType", "?"))
        blocking = a.get("blockingType")
        actions.append(f"{kind} · {blocking}" if blocking else kind)
    return ", ".join(actions) if actions else "(no actions)"


def state_signature(s: RobotState) -> tuple:
    """Fields whose change is logged; excludes pose, battery and speed."""
    return (s.order_id, s.order_update_id, s.last_node_id, s.driving, s.paused, s.fatal_error,
            tuple((a.get("actionId"), a.get("actionStatus"))
                  for a in s.action_states if isinstance(a, dict)))


def state_summary(s: RobotState) -> str:
    parts = []
    if s.order_id:
        parts.append(f"updateId {s.order_update_id}")
    if s.last_node_id:
        parts.append(f"node {s.last_node_id}")
    parts.append("driving" if s.driving else "stopped")
    if s.paused:
        parts.append("PAUSED")
    for a in [a for a in s.action_states if isinstance(a, dict)][-2:]:
        parts.append(f"{a.get('actionType') or a.get('actionId')}: {a.get('actionStatus')}")
    if s.fatal_error:
        parts.append(f"FATAL {s.fatal_error}")
    return " · ".join(parts)

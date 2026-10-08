"""WebSocket hub: keyed robot patches filtered per viewer, change events, and RMF link status."""

import asyncio
import json
import logging
import time
from dataclasses import dataclass, field

from fastapi import WebSocket

log = logging.getLogger("eiu_web.realtime")


@dataclass(eq=False)
class Connection:
    ws: WebSocket
    user_id: str
    sees_fleet: bool
    # (robot) -> whether this viewer may see it; None shows every robot to a fleet viewer.
    visible: object = None
    sent: dict = field(default_factory=dict)
    seq: int = 0
    rmf: str = ""


class Hub:
    def __init__(self, period_ms: int):
        self.period_ms = period_ms
        self._connections: set[Connection] = set()
        # Topics for one user on the next broadcast, e.g. "access" after an admin changed the account.
        self._pending: dict[str, set[str]] = {}

    def notify_user(self, user_id: str, topic: str, sees_fleet: bool | None = None, visible=None) -> None:
        self._pending.setdefault(user_id, set()).add(topic)
        for conn in self._connections:
            if conn.user_id == user_id:
                if sees_fleet is not None:
                    conn.sees_fleet = sees_fleet
                if visible is not None:
                    conn.visible = visible

    def add(self, conn: Connection) -> None:
        self._connections.add(conn)

    def remove(self, conn: Connection) -> None:
        self._connections.discard(conn)

    def drop_user(self, user_id: str) -> list[Connection]:
        """Connections of a user whose sessions ended."""
        return [c for c in self._connections if c.user_id == user_id]

    async def broadcast(self, robots: list[dict], owner_of, changed: dict[str, set[str]], rmf_status: str,
                        fleet_changed: bool = False, alerts_changed: bool = False) -> None:
        pending, self._pending = self._pending, {}
        for conn in list(self._connections):
            topics = set(changed.get(conn.user_id, set())) | pending.get(conn.user_id, set())
            if fleet_changed and conn.sees_fleet:
                topics.add("fleet")
            if alerts_changed and conn.sees_fleet:
                topics.add("alerts")
            try:
                await self._send(conn, robots, owner_of, topics, rmf_status)
            except Exception as e:
                log.debug("dropping connection: %s", e)
                self.remove(conn)

    async def _send(self, conn: Connection, robots, owner_of, topics: set[str], rmf_status: str) -> None:
        if conn.rmf != rmf_status:
            conn.rmf = rmf_status
            await conn.ws.send_text(json.dumps({"type": "system", "rmf": rmf_status}))
        if conn.sees_fleet:
            visible = robots if conn.visible is None else [r for r in robots if conn.visible(r)]
        else:
            visible = [r for r in robots if owner_of(r["deliveryId"]) == conn.user_id]
        upsert, seen = [], set()
        for robot in visible:
            seen.add(robot["name"])
            encoded = json.dumps(robot, sort_keys=True)
            if conn.sent.get(robot["name"]) != encoded:
                conn.sent[robot["name"]] = encoded
                upsert.append(robot)
        remove = [name for name in conn.sent if name not in seen]
        for name in remove:
            del conn.sent[name]
        if upsert or remove:
            conn.seq += 1
            await conn.ws.send_text(json.dumps({"type": "patch", "topic": "robots", "seq": conn.seq,
                                                "periodMs": self.period_ms, "upsert": upsert, "remove": remove}))
        if topics:
            await conn.ws.send_text(json.dumps({"type": "event", "topics": sorted(topics)}))


async def run_ticker(service, operations, alerts, history, hub: Hub, period_s: float, maintenance=None) -> None:
    """Advance the tracker, the operations state, the alerts and the robot history, and push the result to every
    connection, every period_s."""
    while True:
        try:
            changed = await asyncio.to_thread(service.tick)
            fleet_changed = operations.tick()
            if maintenance is not None:
                fleet_changed = await asyncio.to_thread(maintenance.tick) or fleet_changed
            now = time.time()
            alerts_changed = await asyncio.to_thread(alerts.tick, service.rmf_status(), service._robots,
                                                     operations.metrics.attention(now),
                                                     operations.metrics.snapshot(now)["adapters"],
                                                     maintenance.due if maintenance is not None else ())
            await asyncio.to_thread(history.tick, operations.robots())
            if any(changed.values()):
                fleet_changed = True
            await hub.broadcast(service.robots_live(), service.owner_of, changed, service.rmf_status(), fleet_changed,
                                alerts_changed)
        except Exception:
            log.exception("tick failed")
        await asyncio.sleep(period_s)

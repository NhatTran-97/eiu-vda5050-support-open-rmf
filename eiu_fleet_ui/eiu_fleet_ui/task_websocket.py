"""Websocket server receiving RMF's task/fleet event broadcast.

The fleet adapter is the client here (rmf_websocket::BroadcastClient) -- it
connects OUT to the URI configured as vda5050.ui_websocket_uri and pushes one
JSON object per text frame, {"type": ..., "data": ...}, matching rmf_api_msgs'
task_state_update/task_log_update/fleet_state_update/fleet_log_update schemas.
Disabled (server never listens) when the adapter's config doesn't set that URI.

Runs on Qt's own event loop via QtWebSockets -- no separate thread, no new
dependency beyond PySide6 already being required.
"""

import json
from urllib.parse import urlparse

import shiboken6
from PySide6.QtCore import QObject, Signal, Property
from PySide6.QtNetwork import QHostAddress
from PySide6.QtWebSockets import QWebSocketServer


class TaskEventServer(QObject):
    """
    QML receives:
        wsTasks.connected      -> bool (whether the adapter is attached)
    Python receives:
        taskStateUpdate(dict)  -- task_state.json payload
        taskLogUpdate(dict)    -- task_log.json payload
    """

    taskStateUpdate = Signal(dict)
    taskLogUpdate = Signal(dict)
    connectedChanged = Signal()

    def __init__(self, uri: str | None, parent=None):
        super().__init__(parent)
        self._uri = uri
        self._server = None
        self._clients = []

    @Property(bool, notify=connectedChanged)
    def connected(self) -> bool:
        return bool(self._clients)

    def listen(self):
        if not self._uri:
            return
        parsed = urlparse(self._uri)
        port = parsed.port or 9000

        self._server = QWebSocketServer(
            "eiu_fleet_ui task events", QWebSocketServer.SslMode.NonSecureMode, self)
        self._server.newConnection.connect(self._on_new_connection)

        if self._server.listen(QHostAddress.SpecialAddress.Any, port):
            print(f"[WS] listening on :{port} for {self._uri}")
        else:
            print(f"[WS] listen on :{port} failed: {self._server.errorString()}")

    def _on_new_connection(self):
        client = self._server.nextPendingConnection()
        if client is None:
            return
        self._clients.append(client)
        client.textMessageReceived.connect(self._on_message)
        client.disconnected.connect(lambda c=client: self._on_disconnected(c))
        print(f"[WS] fleet adapter connected from {client.peerAddress().toString()}")
        self.connectedChanged.emit()

    def _on_disconnected(self, client):
        # A queued disconnect can still fire after this object's own C++
        # side (or the client's) is torn down during interpreter shutdown.
        if not shiboken6.isValid(self):
            return
        if client in self._clients:
            self._clients.remove(client)
        if shiboken6.isValid(client):
            client.deleteLater()
        print("[WS] fleet adapter disconnected")
        self.connectedChanged.emit()

    def _on_message(self, message: str):
        try:
            envelope = json.loads(message)
        except Exception:
            return
        if not isinstance(envelope, dict):
            return
        kind = envelope.get("type")
        data = envelope.get("data")
        if not isinstance(data, dict):
            return
        if kind == "task_state_update":
            self.taskStateUpdate.emit(data)
        elif kind == "task_log_update":
            self.taskLogUpdate.emit(data)

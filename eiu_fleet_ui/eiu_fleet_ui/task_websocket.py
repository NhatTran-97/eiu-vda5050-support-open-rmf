"""Receive RMF task and fleet events through a Qt WebSocket server."""

import json
import os
from urllib.parse import urlparse

import shiboken6
from PySide6.QtCore import QObject, Signal, Property
from PySide6.QtNetwork import QHostAddress
from PySide6.QtWebSockets import QWebSocketServer


# Port used when the adapter's URI names none.
DEFAULT_PORT = 9000
# Overrides the address the server listens on: an IP address, or "any" for every interface.
BIND_ENV = "EIU_WS_BIND"


def bind_address(uri: str, override: str | None = None) -> QHostAddress:
    """Where to listen: the override, else the host of the adapter's URI (loopback names stay on loopback)."""
    host = (override if override is not None else os.environ.get(BIND_ENV, "")).strip()
    if host.lower() == "any":
        return QHostAddress(QHostAddress.SpecialAddress.Any)
    if not host:
        host = urlparse(uri).hostname or ""
    if host.lower() == "localhost":
        return QHostAddress(QHostAddress.SpecialAddress.LocalHost)
    address = QHostAddress(host)
    if not address.isNull():
        return address
    # A host name names the machine the adapter connects to, not a local interface, so listen on all interfaces.
    return QHostAddress(QHostAddress.SpecialAddress.Any)


class TaskEventServer(QObject):
    """Expose RMF task events and connection state to QML."""

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
        port = urlparse(self._uri).port or DEFAULT_PORT
        address = bind_address(self._uri)

        self._server = QWebSocketServer(
            "eiu_fleet_ui task events", QWebSocketServer.SslMode.NonSecureMode, self)
        self._server.newConnection.connect(self._on_new_connection)

        if self._server.listen(address, port):
            print(f"[WS] listening on {address.toString()}:{port} for {self._uri}")
        else:
            print(f"[WS] listen on {address.toString()}:{port} failed: {self._server.errorString()}")

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

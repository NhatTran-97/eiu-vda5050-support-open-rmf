import io
import os
import sys
import threading
from pathlib import Path
from PySide6.QtWidgets import QApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtCore import QUrl, QTimer, qInstallMessageHandler, QtMsgType


def _qt_msg_handler(msg_type, _ctx, message):
    if "of null" in message:
        return
    prefix = {QtMsgType.QtWarningMsg: "Warning", QtMsgType.QtCriticalMsg: "Critical",
              QtMsgType.QtFatalMsg: "Fatal"}.get(msg_type, "")
    print(f"[Qt{prefix}] {message}" if prefix else message, file=sys.stderr)


def _suppress_rcutils_spam():
    """Filter rcutils DDS deserialization error blocks from fd-2 (C-level stderr).

    Jazzy RMF nodes on the same DDS domain publish FleetState with Jazzy's message
    serialization; Humble rclpy can't deserialize it, causing cascading rcutils
    error blocks.  Python sys.stderr redirect doesn't reach C fwrite(stderr) so we
    intercept at the file-descriptor level.
    """
    real_fd = os.dup(2)
    r_fd, w_fd = os.pipe()
    os.dup2(w_fd, 2)
    os.close(w_fd)

    def _run():
        suppressing = False
        with io.open(r_fd, 'rb', buffering=0) as rd, \
             io.open(real_fd, 'wb', buffering=0) as wr:
            buf = b""
            while True:
                chunk = rd.read(256)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line += b"\n"
                    if b">>> [rcutils" in line:
                        suppressing = True
                    if not suppressing:
                        wr.write(line)
                    if suppressing and b"<<<" in line:
                        suppressing = False

    threading.Thread(target=_run, daemon=True, name="stderr-filter").start()

from .colors import Colors
from .map_provider import MapProvider
from .mqtt_client import MqttClient
from .ros_bridge import RosBridge


def main():
    _suppress_rcutils_spam()   # must be before rclpy.init() inside RosBridge
    qInstallMessageHandler(_qt_msg_handler)
    app = QApplication(sys.argv)
    app.setApplicationName("EIU Fleet UI")

    # ── Backend objects ───────────────────────────────────────────────────────
    colors     = Colors()
    map_prov   = MapProvider()
    mqtt       = MqttClient()
    ros        = RosBridge()

    # ── QML engine + context properties ──────────────────────────────────────
    engine = QQmlApplicationEngine()
    ctx    = engine.rootContext()
    ctx.setContextProperty("C",       colors)
    ctx.setContextProperty("mapProv", map_prov)  # bản đồ + waypoints
    ctx.setContextProperty("mqtt",    mqtt)      # vị trí robot (MQTT)
    ctx.setContextProperty("ros",     ros)       # fleet_states + dispatch (RMF)

    # ── Load QML ─────────────────────────────────────────────────────────────

    _src = Path(__file__).parent.parent / "qml" / "main.qml"
    if _src.exists():
        qml_file = _src
    else:
        from ament_index_python.packages import get_package_share_directory
        qml_file = Path(get_package_share_directory("eiu_fleet_ui")) / "qml" / "main.qml"
    engine.load(QUrl.fromLocalFile(str(qml_file)))

    if not engine.rootObjects():
        sys.exit(-1)

    # ── Khởi động backend sau khi QML load xong ──────────────────────────────
    mqtt.connect_broker("localhost", 1883)
    ros.start()
    app.aboutToQuit.connect(ros.shutdown)   # tắt rclpy gọn gàng khi thoát

    # ── Tự chụp màn hình (debug): EIU_SHOT=/path.png → grab rồi thoát ─────────
    shot = os.environ.get("EIU_SHOT")
    if shot:
        delay = int(os.environ.get("EIU_SHOT_DELAY", "4000"))
        def _grab():
            try:
                win = engine.rootObjects()[0]
                screen = app.primaryScreen()
                img = screen.grabWindow(int(win.winId()))
                ok = img.save(shot)
                print(f"[SHOT] saved={ok} {shot} {img.width()}x{img.height()}")
            except Exception as e:
                print("[SHOT] error:", e)
            app.quit()
        QTimer.singleShot(delay, _grab)

    sys.exit(app.exec())


if __name__ == "__main__":
    main()

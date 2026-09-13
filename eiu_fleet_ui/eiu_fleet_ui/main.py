import io
import os
import sys
import threading
from pathlib import Path
from PySide6.QtGui import QFont, QFontDatabase, QIcon
from PySide6.QtWidgets import QApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtCore import QUrl, QTimer, qInstallMessageHandler, QtMsgType


_FONT_FILES = (
    "IBMPlexSans-Regular.ttf",
    "IBMPlexSans-SemiBold.ttf",
    "IBMPlexSans-Bold.ttf",
    "IBMPlexMono-Regular.ttf",
    "IBMPlexMono-SemiBold.ttf",
    "IBMPlexMono-Bold.ttf",
)


def _resource_dir(name: str) -> Path:
    """Resolve a bundled resource from source or an installed ROS package."""
    source_dir = Path(__file__).resolve().parent.parent / name
    if source_dir.exists():
        return source_dir

    from ament_index_python.packages import get_package_share_directory
    return Path(get_package_share_directory("eiu_fleet_ui")) / name


def _load_fonts(app: QApplication) -> tuple[str, str]:
    """Register the bundled IBM Plex families before the QML engine starts."""
    registered = []
    font_dir = _resource_dir("fonts")

    for filename in _FONT_FILES:
        font_id = QFontDatabase.addApplicationFont(str(font_dir / filename))
        if font_id < 0:
            print(f"[UI] unable to load font: {filename}", file=sys.stderr)
            continue
        registered.extend(QFontDatabase.applicationFontFamilies(font_id))

    sans = next((name for name in registered if name == "IBM Plex Sans"),
                "IBM Plex Sans")
    mono = next((name for name in registered if name == "IBM Plex Mono"),
                "IBM Plex Mono")

    app_font = QFont(sans)
    app_font.setPointSizeF(10.0)
    app.setFont(app_font)
    return sans, mono


def _qt_msg_handler(msg_type, _ctx, message):
    if "of null" in message:
        return
    prefix = {QtMsgType.QtWarningMsg: "Warning", QtMsgType.QtCriticalMsg: "Critical",
              QtMsgType.QtFatalMsg: "Fatal"}.get(msg_type, "")
    print(f"[Qt{prefix}] {message}" if prefix else message, file=sys.stderr)


def _suppress_rcutils_spam():
    """Filter rcutils DDS deserialization error blocks from fd-2 (C-level stderr).

    Jazzy RMF nodes on this DDS domain publish messages Humble rclpy can't
    deserialize, causing cascading rcutils error blocks; sys.stderr redirects
    don't reach C's fwrite(stderr), so this intercepts at the fd level instead.
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
from .config import FleetSettings, load_fleet_config
from .map_provider import MapProvider
from .mqtt_client import MqttClient
from .ros_bridge import RosBridge
from .ros_control import RosControl
from .task_websocket import TaskEventServer


def main():
    # Opt-in: takes over fd 2 for the whole process (hides real errors too), so
    # leave it off during bring-up; set EIU_FILTER_RCUTILS=1 once the log is understood.
    if os.environ.get("EIU_FILTER_RCUTILS") == "1":
        _suppress_rcutils_spam()
    qInstallMessageHandler(_qt_msg_handler)
    app = QApplication(sys.argv)
    app.setOrganizationName("EIU")
    app.setApplicationName("EIU Fleet UI")
    logo_dir = _resource_dir("logo")
    eiu_logo_path = logo_dir / "eiu_logo.png"
    # Window/taskbar icon only -- the in-app logo (eiuLogoUrl below) keeps
    # using eiu_logo.png untouched.
    app.setWindowIcon(QIcon(str(_resource_dir("icons") / "logo_desktop.png")))
    font_sans, font_mono = _load_fonts(app)

    # ── Backend objects ───────────────────────────────────────────────────────
    # Broker/VDA5050/task-category config comes from the adapter's config.yaml,
    # not restated here (see config.py for the resolution order).
    fleet_cfg  = load_fleet_config()
    print(f"[CFG] fleet '{fleet_cfg.fleet_name}' from {fleet_cfg.source}")

    colors     = Colors()
    settings   = FleetSettings(fleet_cfg)
    map_prov   = MapProvider(fleet_cfg)
    mqtt       = MqttClient(fleet_cfg)
    ros        = RosBridge()
    ros.set_fleet_name(fleet_cfg.fleet_name)
    control    = RosControl(fleet_cfg)
    ws_tasks   = TaskEventServer(fleet_cfg.websocket_uri)
    ws_tasks.taskStateUpdate.connect(ros.apply_task_state_update)

    # ── QML engine + context properties ──────────────────────────────────────
    engine = QQmlApplicationEngine()
    ctx    = engine.rootContext()
    ctx.setContextProperty("C",       colors)
    ctx.setContextProperty("cfg",     settings)   # fleet identity + task categories
    ctx.setContextProperty("mapProv", map_prov)  # map image + waypoints
    ctx.setContextProperty("mqtt",    mqtt)      # robot position (MQTT)
    ctx.setContextProperty("ros",     ros)       # fleet_states + dispatch (RMF)
    ctx.setContextProperty("control", control)   # pause/resume, speed limit, init_position
    ctx.setContextProperty("wsTasks", ws_tasks)  # authoritative task state (websocket)
    ctx.setContextProperty("fontSans", font_sans)
    ctx.setContextProperty("fontMono", font_mono)
    ctx.setContextProperty(
        "robotIconUrl",
        QUrl.fromLocalFile(str(logo_dir / "robot.png")),
    )
    ctx.setContextProperty("eiuLogoUrl", QUrl.fromLocalFile(str(eiu_logo_path)))

    # KPI tile logos -- distinct from robotIconUrl above (the on-map marker).
    icons_dir = _resource_dir("icons")
    ctx.setContextProperty(
        "statusActiveIconUrl",
        QUrl.fromLocalFile(str(icons_dir / "active.png")),
    )
    ctx.setContextProperty(
        "fleetRobotIconUrl",
        QUrl.fromLocalFile(str(icons_dir / "robot.png")),
    )

    # ── Load QML ─────────────────────────────────────────────────────────────

    qml_file = _resource_dir("qml") / "main.qml"
    engine.load(QUrl.fromLocalFile(str(qml_file)))

    if not engine.rootObjects():
        sys.exit(-1)

    # ── Start backend services once QML has finished loading ─────────────────
    ros.set_waypoints(map_prov.waypoints())
    mqtt.connect_broker()
    ros.start(on_node_ready=control.attach)
    ws_tasks.listen()
    app.aboutToQuit.connect(mqtt.disconnect_broker)
    app.aboutToQuit.connect(ros.shutdown)   # cleanly shut down rclpy on exit

    # ── Auto screenshot (debug): EIU_SHOT=/path.png → grab then quit ─────────
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

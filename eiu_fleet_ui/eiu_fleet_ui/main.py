import io
import os
import signal
import subprocess
import sys
import threading
from pathlib import Path
from types import SimpleNamespace

# Select the Qt Quick backend before importing Qt.
os.environ.setdefault("QT_QUICK_BACKEND", "software")

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


# Reference layout size for UI scaling.
REFERENCE_SIZE = (1920, 1080)
_SCREEN_PROBE = ("from PySide6.QtGui import QGuiApplication; a = QGuiApplication([]); "
                 "s = a.primaryScreen().availableSize(); print(s.width(), s.height())")


def ui_scale_for(width: int, height: int) -> float:
    """Uniform scale that fits REFERENCE_SIZE into a screen, never below 1."""
    return max(1.0, min(width / REFERENCE_SIZE[0], height / REFERENCE_SIZE[1]))


def apply_ui_scale() -> None:
    """Set the UI scale from EIU_UI_SCALE; auto probes the screen size."""
    if "QT_SCALE_FACTOR" in os.environ:
        return
    requested = os.environ.get("EIU_UI_SCALE", "").strip().lower()
    if not requested:
        return
    if requested != "auto":
        try:
            factor = float(requested)
        except ValueError:
            print(f"[UI] EIU_UI_SCALE={requested!r} is not a number -- ignoring", file=sys.stderr)
            return
    else:
        try:
            # Read screen dimensions before creating QApplication.
            out = subprocess.run([sys.executable, "-c", _SCREEN_PROBE], capture_output=True,
                                 text=True, timeout=15, check=True).stdout.split()
            factor = ui_scale_for(int(out[-2]), int(out[-1]))
        except Exception as exc:
            print(f"[UI] screen probe failed ({exc}) -- no UI scaling", file=sys.stderr)
            return
    if factor > 0 and abs(factor - 1.0) > 0.01:
        os.environ["QT_SCALE_FACTOR"] = f"{factor:.3f}"
        print(f"[UI] scale factor {factor:.3f}")


def _qt_msg_handler(msg_type, _ctx, message):
    if "of null" in message:
        return
    prefix = {QtMsgType.QtWarningMsg: "Warning", QtMsgType.QtCriticalMsg: "Critical",
              QtMsgType.QtFatalMsg: "Fatal"}.get(msg_type, "")
    print(f"[Qt{prefix}] {message}" if prefix else message, file=sys.stderr)


def _suppress_rcutils_spam():
    """Filter repeated rcutils deserialization errors from stderr."""
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
from .graph_editor import GraphEditor
from .map_provider import MapProvider
from .mqtt_client import MqttClient
from .ros_bridge import RosBridge
from .ros_control import RosControl
from .task_websocket import TaskEventServer


def build_engine(app: QApplication):
    """Create the backends and load main.qml without starting any I/O."""
    logo_dir = _resource_dir("logo")
    eiu_logo_path = logo_dir / "eiu_logo.png"
    font_sans, font_mono = _load_fonts(app)

    # Create backend services from the adapter configuration.
    fleet_cfg  = load_fleet_config()
    print(f"[CFG] fleet '{fleet_cfg.fleet_name}' from {fleet_cfg.source}")

    colors     = Colors()
    settings   = FleetSettings(fleet_cfg)
    map_prov   = MapProvider(fleet_cfg)
    mqtt       = MqttClient(fleet_cfg)
    ros        = RosBridge()
    ros.set_fleet_names(list(fleet_cfg.fleet_names))
    ros.set_robot_fleets({r.name: r.fleet_name for r in fleet_cfg.robots})
    control    = RosControl(fleet_cfg)
    ws_tasks   = TaskEventServer(fleet_cfg.websocket_uri)
    ws_tasks.taskStateUpdate.connect(ros.apply_task_state_update)
    graph_ed   = GraphEditor()

    # Expose backend objects to QML.
    engine = QQmlApplicationEngine()
    ctx    = engine.rootContext()
    ctx.setContextProperty("C",       colors)
    ctx.setContextProperty("cfg",     settings)   # Fleet and task settings
    ctx.setContextProperty("mapProv", map_prov)  # Map and waypoints
    ctx.setContextProperty("mqtt",    mqtt)      # Robot telemetry
    ctx.setContextProperty("ros",     ros)       # Fleet state and dispatch
    ctx.setContextProperty("control", control)   # Direct robot control
    ctx.setContextProperty("wsTasks", ws_tasks)  # Task state events
    ctx.setContextProperty("graphEd", graph_ed)  # nav_graph.yaml editor
    ctx.setContextProperty("fontSans", font_sans)
    ctx.setContextProperty("fontMono", font_mono)
    ctx.setContextProperty(
        "robotIconUrl",
        QUrl.fromLocalFile(str(logo_dir / "robot.png")),
    )
    ctx.setContextProperty("eiuLogoUrl", QUrl.fromLocalFile(str(eiu_logo_path)))

    # Logos used by the metric cards.
    icons_dir = _resource_dir("icons")

    # Choose each robot's map icon by manufacturer, with a generic fallback.
    _MANUFACTURER_ICON = {
        "ROBOTIS": icons_dir / "tb3_logo.png",
    }
    ctx.setContextProperty(
        "robotIconUrls",
        {
            r.name: QUrl.fromLocalFile(str(
                _MANUFACTURER_ICON.get(r.manufacturer, logo_dir / "robot.png")))
            for r in fleet_cfg.robots
        },
    )
    ctx.setContextProperty(
        "statusActiveIconUrl",
        QUrl.fromLocalFile(str(icons_dir / "active.png")),
    )
    ctx.setContextProperty(
        "fleetRobotIconUrl",
        QUrl.fromLocalFile(str(icons_dir / "robot.png")),
    )

    # Load the QML interface.

    qml_file = _resource_dir("qml") / "main.qml"
    engine.load(QUrl.fromLocalFile(str(qml_file)))

    # Keep backend objects alive for the lifetime of the QML engine -- a
    # context property with no surviving Python reference gets garbage
    # collected out from under QML (it then reads back as null), which is
    # exactly what happened to graph_ed before it was added here.
    backends = SimpleNamespace(colors=colors, settings=settings, map_prov=map_prov,
                               mqtt=mqtt, ros=ros, control=control, ws_tasks=ws_tasks,
                               graph_ed=graph_ed)
    return engine, backends


def main():
    # Filter stderr only when EIU_FILTER_RCUTILS is enabled.
    if os.environ.get("EIU_FILTER_RCUTILS") == "1":
        _suppress_rcutils_spam()
    qInstallMessageHandler(_qt_msg_handler)
    apply_ui_scale()
    app = QApplication(sys.argv)
    app.setOrganizationName("EIU")
    app.setApplicationName("EIU Fleet UI")
    # Icon for the application window.
    app.setWindowIcon(QIcon(str(_resource_dir("icons") / "logo_desktop.png")))

    engine, backends = build_engine(app)
    if not engine.rootObjects():
        sys.exit(-1)
    map_prov, mqtt, ros = backends.map_prov, backends.mqtt, backends.ros
    control, ws_tasks = backends.control, backends.ws_tasks

    # Start backend services after QML has loaded.
    ros.set_waypoints(map_prov.waypoints())
    mqtt.connect_broker()
    ros.start(on_node_ready=control.attach)
    ws_tasks.listen()
    app.aboutToQuit.connect(mqtt.disconnect_broker)
    app.aboutToQuit.connect(ros.shutdown)   # Shut down ROS on exit

    # Wake Python periodically so SIGINT is handled during the Qt event loop.
    signal.signal(signal.SIGINT, lambda *_: app.quit())
    _sigint_wakeup = QTimer()
    _sigint_wakeup.start(200)
    _sigint_wakeup.timeout.connect(lambda: None)

    # Capture a screenshot when EIU_SHOT is set.
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

#!/usr/bin/env python3
"""
Try runtime robot registration in a sandbox that cannot touch the real broker or RMF.

It starts a private MQTT broker, an RMF core, one fleet adapter per fleet config and a mock
robot for every robot in those configs. Robots listed in --dynamic are left out of the config,
so they can be added while the adapters run. Further mock robots wait on the broker, each
one built to break one of the registration checks.

  registration_sandbox.py up                start everything
  registration_sandbox.py restart-adapters  restart only the fleet adapters (robots added at runtime are reloaded)
  registration_sandbox.py status            show what runs and where the logs are
  registration_sandbox.py down              stop everything it started

Run the dashboard and scripts with the printed ROS_DOMAIN_ID and broker port. Poses, limits and
sizes come from the fleet configs and the nav graph, so nothing here is tied to a robot type.
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import yaml

PACKAGE = "vda5050_fleet_adapter_full_control"


def run_text(*command) -> str:
    return subprocess.run(command, capture_output=True, text=True, check=True).stdout.strip()


def share_dir() -> Path:
    return Path(run_text("ros2", "pkg", "prefix", "--share", PACKAGE))


def fleet_configs(directory: Path) -> list:
    """Paths of the fleet configs in a folder, skipping the runtime robot files that sit beside them."""
    found = []
    for path in sorted(directory.glob("config_*.yaml")):
        data = yaml.safe_load(path.read_text())
        if isinstance(data, dict) and "rmf_fleet" in data and "vda5050" in data:
            found.append(path)
    return found


def load_graph(path: Path) -> dict:
    """Waypoint name to (x, y), the map's name and the chargers, from an RMF nav graph."""
    data = yaml.safe_load(path.read_text())
    level_name, level = next(iter(data["levels"].items()))
    points, chargers = {}, []
    for vertex in level["vertices"]:
        properties = vertex[2] if len(vertex) > 2 else {}
        name = properties.get("name", "")
        if name:
            points[name] = (float(vertex[0]), float(vertex[1]))
            if properties.get("is_charger"):
                chargers.append(name)
    return {"map": level_name, "points": points, "chargers": chargers}


class Sandbox:
    def __init__(self, args):
        self.args = args
        self.dir = Path(args.dir)
        self.state_file = self.dir / "processes.json"
        self.share = share_dir()
        self.graph_file = Path(args.nav_graph) if args.nav_graph else self.share / "maps" / "nav_graph.yaml"
        self.graph = load_graph(self.graph_file)
        self.mock_script = Path(__file__).resolve().parent / "mock_mqtt_robot.py"
        self.env = dict(os.environ, ROS_DOMAIN_ID=str(args.domain))
        self.processes = []
        self.interface = ""
        # Charger of the first robot that was left out of a config, per fleet.
        self.dynamic_home = {}

    # Processes.

    def start(self, label: str, command: list, log: str = "") -> subprocess.Popen:
        log_path = self.dir / (log or f"{label}.log")
        with open(log_path, "wb") as sink:
            process = subprocess.Popen(command, stdout=sink, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                       env=self.env, start_new_session=True)
        self.processes.append({"label": label, "pid": process.pid, "log": str(log_path)})
        return process

    def save_state(self):
        self.state_file.write_text(json.dumps({"processes": self.processes, "domain": self.args.domain,
                                               "port": self.args.port, "docker": self.args.docker_name}))

    # Broker.

    def start_broker(self):
        if self.args.external_broker:
            print(f"using the broker already listening on port {self.args.port}")
            return
        conf = self.dir / "mosquitto.conf"
        conf.write_text(f"listener {self.args.port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
        if shutil.which("mosquitto"):
            self.start("broker", ["mosquitto", "-c", str(conf)])
        elif shutil.which("docker"):
            subprocess.run(["docker", "rm", "-f", self.args.docker_name], capture_output=True)
            subprocess.run(["docker", "run", "-d", "--name", self.args.docker_name, "--network", "host",
                            "-v", f"{conf}:/mosquitto/config/mosquitto.conf:ro", "eclipse-mosquitto:2.0"],
                           check=True, capture_output=True)
        else:
            sys.exit("no mosquitto and no docker to start a broker; install one or use --external-broker")
        time.sleep(1.5)

    # Fleet configs: the package's own, pointed at the sandbox broker.

    def write_configs(self) -> list:
        configs = []
        for source in fleet_configs(self.share / "config"):
            config = yaml.safe_load(source.read_text())
            config["vda5050"]["mqtt"].update({"host": "localhost", "port": self.args.port,
                                              "username": None, "password": None})
            for name in self.args.dynamic:
                left_out = config["rmf_fleet"].get("robots", {}).pop(name, None)
                config["vda5050"].get("robots", {}).pop(name, None)
                if left_out:
                    self.dynamic_home.setdefault(config["rmf_fleet"]["name"], left_out["charger"])
            target = self.dir / source.name
            target.write_text(yaml.safe_dump(config))
            for stale in self.dir.glob(f"{source.stem}.runtime_robots.yaml*"):
                stale.unlink()
            configs.append((target, config))
        return configs

    # Mock robots.

    def mock(self, log: str, identity: tuple, pose: tuple, **options):
        command = [sys.executable, "-u", str(self.mock_script), "--host", "localhost", "--port", str(self.args.port),
                   "--interface", self.interface, "--map", self.graph["map"], "--manufacturer", identity[0],
                   "--serial", identity[1], "--x", f"{pose[0]:.4f}", "--y", f"{pose[1]:.4f}", "--theta", "0.0"]
        for flag, value in options.items():
            flag = "--" + flag.replace("_", "-")
            if value is True:
                command.append(flag)
            elif value is not False and value is not None:
                command += [flag, str(value)]
        self.start(f"mock:{log}", command, f"mock_{log}.log")

    @staticmethod
    def fleet_traits(config: dict) -> dict:
        """Give what a mock of this fleet's robot type declares in its factsheet."""
        radius = float(config["rmf_fleet"]["profile"]["footprint"])
        speed, acceleration = (float(v) for v in config["rmf_fleet"]["limits"]["linear"])
        size = round(radius * 1.2, 3)   # half the diagonal is 0.85 of the footprint radius
        return {"series": f"{config['rmf_fleet']['name']} robot", "speed_max": speed, "accel_max": acceleration,
                "length": size, "width": size, "radius": radius}

    def start_mocks(self, configs: list):
        used = set()
        for path, config in configs:
            traits = self.fleet_traits(config)
            fleet = config["rmf_fleet"]["name"]
            for name, robot in config["vda5050"].get("robots", {}).items():
                charger = config["rmf_fleet"]["robots"][name]["charger"]
                pose = self.graph["points"][charger]
                identity = (robot["manufacturer"], str(robot["serial"]))
                used.add(identity)
                self.mock(name, identity, pose, series=traits["series"], speed_max=traits["speed_max"],
                          accel_max=traits["accel_max"], length=traits["length"], width=traits["width"],
                          start_node=charger)
                print(f"mock {name}: {identity[0]}/{identity[1]} at {charger}, in {fleet}")

        # Robots waiting to be registered, each built to fail one check; they go to the fleet a robot was left out of, else the first.
        _, first = next((c for c in configs if c[1]["rmf_fleet"]["name"] in self.dynamic_home), configs[0])
        traits = self.fleet_traits(first)
        robots = first["vda5050"].get("robots") or {}
        maker = next(iter(robots.values()))["manufacturer"] if robots else "SANDBOX"
        taken = {r["charger"] for r in first["rmf_fleet"].get("robots", {}).values()}
        free = [c for c in self.graph["chargers"] if c not in taken]
        home = self.graph["points"][self.dynamic_home.get(first["rmf_fleet"]["name"]) or (free or self.graph["chargers"])[0]]
        others = [p for n, p in self.graph["points"].items() if n not in self.graph["chargers"]] or [home]
        far = (max(x for x, _ in self.graph["points"].values()) + 1000.0, 0.0)

        common = {"series": traits["series"], "speed_max": traits["speed_max"], "accel_max": traits["accel_max"],
                  "length": traits["length"], "width": traits["width"]}
        candidates = [
            ("good", home, {}, "a robot of the fleet's type standing on a free charger"),
            ("off_graph", far, {}, "the right type, far from the nav graph"),
            ("slow", others[0], {"speed_max": round(traits["speed_max"] * 0.4, 3)}, "slower than the fleet plans for"),
            ("large", others[min(1, len(others) - 1)], {"length": round(traits["radius"] * 5, 3),
                                                        "width": round(traits["radius"] * 4, 3)}, "too big for the footprint"),
            ("no_factsheet", others[min(2, len(others) - 1)], {"no_factsheet": True}, "publishes no factsheet"),
            ("not_localized", others[min(3, len(others) - 1)], {"pose_uninitialized": True}, "pose not initialized (needs confirmation)"),
            ("other_type", others[min(4, len(others) - 1)], {"series": "another type of robot", "kinematic": "OMNI"},
             "a different type"),
        ]
        for index, (label, pose, changes, why) in enumerate(candidates, start=1):
            identity = (maker, f"S{9000 + index}")
            assert identity not in used
            self.mock(label, identity, pose, **{**common, **changes})
            print(f"waiting on the broker: {identity[0]}/{identity[1]} ({label}: {why})")

    def start_adapters(self, configs: list):
        for path, config in configs:
            fleet = config["rmf_fleet"]["name"]
            self.start(f"adapter:{fleet}",
                       ["ros2", "launch", PACKAGE, "fleet_adapter.launch.py", f"config_file:={path}",
                        f"nav_graph:={self.graph_file}", f"node_name:=sandbox_adapter_{fleet}"],
                       f"adapter_{fleet}.log")

    @staticmethod
    def stop_group(pid: int, wait: float = 2.0):
        for sig in (signal.SIGINT, signal.SIGKILL):
            try:
                os.killpg(pid, sig)
            except OSError:
                return
            time.sleep(wait if sig == signal.SIGINT else 0)

    # Commands.

    def up(self):
        self.dir.mkdir(parents=True, exist_ok=True)
        if self.state_file.exists():
            print("stopping the previous sandbox first")
            Sandbox(self.args).down()
        self.processes = []
        self.start_broker()
        configs = self.write_configs()
        self.interface = configs[0][1]["vda5050"]["interface_name"]

        self.start_mocks(configs)
        self.start("rmf_traffic_schedule", ["ros2", "run", "rmf_traffic_ros2", "rmf_traffic_schedule"])
        self.start("rmf_task_dispatcher", ["ros2", "run", "rmf_task_ros2", "rmf_task_dispatcher"])
        time.sleep(3)
        self.start_adapters(configs)
        self.save_state()

        print(f"""
Sandbox is up (logs in {self.dir}).
Use the same settings in every terminal that talks to it:

    export ROS_DOMAIN_ID={self.args.domain}
    ros2 run {PACKAGE} register_robot.py discovered      # after about {self.args.wait} s
    ros2 run {PACKAGE} register_robot.py list

Dashboard against the sandbox, from the eiu_fleet_ui directory (tasks it creates end up in your
usual task history unless you also set HOME to another folder):

    ROS_DOMAIN_ID={self.args.domain} EIU_MQTT_HOST=localhost EIU_MQTT_PORT={self.args.port} \\
    EIU_FLEET_ADAPTERS="{','.join(f'{p}=sandbox_adapter_{c["rmf_fleet"]["name"]}' for p, c in configs)}" \\
    python3 -m eiu_fleet_ui.main

Stop it with:  {Path(sys.argv[0]).name} down""")

    def restart_adapters(self):
        if not self.state_file.exists():
            sys.exit("no sandbox is running")
        state = json.loads(self.state_file.read_text())
        self.processes = [e for e in state["processes"] if not e["label"].startswith("adapter:")]
        for entry in state["processes"]:
            if entry["label"].startswith("adapter:"):
                self.stop_group(entry["pid"])
        configs = [(path, yaml.safe_load(path.read_text())) for path in fleet_configs(self.dir)]
        self.start_adapters(configs)
        self.save_state()
        print("fleet adapters restarted; they load robots added at runtime from their runtime files")

    def status(self):
        if not self.state_file.exists():
            print("no sandbox is running")
            return
        state = json.loads(self.state_file.read_text())
        for entry in state["processes"]:
            try:
                os.kill(entry["pid"], 0)
                alive = "running"
            except OSError:
                alive = "stopped"
            print(f"{alive:8s} {entry['label']:34s} {entry['log']}")

    def down(self):
        if not self.state_file.exists():
            print("no sandbox is running")
            return
        state = json.loads(self.state_file.read_text())
        for entry in reversed(state["processes"]):
            try:
                os.killpg(entry["pid"], signal.SIGINT)
            except OSError:
                pass
        time.sleep(2)
        for entry in state["processes"]:
            self.stop_group(entry["pid"], 0)
        if state.get("docker") and shutil.which("docker"):
            subprocess.run(["docker", "rm", "-f", state["docker"]], capture_output=True)
        self.state_file.unlink()
        print("sandbox stopped")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["up", "restart-adapters", "status", "down"])
    parser.add_argument("--dir", default=os.environ.get("SANDBOX_DIR", "/tmp/registration_sandbox"),
                        help="where configs, logs and state go")
    parser.add_argument("--port", type=int, default=int(os.environ.get("SANDBOX_PORT", "18830")), help="broker port")
    parser.add_argument("--domain", type=int, default=int(os.environ.get("SANDBOX_DOMAIN", "77")), help="ROS_DOMAIN_ID")
    parser.add_argument("--dynamic", nargs="*", default=["tb3_2"],
                        help="robots to take out of the configs so that they can be added while running")
    parser.add_argument("--nav-graph", help="nav graph to use instead of the package's")
    parser.add_argument("--external-broker", action="store_true", help="do not start a broker; one listens on --port")
    parser.add_argument("--docker-name", default="registration_sandbox_broker")
    parser.add_argument("--wait", type=int, default=12, help="seconds until discovery reports the waiting robots")
    args = parser.parse_args()
    sandbox = Sandbox(args)
    getattr(sandbox, args.command.replace("-", "_"))()


if __name__ == "__main__":
    main()

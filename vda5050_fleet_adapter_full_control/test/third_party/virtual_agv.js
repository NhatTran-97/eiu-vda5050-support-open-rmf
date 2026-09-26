// VDA5050 2.1 AGV from vda-5050-lib (VirtualAgvAdapter), configured through environment variables.
const { AgvController, VirtualAgvAdapter } = require("vda-5050-lib");

const env = (name, fallback) => process.env[name] ?? fallback;

const controller = new AgvController(
  { manufacturer: env("AGV_MANUFACTURER", "THIRDPARTY"), serialNumber: env("AGV_SERIAL", "0001") },
  { interfaceName: env("AGV_INTERFACE", "AMR"), transport: { brokerUrl: env("AGV_BROKER", "mqtt://127.0.0.1:1883") }, vdaVersion: "2.1.0" },
  { agvAdapterType: VirtualAgvAdapter },
  {
    initialPosition: {
      mapId: env("AGV_MAP", "tb3_world"),
      x: Number(env("AGV_X", "0")),
      y: Number(env("AGV_Y", "0")),
      theta: Number(env("AGV_THETA", "0")),
      lastNodeId: env("AGV_NODE", "A"),
    },
    vehicleSpeed: Number(env("AGV_SPEED", "2")),
  });

controller.start()
  .then(() => console.log("virtual AGV online"))
  .catch((error) => { console.error(error); process.exit(1); });

for (const signal of ["SIGINT", "SIGTERM"]) {
  process.on(signal, () => controller.stop().finally(() => process.exit(0)));
}

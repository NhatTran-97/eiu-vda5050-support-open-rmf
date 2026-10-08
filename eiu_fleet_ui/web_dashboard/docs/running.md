# Chạy EIU Robot Services

Hai cách chạy:

| Chế độ | Cần gì | Dùng khi |
|---|---|---|
| **Demo** | Node.js, chỉ frontend | Xem và thử giao diện; dữ liệu giả lập chạy ngay trong trình duyệt |
| **Chạy thật** | Docker (image Jazzy), MQTT broker, robot hoặc mô phỏng | Điều khiển robot qua Open-RMF và fleet adapter VDA5050 |

Luồng của hệ thống khi chạy thật:

```
Trình duyệt ──REST/WebSocket──▶ Vite proxy ──▶ Backend (FastAPI, :8000)
                                                   │ Redis (:6379)
                                                   ▼
                                       eiu_rmf_gateway (ROS 2 node)
                                                   │ ROS 2 (ROS_DOMAIN_ID=10)
                                                   ▼
                                Open-RMF + vda5050_fleet_adapter_full_control
                                                   │ MQTT (VDA5050)
                                                   ▼
                                    Robot (vda5050_client_adapter + Nav2)
```

## 1. Demo (chỉ giao diện)

Lần đầu, cài thư viện:

```bash
cd ~/ros2_ws/src/eiu_fleet_ui/web_dashboard/frontend
npm ci
```

Chạy:

```bash
cd ~/ros2_ws/src/eiu_fleet_ui/web_dashboard/frontend
npm run dev                     # http://127.0.0.1:5173, Ctrl+C để dừng
```

Mở `http://127.0.0.1:5173`, chọn một tài khoản trong **Tài khoản demo** rồi bấm **Dùng**:

| Tài khoản | Mật khẩu | Vai trò |
|---|---|---|
| `admin@eiu.edu.vn` | `admin1234` | Quản trị viên |
| `operator.a@eiu.edu.vn` | `demo1234` | Operator: giao vận, vệ sinh |
| `operator.b@eiu.edu.vn` | `demo1234` | Operator: giao vận |
| `operator.c@eiu.edu.vn` | `demo1234` | Operator: tuần tra |

- Dữ liệu demo lưu trong `localStorage` của trình duyệt. **Tài khoản → Dữ liệu demo → Đặt lại dữ liệu demo** tạo lại từ đầu.
- Chế độ demo chỉ có dữ liệu khi mở trên `localhost` hoặc HTTPS. Mở từ thiết bị khác qua IP mạng LAN thì trang tải được nhưng không có dữ liệu.

Chạy dev server trong Docker thay cho Node trên máy:

```bash
cd ~/ros2_ws/src/eiu_fleet_ui/web_dashboard/frontend
docker run -d --name eiu_web_dev --network host -u $(id -u):$(id -g) -e HOME=/tmp \
  -v $PWD:/app -w /app node:20-alpine npx vite --host 127.0.0.1 --port 5173 --strictPort
docker logs -f eiu_web_dev      # xem log
docker rm -f eiu_web_dev        # dừng và xóa
```

Các lệnh khác của frontend:

| Lệnh | Tác dụng |
|---|---|
| `npm test` | Unit test (Vitest) |
| `npm run typecheck` | Kiểm tra TypeScript |
| `npm run build` | Build bản production vào `dist/` |
| `npm run preview` | Chạy thử `dist/` ở `http://127.0.0.1:4173` |

## 2. Chạy thật

Thứ tự khởi động: broker và robot → container Jazzy (Open-RMF, fleet adapter, Redis, gateway, backend) → frontend.

### 2.1 MQTT broker và robot

Broker Mosquitto phải chạy trước, ở đúng địa chỉ và cổng ghi trong file cấu hình fleet adapter (`vda5050_fleet_adapter_full_control/config/*.yaml`, mục `vda5050:`).

```bash
mosquitto -p 1883 -v            # broker không mã hóa, cho mạng phòng lab
```

Broker có TLS và tài khoản riêng cho từng robot: xem [fleet_bringup/README.md](../../../fleet_bringup/README.md#secure-broker).

Phía robot chạy `vda5050_client_adapter`, `tb3_vda5050_bridge` và Nav2 (xem README của hai package đó). Nếu không có robot thật, chọn một cách ở [mục 3](#3-chạy-thật-không-có-robot).

### 2.2 Container Jazzy

Image `rmf_jazzy_vda` đã có sẵn Open-RMF, fleet adapter, Redis, gateway và thư viện của backend. `run.sh` tự build image nếu chưa có, rồi mở shell trong container `rmf_jazzy_vda_dev` (mạng host, `ROS_DOMAIN_ID=10`, workspace gắn vào `/ros2_ws`):

```bash
~/ros2_ws/src/vda5050_fleet_adapter_full_control/docker/run.sh
```

Mở thêm terminal vào cùng container:

```bash
docker exec -it rmf_jazzy_vda_dev bash
```

Build lại image từ đầu (khi đổi `Dockerfile` hoặc `requirements.txt` của backend):

```bash
docker rm -f rmf_jazzy_vda_dev 2>/dev/null
docker rmi rmf_jazzy_vda
~/ros2_ws/src/vda5050_fleet_adapter_full_control/docker/build.sh --no-cache
```

### 2.3 Open-RMF và fleet adapter (trong container)

Build một lần (hoặc sau khi sửa code):

```bash
cd /ros2_ws
colcon build --packages-select vda5050_fleet_adapter_full_control eiu_rmf_gateway fleet_bringup
source install/setup.bash
```

Cách gọn nhất là `fleet_bringup`: một lệnh chạy lõi RMF (traffic schedule, task dispatcher), fleet adapter và dispenser/ingestor giả lập cho nhiệm vụ giao hàng. `use_ui:=false` vì đã dùng web dashboard thay cho UI Qt:

```bash
ros2 launch fleet_bringup fleet_bringup.launch.py use_ui:=false
```

Muốn chọn file cấu hình fleet cụ thể, chạy adapter riêng (mỗi fleet một terminal):

```bash
ros2 launch vda5050_fleet_adapter_full_control fleet_adapter.launch.py \
    config_file:=/ros2_ws/src/vda5050_fleet_adapter_full_control/config/config_tb3.yaml \
    node_name:=vda5050_fleet_adapter_tb3
```

Fleet adapter cần thêm `mutex_group_supervisor` của Open-RMF cho các hành lang có mutex. Chi tiết ở [README của fleet adapter](../../../vda5050_fleet_adapter_full_control/README.md).

### 2.4 Redis và gateway (trong container)

```bash
redis-server --bind 127.0.0.1 --port 6379 --daemonize yes      # mỗi lần khởi động container
ros2 launch eiu_rmf_gateway gateway.launch.py \
    nav_graph_path:=/ros2_ws/src/vda5050_fleet_adapter_full_control/maps/nav_graph.yaml
```

Kiểm tra gateway đã nhận dữ liệu:

```bash
redis-cli get eiu:rmf:gateway           # heartbeat của gateway
redis-cli hkeys eiu:rmf:fleets          # các fleet đã thấy trên /fleet_states
redis-cli hgetall eiu:rmf:controls      # robot mà gateway điều khiển được
```

### 2.5 Backend (trong container)

Tạo tài khoản lần đầu (backend không có tài khoản mặc định):

```bash
cd /ros2_ws/src/eiu_fleet_ui/web_dashboard/backend
python3 -m eiu_web_backend create-user --email admin@eiu.edu.vn --name "Admin" --role admin
python3 -m eiu_web_backend create-user --email op@eiu.edu.vn --name "Operator" --services delivery,cleaning --zones building_a
```

Chạy backend (lắng nghe ở `127.0.0.1:8000`):

```bash
python3 -m eiu_web_backend serve
```

| Lệnh tài khoản | Tác dụng |
|---|---|
| `create-user` | Tạo tài khoản (`--role admin` hoặc operator với `--services`, `--zones`) |
| `set-password` | Đặt lại mật khẩu; đăng xuất mọi phiên của người đó |
| `disable-user` | Khóa tài khoản |
| `list-users` | Liệt kê tài khoản |

Cấu hình backend nằm ở `backend/config/settings.yaml`. Các điểm trong danh mục địa điểm phải có trong nav graph mà fleet adapter dùng (`map.nav_graph`, hoặc biến `EIU_WEB_NAV_GRAPH`).

Kiểm tra liên kết với Open-RMF:

```bash
curl -s http://127.0.0.1:8000/api/v1/status
```

`online` là đã có heartbeat gateway và ít nhất một fleet báo trạng thái; `offline` là có gateway nhưng chưa có fleet nào; `unavailable` là không có gateway hoặc Redis. Khi `unavailable`, tạo nhiệm vụ mới sẽ trả lỗi 503.

### 2.6 Frontend nối với backend (trên máy host)

```bash
cd ~/ros2_ws/src/eiu_fleet_ui/web_dashboard/frontend
npm run dev:backend             # http://127.0.0.1:5173, /api và /ws chuyển tới backend :8000
```

Mở cho thiết bị khác trong mạng LAN (backend vẫn chỉ nghe trên `127.0.0.1`):

```bash
npm run dev:lan                 # http://<IP máy>:5173
```

Đăng nhập bằng tài khoản đã tạo ở bước 2.5.

## 3. Chạy thật không có robot

Mọi bước ở mục 2 giữ nguyên, chỉ thay phần robot ở 2.1 bằng một trong các cách sau.

**Robot ảo VDA5050** (thư viện vda-5050-lib): broker và robot ảo chạy trên host, adapter chạy trong container:

```bash
# Host: broker
/usr/sbin/mosquitto -p 18830 -v

# Host: một robot ảo cho mỗi trạm sạc
~/ros2_ws/src/vda5050_fleet_adapter_full_control/test/third_party/run_virtual_fleet.sh

# Container: fleet adapter với cấu hình robot ảo
ros2 launch vda5050_fleet_adapter_full_control fleet_adapter.launch.py \
    config_file:=/ros2_ws/src/vda5050_fleet_adapter_full_control/test/third_party/config_virtual_agv.yaml \
    node_name:=vda5050_fleet_adapter_virtual
```

**Mô phỏng Gazebo** (3 TurtleBot3, Nav2): chạy `mosquitto -p 1883`, rồi mô phỏng và các bridge theo [tb3_simulation/README.md](../../../tb3_simulation/README.md). Sau đó chạy adapter với cấu hình mô phỏng:

```bash
ros2 launch vda5050_fleet_adapter_full_control fleet_adapter.launch.py \
    config_file:=/ros2_ws/src/vda5050_fleet_adapter_full_control/config/config_tb3_sim.yaml \
    node_name:=vda5050_fleet_adapter_tb3
```

**Thử đăng ký robot lúc đang chạy** (sandbox tự có broker, RMF, các fleet và robot giả):

```bash
ros2 run vda5050_fleet_adapter_full_control registration_sandbox.py up        # status | restart-adapters | down
```

## 4. Dừng hệ thống

| Thành phần | Cách dừng |
|---|---|
| Frontend (`npm run dev`, `dev:backend`) | Ctrl+C trong terminal |
| Backend, gateway, launch ROS | Ctrl+C trong từng terminal của container |
| Redis | `redis-cli shutdown` |
| Container Jazzy | `exit` ở terminal `run.sh` (container tự xóa khi thoát) |
| Dev server Docker | `docker rm -f eiu_web_dev` |

## 5. Cổng và địa chỉ

| Thành phần | Địa chỉ |
|---|---|
| Frontend (dev) | `http://127.0.0.1:5173` |
| Frontend (preview bản build) | `http://127.0.0.1:4173` |
| Backend | `http://127.0.0.1:8000` (`/api/v1`, `/ws`) |
| Redis | `127.0.0.1:6379`, prefix khóa `eiu:rmf` |
| MQTT broker | theo file cấu hình fleet (mục `vda5050:`); ví dụ `1883`, robot ảo dùng `18830` |
| ROS 2 | `ROS_DOMAIN_ID=10` (đặt trong `docker/run.sh`) |

Topic và dịch vụ ROS của adapter, cùng task API của RMF, mở cho mọi node trong cùng `ROS_DOMAIN_ID`. Hãy chạy trên mạng riêng, hoặc dùng SROS2 để giới hạn truy cập.

# Web Dashboard — Hướng dẫn cài đặt, chạy và sử dụng

Web dashboard **EIU Robot Services** là nền tảng vận hành robot cho khuôn viên EIU: operator và admin tạo nhiệm vụ giao vận, vệ sinh, tuần tra, theo dõi robot trên bản đồ, xử lý cảnh báo, lên lịch bảo trì và xem phân tích. Có hai chế độ chạy:

| Chế độ | Lệnh | Dữ liệu |
|---|---|---|
| Demo | `npm run dev` | Backend giả trong trình duyệt (6 robot mô phỏng: 3 giao vận, 2 vệ sinh, 1 tuần tra). Không cần ROS |
| Thật | `npm run dev:backend` | Backend `web_dashboard/backend` nối Open-RMF Jazzy và `vda5050_fleet_adapter_full_control`, điều phối robot thật (mục 6) |

Tài liệu kỹ thuật (kiến trúc, contract API, realtime, bảo mật): [../README.md](../README.md). Toàn bộ lệnh chạy hệ thống theo thứ tự, cả demo lẫn chạy thật: [../docs/running.md](../docs/running.md).

---

## 1. Cài đặt

Yêu cầu: Node.js **20.19 trở lên** và npm.

### Cách A — Cài Node.js trực tiếp trên máy (khuyên dùng)

Ubuntu 22.04 chỉ có Node.js 12 trong apt, quá cũ. **Không dùng `apt install nodejs`.** Cài Node.js 22 LTS bằng nvm (cài vào thư mục home, không cần sudo, không ảnh hưởng ROS):

```bash
curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.3/install.sh | bash
source ~/.bashrc
nvm install 22
node --version          # phải >= v20.19

cd ~/ros2_ws/src/eiu_fleet_ui/web_dashboard/frontend
npm ci                  # cài đúng phiên bản trong package-lock.json vào node_modules/
```

### Cách B — Dùng Docker

Dùng image chính thức `node:20-alpine` (không tự build image). Container mount thư mục `frontend/` vào `/app`, nên code và `node_modules/` vẫn nằm trên máy.

```bash
cd ~/ros2_ws/src/eiu_fleet_ui/web_dashboard/frontend
docker run --rm -u $(id -u):$(id -g) -e HOME=/tmp -v $PWD:/app -w /app node:20-alpine npm ci
```

`node_modules/` chứa bản biên dịch của Vite/Tailwind cho cả Ubuntu (glibc) và Alpine (musl), nên cài một lần dùng được cho cả hai cách.

---

## 2. Chạy

### Trên máy (cách A)

```bash
cd ~/ros2_ws/src/eiu_fleet_ui/web_dashboard/frontend
npm run dev             # mở http://127.0.0.1:5173 — Ctrl+C để dừng
```

| Lệnh | Tác dụng |
|---|---|
| `npm run dev` | Chạy dev server; sửa code thì trình duyệt tự cập nhật |
| `npm test` | Chạy unit test |
| `npm run typecheck` | Kiểm tra TypeScript |
| `npm run build` | Kiểm tra kiểu + build bản production vào `dist/` |
| `npm run preview` | Chạy thử bản `dist/` ở http://127.0.0.1:4173 |
| `npm run export-site` | Tạo lại dữ liệu bản đồ (xem mục 5) |

### Trong Docker (cách B)

```bash
cd ~/ros2_ws/src/eiu_fleet_ui/web_dashboard/frontend
docker run -d --name eiu_web_dev --network host -u $(id -u):$(id -g) -e HOME=/tmp \
  -v $PWD:/app -w /app node:20-alpine npx vite --host 127.0.0.1 --port 5173 --strictPort
```

| Lệnh | Tác dụng |
|---|---|
| `docker logs -f eiu_web_dev` | Xem log dev server |
| `docker exec -it eiu_web_dev sh` | Vào container (thư mục `/app`), chạy `npm test`… |
| `docker stop eiu_web_dev` / `docker start eiu_web_dev` | Dừng / bật lại |
| `docker rm -f eiu_web_dev` | Xóa container |

Hai cách dùng chung cổng 5173 — chỉ chạy một cách tại một thời điểm.

### Mở từ máy khác hoặc điện thoại

Thêm `--host 0.0.0.0` (`npm run dev -- --host 0.0.0.0`) rồi vào `http://<IP máy>:5173`. Backend giả chạy trong service worker, trình duyệt chỉ cho phép trên `localhost` hoặc HTTPS, nên trên thiết bị khác trang hiện được nhưng **không có dữ liệu**. Để xem giao diện điện thoại, dùng chế độ thiết bị của trình duyệt trên máy dev: **F12 → Ctrl+Shift+M**.

### Cấu hình

| Biến trong `.env` | Mặc định | Tác dụng |
|:---:|:---:|---|
| `VITE_USE_MOCKS` | `true` | Bật backend giả trong trình duyệt |
| `VITE_API_PROXY` | `http://127.0.0.1:8000` | Địa chỉ backend thật cho `/api`, `/ws` khi `VITE_USE_MOCKS=false` |

Thời gian và giới hạn của backend giả (tốc độ robot, thời gian chờ bỏ hàng/nhận hàng, số đơn mở tối đa, thông tin hỗ trợ) nằm trong `src/mocks/settings.ts`.

---

## 3. Sử dụng

### Đăng nhập

Trang đăng nhập có ô **Tài khoản demo**; bấm **Dùng** để điền sẵn. Chỉ có hai vai trò: **Operator** (vận hành các dịch vụ được cấp) và **Admin** (toàn quyền).

| Tài khoản | Vai trò | Dịch vụ | Khu vực |
|---|---|---|---|
| `admin@eiu.edu.vn` / `admin1234` | Admin | Tất cả | Tất cả |
| `operator.a@eiu.edu.vn` / `demo1234` | Operator | Giao vận, Vệ sinh | Tòa A, Tòa B |
| `operator.b@eiu.edu.vn` / `demo1234` | Operator | Giao vận | Tòa A |
| `operator.c@eiu.edu.vn` / `demo1234` | Operator | Tuần tra | Tất cả |

### Dữ liệu demo

| Dữ liệu | Nội dung |
|---|---|
| Đội robot | `eiu_delivery` (DEL-01..03), `eiu_cleaning` (CLN-01, CLN-02), `eiu_patrol` (PAT-01) |
| Khu vực | Tòa A (FABLAB, Thư viện, Trung tâm SV, Căng tin), Tòa B (Phòng 204, 118, 312, Hành chính); vùng vệ sinh Sảnh A, Khu đọc Thư viện, Hành lang Tòa B; tuyến tuần tra Hành lang Tòa A, Dãy phòng Tòa B |
| Nhiệm vụ đang chạy | D-1048, FABLAB → Phòng 204, DEL-01, ưu tiên cao |
| Nhiệm vụ hẹn giờ | D-1049, C-1050 (lặp hằng ngày), P-1053 (lặp các ngày trong tuần) |
| Cảnh báo | CLN-02 pin yếu, CLN-01 quá hạn thay chổi, một cảnh báo fleet adapter |
| Bảo trì | CLN-01 thay chổi (quá hạn), PAT-01 kiểm tra camera (khung bảo trì ngày mai) |

Dữ liệu lưu trong `localStorage`; **Tài khoản → Dữ liệu demo → Đặt lại dữ liệu demo** để tạo lại. Robot demo báo telemetry mô phỏng (bình nước, chổi, camera); backend thật chỉ hiện giá trị robot báo, còn lại ghi "Chưa báo".

### Các mục

| Mục | Route | Nội dung |
|---|---|---|
| Tổng quan | `/overview` | 4 KPI (robot trực tuyến, nhiệm vụ đang chạy, robot sẵn sàng, cần xử lý); cột trái: bản đồ trực tiếp và dịch vụ robot; cột phải: cần xử lý và lịch hôm nay; dưới cùng: hoạt động hôm nay |
| Vận hành trực tiếp | `/live-operations` | Trạng thái "Trực tiếp · cập nhật" ở góc phải tiêu đề; bản đồ lớn chiếm hết chiều cao màn hình, robot theo màu trạng thái; cột phải: cần xử lý ở trên, danh sách robot ở dưới; lọc theo dịch vụ |
| Nhiệm vụ | `/tasks` | 4 KPI có dòng phụ; nhiệm vụ của các dịch vụ được xem; lọc dịch vụ, trạng thái (chip có số 0 được làm mờ); tìm; "chỉ nhiệm vụ tôi tạo" |
| Đội robot | `/fleet`, `/fleet/:tên` | Bảng robot (loại, trạng thái, pin, nhiệm vụ, vị trí, thời gian chạy, kết nối); trang chi tiết robot (tóm tắt, chi tiết dịch vụ, vị trí, thông số kỹ thuật với mục **Chi tiết nâng cao**, pin, mức sử dụng, nhiệm vụ gần đây, sự kiện). Admin có thêm tab **Đội robot & đăng ký** (robot mới phát hiện, các fleet) |
| Lịch | `/schedule` | Ngày / tuần / tháng: nhiệm vụ, phiên sạc, khung bảo trì; lọc dịch vụ, robot, khu vực |
| Bảo trì | `/maintenance` | Sức khỏe, trạng thái bảo trì, lần tới, giờ hoạt động, vấn đề; admin lên lịch, bắt đầu, hoàn tất |
| Phân tích | `/analytics` | Lọc khoảng thời gian, dịch vụ, robot, khu vực; số liệu chung và theo dịch vụ |
| Thông báo | `/notifications` | **Cảnh báo** của robot/dịch vụ được theo dõi; **Nhiệm vụ của tôi** |
| Người dùng & quyền | `/admin/users` | Tài khoản, **Sửa quyền** (vai trò, dịch vụ, khu vực, quyền nhiệm vụ và đội robot), **Thêm operator** |
| Địa điểm | `/admin/locations` | Khu vực (mở/đóng cho nhiệm vụ), địa điểm, vùng vệ sinh, tuyến tuần tra; bản đồ và lane; hạ tầng |
| Tích hợp | `/admin/integrations` | Open-RMF, ROS 2 gateway, VDA5050, MQTT, API nhà cung cấp: trạng thái, cập nhật lúc, đội robot, cấu hình, nhật ký |
| Cài đặt | `/admin/settings` | Bật/tắt dịch vụ, đội robot và năng lực, cấu hình hiệu lực, nhật ký thay đổi |

Operator mở `/admin/*` sẽ thấy trang 403. Các route cũ (`/deliveries`, `/tracking/:id`, `/admin/robots/:tên`, ...) chuyển sang chỗ mới.

### Tạo nhiệm vụ

**+ Tạo nhiệm vụ** (trên thanh trên cùng từ 640 px; trên điện thoại ở đầu trang Nhiệm vụ, Lịch, Vận hành trực tiếp; và nút **+** của thẻ dịch vụ) mở **Tạo nhiệm vụ robot**: chọn dịch vụ (chỉ hiện dịch vụ được cấp), rồi điền form của dịch vụ đó (giao vận: điểm nhận, điểm giao, loại hàng, ưu tiên; vệ sinh: vùng, chế độ, thời lượng; tuần tra: khu vực, tuyến, số vòng), chọn **Càng sớm càng tốt** hoặc giờ bắt đầu và lặp lại. Có quyền `fleet.assign` thì chọn được robot; nếu không, hệ thống điều phối tự chọn. Backend kiểm tra lại mọi điều kiện; bị từ chối sẽ hiện lý do, ví dụ "Bạn không có quyền tạo nhiệm vụ vệ sinh."

Bấm robot trên bản đồ, trong bảng hay trong cảnh báo sẽ chọn robot (viền nổi bật, ghim điểm nhận và điểm đến) và mở **ngăn chi tiết robot**, có nút **Xem robot** để sang trang chi tiết. Bấm nhiệm vụ mở **ngăn chi tiết nhiệm vụ** (Tổng quan, Thông số nhiệm vụ, Diễn biến) với **Hủy**, **Tạm dừng / Tiếp tục**, **Gán lại** tùy quyền. Dòng đang mở được tô nhẹ trong bảng, danh sách lịch và danh sách robot.

Trên máy tính, ngăn chi tiết là một panel nổi ở góc trên phải, cao theo nội dung (dài thì cuộn bên trong), nút thao tác nằm ngay sau nội dung. Trên điện thoại, ngăn chi tiết trượt lên từ đáy màn hình.

### Giao diện sáng/tối

Nút mặt trời/mặt trăng trên thanh trên cùng (và ở trang đăng nhập) chọn **Sáng**, **Tối** hoặc **Theo hệ thống**; **Tài khoản → Giao diện** có cùng lựa chọn. **Theo hệ thống** dùng cài đặt sáng/tối của máy tính hoặc điện thoại và đổi theo khi máy đổi. Lựa chọn lưu trên thiết bị đang dùng (không theo tài khoản).

| Giao diện | Hình thức |
|---|---|
| Sáng | Nền trang xám rất nhạt, thẻ trắng viền mảnh, sidebar navy |
| Tối | Nền navy rất đậm, thẻ sáng hơn một bậc, mục lồng trong thẻ sáng hơn nữa; chữ sáng |

Bản đồ trực tiếp là blueprint xanh xám ở cả hai giao diện. Trang đăng nhập dùng ảnh khuôn viên EIU: giao diện sáng phủ trắng nhạt, chữ navy; giao diện tối phủ navy, chữ trắng.

### Các trang khác

| Trang | Dùng để |
|---|---|
| Tài khoản (`/account`) | Ngôn ngữ, giao diện, loại thông báo muốn nhận, đổi mật khẩu, đặt lại dữ liệu demo |
| Menu tài khoản (ảnh đại diện góc phải) | Tên, email, vai trò; dòng "EIU Robot Services / Eastern International University"; ngôn ngữ; Tài khoản; Đăng xuất |
| Ô tìm kiếm | Tìm robot, nhiệm vụ, địa điểm, khu vực (admin: cả operator), không cần dấu (`phong 204` ra `Phòng 204`) |
| Ngôn ngữ | Nút **VI / EN** trên thanh trên, trong menu tài khoản hoặc trang Tài khoản; lưu theo từng người dùng |

### Kích thước màn hình

| Chiều rộng | Giao diện |
|:---:|---|
| < 768 px (điện thoại) | Thanh tab dưới đáy (Tổng quan, Trực tiếp, Nhiệm vụ, Đội robot, Menu); các khối xếp chồng; bảng cuộn ngang trong thẻ; ngăn chi tiết trượt lên từ đáy |
| 768–1023 px (tablet) | Vẫn thanh tab dưới đáy; ngăn chi tiết là panel nổi bên phải |
| 1024–1279 px | Sidebar bên trái; các trang nhiều cột vẫn xếp chồng |
| ≥ 1280 px (desktop) | Sidebar; bố cục nhiều cột đầy đủ (Tổng quan 70/30, Vận hành trực tiếp bản đồ + cột phải) |

---

## 4. Xử lý sự cố

| Hiện tượng | Cách xử lý |
|---|---|
| `Port 5173 is already in use` | Đang có dev server khác (thường là container `eiu_web_dev`): `docker rm -f eiu_web_dev` hoặc dừng `npm run dev` cũ |
| `node: command not found` sau khi cài nvm | Mở terminal mới hoặc `source ~/.bashrc` |
| Trang trắng / không có dữ liệu | Mở bằng `127.0.0.1` hoặc `localhost`, không dùng IP; tải lại trang (Ctrl+Shift+R) |
| Dữ liệu demo lạ sau khi sửa code mock | **Tài khoản → Dữ liệu demo → Đặt lại dữ liệu demo** |
| Một trang báo "Unexpected Application Error" ngay sau khi sửa code | Dev server đọc file đúng lúc đang ghi: Ctrl+C rồi chạy lại `npm run dev`, tải lại trang |
| Chrome báo "Change your password… found in a data breach" khi đăng nhập | Mật khẩu demo (`admin1234`, `demo1234`) có trong danh sách mật khẩu phổ biến; bỏ qua với tài khoản demo, hoặc tắt cảnh báo trong `chrome://password-manager/settings` |
| Lỗi thiếu module sau khi đổi máy hoặc pull code mới | `rm -rf node_modules && npm ci` |

---

## 5. Cập nhật bản đồ

Khi `eiu_fleet_ui/maps/map.yaml` hoặc `nav_graph.yaml` thay đổi, tạo lại dữ liệu bản đồ cho demo:

```bash
cd ~/ros2_ws/src/eiu_fleet_ui/web_dashboard/frontend
npm run export-site     # hoặc: python3 scripts/export_site.py [--map-yaml PATH] [--nav-graph PATH] [--level NAME]
```

Tên địa điểm (Phòng 204, Thư viện…) nằm trong danh mục ở `src/mocks/seed.ts`, mỗi dòng gắn với một waypoint của nav graph. Nếu waypoint không còn trong nav graph, demo báo lỗi khi tạo dữ liệu.

---

## 6. Chạy với Open-RMF và robot thật

Chế độ thật gồm 3 thành phần ngoài trình duyệt:

| Thành phần | Có ROS 2? | Việc |
|---|:---:|---|
| `eiu_rmf_gateway` (ROS package) | Có, node duy nhất | Gửi task lên `/task_api_requests`; đọc `/fleet_states`, `/dispatch_states`, `/task_api_responses`, `/dispenser_states`, `/ingestor_states`; nhận task events của adapter |
| Redis | Không | Kênh trung gian: lệnh, sự kiện, trạng thái fleet, heartbeat ([contract](../../../eiu_rmf_gateway/docs/contract.md)) |
| Backend `web_dashboard/backend` | **Không** | API cho trình duyệt, tài khoản, đơn, thông báo, theo dõi trạng thái |

Không sửa gì ở full_control. Chi tiết: [../backend/README.md](../backend/README.md), [eiu_rmf_gateway](../../../eiu_rmf_gateway/README.md), danh sách interface: [../docs/interfaces.md](../docs/interfaces.md).

### 6.1 Build lại image `rmf_jazzy_vda` (bỏ image cũ)

Image (`vda5050_fleet_adapter_full_control/docker/Dockerfile`) đã có Redis, thư viện của gateway và của backend.

```bash
docker rm -f rmf_jazzy_vda_dev 2>/dev/null
docker rmi rmf_jazzy_vda
cd ~/ros2_ws/src/vda5050_fleet_adapter_full_control/docker
./build.sh --no-cache                              # build lại toàn bộ, mất vài phút
./run.sh                                          # mở container rmf_jazzy_vda_dev
```

### 6.2 Chạy trong container `rmf_jazzy_vda_dev` (khuyên dùng)

Open-RMF, fleet adapter và dispenser/ingestor (với task delivery) phải đang chạy, ví dụ `ros2 launch fleet_bringup fleet_bringup.launch.py`. Ở các shell khác của container (`docker exec -it rmf_jazzy_vda_dev bash`):

```bash
# 1. Redis (mỗi lần mở container)
redis-server --bind 127.0.0.1 --port 6379 --daemonize yes

# 2. Gateway (build một lần)
cd /ros2_ws && colcon build --packages-select eiu_rmf_gateway && source install/setup.bash
ros2 launch eiu_rmf_gateway gateway.launch.py

# 3. Backend
cd /ros2_ws/src/eiu_fleet_ui/web_dashboard/backend
python3 -m eiu_web_backend create-user --email admin@eiu.edu.vn --name "Tên Admin" --role admin   # lần đầu
python3 -m eiu_web_backend create-user --email operator@eiu.edu.vn --name "Operator" --services delivery,cleaning --zones building_a
python3 -m eiu_web_backend serve        # http://127.0.0.1:8000, Ctrl+C để dừng
```

Để sửa nav graph từ web, chạy gateway với file nav graph mà fleet adapter đang dùng (tham số launch `nav_graph` của adapter):

```bash
ros2 launch eiu_rmf_gateway gateway.launch.py nav_graph_path:=/ros2_ws/src/vda5050_fleet_adapter_full_control/maps/nav_graph.yaml
```

Gateway lưu bản cũ thành `<file>.bak` trước khi ghi. Adapter nạp nav graph lúc khởi động, nên sau khi lưu cần khởi động lại adapter.

Kiểm tra:

```bash
redis-cli get eiu:rmf:gateway               # heartbeat của gateway (có ros_domain_id)
redis-cli hkeys eiu:rmf:fleets              # các fleet gateway đã thấy
redis-cli hkeys eiu:rmf:controls            # robot gateway điều khiển được
curl http://127.0.0.1:8000/api/v1/status    # {"rmf":"online"}
```

| `status` | Ý nghĩa |
|---|---|
| `online` | Gateway sống và có fleet gửi `/fleet_states` |
| `offline` | Gateway sống nhưng không có fleet nào gửi dữ liệu (RMF/adapter chưa chạy, sai `ROS_DOMAIN_ID`) |
| `unavailable` | Không có heartbeat: gateway chưa chạy, hoặc không nối được Redis |

Gateway dùng `ROS_DOMAIN_ID` của container (`run.sh` đặt `DOMAIN`), phải trùng với Open-RMF. Backend không cần biết domain.

### 6.3 Backend chạy thẳng trên máy (tùy chọn)

Dùng môi trường ảo để thư viện của backend không lẫn với gói Python khác trên máy (ví dụ gói `argon2` cũ che mất `argon2-cffi`):

```bash
cd ~/ros2_ws/src/eiu_fleet_ui/web_dashboard/backend
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python -m eiu_web_backend create-user --email admin@eiu.edu.vn --name "Admin" --role admin   # lần đầu
.venv/bin/python -m eiu_web_backend serve
```

Backend vẫn cần Redis ở `127.0.0.1:6379` và gateway trong container ROS.

### 6.4 Backend ở container riêng (tùy chọn)

Backend không có ROS nên chạy được ở bất kỳ đâu nối tới được Redis của gateway, ví dụ một VM của trường. Image dựa trên `python:3.12-slim`:

```bash
cd ~/ros2_ws/src/eiu_fleet_ui/web_dashboard/backend
docker build -t eiu_web_backend .                                   # thêm --build-arg WITH_TESTS=true để chạy test
docker run --rm -it -v ~/ros2_ws/src/eiu_fleet_ui:/eiu_fleet_ui eiu_web_backend \
    create-user --email admin@eiu.edu.vn --name "Tên Admin" --role admin
docker run -d --name eiu_web_backend --network host -e EIU_WEB_REDIS_URL=redis://127.0.0.1:6379/0 \
    -v ~/ros2_ws/src/eiu_fleet_ui:/eiu_fleet_ui eiu_web_backend
```

Khi Redis ở máy khác: đặt mật khẩu cho Redis và dùng `redis://:<mật khẩu>@<máy>:6379/0`, chỉ trong mạng nội bộ.

### 6.5 Chạy frontend với backend thật

```bash
cd ~/ros2_ws/src/eiu_fleet_ui/web_dashboard/frontend
npm run dev:backend        # http://127.0.0.1:5173, /api và /ws chuyển sang backend :8000
```

Để máy khác cùng mạng truy cập: chạy `npm run dev:lan` thay cho `npm run dev:backend`, rồi mở `http://<IP máy chạy>:5173` (hoặc `http://<hostname>.local:5173` khi mạng hỗ trợ mDNS). Backend vẫn chỉ nghe trên `127.0.0.1:8000`; Vite chuyển `/api` và `/ws` sang backend. Khi tường lửa bật: `sudo ufw allow 5173/tcp`. Chế độ demo (`npm run dev`) không dùng được từ máy khác vì service worker chỉ chạy trên `localhost` hoặc HTTPS.

Khi Open-RMF không gửi dữ liệu hoặc gateway không chạy, dưới thanh trên cùng hiện dải cảnh báo vàng, và chip trạng thái trên thanh trên cùng đổi màu (bình thường là "Nền tảng hoạt động tốt" màu xám).

### 6.6 Sự kiện task chi tiết (tùy chọn)

Thêm vào config của fleet adapter:

```yaml
vda5050:
  ui_websocket_uri: "ws://127.0.0.1:8100"
```

Gateway nhận và chuyển qua Redis cho backend: phase đang chạy, số vòng patrol đã xong, giờ RMF dự kiến hoàn thành. Mỗi adapter chỉ gửi tới **một** địa chỉ: nếu dashboard QML đang dùng `ui_websocket_uri` thì phải chọn một bên.

### 6.7 Nhiệm vụ trên RMF

| Loại | Gửi lên RMF | Ghi chú |
|---|---|---|
| Giao hàng | Task `delivery`: lấy hàng ở dispenser tại điểm lấy, trả ở ingestor tại điểm đến | Handler mặc định `mock_dispenser_1` / `mock_ingestor_1` (cấu hình `delivery.*`, hoặc khai báo riêng cho từng địa điểm trong `site.yaml`) |
| Tuần tra | Task `patrol`: các điểm dừng theo thứ tự, lặp `rounds` vòng | Nhiều vòng cần ít nhất 2 điểm; tối đa 8 điểm, 10 vòng |

Trạng thái hiển thị: chờ robot → đang đến lấy hàng → chờ bỏ hàng (dispenser bận) → đang giao → đã đến nơi (ingestor bận) → hoàn thành. Tuần tra: chờ robot → đang thực hiện (vòng x/y) → hoàn thành. Khi `/fleet_states` không có đường đi, backend tự tính đường ngắn nhất trên nav graph để vẽ lộ trình và quãng đường còn lại.

### 6.8 Đã kiểm thử

Trên sandbox riêng (broker cổng 18841, ROS domain 78, Redis cổng 6399, 3 robot giả `mock_mqtt_robot.py`, RMF core, `mutex_group_supervisor`, mock dispenser/ingestor, full_control với `ui_websocket_uri` trỏ về gateway), backend chạy trên `python:3.12-slim` không có ROS:

- Đơn giao hàng đi đủ các bước tới hoàn thành; tuần tra 2 vòng xong 2/2; hủy tuần tra đang chạy → `cancelled`.
- Tắt gateway (`kill -9` hoặc `SIGTERM`) → backend báo `unavailable`; bật lại → `online`.
- Tuần tra tạo từ form web hiện robot di chuyển trên bản đồ.
- Vận hành (thêm robot giả thứ 4, serial 0004, không có trong config; `metrics_period_s: 5`; gateway với `nav_graph_path` trỏ tới bản sao nav graph): tạm dừng/tiếp tục, giới hạn tốc độ (đọc lại 0,1 m/s), đặt lại vị trí theo waypoint; robot 0004 hiện ở danh sách chờ đăng ký, adapter từ chối khi trạm sạc đã có chủ; thêm trạm sạc mới bằng trình sửa nav graph → khởi động lại adapter → đăng ký 0004 thành công, robot xuất hiện sau 2 s, gỡ rồi đăng ký lại được; đóng 2 hành lang → adapter báo `closed_lanes [0, 1, 2, 3]`, mở lại; lưu nav graph với SHA cũ bị từ chối (`stale`); trang Hệ thống hiện chỉ số adapter. Mỗi thao tác có trong audit log.

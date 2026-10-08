# Web Dashboard — Kiến trúc tích hợp với Open-RMF và full_control

Tài liệu này mô tả cách web dashboard giao tiếp với Open-RMF Jazzy và `vda5050_fleet_adapter_full_control` ở trạng thái hiện tại (2026-10-02), gồm cả các lệnh vận hành của nền tảng (điều khiển robot cần quyền `fleet.control`, đăng ký robot `robots.manage`, lane và nav graph `locations.manage`): backend **không có ROS**, mọi giao tiếp với ROS 2 đi qua `eiu_rmf_gateway` và Redis. Ảnh SVG nằm trong `img/`; mã mermaid đặt ngay dưới ảnh.

Liên quan: [de_xuat_kien_truc.md](de_xuat_kien_truc.md) (kiến trúc đích), [huong_dan_cai_dat_va_su_dung.md](huong_dan_cai_dat_va_su_dung.md) (cài đặt, chạy), [../backend/README.md](../backend/README.md), [eiu_rmf_gateway](../../../eiu_rmf_gateway/README.md) và [contract](../../../eiu_rmf_gateway/docs/contract.md), danh sách đầy đủ: [../docs/interfaces.md](../docs/interfaces.md).

---

## 1. Tổng quan thành phần

![Tổng quan kiến trúc tích hợp](img/1_tong_quan.svg)

<details><summary>Mã mermaid</summary>

```mermaid
flowchart TB
    subgraph L1 ["① Trình duyệt — React SPA"]
        direction LR
        UI["Trang · Form giao hàng / tuần tra<br/>Bản đồ Leaflet (mét, theo tầng)"]
        Q["TanStack Query<br/>dữ liệu REST"]
        LS["Live store<br/>robot theo tên"]
    end

    PX["② Vite proxy :5173 (sau này nginx :443)<br/>/api → REST · /ws → WebSocket"]

    subgraph L3 ["③ eiu_web_backend — Python thuần, KHÔNG ROS (chạy ở đâu cũng được)"]
        direction LR
        API["FastAPI<br/>REST /api/v1<br/>auth · CSRF · RBAC"]
        HUB["Hub /ws<br/>patch robot theo key<br/>event · trạng thái RMF"]
        SV["Service + tracker<br/>tạo/hủy task<br/>gộp trạng thái mỗi 0.5 s"]
        OP["Operations<br/>tổng quan · nhiệm vụ · điều khiển robot<br/>đăng ký · lane · nav graph · chỉ số adapter"]
        AL["Alerts<br/>điều kiện → cảnh báo<br/>tự đóng · xác nhận"]
        DB[("SQLite")]
        CL["GatewayClient<br/>rmf/client.py"]
        API --> SV
        API --> OP
        HUB --> SV
        SV --> DB
        OP --> DB
        AL --> DB
        HUB --> AL
        SV <--> CL
        OP <--> CL
    end

    RD[("④ Redis — contract v1<br/>commands (stream) ↓ · events (stream) ↑<br/>fleets · workcells · adapters · controls · metrics<br/>lanes · registry · discovery (hash) ↑ · nav_graph · heartbeat ↑")]

    subgraph L5 ["⑤ eiu_rmf_gateway — node ROS 2 duy nhất của web · 1 instance"]
        direction LR
        CORE["core.py<br/>đọc lệnh · kiểm tra · flush 0.1 s"]
        ROS["ros_io.py<br/>topic · service · parameter<br/>tìm adapter mỗi 2 s"]
        TE["task_events.py<br/>WebSocket :8100"]
        NGM["nav_graph.py<br/>đọc · kiểm tra · lưu"]
        ROS --> CORE
        TE --> CORE
        NGM --> CORE
    end

    NGF[("File nav graph<br/>nav_graph_path")]

    DDS{{"⑥ ROS 2 DDS — cùng ROS_DOMAIN_ID<br/>/task_api_requests ↓ · /task_api_responses ↑ · /dispatch_states ↑<br/>/fleet_states ↑ · /dispenser_states ↑ · /ingestor_states ↑<br/>/lane_closure_requests ↓ · /lane_states ↑ · /robot_registration_requests ↓ · /robot_registration_results ↑<br/>/robot_registry ↑ · /robot_discovery ↑ · ~/metrics ↑<br/>service pause · resume ↓ · parameter speed_limit.* ↓ · init_position ↓"}}

    subgraph L7 ["⑦ Open-RMF Jazzy"]
        direction LR
        DSP["rmf_task_dispatcher<br/>đấu thầu · giao task"]
        SCH["rmf_traffic_schedule<br/>+ mutex_group_supervisor"]
        WC["dispenser / ingestor"]
    end

    FA["⑧ vda5050_fleet_adapter_full_control<br/>VDA5050 master · /fleet_states<br/>interface vận hành: pause/resume, speed limit,<br/>init position, đăng ký robot, lane, metrics"]
    MQ[("⑨ MQTT broker — VDA5050 2.1")]

    subgraph L10 ["⑩ Robot"]
        direction LR
        CA["vda5050_client_adapter"] --> TB["tb3_vda5050_bridge"] --> N2["Nav2 + AMCL"]
    end

    L1 <-->|"HTTPS · WebSocket"| PX
    PX <--> API
    PX <--> HUB
    CL <-->|"XADD lệnh · XREAD sự kiện · HGETALL trạng thái"| RD
    RD <-->|"XREADGROUP lệnh · XADD/HSET/SET"| CORE
    ROS <--> DDS
    DDS <--> DSP
    DDS <--> WC
    DDS <--> FA
    DSP <-->|"task được giao"| FA
    FA <--> SCH
    FA <-->|"yêu cầu workcell"| WC
    FA -.->|"task_state_update (ui_websocket_uri, tùy chọn)"| TE
    NGM <-->|"đọc · ghi"| NGF
    NGF -.->|"nạp khi khởi động"| FA
    FA <--> MQ
    MQ <--> CA
```

</details>

| Tầng | Thành phần | Có ROS 2? | Giao tiếp với tầng dưới |
|:---:|---|:---:|---|
| ① | React SPA: trang, form, bản đồ Leaflet, TanStack Query, live store | Không | REST `/api/v1`, WebSocket `/ws` |
| ② | Vite proxy khi phát triển; nginx khi triển khai | Không | Chuyển `/api`, `/ws` tới backend |
| ③ | `eiu_web_backend`: FastAPI, Hub `/ws`, service + tracker, operations, SQLite, `GatewayClient` | **Không** | Redis |
| ④ | Redis: stream lệnh, stream sự kiện, hash trạng thái (fleet, workcell, adapter, điều khiển, chỉ số, lane, registry, discovery), nav graph, heartbeat | Không | |
| ⑤ | `eiu_rmf_gateway`: node ROS 2 duy nhất của web, 1 instance | Có | Topic, service, parameter ROS 2; WebSocket task events :8100; file nav graph |
| ⑥ | ROS 2 DDS, cùng `ROS_DOMAIN_ID` với Open-RMF | | |
| ⑦–⑧ | Open-RMF (dispatcher, schedule, mutex, workcell) và `vda5050_fleet_adapter_full_control` | Có | MQTT VDA5050 2.1 |
| ⑨–⑩ | MQTT broker, robot (client adapter, bridge, Nav2) | | |

Backend gửi hai nhóm lệnh: **task request của Open-RMF** (`dispatch_task_request`, `cancel_task_request`) và **lệnh vận hành** (`robot_pause`, `robot_resume`, `robot_speed_limit`, `robot_init_position`, `registration_request`, `lane_request`, `nav_graph_save`). Gateway chỉ nhận các loại lệnh trong contract, kiểm tra nội dung từng lệnh và từ chối phần còn lại. Fleet adapter quyết định mọi yêu cầu vận hành; backend và gateway chỉ chuyển yêu cầu và trả lại kết luận của adapter. Dashboard QML `eiu_fleet_ui` vẫn chạy song song, dùng cùng các interface vận hành của adapter.

---

## 2. Gửi một nhiệm vụ tới robot

![Luồng gửi nhiệm vụ](img/2_gui_nhiem_vu.svg)

<details><summary>Mã mermaid</summary>

```mermaid
sequenceDiagram
    autonumber
    actor U as Người dùng
    participant W as Web SPA
    participant A as Backend (không ROS)
    participant DB as SQLite
    participant RD as Redis
    participant G as eiu_rmf_gateway
    participant D as rmf_task_dispatcher
    participant F as full_control adapter
    participant R as Robot

    U->>W: Chọn dịch vụ (giao vận, vệ sinh, tuần tra), điền form, bấm Tạo
    W->>A: POST /api/v1/tasks (cookie + X-CSRF-Token)
    A->>A: Kiểm tra quyền dịch vụ và khu vực, form, lịch, giới hạn nhiệm vụ mở (max_open)
    A->>DB: Lưu nhiệm vụ (queued, request_id) + sự kiện + thông báo
    A->>RD: XADD commands {id, type task_request, body = task request RMF}
    A-->>W: 201 TaskDetail (queued)
    G->>RD: XREADGROUP commands
    G->>G: Kiểm tra version, loại lệnh, tuổi lệnh
    G->>D: /task_api_requests (request_id = id lệnh)
    G->>RD: XACK + event command_result ok
    D-->>G: /task_api_responses {success, booking.id}
    G->>RD: XADD events task_api_response
    D->>F: Đấu thầu, giao task cho robot
    D-->>G: /dispatch_states (robot được chọn)
    G->>RD: XADD events dispatch_states
    F->>R: order VDA5050 qua MQTT
    F-->>G: /fleet_states (task_id trên robot) + task_state_update (WebSocket)
    G->>RD: HSET fleets · XADD events task_state
    Note over A,RD: Tracker tick 0.5 s: XREAD events, HGETALL fleets, gộp trạng thái
    A->>DB: rmf_task_id, robot, trạng thái, sự kiện, thông báo
    A-->>W: /ws event {topics} → GET lại nhiệm vụ
```

</details>

Điểm chính:

- Backend lưu nhiệm vụ **trước**, rồi đặt lệnh vào stream `eiu:rmf:commands` với `id` = `request_id` của nhiệm vụ. Gateway publish với đúng id đó, nên câu trả lời của RMF khớp lại được với nhiệm vụ.
- Giao diện tạo nhiệm vụ qua `POST /api/v1/tasks`; các endpoint `/api/v1/deliveries` của bản phát hành đầu vẫn còn, cùng luồng phía sau.
- Gateway chỉ chuyển tiếp: không đổi nội dung task request, chỉ kiểm tra version, loại lệnh, tuổi lệnh (`command_max_age_s`) rồi `XACK`.
- Không có trả lời trong `rmf.dispatch_timeout_s` (15 s) → `failed` (`dispatch.no_response`); không ghi được vào Redis → `failed` ngay (`gateway.unreachable`); gateway từ chối → `failed` (`gateway.<lỗi>`).

---

## 3. Dữ liệu thời gian thực về giao diện

![Luồng dữ liệu thời gian thực](img/3_du_lieu_thoi_gian_thuc.svg)

<details><summary>Mã mermaid</summary>

```mermaid
flowchart LR
    subgraph ROS ["ROS 2 (trong gateway)"]
        FS["/fleet_states<br/>x, y, yaw, level, pin, task_id, path"]
        TR["/task_api_responses<br/>/dispatch_states"]
        WS["/dispenser_states<br/>/ingestor_states"]
        EV["task_state_update<br/>WebSocket :8100"]
    end

    subgraph GW ["eiu_rmf_gateway — Recorder chỉ ghi, flush mỗi 0.1 s"]
        RF["fleets đã đổi"]
        RE["sự kiện theo thứ tự"]
        RW["workcell đã đổi"]
        RO["trạng thái vận hành đã đổi<br/>lane · registry · discovery<br/>controls · metrics · nav graph"]
        HB["heartbeat (hết hạn 3 s)"]
    end

    subgraph RD ["Redis"]
        KF[("eiu:rmf:fleets")]
        KE[("eiu:rmf:events")]
        KW[("eiu:rmf:workcells")]
        KO[("eiu:rmf:adapters · controls · metrics<br/>lanes · registry · discovery · nav_graph")]
        KG[("eiu:rmf:gateway")]
    end

    subgraph BE ["Backend — tracker tick 0.5 s"]
        SNAP["snapshot()<br/>fleet cũ hơn 5 s = stale"]
        DRAIN["drain()<br/>đọc tiếp từ cursor"]
        MERGE["Gộp theo hạng nguồn<br/>local < fleet < dispatch < api = events"]
        LIVE["robots_live()<br/>levelId · deliveryId · còn lại (m)<br/>path RMF hoặc đường nav graph"]
        STAT["rmf: online / offline / unavailable"]
        OPS["Operations.tick()<br/>chỉ số adapter · nav graph mới<br/>so chữ ký trạng thái vận hành"]
        ALT["Alerts.tick()<br/>RMF · fleet · robot · pin · adapter · task<br/>mở sau 5 s · tự đóng"]
    end

    subgraph HUB ["Hub /ws — từng kết nối"]
        DIFF["Lọc theo quyền (fleet:read thấy mọi robot)<br/>chỉ gửi robot thay đổi"]
        EVT["event {topics} cho chủ đơn"]
        EVF["event {fleet} cho kết nối có fleet:read"]
        EVA["event {alerts} cho kết nối có fleet:read"]
        SYS["system {rmf}"]
    end

    BR["Trình duyệt<br/>marker trượt mượt · timeline · dải cảnh báo"]

    FS --> RF --> KF --> SNAP
    TR --> RE --> KE --> DRAIN
    EV --> RE
    WS --> RW --> KW --> SNAP
    HB --> KG --> STAT
    SNAP --> MERGE
    DRAIN --> MERGE
    SNAP --> LIVE --> DIFF --> BR
    MERGE --> EVT --> BR
    STAT --> SYS --> BR
    RO --> KO --> SNAP
    SNAP --> OPS --> EVF --> BR
    OPS --> ALT
    SNAP --> ALT --> EVA --> BR
```

</details>

| Nguồn | Hạng | Cho biết |
|---|:---:|---|
| Backend tự kết luận (hết giờ chờ, không tới được gateway) | 0 | Đơn không được RMF nhận |
| `/fleet_states` | 1 | Robot đang mang `task_id` (underway) hoặc bỏ nó (đoán completed); vị trí, pin, path |
| `/dispatch_states` | 2 | queued, failed, cancelled; robot được giao |
| `/task_api_responses` | 3 | `task_id` của yêu cầu; RMF từ chối; trả lời hủy |
| Task events (`ui_websocket_uri` → gateway :8100) | 3 | Trạng thái, robot, phase, số vòng tuần tra, giờ dự kiến xong |
| `command_result` của gateway | 3 | Lệnh bị từ chối hoặc không publish được |
| `/dispenser_states`, `/ingestor_states` | — | Robot đang chờ bỏ hàng hoặc chờ người nhận |

Cảnh báo vận hành được backend tính mỗi tick từ trạng thái RMF, robot, chỉ số adapter và nhiệm vụ; lưu trong bảng `alerts`; đổi thì gửi `event {topics: ["alerts"]}`. Một fleet mất kết nối toàn bộ chỉ sinh một cảnh báo `fleet.offline`. RMF vẫn báo robot có AGV đã im lặng, nên backend dùng báo cáo metrics của adapter (số robot đang nghe được, robot im lặng lâu nhất) để sinh `adapter.robots_offline` và hiện robot đó là "Mất kết nối".

Trạng thái vận hành (lane, robot đã/chờ đăng ký, điều khiển, chỉ số adapter, nav graph) đi cùng đường: khi nó đổi, backend gửi `event {topics: ["fleet"]}` và trang Vận hành tải lại dữ liệu qua REST.

Gateway ghi Redis mỗi `flush_period_s` (0,1 s), chỉ phần đã đổi; backend đọc mỗi `realtime.period_s` (0,5 s). Sự kiện đọc tiếp từ cursor lưu trong Redis, nên backend khởi động lại không mất sự kiện. Heartbeat của gateway hết hạn sau 3 s: không có heartbeat → `unavailable`; có heartbeat nhưng không fleet nào gửi dữ liệu → `offline`.

---

## 4. Hủy nhiệm vụ

![Luồng hủy nhiệm vụ](img/4_huy_nhiem_vu.svg)

<details><summary>Mã mermaid</summary>

```mermaid
sequenceDiagram
    autonumber
    actor U as Người dùng
    participant W as Web SPA
    participant A as Backend
    participant RD as Redis
    participant G as eiu_rmf_gateway
    participant D as rmf_task_dispatcher
    participant F as full_control adapter
    participant R as Robot

    U->>W: Hủy yêu cầu (chỉ bật khi actions.cancel = true)
    W->>A: POST /api/v1/tasks/{id}/cancel
    alt Đã qua bước được phép hủy hoặc chưa có rmf_task_id
        A-->>W: 409 delivery.not_cancellable / not_dispatched_yet
    else Gateway không chạy hoặc Redis không tới được
        A-->>W: 503 rmf.unavailable
    else Được hủy
        A->>RD: XADD commands cancel_task_request
        A-->>W: TaskDetail (nút hủy tắt trong lúc chờ)
        G->>D: /task_api_requests cancel_task_request
        D->>F: Hủy task
        F->>R: instantActions cancelOrder (qua MQTT)
        D-->>G: /task_api_responses {success}
        G->>RD: XADD events task_api_response
        A->>A: Tracker: cancelled + sự kiện + thông báo
        A-->>W: /ws event → tải lại nhiệm vụ
    end
    Note over A: Không có trả lời sau rmf.cancel_timeout_s → lỗi cancel.no_response
```

</details>

| Loại | Được hủy khi |
|---|---|
| Giao hàng | Đã hẹn giờ, chờ robot, đang đến lấy hàng, chờ bỏ hàng |
| Tuần tra | Đã hẹn giờ, chờ robot, đang thực hiện |
| Vệ sinh | Đã hẹn giờ, chờ robot, đang thực hiện |

Server quyết định (`actions.cancel` trong DTO); giao diện chỉ bật/tắt nút theo kết quả đó.

---

## 5. Lệnh vận hành (quyền `fleet.control`, `robots.manage`, `locations.manage`)

![Luồng lệnh vận hành](img/5_lenh_van_hanh.svg)

<details><summary>Mã mermaid</summary>

```mermaid
sequenceDiagram
    autonumber
    actor U as Người vận hành
    participant FE as Trình duyệt<br/>trang Vận hành
    participant BE as eiu_web_backend<br/>operations.py
    participant R as Redis
    participant GW as eiu_rmf_gateway
    participant FA as full_control adapter
    participant F as File nav graph

    Note over GW,FA: Mỗi 2 s gateway tìm adapter qua service /{node}/{robot}/pause<br/>và topic /{node}/metrics, ghi adapters · controls vào Redis

    U->>FE: Tạm dừng tb3_1
    FE->>BE: POST /fleet/robots/tb3_1/pause
    BE->>R: XADD commands {type: robot_pause, id}
    BE->>BE: chờ kết quả (command_timeout_s)
    R->>GW: XREADGROUP
    GW->>FA: service /{node}/tb3_1/pause (Trigger)
    FA-->>GW: success · message
    GW->>R: XADD events command_result {id, ok, message}
    R-->>BE: XREAD events
    BE-->>FE: {ok, message} · ghi audit log
    alt Adapter từ chối
        BE-->>FE: 409 command.refused + lý do của adapter
    else Không trả lời kịp
        BE-->>FE: 504 command.no_answer
    end

    U->>FE: Đăng ký robot ROBOTIS/0004 (Kiểm tra → Đăng ký)
    FE->>BE: POST /fleet/registration {action: check | add}
    BE->>R: XADD commands {type: registration_request, dry_run}
    R->>GW: XREADGROUP
    GW->>FA: /robot_registration_requests
    FA-->>GW: /robot_registration_results (kết luận: errors, warnings)
    GW->>R: XADD events registration_result
    FA-->>GW: /robot_registry · /robot_discovery (latched)
    GW->>R: HSET registry · discovery
    R-->>BE: kết luận
    BE-->>FE: {ok, errors, warnings, persisted}

    U->>FE: Đóng hành lang Patrol_A1 ↔ Patrol_B1
    FE->>BE: POST /fleet/lanes {close: [0, 1]}
    BE->>R: XADD commands lane_request (mỗi fleet)
    R->>GW: XREADGROUP
    GW->>FA: /lane_closure_requests (LaneRequest)
    FA-->>GW: /lane_states (closed_lanes)
    GW->>R: HSET lanes
    R-->>BE: snapshot → event "fleet" → trang vẽ lại lane đỏ

    U->>FE: Sửa nav graph rồi Lưu
    FE->>BE: PUT /fleet/nav-graph {levelId, baseSha256, vertices, lanes}
    BE->>BE: kiểm tra tên trùng, lane, waypoint của địa điểm
    BE->>R: XADD commands nav_graph_save {yaml, base_sha256}
    R->>GW: XREADGROUP
    GW->>F: so SHA-256 · chép .bak · ghi đè nguyên tử
    GW->>R: command_result {sha256 mới} · SET nav_graph
    R-->>BE: bản đồ và địa điểm cập nhật
    Note over FA,F: Adapter nạp nav graph khi khởi động:<br/>khởi động lại adapter để robot dùng đồ thị mới
```

</details>

| Tính năng | REST | Lệnh gateway | Interface ROS 2 của adapter | Kết quả |
|---|---|---|---|---|
| Tạm dừng, tiếp tục | `POST /fleet/robots/{robot}/pause`, `/resume` | `robot_pause`, `robot_resume` | Service `std_srvs/Trigger` `/<adapter>/<robot>/pause`, `/resume` | `command_result` |
| Giới hạn tốc độ | `POST /fleet/robots/{robot}/speed-limit` | `robot_speed_limit` | Parameter `speed_limit.<robot>` | `command_result`; giá trị mới trong hash `controls` |
| Đặt lại vị trí | `POST /fleet/robots/{robot}/init-position` | `robot_init_position` | Topic `/<adapter>/<robot>/init_position` → `init_position_result` | `command_result` |
| Đăng ký, gỡ robot | `POST /fleet/registration` | `registration_request` | `/robot_registration_requests` → `/robot_registration_results` | `registration_result` |
| Đóng, mở lane | `POST /fleet/lanes` | `lane_request` | `/lane_closure_requests` → `/lane_states` | Hash `lanes` |
| Sửa nav graph | `PUT /fleet/nav-graph` | `nav_graph_save` | Không: gateway ghi file `nav_graph_path` | `command_result` với SHA-256 mới |
| Chỉ số adapter | `GET /fleet/system` | Không | `/<adapter>/metrics` | Hash `metrics`, gộp bằng `eiu_fleet_ui.metrics_model` |
| Tổng quan | `GET /fleet/overview` | Không | Dữ liệu đã có trong Redis | Số robot/nhiệm vụ/cảnh báo, tình trạng hệ thống |
| Nhiệm vụ của mọi người | `GET /fleet/tasks`, `POST /fleet/tasks/{id}/cancel` | `task_request` (`cancel_task_request`) | `/task_api_requests` | Như đơn của người dùng |
| Cảnh báo | `GET /fleet/alerts`, `POST /fleet/alerts/{id}/ack`, `/ack-all`, `/{id}/resolve` | Không | Không (backend tự tính) | Bảng `alerts`, sự kiện `alerts` trên `/ws` |

Chỉ số lane là chỉ số của RMF: các lane trong nav graph, đánh số liên tục qua các tầng theo thứ tự trong file, mỗi chiều một chỉ số. Giao diện vẽ hai chiều của một hành lang thành một đường và đóng/mở cả hai.

---

## 6. Bảng interface

Danh sách đầy đủ (QoS, trường dữ liệu, JSON mẫu, mã lỗi, message WebSocket): [../docs/interfaces.md](../docs/interfaces.md).

| Interface | Giữa | Dùng để |
|---|---|---|
| REST `/api/v1/*`, WebSocket `/ws` | Trình duyệt ↔ backend | Tài khoản, đơn, thông báo, bản đồ; robot thời gian thực |
| Redis `eiu:rmf:commands` | Backend → gateway | Task request của Open-RMF; lệnh vận hành |
| Redis `eiu:rmf:events` | Gateway → backend | `task_api_response`, `dispatch_states`, `task_state`, `command_result`, `registration_result` |
| Redis `eiu:rmf:fleets`, `eiu:rmf:workcells` | Gateway → backend | Trạng thái fleet, robot, workcell mới nhất |
| Redis `eiu:rmf:adapters`, `controls`, `metrics`, `lanes`, `registry`, `discovery` | Gateway → backend | Adapter tìm thấy, điều khiển từng robot, chỉ số, lane đóng, robot đã đăng ký, robot chờ đăng ký |
| Redis `eiu:rmf:nav_graph` | Gateway → backend | File nav graph: đường dẫn, SHA-256, nội dung |
| Redis `eiu:rmf:gateway` | Gateway → backend | Heartbeat |
| `/task_api_requests` | Gateway → RMF | `dispatch_task_request`, `cancel_task_request` |
| `/task_api_responses`, `/dispatch_states` | RMF → gateway | Trả lời, đấu thầu, robot được giao |
| `/fleet_states` | full_control → gateway | Vị trí, tầng, pin, `task_id`, path |
| `/dispenser_states`, `/ingestor_states` | Workcell → gateway | Chờ bỏ hàng / chờ nhận hàng |
| Service `pause`, `resume`; parameter `speed_limit.<robot>`; topic `init_position` | Gateway → full_control | Điều khiển từng robot; gateway nhớ kết quả tạm dừng/tiếp tục gần nhất (`controls.paused`) |
| `/robot_registration_requests` / `_results`, `/robot_registry`, `/robot_discovery` | Gateway ↔ full_control | Đăng ký, gỡ robot; robot chờ đăng ký |
| `/lane_closure_requests`, `/lane_states` | Gateway ↔ full_control | Đóng, mở lane |
| `/<adapter>/metrics` | full_control → gateway | Chỉ số sức khỏe adapter |
| File nav graph (`nav_graph_path`) | Gateway ↔ đĩa | Đọc, lưu nav graph; adapter nạp khi khởi động |
| WebSocket `ws://127.0.0.1:8100` | full_control → gateway | `task_state_update` (tùy chọn, `ui_websocket_uri`) |

Web **không** dùng MQTT và không gửi lệnh trực tiếp cho robot: task đi qua dispatcher của RMF, lệnh vận hành đi qua interface vận hành của fleet adapter.

---

## 7. Hướng mở rộng

| Hướng | Cách làm trên kiến trúc này |
|---|---|
| Trang admin (cài đặt hệ thống, quản lý tài khoản) | Chỉ ở backend và frontend; không đổi contract gateway |
| Áp dụng nav graph mà không khởi động lại adapter | Cần interface nạp lại nav graph ở fleet adapter; gateway thêm một loại lệnh |
| Nhiều replica backend, PostgreSQL | Backend không có ROS nên nhân bản được; tracker chạy ở một instance hoặc dùng consumer group cho stream sự kiện |
| View 3D / digital twin (three.js) | Dùng chung luồng `/ws` (mét + `levelId`), metadata bản đồ và nav graph từ `/levels`; thêm mô hình tòa nhà, model robot, nút 2D/3D, lịch sử telemetry, nguồn `real`/`sim` |
| Đổi sang rmf-web api-server | Contract dùng JSON task của Open-RMF, nên có thể thay gateway bằng một adapter gọi api-server mà backend không đổi |

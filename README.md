# 🎮 Game Server C - Realtime XO (Tic-Tac-Toe) ⚡️
> **Realtime Multiplayer XO** viết bằng **C** — tập trung vào hiệu năng, xử lý socket, và đồng bộ trạng thái ván chơi.

<p align="center">
  <img alt="C" src="https://img.shields.io/badge/Language-C-blue.svg">
  <img alt="Realtime" src="https://img.shields.io/badge/Realtime-Sockets-green.svg">
  <img alt="Game" src="https://img.shields.io/badge/Game-TicTacToe(XO)-orange.svg">
  <img alt="Status" src="https://img.shields.io/badge/Status-Active-success.svg">
</p>

<p align="center">
  <a href="#-tổng-quan">Tổng quan</a> •
  <a href="#-tính-năng">Tính năng</a> •
  <a href="#-demo">Demo</a> •
  <a href="#-kiến-trúc">Kiến trúc</a> •
  <a href="#-giao-thức-clientserver">Protocol</a> •
  <a href="#-build--run">Build</a> •
  <a href="#-hướng-dẫn-test">Test</a> •
  <a href="#-faq">FAQ</a>
</p>

---

## ✨ Tổng quan
Dự án này là một **game server realtime** cho trò chơi **XO (Tic-Tac-Toe)**, viết bằng **C** nhằm:
- ⚙️ Hiểu sâu về **Socket programming** (TCP/UDP tùy triển khai)
- 🧠 Tổ chức **state game** + **room/matchmaking**
- 🔄 Xử lý đồng thời nhiều client (thread/select/epoll tùy)
- 🧩 Thiết kế **protocol** gọn, rõ, dễ debug
- 🚀 Tối ưu hiệu năng & kiểm soát tài nguyên (malloc/free)

---

## 🔥 Tính năng
- 🎲 **Realtime multiplayer**: 2 người chơi 1 ván
- 🧑‍🤝‍🧑 **Room / Match**: tạo phòng, vào phòng, ghép cặp (tùy code)
- 🧾 **Game state**: lưu trạng thái bàn cờ 3x3, lượt đi, kết quả
- ✅ **Validate nước đi**: chống đánh đè ô, đánh sai lượt, out-of-range
- 📣 **Broadcast**: server phát trạng thái cho cả 2 client sau mỗi lượt
- 🧵 **Concurrency**: hỗ trợ nhiều kết nối cùng lúc (tùy triển khai)
- 🧰 **Logging & Debug**: dễ theo dõi request/response

---

## 🖼️ Demo
> Bạn thay ảnh demo của bạn vào đây (giữ đúng tên file hoặc chỉnh lại đường dẫn).

### 📸 Screenshots
- `docs/demo-1.png`
- `docs/demo-2.png`
- `docs/demo-3.png`

Ví dụ nhúng ảnh:

<p align="center">
  <img src="docs/demo-1.png" alt="demo-1" width="75%" />
</p>

<p align="center">
  <img src="docs/demo-2.png" alt="demo-2" width="75%" />
</p>

<p align="center">
  <img src="docs/demo-3.png" alt="demo-3" width="75%" />
</p>

---

## 🧱 Kiến trúc
> (Mô tả theo mô hình chuẩn của game server realtime bằng C)

### 1) 📦 Thành phần chính
- 🖧 **Network Layer**
  - Accept kết nối, đọc/ghi gói tin
  - Parse message theo protocol
- 🎮 **Game Logic**
  - Kiểm tra luật chơi
  - Tính win/lose/draw
  - Chuyển lượt
- 🏠 **Room/Session Manager**
  - Quản lý phòng/phiên game
  - Gán player X/O
- 🧾 **State Storage**
  - Lưu board, players, turn, status
- 🧹 **Resource Management**
  - Cleanup khi client disconnect
  - Timeout / kick AFK (nếu có)

---

## 🗂️ Cấu trúc thư mục (gợi ý)
> Nếu repo của bạn đang khác, bạn cứ gửi cây thư mục, mình sẽ update lại y chang.

```text
.
├─ src/
│  ├─ main.c
│  ├─ server.c
│  ├─ client_handler.c
│  ├─ protocol.c
│  ├─ game.c
│  ├─ room.c
│  └─ utils.c
├─ include/
│  ├─ server.h
│  ├─ protocol.h
│  ├─ game.h
│  ├─ room.h
│  └─ utils.h
├─ tests/                # (optional)
├─ Makefile              # (optional)
└─ README.md
```

---

## 🧠 Luật chơi XO
- Bàn cờ 3x3
- Người chơi **X** đi trước
- Thắng khi có **3 ký hiệu liên tiếp** theo:
  - Hàng ngang ➖
  - Cột dọc ➕
  - Đường chéo ❌
- Hòa khi đầy bàn mà không ai thắng 🤝

---

## 🔌 Giao thức Client/Server
> Server & client nói chuyện bằng message. Có 2 kiểu thường gặp:
- **Text protocol** (dễ debug bằng telnet/netcat)
- **Binary protocol** (nhẹ, nhanh hơn)

### ✅ A) Text protocol (minh họa)
**Ví dụ định dạng:**
- `JOIN <room>`
- `MOVE <row> <col>`
- `STATE <...>`
- `ERROR <message>`
- `WIN / LOSE / DRAW`

**Ví dụ luồng game:**
1. Client A: `JOIN room1`
2. Client B: `JOIN room1`
3. Server: `START X A O B`
4. Client X: `MOVE 0 2`
5. Server: `STATE ...`
6. ... đến khi `WIN/LOSE/DRAW`

### ✅ B) JSON-like (minh họa)
```json
{ "type": "move", "row": 1, "col": 2 }
```

---

## 🛠️ Build & Run
> Giữ nguyên phần build mẫu bên dưới; phần **chạy thực tế** theo đúng repo/ứng dụng của bạn nằm ở mục **“Chạy server & client (PowerShell, build_ascii)”**.

### ✅ Cách 1: GCC nhanh
```bash
gcc -O2 -Wall -Wextra -Iinclude -o server src/*.c
./server
```

### ✅ Cách 2: Makefile
```bash
make
./server
```

### ▶️ Chạy client (nếu có)
```bash
./client 127.0.0.1 7777
```

### ▶️ Chạy server & client (PowerShell, build_ascii)
Mở **3 terminal PowerShell riêng**, làm đúng thứ tự này:

**Terminal 1: chạy server**
```powershell
cd "D:\Học tập\Game Server C++ Realtime XO\build_ascii"
.\nexon_mini_server.exe
```

**Terminal 2: Client A (tạo phòng)**
```powershell
cd "D:\Học tập\Game Server C++ Realtime XO\build_ascii"
.\nexon_mini_client.exe
```
Sau khi vào client, gõ:
```text
LOGIN alice
CREATE_ROOM phong1
```

**Terminal 3: Client B (join phòng)**
```powershell
cd "D:\Học tập\Game Server C++ Realtime XO\build_ascii"
.\nexon_mini_client.exe
```
Sau khi vào client, gõ:
```text
LOGIN bob
LIST_ROOMS
JOIN_ROOM 1
```

### 🎮 Chơi XO (lệnh MOVE)
Hai client thay phiên gõ lệnh:
```text
MOVE 0
MOVE 4
MOVE 1
...
```
Ô hợp lệ là từ **0 đến 8**.

### ⏹️ Dừng server
Quay lại **Terminal 1** (đang chạy server), nhấn **Ctrl + C** để tắt.

---

## 🧪 Hướng dẫn test
### 1) Test nhanh bằng 2 client 
- Làm theo quy trình **3 terminal PowerShell** ở mục **Build & Run**.
- Sau khi 2 client vào cùng phòng, test gameplay bằng các lệnh `MOVE <0..8>`.

---

## ❓ FAQ
### Q: Server dùng TCP hay UDP?
A: Realtime game thường dùng TCP cho đơn giản (đảm bảo thứ tự), nhưng có thể dùng UDP cho latency thấp. (Tùy code bạn đang triển khai.)

### Q: Khi client disconnect thì sao?
A: Server nên cleanup session và có thể báo kết quả (đối thủ thắng) hoặc đóng room.

---

## 🤝 Đóng góp
- Fork repo 🍴
- Tạo branch: `feature/your-feature`
- Commit rõ ràng ✅
- Mở Pull Request 🚀

---

## 📜 License

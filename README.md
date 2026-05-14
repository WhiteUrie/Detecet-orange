# Detect Orange - Webcam

Hệ thống phân loại cam sử dụng YOLO (Ultralytics) để phát hiện và phân loại kích thước, kèm Web UI bằng Flask, và module ESP32 để điều khiển phần cứng (relay/servo/sensor).

**Tổng quan**
- Nguồn ảnh chính: USB webcam (OpenCV). Web UI hiển thị stream, ảnh detect gần nhất, và thống kê.
- ESP32 chịu trách nhiệm điều khiển phần cứng và đồng bộ trạng thái với Flask server.
- Có sketch `esp32c3/esp32c3.ino` để tham khảo cấu hình điều khiển; `esp32cam/esp32cam.ino` là sketch thử nghiệm cho ESP32-CAM.

**Các file model**
- `best.pt` (tên chung, có thể thay đổi)
- `orange_nonorange_model_02.pt`
- `orange_nonorange_model_50e.pt`

## Yêu cầu
- Python 3.9 – 3.12 (project có script ưu tiên Python 3.11)
- pip
- Arduino CLI (tuỳ chọn, chỉ cần nếu muốn tự động cài ESP32 board và thư viện via `setup.py`)

## Cài đặt nhanh
1. Cài Python packages (khuyên dùng):

```bash
python setup.py --only-python
```

2. Hoặc để cả ESP32 setup (nếu có `arduino-cli`):

```bash
python setup.py
```

## Chạy ứng dụng
- Dùng helper `start.py` để chọn môi trường Python phù hợp và cài phụ thuộc nếu cần:

```bash
python start.py
```

- Hoặc chạy trực tiếp (đã cài dependency):

```bash
python app.py
```

- Mở trình duyệt vào: http://127.0.0.1:5000

## Biến môi trường quan trọng
- `CAMERA_INDEX` (mặc định `0`) — nếu có nhiều webcam, đặt index mong muốn.
- `CAMERA_WIDTH` (mặc định `640`)
- `CAMERA_HEIGHT` (mặc định `480`)
- `CAMERA_MAX_INDEX` (mặc định `4`) — số index tối đa `start.py` sẽ thử.
- `AUTO_DETECT_ON_HARDWARE_UPDATE` (`0` hoặc `1`) — nếu `1`, server sẽ chạy detect tự động khi ESP32 báo sensor edge trong `/api/hardware/update`.

Ví dụ trên Windows (tạm thời cho phiên bản terminal hiện hành):

```powershell
set CAMERA_INDEX=1
python start.py
```

## Công thức tính kích thước
Các thông số trong `app.py`:
- `PIXEL_TO_CM_RATIO = 0.0237`
- `DIAMETER_OFFSET_CM = 0`
- `CALIBRATION_SCALE = 1.1374`
- `CALIBRATION_OFFSET_CM = -0.476`

Tính toán:
1. measured_diameter_cm = raw_diameter_px * PIXEL_TO_CM_RATIO + DIAMETER_OFFSET_CM
2. diameter_cm = max(0, measured_diameter_cm * CALIBRATION_SCALE + CALIBRATION_OFFSET_CM)

Ngưỡng phân loại:
- SMALL: diameter < 6.0 cm
- MEDIUM: 6.0 cm <= diameter < 7.0 cm
- LARGE: diameter >= 7.0 cm

`NON_ORANGE` (ví dụ class id khác hoặc tên `non_orange`) sẽ không được tính đường kính.

## API chính (Flask)
- `GET /` → Dashboard (Web UI)
- `GET /video_feed` → MJPEG stream từ webcam
- `GET /api/detect` → Chạy 1 lần detect và trả về JSON
- `GET /api/stats` → Trạng thái hệ thống, thống kê, hardware, last capture
- `POST /api/control` → Gửi lệnh `START` / `STOP` / `RESET`
- `GET /api/command` → ESP32 poll `desired_state`
- `POST /api/hardware/update` → ESP32 gửi trạng thái relay/servo/sensor
- `GET /api/last_capture_image` → Ảnh detect gần nhất (JPEG)

## ESP32 (esp32c3/esp32c3.ino)
- Sketch kết nối Wi‑Fi và poll `GET /api/command` mỗi ~1s.
- Khi sensor chính (Sensor Main) phát hiện edge, ESP32 gọi `GET /api/detect` để lấy category, sau đó điều khiển relay/servo tương ứng.
- Trong sketch, các URL Flask (cần cập nhật theo địa chỉ máy chủ của bạn):
	- `flask_url` (detect)
	- `flask_hardware_url` (hardware update)
	- `flask_command_url` (command poll)

GPIO quan trọng (theo sketch):
- Relay conveyor: `RELAY1_CONVEYOR = 21`
- Relay small: `RELAY2_BRANCH_SMALL = 22`
- Relay medium: `RELAY3_BRANCH_MEDIUM = 23`
- Relay non-orange: `RELAY4_BRANCH_NON_ORANGE = 19`
- Sensor main: `SENSOR1_MAIN = 13` (edge detection)
- Servo pins: `SERVO1_PIN = 25`, `SERVO2_PIN = 26`, `SERVO3_NON_ORANGE_PIN = 33`

## Ghi chú & lưu ý
- `start.py` cố gắng sử dụng một interpreter Python tương thích (khuyến nghị Python 3.11). Nếu không tìm thấy, nó sẽ tạo `.venv311` (nếu có Python 3.11 trên hệ thống).
- `setup.py` có thể kiểm tra vị trí sketch theo tên thư mục viết hoa (`ESP32C3`, `ESP32CAM`) — repo này dùng tên thư mục chữ thường (`esp32c3`, `esp32cam`), nên phần validate có thể báo thiếu file dù chúng tồn tại. Đây chỉ là cảnh báo không ảnh hưởng tới chạy app.
- Nếu đổi file model (ví dụ `best.pt`), khởi động lại app để nạp model mới.
- Nếu không mở được webcam, app sẽ thử các index từ `CAMERA_INDEX` đến `CAMERA_MAX_INDEX`.

---

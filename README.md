# Detect Orange

Hệ thống này dùng Flask, OpenCV, Ultralytics YOLO và ESP32-C3 để nhận diện trái cam từ webcam USB, ước lượng kích thước, hiển thị dashboard web và điều khiển băng tải/phân nhánh phần cứng.

Ứng dụng chính chạy trên máy tính Windows. Flask mở giao diện web, lấy ảnh từ webcam, suy luận bằng file model `.pt`, sau đó trả kết quả cho dashboard và cho ESP32-C3 qua các API nội bộ.

## Thành Phần Chính

- [app.py](app.py) là Flask server chính, xử lý camera, YOLO, API và thống kê.
- [templates/index.html](templates/index.html) là dashboard web.
- [start.py](start.py) là script khởi động ưu tiên môi trường `.venv` trong repo và tự kiểm tra dependency cần thiết.
- [setup.py](setup.py) cài Python dependencies và có thể cài thêm ESP32 Arduino dependencies nếu có `arduino-cli`.
- [esp32c3/esp32c3.ino](esp32c3/esp32c3.ino) là sketch ESP32-C3 điều khiển relay, servo và sensor.
- [esp32cam/esp32cam.ino](esp32cam/esp32cam.ino) là sketch tham khảo cho ESP32-CAM.
- [nondetect/](nondetect/) lưu ảnh debug khi retry detect không tìm thấy vật thể.

## Tính Năng

- Stream camera trực tiếp trên dashboard qua `/video_feed`.
- Chạy detect một lần theo yêu cầu qua `/api/detect`.
- Phân loại kết quả thành `SMALL`, `MEDIUM`, `LARGE` hoặc `NON_ORANGE`.
- Hiển thị ảnh detect gần nhất có bounding box qua `/api/last_capture_image`.
- Đồng bộ trạng thái giữa Flask và ESP32-C3 qua `/api/command` và `/api/hardware/update`.
- Có nút điều khiển `START`, `STOP`, `RESET` trên web UI.
- Lưu ảnh thất bại trong `nondetect/` để debug.

## Cấu Trúc Dự Án

```text
Detecet-orange/
├─ app.py
├─ start.py
├─ setup.py
├─ README.md
├─ best.pt
├─ orange_nonorange_model_02.pt
├─ orange_nonorange_model_03.pt
├─ orange_nonorange_model_04.pt
├─ orange_nonorange_model_50e.pt
├─ orange_sorting_full.txt
├─ templates/
│  └─ index.html
├─ esp32c3/
│  └─ esp32c3.ino
├─ esp32cam/
│  └─ esp32cam.ino
└─ nondetect/
```

## Yêu Cầu

- Windows hoặc Linux có Python và webcam USB.
- Python 3.9 đến 3.12.
- `pip`.
- Arduino CLI là tùy chọn, chỉ cần nếu muốn `setup.py` tự cài phần ESP32.
- ESP32-C3, relay, servo và sensor nếu bạn chạy hệ thống thật.

## Cài Đặt

### Chỉ cài Python dependencies

```bash
python setup.py --only-python
```

### Cài Python và ESP32 dependencies nếu có Arduino CLI

```bash
python setup.py
```

### Bỏ qua phần ESP32

```bash
python setup.py --skip-esp32
```

## Chạy Ứng Dụng

### Cách khuyến nghị

```bash
python start.py
```

`start.py` sẽ ưu tiên môi trường `.venv` trong thư mục project, sau đó mới đến `.venv311` hoặc Python hiện tại nếu tương thích với stack YOLO.

### Chạy trực tiếp

```bash
python app.py
```

Sau khi khởi động, mở:

```text
http://127.0.0.1:5000
```

Ứng dụng cũng tự mở trình duyệt một lần khi chạy `app.py`.

## Luồng Hoạt Động

1. Flask mở dashboard và stream webcam.
2. Người dùng nhấn `START` trên web, server đổi `desired_state` sang `RUNNING`.
3. ESP32-C3 poll `/api/command` mỗi khoảng 1 giây để lấy trạng thái mong muốn.
4. Khi sensor chính kích hoạt, ESP32-C3 gọi `/api/detect` để lấy category từ Flask.
5. Flask phân loại cam, cập nhật thống kê và trả JSON.
6. ESP32-C3 điều khiển relay và servo để đẩy sản phẩm sang nhánh tương ứng.

## Model Suy Luận

Mặc định, `app.py` load model:

```text
orange_nonorange_model_04.pt
```

Các model còn lại trong repo là các biến thể để thử nghiệm hoặc so sánh:

- `best.pt`
- `orange_nonorange_model_02.pt`
- `orange_nonorange_model_03.pt`
- `orange_nonorange_model_04.pt`
- `orange_nonorange_model_50e.pt`

### Quy Tắc Phân Loại

Server coi các class có tên `orange`, `cam`, hoặc tên kết thúc bằng `_orange` là cam hợp lệ để đo kích thước. Các class như `non_orange` hoặc `nonorange` được đưa vào nhóm `NON_ORANGE`.

Ngưỡng hiện tại trong `app.py`:

- `SMALL`: nhỏ hơn 5.5 cm
- `MEDIUM`: từ 5.5 cm đến dưới 6.5 cm
- `LARGE`: từ 6.5 cm trở lên

### Cách Tính Đường Kính

`app.py` đang dùng các hằng số sau:

- `PIXEL_TO_CM_RATIO = 0.0237`
- `DIAMETER_OFFSET_CM = 0`
- `CALIBRATION_SCALE = 0.9886`
- `CALIBRATION_OFFSET_CM = 0.7368`
- `A = 0.0000373307`
- `B = -0.0031015816`
- `C = 1.046601`

Luồng tính toán hiện tại:

1. Lấy bounding box đầu tiên từ YOLO.
2. Tính kích thước pixel của box.
3. Hiệu chỉnh theo vị trí box so với tâm ảnh.
4. Quy đổi sang cm bằng hệ số tuyến tính.
5. Áp calibration scale và offset để ra đường kính cuối.

## Cấu Hình Camera

Các biến môi trường được `app.py` đọc:

- `CAMERA_INDEX`: index webcam ưu tiên, mặc định `0`.
- `CAMERA_WIDTH`: độ rộng khung hình, mặc định `640`.
- `CAMERA_HEIGHT`: chiều cao khung hình, mặc định `480`.
- `CAMERA_MAX_INDEX`: index tối đa mà server thử mở, mặc định `4`.
- `AUTO_DETECT_ON_HARDWARE_UPDATE`: đặt `1` nếu muốn tự detect ngay khi `/api/hardware/update` báo sensor edge.

Ví dụ trên PowerShell:

```powershell
$env:CAMERA_INDEX = "1"
python start.py
```

## API Flask

### `GET /`

Trả về dashboard web.

### `GET /video_feed`

Stream MJPEG từ webcam USB.

### `GET /api/detect`

Chạy detect một lần với retry. Nếu detect thành công, trả về category và `diameter_cm`. Nếu không thấy vật thể sau nhiều lần thử, server lưu ảnh debug vào `nondetect/` và trả `NONE`.

### `GET /api/stats`

Trả trạng thái tổng hợp của hệ thống, gồm:

- `state`
- `desired_state`
- `esp32_state`
- `counts`
- `last_capture`
- `hardware_status`
- `hardware_online`
- `hardware_last_update`

### `POST /api/control`

Nhận payload JSON có `command`:

- `START`
- `STOP`
- `RESET`

`RESET` đặt lại bộ đếm về 0 và đưa hệ thống về `IDLE`.

### `GET /api/command`

ESP32-C3 dùng endpoint này để poll `desired_state`.

### `POST /api/hardware/update`

ESP32-C3 gửi trạng thái `relay`, `servo`, `sensor` và `state` hiện tại lên đây.

### `GET /api/last_capture_image`

Trả ảnh detect gần nhất có bounding box. Nếu chưa có ảnh, server trả `204`.

## Dashboard Web

Giao diện trong [templates/index.html](templates/index.html) gồm:

- khu vực trạng thái phần cứng,
- camera stream trực tiếp,
- ảnh detect gần nhất,
- bộ đếm `Large`, `Medium`, `Small`, `Non Orange`,
- nút điều khiển `START`, `STOP`, `RESET`.

Dashboard tự gọi `/api/stats` mỗi giây để cập nhật trạng thái.

## ESP32-C3

Sketch chính nằm ở [esp32c3/esp32c3.ino](esp32c3/esp32c3.ino).

### Chức Năng

- Kết nối Wi‑Fi.
- Poll `/api/command` định kỳ.
- Gửi trạng thái phần cứng lên `/api/hardware/update`.
- Gọi `/api/detect` khi cần phân loại sản phẩm.
- Điều khiển relay và servo theo category nhận được.

### Cần Sửa Trước Khi Nạp

Trong sketch hiện có các URL mẫu:

- `flask_url`
- `flask_hardware_url`
- `flask_command_url`

Bạn cần đổi các URL này sang IP của máy đang chạy Flask.

Sketch cũng đang chứa SSID và password Wi‑Fi mẫu, nên bạn phải thay bằng thông tin mạng của mình trước khi nạp lên board.

### GPIO Hiện Dùng

- `RELAY1_CONVEYOR = 17`
- `RELAY2_BRANCH_SMALL = 22`
- `RELAY3_BRANCH_MEDIUM = 23`
- `RELAY4_BRANCH_NON_ORANGE = 19`
- `SENSOR1_MAIN = 18`
- `SENSOR2_SMALL = 14`
- `SENSOR3_MEDIUM = 26`
- `SENSOR4_NON_ORANGE = 13`
- `SERVO1_PIN = 25`
- `SERVO2_PIN = 16`
- `SERVO3_NON_ORANGE_PIN = 21`

## Ghi Chú

- `setup.py` chỉ hỗ trợ cài đặt môi trường, không phải file chạy chính.
- Nếu đổi model YOLO, chỉ cần thay file `.pt` trong thư mục gốc và khởi động lại ứng dụng.
- Nếu camera không mở được, thử đặt `CAMERA_INDEX` khác trước khi chạy `start.py`.

# Detecet-orange

He thong phan loai cam su dung YOLOv8 + Flask + ESP32-C3.

## Tong quan hien tai

- Nguon anh detect la USB webcam tren may tinh (OpenCV).
- ESP32-C3 chi dong vai tro dieu khien phan cung va dong bo trang thai len Flask.
- ESP32-CAM co trong du an de test stream/capture rieng, nhung luong detect chinh dang dung webcam PC.
- Web UI hien thi:
1. Trang thai phan cung ESP32-C3 (relay/servo/sensor + online/offline)
2. Camera stream realtime
3. Anh detect gan nhat co bounding box (an label va confidence)
4. Thong ke + nut START/STOP/RESET

## Cai dat

Chi cai Python packages:

```bash
python setup.py --only-python
```

## Chay he thong

```bash
python start.py
```

Mo trinh duyet: http://127.0.0.1:5000

`start.py` se:
- Tu chon interpreter tuong thich YOLO (Python 3.9 -> 3.12)
- Uu tien `.venv311`, sau do `.venv`, sau do interpreter hien tai
- Neu chua co moi truong hop le se tao `.venv311`
- Tu cai cac goi thieu: flask, requests, numpy, opencv-python, ultralytics

## Bien moi truong quan trong

- `CAMERA_INDEX` (mac dinh `0`)
- `CAMERA_WIDTH` (mac dinh `640`)
- `CAMERA_HEIGHT` (mac dinh `480`)
- `CAMERA_MAX_INDEX` (mac dinh `4`)
- `AUTO_DETECT_ON_HARDWARE_UPDATE` (mac dinh `0`)

Vi du doi camera index tren Windows:

```bash
set CAMERA_INDEX=1
python start.py
```

## Cong thuc do kich thuoc

Thong so trong `app.py`:
- `PIXEL_TO_CM_RATIO = 0.0237`
- `DIAMETER_OFFSET_CM = 0`
- `CALIBRATION_SCALE = 1.1374`
- `CALIBRATION_OFFSET_CM = -0.476`

Duong kinh duoc tinh:

1. `measured_diameter_cm = raw_diameter_px * PIXEL_TO_CM_RATIO + DIAMETER_OFFSET_CM`
2. `diameter_cm = max(0, measured_diameter_cm * CALIBRATION_SCALE + CALIBRATION_OFFSET_CM)`

Nguong phan loai:
- SMALL: `< 6.0 cm`
- MEDIUM: `>= 6.0 cm` va `< 7.0 cm`
- LARGE: `>= 7.0 cm`

## API Flask hien tai

- `GET /` -> giao dien dashboard
- `GET /video_feed` -> stream MJPEG tu webcam
- `GET /api/detect` -> detect 1 lan
- `GET /api/stats` -> thong tin UI (counts, state, hardware, last capture)
- `POST /api/control` -> nhan lenh START / STOP / RESET
- `GET /api/command` -> ESP32-C3 poll `desired_state`
- `POST /api/hardware/update` -> ESP32-C3 push relay/servo/sensor/state
- `GET /api/last_capture_image` -> anh detect gan nhat

## Logic ESP32-C3 hien tai (esp32c3/esp32c3.ino)

State machine:
- `IDLE` -> cho lenh RUNNING
- `RUNNING` -> bat relay conveyor, theo doi canh sensor main (edge detection)
- `DETECTING` -> goi Flask `/api/detect` 1 lan
- `SORT_SMALL` -> bat relay nhanh small + servo small (giu 7s)
- `SORT_MEDIUM` -> bat relay nhanh medium + servo medium (giu 7s)
- `SORT_LARGE` -> khong gat servo, cho vat di thang (delay 2s)
- `NONE` (default case) -> detect khong hop le, cho 2s roi quay lai RUNNING

Ghi chu quan trong ve trigger:
- Sensor main dung edge detection de tranh trigger lien tuc khi vat nam tren sensor.
- Sau khi hoan tat moi chu ky sort (SMALL/MEDIUM/LARGE/NONE), `hasBeenDetected` duoc reset ve `false` de cho phep lan detect tiep theo.

## Luu y

- O khung anh detect chi hien thi bounding box, khong hien thi ten class/confidence.
- Neu khong mo duoc webcam, app se thu lan luot tu `CAMERA_INDEX` den `CAMERA_MAX_INDEX`.
- Sau khi thay doi model `best.pt`, can khoi dong lai app.
- File `setup.py` hien co phan check ten thu muc sketch theo dang viet hoa (`ESP32C3`, `ESP32CAM`). Cau truc repo thuc te la viet thuong (`esp32c3`, `esp32cam`), vi vay muc validate co the bao thieu file du sketch ton tai.
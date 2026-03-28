from flask import Flask, request, jsonify, render_template
import os
import threading
import time
import webbrowser
import requests
import cv2
import numpy as np
from ultralytics import YOLO

app = Flask(__name__)
model = YOLO('best.pt') # Model YOLOv8 nhận diện cam của bạn


@app.before_request
def log_request_line():
    print(f"[REQ] {request.method} {request.path} from {request.remote_addr}", flush=True)

ESP32_CAM_URL = "http://10.105.143.194/capture"
PIXEL_TO_CM_RATIO = 0.026 # Hệ số cần được bạn calibrate thực tế
alpha_ema = 0.5 # Hệ số EMA smoothing
ema_diameter = 0

# Biến đếm thống kê cho Web UI
stats = {"small": 0, "medium": 0, "large": 0}
last_capture_event = {"captured_at": 0, "category": "-", "diameter_cm": 0}
hardware_status = {
    "relay": {
        "relay1_conveyor": "UNKNOWN",
        "relay2_small": "UNKNOWN",
        "relay3_medium": "UNKNOWN",
    },
    "servo": {
        "servo_small": "UNKNOWN",
        "servo_medium": "UNKNOWN",
    },
    "sensor": {
        "sensor_main": "UNKNOWN",
        "sensor_small": "UNKNOWN",
        "sensor_medium": "UNKNOWN",
    },
}
last_hardware_update = 0
desired_state = "IDLE"  # Lệnh mong muốn từ Web (START/STOP)
esp32_state = "IDLE"    # Trạng thái thật ESP32-C3 báo về
last_sensor_main_status = "UNKNOWN"
last_auto_detect_ms = 0

def get_image_from_cam():
    resp = requests.get(ESP32_CAM_URL, stream=True, timeout=3)
    image = np.asarray(bytearray(resp.content), dtype="uint8")
    image = cv2.imdecode(image, cv2.IMREAD_COLOR)
    if image is None:
        raise RuntimeError("Camera frame decode failed")
    return image


def run_detection_once():
    global ema_diameter, stats, last_capture_event
    captured_at = int(time.time() * 1000)
    try:
        img = get_image_from_cam()
    except Exception as ex:
        last_capture_event = {"captured_at": captured_at, "category": "ERROR", "diameter_cm": 0}
        return {"status": "error", "message": f"Camera capture failed: {ex}"}

    # Chạy YOLO inference
    results = model(img)

    if len(results[0].boxes) == 0:
        last_capture_event = {"captured_at": captured_at, "category": "NONE", "diameter_cm": 0}
        return {"status": "error", "message": "No orange detected"}

    # Lấy bounding box của quả cam đầu tiên (có độ tin cậy cao nhất)
    box = results[0].boxes[0].xyxy[0].tolist()  # [x1, y1, x2, y2]
    width = box[2] - box[0]
    height = box[3] - box[1]

    # Đo kích thước và EMA
    raw_diameter_px = max(width, height)
    raw_diameter_cm = raw_diameter_px * PIXEL_TO_CM_RATIO
    ema_diameter = (alpha_ema * raw_diameter_cm) + ((1 - alpha_ema) * ema_diameter)

    # Phân loại
    category = "LARGE"
    if ema_diameter < 5.0:
        category = "SMALL"
    elif ema_diameter < 8.0:
        category = "MEDIUM"

    stats[category.lower()] += 1
    result = {
        "status": "success",
        "category": category,
        "diameter_cm": round(ema_diameter, 2),
    }
    last_capture_event = {
        "captured_at": captured_at,
        "category": category,
        "diameter_cm": result["diameter_cm"],
    }
    return result

@app.route('/api/detect', methods=['GET'])
def detect_orange():
    return jsonify(run_detection_once())

@app.route('/api/hardware/update', methods=['POST'])
def update_hardware_status():
    global hardware_status, last_hardware_update, esp32_state, last_sensor_main_status, last_auto_detect_ms
    data = request.json or {}

    relay = data.get("relay")
    servo = data.get("servo")
    sensor = data.get("sensor")
    state = data.get("state")

    if not isinstance(relay, dict) or not isinstance(servo, dict) or not isinstance(sensor, dict):
        return jsonify({"status": "error", "message": "Invalid hardware payload"}), 400

    hardware_status = {
        "relay": relay,
        "servo": servo,
        "sensor": sensor,
    }
    last_hardware_update = int(time.time() * 1000)

    if isinstance(state, str) and state:
        esp32_state = state
        print(f"[HW] ESP32 state update: {esp32_state}", flush=True)

    sensor_main = str(sensor.get("sensor_main", "UNKNOWN"))
    now_ms = int(time.time() * 1000)

    # Trigger detect theo cạnh lên của Sensor Main khi hệ thống đang RUNNING.
    if (
        desired_state == "RUNNING"
        and sensor_main == "TRIGGERED"
        and last_sensor_main_status != "TRIGGERED"
        and (now_ms - last_auto_detect_ms) > 1200
    ):
        print("[HW] Sensor Main edge detected -> running YOLO capture", flush=True)
        detect_result = run_detection_once()
        last_auto_detect_ms = now_ms
        print(f"[HW] Auto detect result: {detect_result}", flush=True)

    last_sensor_main_status = sensor_main

    return jsonify({"status": "success", "updated_at": last_hardware_update, "esp32_state": esp32_state})


@app.route('/api/command', methods=['GET'])
def get_command():
    # Endpoint để ESP32-C3 poll lệnh START/STOP
    print(f"[CMD] /api/command -> desired_state={desired_state}", flush=True)
    return jsonify({"status": "success", "desired_state": desired_state})

@app.route('/')
def index():
    # Render file giao diện HTML
    return render_template('index.html', esp32_cam_ip="http://10.105.143.194")

@app.route('/api/stats', methods=['GET'])
def get_stats():
    # API để Web UI liên tục cập nhật số lượng
    now_ms = int(time.time() * 1000)
    hardware_online = (now_ms - last_hardware_update) <= 5000 if last_hardware_update else False

    # Ưu tiên hiển thị trạng thái theo lệnh người dùng (desired_state)
    # để nút START/STOP không bị nhảy sai khi ESP32 chưa kịp phản hồi.
    if desired_state == "RUNNING":
        system_state = "RUNNING"
    elif desired_state == "IDLE":
        system_state = "IDLE"
    elif hardware_online:
        system_state = esp32_state
    else:
        system_state = desired_state

    return jsonify({
        "status": "success",
        "state": system_state,
        "desired_state": desired_state,
        "esp32_state": esp32_state,
        "counts": stats,
        "last_capture": last_capture_event,
        "hardware_status": hardware_status,
        "hardware_online": hardware_online,
        "hardware_last_update": last_hardware_update
    })

@app.route('/api/control', methods=['POST'])
def control_system():
    global stats, desired_state
    data = request.json or {}
    command = data.get('command')
    
    if command == "START":
        desired_state = "RUNNING"
        print("[CTRL] START received -> desired_state=RUNNING", flush=True)
    elif command == "STOP":
        desired_state = "IDLE"
        print("[CTRL] STOP received -> desired_state=IDLE", flush=True)
    elif command == "RESET":
        # Reset toàn bộ biến đếm về 0
        stats = {"small": 0, "medium": 0, "large": 0}
        desired_state = "IDLE"
        print("[CTRL] RESET received -> reset counts & desired_state=IDLE", flush=True)

    # Trả về trạng thái hiển thị (ưu tiên desired_state)
    display_state = "RUNNING" if desired_state == "RUNNING" else (esp32_state or "IDLE")
    return jsonify({"status": "success", "state": display_state, "desired_state": desired_state})

# Bạn có thể thêm các route @app.route('/') cho Web UI ở đây
if __name__ == '__main__':
    host = '0.0.0.0'
    port = 5000
    # Chỉ mở browser 1 lần khi chạy app (tránh mở 2 tab do reloader).
    if os.environ.get('WERKZEUG_RUN_MAIN') != 'true':
        threading.Timer(1.0, lambda: webbrowser.open(f'http://127.0.0.1:{port}')).start()

    app.run(host=host, port=port, debug=True, use_reloader=False)
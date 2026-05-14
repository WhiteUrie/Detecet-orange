from flask import Flask, request, jsonify, render_template
import os
import threading
import time
import atexit
import webbrowser
import cv2
from ultralytics import YOLO
from datetime import datetime

app = Flask(__name__)
model = YOLO('orange_nonorange_model_04.pt') 


@app.before_request
def log_request_line():
    print(f"[REQ] {request.method} {request.path} from {request.remote_addr}", flush=True)

ESP32_CAM_URL = "http://10.208.198.28/capture"
PIXEL_TO_CM_RATIO = 0.0237 # Hệ số cần được bạn calibrate thực tế
DIAMETER_OFFSET_CM = 0  # sai số
CALIBRATION_SCALE = 1.1374
CALIBRATION_OFFSET_CM = -0.476
CAMERA_INDEX = int(os.environ.get("CAMERA_INDEX", "0"))
CAMERA_WIDTH = int(os.environ.get("CAMERA_WIDTH", "640"))
CAMERA_HEIGHT = int(os.environ.get("CAMERA_HEIGHT", "480"))
CAMERA_MAX_INDEX = int(os.environ.get("CAMERA_MAX_INDEX", "4"))
AUTO_DETECT_ON_HARDWARE_UPDATE = os.environ.get("AUTO_DETECT_ON_HARDWARE_UPDATE", "0") == "1"
# Tạo thư mục nondetect nếu chưa có
NONDETECT_FOLDER = "nondetect"
if not os.path.exists(NONDETECT_FOLDER):
    os.makedirs(NONDETECT_FOLDER, exist_ok=True)

camera_lock = threading.Lock()
capture_image_lock = threading.Lock()
camera = None
active_camera_index = None
last_camera_open_failed_at = 0
last_capture_image_jpeg = None

# Biến đếm thống kê cho Web UI
stats = {"small": 0, "medium": 0, "large": 0, "non_orange": 0}
last_capture_event = {"captured_at": 0, "category": "-", "diameter_cm": 0}
hardware_status = {
    "relay": {
        "relay1_conveyor": "UNKNOWN",
        "relay2_small": "UNKNOWN",
        "relay3_medium": "UNKNOWN",
        "relay4_non_orange": "UNKNOWN",
    },
    "servo": {
        "servo_small": "UNKNOWN",
        "servo_medium": "UNKNOWN",
        "servo_non_orange": "UNKNOWN",
    },
    "sensor": {
        "sensor_main": "UNKNOWN",
        "sensor_small": "UNKNOWN",
        "sensor_medium": "UNKNOWN",
        "sensor_non_orange": "UNKNOWN",
    },
}
last_hardware_update = 0
desired_state = "IDLE"  # Lệnh mong muốn từ Web (START/STOP)
esp32_state = "IDLE"    # Trạng thái thật ESP32-C3 báo về
last_sensor_main_status = "UNKNOWN"
last_auto_detect_ms = 0
last_control_ms = 0

def get_image_from_cam():
    global camera, active_camera_index, last_camera_open_failed_at

    with camera_lock:
        if camera is None or not camera.isOpened():
            now_ms = int(time.time() * 1000)
            # Throttle camera re-open attempts to avoid heavy warning spam.
            if (now_ms - last_camera_open_failed_at) < 1200:
                raise RuntimeError("Webcam is not ready yet")

            candidate_indexes = [CAMERA_INDEX]
            for idx in range(0, CAMERA_MAX_INDEX + 1):
                if idx not in candidate_indexes:
                    candidate_indexes.append(idx)

            opened = None
            opened_index = None
            for idx in candidate_indexes:
                cap = cv2.VideoCapture(idx, cv2.CAP_DSHOW)
                if not cap.isOpened():
                    cap.release()
                    cap = cv2.VideoCapture(idx)

                if cap.isOpened():
                    opened = cap
                    opened_index = idx
                    break
                cap.release()

            if opened is None:
                last_camera_open_failed_at = now_ms
                raise RuntimeError(
                    f"Cannot open USB webcam. Tried indexes: {candidate_indexes}. "
                    "Set CAMERA_INDEX manually if needed."
                )

            camera = opened
            active_camera_index = opened_index
            print(f"[CAM] Using webcam index: {active_camera_index}", flush=True)
            camera.set(cv2.CAP_PROP_FRAME_WIDTH, CAMERA_WIDTH)
            camera.set(cv2.CAP_PROP_FRAME_HEIGHT, CAMERA_HEIGHT)

        ok, frame = camera.read()

    if not ok or frame is None:
        raise RuntimeError("Webcam frame read failed")
    return frame


def release_camera():
    global camera, active_camera_index
    with camera_lock:
        if camera is not None:
            camera.release()
            camera = None
            active_camera_index = None


atexit.register(release_camera)


def run_detection_once():
    global stats, last_capture_event, last_capture_image_jpeg
    captured_at = int(time.time() * 1000)
    
    try:
        img = get_image_from_cam()
    except Exception as ex:
        last_capture_event = {"captured_at": captured_at, "category": "ERROR", "diameter_cm": 0}
        return {"status": "error", "message": f"Camera capture failed: {ex}"}

    # 1. Chạy YOLO với cấu hình chuẩn (giống hệt file test)
    results = model(img, conf=0.99, verbose=False)

    # Nếu không có vật thể nào vượt qua ngưỡng conf=0.9
    if len(results[0].boxes) == 0:
        last_capture_event = {"captured_at": captured_at, "category": "NONE", "diameter_cm": 0}
        return {"status": "error", "message": "No orange detected"}

    # 2. Lấy trực tiếp box đầu tiên 
    target_box = results[0].boxes[0].xyxy[0].tolist()
    target_cls_raw = results[0].boxes[0].cls[0].item()
    target_cls_id = int(target_cls_raw)
    class_name = str(results[0].names.get(target_cls_id, "")).strip().lower()
    normalized_class_name = class_name.replace("-", "_").replace(" ", "_")

    # 3. Chỉ class orange mới phân loại theo kích thước, còn lại là NON_ORANGE.
    diameter_cm = 0.0
    category = "NON_ORANGE"
    is_non_orange = normalized_class_name in {"non_orange", "nonorange"}
    is_orange = (not is_non_orange) and (
        target_cls_id == 0
        or normalized_class_name in {"orange", "cam"}
        or normalized_class_name.endswith("_orange")
    )
    if is_orange:
        # 4. Chỉ tính kích thước cho orange.
        width = target_box[2] - target_box[0]
        height = target_box[3] - target_box[1]
        raw_diameter_px = max(width, height)
        measured_diameter_cm = (raw_diameter_px * PIXEL_TO_CM_RATIO) + DIAMETER_OFFSET_CM
        diameter_cm = max(0, (measured_diameter_cm * CALIBRATION_SCALE) + CALIBRATION_OFFSET_CM)

        category = "LARGE"
        if diameter_cm < 6.0:
            category = "SMALL"
        elif diameter_cm < 7.0:
            category = "MEDIUM"

    # Cập nhật thống kê
    stats_key = "non_orange" if category == "NON_ORANGE" else category.lower()
    stats[stats_key] += 1

    # Lưu lại ảnh có gắn bounding box để hiển thị trên web UI.
    annotated_frame = results[0].plot(labels=False, conf=False)
    ok, encoded = cv2.imencode('.jpg', annotated_frame)
    if ok:
        with capture_image_lock:
            last_capture_image_jpeg = encoded.tobytes()

    result = {
        "status": "success",
        "category": category,
        "diameter_cm": round(diameter_cm, 2),
    }
    
    last_capture_event = {
        "captured_at": captured_at,
        "category": category,
        "diameter_cm": result["diameter_cm"],
    }
    
    return result

def run_detection_with_retry(max_attempts=4):
    """
    Detect object with retry logic: attempt up to 4 times, save failed images to nondetect folder.
    """
    global stats, last_capture_event, last_capture_image_jpeg
    
    captured_at = int(time.time() * 1000)
    failed_images = []
    
    for attempt in range(1, max_attempts + 1):
        try:
            img = get_image_from_cam()
        except Exception as ex:
            last_capture_event = {"captured_at": captured_at, "category": "ERROR", "diameter_cm": 0}
            return {"status": "error", "message": f"Camera capture failed: {ex}"}
        
        failed_images.append(img.copy())
        results = model(img, conf=0.5, verbose=False)
        
        # If detection successful, return result
        if len(results[0].boxes) > 0:
            target_box = results[0].boxes[0].xyxy[0].tolist()
            target_cls_raw = results[0].boxes[0].cls[0].item()
            target_cls_id = int(target_cls_raw)
            class_name = str(results[0].names.get(target_cls_id, "")).strip().lower()
            normalized_class_name = class_name.replace("-", "_").replace(" ", "_")
            
            diameter_cm = 0.0
            category = "NON_ORANGE"
            is_non_orange = normalized_class_name in {"non_orange", "nonorange"}
            is_orange = (not is_non_orange) and (
                target_cls_id == 0
                or normalized_class_name in {"orange", "cam"}
                or normalized_class_name.endswith("_orange")
            )
            if is_orange:
                width = target_box[2] - target_box[0]
                height = target_box[3] - target_box[1]
                raw_diameter_px = max(width, height)
                measured_diameter_cm = (raw_diameter_px * PIXEL_TO_CM_RATIO) + DIAMETER_OFFSET_CM
                diameter_cm = max(0, (measured_diameter_cm * CALIBRATION_SCALE) + CALIBRATION_OFFSET_CM)
                
                category = "LARGE"
                if diameter_cm < 6.0:
                    category = "SMALL"
                elif diameter_cm < 7.0:
                    category = "MEDIUM"
            
            stats_key = "non_orange" if category == "NON_ORANGE" else category.lower()
            stats[stats_key] += 1
            
            annotated_frame = results[0].plot(labels=False, conf=False)
            ok, encoded = cv2.imencode('.jpg', annotated_frame)
            if ok:
                with capture_image_lock:
                    last_capture_image_jpeg = encoded.tobytes()
            
            result = {
                "status": "success",
                "category": category,
                "diameter_cm": round(diameter_cm, 2),
                "attempt": attempt,
            }
            
            last_capture_event = {
                "captured_at": captured_at,
                "category": category,
                "diameter_cm": result["diameter_cm"],
            }
            
            return result
        
        # No detection on this attempt, continue to next
        if attempt < max_attempts:
            print(f"[DETECT] No detection on attempt {attempt}/{max_attempts}, retrying...", flush=True)
            time.sleep(0.1)
        else:
            # All attempts failed, save images to nondetect folder
            print(f"[DETECT] No detection after {max_attempts} attempts, saving debug images", flush=True)
            timestamp_str = datetime.now().strftime("%Y%m%d_%H%M%S")
            
            try:
                for idx, failed_img in enumerate(failed_images, 1):
                    filename = f"{NONDETECT_FOLDER}/{timestamp_str}_attempt{idx}.jpg"
                    cv2.imwrite(filename, failed_img)
                    print(f"[DETECT] Saved: {filename}", flush=True)
            except Exception as e:
                print(f"[DETECT] Error saving debug images: {e}", flush=True)
            
            # Return NONE - ESP32 will continue conveyor motion
            last_capture_event = {"captured_at": captured_at, "category": "NONE", "diameter_cm": 0}
            return {"status": "success", "category": "NONE", "attempt": max_attempts}
    
    return {"status": "error", "message": "Detection retry loop failed"}


@app.route('/api/detect', methods=['GET'])
def detect_orange():
    return jsonify(run_detection_with_retry())

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

    sensor_main = str(sensor.get("sensor_main", "UNKNOWN")).upper()
    now_ms = int(time.time() * 1000)

    # Mac dinh tat auto-detect o /api/hardware/update de tranh dem trung.
    # ESP32-C3 da chu dong goi /api/detect trong state DETECTING.
    if AUTO_DETECT_ON_HARDWARE_UPDATE:
        if (
            sensor_main == "TRIGGERED"
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
    return render_template('index.html')


@app.route('/video_feed')
def video_feed():
    def generate_frames():
        while True:
            try:
                frame = get_image_from_cam()
                ok, buffer = cv2.imencode('.jpg', frame)
                if not ok:
                    time.sleep(0.05)
                    continue

                yield (
                    b'--frame\r\n'
                    b'Content-Type: image/jpeg\r\n\r\n' + buffer.tobytes() + b'\r\n'
                )
            except Exception:
                time.sleep(0.2)

    return app.response_class(
        generate_frames(),
        mimetype='multipart/x-mixed-replace; boundary=frame'
    )


@app.route('/api/last_capture_image', methods=['GET'])
def last_capture_image():
    with capture_image_lock:
        image_bytes = last_capture_image_jpeg

    if image_bytes is None:
        return app.response_class(status=204)

    response = app.response_class(image_bytes, mimetype='image/jpeg')
    response.headers['Cache-Control'] = 'no-store, no-cache, must-revalidate, max-age=0'
    return response

@app.route('/api/stats', methods=['GET'])
def get_stats():
    # API để Web UI liên tục cập nhật số lượng
    global desired_state
    now_ms = int(time.time() * 1000)
    hardware_online = (now_ms - last_hardware_update) <= 5000 if last_hardware_update else False

    hw_status_display = hardware_status.copy() if hardware_status else {}
    if "sensor" in hw_status_display and isinstance(hw_status_display["sensor"], dict):
        sensor_copy = hw_status_display["sensor"].copy()

        if "sensor_small" in sensor_copy:
            sensor_copy["sensor_small"] = "WAITING" if sensor_copy["sensor_small"] == "TRIGGERED" else "TRIGGERED"
        if "sensor_medium" in sensor_copy:
            sensor_copy["sensor_medium"] = "WAITING" if sensor_copy["sensor_medium"] == "TRIGGERED" else "TRIGGERED"
        if "sensor_non_orange" in sensor_copy:
            sensor_copy["sensor_non_orange"] = "WAITING" if sensor_copy["sensor_non_orange"] == "TRIGGERED" else "TRIGGERED"
        hw_status_display["sensor"] = sensor_copy

    if (
        desired_state == "RUNNING"
        and hardware_online
        and esp32_state == "IDLE"
        and (now_ms - last_control_ms) > 5000
    ):
        desired_state = "IDLE"
        print("[SYNC] Auto reset desired_state -> IDLE (ESP32 still IDLE)", flush=True)

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
        "hardware_status": hw_status_display,
        "hardware_online": hardware_online,
        "hardware_last_update": last_hardware_update
    })

@app.route('/api/control', methods=['POST'])
def control_system():
    global stats, desired_state, last_control_ms
    data = request.json or {}
    command = data.get('command')
    now_ms = int(time.time() * 1000)
    
    if command == "START":
        desired_state = "RUNNING"
        last_control_ms = now_ms
        print("[CTRL] START received -> desired_state=RUNNING", flush=True)
    elif command == "STOP":
        desired_state = "IDLE"
        last_control_ms = now_ms
        print("[CTRL] STOP received -> desired_state=IDLE", flush=True)
    elif command == "RESET":
        # Reset toàn bộ biến đếm về 0
        stats = {"small": 0, "medium": 0, "large": 0, "non_orange": 0}
        desired_state = "IDLE"
        last_control_ms = now_ms
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
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

const char* ssid = "Wifi Thu Nghiem";
const char* password = "daubepgioi";
String flask_url = "http://192.168.1.49:5000/api/detect";
String flask_hardware_url = "http://192.168.1.49:5000/api/hardware/update";
String flask_command_url = "http://192.168.1.49:5000/api/command";

HTTPClient hwStatusClient;
HTTPClient stateClient;
HTTPClient detectClient;

// Khai báo chân GPIO
const int RELAY4_BRANCH_NON_ORANGE = 19;
const int RELAY1_CONVEYOR = 17;
const int RELAY2_BRANCH_SMALL = 22;
const int RELAY3_BRANCH_MEDIUM = 23;

const int SENSOR4_NON_ORANGE = 13; 
const int SENSOR1_MAIN = 18;
const int SENSOR2_SMALL = 14;
const int SENSOR3_MEDIUM = 26;

const int SERVO3_NON_ORANGE_PIN = 21;
const int SERVO1_PIN = 25;
const int SERVO2_PIN = 16;

Servo servoSmall;
Servo servoMedium;
Servo servoNonOrange;

enum State { IDLE, RUNNING, DETECTING, SORT_SMALL, SORT_MEDIUM, SORT_NON_ORANGE, SORT_LARGE, NONE };
State currentState = IDLE;

// Biến cho non-blocking delay
unsigned long servoTimer = 0;
bool isServoActive = false;
bool hasBeenDetected = false;
bool sensorMainTriggered = false;
unsigned long large_timer = 0;
unsigned long none_timer = 0;
unsigned long detectDelayStartMs = 0;
const unsigned long DETECT_AFTER_RELAY_OFF_MS = 30;

// Theo doi canh trigger de tranh kich servo lap lai trong cung 1 lan vat di qua sensor.
bool lastSensorSmallTriggered = false;
bool lastSensorMediumTriggered = false;
bool lastSensorNonOrangeTriggered = false;
bool lastSensorMainTriggered = false;  // Thêm edge detection cho SENSOR1_MAIN
bool sensorSmallArmed = false;
bool sensorMediumArmed = false;
bool sensorNonOrangeArmed = false;

// Biến điều hướng HTTP Request
unsigned long commandPollTimer = 0;
unsigned long lastStatusPostTime = 0;
const unsigned long POLLING_INTERVAL = 1000; // Gọi API 1 giây/lần

int servoSmallAngle = 0;
int servoMediumAngle = 0;
int servoNonOrangeAngle = 0;

// --- CÁC HÀM TIỆN ÍCH ---

String extractDesiredState(String payload) {
  // Bỏ qua payload rỗng
  if (payload.length() == 0 || payload == "{}") return "IDLE";
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    return "IDLE";
  }
  
  // Lấy giá trị của key "desired_state", nếu không có thì mặc định là "IDLE"
  return doc["desired_state"] | "IDLE"; 
}

String stateToString(State st) {
  switch (st) {
    case IDLE: return "IDLE";
    case RUNNING: return "RUNNING";
    case DETECTING: return "DETECTING";
    case SORT_SMALL: return "SORT_SMALL";
    case SORT_MEDIUM: return "SORT_MEDIUM";
    case SORT_NON_ORANGE: return "SORT_NON_ORANGE";
    case SORT_LARGE: return "SORT_LARGE";
    default: return "IDLE";
  }
}

String relayStatus(int pin) {
  return digitalRead(pin) == HIGH ? "ON" : "OFF";
}

bool isSensorActiveLow(int pin) {
  // Sensor non-orange đang đấu ngược cực: TRIGGERED khi mức LOW.
  if (pin == SENSOR4_NON_ORANGE) return true;
  return true;
}

bool isTriggered(int pin) {
  int level = digitalRead(pin);
  if (isSensorActiveLow(pin)) {
    return level == LOW;
  }
  return level == HIGH;
}

String sensorStatus(int pin) {
  return isTriggered(pin) ? "TRIGGERED" : "WAITING";
}

String servoStatus(int angle) {
  return angle != 90 ? "ACTIVE" : "IDLE";
}

String parseCategoryFromResponse(String response) {
  // Bỏ qua response rỗng
  if (response.length() == 0 || response == "{}") return "NONE";
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    return "NONE";
  }
  
  // Chuẩn hóa category để tránh lệch kiểu viết: non orange/non-orange/non_orange.
  String category = doc["category"] | "NONE";
  category.trim();
  category.toUpperCase();
  category.replace("-", "_");
  category.replace(" ", "_");

  if (category == "NONORANGE") return "NON_ORANGE";
  return category;
}

// --- CÁC HÀM GIAO TIẾP MẠNG ---

void postHardwareStatus(HTTPClient* http) {
  if (WiFi.status() != WL_CONNECTED) return;
  
  // Sử dụng ArduinoJson để đóng gói dữ liệu chuẩn và an toàn
  StaticJsonDocument<512> doc;
  
  doc["state"] = stateToString(currentState);
  
  JsonObject relay = doc.createNestedObject("relay");
  relay["relay1_conveyor"] = relayStatus(RELAY1_CONVEYOR);
  relay["relay2_small"] = relayStatus(RELAY2_BRANCH_SMALL);
  relay["relay3_medium"] = relayStatus(RELAY3_BRANCH_MEDIUM);
  relay["relay4_non_orange"] = relayStatus(RELAY4_BRANCH_NON_ORANGE);
  
  JsonObject servo = doc.createNestedObject("servo");
  servo["servo_small"] = servoStatus(servoSmallAngle);
  servo["servo_medium"] = servoStatus(servoMediumAngle);
  servo["servo_non_orange"] = servoStatus(servoNonOrangeAngle);
  
  JsonObject sensor = doc.createNestedObject("sensor");
  sensor["sensor_main"] = sensorStatus(SENSOR1_MAIN);
  sensor["sensor_small"] = sensorStatus(SENSOR2_SMALL);
  sensor["sensor_medium"] = sensorStatus(SENSOR3_MEDIUM);
  sensor["sensor_non_orange"] = sensorStatus(SENSOR4_NON_ORANGE);

  String body;
  serializeJson(doc, body);

  http->begin(flask_hardware_url);
  http->addHeader("Content-Type", "application/json");
  http->POST(body);
  http->end();
}

String getDesiredState(HTTPClient* http) {
  if (WiFi.status() != WL_CONNECTED) return "IDLE";
  http->begin(flask_command_url);
  int code = http->GET();
  if (code <= 0) {
    http->end();
    return "IDLE";
  }
  String payload = http->getString();
  http->end();
  
  String ds = extractDesiredState(payload);
  if (ds == "RUNNING") return "RUNNING";
  return "IDLE";
}

String callFlaskAPI(HTTPClient* http) {
  http->begin(flask_url);
  int httpResponseCode = http->GET();
  String payload = "{}";
  if (httpResponseCode > 0) {
    payload = http->getString();
  }
  http->end();
  return payload; 
}

void checkServerConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[SERVER] WiFi is not connected, skip server check.");
    return;
  }

  HTTPClient checkClient;
  checkClient.begin(flask_command_url);
  int code = checkClient.GET();
  if (code > 0) {
    Serial.print("[SERVER] Connected to command endpoint, HTTP code: ");
    Serial.println(code);
  } else {
    Serial.print("[SERVER] Failed to reach command endpoint, error: ");
    Serial.println(code);
  }
  checkClient.end();
}

void forceStopAllOutputs() {
  digitalWrite(RELAY1_CONVEYOR, LOW);
  digitalWrite(RELAY2_BRANCH_SMALL, LOW);
  digitalWrite(RELAY3_BRANCH_MEDIUM, LOW);
  digitalWrite(RELAY4_BRANCH_NON_ORANGE, LOW);
  servoSmall.write(90);
  servoMedium.write(90);
  servoNonOrange.write(90);
  servoSmallAngle = 90;
  servoMediumAngle = 90;
  servoNonOrangeAngle = 90;
  isServoActive = false;
}

// --- SETUP & LOOP ---

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\n[BOOT] ESP32-C3 starting...");
  Serial.print("[WIFI] Connecting to SSID: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }

  Serial.println();
  Serial.println("[WIFI] Connected.");
  Serial.print("[WIFI] IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("[SERVER] Detect URL: ");
  Serial.println(flask_url);
  Serial.print("[SERVER] Hardware URL: ");
  Serial.println(flask_hardware_url);
  Serial.print("[SERVER] Command URL: ");
  Serial.println(flask_command_url);
  checkServerConnection();
  
  pinMode(RELAY1_CONVEYOR, OUTPUT);
  pinMode(RELAY2_BRANCH_SMALL, OUTPUT);
  pinMode(RELAY3_BRANCH_MEDIUM, OUTPUT);
  pinMode(RELAY4_BRANCH_NON_ORANGE, OUTPUT);
  pinMode(SENSOR1_MAIN, INPUT_PULLUP);
  pinMode(SENSOR2_SMALL, INPUT_PULLUP);
  pinMode(SENSOR3_MEDIUM, INPUT_PULLUP);
  pinMode(SENSOR4_NON_ORANGE, INPUT_PULLUP);
  
  servoSmall.attach(SERVO1_PIN);
  servoMedium.attach(SERVO2_PIN);
  servoNonOrange.attach(SERVO3_NON_ORANGE_PIN);
  
  // Khởi tạo trạng thái ban đầu
  forceStopAllOutputs();
}

void loop() {
  // 1. CHỈ POLL LỆNH TỪ SERVER MỖI 1 GIÂY
  if (millis() - commandPollTimer >= POLLING_INTERVAL) {
    commandPollTimer = millis();
    String desiredState = getDesiredState(&stateClient);

    if (desiredState == "RUNNING" && currentState == IDLE) {
      currentState = RUNNING;
      postHardwareStatus(&hwStatusClient); // Báo server ngay khi chuyển trạng thái
    } 
    else if (desiredState == "IDLE" && currentState != IDLE) {
      forceStopAllOutputs();
      currentState = IDLE;
      postHardwareStatus(&hwStatusClient);
    }
  }

  // 2. CHỈ GỬI HEARTBEAT TRẠNG THÁI MỖI 1 GIÂY
  if (millis() - lastStatusPostTime >= POLLING_INTERVAL) {
    lastStatusPostTime = millis();
    postHardwareStatus(&hwStatusClient);
  }

  // 3. LOGIC MÁY TRẠNG THÁI (STATE MACHINE)
  switch(currentState) {
    case IDLE:
      // Các output đã được ngắt ở hàm forceStopAllOutputs
      break;

    case RUNNING: {
      digitalWrite(RELAY1_CONVEYOR, HIGH); // Chạy băng tải
      if(!hasBeenDetected)
      {
        sensorMainTriggered = isTriggered(SENSOR1_MAIN);
      }
      // Edge detection: chỉ trigger khi sensor chuyển từ WAITING -> TRIGGERED
      if (sensorMainTriggered && !lastSensorMainTriggered) { 
        digitalWrite(RELAY1_CONVEYOR, LOW); // Dừng băng tải
        detectDelayStartMs = millis();
        currentState = DETECTING;
        postHardwareStatus(&hwStatusClient);
      }
      
      lastSensorMainTriggered = sensorMainTriggered;
      break;
    }

    case DETECTING:
      {
        // Chờ ngắn sau khi relay băng tải tắt để vật thể ổn định trước khi chụp.
        if (millis() - detectDelayStartMs < DETECT_AFTER_RELAY_OFF_MS) {
          break;
        }

        hasBeenDetected = true;
        String response = callFlaskAPI(&detectClient);
        String category = parseCategoryFromResponse(response);
        
        if (category == "SMALL") {
          currentState = SORT_SMALL;
          sensorSmallArmed = false;
          lastSensorSmallTriggered = isTriggered(SENSOR2_SMALL);
        }
        else if (category == "MEDIUM") {
          currentState = SORT_MEDIUM;
          sensorMediumArmed = false;
          lastSensorMediumTriggered = isTriggered(SENSOR3_MEDIUM);
        }
        else if (category == "NON_ORANGE") {
          currentState = SORT_NON_ORANGE;
          sensorNonOrangeArmed = false;
          lastSensorNonOrangeTriggered = isTriggered(SENSOR4_NON_ORANGE);
        }
        else if (category == "LARGE") {
          currentState = SORT_LARGE;
          large_timer = millis();
        }
        else {
          // Không detect được gì (sau 4 lần thử), xử lý như NON_ORANGE.
          currentState = SORT_NON_ORANGE;
          sensorNonOrangeArmed = false;
          lastSensorNonOrangeTriggered = isTriggered(SENSOR4_NON_ORANGE);
        }
        
        postHardwareStatus(&hwStatusClient);
      }
      break;

    case SORT_SMALL: {
      digitalWrite(RELAY1_CONVEYOR, HIGH);
      digitalWrite(RELAY2_BRANCH_SMALL, HIGH);
      delay(30);
      bool sensorSmallTriggered = isTriggered(SENSOR2_SMALL);

      // Chi arm khi sensor da tro ve WAITING.
      if (!sensorSmallArmed && !sensorSmallTriggered) {
        sensorSmallArmed = true;
      }
      
      // Servo small chi bat dau khi sensor small vua phat hien vat the.
      if (sensorSmallArmed && sensorSmallTriggered && !lastSensorSmallTriggered && !isServoActive) {
        servoSmall.write(30);
        servoSmallAngle = 30;
        servoTimer = millis();
        isServoActive = true;
        postHardwareStatus(&hwStatusClient);
      }

      lastSensorSmallTriggered = sensorSmallTriggered;
      
      // Giữ servo 10s
      if (isServoActive && (millis() - servoTimer >= 4000)) {
         servoSmall.write(90); // Reset
         servoSmallAngle = 90;
         digitalWrite(RELAY2_BRANCH_SMALL, LOW);
         isServoActive = false;
         sensorSmallArmed = false;
         hasBeenDetected = false;
         lastSensorMainTriggered = false;  // Reset edge detection để tránh false trigger
         currentState = RUNNING;
         postHardwareStatus(&hwStatusClient);
      }
      break;
    }

    case SORT_MEDIUM: {
      digitalWrite(RELAY1_CONVEYOR, HIGH);
      digitalWrite(RELAY3_BRANCH_MEDIUM, HIGH);
      delay(30);
      bool sensorMediumTriggered = isTriggered(SENSOR3_MEDIUM);

      // Chi arm khi sensor da tro ve WAITING.
      if (!sensorMediumArmed && !sensorMediumTriggered) {
        sensorMediumArmed = true;
      }
      
      // Servo medium chi bat dau khi sensor medium vua phat hien vat the.
      if (sensorMediumArmed && sensorMediumTriggered && !lastSensorMediumTriggered && !isServoActive) {
         servoMedium.write(35);
         servoMediumAngle = 35;
         servoTimer = millis();
         isServoActive = true;
         postHardwareStatus(&hwStatusClient);
      }

      lastSensorMediumTriggered = sensorMediumTriggered;
      
      // Giữ servo 10s
      if (isServoActive && (millis() - servoTimer >= 4000)) {
         servoMedium.write(90); // Reset
         servoMediumAngle = 90;
         digitalWrite(RELAY3_BRANCH_MEDIUM, LOW);
         isServoActive = false;
         sensorMediumArmed = false;
         hasBeenDetected = false;
         lastSensorMainTriggered = false;  // Reset edge detection để tránh false trigger
         currentState = RUNNING;
         postHardwareStatus(&hwStatusClient);
      }
      break;
      }

     case SORT_NON_ORANGE: {
      digitalWrite(RELAY1_CONVEYOR, HIGH);
      digitalWrite(RELAY4_BRANCH_NON_ORANGE, HIGH);
      delay(30);
      bool sensorNonOrangeTriggered = isTriggered(SENSOR4_NON_ORANGE);

      // Chi arm khi sensor da tro ve WAITING.
      if (!sensorNonOrangeArmed && !sensorNonOrangeTriggered) {
        sensorNonOrangeArmed = true;
      }

      // Servo non orange chi bat dau khi sensor non orange vua phat hien vat the.
      if (sensorNonOrangeArmed && sensorNonOrangeTriggered && !lastSensorNonOrangeTriggered && !isServoActive) {
        servoNonOrange.write(30);
        servoNonOrangeAngle = 30;
        servoTimer = millis();
        isServoActive = true;
        postHardwareStatus(&hwStatusClient);
      }

      lastSensorNonOrangeTriggered = sensorNonOrangeTriggered;

      if (isServoActive && (millis() - servoTimer >= 4000)) {
        servoNonOrange.write(90);
        servoNonOrangeAngle = 90;
        digitalWrite(RELAY4_BRANCH_NON_ORANGE, LOW);
        isServoActive = false;
        sensorNonOrangeArmed = false;
        hasBeenDetected = false;
        lastSensorMainTriggered = false;  // Reset edge detection để tránh false trigger
        currentState = RUNNING;
        postHardwareStatus(&hwStatusClient);
      }
      break;
      }

    case SORT_LARGE:
      digitalWrite(RELAY1_CONVEYOR, HIGH);
      if(millis() - large_timer >= 5000)
      {
        currentState = RUNNING;
        hasBeenDetected = false;
        postHardwareStatus(&hwStatusClient);
      }
      break;

    default:
      digitalWrite(RELAY1_CONVEYOR, HIGH);
      if(millis() - none_timer >= 6000)
      {
        currentState = RUNNING;
        hasBeenDetected = false;
        postHardwareStatus(&hwStatusClient);
      }
      break;
  }

  // Cap nhat trang thai canh trong cac state khac de edge detection hoat dong on dinh.
  if (currentState != SORT_SMALL) {
    lastSensorSmallTriggered = isTriggered(SENSOR2_SMALL);
    sensorSmallArmed = false;
  }
  if (currentState != SORT_MEDIUM) {
    lastSensorMediumTriggered = isTriggered(SENSOR3_MEDIUM);
    sensorMediumArmed = false;
  }
  if (currentState != SORT_NON_ORANGE) {
    lastSensorNonOrangeTriggered = isTriggered(SENSOR4_NON_ORANGE);
    sensorNonOrangeArmed = false;
  }
}
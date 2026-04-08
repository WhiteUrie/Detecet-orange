#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

const char* ssid = "TipToe";
const char* password = "1234567890dcq";
String flask_url = "http://10.160.186.160:5000/api/detect";
String flask_hardware_url = "http://10.160.186.160:5000/api/hardware/update";
String flask_command_url = "http://10.160.186.160:5000/api/command";

HTTPClient hwStatusClient;
HTTPClient stateClient;
HTTPClient detectClient;

// Khai báo chân GPIO
const int RELAY1_CONVEYOR = 2;
const int RELAY2_BRANCH_SMALL = 3;
const int RELAY3_BRANCH_MEDIUM = 4;
const int SENSOR1_MAIN = 5;
const int SENSOR2_SMALL = 6;
const int SENSOR3_MEDIUM = 7;
const int SERVO1_PIN = 8;
const int SERVO2_PIN = 10;

Servo servoSmall;
Servo servoMedium;

enum State { IDLE, RUNNING, DETECTING, SORT_SMALL, SORT_MEDIUM, SORT_LARGE, NONE };
State currentState = IDLE;

// Biến cho non-blocking delay
unsigned long servoTimer = 0;
bool isServoActive = false;
bool hasBeenDetected = false;
bool sensorMainTriggered = false;
unsigned long large_timer = 0;
unsigned long none_timer = 0;

// Theo doi canh trigger de tranh kich servo lap lai trong cung 1 lan vat di qua sensor.
bool lastSensorSmallTriggered = false;
bool lastSensorMediumTriggered = false;
bool lastSensorMainTriggered = false;  // Thêm edge detection cho SENSOR1_MAIN
bool sensorSmallArmed = false;
bool sensorMediumArmed = false;

// Biến điều hướng HTTP Request
unsigned long commandPollTimer = 0;
unsigned long lastStatusPostTime = 0;
const unsigned long POLLING_INTERVAL = 1000; // Gọi API 1 giây/lần

int servoSmallAngle = 0;
int servoMediumAngle = 0;

// --- CÁC HÀM TIỆN ÍCH ---

String extractDesiredState(String payload) {
  // Bỏ qua payload rỗng
  if (payload.length() == 0 || payload == "{}") return "IDLE";
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("[CMD] JSON Parse Error: ");
    Serial.println(error.c_str());
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
    case SORT_LARGE: return "SORT_LARGE";
    default: return "IDLE";
  }
}

String relayStatus(int pin) {
  return digitalRead(pin) == HIGH ? "ON" : "OFF";
}

String sensorStatus(int pin) {
  // Flying Fish sensor thường active LOW.
  return digitalRead(pin) == LOW ? "TRIGGERED" : "WAITING";
}

String servoStatus(int angle) {
  return angle != 90 ? "ACTIVE" : "IDLE";
}

bool isTriggered(int pin) {
  // Flying Fish sensor active LOW.
  return digitalRead(pin) == LOW;
}

String parseCategoryFromResponse(String response) {
  // Bỏ qua response rỗng
  if (response.length() == 0 || response == "{}") return "NONE";
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    Serial.print("[DETECT] JSON Parse Error: ");
    Serial.println(error.c_str());
    return "NONE";
  }
  
  // Lấy giá trị của key "category", nếu không có thì mặc định là "NONE"
  return doc["category"] | "NONE";
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
  
  JsonObject servo = doc.createNestedObject("servo");
  servo["servo_small"] = servoStatus(servoSmallAngle);
  servo["servo_medium"] = servoStatus(servoMediumAngle);
  
  JsonObject sensor = doc.createNestedObject("sensor");
  sensor["sensor_main"] = sensorStatus(SENSOR1_MAIN);
  sensor["sensor_small"] = sensorStatus(SENSOR2_SMALL);
  sensor["sensor_medium"] = sensorStatus(SENSOR3_MEDIUM);

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
    Serial.print("[CMD] GET /api/command failed, code=");
    Serial.println(code);
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

void forceStopAllOutputs() {
  digitalWrite(RELAY1_CONVEYOR, LOW);
  digitalWrite(RELAY2_BRANCH_SMALL, LOW);
  digitalWrite(RELAY3_BRANCH_MEDIUM, LOW);
  servoSmall.write(90);
  servoMedium.write(90);
  servoSmallAngle = 90;
  servoMediumAngle = 90;
  isServoActive = false;
}

// --- SETUP & LOOP ---

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Connecting to ");
  Serial.println(ssid);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[WiFi] Connected, IP: ");
  Serial.println(WiFi.localIP());
  
  pinMode(RELAY1_CONVEYOR, OUTPUT);
  pinMode(RELAY2_BRANCH_SMALL, OUTPUT);
  pinMode(RELAY3_BRANCH_MEDIUM, OUTPUT);
  pinMode(SENSOR1_MAIN, INPUT_PULLUP);
  pinMode(SENSOR2_SMALL, INPUT_PULLUP);
  pinMode(SENSOR3_MEDIUM, INPUT_PULLUP);
  
  servoSmall.attach(SERVO1_PIN);
  servoMedium.attach(SERVO2_PIN);
  
  // Khởi tạo trạng thái ban đầu
  forceStopAllOutputs();
}

void loop() {
  // 1. CHỈ POLL LỆNH TỪ SERVER MỖI 1 GIÂY
  if (millis() - commandPollTimer >= POLLING_INTERVAL) {
    commandPollTimer = millis();
    String desiredState = getDesiredState(&stateClient);

    if (desiredState == "RUNNING" && currentState == IDLE) {
      Serial.println("[STATE] Transition IDLE -> RUNNING");
      currentState = RUNNING;
      postHardwareStatus(&hwStatusClient); // Báo server ngay khi chuyển trạng thái
    } 
    else if (desiredState == "IDLE" && currentState != IDLE) {
      Serial.println("[STATE] Command IDLE -> force stop & go IDLE");
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
        Serial.println("[SENSOR] MAIN TRIGGERED -> DETECTING");
        digitalWrite(RELAY1_CONVEYOR, LOW); // Dừng băng tải
        currentState = DETECTING;
        postHardwareStatus(&hwStatusClient);
      }
      
      lastSensorMainTriggered = sensorMainTriggered;
      break;
    }

    case DETECTING:
      {
        hasBeenDetected = true;
        String response = callFlaskAPI(&detectClient);
        Serial.print("[DETECT] Response: ");
        Serial.println(response);
        String category = parseCategoryFromResponse(response);
        Serial.print("[DETECT] Category: ");
        Serial.println(category);
        
        if (category == "SMALL") {
          currentState = SORT_SMALL;
          sensorSmallArmed = false;
          lastSensorSmallTriggered = isTriggered(SENSOR2_SMALL);
          Serial.println("[STATE] DETECTING -> SORT_SMALL");
        }
        else if (category == "MEDIUM") {
          currentState = SORT_MEDIUM;
          sensorMediumArmed = false;
          lastSensorMediumTriggered = isTriggered(SENSOR3_MEDIUM);
          Serial.println("[STATE] DETECTING -> SORT_MEDIUM");
        }
        else if (category == "LARGE") {
          currentState = SORT_LARGE;
          large_timer = millis();
          Serial.println("[STATE] DETECTING -> SORT_LARGE");
        }
        else {
          // Nếu response không hợp lệ, quay lại RUNNING sau 1s
          Serial.println("[DETECT] Invalid response - waiting...");
          currentState = NONE;
          none_timer = millis();
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
      if (isServoActive && (millis() - servoTimer >= 7000)) {
         servoSmall.write(90); // Reset
         servoSmallAngle = 90;
         digitalWrite(RELAY2_BRANCH_SMALL, LOW);
         isServoActive = false;
         sensorSmallArmed = false;
         hasBeenDetected = false;
         currentState = RUNNING;
         postHardwareStatus(&hwStatusClient);
         Serial.println("hasbeendetected: false");
         Serial.println("DONE");
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
         servoMedium.write(30);
         servoMediumAngle = 30;
         servoTimer = millis();
         isServoActive = true;
         postHardwareStatus(&hwStatusClient);
      }

      lastSensorMediumTriggered = sensorMediumTriggered;
      
      // Giữ servo 10s
      if (isServoActive && (millis() - servoTimer >= 7000)) {
         servoMedium.write(90); // Reset
         servoMediumAngle = 90;
         digitalWrite(RELAY3_BRANCH_MEDIUM, LOW);
         isServoActive = false;
         sensorMediumArmed = false;
         hasBeenDetected = false;
         currentState = RUNNING;
         postHardwareStatus(&hwStatusClient);
         Serial.println("hasbeendetected: false");
         Serial.println("DONE");
      }
      break;
      }

    case SORT_LARGE:
      digitalWrite(RELAY1_CONVEYOR, HIGH);
      if(millis() - large_timer >= 2000)
      {
        currentState = RUNNING;
        hasBeenDetected = false;
        Serial.println("hasbeendetected: false");
        Serial.println("DONE");
      }
      postHardwareStatus(&hwStatusClient);
      break;

    default:
      digitalWrite(RELAY1_CONVEYOR, HIGH);
      if(millis() - none_timer >= 2000)
      {
        currentState = RUNNING;
        hasBeenDetected = false;
        Serial.println("hasbeendetected: false");
        Serial.println("DONE");
      }
      postHardwareStatus(&hwStatusClient);
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
}
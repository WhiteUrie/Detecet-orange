#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>

const char* ssid = "TipToe";
const char* password = "1234567890dcq";
String flask_url = "http://10.105.143.160:5000/api/detect";
String flask_hardware_url = "http://10.105.143.160:5000/api/hardware/update";
String flask_command_url = "http://10.105.143.160:5000/api/command";

// Khai báo chân GPIO (Ví dụ)
const int RELAY1_CONVEYOR = 2;
const int RELAY2_BRANCH_SMALL = 3;
const int RELAY3_BRANCH_MEDIUM = 4;
const int SENSOR1_MAIN = 5;
const int SENSOR2_SMALL = 6;
const int SENSOR3_MEDIUM = 7;
const int SERVO1_PIN = 8;
const int SERVO2_PIN = 9;

Servo servoSmall;
Servo servoMedium;

enum State { IDLE, RUNNING, DETECTING, SORT_SMALL, SORT_MEDIUM, SORT_LARGE };
State currentState = IDLE;

// Biến cho non-blocking delay
unsigned long servoTimer = 0;
bool isServoActive = false;
unsigned long telemetryTimer = 0;
unsigned long commandPollTimer = 0;

int servoSmallAngle = 0;
int servoMediumAngle = 0;

String extractDesiredState(String payload) {
  int idx = payload.indexOf("\"desired_state\":\"");
  if (idx < 0) return "IDLE";
  idx += String("\"desired_state\":\"").length();
  int end = payload.indexOf('"', idx);
  if (end < 0) return "IDLE";
  return payload.substring(idx, end);
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
  return angle != 0 ? "ACTIVE" : "IDLE";
}

void postHardwareStatus() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(flask_hardware_url);
  http.addHeader("Content-Type", "application/json");

  String body = "{";
  body += "\"state\":\"" + stateToString(currentState) + "\",";
  body += "\"relay\":{";
  body += "\"relay1_conveyor\":\"" + relayStatus(RELAY1_CONVEYOR) + "\",";
  body += "\"relay2_small\":\"" + relayStatus(RELAY2_BRANCH_SMALL) + "\",";
  body += "\"relay3_medium\":\"" + relayStatus(RELAY3_BRANCH_MEDIUM) + "\"},";
  body += "\"servo\":{";
  body += "\"servo_small\":\"" + servoStatus(servoSmallAngle) + "\",";
  body += "\"servo_medium\":\"" + servoStatus(servoMediumAngle) + "\"},";
  body += "\"sensor\":{";
  body += "\"sensor_main\":\"" + sensorStatus(SENSOR1_MAIN) + "\",";
  body += "\"sensor_small\":\"" + sensorStatus(SENSOR2_SMALL) + "\",";
  body += "\"sensor_medium\":\"" + sensorStatus(SENSOR3_MEDIUM) + "\"}"
          "}";

  http.POST(body);
  http.end();
}

String getDesiredState() {
  if (WiFi.status() != WL_CONNECTED) return "IDLE";

  HTTPClient http;
  http.begin(flask_command_url);
  int code = http.GET();
  if (code <= 0) {
    Serial.print("[CMD] GET /api/command failed, code=");
    Serial.println(code);
    http.end();
    return "IDLE";
  }

  String payload = http.getString();
  http.end();
  Serial.print("[CMD] /api/command payload: ");
  Serial.println(payload);

  String ds = extractDesiredState(payload);
  if (ds == "RUNNING") return "RUNNING";
  return "IDLE";
}

void forceStopAllOutputs() {
  digitalWrite(RELAY1_CONVEYOR, LOW);
  digitalWrite(RELAY2_BRANCH_SMALL, LOW);
  digitalWrite(RELAY3_BRANCH_MEDIUM, LOW);
  servoSmall.write(0);
  servoMedium.write(0);
  servoSmallAngle = 0;
  servoMediumAngle = 0;
  isServoActive = false;
}

String parseCategoryFromResponse(String response) {
  if (response.indexOf("\"category\":\"SMALL\"") >= 0) return "SMALL";
  if (response.indexOf("\"category\":\"MEDIUM\"") >= 0) return "MEDIUM";
  if (response.indexOf("\"category\":\"LARGE\"") >= 0) return "LARGE";
  return "NONE";
}

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
  
  // Trạng thái ban đầu
  digitalWrite(RELAY1_CONVEYOR, LOW);
  digitalWrite(RELAY2_BRANCH_SMALL, LOW);
  digitalWrite(RELAY3_BRANCH_MEDIUM, LOW);
  servoSmall.write(0);
  servoMedium.write(0);
  servoSmallAngle = 0;
  servoMediumAngle = 0;
}

String callFlaskAPI() {
  HTTPClient http;
  http.begin(flask_url);
  int httpResponseCode = http.GET();
  String payload = "{}";
  if (httpResponseCode > 0) {
    payload = http.getString();
  }
  http.end();
  return payload; // Nên dùng thư viện ArduinoJson để parse response này
}

void loop() {
  if (millis() - commandPollTimer >= 300) {
    commandPollTimer = millis();
    String desiredState = getDesiredState();

    if (desiredState == "RUNNING" && currentState == IDLE) {
      Serial.println("[STATE] Transition IDLE -> RUNNING (command RUNNING)");
      currentState = RUNNING;
    } else if (desiredState == "IDLE" && currentState != IDLE) {
      Serial.println("[STATE] Command IDLE -> force stop & go IDLE");
      forceStopAllOutputs();
      currentState = IDLE;
    }
  }

  if (millis() - telemetryTimer >= 400) {
    telemetryTimer = millis();
    postHardwareStatus();
  }

  switch(currentState) {
    case IDLE:
      forceStopAllOutputs();
      break;

    case RUNNING:
      digitalWrite(RELAY1_CONVEYOR, HIGH); // Chạy băng tải
      if (digitalRead(SENSOR1_MAIN) == LOW) { // Phát hiện vật
        Serial.println("[SENSOR] MAIN TRIGGERED -> DETECTING");
        digitalWrite(RELAY1_CONVEYOR, LOW); // Dừng băng tải
        currentState = DETECTING;
      }
      break;

    case DETECTING:
      {
        String response = callFlaskAPI();
        Serial.print("[DETECT] Response: ");
        Serial.println(response);
        String category = parseCategoryFromResponse(response);
        
        if (category == "SMALL") currentState = SORT_SMALL;
        else if (category == "MEDIUM") currentState = SORT_MEDIUM;
        else if (category == "LARGE") currentState = SORT_LARGE;
        else currentState = RUNNING;
      }
      break;

    case SORT_SMALL:
      digitalWrite(RELAY1_CONVEYOR, HIGH);
      digitalWrite(RELAY2_BRANCH_SMALL, HIGH);
      
      // Chờ sensor 2 phát hiện cam đã vào nhánh
      if (digitalRead(SENSOR2_SMALL) == LOW && !isServoActive) {
        servoSmall.write(45);
        servoSmallAngle = 45;
         servoTimer = millis();
         isServoActive = true;
      }
      
      // Giữ servo 10s
      if (isServoActive && (millis() - servoTimer >= 10000)) {
         servoSmall.write(0); // Reset
        servoSmallAngle = 0;
         digitalWrite(RELAY2_BRANCH_SMALL, LOW);
         isServoActive = false;
         currentState = RUNNING;
      }
      break;

    case SORT_MEDIUM:
      digitalWrite(RELAY1_CONVEYOR, HIGH);
      digitalWrite(RELAY3_BRANCH_MEDIUM, HIGH);

      // Chờ sensor 3 phát hiện cam đã vào nhánh medium
      if (digitalRead(SENSOR3_MEDIUM) == LOW && !isServoActive) {
        servoMedium.write(45);
        servoMediumAngle = 45;
         servoTimer = millis();
         isServoActive = true;
      }

      // Giữ servo 10s
      if (isServoActive && (millis() - servoTimer >= 10000)) {
         servoMedium.write(0); // Reset
        servoMediumAngle = 0;
         digitalWrite(RELAY3_BRANCH_MEDIUM, LOW);
         isServoActive = false;
         currentState = RUNNING;
      }
      break;

    case SORT_LARGE:
      // Cam lớn đi thẳng băng chính, không bật relay phân nhánh
      digitalWrite(RELAY1_CONVEYOR, HIGH);
      digitalWrite(RELAY2_BRANCH_SMALL, LOW);
      digitalWrite(RELAY3_BRANCH_MEDIUM, LOW);
      currentState = RUNNING;
      break;
  }
}
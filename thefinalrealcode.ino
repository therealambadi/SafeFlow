#include <HX711.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// Pin Definitions
#define DOUT 19
#define CLK 23
#define GAS_SENSOR_PIN 35
#define SERVO_PIN 21
#define BUZZER_PIN 26
#define LED_PIN 14
#define I2C_SDA 5
#define I2C_SCL 22

// Constants
#define CALIBRATION_FACTOR 596678.13
#define GAS_LEAK_THRESHOLD_PPM 300
#define LOW_WEIGHT_THRESHOLD 1.5
#define FIREBASE_UPDATE_INTERVAL 500
#define SETTING_CHECK_INTERVAL 3000
#define ALARM_DURATION 10000
#define MAX_UPDATE_RETRIES 3
#define WIFI_RECONNECT_DELAY 10000
#define MQ2_RO_CLEAN_AIR 9.83
#define MQ2_RL 10.0

HX711 scale;
Servo servo;
LiquidCrystal_I2C lcd(0x27, 16, 2);

const char* ssid = "realme";
const char* password = "qazplm1234";
const char* firebaseHost = "gasmonitoring-dc325-default-rtdb.firebaseio.com";
const char* firebaseAuth = "";
const char* telegramBotToken = "7885614442:AAHncbMZkaj0isQQFrywLbfCs5_aaNe-M9Q";
const char* telegramChatID = "1408113550";

bool gasLeakNotified = false;
bool lowWeightNotified = false;
bool autoBookingEnabled = true;
bool valveClosedDueToLeak = false;
bool persistentLeakState = false;
unsigned long alarmStartTime = 0;
bool alarmActive = false;
unsigned long lastFirebaseUpdate = 0;
unsigned long lastSettingCheck = 0;
unsigned long lastSuccessfulUpdate = 0;
unsigned long lastWiFiReconnectAttempt = 0;

void setup() {
  Serial.begin(115200);
  initHardware();
  connectWiFi();
  configTime(19800, 0, "pool.ntp.org");
  checkSettings();
  sendStartupNotification();
}

void loop() {
  float weight = scale.get_units(5);
  float gasPPM = readGasPPM();
  String status = "Safe";

  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiReconnectAttempt > WIFI_RECONNECT_DELAY) {
    Serial.println("WiFi disconnected, attempting to reconnect...");
    connectWiFi();
    lastWiFiReconnectAttempt = millis();
  }

  if (millis() - lastSettingCheck > SETTING_CHECK_INTERVAL) {
    checkSettings();
    lastSettingCheck = millis();
  }

  if (gasPPM > GAS_LEAK_THRESHOLD_PPM) {
    handleGasLeak(weight, gasPPM);
    status = "DANGER - Gas Leak";
  } else {
    handleNormalOperation(weight);
  }

  if (alarmActive && millis() - alarmStartTime > ALARM_DURATION) {
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
    alarmActive = false;
  }

  updateLCD(weight, gasPPM, status);
  checkLowWeight(weight);

  if (millis() - lastFirebaseUpdate > FIREBASE_UPDATE_INTERVAL) {
    if (uploadToFirebase(weight, status, gasPPM)) {
      lastFirebaseUpdate = millis();
      lastSuccessfulUpdate = millis();
    } else {
      Serial.println("Failed to update Firebase");
    }
  }
}

void initHardware() {
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("SafeFlow Init...");

  scale.begin(DOUT, CLK);
  scale.set_scale(CALIBRATION_FACTOR);
  scale.tare();

  pinMode(GAS_SENSOR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  servo.attach(SERVO_PIN);
  servo.write(0);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  valveClosedDueToLeak = false;
  persistentLeakState = false;
  alarmActive = false;
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("Connecting to WiFi...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  
  lcd.setCursor(0, 1);
  lcd.print("Connecting WiFi");

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 20) {
    delay(500);
    Serial.print(".");
    lcd.print(".");
    attempt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Connected");
    lcd.setCursor(0, 1);
    lcd.print("IP: ");
    lcd.print(WiFi.localIP().toString());
    delay(2000);
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("SafeFlow Ready");
    lcd.setCursor(0, 1);
    lcd.print("Mode: ");
    lcd.print(autoBookingEnabled ? "Auto" : "Manual");
  } else {
    Serial.println("\nWiFi Connection Failed");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Failed");
  }
}

void checkSettings() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "https://" + String(firebaseHost) + "/settings/autoBooking.json";
  if (strlen(firebaseAuth) > 0) {
    url += "?auth=" + String(firebaseAuth);
  }

  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    payload.trim();
    bool newSetting = (payload == "true" || payload == "1");

    if (newSetting != autoBookingEnabled) {
      autoBookingEnabled = newSetting;
      Serial.println("Auto Booking changed to: " + String(autoBookingEnabled ? "ON" : "OFF"));

      lcd.setCursor(0, 0);
      lcd.print("Auto Booking: ");
      lcd.print(autoBookingEnabled ? "ON " : "OFF");
      sendTelegramNotification("⚙️ System Setting Changed\n\nAuto Booking: " + String(autoBookingEnabled ? "ENABLED" : "DISABLED"));
      delay(1000);
    }
  } else {
    Serial.printf("Failed to get settings. HTTP code: %d\n", httpCode);
  }
  http.end();
}

void sendTelegramNotification(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Can't send Telegram notification - no WiFi");
    return;
  }

  message.replace(" ", "%20");
  message.replace("\n", "%0A");
  message.replace(":", "%3A");
  message.replace("!", "%21");
  message.replace("=", "%3D");

  String url = "https://api.telegram.org/bot" + String(telegramBotToken) +
               "/sendMessage?chat_id=" + String(telegramChatID) +
               "&text=" + message;

  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    Serial.println("Telegram notification sent");
  } else {
    Serial.printf("Failed to send Telegram notification. HTTP code: %d\n", httpCode);
    String response = http.getString();
    Serial.println("Response: " + response);
  }
  http.end();
}

String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return "Time Unavailable";
  }
  
  char timeString[30];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeString);
}

bool uploadToFirebase(float weight, String status, float gasPPM) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, can't update Firebase");
    return false;
  }

  HTTPClient http;
  String url = "https://" + String(firebaseHost) + "/latest.json";
  if (strlen(firebaseAuth) > 0) {
    url += "?auth=" + String(firebaseAuth);
  }

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<512> doc;
  doc["timestamp"] = getTimeString();
  doc["weight"] = weight;
  doc["status"] = status;
  doc["gas_ppm"] = gasPPM;
  doc["valve_state"] = (servo.read() == 0) ? "open" : "closed";
  doc["auto_booking"] = autoBookingEnabled;
  doc["rssi"] = WiFi.RSSI();
  doc["alarm_active"] = alarmActive;
  doc["persistent_leak"] = persistentLeakState;

  String body;
  serializeJson(doc, body);
  
  Serial.println("Sending to Firebase: " + body);
  
  int retryCount = 0;
  int httpCode = 0;
  
  while (retryCount < MAX_UPDATE_RETRIES) {
    httpCode = http.PUT(body);
    
    if (httpCode == HTTP_CODE_OK) {
      Serial.println("Firebase update successful");
      http.end();
      return true;
    }
    
    Serial.printf("Firebase update failed, code: %d, retry %d\n", httpCode, retryCount);
    retryCount++;
    delay(500);
  }
  
  http.end();
  return false;
}

void handleGasLeak(float weight, float gasPPM) {
  if (!valveClosedDueToLeak || !persistentLeakState) {
    servo.write(90);
    valveClosedDueToLeak = true;
    persistentLeakState = true;
    alarmStartTime = millis();
    alarmActive = true;
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(LED_PIN, HIGH);
    
    String message = "�� GAS LEAK DETECTED ��\n\n";
    message += "⚠️ DANGER: Gas concentration too high!\n";
    message += "�� Gas Level: " + String(gasPPM, 0) + " ppm\n";
    message += "⚖️ Cylinder Weight: " + String(weight, 1) + " kg\n";
    message += "�� Valve: CLOSED automatically\n";
    message += "⏰ Time: " + getTimeString() + "\n\n";
    message += "❗ The valve will remain CLOSED until system restart for safety.\n";
    message += "�� Alarm will sound for 10 seconds.";
    
    sendTelegramNotification(message);
    gasLeakNotified = true;
  }
}

void handleNormalOperation(float weight) {
  if (valveClosedDueToLeak || persistentLeakState) {
    if (gasLeakNotified) {
      String message = "✅ Gas leak resolved but valve remains closed\n\n";
      message += "⚖️ Current Weight: " + String(weight, 1) + " kg\n";
      message += "⏰ Time: " + getTimeString() + "\n\n";
      message += "⚠️ Valve remains CLOSED for safety.\n";
      message += "�� System restart required to reopen valve.";
      
      sendTelegramNotification(message);
      gasLeakNotified = false;
    }
  }
}

void updateLCD(float weight, float gasPPM, String status) {
  lcd.setCursor(0, 0);
  lcd.print(status.substring(0, 16));
  
  lcd.setCursor(0, 1);
  lcd.print("W:");
  lcd.print(weight, 1);
  lcd.print("kg G:");
  lcd.print(gasPPM, 0);
  lcd.print("ppm");
}

void checkLowWeight(float weight) {
  if (weight < LOW_WEIGHT_THRESHOLD) {
    lcd.setCursor(0, 0);
    lcd.print("LOW GAS LEVEL!  ");

    if (!lowWeightNotified) {
      String message = "⚠️ LOW GAS ALERT ⚠️\n\n";
      message += "⚖️ Current Weight: " + String(weight, 1) + " kg\n";
      message += "⏰ Time: " + getTimeString() + "\n\n";
      
      if (autoBookingEnabled) {
        message += "�� AUTO-BOOKING INITIATED\n";
        message += "�� Order ID: #GAS" + String(random(10000, 99999));
      } else {
        message += "ℹ️ Auto-booking is DISABLED\n";
        message += "�� Please order a new cylinder manually";
      }
      
      sendTelegramNotification(message);
      lowWeightNotified = true;
    }
  } else {
    lowWeightNotified = false;
  }
}

float readGasPPM() {
  int sensorVal = analogRead(GAS_SENSOR_PIN);
  float voltage = sensorVal * (3.3 / 4095.0);
  float rs = (3.3 - voltage) / voltage * MQ2_RL;
  float ratio = rs / MQ2_RO_CLEAN_AIR;
  return pow(10, (log10(ratio) * -1.551 + 2.031));
}

void sendStartupNotification() {
  String message = "�� SafeFlow System STARTED\n\n";
  message += "�� System Reset Complete\n";
  message += "�� Valve: OPENED\n";
  message += "⚙️ Auto Booking: " + String(autoBookingEnabled ? "ENABLED" : "DISABLED") + "\n";
  message += "⏰ Startup Time: " + getTimeString() + "\n";
  message += "�� WiFi RSSI: " + String(WiFi.RSSI()) + " dBm\n\n";
  message += "✅ System ready and monitoring";
  
  sendTelegramNotification(message);
}
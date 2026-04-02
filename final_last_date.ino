#include <WiFi.h>
#include <HTTPClient.h>
#include "HX711.h"
#include <Wire.h>
#include "RTClib.h"
#include <HardwareSerial.h>
#include "time.h"  // For NTP sync

// ---------------- WiFi Configuration ----------------
const char* ssid = "den";
const char* password = "12345678";

// ---------------- Google Sheet Script ----------------
const char* scriptUrl = "https://script.google.com/macros/s/AKfycbxUd1jAarR1xMqotgyvtKiAauDZrDoQRwLAZ5o8iEheHLFElJVPuYvAClZzpiLmK-KR8w/exec";  // For POST logging
const char* sheetViewUrl = "https://docs.google.com/spreadsheets/d/1yiUnsRFdxwNl8n6uT9SC1C6jOjdBu_q1LQliYdjkp8Y/edit?usp=sharing";  //  For viewing (SMS link)

// ---------------- HX711 Pins ----------------
#define DOUT1 4
#define SCK1  5
#define DOUT2 34
#define SCK2  18
#define DOUT3 35
#define SCK3  19
#define LED_PIN 13
#define BUZZER_PIN 32   // Buzzer on ESP32 D32

HX711 scale1, scale2, scale3;

// ---------------- GSM Config ----------------
HardwareSerial SIM800(2);
#define SIM800_RX 16
#define SIM800_TX 17
#define SIM800_BAUD 9600
const char phoneNumber[] = "8421948921";

// ---------------- RTC Config ----------------
RTC_DS3231 rtc;

// ---------------- NTP Config ----------------
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800; // UTC +5:30 (IST)
const int daylightOffset_sec = 0;

// ---------------- Prescription Configuration ----------------
int totalDays = 7;     // <<--- prescription length
int completedDays = 0;

// ---------------- Medicine Scheduling ----------------
struct Slot {
  const char* name;
  int hour;
  int minute;
  const char* smsText;
  bool sent;
  bool taken;
  unsigned long sendTime;
};

Slot slots[3] = {
  {"Morning",   13, 55, "Good morning! Time to take your medicine.", false, false, 0},
  {"Afternoon", 13, 57, "Good afternoon! Please take your medicine.", false, false, 0},
  {"Night",     13, 59, "Good evening! Don't forget your medicine.",  false, false, 0}
};

// ---------------- Load Cell Variables ----------------
float calibration_factor = -7050.0;
float baseline1, baseline2, baseline3;
float w1, w2, w3;
float intake_threshold = 0.5;
float return_threshold = 0.2;
bool intakeDetected1 = false, intakeDetected2 = false, intakeDetected3 = false;

// ---------------- Tablet Counters ----------------
int morningLeft = 2;
int afternoonLeft = 2;
int eveningLeft = 2;
int totalTablets = morningLeft + afternoonLeft + eveningLeft;

// ---------------- System Control ----------------
bool slotLocked = false; 
int lastRecordedDay = -1; 
bool reportLinkSent = false; // ✅ Send link only once per prescription

// ---------------- Function Declarations ----------------
bool waitForResponse(const char *response, unsigned long timeout);
bool sendSMS(const char *number, const char *message);
void sendToGoogleSheet(const char* intakeType, const char* status);
void tareAll();
void detectIntake(const char *label, float delta, bool &flag);
void syncRTCWithNTP();
void buzzerAlert(bool on);
int getActiveSlot();
void checkForNewDay();

// =======================================================
// SETUP
// =======================================================
void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n✅ WiFi Connected!");

  // RTC + I2C setup
  Wire.begin(21, 22);
  if (!rtc.begin()) {
    Serial.println("❌ RTC not found! Check wiring.");
  } else {
    Serial.println("✅ RTC ready.");
  }

  // NTP Sync
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  syncRTCWithNTP();

  // HX711 Setup
  scale1.begin(DOUT1, SCK1);
  scale2.begin(DOUT2, SCK2);
  scale3.begin(DOUT3, SCK3);
  scale1.set_scale(calibration_factor);
  scale2.set_scale(calibration_factor);
  scale3.set_scale(calibration_factor);
  tareAll();

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // GSM Setup
  SIM800.begin(SIM800_BAUD, SERIAL_8N1, SIM800_RX, SIM800_TX);
  delay(1000);
  SIM800.println("AT");
  if (waitForResponse("OK", 2000)) {
    SIM800.println("ATE0");
    SIM800.println("AT+CMGF=1");
    Serial.println("✅ SIM800L initialized.");
  }

  Serial.println("✅ System ready.\n");
}

// =======================================================
// LOOP
// =======================================================
void loop() {
  DateTime now = rtc.now();
  Serial.printf("\nCurrent Time: %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());

  checkForNewDay(); 

  // Read load cells
  w1 = scale1.get_units(3);
  w2 = scale2.get_units(3);
  w3 = scale3.get_units(3);
  float d1 = w1 - baseline1;
  float d2 = w2 - baseline2;
  float d3 = w3 - baseline3;

  // Always monitor for wrong compartment attempts
  detectIntake("Night", d1, intakeDetected1);
  detectIntake("Morning", d2, intakeDetected2);
  detectIntake("Afternoon", d3, intakeDetected3);

  // Check scheduled times
  for (int i = 0; i < 3; i++) {
    if (completedDays >= totalDays) continue;

    if (!slots[i].sent && now.hour() == slots[i].hour && now.minute() == slots[i].minute) {
      Serial.printf("Sending %s reminder...\n", slots[i].name);
      bool smsStatus = sendSMS(phoneNumber, slots[i].smsText);
      if (smsStatus) Serial.println("✅ SMS Sent Successfully!");
      else Serial.println("❌ SMS Send Failed!");
      slots[i].sent = true;
      slots[i].sendTime = millis();
      slotLocked = false;
    }

    // Missed dose check
    if (slots[i].sent && !slots[i].taken && millis() - slots[i].sendTime > 300000) {
      Serial.printf("⚠ %s dose missed! Logging to sheet...\n", slots[i].name);
      sendToGoogleSheet(slots[i].name, "Missed");
      slots[i].sent = false;
      slots[i].taken = false;
      slotLocked = true;
    }
  }

  delay(1000);
}

// =======================================================
// RTC SYNC WITH NTP
// =======================================================
void syncRTCWithNTP() {
  struct tm timeinfo;
  Serial.println("⏳ Fetching time from NTP...");
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("⚠ Failed to get NTP time, using RTCs current time.");
    return;
  }

  DateTime ntpTime(
    timeinfo.tm_year + 1900,
    timeinfo.tm_mon + 1,
    timeinfo.tm_mday,
    timeinfo.tm_hour,
    timeinfo.tm_min,
    timeinfo.tm_sec
  );

  rtc.adjust(ntpTime);
  Serial.println("✅ RTC synchronized with NTP!");
  Serial.printf("Time: %02d:%02d:%02d | Date: %02d/%02d/%04d\n",
                timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
}

// =======================================================
// CHECK FOR NEW DAY
// =======================================================
void checkForNewDay() {
  DateTime now = rtc.now();
  int currentDay = now.day();

  if (lastRecordedDay == -1)
    lastRecordedDay = currentDay;

  if (currentDay != lastRecordedDay) {
    completedDays++;
    Serial.printf("🌅 New day detected — Day %d/%d\n", completedDays, totalDays);

    if (completedDays > totalDays) {
      Serial.println("✅ Prescription cycle completed! Stopping reminders.");
      for (int i = 0; i < 3; i++) {
        slots[i].sent = true;
        slots[i].taken = true;
      }
      sendToGoogleSheet("—", "Prescription Completed");
      return;
    }

    for (int i = 0; i < 3; i++) {
      slots[i].sent = false;
      slots[i].taken = false;
    }

    slotLocked = false;
    reportLinkSent = false; // ✅ Reset link for next day
    lastRecordedDay = currentDay;
    sendToGoogleSheet("—", "New Day Started");
  }
}

// =======================================================
// LOAD CELL DETECTION
// =======================================================
void detectIntake(const char *label, float delta, bool &flag) {
  if (!flag && delta > intake_threshold) {
    flag = true;
    int activeSlot = getActiveSlot();

    if (activeSlot == -1) {
      Serial.printf("⚠ %s slot not active! Wrong intake attempt.\n", label);
      buzzerAlert(true);
      return;
    }

    if (strcmp(label, slots[activeSlot].name) != 0) {
      Serial.printf("⚠ Wrong compartment opened: %s instead of %s!\n", label, slots[activeSlot].name);
      buzzerAlert(true);
      return;
    }

    buzzerAlert(false);
    Serial.printf("→ Correct intake successful for %s\n", label);
    digitalWrite(LED_PIN, HIGH);
    delay(300);
    digitalWrite(LED_PIN, LOW);

    if (strcmp(label, "Morning") == 0 && morningLeft > 0) morningLeft--;
    else if (strcmp(label, "Afternoon") == 0 && afternoonLeft > 0) afternoonLeft--;
    else if (strcmp(label, "Night") == 0 && eveningLeft > 0) eveningLeft--;

    totalTablets = morningLeft + afternoonLeft + eveningLeft;
    sendToGoogleSheet(label, "Taken");

    // ✅ Send Google Sheet view link after first successful intake
    if (!reportLinkSent) {
      Serial.println("📨 Sending Google Sheet view link...");
      String sheetMessage = "📋 Health Report Link:\n";
      sheetMessage += sheetViewUrl;
      bool linkSent = sendSMS(phoneNumber, sheetMessage.c_str());
      if (linkSent) {
        Serial.println("✅ Sheet link sent successfully!");
        reportLinkSent = true;
      } else {
        Serial.println("⚠ Failed to send sheet link.");
      }
    }

    Serial.printf("M:%d | A:%d | E:%d | Total:%d\n", morningLeft, afternoonLeft, eveningLeft, totalTablets);

    slots[activeSlot].taken = true;
    slotLocked = true;
    Serial.printf("✅ %s slot completed — locked until next reminder.\n", slots[activeSlot].name);
  }

  if (flag && fabs(delta) < return_threshold) {
    flag = false;
    buzzerAlert(false);
  }
}

// =======================================================
// SUPPORT FUNCTIONS
// =======================================================
int getActiveSlot() {
  for (int i = 0; i < 3; i++) {
    if (slots[i].sent && !slots[i].taken) return i;
  }
  return -1;
}

void buzzerAlert(bool on) {
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
}

void tareAll() {
  scale1.tare(); scale2.tare(); scale3.tare();
  baseline1 = scale1.get_units(10);
  baseline2 = scale2.get_units(10);
  baseline3 = scale3.get_units(10);
}

void sendToGoogleSheet(const char* intakeType, const char* status) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(scriptUrl);
    http.addHeader("Content-Type", "application/json");
    String jsonData = "{";
    jsonData += "\"intake\":\"" + String(intakeType) + "\",";
    jsonData += "\"status\":\"" + String(status) + "\",";
    jsonData += "\"morningLeft\":" + String(morningLeft) + ",";
    jsonData += "\"afternoonLeft\":" + String(afternoonLeft) + ",";
    jsonData += "\"eveningLeft\":" + String(eveningLeft) + ",";
    jsonData += "\"total\":" + String(totalTablets);
    jsonData += "}";
    int httpResponseCode = http.POST(jsonData);
    if (httpResponseCode > 0)
      Serial.printf("Sheet update: %s - %s\n", intakeType, status);
    else
      Serial.printf("Error sending data: %d\n", httpResponseCode);
    http.end();
  }
}

bool waitForResponse(const char *response, unsigned long timeout) {
  unsigned long start = millis();
  String reply;
  while (millis() - start < timeout) {
    while (SIM800.available()) {
      char c = SIM800.read();
      reply += c;
      if (reply.indexOf(response) != -1) return true;
    }
  }
  return false;
}

bool sendSMS(const char *number, const char *message) {
  int maxRetries = 5;
  for (int attempt = 1; attempt <= maxRetries; attempt++) {
    Serial.printf("📡 [Attempt %d/%d] Sending SMS...\n", attempt, maxRetries);
    while (SIM800.available()) SIM800.read();

    SIM800.println("AT+CREG?");
    if (!waitForResponse("+CREG: 0,1", 3000) && !waitForResponse("+CREG: 0,5", 3000)) {
      Serial.println("⚠ GSM not registered yet. Retrying...");
      delay(3000);
      continue;
    }

    SIM800.println("AT+CMGF=1");
    if (!waitForResponse("OK", 2000)) {
      Serial.println("⚠ Failed to set text mode. Retrying...");
      delay(2000);
      continue;
    }

    SIM800.print("AT+CMGS=\"");
    SIM800.print(number);
    SIM800.println("\"");
    if (!waitForResponse(">", 5000)) {
      Serial.println("⚠ No prompt for message input. Retrying...");
      delay(2000);
      continue;
    }

    SIM800.print(message);
    SIM800.write(26);
    if (waitForResponse("+CMGS", 15000)) {
      Serial.printf("✅ SMS sent successfully on attempt %d!\n", attempt);
      return true;
    } else {
      Serial.printf("❌ Attempt %d failed — retrying...\n", attempt);
      delay(3000);
    }
  }

  Serial.println("❌ SMS sending failed after 5 attempts — giving up.");
  return false;
}

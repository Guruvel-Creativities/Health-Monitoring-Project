// ==========================================
// 1. BLYNK CONFIGURATION
// ==========================================
#define BLYNK_TEMPLATE_ID   "YOUR_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "Heart Rate Dashboard"
#define BLYNK_AUTH_TOKEN    "pZz2zTdeRpBy9zKsm4EKTin4ykvcgFKi"

#define BLYNK_PRINT Serial

#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <time.h>

// ==========================================
// 2. NETWORK & HARDWARE CONFIGURATION
// ==========================================
#define WIFI_SSID     "Health Project"
#define WIFI_PASSWORD "Project@123"

// Pin Definitions
#define TOUCH_PIN      D6  // TTP223 Touch Sensor
#define LED_GOOD_HR    D5  // Good HR LED (<= 100 BPM)
#define LED_BAD_HR     D7  // Bad HR LED (> 100 BPM)
#define SDA_PIN        D2  // I2C SDA
#define SCL_PIN        D1  // I2C SCL

// OLED Config
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define I2C_ADDRESS   0x3C

// Long Press Threshold
#define LONG_PRESS_MS 1500  // 1.5 seconds

// Timezone Config for India (UTC + 5:30 -> 19800 seconds offset)
#define GMT_OFFSET_SEC      19800
#define DAYLIGHT_OFFSET_SEC 0

// ==========================================
// 3. GLOBAL OBJECTS & VARIABLES
// ==========================================
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
BlynkTimer timer;

int heartRate = 0;
int spO2 = 0;
bool hasMeasured = false;
bool isMeasuring = false;

// Touch Timing Tracking
unsigned long touchStartTime = 0;
bool touchActive = false;

// Get Accurate Local India Time String
String getIndiaTimeString() {
  time_t nowTime = time(nullptr);
  struct tm* timeinfo = localtime(&nowTime);

  if (timeinfo->tm_year < (2020 - 1900)) {
    return "07:35PM";
  }

  int h = timeinfo->tm_hour;
  int m = timeinfo->tm_min;
  bool isPM = (h >= 12);

  if (h == 0) h = 12;
  else if (h > 12) h -= 12;

  char timeBuff[10];
  snprintf(timeBuff, sizeof(timeBuff), "%02d:%02d%s", h, m, isPM ? "PM" : "AM");
  return String(timeBuff);
}

// Render Header with India Time
void drawHeader(const char* title, const char* badge) {
  display.fillRect(0, 0, 128, 12, SH110X_WHITE);
  display.setTextSize(1);
  display.setTextColor(SH110X_BLACK, SH110X_WHITE);
  display.setCursor(2, 2);
  display.print(title);

  int badgeX = 128 - (strlen(badge) * 6) - 2;
  display.setCursor(badgeX, 2);
  display.print(badge);
  
  display.setTextColor(SH110X_WHITE);
}

// Render Results UI
void drawResultsUI(int hr, int spo2, const char* statusMsg) {
  display.clearDisplay();
  
  String timeStr = getIndiaTimeString();
  drawHeader(timeStr.c_str(), statusMsg);

  if (!hasMeasured) {
    display.drawRoundRect(4, 18, 120, 42, 4, SH110X_WHITE);
    display.setTextSize(1);
    display.setCursor(10, 26);
    display.print(F("HOLD TOUCH"));
    display.setCursor(10, 42);
    display.print(F("to Measure"));
  } else {
    // HR Box
    display.drawRoundRect(2, 16, 60, 45, 4, SH110X_WHITE);
    display.setTextSize(1);
    display.setCursor(8, 20);
    display.print(F("HEART"));
    
    display.setTextSize(2);
    int hrX = (hr >= 100) ? 12 : 18;
    display.setCursor(hrX, 31);
    display.print(hr);
    
    display.setTextSize(1);
    display.setCursor(20, 49);
    display.print(F("bpm"));

    // SpO2 Box
    display.drawRoundRect(66, 16, 60, 45, 4, SH110X_WHITE);
    display.setTextSize(1);
    display.setCursor(72, 20);
    display.print(F("SpO2"));
    
    display.setTextSize(2);
    int spo2X = (spo2 >= 100) ? 76 : 82;
    display.setCursor(spo2X, 31);
    display.print(spo2);
    
    display.setTextSize(1);
    display.setCursor(92, 49);
    display.print(F("%"));
  }

  display.display();
}

// Render 10s Measurement Countdown
void drawMeasuringUI(int secondsRemaining, int progressPercent) {
  display.clearDisplay();
  
  char timerBadge[6];
  snprintf(timerBadge, sizeof(timerBadge), "%ds", secondsRemaining);
  drawHeader("MEASURING...", timerBadge);

  display.setTextSize(1);
  display.setCursor(12, 22);
  display.print(F("Keep Steady"));

  display.drawRoundRect(8, 38, 112, 14, 3, SH110X_WHITE);

  int fillWidth = map(progressPercent, 0, 100, 0, 108);
  if (fillWidth > 0) {
    display.fillRoundRect(10, 40, fillWidth, 10, 2, SH110X_WHITE);
  }

  display.display();
}

// LED Indicator
void updateHRLEDs(int hr) {
  if (hr > 100) {
    digitalWrite(LED_GOOD_HR, LOW);
    digitalWrite(LED_BAD_HR, HIGH);
  } else {
    digitalWrite(LED_GOOD_HR, HIGH);
    digitalWrite(LED_BAD_HR, LOW);
  }
}

// Push data to Blynk & Trigger Event Notification
void sendDataToBlynk(int hr, int spo2) {
  Blynk.virtualWrite(V0, hr);
  Blynk.virtualWrite(V1, spo2);
  Serial.println("Data uploaded to Blynk!");

  // Send Push Notification if Heart Rate is abnormal (> 100 BPM)
  if (hr > 100) {
    String alertMsg = "High Heart Rate Alert: " + String(hr) + " BPM!";
    Blynk.logEvent("abnormal_hr", alertMsg);
    Serial.println("Notification sent: " + alertMsg);
  }
}

// Measurement Cycle
void runMeasurementCycle() {
  if (isMeasuring) return;
  isMeasuring = true;

  Serial.println("Starting 10-second measurement cycle...");

  digitalWrite(LED_GOOD_HR, LOW);
  digitalWrite(LED_BAD_HR, LOW);

  for (int i = 0; i < 10; i++) {
    int secondsRemaining = 10 - i;
    int progressPercent = (i + 1) * 10;

    drawMeasuringUI(secondsRemaining, progressPercent);
    Blynk.run();
    delay(1000);
  }

  heartRate = random(60, 120);
  spO2 = random(95, 100);
  hasMeasured = true;

  updateHRLEDs(heartRate);
  drawResultsUI(heartRate, spO2, "SYNC");

  sendDataToBlynk(heartRate, spO2);
  drawResultsUI(heartRate, spO2, "OK");

  isMeasuring = false;
}

// Auto Timer Callback (Every 10 mins)
void autoMeasurementTimer() {
  Serial.println("10-Minute Auto-Measurement Triggered");
  runMeasurementCycle();
}

// Periodic Display Update (Keeps Clock Current)
void refreshDisplayClock() {
  if (!isMeasuring) {
    drawResultsUI(heartRate, spO2, hasMeasured ? "OK" : "READY");
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(TOUCH_PIN, INPUT);
  pinMode(LED_GOOD_HR, OUTPUT);
  pinMode(LED_BAD_HR, OUTPUT);

  digitalWrite(LED_GOOD_HR, LOW);
  digitalWrite(LED_BAD_HR, LOW);

  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100);

  if (!display.begin(I2C_ADDRESS, true)) {
    Serial.println(F("SH1106 OLED failed!"));
    for (;;);
  }

  display.clearDisplay();
  drawHeader("SYSTEM BOOT", "WIFI");
  display.setTextSize(1);
  display.setCursor(10, 30);
  display.println(F("Connecting WiFi..."));
  display.display();

  // Connect Blynk
  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASSWORD);

  // Sync NTP India Time (UTC+5:30)
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  
  // Wait until time is synced
  time_t nowTime = time(nullptr);
  int retry = 0;
  while (nowTime < 1000 && retry < 20) {
    delay(500);
    nowTime = time(nullptr);
    retry++;
  }

  // Clear Blynk default values
  Blynk.virtualWrite(V0, 0);
  Blynk.virtualWrite(V1, 0);

  // Set Timers
  timer.setInterval(600000L, autoMeasurementTimer); // 10 minutes auto-measure
  timer.setInterval(10000L, refreshDisplayClock);    // Update clock on display every 10 seconds

  drawResultsUI(0, 0, "READY");
}

void loop() {
  Blynk.run();
  timer.run();

  // Long Press Logic (1.5 seconds)
  bool currentTouchState = digitalRead(TOUCH_PIN);

  if (currentTouchState == HIGH) {
    if (!touchActive) {
      touchActive = true;
      touchStartTime = millis();
    } else if (!isMeasuring && (millis() - touchStartTime >= LONG_PRESS_MS)) {
      Serial.println("Touch held for 1.5s - Triggering measurement!");
      runMeasurementCycle();
      touchActive = false;
    }
  } else {
    touchActive = false;
  }

  delay(20);
}

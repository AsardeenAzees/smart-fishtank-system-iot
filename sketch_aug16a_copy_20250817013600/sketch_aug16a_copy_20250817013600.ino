#include "DHT.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <ESP32Servo.h>
#include <time.h>

// -------------------- Configuration --------------------
#define WIFI_SSID        "Redmi Note 8"
#define WIFI_PASSWORD    "kijan123"

#define API_KEY          "AIzaSyCPokWLKrM-JbJgQL0vVDccdR-qrmAHQ_Q"
#define DATABASE_URL     "https://fishtank-system-default-rtdb.asia-southeast1.firebasedatabase.app"

#define SCREEN_WIDTH     128
#define SCREEN_HEIGHT    64
#define OLED_RESET       -1
#define OLED_I2C_ADDRESS 0x3C

// -------------------- Pins (same as your board) --------------------
#define DHTPIN            15
#define DHTTYPE           DHT11
#define TRIG_PIN          27
#define ECHO_PIN          18
#define SIGNAL_PIN        34     // soil
#define LIGHT_SENSOR_PIN  32
#define WATER_LEVEL_PIN   33
#define RELAY_DEVICE1     5      // light
#define RELAY_DEVICE2     16     // pump
#define BUZZER_PIN        25
#define SERVO_PIN         4

// -------------------- Objects --------------------
DHT dht(DHTPIN, DHTTYPE);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig configFB;
Servo feedServo;

// -------------------- Vars --------------------
bool signupOK = false;

float temperature = 0, humidity = 0, distanceCM = 0;
int soilMoisture = 0, lightLevel = 0, waterLevel = 0;

int lightManual = 0, pumpManual = 0, buzzerManual = 0;
int lightAutoPresence = 1, lightAutoLow = 1;
int pumpAutoPresence = 1, pumpAutoHourly = 1;
int pumpMaxOnSec = 300;
int proximityCM = 20, lightLowThresh = 1200, waterLowThresh = 1000, tempHighC = 32;
int tzOffsetMin = 330;
String ntpPool = "pool.ntp.org";

int buzzerEnable = 1;
int silenceWaterAlert = 0;
int feedingEnable = 1;
String feedTimes[2] = {"09:00","18:00"};
String lastFeedISO = "";
int manualFeedRequest = 0;

bool lightOn = false, pumpOn = false;
bool waterLowActive = false, tempHighOnceSent = false;

unsigned long lastUpload = 0;
const unsigned long uploadInterval = 1000;

unsigned long presenceLastSeenMs = 0;
const unsigned long presenceHoldMs = 15000; // keep ON for 15s after last presence

unsigned long pumpStartedMs = 0;
unsigned long lastHourlyPumpMs = 0;
const unsigned long hourlyMs = 60UL * 60UL * 1000UL;

unsigned long lastWaterAlertBeepMs = 0;
bool waterAlertBeeping = false;

unsigned long lastTempHighBeepMs = 0;
bool tempBeepedThisEvent = false;

bool timeSynced = false;

// -------------------- Helpers --------------------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(300); }
  Serial.println("\nWiFi OK");
}

void initFirebase() {
  configFB.api_key = API_KEY;
  configFB.database_url = DATABASE_URL;
  Firebase.begin(&configFB, &auth);
  Firebase.reconnectWiFi(true);

  if (Firebase.signUp(&configFB, &auth, "", "")) {
    signupOK = true;
    Serial.println("Firebase auth OK");
  } else {
    Serial.printf("Firebase auth failed: %s\n", configFB.signer.signupError.message.c_str());
  }
}

void initDisplay() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
    Serial.println("OLED init failed");
    while (true);
  }
  display.clearDisplay();
  display.display();
}

void initHW() {
  dht.begin();
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(SIGNAL_PIN, INPUT);
  pinMode(LIGHT_SENSOR_PIN, INPUT);
  pinMode(WATER_LEVEL_PIN, INPUT);
  pinMode(RELAY_DEVICE1, OUTPUT);
  pinMode(RELAY_DEVICE2, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(RELAY_DEVICE1, HIGH); // active-LOW relays -> OFF
  digitalWrite(RELAY_DEVICE2, HIGH);
  digitalWrite(BUZZER_PIN, LOW);

  feedServo.attach(SERVO_PIN);
  feedServo.write(0);
}

float getDistanceCM() {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  return (duration > 0) ? (duration * 0.0343) / 2.0 : 1000.0;
}

void readSensors() {
  temperature = dht.readTemperature();
  humidity = dht.readHumidity();
  distanceCM = getDistanceCM();
  soilMoisture = analogRead(SIGNAL_PIN);
  lightLevel = analogRead(LIGHT_SENSOR_PIN);
  waterLevel = analogRead(WATER_LEVEL_PIN);
}

void drawLine(const String &label, const String &val, int &y) {
  display.setCursor(0, y);
  display.print(label); display.println(val);
  y += 10;
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  int y = 0;
  drawLine("T:", String(temperature,1)+"C H:"+String(humidity,0)+"%", y);
  drawLine("Dist:", String((int)distanceCM)+"cm", y);
  drawLine("Light:", String(lightLevel), y);
  drawLine("Water:", String(waterLevel), y);
  drawLine("Lgt:"+String(lightOn?"ON":"OFF")+" Pmp:"+String(pumpOn?"ON":"OFF"), "", y);
  display.display();
}

void writeBool(const char* path, bool v) {
  Firebase.RTDB.setInt(&fbdo, path, v?1:0);
}
void writeInt(const char* path, int v) {
  Firebase.RTDB.setInt(&fbdo, path, v);
}
void writeFloat(const char* path, float v) {
  Firebase.RTDB.setFloat(&fbdo, path, v);
}
void writeString(const char* path, const String& s) {
  Firebase.RTDB.setString(&fbdo, path, s);
}

void uploadSensors() {
  if (!Firebase.ready() || !signupOK) return;
  writeFloat("/sensors/temperature", temperature);
  writeFloat("/sensors/humidity",    humidity);
  writeFloat("/sensors/distance",    distanceCM);
  writeInt("/sensors/soilMoisture",  soilMoisture);
  writeInt("/sensors/lightLevel",    lightLevel);
  writeInt("/sensors/waterLevel",    waterLevel);
}

void syncConfig() {
  if (!Firebase.ready() || !signupOK) return;

  auto getI = [&](const char* p, int &dst){ if(Firebase.RTDB.getInt(&fbdo,p)) dst=fbdo.intData(); };
  auto getS = [&](const char* p, String &dst){ if(Firebase.RTDB.getString(&fbdo,p)) dst=fbdo.stringData(); };

  getI("/config/thresholds/proximity_cm", proximityCM);
  getI("/config/thresholds/light_low", lightLowThresh);
  getI("/config/thresholds/water_low", waterLowThresh);
  getI("/config/thresholds/temp_high_c", tempHighC);

  getI("/config/light/manual", lightManual);
  getI("/config/light/auto_presence", lightAutoPresence);
  getI("/config/light/auto_low_light", lightAutoLow);

  getI("/config/pump/manual", pumpManual);
  getI("/config/pump/auto_presence", pumpAutoPresence);
  getI("/config/pump/auto_hourly", pumpAutoHourly);
  getI("/config/pump/max_on_seconds", pumpMaxOnSec);

  getI("/config/buzzer/enable", buzzerEnable);
  getI("/config/buzzer/silence_water_alert", silenceWaterAlert);

  getI("/config/feeding/enable", feedingEnable);
  getS("/config/feeding/times/0", feedTimes[0]);
  getS("/config/feeding/times/1", feedTimes[1]);
  getS("/config/feeding/last_feed_iso", lastFeedISO);
  getI("/config/feeding/manual_feed_request", manualFeedRequest);

  getI("/config/time/tz_offset_minutes", tzOffsetMin);
  getS("/config/time/ntp_pool", ntpPool);
}

void setRelay(int pin, bool on) {
  digitalWrite(pin, on ? LOW : HIGH); // active-LOW
}

void setLight(bool on) { lightOn = on; setRelay(RELAY_DEVICE1, on); writeBool("/state/light_on", on); }
void setPump(bool on)  { pumpOn  = on; setRelay(RELAY_DEVICE2, on); writeBool("/state/pump_on",  on); }

void beepFor(unsigned long ms) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(ms);
  digitalWrite(BUZZER_PIN, LOW);
}

bool isPresence() {
  return distanceCM <= proximityCM;
}

void handlePresenceLogic() {
  if (isPresence()) presenceLastSeenMs = millis();
  bool presenceActive = (millis() - presenceLastSeenMs) <= presenceHoldMs;

  // Light: manual OR (auto rules)
  bool lightDecision = (lightManual == 1);
  if (!lightDecision) {
    if (lightAutoPresence && presenceActive) lightDecision = true;
    if (!lightDecision && lightAutoLow && (lightLevel < lightLowThresh)) lightDecision = true;
  }
  setLight(lightDecision);

  // Pump: manual OR (presence or hourly logic, hourly handled separately)
  bool pumpDecision = (pumpManual == 1);
  if (!pumpDecision && pumpAutoPresence && presenceActive) pumpDecision = true;

  // Safety: enforce max run time
  if (pumpDecision && !pumpOn) {
    pumpStartedMs = millis();
  }
  if (pumpOn && (millis() - pumpStartedMs > (unsigned long)pumpMaxOnSec*1000UL)) {
    pumpDecision = false;
  }
  setPump(pumpDecision);
}

void handleHourlyPump() {
  if (!pumpAutoHourly) return;
  if (millis() - lastHourlyPumpMs >= hourlyMs) {
    lastHourlyPumpMs = millis();
    // Run pump for min( configured max_on_seconds, 30s default )
    unsigned long runMs = min((unsigned long)pumpMaxOnSec*1000UL, 30000UL);
    setPump(true);
    unsigned long start = millis();
    while (millis() - start < runMs) {
      delay(10);
    }
    setPump(false);
  }
}

void handleWaterAlert() {
  bool low = (waterLevel < waterLowThresh);
  writeBool("/state/water_low", low);
  writeBool("/alerts/water_low_active", low);

  if (!buzzerEnable) return;
  if (silenceWaterAlert) return;

  if (low) {
    // Repeat short beeps every ~5s until refilled
    if (millis() - lastWaterAlertBeepMs > 5000) {
      lastWaterAlertBeepMs = millis();
      digitalWrite(BUZZER_PIN, HIGH); delay(200);
      digitalWrite(BUZZER_PIN, LOW);
    }
  }
}

void handleTempAlert() {
  bool high = (temperature > tempHighC);
  writeBool("/state/temp_high", high);

  if (!buzzerEnable) return;
  if (high && !tempBeepedThisEvent) {
    tempBeepedThisEvent = true;
    digitalWrite(BUZZER_PIN, HIGH); delay(2000);
    digitalWrite(BUZZER_PIN, LOW);
  }
  if (!high) {
    tempBeepedThisEvent = false; // reset for next event
  }
}

void handleManualBuzzer() {
  if (buzzerManual == 1) {
    digitalWrite(BUZZER_PIN, HIGH);
  } else {
    if (!waterLowActive) digitalWrite(BUZZER_PIN, LOW); // water alert may toggle it
  }
}

time_t nowUTC() { return time(NULL); }

bool sameDay(time_t a, time_t b, int offsetMin) {
  a += offsetMin*60; b += offsetMin*60;
  tm ta, tb;
  gmtime_r(&a, &ta);
  gmtime_r(&b, &tb);
  return (ta.tm_year==tb.tm_year && ta.tm_yday==tb.tm_yday);
}

bool parseHHMM(const String &s, int &hh, int &mm) {
  int colon = s.indexOf(':');
  if (colon < 0) return false;
  hh = s.substring(0, colon).toInt();
  mm = s.substring(colon+1).toInt();
  return (hh>=0 && hh<24 && mm>=0 && mm<60);
}

String isoNowLocal() {
  time_t t = nowUTC() + tzOffsetMin*60;
  tm tmL; gmtime_r(&t, &tmL);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmL);
  return String(buf);
}

void doFeed() {
  // simple rotate to 120°, pause, back to 0°
  feedServo.write(120); delay(700);
  feedServo.write(0); delay(500);
}

bool hasFedTodayAt(const String& HHMM, const String& lastISO) {
  if (lastISO.length()<10) return false;
  // we’ll just rely on state machine below instead of deep parsing here.
  return false;
}

void handleFeeding() {
  if (!feedingEnable) return;
  if (!timeSynced) return;

  // Manual feed
  if (manualFeedRequest > 0) {
    doFeed();
    writeString("/config/feeding/last_feed_iso", isoNowLocal());
    Firebase.RTDB.setInt(&fbdo, "/config/feeding/manual_feed_request", 0);
  }

  // Scheduled feeds (two times)
  time_t t = nowUTC() + tzOffsetMin*60;
  tm tl; gmtime_r(&t, &tl);
  int currHM = tl.tm_hour*60 + tl.tm_min;

  static int lastMinuteChecked = -1;
  if (lastMinuteChecked == tl.tm_min) return; // run once per minute
  lastMinuteChecked = tl.tm_min;

  // We’ll record last_feed_iso date; allow two feeds/day by recording last feed time,
  // and only trigger if we haven’t fed within ±5 minutes window for that HH:MM.
  String lastISO; 
  if (Firebase.RTDB.getString(&fbdo, "/config/feeding/last_feed_iso")) {
    lastISO = fbdo.stringData();
  }
  time_t lastFeedUTC = 0;
  if (lastISO.length() >= 19) {
    // naive parse YYYY-MM-DDTHH:MM:SS (local)
    int y = lastISO.substring(0,4).toInt();
    int m = lastISO.substring(5,7).toInt();
    int d = lastISO.substring(8,10).toInt();
    int hh = lastISO.substring(11,13).toInt();
    int mm = lastISO.substring(14,16).toInt();
    tm tml = {};
    tml.tm_year = y - 1900;
    tml.tm_mon  = m - 1;
    tml.tm_mday = d;
    tml.tm_hour = hh;
    tml.tm_min  = mm;
    tml.tm_sec  = 0;
    // mktime assumes local, but our tm is "local already", we just convert back to UTC by subtracting offset:
    lastFeedUTC = mktime(&tml) - tzOffsetMin*60;
  }

  auto maybeFeedAt = [&](const String &HHMM) {
    int fh=0,fm=0; if(!parseHHMM(HHMM, fh, fm)) return;
    int feedHM = fh*60+fm;
    if (abs(currHM - feedHM) <= 1) { // minute hits
      // Have we already fed today at around this time? We allow one feed per schedule per day.
      if (lastFeedUTC != 0 && sameDay(nowUTC(), lastFeedUTC, tzOffsetMin)) {
        // already fed today -> skip
        return;
      }
      // short beep before feed
      if (buzzerEnable) { digitalWrite(BUZZER_PIN, HIGH); delay(200); digitalWrite(BUZZER_PIN, LOW); }
      doFeed();
      String iso = isoNowLocal();
      writeString("/config/feeding/last_feed_iso", iso);
    }
  };

  maybeFeedAt(feedTimes[0]);
  maybeFeedAt(feedTimes[1]);
}

void handleControls() {
  if (!Firebase.ready() || !signupOK) return;
  // Read GUI toggles (for quick manual actions)
  int v;
  if (Firebase.RTDB.getInt(&fbdo, "/controls/light"))  { v=fbdo.intData(); setLight(v==1); }
  if (Firebase.RTDB.getInt(&fbdo, "/controls/pump"))   { v=fbdo.intData(); setPump(v==1);  }
  if (Firebase.RTDB.getInt(&fbdo, "/controls/buzzer")) { v=fbdo.intData(); digitalWrite(BUZZER_PIN, v==1?HIGH:LOW); }
}

void syncTime() {
  configTime(0, 0, ntpPool.c_str(), "time.nist.gov");
  Serial.print("Syncing time");
  for (int i=0;i<20;i++) {
    time_t t = nowUTC();
    if (t > 1700000000) { timeSynced = true; break; } // sanity check
    Serial.print("."); delay(500);
  }
  Serial.println(timeSynced ? " OK" : " failed (will retry later)");
}

void setup() {
  Serial.begin(115200);
  connectWiFi();
  initHW();
  initDisplay();
  initFirebase();
  syncConfig();
  syncTime();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  // Pull config & ad-hoc controls periodically
  static unsigned long lastCfgMs = 0;
  if (millis() - lastCfgMs > 2000) { lastCfgMs = millis(); syncConfig(); handleControls(); }

  readSensors();
  updateDisplay();

  // Core automations
  handlePresenceLogic(); // presence + low-light + pump safety
  handleHourlyPump();    // hourly short run
  handleWaterAlert();    // repeating until refilled (unless silenced)
  handleTempAlert();     // one-time 2s beep on high temp
  handleManualBuzzer();  // respect manual buzzer on/off
  handleFeeding();       // scheduled + manual feed

  // Upload telemetry
  if (millis() - lastUpload >= uploadInterval) {
    lastUpload = millis();
    uploadSensors();
  }

  delay(50);
}

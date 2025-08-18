/*******************************
 * Smart Fish Tank – ESP32 Firmware (with Buzzer Feed-Only mode)
 * - Sensors: DHT11, Ultrasonic, LDR (ADC), Water level (ADC)
 * - Actuators: Light relay, Pump relay (active LOW), Buzzer, Servo feeder, OLED
 * - Rules: presence, low-light, hourly pump; buzzer alerts; scheduled feeding
 * - Firebase RTDB sync: sensors, states, commands, settings, logs
 * - Offline-safe automation if Wi-Fi drops
 *******************************/

#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>

// -------------------- User Config --------------------
#define WIFI_SSID "Redmi Note 8"
#define WIFI_PASSWORD "kijan123"

#define API_KEY "AIzaSyCPokWLKrM-JbJgQL0vVDccdR-qrmAHQ_Q"
#define DATABASE_URL "https://fishtank-system-default-rtdb.asia-southeast1.firebasedatabase.app"

// Optional timezone (Sri Lanka +5:30)
static const long GMT_OFFSET_SEC = 5 * 3600 + 30 * 60;
static const int DST_OFFSET_SEC = 0;

// -------------------- Pins (fixed) -------------------
#define PIN_DHT 15 // DHT11 on GPIO15 (boot strap; ensure ~10k pull-up)
#define DHTTYPE DHT11

#define PIN_US_TRIG 27
#define PIN_US_ECHO 18 // Level shift to 3.3V if sensor is 5V!

#define PIN_LDR 32       // ADC
#define PIN_WATER 33     // ADC
#define PIN_SPARE_ADC 34 // Not used

#define PIN_LIGHT_RELAY 5 // Active LOW
#define PIN_PUMP_RELAY 16 // Active LOW
#define PIN_BUZZER 25
#define PIN_SERVO 4

// OLED I2C
#define I2C_SDA 21
#define I2C_SCL 22
#define OLED_ADDR 0x3C
#define OLED_W 128
#define OLED_H 64

// -------------------- Globals ------------------------
DHT dht(PIN_DHT, DHTTYPE);
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
Servo feeder;

FirebaseData fbdo;
FirebaseData stream;
FirebaseAuth auth;
FirebaseConfig configFB;

// Current readings
float gTempC = NAN, gHum = NAN;
int gLightADC = 0, gWaterADC = 0;
int gProximity = 9999; // cm

// Relay/buzzer states (true = ON)
bool lightOn = false, pumpOn = false, buzzerOn = false;

// Modes
enum Mode
{
  MODE_AUTO,
  MODE_MANUAL,
  MODE_OFF
};
Mode lightMode = MODE_AUTO, pumpMode = MODE_AUTO;

// ---- Buzzer mode (defined BEFORE use) ----
enum BuzzerMode
{
  BUZZER_NORMAL,
  BUZZER_FEED_ONLY
};
BuzzerMode buzzerMode = BUZZER_NORMAL;

String buzzerModeToStr(BuzzerMode m) { return (m == BUZZER_FEED_ONLY) ? "feed_only" : "normal"; }
BuzzerMode strToBuzzerMode(const String &s)
{
  String a = s;
  a.toLowerCase();
  return (a == "feed_only") ? BUZZER_FEED_ONLY : BUZZER_NORMAL;
}

// Settings (defaults; will be overwritten from RTDB /settings)
struct
{
  bool presenceRuleEnabled = true;
  bool lowLightRuleEnabled = true;
  struct
  {
    bool enabled = true;
    int seconds = 25; // runtime per hour
  } hourlyPump;
  // Thresholds
  int proximity_cm = 20;
  int low_light_adc = 1700; // adjust based on your LDR divider
  int water_low_adc = 1200; // higher/lower depends on your probe
  float high_temp_c = 32.0;

  // Feeding
  bool feedingEnabled = true;
  String feedTime1 = "08:00";
  String feedTime2 = "20:00";

  // Buzzer
  bool alertsEnabled = true;
} settings;

// Buzzer silence window (epoch seconds)
time_t buzzerSilencedUntil = 0;

// Timers
unsigned long tSensors = 0;
unsigned long tOLED = 0;
unsigned long tLog = 0;
unsigned long tPoll = 0;
unsigned long tNTPRetry = 0;

const unsigned long SENSORS_MS = 2000;
const unsigned long OLED_MS = 2000;
const unsigned long LOG_MS = 60000;
const unsigned long POLL_MS = 2000;

// Hourly scheduler bookkeeping
unsigned long lastHourlyPumpMs = 0;
bool hourlyPumpRunning = false;
unsigned long hourlyPumpStopAt = 0;

// Feeding bookkeeping
time_t lastFeedEpoch = 0;  // last feed time
String lastFeedLabel = ""; // which feed time triggered
bool ntpReady = false;

// -------------------- Helpers ------------------------
void relayWrite(uint8_t pin, bool on)
{
  // Active LOW: LOW = ON
  digitalWrite(pin, on ? LOW : HIGH);
}

void setLight(bool on)
{
  lightOn = on;
  relayWrite(PIN_LIGHT_RELAY, lightOn);
}
void setPump(bool on)
{
  pumpOn = on;
  relayWrite(PIN_PUMP_RELAY, pumpOn);
}
void setBuzzer(bool on)
{
  buzzerOn = on;
  digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
}

long median3(long a, long b, long c)
{
  if (a > b)
    std::swap(a, b);
  if (b > c)
    std::swap(b, c);
  if (a > b)
    std::swap(a, b);
  return b; // median
}

int readUltrasonicCM()
{
  // Return distance in cm; if timeout, return large value
  long readings[3];
  for (int i = 0; i < 3; i++)
  {
    digitalWrite(PIN_US_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_US_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_US_TRIG, LOW);
    long dur = pulseIn(PIN_US_ECHO, HIGH, 25000UL); // ~4.3m max
    readings[i] = dur == 0 ? 99999 : dur / 58;      // us to cm
    delay(10);
  }
  return (int)median3(readings[0], readings[1], readings[2]);
}

void beepMs(uint16_t ms)
{
  setBuzzer(true);
  delay(ms);
  setBuzzer(false);
}

void feederDispense()
{
  // Short beep only in Feed-Only mode to indicate feeding
  if (buzzerMode == BUZZER_FEED_ONLY)
    beepMs(200);

  // Simple 0->90->0 sweep
  feeder.write(90);
  delay(1000);
  feeder.write(0);
}

String modeToStr(Mode m)
{
  switch (m)
  {
  case MODE_AUTO:
    return "auto";
  case MODE_MANUAL:
    return "manual";
  case MODE_OFF:
    return "off";
  }
  return "auto";
}

Mode strToMode(const String &s)
{
  String a = s;
  a.toLowerCase();
  if (a == "manual")
    return MODE_MANUAL;
  if (a == "off")
    return MODE_OFF;
  return MODE_AUTO;
}

String twoDigits(int v) { return (v < 10 ? "0" : "") + String(v); }

String dateStr(time_t t)
{
  struct tm lt;
  localtime_r(&t, &lt);
  return String(1900 + lt.tm_year) + "-" + twoDigits(1 + lt.tm_mon) + "-" + twoDigits(lt.tm_mday);
}
String timeStr(time_t t)
{
  struct tm lt;
  localtime_r(&t, &lt);
  return twoDigits(lt.tm_hour) + ":" + twoDigits(lt.tm_min) + ":" + twoDigits(lt.tm_sec);
}

bool isSameDay(time_t a, time_t b)
{
  struct tm la, lb;
  localtime_r(&a, &la);
  localtime_r(&b, &lb);
  return (la.tm_year == lb.tm_year && la.tm_yday == lb.tm_yday);
}

// Parse "HH:MM" and return seconds since midnight
int parseHHMMtoSec(const String &hhmm)
{
  int colon = hhmm.indexOf(':');
  if (colon < 0)
    return -1;
  int h = hhmm.substring(0, colon).toInt();
  int m = hhmm.substring(colon + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59)
    return -1;
  return h * 3600 + m * 60;
}

// -------------------- Firebase paths -----------------
const char *PATH_SENSORS = "/sensors";
const char *PATH_STATES = "/states";
const char *PATH_SETTINGS = "/settings";
const char *PATH_COMMANDS = "/commands";
const char *PATH_META = "/meta";
const char *PATH_LOGS = "/logs/sensors";

// -------------------- Firebase I/O -------------------
void pushStates()
{
  if (!Firebase.ready())
    return;
  FirebaseJson json;
  json.add("light_on", lightOn);
  json.add("pump_on", pumpOn);
  json.add("buzzer_on", buzzerOn);
  json.add("light_mode", modeToStr(lightMode));
  json.add("pump_mode", modeToStr(pumpMode));
  json.add("buzzer_mode", buzzerModeToStr(buzzerMode)); // include mode
  Firebase.RTDB.updateNode(&fbdo, PATH_STATES, &json);
}

void pushSensors()
{
  if (!Firebase.ready())
    return;
  FirebaseJson j;
  j.add("temperature_c", gTempC);
  j.add("humidity_pct", gHum);
  j.add("light_adc", gLightADC);
  j.add("water_adc", gWaterADC);
  j.add("proximity_cm", gProximity);
  j.add("last_update", (int)time(nullptr));
  Firebase.RTDB.updateNode(&fbdo, PATH_SENSORS, &j);
}

void pushMetaFeed(time_t when, const String &label)
{
  if (!Firebase.ready())
    return;
  FirebaseJson j;
  j.add("last_feed_time", (int)when);
  j.add("last_feed_label", label);
  Firebase.RTDB.updateNode(&fbdo, PATH_META, &j);
}

void logSensors()
{
  if (!Firebase.ready())
    return;
  time_t now = time(nullptr);
  String date = dateStr(now);
  String ts = String((int)now);
  String path = String(PATH_LOGS) + "/" + date + "/" + ts;
  FirebaseJson j;
  j.add("tC", gTempC).add("h", gHum).add("ldr", gLightADC).add("water", gWaterADC).add("prox_cm", gProximity);
  Firebase.RTDB.setJSON(&fbdo, path.c_str(), &j);
}

// ---- JSON helpers ----
bool jsonGetBool(FirebaseJson &j, const char *path, bool &out)
{
  FirebaseJsonData r;
  if (j.get(r, path) && r.success && r.typeNum == FirebaseJson::JSON_BOOL)
  {
    out = r.to<bool>();
    return true;
  }
  return false;
}
bool jsonGetInt(FirebaseJson &j, const char *path, int &out)
{
  FirebaseJsonData r;
  if (j.get(r, path) && r.success && (r.typeNum == FirebaseJson::JSON_INT || r.typeNum == FirebaseJson::JSON_FLOAT || r.typeNum == FirebaseJson::JSON_DOUBLE))
  {
    out = r.to<int>();
    return true;
  }
  return false;
}
bool jsonGetFloat(FirebaseJson &j, const char *path, float &out)
{
  FirebaseJsonData r;
  if (j.get(r, path) && r.success && (r.typeNum == FirebaseJson::JSON_INT || r.typeNum == FirebaseJson::JSON_FLOAT || r.typeNum == FirebaseJson::JSON_DOUBLE))
  {
    out = (float)r.to<double>();
    return true;
  }
  return false;
}
bool jsonGetString(FirebaseJson &j, const char *path, String &out)
{
  FirebaseJsonData r;
  if (j.get(r, path) && r.success && r.typeNum == FirebaseJson::JSON_STRING)
  {
    out = r.to<const char *>();
    return true;
  }
  return false;
}

void fetchSettings()
{
  if (!Firebase.ready())
    return;
  if (Firebase.RTDB.getJSON(&fbdo, PATH_SETTINGS))
  {
    FirebaseJson *jp = fbdo.to<FirebaseJson *>();
    if (!jp)
      return;
    FirebaseJson &j = *jp;

    bool b;
    int i;
    float f;
    String s;

    if (jsonGetBool(j, "presence_rule", b))
      settings.presenceRuleEnabled = b;
    if (jsonGetBool(j, "low_light_rule", b))
      settings.lowLightRuleEnabled = b;
    if (jsonGetBool(j, "hourly_pump/enabled", b))
      settings.hourlyPump.enabled = b;
    if (jsonGetInt(j, "hourly_pump/seconds", i))
      settings.hourlyPump.seconds = i;

    if (jsonGetInt(j, "thresholds/proximity_cm", i))
      settings.proximity_cm = i;
    if (jsonGetInt(j, "thresholds/low_light_adc", i))
      settings.low_light_adc = i;
    if (jsonGetInt(j, "thresholds/water_low_adc", i))
      settings.water_low_adc = i;
    if (jsonGetFloat(j, "thresholds/high_temp_c", f))
      settings.high_temp_c = f;

    if (jsonGetBool(j, "feeding/enabled", b))
      settings.feedingEnabled = b;
    if (jsonGetString(j, "feeding/time1", s))
      settings.feedTime1 = s;
    if (jsonGetString(j, "feeding/time2", s))
      settings.feedTime2 = s;

    if (jsonGetBool(j, "buzzer/alerts_enabled", b))
      settings.alertsEnabled = b;

    if (jsonGetString(j, "light_mode", s))
      lightMode = strToMode(s);
    if (jsonGetString(j, "pump_mode", s))
      pumpMode = strToMode(s);

    // Optional default buzzer mode from settings
    if (jsonGetString(j, "buzzer/mode", s))
      buzzerMode = strToBuzzerMode(s);
  }
}

void applyCommandsSnapshot()
{
  if (!Firebase.ready())
    return;
  if (Firebase.RTDB.getJSON(&fbdo, PATH_COMMANDS))
  {
    FirebaseJson *jp = fbdo.to<FirebaseJson *>();
    if (!jp)
      return;
    FirebaseJson &j = *jp;

    String s;
    bool b;
    int i;

    if (jsonGetString(j, "light_mode", s))
      lightMode = strToMode(s);
    if (jsonGetBool(j, "light_manual_state", b) && lightMode == MODE_MANUAL)
      setLight(b);

    if (jsonGetString(j, "pump_mode", s))
      pumpMode = strToMode(s);
    if (jsonGetBool(j, "pump_manual_state", b) && pumpMode == MODE_MANUAL)
      setPump(b);

    // NEW: buzzer mode command
    if (jsonGetString(j, "buzzer_mode", s))
      buzzerMode = strToBuzzerMode(s);

    if (jsonGetBool(j, "buzzer_toggle", b) && b)
    {
      setBuzzer(!buzzerOn);
      FirebaseJson ack;
      ack.add("buzzer_toggle", false);
      Firebase.RTDB.updateNode(&fbdo, PATH_COMMANDS, &ack);
    }

    if (jsonGetInt(j, "buzzer_silence_minutes", i) && i > 0)
    {
      time_t now = time(nullptr);
      buzzerSilencedUntil = now + i * 60;
      FirebaseJson ack;
      ack.add("buzzer_silence_minutes", 0);
      Firebase.RTDB.updateNode(&fbdo, PATH_COMMANDS, &ack);
    }

    if (jsonGetBool(j, "feed_now", b) && b)
    {
      feederDispense();
      time_t now = time(nullptr);
      lastFeedEpoch = now;
      lastFeedLabel = "manual";
      pushMetaFeed(now, lastFeedLabel);
      FirebaseJson ack;
      ack.add("feed_now", false);
      Firebase.RTDB.updateNode(&fbdo, PATH_COMMANDS, &ack);
    }
  }
}

// Stream handler for faster reaction (optional but nice)
void streamCallback(FirebaseStream data)
{
  if (data.dataTypeEnum() == fb_esp_rtdb_data_type_json)
  {
    // Simple approach: just apply latest commands
    applyCommandsSnapshot();
  }
}
void streamTimeoutCallback(bool timeout) { /* no-op */ }

// -------------------- OLED ---------------------------
void drawOLED()
{
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("T:");
  display.print(isnan(gTempC) ? 0 : gTempC, 1);
  display.print("C  H:");
  display.print(isnan(gHum) ? 0 : gHum, 0);
  display.println("%");

  display.setCursor(0, 10);
  display.print("LDR:");
  display.print(gLightADC);
  display.setCursor(64, 10);
  display.print("Water:");
  display.print(gWaterADC);

  display.setCursor(0, 20);
  display.print("Prox: ");
  display.print(gProximity);
  display.println("cm");

  display.setCursor(0, 30);
  display.print("Light:");
  display.print(lightOn ? "ON " : "OFF");
  display.print(" (");
  display.print(modeToStr(lightMode));
  display.print(")");

  display.setCursor(0, 40);
  display.print("Pump :");
  display.print(pumpOn ? "ON " : "OFF");
  display.print(" (");
  display.print(modeToStr(pumpMode));
  display.print(")");

  display.setCursor(0, 50);
  display.print("Buzz: ");
  display.print(buzzerOn ? "ON" : "OFF");

  display.display();
}

// -------------------- Automation ---------------------
void applyAutomation()
{
  // Presence & low light
  bool presence = gProximity <= settings.proximity_cm;
  bool lowLight = gLightADC <= settings.low_light_adc;

  // LIGHT
  if (lightMode == MODE_OFF)
    setLight(false);
  else if (lightMode == MODE_MANUAL)
  { /* manual */
  }
  else
  {
    bool shouldOn = (settings.presenceRuleEnabled && presence) ||
                    (settings.lowLightRuleEnabled && lowLight);
    setLight(shouldOn);
  }

  // PUMP
  if (pumpMode == MODE_OFF)
    setPump(false);
  else if (pumpMode == MODE_MANUAL)
  { /* manual */
  }
  else
  {
    bool shouldOn = (settings.presenceRuleEnabled && presence);
    if (!hourlyPumpRunning)
      setPump(shouldOn); // hourly pump overrides separately
  }

  // BUZZER alerts (respect mode/silence)
  time_t now = time(nullptr);
  bool withinSilence = now < buzzerSilencedUntil;
  bool waterLow = (gWaterADC <= settings.water_low_adc);
  bool tempHigh = (!isnan(gTempC) && gTempC > settings.high_temp_c);

  // Only alert in NORMAL mode; feed_only mutes regular alarms
  if (buzzerMode == BUZZER_NORMAL && settings.alertsEnabled && !withinSilence)
  {
    if (waterLow)
      beepMs(2000); // 2 sec chirp
    else if (tempHigh)
      beepMs(200); // short beep
  }
}

// Hourly pump worker
void hourlyPumpTick()
{
  if (!settings.hourlyPump.enabled || pumpMode != MODE_AUTO)
    return;

  unsigned long nowMs = millis();
  if (!hourlyPumpRunning)
  {
    // start every ~hour (3,600,000 ms)
    if (nowMs - lastHourlyPumpMs >= 3600000UL)
    {
      hourlyPumpRunning = true;
      setPump(true);
      hourlyPumpStopAt = nowMs + (unsigned long)settings.hourlyPump.seconds * 1000UL;
      lastHourlyPumpMs = nowMs;
    }
  }
  else
  {
    if ((long)(nowMs - hourlyPumpStopAt) >= 0)
    {
      hourlyPumpRunning = false;
      // Return to presence-controlled state:
      bool presence = gProximity <= settings.proximity_cm;
      setPump(settings.presenceRuleEnabled && presence);
    }
  }
}

// Feeding schedule (by NTP local time)
void feedingTick()
{
  if (!settings.feedingEnabled || !ntpReady)
    return;

  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  int secMidnight = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;

  auto maybeFeed = [&](const String &label, const String &hhmm)
  {
    int target = parseHHMMtoSec(hhmm);
    if (target < 0)
      return;
    // Trigger when within 10 seconds after the target sec, once per day per label
    if (secMidnight >= target && secMidnight <= target + 10)
    {
      // Ensure not already fed at this label today
      if (!isSameDay(now, lastFeedEpoch) || lastFeedLabel != label)
      {
        feederDispense();
        lastFeedEpoch = now;
        lastFeedLabel = label;
        pushMetaFeed(now, label);
      }
    }
  };

  maybeFeed("feed1", settings.feedTime1);
  maybeFeed("feed2", settings.feedTime2);
}

// -------------------- Setup & Loop -------------------
void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    if (++dots > 60)
      break;
  }
  Serial.println();
}

void setup()
{
  Serial.begin(115200);

  pinMode(PIN_US_TRIG, OUTPUT);
  pinMode(PIN_US_ECHO, INPUT);

  pinMode(PIN_LDR, INPUT);
  pinMode(PIN_WATER, INPUT);
  pinMode(PIN_SPARE_ADC, INPUT);

  pinMode(PIN_LIGHT_RELAY, OUTPUT);
  pinMode(PIN_PUMP_RELAY, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  setLight(false);
  setPump(false);
  setBuzzer(false);

  dht.begin();

  Wire.begin(I2C_SDA, I2C_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  display.clearDisplay();
  display.display();

  feeder.attach(PIN_SERVO, 500, 2400);
  feeder.write(0);

  connectWiFi();
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  // We'll check ntpReady in loop after time sync

  // Firebase config
  configFB.api_key = API_KEY;
  configFB.database_url = DATABASE_URL;
  configFB.token_status_callback = tokenStatusCallback;

  // Anonymous sign-up
  if (Firebase.signUp(&configFB, &auth, "", ""))
  {
    Serial.println("Firebase signUp OK");
  }
  else
  {
    Serial.printf("Firebase signUp error: %s\n", configFB.signer.signupError.message.c_str());
  }

  Firebase.begin(&configFB, &auth);
  Firebase.reconnectWiFi(true);
  Firebase.setDoubleDigits(3);

  // Stream commands for instant response
  if (!Firebase.RTDB.beginStream(&stream, PATH_COMMANDS))
  {
    Serial.printf("Stream begin error: %s\n", stream.errorReason().c_str());
  }
  else
  {
    Firebase.RTDB.setStreamCallback(&stream, streamCallback, streamTimeoutCallback);
  }

  // First pulls
  fetchSettings();
  applyCommandsSnapshot();
}

void loop()
{
  unsigned long nowMs = millis();

  // NTP ready check (only once)
  if (!ntpReady && nowMs - tNTPRetry > 2000)
  {
    tNTPRetry = nowMs;
    time_t now = time(nullptr);
    if (now > 1700000000)
    { // after ~2023
      ntpReady = true;
      Serial.println("NTP time is ready.");
    }
  }

  // Read sensors + OLED + automation
  if (nowMs - tSensors >= SENSORS_MS)
  {
    tSensors = nowMs;

    gProximity = readUltrasonicCM();

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t))
      gTempC = t;
    if (!isnan(h))
      gHum = h;

    gLightADC = analogRead(PIN_LDR);
    gWaterADC = analogRead(PIN_WATER);

    applyAutomation();
    pushSensors(); // also keeps dashboard fresh
    pushStates();
  }

  if (nowMs - tOLED >= OLED_MS)
  {
    tOLED = nowMs;
    drawOLED();
  }

  if (nowMs - tLog >= LOG_MS)
  {
    tLog = nowMs;
    logSensors();
  }

  if (nowMs - tPoll >= POLL_MS)
  {
    tPoll = nowMs;
    // Periodic safety fetch of settings and commands
    fetchSettings();
    applyCommandsSnapshot();
  }

  hourlyPumpTick();
  feedingTick();
}

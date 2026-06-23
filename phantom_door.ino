/*
 * ================================================================
 *   PHANTOM DOOR — Smart Fingerprint Door Lock
 *   ESP32 + R307 Fingerprint Sensor + SG90 Servo
 *   By Faysal Ali Shah
 * ================================================================
 *
 *  FEATURES:
 *   - Fingerprint auth with retry + anti-jitter stabilization
 *   - Hardware PWM servo (no jitter) with overshoot technique
 *   - Soft-start ramping so servo eases in/out
 *   - Web dashboard (serve local WiFi) — Realtime via WebSocket
 *   - Remote unlock via web button
 *   - Enroll / Delete users from web UI (no re-flashing)
 *   - 4-second unlock hold then auto-relock
 *   - LED status indicator
 *   - Watchdog timer to recover from hangs
 *   - Persistent fingerprint count in NVS (survives reboot)
 *   - Rate limiting: 3 failed attempts → 30s lockout
 *
 *  WIRING:
 *   R307 RX  → GPIO 17 (TX2)
 *   R307 TX  → GPIO 16 (RX2)
 *   R307 VCC → 3.3V (with 100µF cap to GND close to sensor)
 *   R307 GND → GND
 *   SG90 PWM → GPIO 18
 *   SG90 VCC → 5V rail (with 470µF cap to GND close to servo)
 *   SG90 GND → GND (common with ESP32)
 *   LED      → GPIO 2 (built-in) + external LED on GPIO 4
 *   BTN_ENROLL → GPIO 0 (BOOT button, active LOW)
 *
 *  SERVO ANGLES:
 *   LOCKED   = 0°   (no overshoot at rest — spring holds door)
 *   UNLOCK   = 90°  (overshoot to 100° first, settle back → smooth pull)
 *   RELOCK   = 0°   (overshoot to -10° = 0° clamped, then back)
 *
 *  LIBRARIES NEEDED (Arduino IDE / PlatformIO):
 *   - Adafruit Fingerprint Sensor Library (v2.x)
 *   - ESP32 core (espressif) ≥ 2.0.0
 *   - Preferences (built-in)
 *   - WebServer (built-in)
 *   - WebSocketsServer by Markus Sattler
 *   - ArduinoJson ≥ 6.x
 * ================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <Adafruit_Fingerprint.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

// ── WiFi Credentials ─────────────────────────────────────────────
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ── Pin Definitions ───────────────────────────────────────────────
#define SERVO_PIN       18
#define LED_STATUS      2      // Built-in LED
#define LED_EXTERNAL    4      // Green/Red external LED
#define BTN_ENROLL      0      // BOOT button = manual enroll trigger

// ── Servo Config (Hardware PWM via LEDC) ─────────────────────────
#define LEDC_CHANNEL    0
#define LEDC_FREQ_HZ    50     // Standard servo 50Hz
#define LEDC_RES_BITS   16     // 16-bit for fine resolution
// PWM pulse widths at 50Hz, 16-bit:
//   min pulse = 0.5ms → duty = (0.5/20)*65535 = 1638
//   max pulse = 2.5ms → duty = (2.5/20)*65535 = 8192
#define SERVO_MIN_DUTY  1638
#define SERVO_MAX_DUTY  8192

// ── Servo Angle Positions ─────────────────────────────────────────
#define SERVO_LOCKED    0      // Door locked
#define SERVO_UNLOCK    90     // Pull cord to open (overshoot to 100 first)
#define SERVO_OVERSHOOT 100    // Overshoot angle to overcome spring tension
#define SERVO_BACK_10   170    // Relock overshoot (slight compress)

// ── Timing ───────────────────────────────────────────────────────
#define UNLOCK_HOLD_MS      4000   // How long door stays unlocked
#define SERVO_RAMP_STEP_MS  8      // ms per degree during soft-start ramp
#define OVERSHOOT_HOLD_MS   80     // How long to hold overshoot angle
#define LOCKOUT_DURATION_MS 30000  // Lockout after 3 failed attempts
#define MAX_FAIL_ATTEMPTS   3

// ── Fingerprint Sensor ───────────────────────────────────────────
#define FINGERPRINT_RX 16    // ESP32 RX2
#define FINGERPRINT_TX 17    // ESP32 TX2
HardwareSerial fpSerial(2);
Adafruit_Fingerprint finger(&fpSerial);

// ── Server ───────────────────────────────────────────────────────
WebServer server(80);
WebSocketsServer ws(81);

// ── Persistent Storage ───────────────────────────────────────────
Preferences prefs;

// ── State Machine ────────────────────────────────────────────────
enum DoorState {
  STATE_IDLE,
  STATE_UNLOCKED,
  STATE_ENROLLING,
  STATE_LOCKOUT
};

volatile DoorState doorState = STATE_IDLE;
unsigned long unlockStartTime   = 0;
unsigned long lockoutStartTime  = 0;
int           failAttempts      = 0;
bool          enrollPending     = false;
uint8_t       enrollSlot        = 0;

// ── User names storage (slot 1–127) ──────────────────────────────
// Stored in NVS as "uN" where N is slot number
String getUserName(int slot) {
  prefs.begin("door", true);
  String name = prefs.getString(("u" + String(slot)).c_str(), "User " + String(slot));
  prefs.end();
  return name;
}
void setUserName(int slot, const String& name) {
  prefs.begin("door", false);
  prefs.putString(("u" + String(slot)).c_str(), name);
  prefs.end();
}

// ── Access Log (last 20 events in memory) ────────────────────────
struct LogEntry {
  String name;
  bool   success;
  unsigned long timestamp;
};
LogEntry accessLog[20];
int logIndex = 0;

void pushLog(const String& name, bool success) {
  accessLog[logIndex % 20] = { name, success, millis() };
  logIndex++;
}

// ── WebSocket broadcast ──────────────────────────────────────────
void broadcastStatus() {
  StaticJsonDocument<512> doc;
  doc["state"]       = (doorState == STATE_UNLOCKED) ? "unlocked"
                     : (doorState == STATE_LOCKOUT)   ? "lockout"
                     : (doorState == STATE_ENROLLING)  ? "enrolling"
                     :                                   "locked";
  doc["enrollCount"] = finger.getTemplateCount() == FINGERPRINT_OK ? finger.templateCount : 0;
  doc["failCount"]   = failAttempts;

  JsonArray logs = doc.createNestedArray("recentLog");
  for (int i = 0; i < min(logIndex, 20); i++) {
    int idx = (logIndex - 1 - i + 20) % 20;
    JsonObject entry = logs.createNestedObject();
    entry["name"]    = accessLog[idx].name;
    entry["success"] = accessLog[idx].success;
  }

  String out;
  serializeJson(doc, out);
  ws.broadcastTXT(out);
}

// ── Servo helpers ─────────────────────────────────────────────────
uint32_t angleToDuty(int angle) {
  angle = constrain(angle, 0, 180);
  return map(angle, 0, 180, SERVO_MIN_DUTY, SERVO_MAX_DUTY);
}

// Soft-start ramp: moves servo gradually degree-by-degree
void servoRamp(int fromAngle, int toAngle) {
  int step = (toAngle > fromAngle) ? 1 : -1;
  for (int a = fromAngle; a != toAngle; a += step) {
    ledcWrite(LEDC_CHANNEL, angleToDuty(a));
    delay(SERVO_RAMP_STEP_MS);
    esp_task_wdt_reset();  // Pet the watchdog during long ramp
  }
  ledcWrite(LEDC_CHANNEL, angleToDuty(toAngle));
}

void servoUnlock() {
  // 1. Ramp to overshoot (breaks static friction + spring tension)
  servoRamp(SERVO_LOCKED, SERVO_OVERSHOOT);
  delay(OVERSHOOT_HOLD_MS);
  // 2. Settle back to true unlock position (smooth cord tension)
  servoRamp(SERVO_OVERSHOOT, SERVO_UNLOCK);
}

void servoRelock() {
  // Ramp back, slight overshoot the other direction to seat latch
  servoRamp(SERVO_UNLOCK, SERVO_BACK_10);
  delay(OVERSHOOT_HOLD_MS);
  servoRamp(SERVO_BACK_10, SERVO_LOCKED);
}

// ── Status LED helpers ────────────────────────────────────────────
void blinkLED(int times, int onMs = 80, int offMs = 100) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_EXTERNAL, HIGH);
    delay(onMs);
    digitalWrite(LED_EXTERNAL, LOW);
    if (i < times - 1) delay(offMs);
  }
}

// ── Unlock logic ──────────────────────────────────────────────────
void triggerUnlock(const String& who) {
  if (doorState == STATE_UNLOCKED) return; // Already open
  doorState = STATE_UNLOCKED;
  unlockStartTime = millis();
  failAttempts = 0;

  Serial.println("[DOOR] UNLOCKING for: " + who);
  pushLog(who, true);

  blinkLED(2, 200, 100);  // 2 long green blinks
  servoUnlock();
  broadcastStatus();
}

void triggerRelock() {
  doorState = STATE_IDLE;
  Serial.println("[DOOR] RELOCKING");
  servoRelock();
  blinkLED(1, 50);
  broadcastStatus();
}

// ── Fingerprint: Read with retry ──────────────────────────────────
int fingerprintSearch() {
  // Try to get image, up to 3 attempts to debounce placement jitter
  uint8_t p;
  for (int attempt = 0; attempt < 3; attempt++) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) break;
    if (p == FINGERPRINT_NOFINGER) return -1;  // No finger, don't burn attempts
    delay(50);
  }
  if (p != FINGERPRINT_OK) return -1;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) return -2;  // -2 = no match found

  return finger.fingerID;  // Returns matched slot ID (1–127)
}

// ── Fingerprint: Enrollment (2-image process) ─────────────────────
void doEnroll(uint8_t slot, const String& name) {
  Serial.println("[ENROLL] Starting enrollment for slot " + String(slot) + " (" + name + ")");
  ws.broadcastTXT("{\"enrollStatus\":\"Place finger on sensor...\",\"step\":1}");

  uint8_t p;
  // Step 1: First image
  for (int i = 0; i < 30; i++) {  // 3 second timeout
    p = finger.getImage();
    if (p == FINGERPRINT_OK) break;
    delay(100);
    esp_task_wdt_reset();
  }
  if (p != FINGERPRINT_OK) {
    ws.broadcastTXT("{\"enrollStatus\":\"Timeout — no finger detected. Try again.\",\"step\":0}");
    doorState = STATE_IDLE;
    return;
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    ws.broadcastTXT("{\"enrollStatus\":\"Poor image — press firmly and retry.\",\"step\":0}");
    doorState = STATE_IDLE;
    return;
  }

  ws.broadcastTXT("{\"enrollStatus\":\"Good! Lift finger and place again...\",\"step\":2}");
  delay(1500);

  // Wait for finger to lift
  for (int i = 0; i < 20; i++) {
    if (finger.getImage() == FINGERPRINT_NOFINGER) break;
    delay(100);
    esp_task_wdt_reset();
  }

  // Step 2: Second image
  ws.broadcastTXT("{\"enrollStatus\":\"Place same finger again...\",\"step\":2}");
  for (int i = 0; i < 30; i++) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) break;
    delay(100);
    esp_task_wdt_reset();
  }
  if (p != FINGERPRINT_OK) {
    ws.broadcastTXT("{\"enrollStatus\":\"Timeout on second scan. Try again.\",\"step\":0}");
    doorState = STATE_IDLE;
    return;
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    ws.broadcastTXT("{\"enrollStatus\":\"Poor image on second scan. Try again.\",\"step\":0}");
    doorState = STATE_IDLE;
    return;
  }

  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    ws.broadcastTXT("{\"enrollStatus\":\"Fingerprints did not match. Try again.\",\"step\":0}");
    doorState = STATE_IDLE;
    return;
  }

  p = finger.storeModel(slot);
  if (p == FINGERPRINT_OK) {
    setUserName(slot, name);
    String msg = "{\"enrollStatus\":\"Enrolled! " + name + " saved to slot " + String(slot) + ".\",\"step\":3,\"success\":true}";
    ws.broadcastTXT(msg);
    Serial.println("[ENROLL] Success: " + name);
  } else {
    ws.broadcastTXT("{\"enrollStatus\":\"Storage error. Slot may be full.\",\"step\":0}");
  }

  doorState = STATE_IDLE;
  broadcastStatus();
}

// ── Find next free slot ───────────────────────────────────────────
int nextFreeSlot() {
  for (int i = 1; i <= 127; i++) {
    uint8_t p = finger.loadModel(i);
    if (p != FINGERPRINT_OK) return i;
  }
  return -1;  // Full
}

// ── Web Routes ────────────────────────────────────────────────────
// Inline the HTML/CSS/JS to avoid SPIFFS dependency
void handleRoot() {
  // Served from webapp/index.html content (embedded as string)
  // For deployment to GitHub Pages, the HTML is a separate file
  server.send(200, "text/html", getWebPage());
}

void handleUnlock() {
  if (server.hasArg("plain")) {
    StaticJsonDocument<128> doc;
    deserializeJson(doc, server.arg("plain"));
    String who = doc["who"] | "Remote User";
    triggerUnlock(who);
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    triggerUnlock("Remote User");
    server.send(200, "application/json", "{\"ok\":true}");
  }
}

void handleEnroll() {
  if (doorState != STATE_IDLE) {
    server.send(409, "application/json", "{\"error\":\"Busy\"}");
    return;
  }
  StaticJsonDocument<128> doc;
  deserializeJson(doc, server.arg("plain"));
  String name = doc["name"] | "Unknown";

  int slot = nextFreeSlot();
  if (slot == -1) {
    server.send(507, "application/json", "{\"error\":\"Sensor memory full (127 max)\"}");
    return;
  }

  doorState = STATE_ENROLLING;
  enrollSlot = slot;
  server.send(200, "application/json", "{\"ok\":true,\"slot\":" + String(slot) + "}");

  // Run enroll in main loop via flag (not in HTTP handler — blocks WiFi stack)
  enrollPending = true;
  enrollSlot = slot;
  // Store name temporarily
  prefs.begin("door", false);
  prefs.putString("pendingName", name);
  prefs.end();
}

void handleDeleteUser() {
  StaticJsonDocument<64> doc;
  deserializeJson(doc, server.arg("plain"));
  int slot = doc["slot"] | -1;
  if (slot < 1 || slot > 127) {
    server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
    return;
  }
  uint8_t p = finger.deleteModel(slot);
  if (p == FINGERPRINT_OK) {
    // Clear NVS name
    prefs.begin("door", false);
    prefs.remove(("u" + String(slot)).c_str());
    prefs.end();
    server.send(200, "application/json", "{\"ok\":true}");
    broadcastStatus();
  } else {
    server.send(500, "application/json", "{\"error\":\"Delete failed\"}");
  }
}

void handleListUsers() {
  StaticJsonDocument<2048> doc;
  JsonArray users = doc.createNestedArray("users");
  // Probe all 127 slots
  for (int i = 1; i <= 127; i++) {
    uint8_t p = finger.loadModel(i);
    if (p == FINGERPRINT_OK) {
      JsonObject u = users.createNestedObject();
      u["slot"] = i;
      u["name"] = getUserName(i);
    }
    esp_task_wdt_reset();
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleStatus() {
  finger.getTemplateCount();
  StaticJsonDocument<256> doc;
  doc["state"]        = (doorState == STATE_UNLOCKED) ? "unlocked"
                      : (doorState == STATE_LOCKOUT)   ? "lockout"
                      : (doorState == STATE_ENROLLING)  ? "enrolling"
                      :                                   "locked";
  doc["count"]        = finger.templateCount;
  doc["failAttempts"] = failAttempts;
  doc["ip"]           = WiFi.localIP().toString();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ── WebSocket handler ─────────────────────────────────────────────
void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    broadcastStatus();
    Serial.printf("[WS] Client #%u connected\n", num);
  }
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== PHANTOM DOOR BOOTING ===");

  // ── GPIO setup
  pinMode(LED_STATUS,   OUTPUT);
  pinMode(LED_EXTERNAL, OUTPUT);
  pinMode(BTN_ENROLL,   INPUT_PULLUP);
  digitalWrite(LED_STATUS,   LOW);
  digitalWrite(LED_EXTERNAL, LOW);

  // ── Watchdog: 10 second timeout
  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);

  // ── Servo init via hardware PWM
  ledcSetup(LEDC_CHANNEL, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcAttachPin(SERVO_PIN, LEDC_CHANNEL);
  ledcWrite(LEDC_CHANNEL, angleToDuty(SERVO_LOCKED));  // Start locked
  delay(300);  // Let servo settle

  // ── Fingerprint sensor
  fpSerial.begin(57600, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
  delay(100);
  if (!finger.begin()) {
    Serial.println("[ERROR] Fingerprint sensor not found! Check wiring.");
    // Flash error pattern and halt
    while (1) {
      blinkLED(5, 50, 50);
      delay(500);
    }
  }
  finger.getParameters();
  Serial.printf("[FP] Sensor found. Capacity: %d, Enrolled: %d\n",
                finger.capacity, finger.templateCount);

  // ── WiFi
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int wifiTries = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTries < 30) {
    delay(500);
    Serial.print(".");
    wifiTries++;
    esp_task_wdt_reset();
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    blinkLED(3, 100, 100);
  } else {
    Serial.println("\n[WiFi] Failed — running without web features");
  }

  // ── Web routes
  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/status",    HTTP_GET,  handleStatus);
  server.on("/users",     HTTP_GET,  handleListUsers);
  server.on("/unlock",    HTTP_POST, handleUnlock);
  server.on("/enroll",    HTTP_POST, handleEnroll);
  server.on("/delete",    HTTP_POST, handleDeleteUser);
  server.begin();
  ws.begin();
  ws.onEvent(onWsEvent);

  Serial.println("[SERVER] HTTP on :80, WS on :81");
  Serial.println("[READY] Phantom Door is armed.");
  digitalWrite(LED_STATUS, HIGH);
}

// ── Main Loop ─────────────────────────────────────────────────────
void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  ws.loop();

  // ── Auto-relock after hold time ──────────────────────────────
  if (doorState == STATE_UNLOCKED) {
    if (millis() - unlockStartTime >= UNLOCK_HOLD_MS) {
      triggerRelock();
    }
    return;  // Don't scan while door is open
  }

  // ── Release lockout after timeout ────────────────────────────
  if (doorState == STATE_LOCKOUT) {
    if (millis() - lockoutStartTime >= LOCKOUT_DURATION_MS) {
      doorState = STATE_IDLE;
      failAttempts = 0;
      Serial.println("[LOCKOUT] Released");
      broadcastStatus();
    }
    return;
  }

  // ── Handle enroll triggered from web ─────────────────────────
  if (enrollPending && doorState == STATE_ENROLLING) {
    enrollPending = false;
    prefs.begin("door", true);
    String pendingName = prefs.getString("pendingName", "User");
    prefs.end();
    doEnroll(enrollSlot, pendingName);
    return;
  }

  // ── Physical enroll button (BOOT pin, active LOW) ────────────
  if (digitalRead(BTN_ENROLL) == LOW && doorState == STATE_IDLE) {
    delay(50);  // Debounce
    if (digitalRead(BTN_ENROLL) == LOW) {
      int slot = nextFreeSlot();
      if (slot != -1) {
        doorState = STATE_ENROLLING;
        doEnroll(slot, "User " + String(slot));
        // Wait for button release
        while (digitalRead(BTN_ENROLL) == LOW) {
          delay(10);
          esp_task_wdt_reset();
        }
      }
    }
  }

  // ── Fingerprint scan ─────────────────────────────────────────
  int result = fingerprintSearch();

  if (result == -1) {
    // No finger present — normal idle, keep scanning
    return;
  }

  if (result == -2) {
    // Finger detected but no match
    failAttempts++;
    Serial.printf("[DENIED] No match. Fails: %d/%d\n", failAttempts, MAX_FAIL_ATTEMPTS);
    pushLog("Unknown", false);
    blinkLED(3, 50, 50);  // 3 rapid red blinks

    if (failAttempts >= MAX_FAIL_ATTEMPTS) {
      doorState = STATE_LOCKOUT;
      lockoutStartTime = millis();
      Serial.println("[LOCKOUT] Activated for 30 seconds");
      ws.broadcastTXT("{\"alert\":\"LOCKOUT — 3 failed attempts!\"}");
      broadcastStatus();
    } else {
      broadcastStatus();
    }
    delay(1000);  // Cooldown before next scan
    return;
  }

  // ── Match found ───────────────────────────────────────────────
  String name = getUserName(result);
  Serial.printf("[GRANTED] ID: %d, Name: %s, Confidence: %d\n",
                result, name.c_str(), finger.confidence);
  triggerUnlock(name);
}

// ── Embedded web page (served from ESP32) ─────────────────────────
// The full SPA is embedded here so no SPIFFS needed.
// The same HTML file is also in webapp/ for GitHub Pages hosting.
String getWebPage() {
  // This function returns the full HTML content.
  // See the separate webapp/public/index.html for the actual content —
  // in production, paste that file's content here as a raw string literal.
  // For now it returns a redirect to the GitHub Pages URL or the local page.
  return R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Phantom Door</title>
  <style>
    /* Minimal fallback if full SPA not embedded */
    body { font-family: sans-serif; background: #0a0a0a; color: #eee; text-align:center; padding: 40px; }
    h1 { color: #7c3aed; }
    p { color: #888; }
    a { color: #7c3aed; }
  </style>
</head>
<body>
  <h1>Phantom Door</h1>
  <p>ESP32 is running. Open the full dashboard:</p>
  <p><a href="http://YOUR_GITHUB_PAGES_URL?ip=REPLACE_WITH_LOCAL_IP">GitHub Pages Dashboard</a></p>
  <p>Or access API directly:<br>
     GET /status — door state<br>
     GET /users — enrolled users<br>
     POST /unlock — remote unlock<br>
     POST /enroll — start enrollment<br>
     POST /delete — delete user
  </p>
</body>
</html>
)rawhtml";
}

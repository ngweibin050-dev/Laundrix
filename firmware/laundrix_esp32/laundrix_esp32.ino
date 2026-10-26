/*
 * LAUNDRIX IoT v2.4.0 - Power-Resilient + Web Dashboard
 * 
 * FEATURES:
 * - Web dashboard on port 80 (view logs, sensors, control lock/buzzer)
 * - Conditional load cell tare (no drift on reboot with clothes inside)
 * - Extended WiFi retry (handles router lag)
 * - NO LED code (removed completely)
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <FirebaseESP32.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HX711.h>
#include <EEPROM.h>
#include <time.h>
#include <ESP32Servo.h>
#include <DFRobotDFPlayerMini.h>

// =========================================================
// CONFIGURATION
// Credentials live in secrets.h (git-ignored). Copy secrets.example.h to secrets.h and fill it in.
// =========================================================
#include "secrets.h"
#define WIFI_SSID        SECRET_WIFI_SSID
#define WIFI_PASSWORD    SECRET_WIFI_PASSWORD
#define API_KEY          SECRET_API_KEY
#define DATABASE_URL     SECRET_DATABASE_URL
#define USER_EMAIL       SECRET_USER_EMAIL
#define USER_PASSWORD    SECRET_USER_PASSWORD
#define MACHINE_ID       "M001"

// =========================================================
// WEB SERVER
// =========================================================
WebServer server(80);

// Circular buffer for serial logs (store last 50 lines for web viewing)
#define LOG_BUFFER_SIZE 50
#define LOG_LINE_LENGTH 100
char logBuffer[LOG_BUFFER_SIZE][LOG_LINE_LENGTH];
int logIndex = 0;
bool logBufferFull = false;

// =========================================================
// FIREBASE OBJECTS
// =========================================================
FirebaseData firebaseData;
FirebaseData streamData;
FirebaseAuth auth;
FirebaseConfig config;

// =========================================================
// PINS (REMOVED: LED_PIN)
// =========================================================
#define HX711_DT_PIN      16
#define HX711_SCK_PIN     17
#define VIBRATION_AO_PIN  34
#define OLED_SDA          21
#define OLED_SCL          22
#define SCREEN_WIDTH     128
#define SCREEN_HEIGHT     64
#define OLED_RESET        -1
#define OLED_ADDRESS    0x3C
#define DF_RX_PIN        19
#define DF_TX_PIN        18
#define DF_VOLUME        30
#define SERVO_PIN         26
#define LOCK_ANGLE        90
#define UNLOCK_ANGLE       0

// =========================================================
// AUDIO TRACKS
// =========================================================
#define AUDIO_ALARM       1
#define AUDIO_LOCK        2
#define AUDIO_DONE        3
#define AUDIO_STARTUP     4

// =========================================================
// THRESHOLDS
// =========================================================
#define LOAD_HIGH_KG         2.0f
#define LOAD_LOW_KG          0.5f
#define VIB_HIGH_PCT           10      // In Use → Active trigger
#define VIB_LOW_PCT             2      // Active → Clothes Inside trigger
#define VIB_NOISE_FLOOR         2
#define VIB_ATTACK_ALPHA      0.6f
#define VIB_DECAY_ALPHA      0.15f
#define CONTACT_MIC_SAMPLE_RATE  2000
#define CONTACT_MIC_WINDOW_MS     100
#define CONTACT_MIC_SAMPLES       200
#define INUSE_TO_ACTIVE_MS    5000UL
#define ACTIVE_TO_CLOTHES_MS  5000UL
#define STATE_READ_INTERVAL    200UL   // Read state from Firebase every 200ms
#define SENSOR_WRITE_INTERVAL  200UL   // Write sensor data every 200ms
#define CONTROL_INTERVAL      2000UL
#define DISPLAY_UPDATE_MS      200UL
#define STARTUP_AUDIO_MS      8000UL
#define ALARM_TRACK_MS        4000UL
#define WASH_CYCLE_DURATION_SEC  2700
#define UNAUTHORIZED_WARNING_SEC   60

// =========================================================
// EEPROM
// =========================================================
#define EEPROM_SIZE          64
#define EEPROM_MAGIC_ADDR     0
#define EEPROM_FACTOR_ADDR    4
#define EEPROM_OFFSET_ADDR    8
#define EEPROM_MAGIC_VALUE 0xCAFE
#define DEFAULT_CAL_FACTOR 18100.0f

// =========================================================
// MACHINE STATES
// =========================================================
enum MachineState {
  STATE_AVAILABLE,
  STATE_IN_USE,
  STATE_ACTIVE,
  STATE_CLOTHES_INSIDE,
  STATE_UNAUTHORIZED
};

const char* stateNames[] = {
  "Available", "In Use", "Active", "Clothes Inside", "Unauthorized Use"
};

// =========================================================
// HARDWARE OBJECTS
// =========================================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
HX711 scale;
HardwareSerial dfSerial(1);
DFRobotDFPlayerMini dfPlayer;
Servo lockServo;

// =========================================================
// STATE VARIABLES
// =========================================================
MachineState currentState    = STATE_AVAILABLE;
MachineState previousState   = STATE_AVAILABLE;
MachineState preUnauthorizedState = STATE_AVAILABLE;

volatile bool firebaseBuzzerState = false;
volatile bool firebaseLocked      = false;
bool prevBuzzerState     = false;
bool prevFirebaseLocked  = false;

float currentLoad        = 0.0f;
int   vibrationPercent   = 0;
int   vibrationRaw       = 0;
float vibrationSmoothed  = 0.0f;
float vibrationRMS       = 0.0f;

static int contactMicBuffer[CONTACT_MIC_SAMPLES];

float calibrationFactor = DEFAULT_CAL_FACTOR;
long  zeroOffset        = 0;

bool firebaseReady    = false;
bool oledReady        = false;
bool dfPlayerReady    = false;

unsigned long lastStateRead        = 0;
unsigned long lastSensorWrite      = 0;
unsigned long lastControlWrite     = 0;
unsigned long lastDisplay          = 0;
unsigned long inUseToActiveStart   = 0;
unsigned long activeToClothesStart = 0;
unsigned long washStartTime        = 0;
unsigned long estimatedEndTime     = 0;
unsigned long unauthorizedStartTime = 0;

int  animFrame        = 0;
int  warningCountdown = 0;
int  currentServoAngle = UNLOCK_ANGLE;

bool systemReady         = false;
bool startupAudioComplete = false;
unsigned long startupAudioStart = 0;

bool     audioLooping = false;
uint8_t  loopTrack    = 0;
unsigned long lastLoopPlay = 0;

bool processingCommand = false;
bool stateRecovered    = false;

// Cooldown after writing state to prevent immediate read-back
unsigned long lastStateWriteTime = 0;
const unsigned long STATE_WRITE_COOLDOWN = 500;

// =========================================================
// ICONS
// =========================================================
const unsigned char PROGMEM wifiIcon[] = {
  0b00000000, 0b01111110, 0b10000001, 0b00111100,
  0b01000010, 0b00011000, 0b00100100, 0b00011000
};
const unsigned char PROGMEM noWifiIcon[] = {
  0b10000001, 0b01111110, 0b10100101, 0b01011010,
  0b01011010, 0b00111100, 0b01000010, 0b10011001
};

// =========================================================
// FORWARD DECLARATIONS
// =========================================================
void streamCallback(StreamData data);
void streamTimeoutCallback(bool timeout);
void readStateFromFirebase();
void writeStateToFirebase(const char* newState);
void updateMachineState();
void applyOutputs();
void updateDisplay();
void writeSensorData();
void writeControlData();
void processSerialCommand();
void readSensors();
void readContactMicrophone();
void handleReleaseCommand();
void handleTareCommand();
void handleUserRequest(String uid);
void handleUnlockCommand();
void dismissUnauthorized();
void triggerUnauthorized(String uid);
void clearCommand(const char* cmd);
void resetAllTimers();
void recoverStateFromFirebase();
void applyStateFromString(const String& stateStr);
uint32_t getTimestamp();

void showBootScreen(const char* msg);
void initDFPlayer();
void initDisplay();
void initLoadCell();
void connectWiFi();
void syncTime();
void initFirebase();
void initWebServer();

void drawTopBar();
void drawBottomBar();
void drawAvailableScreen();
void drawInUseScreen();
void drawActiveScreen();
void drawClothesInsideScreen();
void drawUnauthorizedScreen();
void drawProgressBar(int y, unsigned long elapsed, unsigned long total, const char* label);
void drawConditionRow(int y, const char* label, float value, const char* unit,
                      bool condMet, float threshold, bool above);

// Web server handlers
void handleWebRoot();
void handleWebStatus();
void handleWebCommand();
void handleWebLogs();
void addToLog(const char* msg);
void addToLog(String msg);

// =========================================================
// LOGGING SYSTEM (for Web Dashboard)
// =========================================================
void addToLog(const char* msg) {
  strncpy(logBuffer[logIndex], msg, LOG_LINE_LENGTH - 1);
  logBuffer[logIndex][LOG_LINE_LENGTH - 1] = '\0';
  logIndex = (logIndex + 1) % LOG_BUFFER_SIZE;
  if (logIndex == 0) logBufferFull = true;
  // Also print to serial
  Serial.println(msg);
}

void addToLog(String msg) {
  addToLog(msg.c_str());
}

// =========================================================
// AUDIO
// =========================================================
void playOnce(uint8_t track) {
  if (!dfPlayerReady) return;
  dfPlayer.play(track);
  char buf[32];
  snprintf(buf, sizeof(buf), "[Audio] playOnce(%u)", track);
  addToLog(buf);
}

void startAlarmLoop(uint8_t track) {
  if (!dfPlayerReady) return;
  loopTrack    = track;
  audioLooping = true;
  dfPlayer.play(track);
  lastLoopPlay = millis();
  addToLog("[Audio] alarmLoop start");
}

void manualLoopTick() {
  if (!audioLooping || !dfPlayerReady) return;
  if (millis() - lastLoopPlay >= ALARM_TRACK_MS) {
    dfPlayer.play(loopTrack);
    lastLoopPlay = millis();
  }
}

void stopAudio() {
  if (!dfPlayerReady) return;
  dfPlayer.stop();
  audioLooping = false;
  loopTrack    = 0;
  lastLoopPlay = 0;
  addToLog("[Audio] stop");
}

// =========================================================
// SERVO
// =========================================================
void setServo(int angle) {
  if (currentServoAngle != angle) {
    lockServo.write(angle);
    currentServoAngle = angle;
  }
}

void resetAllTimers() {
  inUseToActiveStart   = 0;
  activeToClothesStart = 0;
}

// =========================================================
// STATE MANAGEMENT
// =========================================================
void readStateFromFirebase() {
  // Don't read if we just wrote state recently
  if (millis() - lastStateWriteTime < STATE_WRITE_COOLDOWN) {
    return;
  }

  String base = "/iot/" + String(MACHINE_ID);
  
  if (!Firebase.ready()) return;

  if (Firebase.getString(firebaseData, base + "/state")) {
    String stateStr = firebaseData.stringData();
    
    if (stateStr != stateNames[currentState]) {
      char buf[128];
      snprintf(buf, sizeof(buf), "[StateRead] Firebase: %s → Local: %s", 
               stateStr.c_str(), stateNames[currentState]);
      addToLog(buf);
      applyStateFromString(stateStr);
    }
  }

  // Read locked and buzzer states only
  if (Firebase.getBool(firebaseData, base + "/locked")) {
    firebaseLocked = firebaseData.boolData();
  }
  if (Firebase.getBool(firebaseData, base + "/buzzerState")) {
    firebaseBuzzerState = firebaseData.boolData();
  }
}

void writeStateToFirebase(const char* newState) {
  String base = "/iot/" + String(MACHINE_ID);
  
  if (!Firebase.ready()) return;

  FirebaseJson json;
  json.set("state", newState);
  json.set("lastStateUpdate", (int)getTimestamp());
  
  if (Firebase.updateNode(firebaseData, base, json)) {
    char buf[64];
    snprintf(buf, sizeof(buf), "[StateWrite] IoT wrote state: %s", newState);
    addToLog(buf);
    lastStateWriteTime = millis();
  } else {
    char buf[128];
    snprintf(buf, sizeof(buf), "[StateWrite] FAILED: %s", firebaseData.errorReason().c_str());
    addToLog(buf);
  }
}

void applyStateFromString(const String& stateStr) {
  if (stateStr == "Available") {
    if (currentState != STATE_AVAILABLE) {
      addToLog("[StateSync] → AVAILABLE");
      currentState       = STATE_AVAILABLE;
      previousState      = STATE_AVAILABLE;
      preUnauthorizedState = STATE_AVAILABLE;
      resetAllTimers();
      washStartTime      = 0;
      estimatedEndTime   = 0;
      stopAudio();
    }
  }
  else if (stateStr == "In Use") {
    if (currentState == STATE_AVAILABLE) {
      addToLog("[StateSync] → IN_USE");
      currentState  = STATE_IN_USE;
      previousState = STATE_AVAILABLE;
      resetAllTimers();
    }
  }
  else if (stateStr == "Active") {
    if (currentState == STATE_IN_USE) {
      addToLog("[StateSync] → ACTIVE");
      currentState     = STATE_ACTIVE;
      previousState    = STATE_IN_USE;
      washStartTime    = millis();
      estimatedEndTime = getTimestamp() + WASH_CYCLE_DURATION_SEC;
      resetAllTimers();
    }
  }
  else if (stateStr == "Clothes Inside") {
    if (currentState == STATE_ACTIVE) {
      addToLog("[StateSync] → CLOTHES_INSIDE");
      currentState     = STATE_CLOTHES_INSIDE;
      previousState    = STATE_ACTIVE;
      estimatedEndTime = 0;
      playOnce(AUDIO_DONE);
      resetAllTimers();
    }
  }
  else if (stateStr == "Unauthorized Use") {
    if (currentState != STATE_UNAUTHORIZED) {
      preUnauthorizedState = currentState;
      currentState = STATE_UNAUTHORIZED;
      unauthorizedStartTime = millis();
      warningCountdown = UNAUTHORIZED_WARNING_SEC;
    }
  }
}

// =========================================================
// WEB SERVER HANDLERS
// =========================================================
void handleWebRoot() {
  String html = R"(
<!DOCTYPE html>
<html>
<head>
  <meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1.0'>
  <title>LAUNDRIX )" + String(MACHINE_ID) + R"( Control</title>
  <style>
    body { font-family: Arial; margin: 0; padding: 20px; background: #1a1a2e; color: #eee; }
    h1 { color: #16c79a; border-bottom: 2px solid #16c79a; padding-bottom: 10px; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; max-width: 800px; margin: 0 auto; }
    .card { background: #16213e; padding: 20px; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }
    .status { font-size: 24px; font-weight: bold; color: #16c79a; }
    .metric { display: flex; justify-content: space-between; margin: 10px 0; padding: 10px; background: #0f3460; border-radius: 5px; }
    button { width: 100%; padding: 15px; margin: 5px 0; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; transition: all 0.3s; }
    .btn-primary { background: #e94560; color: white; }
    .btn-primary:hover { background: #ff6b6b; }
    .btn-secondary { background: #533483; color: white; }
    .btn-secondary:hover { background: #7b2cbf; }
    .btn-success { background: #16c79a; color: white; }
    .btn-success:hover { background: #13b386; }
    .logs { font-family: monospace; font-size: 12px; background: #000; padding: 10px; border-radius: 5px; height: 200px; overflow-y: auto; white-space: pre-wrap; }
    .indicator { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }
    .indicator.on { background: #16c79a; box-shadow: 0 0 10px #16c79a; }
    .indicator.off { background: #e94560; }
    @media (max-width: 600px) { .grid { grid-template-columns: 1fr; } }
  </style>
</head>
<body>
  <h1>🏪 LAUNDRIX )" + String(MACHINE_ID) + R"( Dashboard</h1>
  <div class='grid'>
    <div class='card'>
      <h3>📊 Current Status</h3>
      <div class='status' id='state'>Loading...</div>
      <div class='metric'><span>Load:</span><span id='load'>-- kg</span></div>
      <div class='metric'><span>Vibration:</span><span id='vib'>-- %</span></div>
      <div class='metric'><span>Lock:</span><span id='lock'>--</span></div>
      <div class='metric'><span>Buzzer:</span><span id='buzzer'>--</span></div>
      <div class='metric'><span>WiFi RSSI:</span><span id='wifi'>-- dBm</span></div>
      <div class='metric'><span>Uptime:</span><span id='uptime'>--</span></div>
    </div>
    
    <div class='card'>
      <h3>🎛️ Quick Controls</h3>
      <button class='btn-primary' onclick='sendCmd("unlock")'>🔓 Unlock Door</button>
      <button class='btn-primary' onclick='sendCmd("lock")'>🔒 Lock Door</button>
      <button class='btn-secondary' onclick='sendCmd("tare")'>⚖️ Tare Scale</button>
      <button class='btn-secondary' onclick='sendCmd("release")'>✅ Release Machine</button>
      <button class='btn-success' onclick='sendCmd("dismiss")'>🔕 Dismiss Alarm</button>
      <button class='btn-success' onclick='sendCmd("recalibrate")'>🔄 Recalibrate Scale</button>
    </div>
    
    <div class='card' style='grid-column: 1 / -1;'>
      <h3>📜 System Logs <button onclick='loadLogs()' style='width: auto; padding: 5px 10px; float: right;'>Refresh</button></h3>
      <div class='logs' id='logs'>Loading logs...</div>
    </div>
  </div>

  <script>
    function updateStatus() {
      fetch('/api/status')
        .then(r => r.json())
        .then(data => {
          document.getElementById('state').textContent = data.state;
          document.getElementById('load').textContent = data.load + ' kg';
          document.getElementById('vib').textContent = data.vibration + ' %';
          document.getElementById('lock').innerHTML = (data.locked ? '<span class="indicator on"></span>Locked' : '<span class="indicator off"></span>Unlocked');
          document.getElementById('buzzer').innerHTML = (data.buzzer ? '<span class="indicator on"></span>On' : '<span class="indicator off"></span>Off');
          document.getElementById('wifi').textContent = data.wifiRSSI + ' dBm';
          document.getElementById('uptime').textContent = Math.floor(data.uptime / 60000) + ' min';
        });
    }
    
    function sendCmd(cmd) {
      fetch('/api/command?cmd=' + cmd)
        .then(r => r.text())
        .then(msg => { alert(msg); updateStatus(); });
    }
    
    function loadLogs() {
      fetch('/api/logs')
        .then(r => r.text())
        .then(text => { document.getElementById('logs').textContent = text; });
    }
    
    setInterval(updateStatus, 1000);
    updateStatus();
    loadLogs();
  </script>
</body>
</html>
)";
  server.send(200, "text/html", html);
}

void handleWebStatus() {
  String json = "{";
  json += "\"state\":\"" + String(stateNames[currentState]) + "\",";
  json += "\"load\":" + String(currentLoad, 2) + ",";
  json += "\"vibration\":" + String(vibrationPercent) + ",";
  json += "\"locked\":" + String(firebaseLocked ? "true" : "false") + ",";
  json += "\"buzzer\":" + String(firebaseBuzzerState ? "true" : "false") + ",";
  json += "\"wifiRSSI\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime\":" + String(millis());
  json += "}";
  server.send(200, "application/json", json);
}

void handleWebCommand() {
  String cmd = server.arg("cmd");
  String response;
  
  if (cmd == "unlock") {
    handleUnlockCommand();
    response = "Unlock command executed";
  }
  else if (cmd == "lock") {
    firebaseLocked = true;
    FirebaseJson json;
    json.set("locked", true);
    Firebase.updateNode(firebaseData, "/iot/" + String(MACHINE_ID), json);
    response = "Lock command executed";
  }
  else if (cmd == "tare") {
    handleTareCommand();
    response = "Tare executed";
  }
  else if (cmd == "release") {
    handleReleaseCommand();
    response = "Machine released";
  }
  else if (cmd == "dismiss") {
    dismissUnauthorized();
    clearCommand("dismissAlarm");
    response = "Alarm dismissed";
  }
  else if (cmd == "recalibrate") {
    addToLog("[Web] Force recalibration requested");
    scale.tare(15);
    zeroOffset = scale.get_offset();
    EEPROM.put(EEPROM_OFFSET_ADDR, zeroOffset);
    EEPROM.commit();
    char buf[64];
    snprintf(buf, sizeof(buf), "Recalibrated. New offset: %ld", zeroOffset);
    response = buf;
  }
  else {
    response = "Unknown command: " + cmd;
  }
  
  addToLog(("[Web] Command: " + cmd + " - " + response).c_str());
  server.send(200, "text/plain", response);
}

void handleWebLogs() {
  String logs = "=== LAUNDRIX " + String(MACHINE_ID) + " Logs ===\n";
  logs += "Uptime: " + String(millis() / 1000) + " seconds\n";
  logs += "Free Heap: " + String(ESP.getFreeHeap()) + " bytes\n\n";
  
  int start = logBufferFull ? logIndex : 0;
  int count = logBufferFull ? LOG_BUFFER_SIZE : logIndex;
  
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % LOG_BUFFER_SIZE;
    logs += String(logBuffer[idx]) + "\n";
  }
  
  server.send(200, "text/plain", logs);
}

void initWebServer() {
  server.on("/", handleWebRoot);
  server.on("/api/status", handleWebStatus);
  server.on("/api/command", handleWebCommand);
  server.on("/api/logs", handleWebLogs);
  server.begin();
  addToLog("[Web] HTTP server started on http://" + WiFi.localIP().toString());
}

// =========================================================
// SETUP
// =========================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  addToLog("\n================================");
  addToLog("  LAUNDRIX IoT v2.4.0");
  addToLog("  App handles currentUserId");
  addToLog("  Web Dashboard enabled");
  addToLog("================================\n");
  
  lockServo.attach(SERVO_PIN);
  setServo(UNLOCK_ANGLE);

  EEPROM.begin(EEPROM_SIZE);

  initDFPlayer();
  initDisplay();
  showBootScreen("Initializing...");
  delay(500);

  initLoadCell();  // Conditional tare - fixed for power resilience
  showBootScreen("Scale ready");
  delay(300);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(VIBRATION_AO_PIN, INPUT);
  addToLog("[ContactMic] PES1-Q46F3A on GPIO " + String(VIBRATION_AO_PIN));
  delay(200);

  connectWiFi();  // Extended retry for Wifi Connection - fixed for router lag
  syncTime();    // NTP Sync 
  initFirebase();
  initWebServer();  // Start web server

  // Power-loss state recovery from Firebase on startup
  recoverStateFromFirebase();

  playOnce(AUDIO_STARTUP);
  startupAudioStart = millis();
  char buf[64];
  snprintf(buf, sizeof(buf), "[Startup] Audio started — blocked for %lu ms", STARTUP_AUDIO_MS);
  addToLog(buf);

  if (oledReady) { showBootScreen("System Ready!"); delay(600); }
  addToLog("\n--- Ready! ---\n");
}

// =========================================================
// MAIN LOOP
// =========================================================
void loop() {
  unsigned long now = millis();
  
  // Handle web clients
  server.handleClient();

  if (firebaseReady && Firebase.ready()) {
    Firebase.readStream(streamData);
  }

  // Startup gate
  if (!startupAudioComplete) {
    if (now - startupAudioStart >= STARTUP_AUDIO_MS) {
      startupAudioComplete = true;
      systemReady = true;
      addToLog("[Startup] Complete — operational");
      char buf[64];
      snprintf(buf, sizeof(buf), "[Startup] State: %s", stateNames[currentState]);
      addToLog(buf);
    }
    setServo(UNLOCK_ANGLE);
    readSensors();
    if (now - lastDisplay >= DISPLAY_UPDATE_MS) {
      updateDisplay();
      animFrame++;
      lastDisplay = now;
    }
    delay(10);
    return;
  }

  // ── Normal operation ──
  
  // 1. Read state from Firebase every 200ms
  if (now - lastStateRead >= STATE_READ_INTERVAL) {
    readStateFromFirebase();
    lastStateRead = now;
  }

  // 2. Read sensors
  readSensors();
  manualLoopTick();
  
  // 3. Check for IoT-triggered transitions
  updateMachineState();
  
  // 4. Apply outputs (REMOVED LED logic)
  applyOutputs();

  // 5. Update display
  if (now - lastDisplay >= DISPLAY_UPDATE_MS) {
    updateDisplay();
    animFrame++;
    lastDisplay = now;
  }

  // 6. Write sensor data
  if (now - lastSensorWrite >= SENSOR_WRITE_INTERVAL) {
    writeSensorData();
    lastSensorWrite = now;
  }

  // 7. Write control data
  if (firebaseReady && Firebase.ready()) {
    if (now - lastControlWrite >= CONTROL_INTERVAL) {
      writeControlData();
      lastControlWrite = now;
    }
  }

  processSerialCommand();
  delay(10);
}

// =========================================================
// POWER LOSS RECOVERY
// =========================================================
void recoverStateFromFirebase() {
  addToLog("[Recovery] Reading state from Firebase...");
  String base = "/iot/" + String(MACHINE_ID);

  if (!Firebase.ready()) {
    addToLog("[Recovery] Firebase not ready — defaulting to Available");
    currentState = STATE_AVAILABLE;
    return;
  }

  if (Firebase.getString(firebaseData, base + "/state")) {
    String savedState = firebaseData.stringData();
    char buf[128];
    snprintf(buf, sizeof(buf), "[Recovery] State from DB: '%s'", savedState.c_str());
    addToLog(buf);

    applyStateFromString(savedState);
    stateRecovered = true;

    if (currentState == STATE_ACTIVE) {
      if (Firebase.getInt(firebaseData, base + "/estimatedEndTime")) {
        estimatedEndTime = (unsigned long)firebaseData.intData();
      }
    }

    if (Firebase.getBool(firebaseData, base + "/locked")) {
      firebaseLocked = firebaseData.boolData();
      prevFirebaseLocked = firebaseLocked;
    }

    if (Firebase.getBool(firebaseData, base + "/buzzerState")) {
      firebaseBuzzerState = firebaseData.boolData();
      prevBuzzerState = firebaseBuzzerState;
    }

    snprintf(buf, sizeof(buf), "[Recovery] *** RESUMED: %s ***", stateNames[currentState]);
    addToLog(buf);
  } else {
    char buf[128];
    snprintf(buf, sizeof(buf), "[Recovery] Read failed: %s", firebaseData.errorReason().c_str());
    addToLog(buf);
    currentState = STATE_AVAILABLE;
  }

  // Clear stale commands
  FirebaseJson cmdJson;
  cmdJson.set("unlock", false);
  cmdJson.set("release", false);
  cmdJson.set("dismissAlarm", false);
  cmdJson.set("tare", false);
  Firebase.updateNode(firebaseData, base + "/commands", cmdJson);
}

// =========================================================
// STATE MACHINE
// =========================================================
void updateMachineState() {
  unsigned long now = millis();
  previousState = currentState;

  if (currentState == STATE_UNAUTHORIZED) return;

  bool loadHigh = (currentLoad >= LOAD_HIGH_KG);
  bool vibHigh  = (vibrationPercent >= VIB_HIGH_PCT);
  bool vibLow   = (vibrationPercent <= VIB_LOW_PCT);

  switch (currentState) {

    case STATE_AVAILABLE:
      break;

    case STATE_IN_USE:
      if (loadHigh && vibHigh) {
        if (inUseToActiveStart == 0) {
          inUseToActiveStart = now;
          addToLog("[State] IN_USE→ACTIVE timer started");
        }
        if ((now - inUseToActiveStart) >= INUSE_TO_ACTIVE_MS) {
          currentState = STATE_ACTIVE;
          inUseToActiveStart = 0;
          washStartTime = now;
          estimatedEndTime = getTimestamp() + WASH_CYCLE_DURATION_SEC;
          
          writeStateToFirebase("Active");
          
          addToLog(">> TRANSITION: IN_USE → ACTIVE (IoT wrote to Firebase)");
        }
      } else {
        inUseToActiveStart = 0;
      }
      break;

    case STATE_ACTIVE:
      if (loadHigh && vibLow) {
        if (activeToClothesStart == 0) {
          activeToClothesStart = now;
          addToLog("[State] ACTIVE→CLOTHES_INSIDE timer started");
        }
        if ((now - activeToClothesStart) >= ACTIVE_TO_CLOTHES_MS) {
          currentState = STATE_CLOTHES_INSIDE;
          activeToClothesStart = 0;
          estimatedEndTime = 0;
          
          writeStateToFirebase("Clothes Inside");
          playOnce(AUDIO_DONE);
          
          addToLog(">> TRANSITION: ACTIVE → CLOTHES_INSIDE (IoT wrote to Firebase)");
        }
      } else {
        activeToClothesStart = 0;
      }
      break;

    case STATE_CLOTHES_INSIDE:
      break;

    default:
      break;
  }
}

// =========================================================
// WRITE SENSOR DATA
// =========================================================
void writeSensorData() {
  float load = (isnan(currentLoad) || isinf(currentLoad)) ? 0.0f : currentLoad;
  String base = "/iot/" + String(MACHINE_ID);

  FirebaseJson json;
  json.set("load", load);
  json.set("vibration", vibrationPercent);
  json.set("vibrationRMS", vibrationRMS);
  json.set("lastSensorUpdate", (int)getTimestamp());

  if (Firebase.ready()) {
    Firebase.updateNode(firebaseData, base, json);
  }
}

// =========================================================
// WRITE CONTROL DATA
// =========================================================
void writeControlData() {
  String base = "/iot/" + String(MACHINE_ID);

  FirebaseJson json;
  json.set("isLive", true);
  json.set("lastPing", (int)getTimestamp());

  if (estimatedEndTime > 0) {
    json.set("estimatedEndTime", (int)estimatedEndTime);
  }

  if (Firebase.ready()) {
    Firebase.updateNode(firebaseData, base, json);
  }
}

// =========================================================
// FIREBASE INIT
// =========================================================
void initFirebase() {
  addToLog("[Firebase] Initializing v4.4.17...");

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  config.timeout.serverResponse = 1000;
  config.timeout.socketConnection = 1000;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Firebase.setReadTimeout(firebaseData, 1000);
  Firebase.setwriteSizeLimit(firebaseData, "tiny");

  String machinePath = "/iot/" + String(MACHINE_ID);
  if (Firebase.beginStream(streamData, machinePath)) {
    addToLog("[Firebase] Stream started");
    Firebase.setStreamCallback(streamData, streamCallback, streamTimeoutCallback);
  } else {
    char buf[128];
    snprintf(buf, sizeof(buf), "[Firebase] Stream failed: %s", streamData.errorReason().c_str());
    addToLog(buf);
  }

  firebaseReady = true;
  addToLog("[Firebase] Ready");
}

// =========================================================
// STREAM CALLBACK
// =========================================================
void streamCallback(StreamData data) {
  if (!systemReady) return;

  String path = data.dataPath();
  String dataStr = data.stringData();

  if (path == "/commands/unlock" && (dataStr == "true" || dataStr == "1")) {
    processingCommand = true;
    handleUnlockCommand();
    processingCommand = false;
  }
  else if (path == "/commands/release" && (dataStr == "true" || dataStr == "1")) {
    processingCommand = true;
    handleReleaseCommand();
    processingCommand = false;
  }
  else if (path == "/commands/dismissAlarm" && (dataStr == "true" || dataStr == "1")) {
    processingCommand = true;
    dismissUnauthorized();
    clearCommand("dismissAlarm");
    processingCommand = false;
  }
  else if (path == "/commands/tare" && (dataStr == "true" || dataStr == "1")) {
    processingCommand = true;
    handleTareCommand();
    processingCommand = false;
  }
  else if (path == "/commands/requestUserId" && dataStr.length() > 0 && dataStr != "null") {
    processingCommand = true;
    handleUserRequest(dataStr);
    processingCommand = false;
  }
}

void streamTimeoutCallback(bool timeout) {
  if (!timeout) return;
  addToLog("[Firebase] Stream timeout — reconnecting...");
  String machinePath = "/iot/" + String(MACHINE_ID);
  Firebase.beginStream(streamData, machinePath);
  Firebase.setStreamCallback(streamData, streamCallback, streamTimeoutCallback);
}

// =========================================================
// COMMANDS
// =========================================================
void handleUnlockCommand() {
  firebaseLocked = false;
  FirebaseJson json;
  json.set("locked", false);
  Firebase.updateNode(firebaseData, "/iot/" + String(MACHINE_ID), json);
  clearCommand("unlock");
}

void handleReleaseCommand() {
  currentState = STATE_AVAILABLE;
  previousState = STATE_CLOTHES_INSIDE;
  preUnauthorizedState = STATE_AVAILABLE;
  resetAllTimers();
  stopAudio();
  washStartTime = 0;
  estimatedEndTime = 0;

  FirebaseJson json;
  json.set("buzzerState", false);
  json.set("locked", false);
  Firebase.updateNode(firebaseData, "/iot/" + String(MACHINE_ID), json);

  clearCommand("release");
}

void handleTareCommand() {
  addToLog(">> Tare... ");
  if (scale.is_ready()) {
    scale.tare(10);
    zeroOffset = scale.get_offset();
    EEPROM.put(EEPROM_MAGIC_ADDR, (uint16_t)EEPROM_MAGIC_VALUE);
    EEPROM.put(EEPROM_OFFSET_ADDR, zeroOffset);
    EEPROM.commit();
    addToLog("OK");
  } else {
    addToLog("FAIL");
  }
  clearCommand("tare");
}

void handleUserRequest(String uid) {
  String base = "/iot/" + String(MACHINE_ID);

  if (currentState == STATE_AVAILABLE) {
    currentState = STATE_IN_USE;
    previousState = STATE_AVAILABLE;
    resetAllTimers();
    
    FirebaseJson json;
    json.set("locked", true);
    Firebase.updateNode(firebaseData, base, json);
    
    addToLog(">> User request: AVAILABLE → IN_USE");
  } else if (uid == "owner") {
    FirebaseJson json;
    json.set("locked", false);
    Firebase.updateNode(firebaseData, base, json);
    addToLog(">> Owner unlock");
  } else {
    triggerUnauthorized(uid);
    addToLog(">> UNAUTHORIZED");
  }

  FirebaseJson cmdJson;
  cmdJson.set("requestUserId", "");
  Firebase.updateNode(firebaseData, base + "/commands", cmdJson);
}

void triggerUnauthorized(String uid) {
  char buf[64];
  snprintf(buf, sizeof(buf), ">> UNAUTHORIZED: %s", uid.c_str());
  addToLog(buf);
  preUnauthorizedState = currentState;
  currentState = STATE_UNAUTHORIZED;
  unauthorizedStartTime = millis();
  warningCountdown = UNAUTHORIZED_WARNING_SEC;
}

void dismissUnauthorized() {
  if (currentState == STATE_UNAUTHORIZED) {
    currentState = preUnauthorizedState;
    addToLog(">> Alarm dismissed");
  }
}

void clearCommand(const char* cmd) {
  String path = "/iot/" + String(MACHINE_ID) + "/commands";
  FirebaseJson json;
  json.set(cmd, false);
  Firebase.updateNode(firebaseData, path, json);
}

// =========================================================
// OUTPUTS (REMOVED LED LOGIC)
// =========================================================
void applyOutputs() {
  bool lk = firebaseLocked;
  bool bz = firebaseBuzzerState;

  setServo(lk ? LOCK_ANGLE : UNLOCK_ANGLE);

  if (bz != prevBuzzerState) {
    if (bz) {
      startAlarmLoop(AUDIO_ALARM);
    } else {
      stopAudio();
    }
    prevBuzzerState = bz;
  }

  if (!bz && (lk != prevFirebaseLocked)) {
    playOnce(AUDIO_LOCK);
    prevFirebaseLocked = lk;
  }
}

// =========================================================
// HARDWARE INIT
// =========================================================
void initDFPlayer() {
  Serial.print("[DFPlayer] Init... ");
  dfSerial.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
  delay(600);
  if (!dfPlayer.begin(dfSerial, false, true)) {
    Serial.println("FAIL");
    dfPlayerReady = false;
    return;
  }
  dfPlayer.outputDevice(DFPLAYER_DEVICE_SD);
  dfPlayer.volume(DF_VOLUME);
  dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
  dfPlayerReady = true;
  Serial.printf("OK vol=%d\n", DF_VOLUME);
}

void initDisplay() {
  Serial.print("[OLED] Init... ");
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(100000);
  delay(100);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("FAIL");
    oledReady = false;
    return;
  }
  oledReady = true;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.display();
  Serial.println("OK");
}

void showBootScreen(const char* msg) {
  if (!oledReady) return;
  display.clearDisplay();
  display.fillRect(0, 0, 128, 14, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 3);
  display.print("LAUNDRIX ");
  display.print(MACHINE_ID);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(14, 20);
  display.print("Smart Laundry v2.4.0");
  display.drawLine(8, 30, 120, 30, SSD1306_WHITE);
  int len = strlen(msg) * 6;
  display.setCursor((128 - len) / 2, 38);
  display.print(msg);
  display.display();
}

// =========================================================
// FIX 1: CONDITIONAL TARE (No drift on reboot)
// =========================================================
void initLoadCell() {
  Serial.print("[Scale] Init... ");
  addToLog("[Scale] Initializing...");
  scale.begin(HX711_DT_PIN, HX711_SCK_PIN);

  // ✅ WARM-UP: Discard first readings — HX711 needs time to settle
  addToLog("[Scale] Warm-up: discarding first 5 readings...");
  for (int i = 0; i < 5; i++) {
    if (scale.is_ready()) scale.read();
    delay(500);  // 500ms × 5 = 2.5 saat warm-up
  }
  addToLog("[Scale] Warm-up complete");
  
  uint16_t magic = 0;
  EEPROM.get(EEPROM_MAGIC_ADDR, magic);
  
  if (magic == EEPROM_MAGIC_VALUE) {
    // === RESTORE FROM NVM (Normal reboot) ===
    EEPROM.get(EEPROM_FACTOR_ADDR, calibrationFactor);
    EEPROM.get(EEPROM_OFFSET_ADDR, zeroOffset);
    
    if (isnan(calibrationFactor) || calibrationFactor <= 0)
      calibrationFactor = DEFAULT_CAL_FACTOR;
    
    scale.set_scale(calibrationFactor);
    scale.set_offset(zeroOffset);  // CRITICAL: Use saved offset, don't tare!
    
    char buf[64];
    snprintf(buf, sizeof(buf), "[Scale] Restored from NVM (offset: %ld)", zeroOffset);
    addToLog(buf);
    Serial.println("OK (restored)");
  } else {
    // === FIRST BOOT EVER ===
    Serial.println("First boot - calibrating...");
    addToLog("[Scale] First boot - performing initial tare");
    calibrationFactor = DEFAULT_CAL_FACTOR;
    scale.set_scale(calibrationFactor);
    scale.tare(15);  // Only tare on virgin boot!
    zeroOffset = scale.get_offset();
    
    // Persist to NVM
    EEPROM.put(EEPROM_MAGIC_ADDR, (uint16_t)EEPROM_MAGIC_VALUE);
    EEPROM.put(EEPROM_FACTOR_ADDR, calibrationFactor);
    EEPROM.put(EEPROM_OFFSET_ADDR, zeroOffset);
    EEPROM.commit();
    
    char buf[64];
    snprintf(buf, sizeof(buf), "[Scale] Initial calibration done (offset: %ld)", zeroOffset);
    addToLog(buf);
    Serial.println("OK (initial)");
  }
}

// =========================================================
// FIX 2: EXTENDED WIFI RETRY (Handles router lag)
// =========================================================
void connectWiFi() {
  Serial.print("[WiFi] Connecting");
  addToLog("[WiFi] Starting connection...");
  if (oledReady) showBootScreen("Waiting for WiFi...");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  const int MAX_ATTEMPTS = 300;  // 300 × 1s = 5 minutes
  
  while (WiFi.status() != WL_CONNECTED && attempts < MAX_ATTEMPTS) {
    delay(1000);
    Serial.print(".");
    
    // Update display every 10 seconds
    if (oledReady && attempts % 10 == 0) {
      char msg[32];
      snprintf(msg, sizeof(msg), "WiFi... %ds", attempts);
      showBootScreen(msg);
    }
    
    attempts++;
    
    // Restart WiFi stack every 60 seconds
    if (attempts % 60 == 0) {
      addToLog("[WiFi] Resetting WiFi stack...");
      WiFi.disconnect();
      delay(1000);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    char buf[128];
    snprintf(buf, sizeof(buf), "[WiFi] Connected! IP: %s", WiFi.localIP().toString().c_str());
    addToLog(buf);
    Serial.printf(" OK (%s)\n", WiFi.localIP().toString().c_str());
    if (oledReady) showBootScreen("WiFi Connected!");
    delay(500);
  } else {
    addToLog("[WiFi] Failed after 5min - restarting...");
    Serial.println(" FAIL! Restarting...");
    if (oledReady) {
      display.clearDisplay();
      display.setCursor(0, 20);
      display.print("WiFi Failed!");
      display.setCursor(0, 40);
      display.print("Restarting...");
      display.display();
      delay(2000);
    }
    ESP.restart();
  }
}

void syncTime() {
  Serial.print("[NTP] Sync... ");
  configTime(8 * 3600, 0, "pool.ntp.org");
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 10) { delay(500); attempts++; }
  if (getLocalTime(&timeinfo)) {
    char buf[20]; strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    Serial.printf("OK (%s)\n", buf);
  } else {
    Serial.println("FAIL");
  }
}

// =========================================================
// SENSORS
// =========================================================
void readContactMicrophone() {
  unsigned long samplePeriod = 1000000 / CONTACT_MIC_SAMPLE_RATE;

  for (int i = 0; i < CONTACT_MIC_SAMPLES; i++) {
    unsigned long t0 = micros();
    contactMicBuffer[i] = analogRead(VIBRATION_AO_PIN);  // raw ADC values
    while (micros() - t0 < samplePeriod) {}
  }

  long sum = 0;
  for (int i = 0; i < CONTACT_MIC_SAMPLES; i++) sum += contactMicBuffer[i];
  float dc = (float)sum / CONTACT_MIC_SAMPLES;

  float ss = 0;
  for (int i = 0; i < CONTACT_MIC_SAMPLES; i++) {
    float d = contactMicBuffer[i] - dc;
    ss += d * d;
  }
  float rms = sqrt(ss / CONTACT_MIC_SAMPLES); // concept of RMS
  vibrationRMS = rms;

  float instant = (rms / 500.0f) * 100.0f;
  if (instant < VIB_NOISE_FLOOR) instant = 0;
  if (instant > 100) instant = 100;

  vibrationRaw = (int)rms;

  float alpha = (instant > vibrationSmoothed) ? VIB_ATTACK_ALPHA : VIB_DECAY_ALPHA;  // filtering algorithm
  vibrationSmoothed = alpha * instant + (1.0f - alpha) * vibrationSmoothed;
  vibrationPercent = constrain((int)(vibrationSmoothed + 0.5f), 0, 100);
}

void readSensors() {
  if (scale.is_ready()) {
    float r = scale.get_units(1);
    currentLoad = (isnan(r) || isinf(r) || r < 0) ? 0.0f : r;
  }
  readContactMicrophone();
}

uint32_t getTimestamp() {
  time_t now;
  time(&now);
  return (uint32_t)now;
}

// =========================================================
// DISPLAY
// =========================================================
void drawProgressBar(int y, unsigned long elapsed, unsigned long total, const char* label) {
  int barW = constrain((int)((float)elapsed / total * 86), 0, 86);
  int secs = (int)(elapsed / 1000);
  display.setTextSize(1);
  display.setCursor(0, y);
  display.print(label);
  display.drawRect(0, y + 9, 88, 6, SSD1306_WHITE);
  display.fillRect(1, y + 10, barW, 4, SSD1306_WHITE);
  display.setCursor(91, y + 9);
  display.printf("%d/5s", secs);
}

void drawConditionRow(int y, const char* label, float value, const char* unit,
                      bool condMet, float threshold, bool above) {
  display.setCursor(0, y);
  display.printf("%-4s %5.2f%s", label, value, unit);
  display.setCursor(72, y);
  display.printf("%s%.1f", above ? ">=" : "<=", threshold);
  display.setCursor(115, y);
  if (condMet) {
    display.fillRect(115, y, 6, 7, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(116, y + 1);
    display.print("v");
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.drawRect(115, y, 6, 7, SSD1306_WHITE);
    display.setCursor(116, y + 1);
    display.print("x");
  }
}

void drawTopBar() {
  display.fillRect(0, 0, 30, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 2);
  display.print(MACHINE_ID);
  display.setTextColor(SSD1306_WHITE);
  display.drawLine(31, 0, 31, 10, SSD1306_WHITE);

  struct tm ti;
  if (getLocalTime(&ti)) {
    char buf[6]; strftime(buf, sizeof(buf), "%H:%M", &ti);
    display.setCursor(36, 2);
    display.print(buf);
  }

  if (audioLooping) display.fillCircle(86, 5, 3, SSD1306_WHITE);
  else              display.drawCircle(86, 5, 3, SSD1306_WHITE);

  bool lk = firebaseLocked;
  if (lk) {
    display.fillRect(94, 1, 8, 8, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(96, 2); display.print("L");
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.drawRect(94, 1, 8, 8, SSD1306_WHITE);
    display.setCursor(96, 2); display.print("L");
  }

  if (WiFi.status() == WL_CONNECTED)
    display.drawBitmap(117, 1, wifiIcon,   8, 8, SSD1306_WHITE);
  else
    display.drawBitmap(117, 1, noWifiIcon, 8, 8, SSD1306_WHITE);

  display.drawLine(0, 11, 127, 11, SSD1306_WHITE);
}

void drawBottomBar() {
  display.drawLine(0, 53, 127, 53, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 55);
  display.printf("W:%4.2fkg", currentLoad);
  display.setCursor(50, 55);
  display.print("V:");
  display.drawRect(62, 55, 36, 7, SSD1306_WHITE);
  int barW = map(vibrationPercent, 0, 100, 0, 34);
  display.fillRect(63, 56, barW, 5, SSD1306_WHITE);
  display.setCursor(101, 55);
  display.printf("%3d%%", vibrationPercent);
}

void drawAvailableScreen() {
  display.fillRect(0, 13, 128, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(4, 15);
  display.print("   STATUS: AVAILABLE  ");
  display.setTextColor(SSD1306_WHITE);
  display.drawRect(4, 27, 10, 10, SSD1306_WHITE);
  display.drawLine(6, 32, 8, 34, SSD1306_WHITE);
  display.drawLine(8, 34, 13, 28, SSD1306_WHITE);
  display.setCursor(18, 29);
  display.print("READY FOR USE");
  display.setCursor(4, 42);
  if ((animFrame / 4) % 2 == 0) display.print("> SCAN QR TO START <");
  else                           display.print("  SCAN QR TO START  ");
}

void drawInUseScreen() {
  unsigned long now = millis();
  const char spin[] = {'|', '/', '-', '\\'};
  display.fillRect(0, 13, 128, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  char banner[22];
  snprintf(banner, sizeof(banner), "[%c]  STATUS: IN USE  ", spin[(animFrame / 2) % 4]);
  display.setCursor(2, 15);
  display.print(banner);
  display.setTextColor(SSD1306_WHITE);

  bool loadOK = (currentLoad >= LOAD_HIGH_KG);
  bool vibOK  = (vibrationPercent >= VIB_HIGH_PCT);

  drawConditionRow(25, "Load", currentLoad, "kg", loadOK, LOAD_HIGH_KG, true);
  drawConditionRow(34, "Vib ", (float)vibrationPercent, "% ", vibOK, VIB_HIGH_PCT, true);

  if (inUseToActiveStart > 0 && loadOK && vibOK) {
    drawProgressBar(43, now - inUseToActiveStart, INUSE_TO_ACTIVE_MS, "Starting:");
  } else if (!loadOK && !vibOK) {
    display.setCursor(4, 44);
    display.print("Waiting for laundry...");
  } else {
    display.setCursor(4, 44);
    display.printf("Need:%s%s", !loadOK ? " Load" : "", !vibOK ? " Vib" : "");
  }
}

void drawActiveScreen() {
  unsigned long now = millis();
  const char spin[] = {'|', '/', '-', '\\'};
  display.fillRect(0, 13, 128, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  char banner[22];
  snprintf(banner, sizeof(banner), "[%c]  STATUS: ACTIVE  ", spin[(animFrame / 2) % 4]);
  display.setCursor(2, 15);
  display.print(banner);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 25);
  display.printf("Load: %.2fkg   Vib: %d%%", currentLoad, vibrationPercent);

  if (estimatedEndTime > 0) {
    unsigned long ts = getTimestamp();
    int remaining = (estimatedEndTime > ts) ? (int)(estimatedEndTime - ts) : 0;
    display.setCursor(0, 34);
    if (remaining > 0)
      display.printf("Est: %02d:%02d remaining", remaining / 60, remaining % 60);
    else
      display.print("Finishing up...");
  } else {
    display.setCursor(0, 34);
    display.print("Washing in progress...");
  }

  bool loadOK = (currentLoad >= LOAD_HIGH_KG);
  bool vibDone = (vibrationPercent <= VIB_LOW_PCT);
  if (activeToClothesStart > 0 && loadOK && vibDone) {
    drawProgressBar(43, now - activeToClothesStart, ACTIVE_TO_CLOTHES_MS, "Finishing:");
  } else if (vibrationPercent > VIB_LOW_PCT) {
    display.setCursor(0, 44);
    display.printf("Vib: %d%% (wait <=%d%%)", vibrationPercent, VIB_LOW_PCT);
  }
}

void drawClothesInsideScreen() {
  bool flash = ((animFrame / 3) % 2 == 0);
  if (flash) {
    display.fillRect(0, 13, 128, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.drawRect(0, 13, 128, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE);
  }
  display.setTextSize(1);
  display.setCursor(2, 15);
  display.print("[!]  CYCLE COMPLETE ");
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 26);
  display.print("Collect your laundry!");
  display.setCursor(0, 35);
  display.printf("Load: %.2fkg  Vib: %d%%", currentLoad, vibrationPercent);
  display.setCursor(0, 44);
  if ((animFrame / 4) % 2 == 0) display.print("> Waiting for pickup <");
  else                           display.print("  Waiting for pickup  ");
}

void drawUnauthorizedScreen() {
  bool flash = ((animFrame / 2) % 2 == 0);
  if (flash) {
    display.fillRect(0, 13, 128, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.drawRect(0, 13, 128, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE);
  }
  display.setTextSize(1);
  display.setCursor(2, 15);
  display.print("!! UNAUTHORIZED USE !!");
  display.setTextColor(SSD1306_WHITE);

  warningCountdown = UNAUTHORIZED_WARNING_SEC - (int)((millis() - unauthorizedStartTime) / 1000);
  if (warningCountdown < 0) warningCountdown = 0;

  if (warningCountdown > 0) {
    display.setCursor(2, 27);
    display.print("Alarm triggers in:");
    display.setTextSize(2);
    display.setCursor(72, 24);
    display.printf("%02d", warningCountdown);
    display.setTextSize(1);
    display.setCursor(2, 44);
    display.print("Security alert raised!");
  } else {
    display.setTextSize(2);
    if (flash) {
      display.fillRect(0, 25, 128, 20, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      display.setCursor(8, 30);
      display.print("!! ALARM !!");
      display.setTextColor(SSD1306_WHITE);
    } else {
      display.drawRect(0, 25, 128, 20, SSD1306_WHITE);
      display.setCursor(8, 30);
      display.print("!! ALARM !!");
    }
    display.setTextSize(1);
  }
}

void updateDisplay() {
  if (!oledReady) return;
  display.clearDisplay();
  drawTopBar();
  switch (currentState) {
    case STATE_AVAILABLE:      drawAvailableScreen();     break;
    case STATE_IN_USE:         drawInUseScreen();         break;
    case STATE_ACTIVE:         drawActiveScreen();        break;
    case STATE_CLOTHES_INSIDE: drawClothesInsideScreen(); break;
    case STATE_UNAUTHORIZED:   drawUnauthorizedScreen();  break;
  }
  drawBottomBar();
  display.display();
}

// =========================================================
// SERIAL COMMANDS (Updated with recalibrate)
// =========================================================
void processSerialCommand() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim(); cmd.toLowerCase();

  if (cmd == "help") {
    Serial.println("\n=== LAUNDRIX v2.4.0 ===");
    Serial.println("Architecture:");
    Serial.println("- Read state from Firebase every 200ms");
    Serial.println("- Write state ONLY on IoT-triggered transitions");
    Serial.println("- Web dashboard available at device IP");
    Serial.println("\nCommands: status, state, sensors, thresholds, reboot, recalibrate, webip");
  }
  else if (cmd == "status") {
    Serial.printf("\nState: %s | Load: %.2fkg | Vib: %d%%\n", 
                  stateNames[currentState], currentLoad, vibrationPercent);
    Serial.printf("Firebase: %s | WiFi: %s (%d dBm)\n", 
                  Firebase.ready() ? "Ready" : "Not ready",
                  WiFi.status() == WL_CONNECTED ? "OK" : "Disconnected",
                  WiFi.RSSI());
  }
  else if (cmd == "state") {
    Serial.printf("Current: %s\n", stateNames[currentState]);
  }
  else if (cmd == "sensors") {
    Serial.printf("Load: %.2fkg | Vib: %d%% | RMS: %.1f\n", 
                  currentLoad, vibrationPercent, vibrationRMS);
  }
  else if (cmd == "thresholds") {
    Serial.printf("Load High: %.1fkg | Vib High: %d%% | Vib Low: %d%%\n",
                  LOAD_HIGH_KG, VIB_HIGH_PCT, VIB_LOW_PCT);
  }
  else if (cmd == "reboot") {
    ESP.restart();
  }
  else if (cmd == "recalibrate") {
    addToLog("[Serial] Force recalibration requested");
    scale.tare(15);
    zeroOffset = scale.get_offset();
    EEPROM.put(EEPROM_OFFSET_ADDR, zeroOffset);
    EEPROM.commit();
    Serial.printf("New offset saved: %ld\n", zeroOffset);
    addToLog("[Scale] Recalibrated via serial command");
  }
  else if (cmd == "webip") {
    Serial.printf("Web dashboard: http://%s\n", WiFi.localIP().toString().c_str());
  }
}
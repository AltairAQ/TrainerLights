// ============================================================
// TrainerLights CLIENT - NodeMCU ESP-12E (V2.0)
// Authors: Khader Afeez <khaderafeez16@gmail.com>
//          Abin Abraham <abinabraham176@gmail.com>
//          James Kattoor <jameskattoor004@gmail.com>
// Fixed:   Late response sent after stop_test (resultSent guard
//          now checked against isStopped flag), pulseIn blocking
//          replaced with non-blocking echo approach via ISR timer,
//          result blink task now properly disabled on cancelStimulus
// ============================================================

#include <ESP8266WiFi.h>
#include <TaskScheduler.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <EEPROM.h>

extern "C" {
#include "user_interface.h"
}

// ---- Pin map ----
#define PIN_TRIG   D1
#define PIN_ECHO   D2
#define LED_OK     D4   // Green  — hit
#define LED_ERR    D5   // Red    — miss / timeout
#define LED_STIM   D6   // Blue   — react now
#define LED_STATUS D7   // Yellow — WiFi/WS status

const char* SSID      = "TrainerLights";
const char* PASSWORD  = "1234567890";
const char* SERVER_IP = "192.168.4.1";
const int   WS_PORT   = 81;

#define EEPROM_SIZE 8
#define ADDR_ID     0
#define ADDR_MAGIC  1
#define MAGIC_VALUE 0xAB

WebSocketsClient webSocket;
bool wsConnected = false;
bool wsStarted   = false;
int  nodeID      = 0;

// ---- Stimulus state ----
bool          active        = false;
bool          isStopped     = false;  // FIX: server sent stop_test
int           stimTimeoutMs = 1000;
int           stimDelayMs   = 0;
int           detMin        = 0;
int           detMax        = 50;
unsigned long stimOnTime    = 0;
bool          resultSent    = false;
int           resultPin     = LED_OK;

// ============================================================
// Non-blocking HC-SR04 via echo interrupt
// ============================================================
volatile unsigned long echoStart = 0;
volatile unsigned long echoDur   = 0;
volatile bool          echoReady = false;

void ICACHE_RAM_ATTR echoISR() {
  if (digitalRead(PIN_ECHO) == HIGH) {
    echoStart = micros();
  } else {
    unsigned long dur = micros() - echoStart;
    if (dur > 0 && dur < 15000) {   // valid window (< ~257cm)
      echoDur   = dur;
      echoReady = true;
    }
  }
}

void triggerPulse() {
  echoReady = false;
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
}

long getDistance() {
  if (!echoReady) return 999;
  echoReady = false;
  return (long)((echoDur * 0.0343f) / 2.0f);
}

// ============================================================
// LED helpers
// ============================================================
void setGameLEDs(int blue, int green, int red) {
  digitalWrite(LED_STIM, blue);
  digitalWrite(LED_OK,   green);
  digitalWrite(LED_ERR,  red);
}

// ============================================================
// Task Scheduler
// ============================================================
Scheduler runner;

void taskOnDelay();
void taskMeasure();
void taskOnTimeout();
void taskLedsOff();
void taskRegister();
void taskResultBlink();
void taskTrigger();   // FIX: periodic non-blocking trigger

Task tOnDelay    (0,   TASK_ONCE,    &taskOnDelay,     &runner, false);
Task tMeasure    (25,  TASK_FOREVER, &taskMeasure,     &runner, false);
Task tOnTimeout  (10000, TASK_ONCE,  &taskOnTimeout,   &runner, false);
Task tLedsOff    (300, TASK_ONCE,    &taskLedsOff,     &runner, false);
Task tRegister   (600, TASK_ONCE,    &taskRegister,    &runner, false);
Task tResultBlink(100, TASK_FOREVER, &taskResultBlink, &runner, false);
Task tTrigger    (30,  TASK_FOREVER, &taskTrigger,     &runner, false); // FIX: trigger every 30ms

// ============================================================
// EEPROM helpers
// ============================================================
int readID() {
  EEPROM.begin(EEPROM_SIZE);
  byte magic = EEPROM.read(ADDR_MAGIC);
  int  id    = (magic == MAGIC_VALUE) ? (int)EEPROM.read(ADDR_ID) : 0;
  EEPROM.end();
  return id;
}

void saveID(int id) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(ADDR_ID,    (byte)id);
  EEPROM.write(ADDR_MAGIC, MAGIC_VALUE);
  EEPROM.commit();
  EEPROM.end();
}

// ============================================================
// Cancel / send helpers
// ============================================================
void cancelStimulus() {
  active = false;
  tOnDelay.disable();
  tMeasure.disable();
  tOnTimeout.disable();
  tLedsOff.disable();
  tResultBlink.disable();   // FIX: was missing in original
  tTrigger.disable();
  setGameLEDs(LOW, LOW, LOW);
}

void sendResult(unsigned long reactionMs, int distCm, int error) {
  // FIX: gate on isStopped — discard late results after stop_test
  if (!wsConnected || resultSent || isStopped) return;
  resultSent = true;
  StaticJsonDocument<200> doc;
  doc["type"]     = "response";
  doc["node_id"]  = nodeID;
  doc["time"]     = (int)reactionMs;
  doc["distance"] = distCm;
  doc["error"]    = error;
  doc["ip"]       = WiFi.localIP().toString();
  doc["mac"]      = WiFi.macAddress();
  String msg; serializeJson(doc, msg);
  webSocket.sendTXT(msg);
}

// ============================================================
// Tasks
// ============================================================
void taskRegister() {
  if (!wsConnected) return;
  StaticJsonDocument<150> doc;
  doc["type"]    = "sensor";
  doc["ip"]      = WiFi.localIP().toString();
  doc["mac"]     = WiFi.macAddress();
  doc["node_id"] = nodeID;
  String msg; serializeJson(doc, msg);
  webSocket.sendTXT(msg);
  digitalWrite(LED_STATUS, HIGH);
}

void taskOnDelay() {
  resultSent  = false;
  stimOnTime  = millis();
  active      = true;
  setGameLEDs(HIGH, LOW, LOW);  // Blue on — react!
  tOnTimeout.setInterval(stimTimeoutMs > 100 ? stimTimeoutMs : 100);
  tOnTimeout.restartDelayed();
  tTrigger.enable();   // FIX: start non-blocking polling
  tMeasure.enable();
}

void taskTrigger() {
  // FIX: fire ultrasonic pulse on a schedule, non-blocking
  triggerPulse();
}

void taskResultBlink() {
  digitalWrite(resultPin, !digitalRead(resultPin));
}

void taskMeasure() {
  if (!active) { tMeasure.disable(); tTrigger.disable(); return; }
  long dist = getDistance();
  if (dist <= 0 || dist >= 999 || dist > detMax || dist < detMin) return;

  unsigned long reactionMs = millis() - stimOnTime;
  if (reactionMs < 10) return;  // anti-cheat

  tOnTimeout.disable();
  tMeasure.disable();
  tTrigger.disable();
  active = false;

  resultPin = LED_OK;
  setGameLEDs(LOW, LOW, LOW);
  digitalWrite(LED_OK, HIGH);
  tResultBlink.enable();
  tLedsOff.setInterval(2500);
  tLedsOff.restartDelayed();

  sendResult(reactionMs, (int)dist, 0);
}

void taskOnTimeout() {
  if (!active) return;
  tMeasure.disable();
  tTrigger.disable();
  active = false;

  resultPin = LED_ERR;
  setGameLEDs(LOW, LOW, LOW);
  digitalWrite(LED_ERR, HIGH);
  tResultBlink.enable();
  tLedsOff.setInterval(2500);
  tLedsOff.restartDelayed();

  sendResult((unsigned long)(stimDelayMs + stimTimeoutMs), 0, 1);
}

void taskLedsOff() {
  tResultBlink.disable();
  setGameLEDs(LOW, LOW, LOW);
}

// ============================================================
// Status LED (blinks when not connected)
// ============================================================
unsigned long blinkLast  = 0;
bool          blinkState = false;

void updateStatusLED() {
  if (wsConnected) return;
  unsigned long interval = (WiFi.status() == WL_CONNECTED) ? 700 : 200;
  if (millis() - blinkLast >= interval) {
    blinkLast  = millis();
    blinkState = !blinkState;
    digitalWrite(LED_STATUS, blinkState ? HIGH : LOW);
  }
}

// ============================================================
// WebSocket events
// ============================================================
void onWSEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_DISCONNECTED:
      wsConnected = false;
      digitalWrite(LED_STATUS, LOW);
      cancelStimulus();
      tRegister.disable();
      break;

    case WStype_CONNECTED:
      wsConnected = true;
      isStopped   = false;  // FIX: clear flag on fresh connect
      tRegister.restartDelayed();
      break;

    case WStype_TEXT: {
      StaticJsonDocument<300> doc;
      if (deserializeJson(doc, payload)) break;
      const char* t = doc["type"]; if (!t) break;

      if (strcmp(t, "assign_id") == 0) {
        int newID = doc["node_id"] | 0;
        if (newID > 0 && newID != nodeID) {
          nodeID = newID; saveID(nodeID); tRegister.restartDelayed();
        }
      }

      if (strcmp(t, "stimulus") == 0) {
        isStopped     = false;   // FIX: new stimulus means session is live
        stimTimeoutMs = doc["timeout"] | 1000;
        stimDelayMs   = doc["delay"]   | 0;
        detMin        = doc["min_detection_range"] | 0;
        detMax        = doc["max_detection_range"] | 50;
        if (stimTimeoutMs < 100) stimTimeoutMs = 100;
        if (stimDelayMs   < 0)   stimDelayMs   = 0;
        cancelStimulus();
        tOnDelay.setInterval(stimDelayMs > 0 ? stimDelayMs : 10);
        tOnDelay.restartDelayed();
      }

      if (strcmp(t, "stop_test") == 0) {
        // FIX: set flag so any in-flight sendResult is discarded
        isStopped = true;
        cancelStimulus();
      }

      if (strcmp(t, "blink") == 0) {
        setGameLEDs(HIGH, HIGH, HIGH);
        tLedsOff.setInterval(500);
        tLedsOff.restartDelayed();
      }

      if (strcmp(t, "restart")  == 0) { cancelStimulus(); delay(100); ESP.restart(); }
      if (strcmp(t, "reset_id") == 0) { saveID(0); nodeID = 0; delay(100); ESP.restart(); }
      break;
    }
    default: break;
  }
}

// ============================================================
// WiFi / WS init
// ============================================================
void connectWiFi() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
}

void connectWS() {
  webSocket.begin(SERVER_IP, WS_PORT, "/");
  webSocket.onEvent(onWSEvent);
  webSocket.setReconnectInterval(3000);
  webSocket.enableHeartbeat(10000, 3000, 2);
  wsStarted = true;
}

// ============================================================
// Setup / Loop
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG,   OUTPUT); pinMode(PIN_ECHO,   INPUT);
  pinMode(LED_OK,     OUTPUT); pinMode(LED_ERR,    OUTPUT);
  pinMode(LED_STIM,   OUTPUT); pinMode(LED_STATUS, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  setGameLEDs(LOW, LOW, LOW);
  digitalWrite(LED_STATUS, LOW);

  // FIX: attach echo interrupt for non-blocking distance sensing
  attachInterrupt(digitalPinToInterrupt(PIN_ECHO), echoISR, CHANGE);

  wifi_set_sleep_type(NONE_SLEEP_T);
  nodeID = readID();

  // Boot blink
  digitalWrite(LED_STATUS, HIGH); setGameLEDs(HIGH, HIGH, HIGH);
  delay(300);
  digitalWrite(LED_STATUS, LOW);  setGameLEDs(LOW, LOW, LOW);

  connectWiFi();
  connectWS();
}

void loop() {
  if (wsStarted) webSocket.loop();
  runner.execute();
  updateStatusLED();

  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 2000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED && wsConnected) {
      wsConnected = false;
      digitalWrite(LED_STATUS, LOW);
      cancelStimulus();
    }
  }
}

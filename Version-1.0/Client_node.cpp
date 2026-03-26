// TrainerLights Server
// Author: khader afeez
// khaderafeez16@gmail.com// ============================================================
// TrainerLights CLIENT - NodeMCU ESP-12E 
// FIXED: 2.5 Second FAST BLINK for Results (Hit & Miss)
// abin abraham
// abinabraham176@gmail.com//=============================
// ============================================================

#include <ESP8266WiFi.h>
#include <TaskScheduler.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <EEPROM.h>

extern "C" {
#include "user_interface.h"
}

// PINS
#define PIN_TRIG   D1  // HC-SR04 Trigger
#define PIN_ECHO   D2  // HC-SR04 Echo
#define LED_OK     D4  // Green  — success (HIT)
#define LED_ERR    D5  // Red    — missed (TIMEOUT)
#define LED_STIM   D6  // Blue   — react now (STIMULUS)
#define LED_STATUS D7  // Yellow — connection

const char* SSID      = "TrainerLights";
const char* PASSWORD  = "1234567890";
const char* SERVER_IP = "192.168.4.1";
const int   WS_PORT   = 81;

#define EEPROM_SIZE  8
#define ADDR_ID      0
#define ADDR_MAGIC   1
#define MAGIC_VALUE  0xAB

WebSocketsClient webSocket;
bool wsConnected = false;
bool wsStarted   = false;
int nodeID = 0;

bool          active         = false;
int           stimTimeoutMs  = 1000;  
int           stimDelayMs    = 0;     
int           detMin         = 0;     
int           detMax         = 50;    
unsigned long stimOnTime     = 0;     
bool          resultSent     = false; 
int           resultPin      = LED_OK; // Tracks which LED should blink

// STRICT LED CONTROLLER
void setGameLEDs(int blueStim, int greenOk, int redErr) {
  digitalWrite(LED_STIM, blueStim);
  digitalWrite(LED_OK,   greenOk);
  digitalWrite(LED_ERR,  redErr);
}

// FAST SENSOR READ (Prevents ESP Freezing)
long getFastDistance() {
  digitalWrite(PIN_TRIG, LOW); delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long dur = pulseIn(PIN_ECHO, HIGH, 15000); 
  if (dur == 0) return 999; 
  return (dur * 0.0343) / 2; 
}

unsigned long blinkLast  = 0;
bool          blinkState = false;

Scheduler runner;
void taskOnDelay();    
void taskMeasure();    
void taskOnTimeout();  
void taskLedsOff();    
void taskRegister();   
void taskResultBlink(); // NEW: Task for fast blinking

Task tOnDelay   (0,     TASK_ONCE,    &taskOnDelay,   &runner, false);
Task tMeasure   (25,    TASK_FOREVER, &taskMeasure,   &runner, false);
Task tOnTimeout (10000, TASK_ONCE,    &taskOnTimeout, &runner, false);
Task tLedsOff   (300,   TASK_ONCE,    &taskLedsOff,   &runner, false);
Task tRegister  (600,   TASK_ONCE,    &taskRegister,  &runner, false);
Task tResultBlink(100,  TASK_FOREVER, &taskResultBlink, &runner, false); // Blinks every 100ms

int readID() {
  EEPROM.begin(EEPROM_SIZE);
  byte magic = EEPROM.read(ADDR_MAGIC);
  int  id    = (magic == MAGIC_VALUE) ? (int)EEPROM.read(ADDR_ID) : 0;
  EEPROM.end();
  return id;
}

void saveID(int id) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(ADDR_ID, (byte)id);
  EEPROM.write(ADDR_MAGIC, MAGIC_VALUE);
  EEPROM.commit();
  EEPROM.end();
}

void cancelStimulus() {
  active = false;
  tOnDelay.disable(); tMeasure.disable(); tOnTimeout.disable(); 
  tLedsOff.disable(); tResultBlink.disable();
  setGameLEDs(LOW, LOW, LOW); 
}

void sendResult(unsigned long reactionMs, int distCm, int error) {
  if (!wsConnected || resultSent) return;
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
  resultSent = false; stimOnTime = millis(); active = true;
  setGameLEDs(HIGH, LOW, LOW); // BLUE ON (React!)
  tOnTimeout.setInterval(stimTimeoutMs > 100 ? stimTimeoutMs : 100);
  tOnTimeout.restartDelayed();
  tMeasure.enable();
}

// NEW: This toggles the selected LED incredibly fast
void taskResultBlink() {
  digitalWrite(resultPin, !digitalRead(resultPin));
}

void taskMeasure() {
  if (!active) { tMeasure.disable(); return; }
  long dist = getFastDistance(); 
  if (dist <= 0 || dist > detMax || dist < detMin) return; 

  unsigned long reactionMs = millis() - stimOnTime;
  if (reactionMs < 10) return; // Anti-cheat

  tOnTimeout.disable(); tMeasure.disable(); active = false;
  
  // Start the Fast Blink for GREEN (Hit)
  resultPin = LED_OK;
  digitalWrite(LED_STIM, LOW); // Blue off
  digitalWrite(LED_ERR, LOW);  // Red off
  digitalWrite(LED_OK, HIGH);  // Start Green on
  
  tResultBlink.enable();       // Start the blinking engine
  
  tLedsOff.setInterval(2500);  // Stop blinking after 2.5 seconds
  tLedsOff.restartDelayed();
  
  sendResult(reactionMs, (int)dist, 0); 
}

void taskOnTimeout() {
  if (!active) return; 
  tMeasure.disable(); active = false;
  
  // Start the Fast Blink for RED (Miss)
  resultPin = LED_ERR;
  digitalWrite(LED_STIM, LOW); // Blue off
  digitalWrite(LED_OK, LOW);   // Green off
  digitalWrite(LED_ERR, HIGH); // Start Red on
  
  tResultBlink.enable();       // Start the blinking engine
  
  tLedsOff.setInterval(2500);  // Stop blinking after 2.5 seconds
  tLedsOff.restartDelayed();
  
  sendResult((unsigned long)(stimDelayMs + stimTimeoutMs), 0, 1); 
}

void taskLedsOff() {
  tResultBlink.disable();     // Stop the blinking engine
  setGameLEDs(LOW, LOW, LOW); // Turn off cleanly
}

void updateStatusLED() {
  if (wsConnected) return; 
  unsigned long interval = (WiFi.status() == WL_CONNECTED) ? 700 : 200;
  if (millis() - blinkLast >= interval) {
    blinkLast = millis(); blinkState = !blinkState;
    digitalWrite(LED_STATUS, blinkState ? HIGH : LOW);
  }
}

void onWSEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      wsConnected = false; digitalWrite(LED_STATUS, LOW); cancelStimulus(); tRegister.disable(); break;
    case WStype_CONNECTED:
      wsConnected = true; tRegister.restartDelayed(); break;
    case WStype_TEXT: {
      StaticJsonDocument<300> doc;
      if (deserializeJson(doc, payload)) break;
      const char* t = doc["type"]; if (!t) break;

      if (strcmp(t, "assign_id") == 0) {
        int newID = doc["node_id"] | 0;
        if (newID > 0 && newID != nodeID) { nodeID = newID; saveID(nodeID); tRegister.restartDelayed(); }
      }

      if (strcmp(t, "stimulus") == 0) {
        stimTimeoutMs = doc["timeout"] | 1000; stimDelayMs = doc["delay"] | 0;
        detMin = doc["min_detection_range"] | 0; detMax = doc["max_detection_range"] | 50;
        if (stimTimeoutMs < 100) stimTimeoutMs = 100;
        if (stimDelayMs < 0) stimDelayMs = 0;
        cancelStimulus();
        tOnDelay.setInterval(stimDelayMs > 0 ? stimDelayMs : 10);
        tOnDelay.restartDelayed();
      }

      if (strcmp(t, "blink") == 0) {
        setGameLEDs(HIGH, HIGH, HIGH); 
        tLedsOff.setInterval(500); tLedsOff.restartDelayed();
      }

      if (strcmp(t, "restart") == 0) { cancelStimulus(); delay(100); ESP.restart(); }
      if (strcmp(t, "reset_id") == 0) { saveID(0); nodeID = 0; delay(100); ESP.restart(); }
      break;
    }
    default: break;
  }
}

void connectWiFi() {
  WiFi.persistent(false); WiFi.setAutoReconnect(true); WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.mode(WIFI_STA); WiFi.begin(SSID, PASSWORD);
}

void connectWS() {
  webSocket.begin(SERVER_IP, WS_PORT, "/");
  webSocket.onEvent(onWSEvent);
  webSocket.setReconnectInterval(3000);
  webSocket.enableHeartbeat(10000, 3000, 2);
  wsStarted = true;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT);
  pinMode(LED_OK, OUTPUT); pinMode(LED_ERR, OUTPUT);
  pinMode(LED_STIM, OUTPUT); pinMode(LED_STATUS, OUTPUT);
  digitalWrite(PIN_TRIG, LOW); digitalWrite(LED_STATUS, LOW);
  setGameLEDs(LOW, LOW, LOW); 
  wifi_set_sleep_type(NONE_SLEEP_T);
  nodeID = readID();

  digitalWrite(LED_STATUS, HIGH); setGameLEDs(HIGH, HIGH, HIGH);
  delay(300);
  digitalWrite(LED_STATUS, LOW); setGameLEDs(LOW, LOW, LOW);
  
  connectWiFi(); connectWS();
}

void loop() {
  if (wsStarted) webSocket.loop();
  runner.execute();
  updateStatusLED();

  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 2000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED && wsConnected) {
      wsConnected = false; digitalWrite(LED_STATUS, LOW); cancelStimulus();
    }
  }
}

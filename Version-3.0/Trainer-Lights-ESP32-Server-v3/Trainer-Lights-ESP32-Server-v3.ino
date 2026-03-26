// ============================================================
// TrainerLights SERVER - ESP32 WROOM-32D (V3.0)
// Authors: Khader Afeez <khaderafeez16@gmail.com>
//          Abin Abraham <abinabraham176@gmail.com>
// v3.0:    Phone interface migrated from WebSocket/HTTP to
//          Bluetooth Classic SPP (BluetoothSerial). Nodes still
//          communicate via Wi-Fi AP + WebSocket (unchanged).
//          JSON protocol identical — Flutter app speaks same
//          message types as the old web UI.
// ============================================================

// Node-facing WebSocket client limit
#define WEBSOCKETS_SERVER_CLIENT_MAX 12

#include <WiFi.h>
#include <WebSocketsServer.h>
#include <TaskScheduler.h>
#include <ArduinoJson.h>
#include <LinkedList.h>
#include "esp_wifi.h"
#include "BluetoothSerial.h"

// Sanity check — BT Classic requires the Classic BT stack enabled in sdkconfig
#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
  #error "Bluetooth Classic is not enabled. Enable it in Arduino ESP32 board config."
#endif

// ============================================================
// Sensor node descriptor
// ============================================================
class Sensor {
  public:
    IPAddress ip;
    String    mac;
    int       nodeID;
    int       num;      // WebSocket slot index
    bool      enabled;
};

LinkedList<Sensor*> sensorList = LinkedList<Sensor*>();

// ============================================================
// Bluetooth Serial (phone interface)
// ============================================================
BluetoothSerial BT;
bool btConnected  = false;
String btRxBuffer = "";       // accumulates incoming bytes until '\n'

// ============================================================
// Node-facing WebSocket server (Wi-Fi AP)
// ============================================================
WebSocketsServer webSocket = WebSocketsServer(81);

const char* apName     = "TrainerLights";
const char* apPassword = "1234567890";

// ============================================================
// Session state
// ============================================================
String tmode            = "random";
int min_delay           = 4000;
int max_delay           = 4000;
int mim_timeout         = 1000;
int max_timeout         = 2000;
int min_detection_range = 0;
int max_detection_range = 50;
int timeout_ms          = 1000;
int tdelay              = 0;

bool isTesting      = false;
int  currentSensor  = 0;
int  lastSensor     = -1;
bool stimulating    = false;
int  stimulatingNum = -1;

// ============================================================
// Stats
// ============================================================
int   test_score        = 0;
int   test_errors       = 0;
long  sum_response_time = 0;
long  sum_distance      = 0;
int   hit_count         = 0;
int   max_distance      = 0;
int   min_distance      = 9999;
int   max_response_time = 0;
int   min_response_time = 9999;
int   avg_response_time = 0;
int   avg_distance      = 0;

// ============================================================
// Task Scheduler
// ============================================================
Scheduler ts;
void onStimulusTimeout();
Task tStimulusTimeout(5000, TASK_ONCE, &onStimulusTimeout, &ts, false);

// Forward declarations
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
void processAppMessage(const String& raw);

// ============================================================
// Bluetooth callbacks
// ============================================================
void onBtConnect(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) {
  if (event == ESP_SPP_SRV_OPEN_EVT) {
    btConnected = true;
    Serial.println("[BT] Phone connected");
    // Treat same as old app_connected — stop any ghost test
    isTesting      = false;
    stimulating    = false;
    stimulatingNum = -1;
    tStimulusTimeout.disable();
    // Send current node list to app immediately
    sendSensorListBT();
  } else if (event == ESP_SPP_CLOSE_EVT) {
    btConnected = false;
    btRxBuffer  = "";
    Serial.println("[BT] Phone disconnected");
  }
}

// ============================================================
// Helpers — BT send
// ============================================================

// All messages to the app go through this.
// We terminate each JSON object with '\n' so Flutter can
// split the stream into discrete messages.
void sendBT(const String& msg) {
  if (!btConnected) return;
  BT.println(msg);   // println appends \r\n — Flutter trims both
}

void sendSensorListBT() {
  if (!btConnected) return;
  StaticJsonDocument<1024> doc;
  doc["type"]  = "sensor_list";
  doc["count"] = sensorList.size();
  JsonArray arr = doc.createNestedArray("sensors");
  for (int i = 0; i < sensorList.size(); i++) {
    Sensor*    s = sensorList.get(i);
    JsonObject o = arr.createNestedObject();
    o["node_id"] = s->nodeID;
    o["ip"]      = s->ip.toString();
    o["mac"]     = s->mac;
    o["index"]   = i + 1;
  }
  String msg;
  serializeJson(doc, msg);
  sendBT(msg);
}

void resetStats() {
  test_score = test_errors = hit_count = 0;
  sum_response_time = sum_distance = 0;
  max_distance = max_response_time = avg_response_time = avg_distance = 0;
  min_distance = min_response_time = 9999;
}

void sendStats() {
  if (!btConnected) return;
  StaticJsonDocument<256> doc;
  doc["type"]              = "stats";
  doc["test_score"]        = test_score;
  doc["test_errors"]       = test_errors;
  doc["max_distance"]      = max_distance;
  doc["min_distance"]      = (min_distance == 9999) ? 0 : min_distance;
  doc["avg_distance"]      = avg_distance;
  doc["max_response_time"] = max_response_time;
  doc["min_response_time"] = (min_response_time == 9999) ? 0 : min_response_time;
  doc["avg_response_time"] = avg_response_time;
  doc["is_testing"]        = isTesting;
  String msg;
  serializeJson(doc, msg);
  sendBT(msg);
}

// ============================================================
// Node list helpers
// ============================================================
int getNextNodeID() {
  int maxID = 0;
  for (int i = 0; i < sensorList.size(); i++)
    if (sensorList.get(i)->nodeID > maxID) maxID = sensorList.get(i)->nodeID;
  return maxID + 1;
}

void removeSensorAt(int index) {
  Sensor* s = sensorList.get(index);
  if (stimulating && s->num == stimulatingNum) {
    stimulating    = false;
    stimulatingNum = -1;
    tStimulusTimeout.disable();
    if (isTesting) { test_errors++; sendStats(); }
  }
  delete s;
  sensorList.remove(index);
}

// ============================================================
// Process a complete JSON message from the Flutter app
// ============================================================
void processAppMessage(const String& raw) {
  StaticJsonDocument<512> root;
  if (deserializeJson(root, raw)) {
    Serial.println("[BT] Bad JSON: " + raw);
    return;
  }
  const char* jtype = root["type"];
  if (!jtype) return;

  if (strcmp(jtype, "app_connected") == 0) {
    // Already handled in onBtConnect, but handle gracefully if re-sent
    isTesting = false; stimulating = false; stimulatingNum = -1;
    tStimulusTimeout.disable();
    sendSensorListBT();
  }

  else if (strcmp(jtype, "list_sensors") == 0) {
    sendSensorListBT();
  }

  else if (strcmp(jtype, "start_test") == 0) {
    isTesting   = true;
    lastSensor  = -1;
  }

  else if (strcmp(jtype, "stop_test") == 0) {
    isTesting      = false;
    stimulating    = false;
    stimulatingNum = -1;
    tStimulusTimeout.disable();
    sendStats();
  }

  else if (strcmp(jtype, "clear_stats") == 0) {
    resetStats();
    sendStats();
  }

  else if (strcmp(jtype, "clear_nodes") == 0) {
    for (int i = 0; i < sensorList.size(); i++) delete sensorList.get(i);
    sensorList.clear();
    stimulating = false; stimulatingNum = -1;
    tStimulusTimeout.disable();
    sendSensorListBT();
  }

  else if (strcmp(jtype, "remove_node") == 0) {
    String targetMac = root["mac"] | String("");
    for (int i = 0; i < sensorList.size(); i++) {
      if (sensorList.get(i)->mac == targetMac) {
        removeSensorAt(i);
        break;
      }
    }
    sendSensorListBT();
  }

  else if (strcmp(jtype, "blink_all") == 0) {
    for (int i = 0; i < sensorList.size(); i++)
      webSocket.sendTXT((uint8_t)sensorList.get(i)->num, "{\"type\":\"blink\"}");
  }

  else if (strcmp(jtype, "config") == 0) {
    tmode               = String((const char*)root["tmode"]);
    min_delay           = root["min_delay"]           | 0;
    max_delay           = root["max_delay"]           | 500;
    mim_timeout         = root["mim_timeout"]         | 1000;
    max_timeout         = root["max_timeout"]         | 2000;
    min_detection_range = root["min_detection_range"] | 0;
    max_detection_range = root["max_detection_range"] | 50;
    if (mim_timeout < 100)             mim_timeout = 100;
    if (max_timeout < mim_timeout) max_timeout = mim_timeout;
    // Flash all nodes to confirm config received
    for (int i = 0; i < sensorList.size(); i++)
      webSocket.sendTXT((uint8_t)sensorList.get(i)->num, "{\"type\":\"blink\"}");
  }
}

// ============================================================
// Read BT stream — newline-delimited JSON
// ============================================================
void readBT() {
  while (BT.available()) {
    char c = (char)BT.read();
    if (c == '\n' || c == '\r') {
      btRxBuffer.trim();
      if (btRxBuffer.length() > 0) {
        processAppMessage(btRxBuffer);
        btRxBuffer = "";
      }
    } else {
      btRxBuffer += c;
      // Guard against runaway buffer (e.g. malformed stream)
      if (btRxBuffer.length() > 1024) btRxBuffer = "";
    }
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  // ---- Wi-Fi AP for nodes ----
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName, apPassword, 1, 0, 10);
  esp_wifi_set_max_tx_power(84);
  WiFi.setSleep(false);
  Serial.print("[WiFi] AP IP: ");
  Serial.println(WiFi.softAPIP());

  // ---- Node WebSocket server ----
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  webSocket.enableHeartbeat(15000, 3000, 2);
  Serial.println("[WS] Node WebSocket server started on port 81");

  // ---- Bluetooth Classic SPP ----
  BT.begin("TrainerLights");          // Advertised BT device name
  BT.register_callback(onBtConnect);
  Serial.println("[BT] Bluetooth SPP started — pairing name: TrainerLights");

  Serial.println("[OK] TrainerLights ESP32 v3.0 ready.");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  webSocket.loop();
  ts.execute();
  readBT();   // Non-blocking BT stream reader

  // Stimulus dispatch — only when app is connected and test is running
  if (!stimulating && isTesting && btConnected && sensorList.size() > 0) {
    int sz = sensorList.size();

    if (tmode == "random") {
      int pick = random(0, sz);
      if (sz > 1) while (pick == lastSensor) pick = random(0, sz);
      currentSensor = pick;
    } else {
      currentSensor = (lastSensor + 1) % sz;
    }

    timeout_ms = (mim_timeout == max_timeout) ? mim_timeout : random(mim_timeout, max_timeout + 1);
    tdelay     = (min_delay   == max_delay)   ? min_delay   : random(min_delay,   max_delay   + 1);
    if (timeout_ms < 100) timeout_ms = 100;
    if (tdelay     < 0)   tdelay     = 0;

    lastSensor    = currentSensor;
    Sensor* s     = sensorList.get(currentSensor);

    if (s) {
      StaticJsonDocument<200> doc;
      doc["type"]                = "stimulus";
      doc["node_id"]             = s->nodeID;
      doc["timeout"]             = timeout_ms;
      doc["delay"]               = tdelay;
      doc["min_detection_range"] = min_detection_range;
      doc["max_detection_range"] = max_detection_range;
      String msg;
      serializeJson(doc, msg);
      webSocket.sendTXT((uint8_t)s->num, msg);

      stimulatingNum = s->num;
      tStimulusTimeout.setInterval(tdelay + timeout_ms + 1000);
      tStimulusTimeout.restartDelayed();
      stimulating = true;
    }
  }
}

// ============================================================
// Node WebSocket event handler
// ============================================================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  IPAddress ip = webSocket.remoteIP(num);

  switch (type) {

    case WStype_DISCONNECTED:
      for (int i = 0; i < sensorList.size(); i++) {
        if (sensorList.get(i)->num == (int)num) {
          removeSensorAt(i);
          sendSensorListBT();
          break;
        }
      }
      break;

    case WStype_CONNECTED:
      Serial.printf("[WS] Node connected on slot %u from %s\n", num, ip.toString().c_str());
      break;

    case WStype_TEXT: {
      StaticJsonDocument<512> root;
      if (deserializeJson(root, payload)) break;
      const char* jtype = root["type"];
      if (!jtype) break;

      // ---- Node registers itself ----
      if (strcmp(jtype, "sensor") == 0) {
        String mac    = root["mac"] | String("unknown");
        int    sentID = root["node_id"] | 0;
        bool   found  = false;

        for (int i = 0; i < sensorList.size(); i++) {
          if (sensorList.get(i)->mac == mac) {
            sensorList.get(i)->num = (int)num;
            sensorList.get(i)->ip  = ip;
            found = true;
            StaticJsonDocument<64> a;
            a["type"] = "assign_id"; a["node_id"] = sensorList.get(i)->nodeID;
            String m; serializeJson(a, m);
            webSocket.sendTXT(num, m);
            break;
          }
        }
        if (!found) {
          Sensor* ns  = new Sensor();
          ns->ip      = ip; ns->mac = mac; ns->num = (int)num; ns->enabled = true;
          if (sentID > 0) {
            bool taken = false;
            for (int i = 0; i < sensorList.size(); i++)
              if (sensorList.get(i)->nodeID == sentID) { taken = true; break; }
            ns->nodeID = taken ? getNextNodeID() : sentID;
          } else { ns->nodeID = getNextNodeID(); }
          sensorList.add(ns);
          StaticJsonDocument<64> a;
          a["type"] = "assign_id"; a["node_id"] = ns->nodeID;
          String m; serializeJson(a, m);
          webSocket.sendTXT(num, m);
        }
        sendSensorListBT();
      }

      // ---- Node hit/miss response ----
      else if (strcmp(jtype, "response") == 0) {
        if (!isTesting) break;   // discard late responses

        tStimulusTimeout.disable();
        stimulating    = false;
        stimulatingNum = -1;

        int error = root["error"]    | 0;
        int rtime = root["time"]     | 0;
        int rdist = root["distance"] | 0;

        if (error == 1) {
          test_errors++;
        } else {
          test_score++; hit_count++;
          if (rtime > max_response_time) max_response_time = rtime;
          if (rtime < min_response_time) min_response_time = rtime;
          if (rdist > max_distance) max_distance = rdist;
          if (rdist < min_distance && rdist > 0) min_distance = rdist;
          sum_response_time += rtime;
          sum_distance      += rdist;
          avg_response_time  = (int)(sum_response_time / hit_count);
          avg_distance       = (int)(sum_distance      / hit_count);
        }
        sendStats();
      }

      break;
    }
    default: break;
  }
}

// ============================================================
// Stimulus timeout callback
// ============================================================
void onStimulusTimeout() {
  if (!stimulating || !isTesting) return;
  stimulating    = false;
  stimulatingNum = -1;
  test_errors++;
  sendStats();
}

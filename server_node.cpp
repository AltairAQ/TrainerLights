// TrainerLights Server
// Author: khader afeez
// khaderafeez16@gmail.com
// ============================================================
// TrainerLights SERVER - NodeMCU ESP-12E (V1.0 FINAL)
// Features: Mobile UI, Ghost-Test Fix, Clear Stats, Node Memory
// abin abraham
// abinabraham176@gmail.com
// ============================================================

#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <WebSocketsServer.h>
#include <ESP8266WebServer.h>
#include <TaskScheduler.h>
#include <ArduinoJson.h>
#include <LinkedList.h>
#include <pgmspace.h>

extern "C" {
#include "user_interface.h"
}

class Sensor {
  public:
    IPAddress ip;
    String    mac;
    int       nodeID;
    uint8_t   num;
    bool      enabled;
};

LinkedList<Sensor*> sensorList = LinkedList<Sensor*>();
uint8_t appConnected = 255;

ESP8266WebServer webServer(80);
WebSocketsServer webSocket = WebSocketsServer(81);

const char* apName     = "TrainerLights";
const char* apPassword = "1234567890";

String tmode            = "random";
int min_delay           = 4000;
int max_delay           = 4000;
int mim_timeout         = 1000;
int max_timeout         = 2000;
int min_detection_range = 0;
int max_detection_range = 50;
int timeout_ms          = 1000;
int tdelay              = 0;

bool isTesting    = false;
int  currentSensor= 0;
int  lastSensor   = -1;
bool stimulating  = false;

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

Scheduler ts;
void onStimulusTimeout();
Task tStimulusTimeout(5000, TASK_ONCE, &onStimulusTimeout, &ts, false);

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);

int getNextNodeID() {
  int maxID = 0;
  for (int i = 0; i < sensorList.size(); i++) {
    if (sensorList.get(i)->nodeID > maxID) maxID = sensorList.get(i)->nodeID;
  }
  return maxID + 1;
}

void sendSensorList(uint8_t toSocket) {
  StaticJsonDocument<512> doc;
  doc["type"]  = "sensor_list";
  doc["count"] = sensorList.size();
  JsonArray arr = doc.createNestedArray("sensors");
  for (int i = 0; i < sensorList.size(); i++) {
    Sensor* s   = sensorList.get(i);
    JsonObject o = arr.createNestedObject();
    o["node_id"] = s->nodeID; o["ip"] = s->ip.toString(); o["mac"] = s->mac; o["socket"] = s->num; o["index"] = i + 1;
  }
  String msg; serializeJson(doc, msg);
  webSocket.sendTXT(toSocket, msg);
}

void resetStats() {
  test_score = 0; test_errors = 0; sum_response_time = 0; sum_distance = 0; hit_count = 0; 
  max_distance = 0; min_distance = 9999; max_response_time = 0; min_response_time = 9999;
  avg_response_time = 0; avg_distance = 0;
}

void sendStats() {
  if (appConnected == 255) return;
  StaticJsonDocument<256> doc;
  doc["type"] = "stats"; doc["test_score"] = test_score; doc["test_errors"] = test_errors;
  doc["max_distance"] = max_distance; doc["min_distance"] = (min_distance == 9999) ? 0 : min_distance;
  doc["avg_distance"] = avg_distance; doc["max_response_time"] = max_response_time;
  doc["min_response_time"] = (min_response_time == 9999) ? 0 : min_response_time; doc["avg_response_time"] = avg_response_time;
  String msg; serializeJson(doc, msg); webSocket.sendTXT(appConnected, msg);
}

// ============================================================
// MODERN MOBILE WEB APP
// ============================================================
const char PAGE_HEAD[] PROGMEM = R"=====(
<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>TrainerLights</title>
<style>
:root { --bg:#f4f7f6; --card:#ffffff; --text:#1d1d1f; --green:#34c759; --red:#ff3b30; --blue:#007aff; --gray:#8e8e93; --lightgray:#f2f2f7; }
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--text);-webkit-font-smoothing:antialiased;}
.topbar{background:#1d1d1f;color:#fff;padding:16px 20px;font-size:20px;font-weight:700;position:fixed;top:0;width:100%;z-index:1000;display:flex;justify-content:space-between;align-items:center;box-shadow:0 2px 10px rgba(0,0,0,0.2);}
.badge{font-size:12px;padding:4px 12px;border-radius:20px;background:var(--red);color:#fff;font-weight:600;text-transform:uppercase;}
.badge.on{background:var(--green);}
.wrap{padding:80px 16px 40px;max-width:600px;margin:0 auto;}
.card{background:var(--card);border-radius:16px;padding:20px;margin-bottom:20px;box-shadow:0 4px 24px rgba(0,0,0,0.06);}
.card h2{font-size:18px;margin-bottom:16px;color:var(--text);font-weight:700;border-bottom:2px solid var(--lightgray);padding-bottom:10px;}
.row{display:flex;gap:12px;margin-bottom:12px;}
.box{flex:1;text-align:center;padding:16px 8px;border-radius:12px;background:var(--lightgray);}
.num{font-size:36px;font-weight:800;line-height:1;}
.lbl{font-size:12px;color:var(--gray);margin-top:6px;font-weight:500;text-transform:uppercase;}
.green{color:var(--green);}.red{color:var(--red);}.blue{color:var(--blue);}
.timer{font-size:56px;font-weight:700;font-family:monospace;text-align:center;padding:10px 0;color:var(--text);}
.timer small{font-size:24px;color:var(--gray);}
.btn{display:block;width:100%;padding:16px;border:none;border-radius:12px;font-size:16px;font-weight:700;cursor:pointer;margin-bottom:10px;color:#fff;}
.btn:active{opacity:0.8;}
.btn-green{background:var(--green);} .btn-red{background:var(--red);} .btn-blue{background:var(--blue);} .btn-gray{background:var(--gray);}
.half{display:flex;gap:12px;} .half .btn{flex:1;margin-bottom:0;}
label{display:block;font-size:13px;color:var(--gray);margin:12px 0 6px;font-weight:600;}
input,select{width:100%;padding:14px;border:2px solid var(--lightgray);border-radius:10px;font-size:16px;background:var(--card);color:var(--text);font-weight:500;outline:none;}
input:focus,select:focus{border-color:var(--blue);}
.node-grid{display:flex;flex-wrap:wrap;gap:12px;margin-top:16px;}
.node-card{flex:1 1 calc(50% - 12px);background:#eaffec;border:2px solid var(--green);border-radius:12px;padding:16px;text-align:center;}
.nid{font-size:36px;font-weight:800;color:#1a5c2a;line-height:1;}
.ntag{font-size:11px;background:#d4f8c4;color:#1a5c2a;border-radius:6px;padding:4px 8px;display:inline-block;margin:8px 0;font-weight:600;}
.nip{font-size:12px;color:#444;}
.empty{color:var(--gray);font-size:14px;padding:24px;text-align:center;width:100%;background:var(--lightgray);border-radius:12px;}
.srow{display:flex;justify-content:space-between;align-items:center;margin-bottom:16px;}
.cbadge{font-size:13px;padding:6px 14px;border-radius:12px;background:var(--lightgray);color:var(--gray);font-weight:700;}
.cbadge.ok{background:#d4f8c4;color:#1a5c2a;}
</style></head><body>
<div class='topbar'>TrainerLights<span class='badge' id='wsb'>Offline</span></div><div class='wrap'>
)=====";

const char PAGE_STATS[] PROGMEM = R"=====(
<div class='card'><h2>Session Timer</h2>
<div class='timer' id='timer'>00:00<small>.00</small></div>
<div class='half' style='margin-bottom:12px;'>
  <button class='btn btn-green' id='btnS' onclick='toggleTimer()'>Start</button>
  <button class='btn btn-gray' onclick='resetTimer()'>Reset Timer</button>
</div>
</div>
<div class='card'>
  <div class='srow'>
    <h2 style='margin:0; border:none; padding:0;'>Live Statistics</h2>
    <button class='btn btn-gray' style='width:auto;padding:8px 16px;margin:0;font-size:13px;' onclick='clearStats()'>Clear Stats</button>
  </div>
<div class='row'>
  <div class='box'><div class='num green' id='sc'>0</div><div class='lbl'>Hits</div></div>
  <div class='box'><div class='num red' id='er'>0</div><div class='lbl'>Misses</div></div>
  <div class='box'><div class='num blue' id='tot'>0</div><div class='lbl'>Total</div></div>
</div>
<div class='row'>
  <div class='box'><div class='num blue' id='avt'>0</div><div class='lbl'>Avg ms</div></div>
  <div class='box'><div class='num green' id='mit'>0</div><div class='lbl'>Best ms</div></div>
  <div class='box'><div class='num red' id='mat'>0</div><div class='lbl'>Worst ms</div></div>
</div>
</div>
)=====";

const char PAGE_NODES[] PROGMEM = R"=====(
<div class='card'><h2>Hardware Nodes</h2>
<div class='srow'>
  <span class='cbadge' id='nc'>Scanning...</span>
  <button class='btn btn-gray' style='width:auto;padding:8px 16px;margin:0;font-size:13px;' onclick='reqList()'>Refresh</button>
</div>
<div class='half'>
  <button class='btn btn-blue' style='font-size:14px;' onclick='blinkAll()'>Test Lights</button>
  <button class='btn btn-red' style='font-size:14px;' onclick='clearNodes()'>Clear Nodes</button>
</div>
<div class='node-grid' id='ng'><div class='empty'>Waiting for nodes...</div></div></div>
)=====";

const char PAGE_SETTINGS[] PROGMEM = R"=====(
<div class='card'><h2>Training Settings</h2>
<label>Mode</label><select id='tmode'><option value='random'>Random</option><option value='sequence'>Sequential</option></select>
<label>Delay Min (ms)</label><input id='min_delay' type='number' value='4000' min='0'>
<label>Delay Max (ms)</label><input id='max_delay' type='number' value='4000' min='0'>
<label>Timeout Min (ms)</label><input id='mim_timeout' type='number' value='1000' min='100'>
<label>Timeout Max (ms)</label><input id='max_timeout' type='number' value='2000' min='100'>
<label>Detection Min (cm)</label><input id='min_det' type='number' value='2' min='0'>
<label>Detection Max (cm)</label><input id='max_det' type='number' value='40' min='5'><br><br>
<button class='btn btn-green' id='btnApply' onclick='applySettings()'>Save Settings</button></div>
)=====";

const char PAGE_JS[] PROGMEM = R"=====(
<script>
var ws=null, running=false, tInt=null; var t0=0, tOff=0, tm=0;
function connect(){
  var h=location.hostname||'192.168.4.1'; ws=new WebSocket('ws://'+h+':81/',['arduino']);
  ws.onopen=function(){ document.getElementById('wsb').textContent='Online'; document.getElementById('wsb').className='badge on'; ws.send(JSON.stringify({type:'app_connected',current_time:Date.now()})); reqList(); };
  ws.onclose=function(){ document.getElementById('wsb').textContent='Offline'; document.getElementById('wsb').className='badge'; setTimeout(connect,3000); };
  ws.onerror=function(e){ws.close();};
  ws.onmessage=function(e){ var d=JSON.parse(e.data); if(d.type==='sensor_list') showNodes(d); if(d.type==='stats') showStats(d); };
}
function send(o){if(ws&&ws.readyState===1) ws.send(JSON.stringify(o));}
function reqList(){ send({type:'list_sensors'}); }
function blinkAll(){ send({type:'blink_all'}); }
function clearNodes(){ if(confirm('Erase all connected nodes?')) send({type:'clear_nodes'}); }
function clearStats(){ send({type:'clear_stats'}); }

function showNodes(d){
  var g=document.getElementById('ng'); var b=document.getElementById('nc'); var n=d.sensors?d.sensors.length:0;
  if(n===0){ b.textContent='0 Nodes'; b.className='cbadge'; g.innerHTML="<div class='empty'>No nodes connected. Turn on hardware.</div>"; return; }
  b.textContent=n+' Node'+(n>1?'s':'')+' Active'; b.className='cbadge ok'; var h='';
  for(var i=0;i<d.sensors.length;i++){ var s=d.sensors[i]; h+="<div class='node-card'><div class='nid'>"+s.node_id+"</div><div class='ntag'>Connected</div><div class='nip'>"+s.ip+"</div></div>"; }
  g.innerHTML=h;
}
function showStats(d){
  document.getElementById('sc').textContent=d.test_score; document.getElementById('er').textContent=d.test_errors; document.getElementById('tot').textContent=d.test_score+d.test_errors;
  document.getElementById('avt').textContent=d.avg_response_time; document.getElementById('mit').textContent=d.min_response_time; document.getElementById('mat').textContent=d.max_response_time;
}
function pad(n){ return n<10?'0'+n:n; }
function updateTimer(){
  tm=Date.now()-t0+tOff; var h=Math.floor(tm/3600000), m=Math.floor(tm/60000)%60, s=Math.floor(tm/1000)%60, ms=Math.floor(tm/10)%100;
  var str=pad(m)+':'+pad(s); if(h>0) str=h+':'+str;
  document.getElementById('timer').innerHTML=str+'<small>.'+pad(ms)+'</small>';
}
function toggleTimer(){
  if(running){ tOff=tm; clearInterval(tInt); running=false; document.getElementById('btnS').textContent='Resume'; document.getElementById('btnS').className='btn btn-blue'; send({type:'stop_test'}); }
  else { t0=Date.now(); clearInterval(tInt); tInt=setInterval(updateTimer,50); running=true; document.getElementById('btnS').textContent='Pause'; document.getElementById('btnS').className='btn btn-red'; send({type:'start_test'}); }
}
function resetTimer(){
  clearInterval(tInt); running=false; tOff=0; tm=0; document.getElementById('timer').innerHTML='00:00<small>.00</small>';
  document.getElementById('btnS').textContent='Start'; document.getElementById('btnS').className='btn btn-green'; send({type:'stop_test'});
}
function applySettings(){
  var btn = document.getElementById('btnApply');
  send({ type:'config', tmode:document.getElementById('tmode').value, min_delay:parseInt(document.getElementById('min_delay').value), max_delay:parseInt(document.getElementById('max_delay').value), mim_timeout:parseInt(document.getElementById('mim_timeout').value), max_timeout:parseInt(document.getElementById('max_timeout').value), min_detection_range:parseInt(document.getElementById('min_det').value), max_detection_range:parseInt(document.getElementById('max_det').value) });
  btn.textContent = 'Settings Saved & Nodes Flashed!'; btn.className = 'btn btn-blue';
  setTimeout(function(){ btn.textContent = 'Save Settings'; btn.className = 'btn btn-green'; }, 2000);
}
setInterval(function(){ if(ws&&ws.readyState===1) reqList(); }, 5000);
connect();
</script></body></html>
)=====";

void handleRoot() {
  webServer.send(200,"text/html", String(PAGE_HEAD) + String(PAGE_STATS) + String(PAGE_NODES) + String(PAGE_SETTINGS) + String(PAGE_JS));
}

void setup() {
  Serial.begin(115200);
  struct softap_config cfg; wifi_softap_get_config(&cfg); cfg.max_connection = 8; wifi_softap_set_config(&cfg);
  wifi_set_sleep_type(NONE_SLEEP_T);
  WiFi.mode(WIFI_AP); WiFi.softAP(apName, apPassword);
  
  webSocket.begin(); webSocket.onEvent(webSocketEvent);
  webServer.on("/", handleRoot); webServer.begin();
}

void loop() {
  webSocket.loop(); webServer.handleClient(); ts.execute();

  if (!stimulating && isTesting && sensorList.size() > 0) {
    int sz = sensorList.size();
    if (tmode == "random") {
      int pick = random(0, sz);
      if (sz > 1) { while (pick == lastSensor) pick = random(0, sz); }
      currentSensor = pick;
    } else {
      currentSensor = (lastSensor + 1) % sz;
    }

    timeout_ms = (mim_timeout == max_timeout) ? mim_timeout : random(mim_timeout, max_timeout + 1);
    tdelay     = (min_delay == max_delay) ? min_delay : random(min_delay, max_delay + 1);
    if (timeout_ms < 100)  timeout_ms = 100;
    if (tdelay     < 0)    tdelay     = 0;

    lastSensor = currentSensor; Sensor* s  = sensorList.get(currentSensor);

    if (s) {
      StaticJsonDocument<200> doc; doc["type"] = "stimulus"; doc["node_id"] = s->nodeID; doc["timeout"] = timeout_ms; doc["delay"] = tdelay; doc["min_detection_range"] = min_detection_range; doc["max_detection_range"] = max_detection_range;
      String msg; serializeJson(doc, msg); webSocket.sendTXT(s->num, msg);
      tStimulusTimeout.setInterval(tdelay + timeout_ms + 1000); tStimulusTimeout.restartDelayed();
      stimulating = true;
    }
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  IPAddress ip = webSocket.remoteIP(num);
  switch (type) {
    case WStype_DISCONNECTED:
      for (int i = 0; i < sensorList.size(); i++) {
        if (sensorList.get(i)->num == num) { sensorList.remove(i); if (appConnected != 255) sendSensorList(appConnected); break; }
      }
      break;
    case WStype_CONNECTED: break;
    case WStype_TEXT: {
      StaticJsonDocument<400> root;
      if (deserializeJson(root, payload)) break;
      const char* jtype = root["type"]; if (!jtype) break;

      if (strcmp(jtype, "sensor") == 0) {
        String mac = root["mac"] | String("unknown"); int sentID = root["node_id"] | 0; bool found = false;
        for (int i = 0; i < sensorList.size(); i++) {
          if (sensorList.get(i)->mac == mac) {
            sensorList.get(i)->num = num; sensorList.get(i)->ip = ip; found = true;
            StaticJsonDocument<64> a; a["type"] = "assign_id"; a["node_id"] = sensorList.get(i)->nodeID;
            String m; serializeJson(a, m); webSocket.sendTXT(num, m); break;
          }
        }
        if (!found) {
          Sensor* ns = new Sensor(); ns->ip = ip; ns->mac = mac; ns->num = num; ns->enabled = true;
          if (sentID > 0) {
            bool taken = false;
            for (int i = 0; i < sensorList.size(); i++) { if (sensorList.get(i)->nodeID == sentID) { taken = true; break; } }
            ns->nodeID = taken ? getNextNodeID() : sentID;
          } else { ns->nodeID = getNextNodeID(); }
          sensorList.add(ns);
          StaticJsonDocument<64> a; a["type"] = "assign_id"; a["node_id"] = ns->nodeID;
          String m; serializeJson(a, m); webSocket.sendTXT(num, m);
        }
        if (appConnected != 255) sendSensorList(appConnected);
      }

      if (strcmp(jtype, "list_sensors") == 0) sendSensorList(num);

      if (strcmp(jtype, "response") == 0) {
        tStimulusTimeout.disable(); stimulating = false;
        int error = root["error"] | 0; int rtime = root["time"] | 0; int rdist = root["distance"] | 0;

        if (error == 1) { test_errors++; } else {
          test_score++; hit_count++;
          if (rtime > max_response_time) max_response_time = rtime;
          if (rtime < min_response_time) min_response_time = rtime;
          if (rdist > max_distance) max_distance = rdist;
          if (rdist < min_distance && rdist > 0) min_distance = rdist;
          sum_response_time += rtime; sum_distance += rdist;
          avg_response_time = (int)(sum_response_time / hit_count); avg_distance = (int)(sum_distance / hit_count);
        }
        sendStats();
      }

      if (strcmp(jtype, "start_test") == 0) { isTesting = true; lastSensor = -1; }
      if (strcmp(jtype, "stop_test") == 0) { isTesting = false; stimulating = false; tStimulusTimeout.disable(); }
      
      // CLEAR ONLY THE DETAILS (STATS)
      if (strcmp(jtype, "clear_stats") == 0) { resetStats(); sendStats(); }
      
      if (strcmp(jtype, "clear_nodes") == 0) { sensorList.clear(); sendSensorList(appConnected); }

      // PREVENTS GHOST TESTS ON PAGE REFRESH
      if (strcmp(jtype, "app_connected") == 0) { 
        appConnected = num; 
        isTesting = false; stimulating = false; tStimulusTimeout.disable();
        sendSensorList(appConnected); 
      }
      
      if (strcmp(jtype, "blink_all") == 0) { for (int i = 0; i < sensorList.size(); i++) webSocket.sendTXT(sensorList.get(i)->num, "{\"type\":\"blink\"}"); }

      if (strcmp(jtype, "config") == 0) {
        tmode = String((const char*)root["tmode"]); min_delay = root["min_delay"] | 0; max_delay = root["max_delay"] | 500;
        mim_timeout = root["mim_timeout"] | 1000; max_timeout = root["max_timeout"] | 2000;
        min_detection_range = root["min_detection_range"] | 0; max_detection_range = root["max_detection_range"] | 50;
        if (mim_timeout < 100) mim_timeout = 100; if (max_timeout < mim_timeout) max_timeout = mim_timeout;
        for (int i = 0; i < sensorList.size(); i++) webSocket.sendTXT(sensorList.get(i)->num, "{\"type\":\"blink\"}");
      }
      break;
    }
    default: break;
  }
}

void onStimulusTimeout() {
  if (!stimulating) return;
  stimulating = false; test_errors++; sendStats();
}

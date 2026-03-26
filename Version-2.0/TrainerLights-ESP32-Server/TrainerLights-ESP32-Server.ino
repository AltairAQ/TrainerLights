// ============================================================
// TrainerLights SERVER - ESP32 WROOM-32D (V2.0)
// Authors: Khader Afeez <khaderafeez16@gmail.com>
//          Abin Abraham <abinabraham176@gmail.com>
//          James Kattoor <jameskattoor004@gmail.com>
// Fixed:   Memory leaks, remove_node null-guard, late response
//          ignored when stopped, node-drop stimulus stuck state,
//          UI: node-drop visual, session indicator, numpad inputs
// ============================================================

// MUST be defined before any WebSockets include
#define WEBSOCKETS_SERVER_CLIENT_MAX 12

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <TaskScheduler.h>
#include <ArduinoJson.h>
#include <LinkedList.h>
#include <pgmspace.h>
#include "esp_wifi.h"
// ========================================Trainerlights====================
// Sensor
// ============================================================
class Sensor {
  public:
    IPAddress ip;
    String    mac;
    int       nodeID;
    int       num;      // WebSocket slot
    bool      enabled;
};

LinkedList<Sensor*> sensorList = LinkedList<Sensor*>();
int appConnected = -1;

WebServer        webServer(80);
WebSocketsServer webSocket = WebSocketsServer(81);

const char* apName     = "TrainerLights";
const char* apPassword = "1234567890";

// ---- Session state ----
String tmode            = "random";
int min_delay           = 4000;
int max_delay           = 4000;
int mim_timeout         = 1000;
int max_timeout         = 2000;
int min_detection_range = 0;
int max_detection_range = 50;
int timeout_ms          = 1000;
int tdelay              = 0;

bool isTesting     = false;
int  currentSensor = 0;
int  lastSensor    = -1;
bool stimulating   = false;
int  stimulatingNum = -1;   // FIX: track which socket is being stimulated

// ---- Stats ----
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

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);

// ============================================================
// Helpers
// ============================================================
int getNextNodeID() {
  int maxID = 0;
  for (int i = 0; i < sensorList.size(); i++)
    if (sensorList.get(i)->nodeID > maxID) maxID = sensorList.get(i)->nodeID;
  return maxID + 1;
}

// FIX: helper to safely delete a sensor from the list
void removeSensorAt(int index) {
  Sensor* s = sensorList.get(index);
  // FIX: if this node was being stimulated, clear the stuck state
  if (stimulating && s->num == stimulatingNum) {
    stimulating     = false;
    stimulatingNum  = -1;
    tStimulusTimeout.disable();
    // Count as error only if test is still running
    if (isTesting) { test_errors++; if (appConnected != -1) sendStats(); }
  }
  delete s;                   // FIX: free memory
  sensorList.remove(index);
}

void sendSensorList(int toSocket) {
  if (toSocket == -1) return; // FIX: null-guard
  StaticJsonDocument<1024> doc;
  doc["type"]  = "sensor_list";
  doc["count"] = sensorList.size();
  JsonArray arr = doc.createNestedArray("sensors");
  for (int i = 0; i < sensorList.size(); i++) {
    Sensor* s = sensorList.get(i);
    JsonObject o = arr.createNestedObject();
    o["node_id"] = s->nodeID;
    o["ip"]      = s->ip.toString();
    o["mac"]     = s->mac;
    o["socket"]  = s->num;
    o["index"]   = i + 1;
  }
  String msg;
  serializeJson(doc, msg);
  webSocket.sendTXT((uint8_t)toSocket, msg);
}

void resetStats() {
  test_score = test_errors = hit_count = 0;
  sum_response_time = sum_distance = 0;
  max_distance = max_response_time = avg_response_time = avg_distance = 0;
  min_distance = min_response_time = 9999;
}

void sendStats() {
  if (appConnected == -1) return;
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
  doc["is_testing"]        = isTesting;   // FIX: let UI know session state
  String msg;
  serializeJson(doc, msg);
  webSocket.sendTXT((uint8_t)appConnected, msg);
}

// ============================================================
// Web UI  (PROGMEM)
// ============================================================
const char PAGE_HEAD[] PROGMEM = R"=====(
<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>TrainerLights</title>
<style>
:root{--bg:#f4f7f6;--card:#fff;--text:#1d1d1f;--green:#34c759;--red:#ff3b30;--blue:#007aff;--gray:#8e8e93;--lg:#f2f2f7;}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--text);-webkit-font-smoothing:antialiased;}
.topbar{background:#1d1d1f;color:#fff;padding:16px 20px;font-size:20px;font-weight:700;position:fixed;top:0;width:100%;z-index:1000;display:flex;justify-content:space-between;align-items:center;gap:8px;box-shadow:0 2px 10px rgba(0,0,0,.2);}
.topbadges{display:flex;gap:6px;align-items:center;}
.badge{font-size:11px;padding:4px 10px;border-radius:20px;background:var(--red);color:#fff;font-weight:700;text-transform:uppercase;white-space:nowrap;}
.badge.on{background:var(--green);}
.badge.testing{background:var(--blue);animation:pulse 1.2s infinite;}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.55}}
.wrap{padding:80px 16px 40px;max-width:600px;margin:0 auto;}
.card{background:var(--card);border-radius:16px;padding:20px;margin-bottom:20px;box-shadow:0 4px 24px rgba(0,0,0,.06);}
.card h2{font-size:18px;margin-bottom:16px;font-weight:700;border-bottom:2px solid var(--lg);padding-bottom:10px;}
.row{display:flex;gap:12px;margin-bottom:12px;}
.box{flex:1;text-align:center;padding:16px 8px;border-radius:12px;background:var(--lg);}
.num{font-size:36px;font-weight:800;line-height:1;}
.lbl{font-size:11px;color:var(--gray);margin-top:6px;font-weight:600;text-transform:uppercase;}
.green{color:var(--green)}.red{color:var(--red)}.blue{color:var(--blue)}
.timer{font-size:56px;font-weight:700;font-family:monospace;text-align:center;padding:10px 0;}
.timer small{font-size:24px;color:var(--gray);}
.btn{display:block;width:100%;padding:16px;border:none;border-radius:12px;font-size:16px;font-weight:700;cursor:pointer;margin-bottom:10px;color:#fff;transition:opacity .15s;}
.btn:active{opacity:.75}
.btn-green{background:var(--green)}.btn-red{background:var(--red)}.btn-blue{background:var(--blue)}.btn-gray{background:var(--gray)}
.btn:disabled{opacity:.4;cursor:not-allowed;}
.half{display:flex;gap:12px;}.half .btn{flex:1;margin-bottom:0;}
label{display:block;font-size:13px;color:var(--gray);margin:12px 0 6px;font-weight:600;}
input,select{width:100%;padding:14px;border:2px solid var(--lg);border-radius:10px;font-size:16px;background:var(--card);color:var(--text);font-weight:500;outline:none;}
input:focus,select:focus{border-color:var(--blue);}
.srow{display:flex;justify-content:space-between;align-items:center;margin-bottom:16px;}
.cbadge{font-size:13px;padding:6px 14px;border-radius:12px;background:var(--lg);color:var(--gray);font-weight:700;}
.cbadge.ok{background:#d4f8c4;color:#1a5c2a;}
.node-list{display:flex;flex-direction:column;gap:10px;max-height:480px;overflow-y:auto;padding-right:4px;}
.node-list::-webkit-scrollbar{width:4px;}
.node-list::-webkit-scrollbar-thumb{background:#ccc;border-radius:4px;}
.nrow{display:flex;align-items:center;border-radius:12px;padding:12px 14px;gap:12px;border:2px solid var(--green);background:#f8fff9;transition:border-color .3s,background .3s;}
.nrow.dropped{border-color:var(--red);background:#fff5f5;}
.nbadge{color:#fff;font-size:18px;font-weight:800;min-width:42px;height:42px;border-radius:10px;display:flex;align-items:center;justify-content:center;background:var(--green);transition:background .3s;}
.nrow.dropped .nbadge{background:var(--red);}
.ninfo{flex:1;min-width:0;}
.nname{font-size:15px;font-weight:700;color:#1a5c2a;}
.nrow.dropped .nname{color:#c0392b;}
.ndetail{font-size:12px;color:#666;margin-top:2px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
.nstatus{font-size:11px;border-radius:6px;padding:3px 8px;font-weight:700;white-space:nowrap;background:#d4f8c4;color:#1a5c2a;}
.nrow.dropped .nstatus{background:#ffd5d5;color:#c0392b;}
.nremove{background:var(--red);color:#fff;border:none;border-radius:8px;width:34px;height:34px;font-size:18px;font-weight:700;cursor:pointer;display:flex;align-items:center;justify-content:center;flex-shrink:0;}
.nremove:active{opacity:.7}
.empty{color:var(--gray);font-size:14px;padding:24px;text-align:center;width:100%;background:var(--lg);border-radius:12px;}
.toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%) translateY(80px);background:#1d1d1f;color:#fff;padding:12px 22px;border-radius:12px;font-size:14px;font-weight:600;opacity:0;transition:all .35s;z-index:9999;white-space:nowrap;}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0);}
</style></head><body>
<div class='topbar'>TrainerLights
  <div class='topbadges'>
    <span class='badge' id='tbTest' style='display:none'>Testing</span>
    <span class='badge' id='wsb'>Offline</span>
  </div>
</div>
<div id='toast' class='toast'></div>
<div class='wrap'>
)=====";

const char PAGE_STATS[] PROGMEM = R"=====(
<div class='card'><h2>Session Timer</h2>
<div class='timer' id='timer'>00:00<small>.00</small></div>
<div class='half' style='margin-bottom:12px;'>
  <button class='btn btn-green' id='btnS' onclick='toggleTimer()'>Start</button>
  <button class='btn btn-gray' onclick='resetTimer()'>Reset</button>
</div></div>
<div class='card'>
  <div class='srow'>
    <h2 style='margin:0;border:none;padding:0;'>Live Stats</h2>
    <button class='btn btn-gray' style='width:auto;padding:8px 16px;margin:0;font-size:13px;' onclick='clearStats()'>Clear</button>
  </div>
  <div class='row'>
    <div class='box'><div class='num green' id='sc'>0</div><div class='lbl'>Hits</div></div>
    <div class='box'><div class='num red'   id='er'>0</div><div class='lbl'>Misses</div></div>
    <div class='box'><div class='num blue'  id='tot'>0</div><div class='lbl'>Total</div></div>
  </div>
  <div class='row'>
    <div class='box'><div class='num blue'  id='avt'>0</div><div class='lbl'>Avg ms</div></div>
    <div class='box'><div class='num green' id='mit'>0</div><div class='lbl'>Best ms</div></div>
    <div class='box'><div class='num red'   id='mat'>0</div><div class='lbl'>Worst ms</div></div>
  </div>
</div>
)=====";

const char PAGE_NODES[] PROGMEM = R"=====(
<div class='card'>
<h2>Hardware Nodes</h2>
<div class='srow'>
  <span class='cbadge' id='nc'>Scanning...</span>
  <button class='btn btn-gray' style='width:auto;padding:8px 16px;margin:0;font-size:13px;' onclick='reqList()'>Refresh</button>
</div>
<div class='half' style='margin-bottom:14px;'>
  <button class='btn btn-blue' style='font-size:14px;' onclick='blinkAll()'>Test Lights</button>
  <button class='btn btn-red'  style='font-size:14px;' onclick='clearNodes()'>Clear All</button>
</div>
<div class='node-list' id='ng'><div class='empty'>Waiting for nodes...</div></div>
</div>
)=====";

const char PAGE_SETTINGS[] PROGMEM = R"=====(
<div class='card'><h2>Training Settings</h2>
<label>Mode</label>
<select id='tmode'><option value='random'>Random</option><option value='sequence'>Sequential</option></select>
<label>Delay Min (ms)</label><input id='min_delay'   type='number' inputmode='numeric' value='4000' min='0'>
<label>Delay Max (ms)</label><input id='max_delay'   type='number' inputmode='numeric' value='4000' min='0'>
<label>Timeout Min (ms)</label><input id='mim_timeout' type='number' inputmode='numeric' value='1000' min='100'>
<label>Timeout Max (ms)</label><input id='max_timeout' type='number' inputmode='numeric' value='2000' min='100'>
<label>Detection Min (cm)</label><input id='min_det' type='number' inputmode='numeric' value='2'  min='0'>
<label>Detection Max (cm)</label><input id='max_det' type='number' inputmode='numeric' value='40' min='5'>
<br><br>
<button class='btn btn-green' id='btnApply' onclick='applySettings()'>Save Settings</button>
</div>
)=====";

const char PAGE_JS[] PROGMEM = R"=====(
<script>
var ws=null, running=false, tInt=null, t0=0, tOff=0, tm=0;
var knownNodes={};   // mac -> node row data, for drop detection

function connect(){
  var h=location.hostname||'192.168.4.1';
  ws=new WebSocket('ws://'+h+':81/',['arduino']);
  ws.onopen=function(){
    document.getElementById('wsb').textContent='Online';
    document.getElementById('wsb').className='badge on';
    ws.send(JSON.stringify({type:'app_connected',current_time:Date.now()}));
    reqList();
  };
  ws.onclose=function(){
    document.getElementById('wsb').textContent='Offline';
    document.getElementById('wsb').className='badge';
    setTimeout(connect,3000);
  };
  ws.onerror=function(){ws.close();};
  ws.onmessage=function(e){
    var d=JSON.parse(e.data);
    if(d.type==='sensor_list') showNodes(d);
    if(d.type==='stats')       showStats(d);
  };
}

function send(o){if(ws&&ws.readyState===1) ws.send(JSON.stringify(o));}
function reqList(){ send({type:'list_sensors'}); }
function blinkAll(){ send({type:'blink_all'}); }

function clearNodes(){
  if(!confirm('Clear ALL nodes? This stops any active test.')) return;
  send({type:'clear_nodes'});
}

function clearStats(){ send({type:'clear_stats'}); toast('Stats cleared'); }
function removeNode(mac){ send({type:'remove_node',mac:mac}); }

// FIX: detect drops by comparing incoming list to knownNodes
function showNodes(d){
  var g=document.getElementById('ng');
  var b=document.getElementById('nc');
  var sensors=d.sensors||[];
  var n=sensors.length;

  // Build a set of currently active macs
  var activeMacs={};
  sensors.forEach(function(s){ activeMacs[s.mac]=s; });

  // Mark drops in knownNodes
  Object.keys(knownNodes).forEach(function(mac){
    knownNodes[mac].live = !!activeMacs[mac];
  });
  // Add new arrivals
  sensors.forEach(function(s){
    if(!knownNodes[s.mac]) knownNodes[s.mac]={live:true,data:s};
    else { knownNodes[s.mac].live=true; knownNodes[s.mac].data=s; }
  });

  if(Object.keys(knownNodes).length===0){
    b.textContent='0 Nodes'; b.className='cbadge';
    g.innerHTML="<div class='empty'>No nodes connected. Turn on hardware.</div>";
    return;
  }

  var liveCount=sensors.length;
  b.textContent=liveCount+' Node'+(liveCount!==1?'s':'')+' Active'+(Object.keys(knownNodes).length>liveCount?' ('+( Object.keys(knownNodes).length-liveCount)+' dropped)':'');
  b.className= liveCount>0 ? 'cbadge ok' : 'cbadge';

  var h='';
  Object.keys(knownNodes).forEach(function(mac){
    var entry=knownNodes[mac]; var s=entry.data;
    var dropped=!entry.live;
    h+="<div class='nrow"+(dropped?' dropped':'')+"'>"+
       "<div class='nbadge'>"+s.node_id+"</div>"+
       "<div class='ninfo'>"+
         "<div class='nname'>Node "+s.node_id+"</div>"+
         "<div class='ndetail'>"+s.ip+"</div>"+
       "</div>"+
       "<span class='nstatus'>"+(dropped?'&#9679; Dropped':'&#9679; Live')+"</span>"+
       "<button class='nremove' onclick=\"removeNode('"+mac+"')\" title='Remove'>&#x2715;</button>"+
       "</div>";
  });
  g.innerHTML=h;
}

function showStats(d){
  document.getElementById('sc').textContent=d.test_score;
  document.getElementById('er').textContent=d.test_errors;
  document.getElementById('tot').textContent=d.test_score+d.test_errors;
  document.getElementById('avt').textContent=d.avg_response_time;
  document.getElementById('mit').textContent=d.min_response_time;
  document.getElementById('mat').textContent=d.max_response_time;
  // FIX: reflect server testing state in UI badge
  var tb=document.getElementById('tbTest');
  tb.style.display = d.is_testing ? 'inline-block' : 'none';
}

function pad(n){return n<10?'0'+n:''+n;}
function updateTimer(){
  tm=Date.now()-t0+tOff;
  var h=Math.floor(tm/3600000),m=Math.floor(tm/60000)%60,s=Math.floor(tm/1000)%60,ms=Math.floor(tm/10)%100;
  var str=pad(m)+':'+pad(s); if(h>0) str=h+':'+str;
  document.getElementById('timer').innerHTML=str+'<small>.'+pad(ms)+'</small>';
}

function toggleTimer(){
  if(running){
    tOff=tm; clearInterval(tInt); running=false;
    document.getElementById('btnS').textContent='Resume';
    document.getElementById('btnS').className='btn btn-blue';
    send({type:'stop_test'});
  } else {
    t0=Date.now(); clearInterval(tInt);
    tInt=setInterval(updateTimer,50);
    running=true;
    document.getElementById('btnS').textContent='Pause';
    document.getElementById('btnS').className='btn btn-red';
    send({type:'start_test'});
  }
}

function resetTimer(){
  clearInterval(tInt); running=false; tOff=0; tm=0;
  document.getElementById('timer').innerHTML='00:00<small>.00</small>';
  document.getElementById('btnS').textContent='Start';
  document.getElementById('btnS').className='btn btn-green';
  send({type:'stop_test'});
}

function applySettings(){
  var btn=document.getElementById('btnApply');
  send({type:'config',
    tmode:document.getElementById('tmode').value,
    min_delay:parseInt(document.getElementById('min_delay').value),
    max_delay:parseInt(document.getElementById('max_delay').value),
    mim_timeout:parseInt(document.getElementById('mim_timeout').value),
    max_timeout:parseInt(document.getElementById('max_timeout').value),
    min_detection_range:parseInt(document.getElementById('min_det').value),
    max_detection_range:parseInt(document.getElementById('max_det').value)
  });
  btn.textContent='Saved!'; btn.className='btn btn-blue';
  setTimeout(function(){btn.textContent='Save Settings';btn.className='btn btn-green';},2000);
}

function toast(msg){
  var t=document.getElementById('toast');
  t.textContent=msg; t.classList.add('show');
  setTimeout(function(){t.classList.remove('show');},2200);
}

setInterval(function(){if(ws&&ws.readyState===1) reqList();},4000);
connect();
</script></body></html>
)=====";

// ============================================================
// HTTP
// ============================================================
void handleRoot() {
  String page = String(FPSTR(PAGE_HEAD))
              + String(FPSTR(PAGE_STATS))
              + String(FPSTR(PAGE_NODES))
              + String(FPSTR(PAGE_SETTINGS))
              + String(FPSTR(PAGE_JS));
  webServer.send(200, "text/html", page);
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName, apPassword, 1, 0, 10);
  esp_wifi_set_max_tx_power(84);
  WiFi.setSleep(false);

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  if (MDNS.begin("trainerlights")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: trainerlights.local");
  }

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  webSocket.enableHeartbeat(15000, 3000, 2);

  webServer.on("/", handleRoot);
  webServer.begin();

  Serial.println("TrainerLights ESP32 v2.0 ready.");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  webSocket.loop();
  webServer.handleClient();
  ts.execute();

  if (!stimulating && isTesting && sensorList.size() > 0) {
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

    lastSensor = currentSensor;
    Sensor* s  = sensorList.get(currentSensor);

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

      stimulatingNum = s->num;   // FIX: track which socket
      tStimulusTimeout.setInterval(tdelay + timeout_ms + 1000);
      tStimulusTimeout.restartDelayed();
      stimulating = true;
    }
  }
}

// ============================================================
// WebSocket Events
// ============================================================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  IPAddress ip = webSocket.remoteIP(num);

  switch (type) {

    case WStype_DISCONNECTED:
      if ((int)num == appConnected) appConnected = -1;
      for (int i = 0; i < sensorList.size(); i++) {
        if (sensorList.get(i)->num == (int)num) {
          removeSensorAt(i);                       // FIX: uses safe helper
          if (appConnected != -1) sendSensorList(appConnected);
          break;
        }
      }
      break;

    case WStype_CONNECTED:
      break;

    case WStype_TEXT: {
      StaticJsonDocument<512> root;
      if (deserializeJson(root, payload)) break;
      const char* jtype = root["type"];
      if (!jtype) break;

      // ---- Node registers ----
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
            String m; serializeJson(a, m); webSocket.sendTXT(num, m);
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
          String m; serializeJson(a, m); webSocket.sendTXT(num, m);
        }
        if (appConnected != -1) sendSensorList(appConnected);
      }

      if (strcmp(jtype, "list_sensors") == 0) sendSensorList((int)num);

      // ---- Hit/miss response ----
      if (strcmp(jtype, "response") == 0) {
        // FIX: ignore late responses if test was stopped
        if (!isTesting) break;

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

      if (strcmp(jtype, "start_test") == 0) { isTesting = true; lastSensor = -1; }

      if (strcmp(jtype, "stop_test") == 0) {
        isTesting      = false;
        stimulating    = false;
        stimulatingNum = -1;
        tStimulusTimeout.disable();
        sendStats(); // FIX: push updated is_testing=false to UI immediately
      }

      if (strcmp(jtype, "clear_stats") == 0) { resetStats(); sendStats(); }

      if (strcmp(jtype, "clear_nodes") == 0) {
        // FIX: free memory before clearing
        for (int i = 0; i < sensorList.size(); i++) delete sensorList.get(i);
        sensorList.clear();
        stimulating = false; stimulatingNum = -1; tStimulusTimeout.disable();
        sendSensorList(appConnected);
      }

      if (strcmp(jtype, "remove_node") == 0) {
        String targetMac = root["mac"] | String("");
        for (int i = 0; i < sensorList.size(); i++) {
          if (sensorList.get(i)->mac == targetMac) {
            removeSensorAt(i); break;             // FIX: uses safe helper
          }
        }
        sendSensorList(appConnected);             // null-guarded inside sendSensorList
      }

      if (strcmp(jtype, "app_connected") == 0) {
        appConnected = (int)num;
        isTesting    = false;
        stimulating  = false;
        stimulatingNum = -1;
        tStimulusTimeout.disable();
        sendSensorList(appConnected);
      }

      if (strcmp(jtype, "blink_all") == 0)
        for (int i = 0; i < sensorList.size(); i++)
          webSocket.sendTXT((uint8_t)sensorList.get(i)->num, "{\"type\":\"blink\"}");

      if (strcmp(jtype, "config") == 0) {
        tmode               = String((const char*)root["tmode"]);
        min_delay           = root["min_delay"]           | 0;
        max_delay           = root["max_delay"]           | 500;
        mim_timeout         = root["mim_timeout"]         | 1000;
        max_timeout         = root["max_timeout"]         | 2000;
        min_detection_range = root["min_detection_range"] | 0;
        max_detection_range = root["max_detection_range"] | 50;
        if (mim_timeout < 100)             mim_timeout = 100;
        if (max_timeout < mim_timeout) max_timeout = mim_timeout;
        for (int i = 0; i < sensorList.size(); i++)
          webSocket.sendTXT((uint8_t)sensorList.get(i)->num, "{\"type\":\"blink\"}");
      }

      break;
    }
    default: break;
  }
}

// ============================================================
// Timeout callback
// ============================================================
void onStimulusTimeout() {
  if (!stimulating || !isTesting) return; // FIX: guard both flags
  stimulating    = false;
  stimulatingNum = -1;
  test_errors++;
  sendStats();
}

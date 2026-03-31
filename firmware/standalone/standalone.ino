// airpulse standalone — M5StickC serves its own dashboard
// connect to same WiFi, open http://<device-ip> in browser
// no laptop or Rust backend needed

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "esp_wifi.h"
#include <ArduinoJson.h>

#define WIFI_SSID  "Altibox150666"
#define WIFI_PASS  "s7D77XZh"
#define HOP_MS     2000
#define MAX_DEVICES 64
#define RING_SIZE   32

// ── Packet ring buffer ────────────────────────────────────────
struct PktRecord {
  uint8_t src[6];
  uint8_t dst[6];
  int8_t  rssi;
  uint8_t ftype;
  uint8_t fsubtype;
  uint8_t channel;
  char    os_hint[12];
  char    ssid[33];
  bool    rand_mac;
};

volatile PktRecord ring[RING_SIZE];
volatile uint32_t  ring_head = 0;
volatile uint32_t  ring_tail = 0;
volatile uint32_t  pkt_count = 0;

// ── Device state ──────────────────────────────────────────────
struct Device {
  uint8_t  mac[6];
  int8_t   rssi;
  uint64_t frames;
  char     os_hint[12];
  char     probed_ssid[33];
  bool     flagged;
  bool     rand_mac;
  uint32_t last_seen;
};

Device devices[MAX_DEVICES];
uint8_t dev_count = 0;
SemaphoreHandle_t dev_mutex;

// ── Channel hopping ───────────────────────────────────────────
uint8_t  channels[] = {1, 6, 11};
uint8_t  ch_idx     = 0;
uint32_t last_hop   = 0;

// ── Web server ────────────────────────────────────────────────
AsyncWebServer  server(80);
AsyncWebSocket  ws("/ws");

uint32_t pps = 0, last_pps_count = 0;
uint32_t start_ms = 0;

// ── Helpers ───────────────────────────────────────────────────
void mac_str(const uint8_t* mac, char* out) {
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

bool is_rand(const uint8_t* mac) { return (mac[0] & 0x02) != 0; }

void fingerprint(const uint8_t* payload, uint16_t len, char* os, char* ssid) {
  strcpy(os, "unknown"); ssid[0] = 0;
  if (len < 26) return;
  const uint8_t* body = payload + 24;
  uint16_t bl = len - 24;
  bool ms=false, apple=false, ht=false;
  uint8_t rates=0; bool ext=false;
  uint16_t off=0;
  while (off+2 <= bl) {
    uint8_t id=body[off], ilen=body[off+1];
    if (off+2+ilen > bl) break;
    const uint8_t* d = body+off+2;
    if (id==0 && ilen>0 && ilen<33) { memcpy(ssid,d,ilen); ssid[ilen]=0; }
    if (id==1)  rates=ilen;
    if (id==50) ext=true;
    if (id==45) ht=true;
    if (id==221 && ilen>=3) {
      if (d[0]==0&&d[1]==0x50&&d[2]==0xf2) ms=true;
      if (d[0]==0&&d[1]==0x17&&d[2]==0xf2) apple=true;
    }
    off += 2+ilen;
  }
  if (apple)              strcpy(os,"apple");
  else if (ms)            strcpy(os,"windows");
  else if (ht&&ext&&rates>=8) strcpy(os,"android");
  else if (rates>0&&!ht)  strcpy(os,"linux");
}

// ── Promiscuous callback (Core 1) ─────────────────────────────
void IRAM_ATTR pkt_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t* p = (wifi_promiscuous_pkt_t*)buf;
  uint32_t next = (ring_head+1)%RING_SIZE;
  if (next == ring_tail) return;
  const uint8_t* pl = p->payload;
  uint16_t plen = p->rx_ctrl.sig_len;
  PktRecord& r = (PktRecord&)ring[ring_head];
  memcpy(r.dst, pl+4,  6);
  memcpy(r.src, pl+10, 6);
  r.rssi     = p->rx_ctrl.rssi;
  r.ftype    = (pl[0]&0x0C)>>2;
  r.fsubtype = (pl[0]&0xF0)>>4;
  r.channel  = p->rx_ctrl.channel;
  r.rand_mac = is_rand(r.src);
  strcpy(r.os_hint,"unknown"); r.ssid[0]=0;
  if (r.ftype==0 && r.fsubtype==4 && plen>26)
    fingerprint(pl, plen, r.os_hint, r.ssid);
  ring_head = next;
  pkt_count++;
}

// ── Process ring → device table (Core 0 task) ─────────────────
void process_task(void* param) {
  while (true) {
    while (ring_tail != ring_head) {
      PktRecord pkt;
      memcpy(&pkt, (const void*)&ring[ring_tail], sizeof(PktRecord));
      ring_tail = (ring_tail+1)%RING_SIZE;

      if (xSemaphoreTake(dev_mutex, 10)) {
        // find or create device
        int idx = -1;
        for (int i=0; i<dev_count; i++) {
          if (memcmp(devices[i].mac, pkt.src, 6)==0) { idx=i; break; }
        }
        if (idx < 0 && dev_count < MAX_DEVICES) {
          idx = dev_count++;
          memcpy(devices[idx].mac, pkt.src, 6);
          devices[idx].frames = 0;
          devices[idx].flagged = false;
        }
        if (idx >= 0) {
          devices[idx].rssi      = pkt.rssi;
          devices[idx].frames++;
          devices[idx].last_seen = millis();
          devices[idx].rand_mac  = pkt.rand_mac;
          if (strlen(pkt.os_hint)>0 && strcmp(pkt.os_hint,"unknown")!=0)
            strncpy(devices[idx].os_hint, pkt.os_hint, 12);
          if (strlen(pkt.ssid)>0)
            strncpy(devices[idx].probed_ssid, pkt.ssid, 32);
          // flag deauth bursts (simple: any deauth flags device)
          if (pkt.ftype==0 && pkt.fsubtype==12)
            devices[idx].flagged = true;
        }
        xSemaphoreGive(dev_mutex);
      }
    }
    vTaskDelay(2 / portTICK_PERIOD_MS);
  }
}

// ── Build JSON state ──────────────────────────────────────────
String build_json() {
  String j = "{\"devices\":[";
  if (xSemaphoreTake(dev_mutex, 50)) {
    for (int i=0; i<dev_count; i++) {
      char mac[18]; mac_str(devices[i].mac, mac);
      uint32_t ago = (millis() - devices[i].last_seen) / 1000;
      if (i>0) j += ",";
      j += "{\"mac\":\""; j += mac;
      j += "\",\"rssi\":"; j += devices[i].rssi;
      j += ",\"frames\":"; j += (uint32_t)devices[i].frames;
      j += ",\"os\":\""; j += devices[i].os_hint;
      j += "\",\"ssid\":\""; j += devices[i].probed_ssid;
      j += "\",\"rand\":"; j += devices[i].rand_mac?"true":"false";
      j += ",\"flagged\":"; j += devices[i].flagged?"true":"false";
      j += ",\"ago\":"; j += ago;
      j += "}";
    }
    xSemaphoreGive(dev_mutex);
  }
  j += "],\"stats\":{\"total\":";
  j += pkt_count;
  j += ",\"pps\":"; j += pps;
  j += ",\"devices\":"; j += dev_count;
  j += ",\"uptime\":"; j += (millis()-start_ms)/1000;
  j += ",\"ch\":"; j += channels[ch_idx];
  j += "}}";
  return j;
}

// ── Dashboard HTML (minimal, served from flash) ───────────────
const char HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang=en><head>
<meta charset=UTF-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>airpulse</title>
<link href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@300;400;500&display=swap" rel=stylesheet>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#000;--s1:#0a0a0a;--s2:#111;--b1:#1a1a1a;--b2:#222;--t0:#fff;--t1:#888;--t2:#333;--g:#00ff88;--r:#ff3355;--b:#4488ff;--a:#ffaa00}
html,body{height:100%;overflow:hidden;background:var(--bg)}
body{font-family:'JetBrains Mono',monospace;font-size:11px;color:var(--t0);display:grid;grid-template-rows:40px 1fr 36px;height:100vh}
#top{background:var(--bg);border-bottom:1px solid var(--b1);display:flex;align-items:center;padding:0 16px;gap:14px}
.logo{font-size:12px;letter-spacing:.3em;font-weight:400}.logo b{color:var(--g);font-weight:400}
.vsep{width:1px;height:12px;background:var(--b2)}
.badge{font-size:8px;letter-spacing:.16em;padding:3px 8px;border:1px solid var(--b1);color:var(--t2)}
.badge.on{color:var(--g);border-color:#00ff8840;animation:gp 2s infinite}
@keyframes gp{0%,100%{opacity:1}50%{opacity:.4}}
.kv{display:flex;flex-direction:column;align-items:flex-end}
.kn{font-size:16px;font-weight:300;line-height:1;transition:color .3s}
.kl{font-size:7px;letter-spacing:.2em;color:var(--t2);margin-top:2px}
.sp{flex:1}
#main{display:grid;grid-template-columns:1fr 260px;overflow:hidden;min-height:0}
#left{overflow-y:auto;scrollbar-width:thin;scrollbar-color:var(--b1) transparent;border-right:1px solid var(--b1)}
table{width:100%;border-collapse:collapse}
thead th{padding:5px 16px;text-align:left;font-size:7px;letter-spacing:.16em;color:var(--t2);border-bottom:1px solid var(--b1);background:var(--bg);position:sticky;top:0}
tbody tr{border-bottom:1px solid var(--b1);cursor:pointer;transition:background .1s}
tbody tr:hover{background:var(--s1)}
tbody tr.sel{background:var(--s2)}
tbody tr.fl{background:#ff33550a}
td{padding:5px 16px;color:var(--t1);font-size:9px}
.tm{color:var(--t0)}
.rbar{height:1px;background:var(--b2);width:28px;display:inline-block;vertical-align:middle;margin-right:4px;position:relative;top:-1px;overflow:hidden}
.rfill{position:absolute;top:0;left:0;height:100%}
#right{background:var(--bg);display:flex;flex-direction:column;overflow:hidden}
#detail-empty{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:8px;color:var(--t2);font-size:9px;letter-spacing:.14em}
#detail-content{flex:1;overflow-y:auto;scrollbar-width:none;display:none}
#detail-content::-webkit-scrollbar{display:none}
.dp{padding:16px 14px 0}
.d-mac{font-size:9px;color:var(--t2);letter-spacing:.06em;margin-bottom:4px}
.d-name{font-size:20px;font-weight:300;line-height:1.2;margin-bottom:16px;letter-spacing:-.01em}
.drow{display:flex;justify-content:space-between;align-items:baseline;padding:7px 0;border-bottom:1px solid var(--b1)}
.drow:last-child{border-bottom:none}
.dk{font-size:7px;letter-spacing:.18em;color:var(--t2)}
.dv{font-size:11px}
.dflag{margin:10px 14px;padding:8px;border:1px solid #ff335540;color:var(--r);font-size:8px;letter-spacing:.12em;display:none}
.dflag.show{display:block}
.dsec{padding:10px 14px 4px;font-size:7px;letter-spacing:.18em;color:var(--t2);border-top:1px solid var(--b1);margin-top:4px}
#feed-panel{padding:0 14px 12px;overflow-y:auto;flex:1;scrollbar-width:none}
#feed-panel::-webkit-scrollbar{display:none}
.fr{display:flex;gap:6px;padding:3px 0;border-bottom:1px solid var(--b1);align-items:baseline;font-size:9px}
.fr:last-child{border-bottom:none}
.fts{font-size:8px;color:var(--t2);white-space:nowrap;flex-shrink:0}
.fm{color:var(--t1);word-break:break-all;line-height:1.3}
.ftag{font-size:7px;padding:1px 3px;border-radius:1px;font-weight:500;letter-spacing:.06em;margin-right:3px}
.ft-new{color:var(--g);border:1px solid #00ff8830}
.ft-deauth{color:var(--r);border:1px solid #ff335530}
.ft-probe{color:var(--a);border:1px solid #ffaa0030}
#status-bar{border-top:1px solid var(--b1);background:var(--bg);display:flex;align-items:center;padding:0 16px;gap:16px}
.sb{font-size:8px;letter-spacing:.16em;color:var(--t2)}
.sb span{color:var(--t1)}
.osdot{display:inline-block;width:6px;height:6px;border-radius:50%;margin-right:4px;vertical-align:middle}
</style></head><body>
<div id=top>
  <div class=logo>AIR<b>PULSE</b></div>
  <div class=vsep></div>
  <div class=badge id=cb>OFFLINE</div>
  <div class=kv><div class=kn id=sd style="color:var(--b)">0</div><div class=kl>DEVICES</div></div>
  <div class=kv><div class=kn id=sp style="color:var(--g)">0</div><div class=kl>PKT/S</div></div>
  <div class=kv><div class=kn id=st>0</div><div class=kl>TOTAL</div></div>
  <div class=sp></div>
  <div class=kv><div class=kn id=su style="color:var(--t2)">ch1</div><div class=kl>CHANNEL</div></div>
</div>
<div id=main>
  <div id=left>
    <table>
      <thead><tr><th>MAC</th><th>OS</th><th>SSID PROBED</th><th>RSSI</th><th>FRAMES</th><th>SEEN</th></tr></thead>
      <tbody id=dt></tbody>
    </table>
  </div>
  <div id=right>
    <div id=detail-empty>
      <svg width=20 height=20 viewBox="0 0 24 24" fill=none stroke=#2a2a2a stroke-width=1.5><circle cx=12 cy=12 r=10/><line x1=12 y1=8 x2=12 y2=12/><line x1=12 y1=16 x2=12.01 y2=16/></svg>
      <span>click a device</span>
    </div>
    <div id=detail-content>
      <div class=dp>
        <div class=d-mac id=d-mac></div>
        <div class=d-name id=d-name></div>
        <div class=drow><span class=dk>OS</span><span class=dv id=d-os></span></div>
        <div class=drow><span class=dk>RSSI</span><span class=dv id=d-rssi></span></div>
        <div class=drow><span class=dk>FRAMES</span><span class=dv id=d-frames></span></div>
        <div class=drow><span class=dk>LAST SEEN</span><span class=dv id=d-seen></span></div>
        <div class=drow><span class=dk>PROBED SSID</span><span class=dv id=d-ssid></span></div>
        <div class=drow><span class=dk>MAC TYPE</span><span class=dv id=d-rand></span></div>
      </div>
      <div class=dflag id=d-flag>ANOMALY DETECTED</div>
      <div class=dsec>RECENT EVENTS</div>
      <div id=feed-panel></div>
    </div>
  </div>
</div>
<div id=status-bar>
  <span class=sb>STANDALONE MODE</span>
  <div class=vsep></div>
  <span class=sb>UPTIME <span id=sbu>0s</span></span>
  <div class=vsep></div>
  <span class=sb>OS LEGEND
    <span><span class=osdot style=background:#ccc></span>Apple</span>
    <span><span class=osdot style=background:#4488ff></span>Windows</span>
    <span><span class=osdot style=background:#00ff88></span>Android</span>
    <span><span class=osdot style=background:#ffaa00></span>Linux</span>
  </span>
</div>
<script>
const osC=o=>({apple:'#ccc',windows:'#4488ff',android:'#00ff88',linux:'#ffaa00'}[o]||'#333');
const osL=o=>({apple:'Apple',windows:'Windows',android:'Android',linux:'Linux',unknown:'Unknown'}[o]||'?');
let devs=[],sel=null,feed=[];
const ts=()=>new Date().toTimeString().slice(0,8);

function render(){
  const sorted=[...devs].sort((a,b)=>b.frames-a.frames);
  document.getElementById('dt').innerHTML=sorted.map(d=>{
    const pct=Math.min(100,Math.max(0,(d.rssi+100)*2));
    const rc=d.rssi>-60?'#00ff88':d.rssi>-75?'#ffaa00':'#ff3355';
    return `<tr class="${d.mac===sel?'sel':d.flagged?'fl':''}" onclick="pick('${d.mac}')">
      <td class=tm>${d.mac}</td>
      <td><span class=osdot style="background:${osC(d.os)}"></span>${osL(d.os)}</td>
      <td style="color:var(--t1)">${d.ssid||'—'}</td>
      <td><div class=rbar><div class=rfill style="width:${pct*.5}%;background:${rc}"></div></div><span style="color:${rc}">${d.rssi}</span></td>
      <td>${d.frames.toLocaleString()}</td>
      <td style="color:${d.ago===0?'#00ff88':'var(--t2)'}">${d.ago===0?'now':d.ago+'s'}</td>
    </tr>`;
  }).join('');
  if(sel){
    const d=devs.find(x=>x.mac===sel);
    if(d) updateDetail(d);
  }
}

function updateDetail(d){
  document.getElementById('detail-empty').style.display='none';
  document.getElementById('detail-content').style.display='flex';
  document.getElementById('detail-content').style.flexDirection='column';
  document.getElementById('d-mac').textContent=d.mac;
  const nm=document.getElementById('d-name');
  nm.textContent=osL(d.os);
  nm.style.color=osC(d.os);
  const osEl=document.getElementById('d-os');
  osEl.textContent=osL(d.os);
  osEl.style.color=osC(d.os);
  const rssiEl=document.getElementById('d-rssi');
  const rc=d.rssi>-60?'#00ff88':d.rssi>-75?'#ffaa00':'#ff3355';
  rssiEl.textContent=d.rssi+' dBm';
  rssiEl.style.color=rc;
  document.getElementById('d-frames').textContent=d.frames.toLocaleString();
  document.getElementById('d-seen').textContent=d.ago===0?'now':d.ago+'s ago';
  document.getElementById('d-ssid').textContent=d.ssid||'—';
  const rEl=document.getElementById('d-rand');
  rEl.textContent=d.rand?'randomized':'permanent';
  rEl.style.color=d.rand?'#ffaa00':'#888';
  const fl=document.getElementById('d-flag');
  d.flagged?fl.classList.add('show'):fl.classList.remove('show');
  const fp=document.getElementById('feed-panel');
  fp.innerHTML=feed.filter(f=>f.mac===d.mac||!f.mac).slice(0,20).map(f=>
    `<div class=fr><span class=fts>${f.ts}</span><span class=fm><span class="ftag ft-${f.cls}">${f.tag}</span>${f.msg}</span></div>`
  ).join('');
}

function pick(mac){
  sel=mac;
  render();
  const d=devs.find(x=>x.mac===mac);
  if(d) updateDetail(d);
}
window.pick=pick;

let prevDevs=new Map();
function processFeed(newDevs){
  newDevs.forEach(d=>{
    const prev=prevDevs.get(d.mac);
    if(!prev) feed.unshift({ts:ts(),cls:'new',tag:'NEW',msg:osL(d.os)+' device · '+d.mac.slice(-8),mac:d.mac});
    if(d.flagged&&(!prev||!prev.flagged)) feed.unshift({ts:ts(),cls:'deauth',tag:'DEAUTH',msg:'anomaly · '+d.mac.slice(-8),mac:d.mac});
    if(d.ssid&&d.ssid.length>0&&(!prev||prev.ssid!==d.ssid)) feed.unshift({ts:ts(),cls:'probe',tag:'PROBE',msg:'"'+d.ssid+'" · '+d.mac.slice(-8),mac:d.mac});
    prevDevs.set(d.mac,{...d});
  });
  feed=feed.slice(0,60);
}

const cb=document.getElementById('cb');
function connect(){
  const ws=new WebSocket('ws://'+location.host+'/ws');
  ws.onopen=()=>{cb.textContent='LIVE';cb.className='badge on';};
  ws.onclose=()=>{cb.textContent='OFFLINE';cb.className='badge';setTimeout(connect,2000);};
  ws.onmessage=e=>{
    const data=JSON.parse(e.data);
    processFeed(data.devices||[]);
    devs=data.devices||[];
    const s=data.stats||{};
    document.getElementById('sd').textContent=s.devices||0;
    document.getElementById('sp').textContent=s.pps||0;
    document.getElementById('st').textContent=(s.total||0).toLocaleString();
    document.getElementById('su').textContent=s.uptime>3600?Math.floor(s.uptime/3600)+'h':s.uptime>60?Math.floor(s.uptime/60)+'m':s.uptime+'s';
    document.getElementById('sbu').textContent=document.getElementById('su').textContent;
    document.getElementById('su').textContent='ch'+(s.ch||1);
    render();
  };
}
connect();
</script></body></html>)rawhtml";

// ── WebSocket event ───────────────────────────────────────────
void on_ws(AsyncWebSocket* server, AsyncWebSocketClient* client,
           AwsEventType type, void* arg, uint8_t* data, size_t len) {
  // no-op, we push only
}

// ── Broadcast task ────────────────────────────────────────────
void broadcast_task(void* param) {
  while (true) {
    String json = build_json();
    ws.textAll(json);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  start_ms = millis();

  dev_mutex = xSemaphoreCreateMutex();
  memset(devices, 0, sizeof(devices));

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("connecting wifi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500); Serial.print("."); tries++;
  }
  if (WiFi.status() != WL_CONNECTED) { Serial.println("FAILED"); ESP.restart(); }
  Serial.print("\nip: "); Serial.println(WiFi.localIP());
  Serial.println("open browser to: http://" + WiFi.localIP().toString());

  // serve dashboard
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", HTML);
  });
  ws.onEvent(on_ws);
  server.addHandler(&ws);
  server.begin();

  // start promiscuous
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(pkt_cb);
  esp_wifi_set_channel(channels[ch_idx], WIFI_SECOND_CHAN_NONE);

  xTaskCreatePinnedToCore(process_task,  "proc",   8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(broadcast_task,"bcast",  4096, NULL, 1, NULL, 0);

  Serial.println("ready");
}

void loop() {
  uint32_t now = millis();
  // channel hop
  if (now - last_hop > HOP_MS) {
    ch_idx = (ch_idx+1) % 3;
    esp_wifi_set_channel(channels[ch_idx], WIFI_SECOND_CHAN_NONE);
    last_hop = now;
  }
  // pps
  static uint32_t last_pps_t = 0;
  if (now - last_pps_t >= 1000) {
    pps = pkt_count - last_pps_count;
    last_pps_count = pkt_count;
    last_pps_t = now;
  }
  ws.cleanupClients();
  delay(10);
}

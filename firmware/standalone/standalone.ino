// airpulse standalone
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "esp_wifi.h"

#define WIFI_SSID  "Altibox150666"
#define WIFI_PASS  "s7D77XZh"
#define HOP_MS     2000
#define MAX_DEV    64
#define RING_SZ    32

struct Pkt {
  uint8_t src[6], dst[6];
  int8_t rssi;
  uint8_t ftype, fsub, ch;
  char os[12], ssid[33];
  bool rnd;
};

volatile Pkt ring[RING_SZ];
volatile uint32_t rhead=0, rtail=0, pktc=0;

struct Dev {
  uint8_t mac[6];
  int8_t rssi;
  uint64_t frames;
  char os[12], ssid[33];
  bool flagged, rnd;
  uint32_t last;
};

Dev devs[MAX_DEV];
uint8_t dcnt=0;
SemaphoreHandle_t dmx;
uint8_t chs[]={1,6,11};
uint8_t chi=0;
uint32_t lhop=0, pps=0, lpps=0, sms=0;

AsyncWebServer srv(80);
AsyncWebSocket ws("/ws");

void mac2s(const uint8_t* m, char* o){
  snprintf(o,18,"%02x:%02x:%02x:%02x:%02x:%02x",m[0],m[1],m[2],m[3],m[4],m[5]);
}
bool isrnd(const uint8_t* m){return(m[0]&0x02)!=0;}

void fprint(const uint8_t* p, uint16_t l, char* os, char* ssid){
  strcpy(os,"unknown"); ssid[0]=0;
  if(l<26)return;
  const uint8_t* b=p+24; uint16_t bl=l-24;
  bool ms=false,apl=false,ht=false; uint8_t rc=0; bool ext=false;
  uint16_t o=0;
  while(o+2<=bl){
    uint8_t id=b[o],il=b[o+1];
    if(o+2+il>bl)break;
    const uint8_t* d=b+o+2;
    if(id==0&&il>0&&il<33){memcpy(ssid,d,il);ssid[il]=0;}
    if(id==1)rc=il; if(id==50)ext=true; if(id==45)ht=true;
    if(id==221&&il>=3){
      if(d[0]==0&&d[1]==0x50&&d[2]==0xf2)ms=true;
      if(d[0]==0&&d[1]==0x17&&d[2]==0xf2)apl=true;
    }
    o+=2+il;
  }
  if(apl)strcpy(os,"apple");
  else if(ms)strcpy(os,"windows");
  else if(ht&&ext&&rc>=8)strcpy(os,"android");
  else if(rc>0&&!ht)strcpy(os,"linux");
}

void IRAM_ATTR pkt_cb(void* buf, wifi_promiscuous_pkt_type_t t){
  wifi_promiscuous_pkt_t* p=(wifi_promiscuous_pkt_t*)buf;
  uint32_t nx=(rhead+1)%RING_SZ;
  if(nx==rtail)return;
  const uint8_t* pl=p->payload; uint16_t pl2=p->rx_ctrl.sig_len;
  Pkt& r=(Pkt&)ring[rhead];
  memcpy(r.dst,pl+4,6); memcpy(r.src,pl+10,6);
  r.rssi=p->rx_ctrl.rssi;
  r.ftype=(pl[0]&0x0C)>>2; r.fsub=(pl[0]&0xF0)>>4;
  r.ch=p->rx_ctrl.channel; r.rnd=isrnd(r.src);
  strcpy(r.os,"unknown"); r.ssid[0]=0;
  if(r.ftype==0&&r.fsub==4&&pl2>26)fprint(pl,pl2,r.os,r.ssid);
  rhead=nx; pktc++;
}

void proc_task(void* p){
  while(true){
    while(rtail!=rhead){
      Pkt pk; memcpy(&pk,(const void*)&ring[rtail],sizeof(Pkt));
      rtail=(rtail+1)%RING_SZ;
      if(xSemaphoreTake(dmx,10)){
        int idx=-1;
        for(int i=0;i<dcnt;i++){if(memcmp(devs[i].mac,pk.src,6)==0){idx=i;break;}}
        if(idx<0&&dcnt<MAX_DEV){idx=dcnt++;memcpy(devs[idx].mac,pk.src,6);devs[idx].frames=0;devs[idx].flagged=false;}
        if(idx>=0){
          devs[idx].rssi=pk.rssi; devs[idx].frames++; devs[idx].last=millis();
          devs[idx].rnd=pk.rnd;
          if(strcmp(pk.os,"unknown")!=0)strncpy(devs[idx].os,pk.os,12);
          if(strlen(pk.ssid)>0)strncpy(devs[idx].ssid,pk.ssid,32);
          if(pk.ftype==0&&pk.fsub==12)devs[idx].flagged=true;
        }
        xSemaphoreGive(dmx);
      }
    }
    vTaskDelay(2/portTICK_PERIOD_MS);
  }
}

String mkjson(){
  String j="{\"devices\":[";
  if(xSemaphoreTake(dmx,50)){
    for(int i=0;i<dcnt;i++){
      char m[18]; mac2s(devs[i].mac,m);
      uint32_t ago=(millis()-devs[i].last)/1000;
      if(i>0)j+=",";
      j+="{\"mac\":\"";j+=m;
      j+="\",\"rssi\":";j+=devs[i].rssi;
      j+=",\"frames\":";j+=(uint32_t)devs[i].frames;
      j+=",\"os\":\"";j+=devs[i].os;
      j+="\",\"ssid\":\"";j+=devs[i].ssid;
      j+="\",\"rand\":";j+=devs[i].rnd?"true":"false";
      j+=",\"flagged\":";j+=devs[i].flagged?"true":"false";
      j+=",\"ago\":";j+=ago;j+="}";
    }
    xSemaphoreGive(dmx);
  }
  j+="],\"stats\":{\"total\":";j+=pktc;
  j+=",\"pps\":";j+=pps;
  j+=",\"devices\":";j+=dcnt;
  j+=",\"uptime\":";j+=(millis()-sms)/1000;
  j+=",\"ch\":";j+=chs[chi];j+="}}";
  return j;
}

void on_ws(AsyncWebSocket* s,AsyncWebSocketClient* c,AwsEventType t,void* a,uint8_t* d,size_t l){}

void bcast_task(void* p){
  while(true){ws.textAll(mkjson());vTaskDelay(1000/portTICK_PERIOD_MS);}
}

const char* getHTML(){
  static char buf[12000];
  strcpy(buf,"<!DOCTYPE html><html lang=en><head><meta charset=UTF-8><meta name=viewport content=\"width=device-width,initial-scale=1\"><title>airpulse</title>");
  strcat(buf,"<link href=\"https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@300;400&display=swap\" rel=stylesheet>");
  strcat(buf,"<style>*{box-sizing:border-box;margin:0;padding:0}:root{--bg:#000;--b1:#1a1a1a;--b2:#222;--g:#00ff88;--r:#ff3355;--b:#4488ff;--a:#ffaa00}html,body{height:100%;overflow:hidden;background:#000}body{font-family:'JetBrains Mono',monospace;font-size:11px;color:#fff;display:grid;grid-template-rows:40px 1fr 36px;height:100vh}");
  strcat(buf,"#top{background:#000;border-bottom:1px solid #1a1a1a;display:flex;align-items:center;padding:0 16px;gap:14px}.logo{font-size:12px;letter-spacing:.3em}.logo b{color:#00ff88;font-weight:400}.vsep{width:1px;height:12px;background:#222}.badge{font-size:8px;letter-spacing:.16em;padding:3px 8px;border:1px solid #1a1a1a;color:#333}.badge.on{color:#00ff88;border-color:#00ff8840;animation:gp 2s infinite}@keyframes gp{0%,100%{opacity:1}50%{opacity:.4}}.kv{display:flex;flex-direction:column;align-items:flex-end}.kn{font-size:16px;font-weight:300;line-height:1}.kl{font-size:7px;letter-spacing:.2em;color:#333;margin-top:2px}.sp{flex:1}");
  strcat(buf,"#main{display:grid;grid-template-columns:1fr 260px;overflow:hidden;min-height:0}#left{overflow-y:auto;border-right:1px solid #1a1a1a}table{width:100%;border-collapse:collapse}thead th{padding:5px 16px;text-align:left;font-size:7px;letter-spacing:.16em;color:#333;border-bottom:1px solid #1a1a1a;background:#000;position:sticky;top:0}tbody tr{border-bottom:1px solid #1a1a1a;cursor:pointer;transition:background .1s}tbody tr:hover{background:#0a0a0a}tbody tr.sel{background:#111}tbody tr.fl{background:#1a0505}td{padding:5px 16px;color:#888;font-size:9px}.tm{color:#fff}.rbar{height:1px;background:#222;width:28px;display:inline-block;vertical-align:middle;margin-right:4px;position:relative;top:-1px;overflow:hidden}.rfill{position:absolute;top:0;left:0;height:100%}");
  strcat(buf,"#right{background:#000;display:flex;flex-direction:column;overflow:hidden}#de{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:8px;color:#333;font-size:9px;letter-spacing:.14em}#dc{flex:1;overflow-y:auto;scrollbar-width:none;display:none;flex-direction:column}#dc::-webkit-scrollbar{display:none}.dp{padding:16px 14px 0}.d-mac{font-size:9px;color:#333;letter-spacing:.06em;margin-bottom:4px}.d-name{font-size:20px;font-weight:300;line-height:1.2;margin-bottom:16px}.drow{display:flex;justify-content:space-between;padding:7px 0;border-bottom:1px solid #1a1a1a}.drow:last-child{border-bottom:none}.dk{font-size:7px;letter-spacing:.18em;color:#333}.dv{font-size:11px}.dflag{margin:10px 14px;padding:8px;border:1px solid #ff335540;color:#ff3355;font-size:8px;letter-spacing:.12em;display:none}.dflag.show{display:block}.dsec{padding:10px 14px 4px;font-size:7px;letter-spacing:.18em;color:#333;border-top:1px solid #1a1a1a;margin-top:4px}#fp{padding:0 14px 12px;overflow-y:auto;flex:1;scrollbar-width:none}.fr{display:flex;gap:6px;padding:3px 0;border-bottom:1px solid #111;font-size:9px}.fts{font-size:8px;color:#333;white-space:nowrap;flex-shrink:0}.fm{color:#888;word-break:break-all;line-height:1.3}.ftag{font-size:7px;padding:1px 3px;font-weight:500;letter-spacing:.06em;margin-right:3px}.fn{color:#00ff88;border:1px solid #00ff8830}.fd{color:#ff3355;border:1px solid #ff335530}.fp2{color:#ffaa00;border:1px solid #ffaa0030}");
  strcat(buf,"#sb{border-top:1px solid #1a1a1a;background:#000;display:flex;align-items:center;padding:0 16px;gap:16px}.sbt{font-size:8px;letter-spacing:.16em;color:#333}.sbt span{color:#666}.osd{display:inline-block;width:5px;height:5px;border-radius:50%;margin-right:3px;vertical-align:middle}</style></head><body>");
  strcat(buf,"<div id=top><div class=logo>AIR<b>PULSE</b></div><div class=vsep></div><div class=badge id=cb>OFFLINE</div><div class=kv><div class=kn id=sd style=\"color:#4488ff\">0</div><div class=kl>DEVICES</div></div><div class=kv><div class=kn id=sp style=\"color:#00ff88\">0</div><div class=kl>PKT/S</div></div><div class=kv><div class=kn id=st>0</div><div class=kl>TOTAL</div></div><div class=sp></div><div class=kv><div class=kn id=su style=\"color:#333\">ch1</div><div class=kl>CHANNEL</div></div></div>");
  strcat(buf,"<div id=main><div id=left><table><thead><tr><th>MAC</th><th>OS</th><th>SSID</th><th>RSSI</th><th>FRAMES</th><th>SEEN</th></tr></thead><tbody id=dt></tbody></table></div>");
  strcat(buf,"<div id=right><div id=de><svg width=20 height=20 viewBox=\"0 0 24 24\" fill=none stroke=#222 stroke-width=1.5><circle cx=12 cy=12 r=10/><line x1=12 y1=8 x2=12 y2=12/><line x1=12 y1=16 x2=12.01 y2=16/></svg><span>click a device</span></div>");
  strcat(buf,"<div id=dc><div class=dp><div class=d-mac id=dm></div><div class=d-name id=dn></div><div class=drow><span class=dk>OS</span><span class=dv id=dos></span></div><div class=drow><span class=dk>RSSI</span><span class=dv id=dr></span></div><div class=drow><span class=dk>FRAMES</span><span class=dv id=df></span></div><div class=drow><span class=dk>SEEN</span><span class=dv id=dls></span></div><div class=drow><span class=dk>SSID</span><span class=dv id=dss></span></div><div class=drow><span class=dk>MAC</span><span class=dv id=drd></span></div></div><div class=dflag id=dfl>ANOMALY</div><div class=dsec>EVENTS</div><div id=fp></div></div></div></div>");
  strcat(buf,"<div id=sb><span class=sbt>STANDALONE</span><div class=vsep></div><span class=sbt>UP <span id=sbu>0s</span></span><div class=vsep></div><span class=sbt><span><span class=osd style=background:#ccc></span>Apple</span> <span><span class=osd style=background:#4488ff></span>Win</span> <span><span class=osd style=background:#00ff88></span>Android</span> <span><span class=osd style=background:#ffaa00></span>Linux</span></span></div>");
  strcat(buf,"<script>var oC={apple:'#ccc',windows:'#4488ff',android:'#00ff88',linux:'#ffaa00'};var oL={apple:'Apple',windows:'Windows',android:'Android',linux:'Linux',unknown:'Unknown'};var dv=[],sl=null,fd=[],pv={};function ts(){return new Date().toTimeString().slice(0,8);}function gc(o){return oC[o]||'#333';}function gl(o){return oL[o]||'?';}");
  strcat(buf,"function rnd(){var s=dv.slice().sort(function(a,b){return b.frames-a.frames;});var h='';for(var i=0;i<s.length;i++){var d=s[i];var p=Math.min(100,Math.max(0,(d.rssi+100)*2));var rc=d.rssi>-60?'#00ff88':d.rssi>-75?'#ffaa00':'#ff3355';var cl=d.mac===sl?'sel':d.flagged?'fl':'';h+='<tr class=\"'+cl+'\" onclick=\"pk(\\''+d.mac+'\\')\">';h+='<td class=\"tm\">'+d.mac+'</td><td><span class=\"osd\" style=\"background:'+gc(d.os)+'\"></span>'+gl(d.os)+'</td><td>'+(d.ssid||'-')+'</td><td><div class=\"rbar\"><div class=\"rfill\" style=\"width:'+(p*.5)+'%;background:'+rc+'\"></div></div><span style=\"color:'+rc+'\">'+d.rssi+'</span></td><td>'+d.frames.toLocaleString()+'</td><td style=\"color:'+(d.ago===0?'#00ff88':'#333')+'\">'+( d.ago===0?'now':d.ago+'s')+'</td></tr>';}document.getElementById('dt').innerHTML=h;if(sl){var d2=null;for(var j=0;j<dv.length;j++){if(dv[j].mac===sl){d2=dv[j];break;}}if(d2)sd(d2);}}");
  strcat(buf,"function sd(d){document.getElementById('de').style.display='none';var dc=document.getElementById('dc');dc.style.display='flex';document.getElementById('dm').textContent=d.mac;var dn=document.getElementById('dn');dn.textContent=gl(d.os);dn.style.color=gc(d.os);var dos=document.getElementById('dos');dos.textContent=gl(d.os);dos.style.color=gc(d.os);var rc=d.rssi>-60?'#00ff88':d.rssi>-75?'#ffaa00':'#ff3355';var dr=document.getElementById('dr');dr.textContent=d.rssi+' dBm';dr.style.color=rc;document.getElementById('df').textContent=d.frames.toLocaleString();document.getElementById('dls').textContent=d.ago===0?'now':d.ago+'s ago';document.getElementById('dss').textContent=d.ssid||'-';var drd=document.getElementById('drd');drd.textContent=d.rand?'randomized':'permanent';drd.style.color=d.rand?'#ffaa00':'#888';var fl=document.getElementById('dfl');if(d.flagged)fl.classList.add('show');else fl.classList.remove('show');var fh='';var n=0;for(var i=0;i<fd.length&&n<15;i++){if(!fd[i].mac||fd[i].mac===d.mac){fh+='<div class=\"fr\"><span class=\"fts\">'+fd[i].ts+'</span><span class=\"fm\"><span class=\"ftag '+fd[i].c+'\">'+fd[i].t+'</span>'+fd[i].m+'</span></div>';n++;}}document.getElementById('fp').innerHTML=fh;}");
  strcat(buf,"function pk(m){sl=m;rnd();var d=null;for(var i=0;i<dv.length;i++){if(dv[i].mac===m){d=dv[i];break;}}if(d)sd(d);}function pf(nd){for(var i=0;i<nd.length;i++){var d=nd[i];var p=pv[d.mac];if(!p)fd.unshift({ts:ts(),c:'fn',t:'NEW',m:gl(d.os)+' '+d.mac.slice(-8),mac:d.mac});if(d.flagged&&(!p||!p.f))fd.unshift({ts:ts(),c:'fd',t:'DEAUTH',m:'anomaly '+d.mac.slice(-8),mac:d.mac});if(d.ssid&&d.ssid.length>0&&(!p||p.s!==d.ssid))fd.unshift({ts:ts(),c:'fp2',t:'PROBE',m:'\"'+d.ssid+'\" '+d.mac.slice(-8),mac:d.mac});pv[d.mac]={s:d.ssid,f:d.flagged};}if(fd.length>60)fd=fd.slice(0,60);}");
  strcat(buf,"var cb=document.getElementById('cb');function cn(){var ws=new WebSocket('ws://'+location.host+'/ws');ws.onopen=function(){cb.textContent='LIVE';cb.className='badge on';};ws.onclose=function(){cb.textContent='OFFLINE';cb.className='badge';setTimeout(cn,2000);};ws.onmessage=function(e){var data=JSON.parse(e.data);pf(data.devices||[]);dv=data.devices||[];var s=data.stats||{};document.getElementById('sd').textContent=s.devices||0;document.getElementById('sp').textContent=s.pps||0;document.getElementById('st').textContent=(s.total||0).toLocaleString();var u=s.uptime||0;var ut=u>3600?Math.floor(u/3600)+'h':u>60?Math.floor(u/60)+'m':u+'s';document.getElementById('sbu').textContent=ut;document.getElementById('su').textContent='ch'+(s.ch||1);rnd();};}cn();</script></body></html>");
  return buf;
}

void on_ws2(AsyncWebSocket* s,AsyncWebSocketClient* c,AwsEventType t,void* a,uint8_t* d,size_t l){}

void bcast2(void* p){
  while(true){ws.textAll(mkjson());vTaskDelay(1000/portTICK_PERIOD_MS);}
}

void setup(){
  Serial.begin(115200);
  sms=millis();
  dmx=xSemaphoreCreateMutex();
  memset(devs,0,sizeof(devs));
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID,WIFI_PASS);
  Serial.print("connecting");
  int tr=0;
  while(WiFi.status()!=WL_CONNECTED&&tr<30){delay(500);Serial.print(".");tr++;}
  if(WiFi.status()!=WL_CONNECTED){Serial.println("FAILED");ESP.restart();}
  Serial.print("\nip: ");Serial.println(WiFi.localIP());
  srv.on("/",HTTP_GET,[](AsyncWebServerRequest* r){r->send(200,"text/html",getHTML());});
  ws.onEvent(on_ws2);
  srv.addHandler(&ws);
  srv.begin();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(pkt_cb);
  esp_wifi_set_channel(chs[chi],WIFI_SECOND_CHAN_NONE);
  xTaskCreatePinnedToCore(proc_task,"proc",8192,NULL,1,NULL,0);
  xTaskCreatePinnedToCore(bcast2,"bcast",4096,NULL,1,NULL,0);
  Serial.println("ready");
}

void loop(){
  uint32_t now=millis();
  if(now-lhop>HOP_MS){chi=(chi+1)%3;esp_wifi_set_channel(chs[chi],WIFI_SECOND_CHAN_NONE);lhop=now;}
  static uint32_t lpt=0;
  if(now-lpt>=1000){pps=pktc-lpps;lpps=pktc;lpt=now;}
  ws.cleanupClients();
  delay(10);
}

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "esp_wifi.h"

#define WIFI_SSID     "Altibox150666"
#define WIFI_PASS     "s7D77XZh"
#define SERVER_IP     "192.168.10.134"
#define SERVER_PORT   8766
#define WS_PATH       "/esp"
#define RING_SIZE 64
#define HOP_INTERVAL_MS 2000

struct PktRecord {
  uint8_t src[6];
  uint8_t dst[6];
  int8_t  rssi;
  uint8_t frame_type;
  uint8_t frame_subtype;
  uint8_t channel;
  uint32_t ts;
};

volatile PktRecord ring[RING_SIZE];
volatile uint32_t ring_head = 0;
volatile uint32_t ring_tail = 0;
volatile uint32_t pkt_count = 0;

uint8_t channels[] = {1, 6, 11};
uint8_t ch_idx = 0;
bool hop_enabled = true;
uint32_t last_hop = 0;

WebSocketsClient ws;
bool ws_connected = false;
uint32_t led_off_at = 0;

void IRAM_ATTR pkt_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t* p = (wifi_promiscuous_pkt_t*)buf;
  uint32_t next = (ring_head + 1) % RING_SIZE;
  if (next == ring_tail) return;
  const uint8_t* payload = p->payload;
  uint8_t ftype    = (payload[0] & 0x0C) >> 2;
  uint8_t fsubtype = (payload[0] & 0xF0) >> 4;
  PktRecord& r = (PktRecord&)ring[ring_head];
  memcpy(r.dst, payload + 4, 6);
  memcpy(r.src, payload + 10, 6);
  r.rssi = p->rx_ctrl.rssi;
  r.frame_type = ftype;
  r.frame_subtype = fsubtype;
  r.channel = p->rx_ctrl.channel;
  r.ts = millis();
  ring_head = next;
  pkt_count++;
}

void mac_str(const uint8_t* mac, char* out) {
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char* frame_type_str(uint8_t ftype, uint8_t fsubtype) {
  if (ftype == 0) {
    if (fsubtype == 4)  return "probe";
    if (fsubtype == 10) return "disassoc";
    if (fsubtype == 12) return "deauth";
    if (fsubtype == 8)  return "beacon";
    return "mgmt";
  }
  if (ftype == 2) return "data";
  return "ctrl";
}

void ws_event(WStype_t etype, uint8_t* payload, size_t len) {
  if (etype == WStype_CONNECTED) {
    ws_connected = true;
    Serial.println("ws connected");
  } else if (etype == WStype_DISCONNECTED) {
    ws_connected = false;
    Serial.println("ws disconnected");
  }
}

void sender_task(void* param) {
  while (true) {
    ws.loop();
    int drained = 0;
    while (ring_tail != ring_head && drained < 16) {
      PktRecord pkt;
      memcpy(&pkt, (const void*)&ring[ring_tail], sizeof(PktRecord));
      ring_tail = (ring_tail + 1) % RING_SIZE;
      drained++;
      if (!ws_connected) continue;
      char src_s[18], dst_s[18];
      mac_str(pkt.src, src_s);
      mac_str(pkt.dst, dst_s);
      const char* ftype = frame_type_str(pkt.frame_type, pkt.frame_subtype);
      char json[180];
      snprintf(json, sizeof(json),
        "{\"mac\":\"%s\",\"dst\":\"%s\",\"rssi\":%d,\"type\":\"%s\",\"ch\":%d,\"ts\":%lu}",
        src_s, dst_s, (int)pkt.rssi, ftype, (int)pkt.channel, (unsigned long)pkt.ts);
      ws.sendTXT(json);
    }
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("booting");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("wifi connecting");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("wifi FAILED, restarting");
    ESP.restart();
  }
  Serial.print("\nwifi ok: ");
  Serial.println(WiFi.localIP());

  ws.begin(SERVER_IP, SERVER_PORT, WS_PATH);
  ws.onEvent(ws_event);
  ws.setReconnectInterval(2000);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(pkt_callback);
  esp_wifi_set_channel(channels[ch_idx], WIFI_SECOND_CHAN_NONE);

  xTaskCreatePinnedToCore(sender_task, "sender", 8192, NULL, 1, NULL, 0);
  Serial.println("ready - capturing packets");
}

void loop() {
  uint32_t now = millis();
  if (hop_enabled && (now - last_hop > HOP_INTERVAL_MS)) {
    ch_idx = (ch_idx + 1) % 3;
    esp_wifi_set_channel(channels[ch_idx], WIFI_SECOND_CHAN_NONE);
    last_hop = now;
  }
  static uint32_t last_print = 0;
  if (now - last_print > 5000) {
    Serial.printf("pkts: %lu  ws: %s  ch: %d\n",
      pkt_count, ws_connected ? "ok" : "--", channels[ch_idx]);
    last_print = now;
  }
  delay(10);
}

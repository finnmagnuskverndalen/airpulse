# airpulse

Real-time WiFi packet intelligence. The M5StickC runs in promiscuous mode, captures 802.11 frames, fingerprints devices by OS, and serves a live dashboard directly from the device — no laptop, no backend, no cloud.

Open `http://<device-ip>` from any browser on the same network.

---

## what it does

- captures all 802.11 frames in range — probes, beacons, data, deauth, management
- identifies device OS from probe request information elements (Apple, Windows, Android, Linux)
- detects randomized/private MAC addresses
- tracks which SSIDs each device is probing for
- flags deauth anomalies in real-time
- serves a live dashboard over WebSocket from the device itself

---

## hardware

- **M5StickC** (ESP32-PICO-D4) — WiFi radio in promiscuous mode
- USB-C power — wall charger, powerbank, or laptop
- Any browser on the same WiFi network to view the dashboard

---

## setup

### 1. clone

```bash
git clone https://github.com/finnmagnuskverndalen/airpulse
cd airpulse
```

### 2. install tools

```bash
# Arduino CLI
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
export PATH=$PATH:$HOME/bin

# ESP32 board support (2.0.14 — newer versions break M5StickC library)
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core install esp32:esp32@2.0.14

# Required libraries
arduino-cli lib install "WebSockets" "ArduinoJson" "ESPAsyncWebServer" "AsyncTCP"

# Serial port access (Linux)
sudo usermod -aG dialout $USER && newgrp dialout
```

### 3. configure

Edit `firmware/standalone/standalone.ino` and set your WiFi credentials at the top:

```cpp
#define WIFI_SSID  "your-network"
#define WIFI_PASS  "your-password"
```

### 4. compile and flash

```bash
# Compile
arduino-cli compile --fqbn esp32:esp32:m5stick-c \
  --build-path /tmp/airpulse_build \
  firmware/standalone

# Flash (use sudo chmod 666 /dev/ttyUSB0 if permission denied)
arduino-cli upload --fqbn esp32:esp32:m5stick-c \
  --port /dev/ttyUSB0 \
  --input-dir /tmp/airpulse_build \
  firmware/standalone
```

### 5. find the device IP

```bash
sudo nmap -sn 192.168.1.0/24 | grep -i espressif
```

Or check your router's connected devices list for a device named `esp32-xxxxxx`.

### 6. open the dashboard

```
http://<device-ip>
```

Works from any device — phone, tablet, laptop — on the same WiFi network.

---

## dashboard

```
┌─────────────────────────────────────────────────────────────┐
│  AIRPULSE  ● LIVE   14 devices   342/s   48,291 total       │
├──────────────────────────────────────┬──────────────────────┤
│  MAC              OS   SSID   RSSI   │  Apple               │
│  aa:bb:cc:...  ● Apple  Home  -55   │  f8:4d:89:aa:cc:01   │
│  dc:a6:32:...  ● Linux   —    -62   │                       │
│  f8:32:e4:...  ● Win   Corp  -71   │  OS        Apple      │
│  b4:e6:2d:...  ● Andr  Home  -78   │  RSSI      -55 dBm    │
│  ...                                 │  FRAMES    412        │
│                                      │  PROBED    HomeNet    │
│                                      │  MAC TYPE  permanent  │
│                                      │                       │
│                                      │  RECENT EVENTS        │
│                                      │  NEW Apple device     │
│                                      │  PROBE "HomeNet"      │
├──────────────────────────────────────┴──────────────────────┤
│  STANDALONE MODE  ·  UPTIME 4m  ·  ● Apple  ● Win  ● Andr  │
└─────────────────────────────────────────────────────────────┘
```

Click any row to inspect the device. The right panel shows OS fingerprint, signal strength, frame count, probed SSID, MAC type, and a live event feed filtered to that device.

---

## OS fingerprinting

Passive only — reads probe request information elements broadcast by devices when scanning for networks. No payload capture, no traffic interception.

| OS | Detection method |
|---|---|
| Apple | Vendor IE `00:17:f2` in probe requests |
| Windows | Vendor IE `00:50:f2` (Microsoft) in probe requests |
| Android | HT capabilities + extended rates + 8+ supported rates |
| Linux | Basic rates only, no vendor IE, no HT |
| Randomized MAC | Locally administered bit set `(mac[0] & 0x02) != 0` |

---

## channel hopping

The device automatically cycles through channels 1, 6, and 11 every 2 seconds, covering the three non-overlapping 2.4GHz channels used by most devices.

---

## notes

- The M5StickC display is non-functional due to AXP192 power management chip incompatibility with ESP32 Arduino core 2.x. Everything works headlessly over WiFi.
- This tool is for use on networks you own or have permission to monitor. Passive 802.11 frame capture is legal in most jurisdictions when used on your own airspace.
- No payload data is captured — only frame headers (MAC addresses, frame types, signal strength, probe request IEs).

---

## license

MIT

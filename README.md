# airpulse

Real-time WiFi packet intelligence dashboard. An M5StickC runs in promiscuous mode capturing every 802.11 frame flying through the air, streams metadata to a Rust backend over WebSocket, and fires it to an LLM every 30 seconds for plain-English analysis. A browser dashboard renders a live force-directed graph of every device talking to every other device on your network.

![dashboard mockup](https://raw.githubusercontent.com/finnmagnuskverndalen/airpulse/master/static/preview.png)

---

## what it does

- captures all 802.11 frames in range — probes, beacons, data, deauth, management
- identifies device manufacturers from MAC addresses (39k+ OUI entries, fully offline)
- detects anomalies in real-time: deauth bursts, SSID harvesting, silent watchers
- streams LLM analysis every 30 seconds — plain English, no bullet points
- renders a live force-directed graph of every device and connection
- zero cloud dependencies — everything runs on your laptop

---

## architecture

```
M5StickC (ESP32-PICO-D4)
  Core 1 — WiFi promiscuous callback (IRAM, interrupt-driven)
  Core 0 — drain ring buffer → serialize JSON → WebSocket TX
      │
      │  ws://laptop:8766/esp
      ▼
Rust backend (Tokio + axum)
  bridge.rs      — accept M5StickC connection, deserialize packets
  aggregator.rs  — DashMap device state, anomaly rules, 1s broadcast
  oui.rs         — offline MAC → manufacturer lookup
  llm.rs         — OpenRouter streaming client, 30s summaries
  broadcaster.rs — fan-out state to all browser WebSocket clients
      │
      │  ws://localhost:8765/ws
      ▼
Browser dashboard
  D3.js force-directed graph — live node/edge updates
  sidebar — live feed, anomaly alerts, LLM typewriter analysis
  device table — click any row to highlight its node
```

---

## stack

| layer | choice | why |
|---|---|---|
| firmware | Arduino C++ | bare metal, IRAM callbacks, dual-core — ~10x faster than MicroPython |
| backend | Rust + Tokio | async, zero-cost abstractions, handles 10k+ packets/sec |
| http + ws | axum | built on Tokio, serves frontend + WebSocket on same port |
| device state | dashmap | lock-free concurrent hashmap, no mutex on hot path |
| packet fan-out | tokio::sync::broadcast | push to all browser clients simultaneously |
| frontend | D3.js + vanilla JS | force-directed graph, no framework overhead |

---

## hardware

- **M5StickC** — ESP32-PICO-D4, full promiscuous mode support
- USB-C to laptop — no battery required
- Tested on Debian 13

---

## setup

### 1. clone

```bash
git clone https://github.com/finnmagnuskverndalen/airpulse
cd airpulse
```

### 2. install dependencies

```bash
# Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source $HOME/.cargo/env

# Arduino CLI
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
export PATH=$PATH:$HOME/bin

# ESP32 board support (use 2.0.14 — newer versions break M5StickC)
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core install esp32:esp32@2.0.14
arduino-cli lib install "WebSockets" "ArduinoJson"

# Serial port access
sudo usermod -aG dialout $USER && newgrp dialout
```

### 3. OUI database

```bash
curl -L -o /tmp/oui.txt "https://standards-oui.ieee.org/oui/oui.txt"
python3 - << 'EOF'
with open("/tmp/oui.txt") as f:
    lines = f.readlines()
out = ["mac_prefix,vendor_name\n"]
for line in lines:
    if "(hex)" in line:
        parts = line.split("(hex)")
        if len(parts) == 2:
            mac = parts[0].strip().replace("-","").lower()
            vendor = parts[1].strip().strip('"')
            out.append(f"{mac},{vendor}\n")
with open("data/oui.csv","w") as f:
    f.writelines(out)
print(f"loaded {len(out)-1} entries")
EOF
```

### 4. firmware

```bash
# Edit firmware/recon/recon.ino and set:
#   WIFI_SSID  — your WiFi network name
#   WIFI_PASS  — your WiFi password
#   SERVER_IP  — your laptop IP (run: ip route get 1 | awk '{print $7; exit}')

arduino-cli compile --fqbn esp32:esp32:m5stick-c firmware/recon
arduino-cli upload --fqbn esp32:esp32:m5stick-c --port /dev/ttyUSB0 firmware/recon
```

### 5. environment

```bash
cp .env.example .env
nano .env   # set OPENROUTER_API_KEY
```

### 6. run

```bash
cargo run --release
```

Open `http://localhost:8765`

---

## environment variables

| variable | default | description |
|---|---|---|
| `OPENROUTER_API_KEY` | — | required for LLM analysis |
| `SERVER_HOST` | `0.0.0.0` | backend bind address |
| `SERVER_PORT` | `8765` | browser WebSocket + HTTP port |
| `ESP_WS_PORT` | `8766` | M5StickC WebSocket port |
| `OUI_CSV_PATH` | `data/oui.csv` | offline OUI database path |
| `LLM_MODEL` | `deepseek/deepseek-chat-v3-0324:free` | OpenRouter model |
| `LLM_INTERVAL_SECS` | `30` | seconds between LLM summaries |

---

## anomaly detection

| rule | trigger | severity |
|---|---|---|
| deauth burst | >5 deauth frames in 2s from same MAC | HIGH |
| ssid harvest | same MAC probing >3 different SSIDs | MEDIUM |
| new device | MAC not seen before | INFO |
| silent watcher | strong RSSI, zero data frames | LOW |

---

## dashboard

```
┌──────────────────────────────────────────────────────────────┐
│  ◉ AIRPULSE  ● LIVE   14 devices   320/s   uptime 4m        │
├──────────────────────────────────────┬───────────────────────┤
│                                      │  AI ANALYST           │
│                                      │  "3 new devices..."   │
│   D3.js force-directed graph         │                       │
│   nodes = devices                    │  ALERTS               │
│   edges = observed traffic           │  ⚑ deauth burst       │
│   size  = frame count                │  ⚑ new RPi            │
│   color = device type                │                       │
│                                      │  LIVE FEED            │
│                                      │  probe → HomeNet      │
│                                      │  deauth !! aa:bb:cc   │
├──────────────────────────────────────┴───────────────────────┤
│  MAC  │  Manufacturer  │  Type  │  RSSI  │  Frames  │  Age  │
└──────────────────────────────────────────────────────────────┘
```

**node colors:** blue = router · green = phone/laptop · amber = IoT · red = unknown/flagged

**node size:** scales with frame count — heavy talkers are bigger

**red outline:** device flagged by anomaly detector

---

## known issues

- M5StickC display is non-functional due to AXP192 power management chip incompatibility with ESP32 Arduino core 2.x. The device captures and streams packets correctly — all data is visible in the browser dashboard. Hardware fix: short G0 to GND on boot to force download mode, then flash a patched firmware.
- OUI database is not included in the repo. Run the setup step above to download it (~39k entries from IEEE).

---

## license

MIT — do whatever you want with it.

---

*built in one session with an M5StickC, a Debian laptop, and too much stubbornness*

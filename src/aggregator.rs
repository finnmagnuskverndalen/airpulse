
use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::Arc;
use std::time::{Duration, Instant};
use dashmap::DashMap;
use serde::{Deserialize, Serialize};
use tokio::sync::{broadcast, mpsc};

#[derive(Debug, Clone, Deserialize)]
pub struct Packet {
    pub mac: String,
    pub dst: String,
    pub rssi: i8,
    #[serde(rename = "type")]
    pub frame_type: String,
    pub ch: u8,
    pub ts: u64,
}

#[derive(Debug, Clone, Serialize)]
pub struct DeviceState {
    pub mac: String,
    pub manufacturer: String,
    pub frame_count: u64,
    pub rssi: i8,
    pub device_type: String,
    pub flagged: bool,
    pub last_seen: u64,
}

#[derive(Debug, Clone, Serialize)]
pub struct Alert {
    pub severity: String,
    pub message: String,
    pub mac: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct BroadcastMsg {
    pub devices: Vec<DeviceState>,
    pub edges: Vec<(String, String, u64)>,
    pub events: Vec<String>,
    pub alerts: Vec<Alert>,
    pub stats: Stats,
    pub analysis: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct Stats {
    pub total_packets: u64,
    pub packets_per_sec: u64,
    pub device_count: usize,
    pub uptime_secs: u64,
}

struct InternalDevice {
    manufacturer: String,
    frame_count: u64,
    rssi: i8,
    device_type: String,
    flagged: bool,
    last_seen: Instant,
    probed_ssids: HashSet<String>,
    deauth_times: VecDeque<Instant>,
}

pub async fn run(
    mut pkt_rx: mpsc::Receiver<Packet>,
    bcast_tx: Arc<broadcast::Sender<String>>,
    oui_db: Arc<HashMap<String, String>>,
) {
    let devices: Arc<DashMap<String, InternalDevice>> = Arc::new(DashMap::new());
    let edges: Arc<DashMap<(String, String), u64>> = Arc::new(DashMap::new());
    let mut events: VecDeque<String> = VecDeque::with_capacity(50);
    let mut alerts: Vec<Alert> = Vec::new();
    let mut total_packets: u64 = 0;
    let mut last_pps_count: u64 = 0;
    let mut pps: u64 = 0;
    let start = Instant::now();
    let mut last_broadcast = Instant::now();
    let mut last_pps_time = Instant::now();
    let mut last_llm = Instant::now();
    let llm_interval = std::env::var("LLM_INTERVAL_SECS")
        .unwrap_or("30".into()).parse::<u64>().unwrap_or(30);
    let mut current_analysis = String::from("Waiting for data...");
    let mut pending_analysis: Option<tokio::task::JoinHandle<String>> = None;

    loop {
        tokio::select! {
            Some(pkt) = pkt_rx.recv() => {
                total_packets += 1;
                let is_new = !devices.contains_key(&pkt.mac);
                let manufacturer = crate::oui::lookup(&oui_db, &pkt.mac);

                if is_new {
                    let dtype = guess_type(&manufacturer);
                    devices.insert(pkt.mac.clone(), InternalDevice {
                        manufacturer: manufacturer.clone(),
                        frame_count: 0,
                        rssi: pkt.rssi,
                        device_type: dtype.clone(),
                        flagged: false,
                        last_seen: Instant::now(),
                        probed_ssids: HashSet::new(),
                        deauth_times: VecDeque::new(),
                    });
                    let msg = format!("new device: {} ({})", manufacturer, &pkt.mac[..8]);
                    events.push_front(msg);
                    if events.len() > 50 { events.pop_back(); }
                    alerts.push(Alert {
                        severity: "INFO".into(),
                        message: format!("new device: {}", manufacturer),
                        mac: Some(pkt.mac.clone()),
                    });
                }

                if let Some(mut dev) = devices.get_mut(&pkt.mac) {
                    dev.frame_count += 1;
                    dev.rssi = pkt.rssi;
                    dev.last_seen = Instant::now();

                    if pkt.frame_type == "deauth" {
                        dev.deauth_times.push_back(Instant::now());
                        while dev.deauth_times.front()
                            .map(|t| t.elapsed() > Duration::from_secs(2))
                            .unwrap_or(false) {
                            dev.deauth_times.pop_front();
                        }
                        if dev.deauth_times.len() >= 5 && !dev.flagged {
                            dev.flagged = true;
                            alerts.push(Alert {
                                severity: "HIGH".into(),
                                message: format!("deauth burst: {} frames in 2s", dev.deauth_times.len()),
                                mac: Some(pkt.mac.clone()),
                            });
                            events.push_front(format!("DEAUTH burst from {}", &pkt.mac[..8]));
                        }
                    }
                }

                let bcast_mac = "ff:ff:ff:ff:ff:ff".to_string();
                if pkt.dst != bcast_mac {
                    let key = (pkt.mac.clone(), pkt.dst.clone());
                    *edges.entry(key).or_insert(0) += 1;
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(100)) => {}
        }

        // check if LLM task finished
        if let Some(ref mut handle) = pending_analysis {
            if handle.is_finished() {
                if let Ok(result) = pending_analysis.take().unwrap().await {
                    if !result.is_empty() {
                        current_analysis = result;
                        tracing::info!("LLM analysis updated");
                    }
                }
                pending_analysis = None;
            }
        }

        // fire LLM every N seconds
        if last_llm.elapsed() >= Duration::from_secs(llm_interval) && pending_analysis.is_none() {
            last_llm = Instant::now();
            let dev_count = devices.len();
            let new_count = devices.iter().filter(|e| e.last_seen.elapsed() < Duration::from_secs(llm_interval)).count();
            let deauth_count = alerts.iter().filter(|a| a.severity == "HIGH").count();
            let top: Vec<String> = {
                let mut v: Vec<_> = devices.iter()
                    .map(|e| (e.manufacturer.clone(), e.frame_count))
                    .collect();
                v.sort_by(|a,b| b.1.cmp(&a.1));
                v.iter().take(5).map(|(m,c)| format!("{} ({})", m, c)).collect()
            };

            let summary = format!(
                "Devices seen: {} ({} new in last {}s)
Total packets: {}
Packets/sec: {}
Deauth alerts: {}
Top talkers: {}
Recent events: {}",
                dev_count, new_count, llm_interval,
                total_packets, pps, deauth_count,
                top.join(", "),
                events.iter().take(5).cloned().collect::<Vec<_>>().join("; ")
            );

            tracing::info!("firing LLM analysis");
            pending_analysis = Some(tokio::spawn(crate::llm::analyze(summary)));
        }

        // pps counter
        if last_pps_time.elapsed() >= Duration::from_secs(1) {
            pps = total_packets - last_pps_count;
            last_pps_count = total_packets;
            last_pps_time = Instant::now();
        }

        // broadcast every second
        if last_broadcast.elapsed() >= Duration::from_secs(1) {
            last_broadcast = Instant::now();

            let device_list: Vec<DeviceState> = devices.iter().map(|e| {
                let d = e.value();
                DeviceState {
                    mac: e.key().clone(),
                    manufacturer: d.manufacturer.clone(),
                    frame_count: d.frame_count,
                    rssi: d.rssi,
                    device_type: d.device_type.clone(),
                    flagged: d.flagged,
                    last_seen: d.last_seen.elapsed().as_secs(),
                }
            }).collect();

            let edge_list: Vec<(String, String, u64)> = edges.iter()
                .map(|e| (e.key().0.clone(), e.key().1.clone(), *e.value()))
                .collect();

            let msg = BroadcastMsg {
                devices: device_list,
                edges: edge_list,
                events: events.iter().cloned().collect(),
                alerts: alerts.clone(),
                stats: Stats {
                    total_packets,
                    packets_per_sec: pps,
                    device_count: devices.len(),
                    uptime_secs: start.elapsed().as_secs(),
                },
                analysis: current_analysis.clone(),
            };

            if let Ok(json) = serde_json::to_string(&msg) {
                let _ = bcast_tx.send(json);
            }

            if alerts.len() > 20 { alerts.drain(0..alerts.len()-20); }
        }
    }
}

fn guess_type(manufacturer: &str) -> String {
    let m = manufacturer.to_lowercase();
    if m.contains("cisco") || m.contains("netgear") || m.contains("tp-link") ||
       m.contains("asus") || m.contains("ubiquiti") || m.contains("mikrotik") {
        "Router".into()
    } else if m.contains("apple") || m.contains("samsung") || m.contains("xiaomi") ||
              m.contains("oneplus") || m.contains("huawei") || m.contains("google") {
        "Phone".into()
    } else if m.contains("raspberry") || m.contains("espressif") || m.contains("arduino") {
        "IoT".into()
    } else if m.contains("intel") || m.contains("dell") || m.contains("lenovo") ||
              m.contains("hp ") || m.contains("hewlett") {
        "Laptop".into()
    } else {
        "Unknown".into()
    }
}

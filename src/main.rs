
use std::sync::Arc;
use tokio::sync::{broadcast, mpsc};
use dotenvy::dotenv;

mod bridge;
mod aggregator;
mod broadcaster;
mod oui;
mod llm;

pub use aggregator::Packet;

fn run_setup() {
    let status = std::process::Command::new("python3")
        .arg("setup.py")
        .status();
    match status {
        Ok(s) if s.success() => {}
        Ok(_) => { eprintln!("setup exited with error"); std::process::exit(1); }
        Err(e) => { eprintln!("could not run setup.py: {}", e); std::process::exit(1); }
    }
}

fn needs_setup() -> bool {
    if !std::path::Path::new(".env").exists() {
        return true;
    }
    dotenv().ok();
    let key = std::env::var("OPENROUTER_API_KEY").unwrap_or_default();
    key.is_empty() || key == "your-key-here"
}

#[tokio::main]
async fn main() {
    if needs_setup() {
        println!("\n  no .env found — running setup first\n");
        run_setup();
    }

    dotenv().ok();
    tracing_subscriber::fmt::init();

    let host = std::env::var("SERVER_HOST").unwrap_or("0.0.0.0".into());
    let port: u16 = std::env::var("SERVER_PORT").unwrap_or("8765".into()).parse().unwrap();
    let esp_port: u16 = std::env::var("ESP_WS_PORT").unwrap_or("8766".into()).parse().unwrap();
    let oui_path = std::env::var("OUI_CSV_PATH").unwrap_or("data/oui.csv".into());

    tracing::info!("loading OUI database from {}", oui_path);
    let oui_db = Arc::new(oui::load(&oui_path));
    tracing::info!("loaded {} OUI entries", oui_db.len());

    let (pkt_tx, pkt_rx) = mpsc::channel::<Packet>(4096);
    let (bcast_tx, _) = broadcast::channel::<String>(256);
    let bcast_tx = Arc::new(bcast_tx);

    let agg_handle = {
        let bcast = bcast_tx.clone();
        let oui = oui_db.clone();
        tokio::spawn(aggregator::run(pkt_rx, bcast, oui))
    };

    let bridge_handle = {
        let tx = pkt_tx.clone();
        tokio::spawn(bridge::run(esp_port, tx))
    };

    let web_handle = {
        let bcast = bcast_tx.clone();
        tokio::spawn(broadcaster::run(host, port, bcast))
    };

    tracing::info!("airpulse running — esp ws on :{} — browser on :{}", esp_port, port);

    tokio::select! {
        _ = agg_handle => tracing::error!("aggregator exited"),
        _ = bridge_handle => tracing::error!("bridge exited"),
        _ = web_handle => tracing::error!("web server exited"),
    }
}

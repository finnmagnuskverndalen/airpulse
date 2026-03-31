
use tokio::net::TcpListener;
use tokio_tungstenite::accept_async;
use futures_util::StreamExt;
use tokio::sync::mpsc;
use crate::Packet;

pub async fn run(port: u16, tx: mpsc::Sender<Packet>) {
    let addr = format!("0.0.0.0:{}", port);
    let listener = TcpListener::bind(&addr).await.expect("bridge bind failed");
    tracing::info!("bridge listening on {}", addr);

    loop {
        match listener.accept().await {
            Ok((stream, addr)) => {
                tracing::info!("ESP32 connected from {}", addr);
                let tx = tx.clone();
                tokio::spawn(async move {
                    let ws = match accept_async(stream).await {
                        Ok(ws) => ws,
                        Err(e) => { tracing::warn!("ws handshake failed: {}", e); return; }
                    };
                    let (_, mut rx) = ws.split();
                    while let Some(msg) = rx.next().await {
                        match msg {
                            Ok(tokio_tungstenite::tungstenite::Message::Text(txt)) => {
                                match serde_json::from_str::<Packet>(&txt) {
                                    Ok(pkt) => { let _ = tx.send(pkt).await; }
                                    Err(e) => tracing::warn!("parse error: {} — {}", e, txt),
                                }
                            }
                            Err(e) => { tracing::warn!("ws error: {}", e); break; }
                            _ => {}
                        }
                    }
                    tracing::info!("ESP32 disconnected");
                });
            }
            Err(e) => tracing::warn!("accept error: {}", e),
        }
    }
}

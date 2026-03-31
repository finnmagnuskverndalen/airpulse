
use std::sync::Arc;
use axum::{Router, routing::get};
use axum::extract::{WebSocketUpgrade, State};
use axum::extract::ws::{WebSocket, Message};
use axum::response::IntoResponse;
use tower_http::services::ServeDir;
use tokio::sync::broadcast;
use futures_util::{sink::SinkExt, stream::StreamExt};

type BcastTx = Arc<broadcast::Sender<String>>;

pub async fn run(host: String, port: u16, bcast_tx: BcastTx) {
    let app = Router::new()
        .route("/ws", get(ws_handler))
        .nest_service("/", ServeDir::new("static"))
        .with_state(bcast_tx);

    let addr = format!("{}:{}", host, port);
    tracing::info!("browser server on http://{}", addr);
    let listener = tokio::net::TcpListener::bind(&addr).await.unwrap();
    axum::serve(listener, app).await.unwrap();
}

async fn ws_handler(
    ws: WebSocketUpgrade,
    State(bcast_tx): State<BcastTx>,
) -> impl IntoResponse {
    ws.on_upgrade(move |socket| handle_socket(socket, bcast_tx))
}

async fn handle_socket(socket: WebSocket, bcast_tx: BcastTx) {
    let mut rx = bcast_tx.subscribe();
    let (mut sender, _) = socket.split();
    tracing::info!("browser client connected");
    while let Ok(msg) = rx.recv().await {
        if sender.send(Message::Text(msg.into())).await.is_err() {
            break;
        }
    }
    tracing::info!("browser client disconnected");
}

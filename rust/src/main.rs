use std::sync::{Arc, Mutex};

use chrono::Local;
use futures_util::{SinkExt, StreamExt};
use serde_json::Value;
use tokio_tungstenite::connect_async;
use tokio_tungstenite::tungstenite::Message;

// Binance: BTCUSDT (달러 기준)
const BINANCE_WS: &str = "wss://stream.binance.com:9443/ws/btcusdt@bookTicker";

// Upbit: KRW-BTC (원화 기준)
const UPBIT_WS: &str = "wss://api.upbit.com/websocket/v1";

// 실제 USD/KRW 환율 (키 불필요, 일 1회 갱신)
const FX_API: &str = "https://open.er-api.com/v6/latest/USD";

#[derive(Default)]
struct State {
    binance_mid: Option<f64>,
    upbit_mid: Option<f64>,
    fx_rate: Option<f64>, // 실제 USD/KRW 환율
}

fn print_diff(state: &State) {
    let (b, u, fx) = match (state.binance_mid, state.upbit_mid, state.fx_rate) {
        (Some(b), Some(u), Some(fx)) => (b, u, fx),
        _ => return,
    };

    // 시장 암묵 환율 = Upbit KRW 가격 / Binance USD 가격
    let implied_rate = u / b;

    // 가격 괴리율 = (Upbit - Binance를 실제환율로 환산) / 환산값 * 100
    let binance_in_krw = b * fx;
    let premium_pct = (u - binance_in_krw) / binance_in_krw * 100.0;

    let dt = Local::now().format("%H:%M:%S%.3f");
    println!(
        "[{dt}] Binance={b:>10.2} USD  Upbit={u:>14.0} KRW  실제환율={fx:>8.2}  암묵환율={implied_rate:>8.2}  괴리율={premium_pct:>+6.3}%"
    );
}

async fn fx_updater(state: Arc<Mutex<State>>) {
    let client = reqwest::Client::new();
    loop {
        match fetch_fx(&client).await {
            Ok(rate) => state.lock().unwrap().fx_rate = Some(rate),
            Err(e) => eprintln!("[!] 환율 조회 실패: {e}"),
        }
        // 환율 소스가 일 1회 갱신이라 1시간 주기로 충분
        tokio::time::sleep(std::time::Duration::from_secs(3600)).await;
    }
}

async fn fetch_fx(client: &reqwest::Client) -> Result<f64, Box<dyn std::error::Error>> {
    let json: Value = client.get(FX_API).send().await?.json().await?;
    json["rates"]["KRW"]
        .as_f64()
        .ok_or_else(|| "KRW 환율 파싱 실패".into())
}

async fn binance_receiver(state: Arc<Mutex<State>>) {
    let (ws, _) = connect_async(BINANCE_WS)
        .await
        .expect("Binance 연결 실패");
    let (_, mut read) = ws.split();

    while let Some(msg) = read.next().await {
        let msg = msg.expect("Binance 수신 실패");
        let text = match msg {
            Message::Text(t) => t.to_string(),
            Message::Binary(b) => String::from_utf8_lossy(&b).into_owned(),
            _ => continue,
        };
        let data: Value = match serde_json::from_str(&text) {
            Ok(v) => v,
            Err(_) => continue,
        };

        // "b" 최우선 매수호가, "a" 최우선 매도호가
        let bid: f64 = data["b"].as_str().unwrap_or("0").parse().unwrap_or(0.0);
        let ask: f64 = data["a"].as_str().unwrap_or("0").parse().unwrap_or(0.0);

        let mut st = state.lock().unwrap();
        st.binance_mid = Some((bid + ask) / 2.0);
        print_diff(&st);
    }
}

async fn upbit_receiver(state: Arc<Mutex<State>>) {
    let (ws, _) = connect_async(UPBIT_WS).await.expect("Upbit 연결 실패");
    let (mut write, mut read) = ws.split();

    // Upbit 구독 요청
    let payload = serde_json::json!([
        {"ticket": "kimchi-monitor"},
        {"type": "orderbook", "codes": ["KRW-BTC"]}
    ]);
    write
        .send(Message::Text(payload.to_string().into()))
        .await
        .expect("Upbit 구독 요청 실패");

    while let Some(msg) = read.next().await {
        let msg = msg.expect("Upbit 수신 실패");
        let text = match msg {
            Message::Text(t) => t.to_string(),
            Message::Binary(b) => String::from_utf8_lossy(&b).into_owned(),
            _ => continue,
        };
        let data: Value = match serde_json::from_str(&text) {
            Ok(v) => v,
            Err(_) => continue,
        };

        if data["type"].as_str() != Some("orderbook") {
            continue;
        }

        let unit = &data["orderbook_units"][0];
        let bid = unit["bid_price"].as_f64().unwrap_or(0.0);
        let ask = unit["ask_price"].as_f64().unwrap_or(0.0);

        let mut st = state.lock().unwrap();
        st.upbit_mid = Some((bid + ask) / 2.0);
    }
}

#[tokio::main]
async fn main() {
    // rustls 0.23: 프로세스 단위 CryptoProvider를 명시적으로 설치해야 TLS 핸드셰이크 가능
    rustls::crypto::ring::default_provider()
        .install_default()
        .expect("CryptoProvider 설치 실패");

    println!("[*] Binance + Upbit 연결 중...\n");
    println!(
        "{:<15} {:>14} {:>18} {:>10} {:>10} {:>8}",
        "시각", "Binance", "Upbit", "실제환율", "암묵환율", "괴리율"
    );
    println!("{}", "-".repeat(90));

    let state = Arc::new(Mutex::new(State::default()));

    let f = tokio::spawn(fx_updater(state.clone()));
    let b = tokio::spawn(binance_receiver(state.clone()));
    let u = tokio::spawn(upbit_receiver(state.clone()));

    let _ = tokio::join!(f, b, u);
}

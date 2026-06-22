import asyncio
import json
import urllib.request
import websockets
from datetime import datetime

# Binance: BTCUSDT (달러 기준)
BINANCE_WS = "wss://stream.binance.com:9443/ws/btcusdt@bookTicker"

# Upbit: KRW-BTC (원화 기준)
UPBIT_WS   = "wss://api.upbit.com/websocket/v1"

# 실제 USD/KRW 환율 (키 불필요, 일 1회 갱신)
FX_API = "https://open.er-api.com/v6/latest/USD"

# 공유 상태
state = {
    "binance_mid": None,
    "upbit_mid":   None,
    "fx_rate":     None,   # 실제 USD/KRW 환율
}

def fetch_fx():
    with urllib.request.urlopen(FX_API, timeout=10) as resp:
        return float(json.load(resp)["rates"]["KRW"])

def print_diff():
    b  = state["binance_mid"]
    u  = state["upbit_mid"]
    fx = state["fx_rate"]
    if b is None or u is None or fx is None:
        return

    # 시장 암묵 환율 = Upbit KRW 가격 / Binance USD 가격
    implied_rate = u / b

    # 가격 괴리율 = (Upbit - Binance를 실제환율로 환산) / 환산값 * 100
    binance_in_krw = b * fx
    premium_pct    = (u - binance_in_krw) / binance_in_krw * 100

    dt = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(
        f"[{dt}] "
        f"Binance={b:>10.2f} USD  "
        f"Upbit={u:>14.0f} KRW  "
        f"실제환율={fx:>8.2f}  "
        f"암묵환율={implied_rate:>8.2f}  "
        f"괴리율={premium_pct:>+6.3f}%"
    )

async def fx_updater():
    while True:
        try:
            state["fx_rate"] = await asyncio.to_thread(fetch_fx)
        except Exception as e:
            print(f"[!] 환율 조회 실패: {e}")
        await asyncio.sleep(3600)  # 환율 소스가 일 1회 갱신이라 1시간 주기로 충분

async def binance_receiver():
    async with websockets.connect(BINANCE_WS) as ws:
        while True:
            data = json.loads(await ws.recv())
            
            bid     = float(data["b"])  # 최우선 매수호가
            bid_qty = float(data["B"])  # 매수 수량
            ask     = float(data["a"])  # 최우선 매도호가
            ask_qty = float(data["A"])  # 매도 수량
            
            state["binance_mid"] = (bid + ask) / 2
            print_diff()

async def upbit_receiver():
    async with websockets.connect(UPBIT_WS) as ws:
        # Upbit 구독 요청
        payload = json.dumps([
            {"ticket": "kimchi-monitor"},
            {"type": "orderbook", "codes": ["KRW-BTC"]}
        ])
        await ws.send(payload)

        while True:
            raw  = await ws.recv()
            data = json.loads(raw)
            if data.get("type") != "orderbook":
                continue

            units = data["orderbook_units"]
            bid   = float(units[0]["bid_price"])
            ask   = float(units[0]["ask_price"])
            state["upbit_mid"] = (bid + ask) / 2

async def main():
    print("[*] Binance + Upbit 연결 중...\n")
    print(f"{'시각':<15} {'Binance':>14} {'Upbit':>18} {'실제환율':>10} {'암묵환율':>10} {'괴리율':>8}")
    print("-" * 90)
    await asyncio.gather(
        fx_updater(),
        binance_receiver(),
        upbit_receiver(),
    )

if __name__ == "__main__":
    asyncio.run(main())
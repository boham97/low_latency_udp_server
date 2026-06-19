import asyncio
import json
import websockets
from datetime import datetime

# Binance: BTCUSDT (달러 기준)
BINANCE_WS = "wss://stream.binance.com:9443/ws/btcusdt@bookTicker"

# Upbit: KRW-BTC (원화 기준)
UPBIT_WS   = "wss://api.upbit.com/websocket/v1"

# 공유 상태
state = {
    "binance_mid": None,
    "upbit_mid":   None,
}

def print_diff():
    b = state["binance_mid"]
    u = state["upbit_mid"]
    if b is None or u is None:
        return

    # 암묵적 환율 = Upbit KRW 가격 / Binance USD 가격
    implied_rate   = u / b

    # 환율차 = (Upbit - Binance*환율) / (Binance*환율) * 100
    binance_in_krw = b * implied_rate
    rate_diff_pct     = (u - binance_in_krw) / binance_in_krw * 100

    dt = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(
        f"[{dt}] "
        f"Binance={b:>10.2f} USD  "
        f"Upbit={u:>14.0f} KRW  "
        f"환율={implied_rate:>8.2f}  "
        f"환율차={rate_diff_pct:>+6.3f}%"
    )

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
            print('recv!')
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
    print(f"{'시각':<15} {'Binance':>14} {'Upbit':>18} {'환율':>10} {'환율차':>10}")
    print("-" * 80)
    await asyncio.gather(
        binance_receiver(),
        upbit_receiver(),
    )

if __name__ == "__main__":
    asyncio.run(main())
# IEX DEEP 프로토콜 정리

출처: `DEEP Specification v1.08` (2021-10-19), `IEX Transport Specification v1.25` (2017-05-12)

## IEX DEEP이란

DEEP(**D**epth of book)는 IEX(Investors Exchange)가 제공하는 **실시간 호가창(market-by-price) + 체결 정보** 멀티캐스트 데이터 피드입니다.

- **무엇을 주나**: 가격 레벨별 **표시 주문(displayed)의 합산 수량**, 최종 체결 가격/수량, 경매(auction) 정보, 종목 상태(거래정지·공매도 규제 등)
- **무엇을 안 주나**: 개별 주문 단위 정보 ❌, 비표시(non-displayed)·리저브 주문의 숨은 수량 ❌, 라우팅 체결 ❌, **주문 입력 불가**(주문은 FIX로)
- 더 가벼운 게 필요하면 최우선 호가만 주는 **TOPS** 프로토콜이 따로 있음

## 2계층 구조 (핵심)

DEEP은 **메시지 정의**일 뿐이고, 전송은 하위 계층 **IEX-TP(IEX Transport Protocol)**가 담당합니다.

```
[ UDP 멀티캐스트 패킷 ]
  └ IEX-TP Outbound Segment (40바이트 헤더 + payload)
       └ Message Block (2바이트 길이 + Message Data)
            └ DEEP 메시지  ← Message Protocol ID = 0x8004, Channel ID = 1
```

### IEX-TP가 책임지는 것 (전송/신뢰성)

40바이트 헤더의 주요 필드:

| 필드 | 역할 |
|---|---|
| Message Protocol ID | 상위 프로토콜 식별 (DEEP = `0x8004`) |
| Session ID + Sequence Number | 메시지를 하루 단위로 유일하게 식별 |
| Stream Offset | 바이트 스트림 내 오프셋 |
| First Message Sequence Number / Message Count | 한 세그먼트에 여러 메시지 묶음 가능 |
| Send Time | 전송 시각 |

- **A/B 라인 이중화**: 동일 메시지를 두 멀티캐스트 그룹으로 보냄(순서 동일, 패킷 묶음은 다를 수 있음) → 한쪽 유실 대비
- **Heartbeat**: 1초간 메시지 없으면 빈 세그먼트 전송
- **Gap Fill**: 시퀀스 갭 감지 시 TCP/UDP 유니캐스트로 재전송 요청 (UDP는 요청당 1,000개 제한, TCP는 무제한)

### DEEP이 책임지는 것 (의미/내용)

- 모든 필드는 **리틀 엔디언**
- 가격은 8바이트 정수에 **소수점 4자리 암묵 고정**(예: `123400` = $12.34)
- 타임스탬프는 **POSIX 이후 나노초**

## DEEP 메시지 종류

### 관리 메시지(Administrative)

| 타입 | 이름 | 내용 |
|---|---|---|
| `S` | System Event | 장 시작/종료 등 세션 이벤트 (O/S/R/M/E/C) |
| `D` | Security Directory | 종목 기본정보(라운드랏, 전일 종가 등), 장전 일괄 전송 |
| `H` | Trading Status | 거래 상태 (Halt/Pause/OAP/Trading) + 사유 |
| `O` | Operational Halt | 운영상 거래정지 |
| `P` | Short Sale Price Test | Reg SHO 공매도 가격제한(Rule 201) |
| `I` | Retail Liquidity Indicator | 리테일 유동성 매수/매도 관심 표시 |
| `E` | Security Event | 종목별 Opening/Closing Process 완료 |

### 거래 메시지(Trading)

| 타입 | 이름 | 내용 |
|---|---|---|
| `8`/`5` | **Price Level Update** | 매수(`8`)/매도(`5`) 가격 레벨 합산 수량 갱신. 수량 0 = 레벨 제거 |
| `T` | Trade Report | 개별 체결(가격/수량/Trade ID) |
| `X` | Official Price | IEX 공식 시가/종가 |
| `B` | Trade Break | 당일 체결 취소 (해당 Trade ID 참조) |

### 경매 메시지(Auction) — IEX 상장 종목 한정

| 타입 | 이름 | 내용 |
|---|---|---|
| `A` | Auction Information | 매칭 가격, paired/imbalance 수량, collar 등. Opening/Closing/IPO/Halt/Volatility 경매 |

## 가장 중요한 동작: Event Flags와 원자적(atomic) 갱신

호가창 재구성에서 핵심입니다. Price Level Update 메시지의 **Event Flags**:

- `0x0` = 호가창이 **전이 중**(transition) — 한 이벤트가 여러 레벨을 동시에 바꾸는 중
- `0x1` = 전이 완료

하나의 이벤트(예: 공격적 주문 진입으로 여러 레벨 동시 소진)는 `PLU(0x0), PLU(0x0), …, PLU(0x1)` 형태로 전달됩니다.

**규칙**: 전이가 끝날 때(`0x1` 도착)까지는 IEX BBO를 재계산하지 말 것. 중간 상태의 "존재하지 않았던" BBO를 보지 않기 위함입니다. 단일 레벨만 바뀌면 선행 `0x0` 없이 `0x1` 하나만 옵니다.

## 참고: 디렉터리의 pcap 파일

`20180127_IEXTP1_DEEP1.0.pcap` (약 10MB)는 실제 DEEP 멀티캐스트 캡처본입니다. 위 헤더 구조대로 파싱하면 됩니다.

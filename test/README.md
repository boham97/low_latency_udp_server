# test/ — IEX DEEP 학습·실험 코드

본 프로젝트(`src/`)로 들어가기 전 단계의 실험 코드. 각 소스 상단 주석에 `Build:` / `Run:` 명령이 있고,
**바이너리는 소스와 같은 디렉토리에서 빌드·실행**하는 것을 전제로 경로가 잡혀 있다(`../data`, `../out`).

```
common/   iex_deep_wire.h        와이어 포맷 구조체 (LE + pack(1) 전제)
data/     *.pcap                 IEX HIST DEEP 캡처 (입력 데이터)
parse/    iex_deep_parse[_pcap|_mmap].c, deep_dump.c
                                 오프라인 파서 계보 (fread / libpcap / mmap) + 전체 메시지 TSV 덤프
udp/      udp_replay_send.c, udp_deep_server.c
                                 pcap → UDP replay 송신기, UDP 수신 Rx 스테이지(seq 갭 검사)
demo/     zerocopy_demo.c, copy_vs_zc.c, pack_demo.c, buftest.c
                                 개념 확인용 소품 (zero-copy, 구조체 패킹, stdio 버퍼링)
docs/     IEX_DEEP_프로토콜_정리.md, TIL*.md, spec/*.pdf
out/      실행 산출물 (덤프 / strace 로그) — 재생성 가능, git 제외
```

## 자주 쓰는 실행

```sh
# 오프라인 파싱 결과 요약 (인자 없으면 ../data 의 pcap 사용)
cd parse && ./iex_deep_parse

# 전체 메시지 TSV 덤프
cd parse && ./deep_dump > ../out/deep_dump.tsv

# UDP 경로: 서버 먼저 띄우고(2초 idle 시 자동 종료) 리플레이 송신
cd udp && ./udp_deep_server 9004 & sleep 1; ./udp_replay_send

# 서버 시스템콜 트레이스 (strace 오버헤드로 서버가 느려져 최대속도 리플레이는
# 커널 큐에서 드롭된다 — 페이싱 150us를 줘야 전 구간이 잡힌다)
cd udp && strace -f -tt -T -o ../out/udp_server_strace.log ./udp_deep_server 9004 &
sleep 1; ./udp_replay_send ../data/20180127_IEXTP1_DEEP1.0.pcap 127.0.0.1 9004 150
```

# test/ — IEX DEEP 학습·실험 코드

본 프로젝트(`src/`)로 들어가기 전 단계의 실험 코드. 각 소스 상단 주석에 `Build:` / `Run:` 명령이 있고,
**바이너리는 소스와 같은 디렉토리에서 빌드·실행**하는 것을 전제로 경로가 잡혀 있다(`../data`, `../out`).

```
data/     *.pcap                 IEX HIST DEEP 캡처 (입력 데이터)
parse/    iex_deep_parse[_pcap|_mmap].c, deep_dump.c
                                 오프라인 파서 계보 (fread / libpcap / mmap) + 전체 메시지 TSV 덤프
udp/      udp_replay_send.c, udp_deep_server.c
                                 pcap → UDP replay 송신기, UDP 수신 Rx 스테이지(seq 갭 검사)
demo/     zerocopy_demo.c, copy_vs_zc.c, pack_demo.c, buftest.c
                                 개념 확인용 소품 (zero-copy, 구조체 패킹, stdio 버퍼링)
bench/    slot_vs_index.c        큐에 데이터 직접(복사/슬롯대여) vs 인덱스 전달 비교
                                 결과는 bench/results.md
docs/     IEX_DEEP_프로토콜_정리.md, TIL*.md, spec/*.pdf
out/      실행 산출물 (덤프 / strace 로그) — 재생성 가능, git 제외
```

와이어 포맷 구조체 `iex_deep_wire.h`는 `src/`도 쓰게 되어 `include/ll/`로 옮겼다
(`test/common/` 삭제). test 쪽 빌드는 `-I../../include`.

## 자주 쓰는 실행

```sh
# 오프라인 파싱 결과 요약 (인자 없으면 ../data 의 pcap 사용)
cd parse && ./iex_deep_parse

# 전체 메시지 TSV 덤프
cd parse && ./deep_dump > ../out/deep_dump.tsv

# UDP 경로: 서버 먼저 띄우고(2초 idle 시 자동 종료) 리플레이 송신
cd udp && ./udp_deep_server 9004 & sleep 1; ./udp_replay_send

# 슬롯 대여 vs 인덱스 전달 (조합당 별도 프로세스로 3회, 중앙값을 읽는다)
cd bench && for v in copy borrow index scatter; do for b in 16 64 2048; do
  for r in 1 2 3; do ./slot_vs_index $v $b; done; done; done

# 서버 시스템콜 트레이스 (strace 오버헤드로 서버가 느려져 최대속도 리플레이는
# 커널 큐에서 드롭된다 — 페이싱 150us를 줘야 전 구간이 잡힌다)
cd udp && strace -f -tt -T -o ../out/udp_server_strace.log ./udp_deep_server 9004 &
sleep 1; ./udp_replay_send ../data/20180127_IEXTP1_DEEP1.0.pcap 127.0.0.1 9004 150
```

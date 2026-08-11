# 핫패스 구현체는 여기에 추가
#   net/     UDP 수신 Rx 스테이지
#   spsc/    스테이지 간 SPSC 큐
#   mempool/ in-flight 메시지 블록 풀
#   deep/    IEX-TP / DEEP 디코드
#   book/    오더북 적용 + BBO 발행

find_package(Threads REQUIRED)

# Rx 스테이지 + 드레인 소비자 (스테이지 연결 확인용 실행 파일)
add_executable(rx_stage
    net/rx_stage.c
    net/rx_main.c
)
target_link_libraries(rx_stage PRIVATE Threads::Threads)

# 오프라인 파이프라인: pcap → 프레이밍 → 디코드 → 북 → BBO (진행순서 1단계)
# 프레이밍은 rx_stage.c를 그대로 공유한다 — 여기서 확정한 정확성이 라이브 경로와
# 같은 코드여야 검증이 옮겨간다.
add_executable(book_offline
    net/rx_stage.c
    deep/symtab.c
    book/book.c
    book/book_offline.c
)

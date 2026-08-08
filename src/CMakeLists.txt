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

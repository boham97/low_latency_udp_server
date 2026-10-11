find_package(Threads REQUIRED)

add_executable(spsc_variants_bench spsc_variants_bench.c)
target_link_libraries(spsc_variants_bench PRIVATE Threads::Threads)

# 오더북 레이아웃 비교: [0]=best (src/book/book.c) vs [n-1]=best (book_rev.c)
add_executable(book_layout_bench
    book_layout_bench.c
    book_rev.c
    ${CMAKE_SOURCE_DIR}/src/book/book.c
    ${CMAKE_SOURCE_DIR}/src/deep/symtab.c
    ${CMAKE_SOURCE_DIR}/src/net/rx_stage.c
)

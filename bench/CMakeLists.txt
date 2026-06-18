find_package(Threads REQUIRED)

add_executable(spsc_variants_bench spsc_variants_bench.c)
target_link_libraries(spsc_variants_bench PRIVATE Threads::Threads)

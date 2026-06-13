find_package(Threads REQUIRED)

add_executable(spsc_cacheline_bench spsc_cacheline_bench.c)
target_link_libraries(spsc_cacheline_bench PRIVATE Threads::Threads)

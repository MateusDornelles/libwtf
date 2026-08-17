add_executable(wtf_benchmark wtf_benchmark.c)
target_link_libraries(wtf_benchmark PRIVATE wtf)
target_compile_features(wtf_benchmark PRIVATE c_std_11)

if(NOT WIN32)
    target_compile_options(wtf_benchmark PRIVATE -Wall -Wextra)
endif()

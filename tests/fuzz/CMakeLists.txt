# Fuzz targets, built only with -DAWTRIX_FUZZ=ON and clang (libFuzzer). See each target's header.
add_executable(fuzz-radio-stream fuzz_radio_stream.cpp)
target_compile_options(fuzz-radio-stream PRIVATE -fsanitize=fuzzer,address,undefined -g)
target_link_options(fuzz-radio-stream PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_libraries(fuzz-radio-stream PRIVATE awtrix_core)

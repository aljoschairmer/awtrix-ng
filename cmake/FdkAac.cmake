include_guard(GLOBAL)

# The AAC decoder of the radio: the decoder part of the Fraunhofer FDK AAC library, vendored in
# lib/fdk-aac (see its README.vendor). Linux, the TC002 and the web engine build it; the ESP32 does
# not (platformio.ini leaves src/core/radio/stream/ out).
get_filename_component(AWTRIX_FDKAAC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../lib/fdk-aac" ABSOLUTE)
file(STRINGS "${AWTRIX_FDKAAC_ROOT}/sources.txt" AWTRIX_FDKAAC_RELATIVE)
set(AWTRIX_FDKAAC_SOURCES)
foreach(source IN LISTS AWTRIX_FDKAAC_RELATIVE)
  list(APPEND AWTRIX_FDKAAC_SOURCES "${AWTRIX_FDKAAC_ROOT}/${source}")
endforeach()
add_library(awtrix_fdkaac STATIC ${AWTRIX_FDKAAC_SOURCES})
target_include_directories(awtrix_fdkaac SYSTEM PUBLIC "${AWTRIX_FDKAAC_ROOT}/libAACdec/include"
  "${AWTRIX_FDKAAC_ROOT}/libSYS/include")
target_include_directories(awtrix_fdkaac PRIVATE
  "${AWTRIX_FDKAAC_ROOT}/libArithCoding/include" "${AWTRIX_FDKAAC_ROOT}/libDRCdec/include"
  "${AWTRIX_FDKAAC_ROOT}/libSACdec/include" "${AWTRIX_FDKAAC_ROOT}/libSBRdec/include"
  "${AWTRIX_FDKAAC_ROOT}/libMpegTPDec/include" "${AWTRIX_FDKAAC_ROOT}/libFDK/include"
  "${AWTRIX_FDKAAC_ROOT}/libPCMutils/include")
# Third-party code: its own warnings are not ours to fix, and it is the hot path of a station.
target_compile_options(awtrix_fdkaac PRIVATE -w -O2 -fno-exceptions -fno-rtti)
set_target_properties(awtrix_fdkaac PROPERTIES CXX_EXTENSIONS ON POSITION_INDEPENDENT_CODE OFF)

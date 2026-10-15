cmake_minimum_required(VERSION 3.16)
project(ears LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TEST_EARS "Build test_ears CLI" ON)

if(EXISTS "${CMAKE_SOURCE_DIR}/models/ggml-medium.bin")
  add_custom_command(
    OUTPUT "${CMAKE_SOURCE_DIR}/src/assets.cpp"
    COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/gen_assets.py"
            "${CMAKE_SOURCE_DIR}/models"
            "${CMAKE_SOURCE_DIR}/src/assets.cpp"
    DEPENDS
      "${CMAKE_SOURCE_DIR}/scripts/gen_assets.py"
      "${CMAKE_SOURCE_DIR}/models/ggml-medium.bin"
    COMMENT "Generating assets.cpp from models/"
  )
  add_custom_target(ears_assets DEPENDS "${CMAKE_SOURCE_DIR}/src/assets.cpp")
endif()

if(NOT SBS_PREFIX)
  if(DEFINED ENV{SBS_PREFIX} AND NOT "$ENV{SBS_PREFIX}" STREQUAL "")
    set(SBS_PREFIX "$ENV{SBS_PREFIX}")
  endif()
endif()
set(SBS_PREFIX "${SBS_PREFIX}" CACHE PATH "simplybs host prefix")

find_library(WHISPER_LIB whisper PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(GGML_LIB ggml PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(GGML_CPU_LIB ggml-cpu PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(GGML_BASE_LIB ggml-base PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
# Required on Apple (whisper built with GGML_BLAS=Apple); absent on Linux/mingw.
if(APPLE)
  find_library(GGML_BLAS_LIB ggml-blas PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
else()
  find_library(GGML_BLAS_LIB ggml-blas PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()
find_library(AVFORMAT_LIB avformat PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(AVCODEC_LIB avcodec PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(AVUTIL_LIB avutil PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(SWRESAMPLE_LIB swresample PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(ZLIB_LIB z PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT ZLIB_LIB)
  find_library(ZLIB_LIB z REQUIRED)
endif()

include_directories(
  "${CMAKE_SOURCE_DIR}/include"
  "${SBS_PREFIX}/include"
)

set(EARS_SOURCES
  src/abi.cpp
  src/assets.cpp
  src/jobs.cpp
  src/ctx.cpp
  src/decode.cpp
  src/whisper_ctx.cpp
  src/pipeline.cpp
  src/output_json.cpp
)

add_library(ears SHARED ${EARS_SOURCES})
if(TARGET ears_assets)
  add_dependencies(ears ears_assets)
endif()

set(EARS_LIBS
  ${WHISPER_LIB}
  ${GGML_LIB}
  ${GGML_CPU_LIB}
  ${GGML_BASE_LIB}
)
if(GGML_BLAS_LIB)
  list(APPEND EARS_LIBS ${GGML_BLAS_LIB})
endif()
list(APPEND EARS_LIBS
  ${AVFORMAT_LIB}
  ${AVCODEC_LIB}
  ${AVUTIL_LIB}
  ${SWRESAMPLE_LIB}
  ${ZLIB_LIB}
)

target_link_libraries(ears PRIVATE ${EARS_LIBS})

if(APPLE)
  set_target_properties(ears PROPERTIES
    OUTPUT_NAME "ears"
    PREFIX "lib"
    INSTALL_NAME_DIR "@rpath"
    BUILD_WITH_INSTALL_RPATH ON
    BUILD_RPATH "${SBS_PREFIX}/lib"
    INSTALL_RPATH "@rpath;${SBS_PREFIX}/lib"
  )
  # whisper/ggml Apple builds use Accelerate (BLAS + vDSP).
  target_link_libraries(ears PRIVATE "-framework Accelerate" "-framework Foundation")
  if(EXISTS "${CMAKE_SOURCE_DIR}/exports.txt")
    target_link_libraries(ears PRIVATE "-Wl,-exported_symbols_list,${CMAKE_SOURCE_DIR}/exports.txt")
  endif()
elseif(ANDROID)
  set_target_properties(ears PROPERTIES
    OUTPUT_NAME "ears"
    PREFIX "lib"
    BUILD_WITH_INSTALL_RPATH ON
    BUILD_RPATH "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN"
  )
  # ffmpeg avutil pulls mediandk/android; whisper/ggml are static.
  target_link_libraries(ears PRIVATE android log mediandk atomic m dl)
  if(EXISTS "${CMAKE_SOURCE_DIR}/exports.txt")
    set(_ears_ver "${CMAKE_CURRENT_BINARY_DIR}/exports.ver")
    file(STRINGS "${CMAKE_SOURCE_DIR}/exports.txt" _ears_syms)
    set(_ears_ver_body "{\n  global:\n")
    foreach(_sym ${_ears_syms})
      string(REGEX REPLACE "^_" "" _sym "${_sym}")
      string(APPEND _ears_ver_body "    ${_sym};\n")
    endforeach()
    string(APPEND _ears_ver_body "  local:\n    *;\n};\n")
    file(WRITE "${_ears_ver}" "${_ears_ver_body}")
    target_link_libraries(ears PRIVATE "-Wl,--version-script,${_ears_ver}")
  endif()
else()
  target_link_libraries(ears PRIVATE pthread dl m)
  # Same GNU ld GOTPCREL/push bug as thumbnailer (libavformat).
  target_link_options(ears PRIVATE "-Wl,--no-relax")
  set_target_properties(ears PROPERTIES
    OUTPUT_NAME "ears"
    PREFIX "lib"
    BUILD_RPATH "${SBS_PREFIX}/lib"
    INSTALL_RPATH "${SBS_PREFIX}/lib"
  )
endif()

if(BUILD_TEST_EARS AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT ANDROID)
  add_executable(test_ears tools/test_ears.cpp)
  target_link_libraries(test_ears PRIVATE ears)
  if(NOT APPLE)
    target_link_libraries(test_ears PRIVATE dl)
  endif()
endif()

install(TARGETS ears LIBRARY DESTINATION lib)

cmake_minimum_required(VERSION 3.16)
# Must be set before project(): it is what makes the binaries run on macOS 11 and later.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "")
endif()
project(sermon_dsp LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)  # without this CMake adds no optimization flags at all
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(sermon_dsp STATIC
  src/declip.cpp
  src/declip_spade.cpp
  src/fft.cpp
  src/wpe.cpp
  src/voice_leveling.cpp
  src/loudness.cpp
  src/leveler.cpp
  src/biquad.cpp
  src/voice_eq.cpp
  src/final_lowpass.cpp
  src/normalize.cpp
  src/resample.cpp
  src/compressor.cpp
  src/deesser.cpp
  src/limiter.cpp
  src/audio_io.cpp
  src/noise_reduction.cpp
  src/pipeline.cpp
  src/trim.cpp
  src/analysis.cpp
  src/rt/stages_basic.cpp
  src/rt/deesser_rt.cpp
  src/rt/noise_rt.cpp
  src/rt/limiter_rt.cpp
  src/rt/chain.cpp
  src/rt/workfile.cpp
  src/rt/host.cpp
  src/rt/render.cpp
  src/mp3_export.cpp
  src/wav_export.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(sermon_dsp PUBLIC Threads::Threads)
target_include_directories(sermon_dsp PUBLIC src third_party)
if(MSVC)
  target_compile_options(sermon_dsp PRIVATE /W3 /utf-8 /D_USE_MATH_DEFINES /DNOMINMAX)
else()
  target_compile_options(sermon_dsp PRIVATE -Wall -Wextra)
endif()

# Optional third-party backends (see scripts/fetch_deps.sh, scripts/build_lame.sh).
set(RNNOISE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise)
if(EXISTS ${RNNOISE_DIR}/src/rnnoise_data.c)
  add_library(rnnoise STATIC
    ${RNNOISE_DIR}/src/denoise.c ${RNNOISE_DIR}/src/rnn.c ${RNNOISE_DIR}/src/pitch.c
    ${RNNOISE_DIR}/src/kiss_fft.c ${RNNOISE_DIR}/src/celt_lpc.c ${RNNOISE_DIR}/src/nnet.c
    ${RNNOISE_DIR}/src/nnet_default.c ${RNNOISE_DIR}/src/parse_lpcnet_weights.c
    ${RNNOISE_DIR}/src/rnnoise_data.c ${RNNOISE_DIR}/src/rnnoise_tables.c)
  target_include_directories(rnnoise PUBLIC ${RNNOISE_DIR}/include PRIVATE ${RNNOISE_DIR}/src)
  target_compile_definitions(rnnoise PRIVATE RNNOISE_BUILD)
  set_target_properties(rnnoise PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(sermon_dsp PUBLIC rnnoise)
  target_compile_definitions(sermon_dsp PUBLIC SERMON_HAVE_RNNOISE)
  message(STATUS "RNNoise: enabled")
else()
  message(STATUS "RNNoise: not found (run scripts/fetch_deps.sh) - noise reduction disabled")
endif()

# Cross builds (e.g. Intel on Apple silicon): -DCMAKE_OSX_ARCHITECTURES=x86_64 -DSERMON_ARCH=x86_64
# -DSERMON_DF_TARGET=x86_64-apple-darwin picks the matching third-party builds (lame-install-x86_64,
# DeepFilterNet/target/x86_64-apple-darwin). Windows (MinGW): -DSERMON_DF_TARGET=x86_64-pc-windows-gnu.
set(SERMON_ARCH "" CACHE STRING "Architecture suffix of the third-party libraries (empty = native)")
set(SERMON_DF_TARGET "" CACHE STRING "Rust target triple DeepFilterNet was built for (empty = native, e.g. x86_64-apple-darwin, x86_64-pc-windows-gnu)")
if(SERMON_ARCH)
  set(SERMON_SUFFIX "-${SERMON_ARCH}")
else()
  set(SERMON_SUFFIX "")
endif()
if(SERMON_DF_TARGET)
  set(DF_RELEASE_SUBDIR "${SERMON_DF_TARGET}/release")
else()
  set(DF_RELEASE_SUBDIR "release")
endif()

# DeepFilterNet (libDF C API, built with cargo; see scripts/build_deepfilternet.sh).
set(DF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/DeepFilterNet)
if(EXISTS ${DF_DIR}/target/${DF_RELEASE_SUBDIR}/libdf.a AND EXISTS ${DF_DIR}/models/DeepFilterNet3_onnx.tar.gz)
  target_link_libraries(sermon_dsp PUBLIC ${DF_DIR}/target/${DF_RELEASE_SUBDIR}/libdf.a)
  if(APPLE)
    target_link_libraries(sermon_dsp PUBLIC iconv)
  endif()
  if(WIN32)  # what Rust's standard library needs when linked from C++
    target_link_libraries(sermon_dsp PUBLIC ws2_32 userenv bcrypt ntdll advapi32 kernel32 synchronization)
  endif()
  target_compile_definitions(sermon_dsp PUBLIC SERMON_HAVE_DEEPFILTERNET
    SERMON_DF_MODEL_PATH="${DF_DIR}/models/DeepFilterNet3_onnx.tar.gz")
  message(STATUS "DeepFilterNet: enabled")
else()
  message(STATUS "DeepFilterNet: not found (run scripts/build_deepfilternet.sh) - using RNNoise")
endif()

set(LAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lame-install${SERMON_SUFFIX})
if(EXISTS ${LAME_DIR}/lib/libmp3lame.a)
  target_include_directories(sermon_dsp PUBLIC ${LAME_DIR}/include)
  target_link_libraries(sermon_dsp PUBLIC ${LAME_DIR}/lib/libmp3lame.a)
  target_compile_definitions(sermon_dsp PUBLIC SERMON_HAVE_LAME)
  message(STATUS "LAME: enabled")
else()
  message(STATUS "LAME: not found (run scripts/build_lame.sh) - MP3 export disabled")
endif()

enable_testing()
add_executable(dsp_tests tests/dsp_tests.cpp)
target_link_libraries(dsp_tests PRIVATE sermon_dsp)
add_test(NAME dsp_tests COMMAND dsp_tests)

add_executable(rt_tests tests/rt_tests.cpp)
target_link_libraries(rt_tests PRIVATE sermon_dsp)
add_test(NAME rt_tests COMMAND rt_tests)

add_executable(stage_tests tests/stage_tests.cpp)
target_link_libraries(stage_tests PRIVATE sermon_dsp)
add_test(NAME stage_tests COMMAND stage_tests)

add_executable(sermon_clean tools/sermon_clean.cpp)
target_link_libraries(sermon_clean PRIVATE sermon_dsp)
add_executable(sermon_host tools/sermon_host.cpp)
target_link_libraries(sermon_host PRIVATE sermon_dsp)
if(MINGW)  # self-contained executables (no MinGW runtime DLLs next to the sidecars)
  target_link_options(sermon_dsp PUBLIC -static)
endif()
add_executable(host_test tools/host_test.cpp)
target_link_libraries(host_test PRIVATE sermon_dsp)

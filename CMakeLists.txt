cmake_minimum_required(VERSION 3.20)

project(ClearMic VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# RNNoise has no upstream CMake project. Fetch a fixed source revision and its
# upstream model archive; verify both before compiling the library.
set(CLEARMIC_RNNOISE_REVISION 70f1d256acd4b34a572f999a05c87bf00b67730d)
set(CLEARMIC_RNNOISE_MODEL_HASH 0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37)
set(CLEARMIC_EXTERNAL_DIR "${CMAKE_BINARY_DIR}/_deps/clearmic")
file(MAKE_DIRECTORY "${CLEARMIC_EXTERNAL_DIR}")

set(_rnnoise_source_archive "${CLEARMIC_EXTERNAL_DIR}/rnnoise-${CLEARMIC_RNNOISE_REVISION}.tar.gz")
if(NOT EXISTS "${_rnnoise_source_archive}")
    file(DOWNLOAD
        "https://codeload.github.com/xiph/rnnoise/tar.gz/${CLEARMIC_RNNOISE_REVISION}"
        "${_rnnoise_source_archive}"
        EXPECTED_HASH SHA256=F61EE0B3F4C4CD337303E003D333357C5EAF25EF5D75A742109EE59E9A0A3932
        SHOW_PROGRESS
        STATUS _rnnoise_source_status)
    list(GET _rnnoise_source_status 0 _rnnoise_source_result)
    if(NOT _rnnoise_source_result EQUAL 0)
        message(FATAL_ERROR "Could not download RNNoise source: ${_rnnoise_source_status}")
    endif()
endif()
file(SHA256 "${_rnnoise_source_archive}" _rnnoise_source_actual_hash)
string(TOUPPER "${_rnnoise_source_actual_hash}" _rnnoise_source_actual_hash_upper)
if(NOT _rnnoise_source_actual_hash_upper STREQUAL "F61EE0B3F4C4CD337303E003D333357C5EAF25EF5D75A742109EE59E9A0A3932")
    message(FATAL_ERROR "Cached RNNoise source archive does not match the pinned SHA-256")
endif()

set(_rnnoise_source_root "${CLEARMIC_EXTERNAL_DIR}/rnnoise-${CLEARMIC_RNNOISE_REVISION}")
if(NOT EXISTS "${_rnnoise_source_root}/src/denoise.c")
    file(ARCHIVE_EXTRACT INPUT "${_rnnoise_source_archive}" DESTINATION "${CLEARMIC_EXTERNAL_DIR}")
endif()

set(_rnnoise_model_archive "${CLEARMIC_EXTERNAL_DIR}/rnnoise-model-${CLEARMIC_RNNOISE_MODEL_HASH}.tar.gz")
if(NOT EXISTS "${_rnnoise_model_archive}")
    file(DOWNLOAD
        "https://media.xiph.org/rnnoise/models/rnnoise_data-${CLEARMIC_RNNOISE_MODEL_HASH}.tar.gz"
        "${_rnnoise_model_archive}"
        EXPECTED_HASH SHA256=0A8755F8E2D834EFF6A54714ECC7D75F9932E845DF35F8B59BC52A7CFE6E8B37
        SHOW_PROGRESS
        STATUS _rnnoise_model_status)
    list(GET _rnnoise_model_status 0 _rnnoise_model_result)
    if(NOT _rnnoise_model_result EQUAL 0)
        message(FATAL_ERROR "Could not download RNNoise model: ${_rnnoise_model_status}")
    endif()
endif()
file(SHA256 "${_rnnoise_model_archive}" _rnnoise_model_actual_hash)
string(TOUPPER "${_rnnoise_model_actual_hash}" _rnnoise_model_actual_hash_upper)
string(TOUPPER "${CLEARMIC_RNNOISE_MODEL_HASH}" _rnnoise_model_expected_hash)
if(NOT "${_rnnoise_model_actual_hash_upper}" STREQUAL "${_rnnoise_model_expected_hash}")
    message(FATAL_ERROR "Cached RNNoise model archive does not match the pinned SHA-256")
endif()

set(_rnnoise_model_root "${CLEARMIC_EXTERNAL_DIR}/rnnoise-model-${CLEARMIC_RNNOISE_MODEL_HASH}")
if(NOT EXISTS "${_rnnoise_model_root}/src/rnnoise_data.c")
    file(MAKE_DIRECTORY "${_rnnoise_model_root}")
    file(ARCHIVE_EXTRACT INPUT "${_rnnoise_model_archive}" DESTINATION "${_rnnoise_model_root}")
endif()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/licenses/rnnoise")
configure_file("${_rnnoise_source_root}/COPYING" "${CMAKE_BINARY_DIR}/licenses/rnnoise/COPYING" COPYONLY)
install(FILES "${CMAKE_BINARY_DIR}/licenses/rnnoise/COPYING"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/clearmic/licenses/rnnoise")
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/clearmic")

add_library(rnnoise STATIC
    "${_rnnoise_source_root}/src/denoise.c"
    "${_rnnoise_source_root}/src/rnn.c"
    "${_rnnoise_source_root}/src/pitch.c"
    "${_rnnoise_source_root}/src/kiss_fft.c"
    "${_rnnoise_source_root}/src/celt_lpc.c"
    "${_rnnoise_source_root}/src/nnet.c"
    "${_rnnoise_source_root}/src/nnet_default.c"
    "${_rnnoise_source_root}/src/parse_lpcnet_weights.c"
    "${_rnnoise_source_root}/src/rnnoise_tables.c"
    "${_rnnoise_model_root}/src/rnnoise_data.c")
target_include_directories(rnnoise PUBLIC
    "${_rnnoise_source_root}/include"
    "${_rnnoise_source_root}/src"
    "${_rnnoise_model_root}/src")
if(UNIX AND NOT APPLE)
    target_link_libraries(rnnoise PRIVATE m)
endif()

add_library(clearmic_core
    audio-engine/core/src/device.cpp
    audio-engine/core/src/noise_suppressor.cpp
    audio-engine/core/src/processing.cpp
    audio-engine/core/src/processor_chain.cpp
    audio-engine/core/src/wav.cpp)
target_include_directories(clearmic_core PUBLIC audio-engine/core/include)
target_link_libraries(clearmic_core PRIVATE rnnoise)

if(WIN32)
    add_library(clearmic_platform audio-engine/platform/windows/src/device_manager.cpp
        audio-engine/platform/windows/src/live_capture.cpp
        audio-engine/platform/windows/src/live_processing.cpp
        audio-engine/platform/windows/src/desktop_application.cpp)
    target_include_directories(clearmic_platform PUBLIC audio-engine/platform/windows/include)
    target_link_libraries(clearmic_platform PUBLIC clearmic_core ole32 uuid setupapi cfgmgr32 bthprops comctl32 shell32 winmm)
    target_compile_definitions(clearmic_platform PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
    pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
    add_library(clearmic_platform audio-engine/platform/linux/src/device_manager.cpp
        audio-engine/platform/linux/src/bluez_battery.cpp
        audio-engine/platform/linux/src/upower_battery.cpp
        audio-engine/platform/linux/src/live_capture.cpp
        audio-engine/platform/linux/src/realtime_microphone.cpp
        audio-engine/platform/linux/src/desktop_application.cpp)
    target_include_directories(clearmic_platform PUBLIC audio-engine/platform/linux/include)
    target_link_libraries(clearmic_platform PUBLIC clearmic_core PkgConfig::PIPEWIRE PkgConfig::GTK3 PkgConfig::GSTREAMER PkgConfig::GIO)
else()
    message(FATAL_ERROR "ClearMic currently supports Windows and Linux.")
endif()

add_executable(clearmic-cli audio-engine/core/src/main.cpp)
target_link_libraries(clearmic-cli PRIVATE clearmic_core clearmic_platform)
install(TARGETS clearmic-cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(WIN32)
    add_executable(clearmic-desktop WIN32 audio-engine/platform/windows/src/desktop_main.cpp)
    target_link_libraries(clearmic-desktop PRIVATE clearmic_platform)
    target_compile_definitions(clearmic-desktop PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    install(TARGETS clearmic-desktop RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    include(InstallRequiredSystemLibraries)
    set(CPACK_GENERATOR WIX)
    set(CPACK_PACKAGE_VENDOR "ClearMic contributors")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local microphone enhancement for Windows")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "ClearMic")
    set(CPACK_PACKAGE_EXECUTABLES "clearmic-desktop" "ClearMic")
    set(CPACK_CREATE_DESKTOP_LINKS "clearmic-desktop")
    set(CPACK_WIX_UPGRADE_GUID "3DE1AD88-0F40-4DD5-861F-2D94D7DA6BBC")
    set(CPACK_WIX_PROGRAM_MENU_FOLDER "ClearMic")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(clearmic-core-tests audio-engine/tests/device_tests.cpp audio-engine/tests/wav_tests.cpp
        audio-engine/tests/processor_chain_tests.cpp)
    target_link_libraries(clearmic-core-tests PRIVATE clearmic_core)
    if(WIN32)
        target_include_directories(clearmic-core-tests PRIVATE audio-engine/platform/windows/include)
        add_executable(clearmic-virtual-cable-tests audio-engine/platform/windows/tests/virtual_cable_tests.cpp)
        target_include_directories(clearmic-virtual-cable-tests PRIVATE audio-engine/platform/windows/include)
        add_test(NAME clearmic-virtual-cable-tests COMMAND clearmic-virtual-cable-tests)
    endif()
    if(UNIX AND NOT APPLE)
        target_include_directories(clearmic-core-tests PRIVATE audio-engine/platform/linux/src)
        target_sources(clearmic-core-tests PRIVATE audio-engine/platform/linux/src/upower_battery.cpp)
        target_link_libraries(clearmic-core-tests PRIVATE PkgConfig::GIO)
    endif()
    add_test(NAME clearmic-core-tests COMMAND clearmic-core-tests)
    if(WIN32 OR UNIX AND NOT APPLE)
        add_test(NAME clearmic-invalid-record-test-gain COMMAND clearmic-cli record-test 1 original.wav processed.wav --input-gain-db=13)
        set_tests_properties(clearmic-invalid-record-test-gain PROPERTIES WILL_FAIL TRUE)
        add_test(NAME clearmic-invalid-record-test-switch COMMAND clearmic-cli record-test 1 original.wav processed.wav --noise-gate=maybe)
        set_tests_properties(clearmic-invalid-record-test-switch PROPERTIES WILL_FAIL TRUE)
        add_test(NAME clearmic-nonfinite-record-test-gain COMMAND clearmic-cli record-test 1 original.wav processed.wav --input-gain-db=nan)
        set_tests_properties(clearmic-nonfinite-record-test-gain PROPERTIES WILL_FAIL TRUE)
        add_test(NAME clearmic-zero-record-test-duration COMMAND clearmic-cli record-test 0 original.wav processed.wav)
        set_tests_properties(clearmic-zero-record-test-duration PROPERTIES WILL_FAIL TRUE)
        add_test(NAME clearmic-overlong-record-test-duration COMMAND clearmic-cli record-test 31 original.wav processed.wav)
        set_tests_properties(clearmic-overlong-record-test-duration PROPERTIES WILL_FAIL TRUE)
        add_test(NAME clearmic-malformed-record-test-duration COMMAND clearmic-cli record-test 1sec original.wav processed.wav)
        set_tests_properties(clearmic-malformed-record-test-duration PROPERTIES WILL_FAIL TRUE)
        add_test(NAME clearmic-negative-record-test-duration COMMAND clearmic-cli record-test -1 original.wav processed.wav)
        set_tests_properties(clearmic-negative-record-test-duration PROPERTIES WILL_FAIL TRUE)
    endif()
    if(UNIX AND NOT APPLE)
        add_executable(clearmic-pipewire-buffer-tests audio-engine/tests/pipewire_capture_buffer_tests.cpp)
        target_include_directories(clearmic-pipewire-buffer-tests PRIVATE audio-engine/platform/linux/include)
        add_test(NAME clearmic-pipewire-buffer-tests COMMAND clearmic-pipewire-buffer-tests)
        add_test(NAME clearmic-no-source-errors COMMAND clearmic-cli serve)
        set_tests_properties(clearmic-no-source-errors PROPERTIES WILL_FAIL TRUE)
        add_test(NAME clearmic-invalid-serve-option COMMAND clearmic-cli serve --unknown)
        set_tests_properties(clearmic-invalid-serve-option PROPERTIES WILL_FAIL TRUE)
        add_test(NAME clearmic-invalid-processing-switch COMMAND clearmic-cli serve --noise-gate=maybe)
        set_tests_properties(clearmic-invalid-processing-switch PROPERTIES WILL_FAIL TRUE)
    endif()
endif()

include(CPack)

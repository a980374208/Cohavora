cmake_minimum_required(VERSION 3.24)
project(cohavora_e2ee_backend_patch LANGUAGES CXX)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(SDK "${REPO_ROOT}/deps/webrtc")
set(PATCHED "${REPO_ROOT}/build-e2ee-backend/patched" CACHE PATH "Prepared crypto repair")
add_library(e2ee_backend_patch OBJECT "${PATCHED}/frame_crypto_transformer.cc" fatal_log.cpp)
target_compile_features(e2ee_backend_patch PUBLIC cxx_std_20)
target_compile_definitions(e2ee_backend_patch PUBLIC WEBRTC_WIN WIN32_LEAN_AND_MEAN NOMINMAX RTC_DISABLE_CHECK_MSG=1 RTC_DISABLE_CHECKING=1 RTC_NO_UNIQUE_ADDRESS= BORINGSSL_PREFIX=cohavora_bssl)
target_compile_options(e2ee_backend_patch PUBLIC /utf-8 /Zc:preprocessor)
target_include_directories(e2ee_backend_patch PUBLIC
    "${PATCHED}/include" "${SDK}/include" "${SDK}/include/out-x64-release/gen"
    "${SDK}/include/third_party/abseil-cpp"
    "${SDK}/include/third_party/boringssl/src/include" "${SDK}/include/boringssl-prefix/Release")
set_property(TARGET e2ee_backend_patch PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
add_executable(test_e2ee_key_epoch "${REPO_ROOT}/tests/security/test_e2ee_key_epoch.cpp")
target_include_directories(test_e2ee_key_epoch PRIVATE "${REPO_ROOT}")
target_link_libraries(test_e2ee_key_epoch PRIVATE e2ee_backend_patch "${SDK}/lib/Release/webrtc.lib"
    ws2_32 winmm secur32 crypt32 advapi32 iphlpapi dmoguids msdmo amstrmid wmcodecdspuuid
    avrt strmiids ole32 oleaut32 propsys d3d11 dxgi dwmapi shcore shell32)
set_property(TARGET test_e2ee_key_epoch PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
enable_testing()
add_test(NAME e2ee_key_epoch COMMAND test_e2ee_key_epoch)
set_tests_properties(e2ee_key_epoch PROPERTIES TIMEOUT 60)

# Flutter uses the full pinned source build prepared by
# tests/runtime/tools/media/prepare_e2ee_flutter_backend.py.

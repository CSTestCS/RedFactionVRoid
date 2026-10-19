cmake_minimum_required(VERSION 3.16)
project(RedFactionVRoid VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(RFV_CORE_SOURCES
  src/core/json.cpp
  src/core/ini.cpp
  src/core/log.cpp
  src/vrm/loader.cpp
  src/vrm/avatar.cpp
  src/game/pointer_path.cpp
  src/game/binding.cpp
)

if(WIN32)
  # ---------------------------------------------------------------- the mod (dxgi.dll)
  enable_language(ASM)
  if(NOT MINGW)
    message(FATAL_ERROR "The mod uses GNU assembler export trampolines; build it with MinGW-w64 "
                        "(e.g. cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake).")
  endif()

  set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
  add_custom_command(
    OUTPUT ${GEN_DIR}/default_config.inc
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/config/RFVRoid.ini -DOUT=${GEN_DIR}/default_config.inc
            -DNAME=kDefaultConfig -P ${CMAKE_SOURCE_DIR}/cmake/embed_text.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/config/RFVRoid.ini ${CMAKE_SOURCE_DIR}/cmake/embed_text.cmake
    COMMENT "Embedding default RFVRoid.ini")

  add_library(rfv_dxgi SHARED
    ${RFV_CORE_SOURCES}
    src/win/dllmain.cpp
    src/win/dxgi_proxy.cpp
    src/win/dxgi_exports.S
    src/win/dxgi.def
    src/win/paths.cpp
    src/win/vtable_hook.cpp
    src/win/memory_win.cpp
    src/win/d3d_hooks.cpp
    src/win/image_decode.cpp
    src/win/shader_compiler.cpp
    src/win/avatar_renderer.cpp
    src/win/ui_renderer.cpp
    src/win/input.cpp
    src/win/mod.cpp
    ${GEN_DIR}/default_config.inc
  )
  set_target_properties(rfv_dxgi PROPERTIES
    OUTPUT_NAME dxgi
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  target_include_directories(rfv_dxgi PRIVATE ${GEN_DIR} src/win)
  target_compile_definitions(rfv_dxgi PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0601 __USE_MINGW_ANSI_STDIO=1
    RFV_VERSION_STRING="${PROJECT_VERSION}")
  target_compile_options(rfv_dxgi PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type>)
  # Never link dxgi/d3d11 import libraries: this DLL *is* dxgi.dll. Everything is loaded at runtime.
  target_link_libraries(rfv_dxgi PRIVATE ole32 oleaut32 windowscodecs shell32 gdi32 user32 uuid dxguid)
  target_link_options(rfv_dxgi PRIVATE -static -static-libgcc -static-libstdc++ -s)

  # ---------------------------------------------------------------- installer (.exe)
  set(PAYLOAD_DLL ${CMAKE_BINARY_DIR}/bin/dxgi.dll)
  set(MANIFEST ${CMAKE_SOURCE_DIR}/src/installer/installer.manifest)
  set(RFV_VERSION ${PROJECT_VERSION})
  set(RFV_VERSION_COMMAS "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
  configure_file(src/installer/installer.rc.in ${GEN_DIR}/installer.rc @ONLY)
  add_executable(rfv_installer WIN32 src/installer/installer.cpp ${GEN_DIR}/installer.rc)
  set_source_files_properties(${GEN_DIR}/installer.rc PROPERTIES OBJECT_DEPENDS ${PAYLOAD_DLL})
  add_dependencies(rfv_installer rfv_dxgi)
  set_target_properties(rfv_installer PROPERTIES
    OUTPUT_NAME RFVRoid_Installer
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  target_include_directories(rfv_installer PRIVATE src/installer)
  target_compile_definitions(rfv_installer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(rfv_installer PRIVATE ole32 shell32 advapi32 uuid)
  target_link_options(rfv_installer PRIVATE -static -static-libgcc -static-libstdc++ -municode -s)
else()
  # ---------------------------------------------------------------- host unit tests
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(rfv_tests tests/test_main.cpp ${RFV_CORE_SOURCES})
  target_compile_options(rfv_tests PRIVATE -Wall -Wextra)
  set(TEST_DATA ${CMAKE_BINARY_DIR}/testdata)
  add_custom_command(
    OUTPUT ${TEST_DATA}/test_v1.vrm ${TEST_DATA}/test_v0.vrm ${TEST_DATA}/expected.json
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/make_test_vrm.py ${TEST_DATA}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/make_test_vrm.py)
  add_custom_target(rfv_testdata ALL DEPENDS ${TEST_DATA}/test_v1.vrm)
  add_dependencies(rfv_tests rfv_testdata)
  enable_testing()
  add_test(NAME rfv_tests COMMAND rfv_tests ${TEST_DATA})
endif()

if(WIN32)
  # Smoke-test harness (acts like a tiny D3D11 game that loads the mod). Not shipped.
  add_executable(rfv_smoke tests/win_smoke.cpp)
  set_target_properties(rfv_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  target_compile_definitions(rfv_smoke PRIVATE UNICODE _UNICODE)
  target_link_libraries(rfv_smoke PRIVATE uuid dxguid)
  target_link_options(rfv_smoke PRIVATE -static -static-libgcc -static-libstdc++ -municode)
endif()

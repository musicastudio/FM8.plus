cmake_minimum_required(VERSION 3.20)
project(FM8plus CXX)

# FM8 is x64-only; refuse anything else so a wrong-arch build cannot silently ship.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "FM8.plus must be built for x64")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/W4 /permissive- /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# --- MinHook (vendored) -----------------------------------------------------
add_subdirectory(third_party/minhook EXCLUDE_FROM_ALL)

# --- shared feature core ----------------------------------------------------
add_library(fm8plus_core STATIC
  src/core/fm8plus.cpp
  src/core/settings.cpp
  src/core/ui.cpp
  src/core/rsrc.cpp
  src/core/standalone.cpp)
target_include_directories(fm8plus_core PUBLIC
  src/core
  third_party/minhook/include
  "${CMAKE_BINARY_DIR}")   # build/gui/forms/*.h from tools/gen_forms.py (optional, __has_include)
target_link_libraries(fm8plus_core PUBLIC minhook Shlwapi Winmm Gdiplus)

# helper: make a shim that emits <name>.<ext> with no lib prefix
function(fm8_shim tgt src outname ext)
  add_library(${tgt} SHARED ${src})
  target_link_libraries(${tgt} PRIVATE fm8plus_core)
  set_target_properties(${tgt} PROPERTIES OUTPUT_NAME "${outname}" PREFIX "" SUFFIX ".${ext}")
endfunction()

# --- VST2 wrapper: builds FM8.plus.dll (distinct plug-in, loads stock FM8.dll) ---------------
fm8_shim(shim_vst2 src/shim_vst2/shim_vst2.cpp "FM8.plus" "dll")

# --- VST3 wrapper: builds FM8.plus.vst3 (single-file module, loads stock FM8.vst3) -----------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/shim_vst3/shim_vst3.cpp")
  # The Steinberg headers reference themselves via a "pluginterfaces/" prefix, so the vendored
  # submodule must be reachable under that name. Ensure a junction to it exists (idempotent).
  set(PI_LINK "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pluginterfaces")
  if(NOT EXISTS "${PI_LINK}/vst/ivstaudioprocessor.h")
    file(TO_NATIVE_PATH "${PI_LINK}" PI_LINK_NATIVE)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3_pluginterfaces" PI_SRC_NATIVE)
    execute_process(COMMAND cmd /c mklink /J "${PI_LINK_NATIVE}" "${PI_SRC_NATIVE}")
  endif()
  fm8_shim(shim_vst3 src/shim_vst3/shim_vst3.cpp "FM8.plus" "vst3")
  target_include_directories(shim_vst3 PRIVATE third_party)
endif()

# --- standalone launcher: builds FM8.plus.exe ------------------------------
# Starts the real FM8.exe and injects FM8.plus.dll, which runs the standalone attach. No core link
# needed (the injected DLL carries the features); this is a tiny GUI-subsystem executable.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/launcher/launcher.cpp")
  # No icon is embedded: the FM8+ icon is FM8's own icon with our overlay, so it is composited on
  # the user's machine from their own FM8 install at install time (never redistributed by us).
  add_executable(launcher WIN32 src/launcher/launcher.cpp)
  set_target_properties(launcher PROPERTIES OUTPUT_NAME "FM8.plus")
endif()

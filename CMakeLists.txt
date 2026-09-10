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
  src/core/ui.cpp)
target_include_directories(fm8plus_core PUBLIC
  src/core
  third_party/minhook/include)
target_link_libraries(fm8plus_core PUBLIC minhook Shlwapi Winmm)

# helper: make a shim that emits <name>.<ext> with no lib prefix
function(fm8_shim tgt src outname ext)
  add_library(${tgt} SHARED ${src})
  target_link_libraries(${tgt} PRIVATE fm8plus_core)
  set_target_properties(${tgt} PROPERTIES OUTPUT_NAME "${outname}" PREFIX "" SUFFIX ".${ext}")
endfunction()

# --- VST2 proxy: builds FM8.dll --------------------------------------------
fm8_shim(shim_vst2 src/shim_vst2/shim_vst2.cpp "FM8" "dll")

# --- VST3 proxy: builds FM8.vst3 (single-file module) ----------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/shim_vst3/shim_vst3.cpp")
  fm8_shim(shim_vst3 src/shim_vst3/shim_vst3.cpp "FM8" "vst3")
  target_include_directories(shim_vst3 PRIVATE third_party/vst3_pluginterfaces)
endif()

# --- standalone sideload: builds version.dll -------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/shim_exe/shim_exe.cpp")
  fm8_shim(shim_exe src/shim_exe/shim_exe.cpp "version" "dll")
  target_sources(shim_exe PRIVATE src/shim_exe/version.def)
endif()

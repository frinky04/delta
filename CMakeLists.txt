cmake_minimum_required(VERSION 3.20)
project(delta C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- glfw (vendored) ---------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)
find_package(OpenGL REQUIRED)

# --- dear imgui (vendored) ---------------------------------------------------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_USE_WCHAR32)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# --- hunspell (vendored, see third_party/hunspell/DELTA.md) -------------------
set(HUNSPELL_DIR ${CMAKE_SOURCE_DIR}/third_party/hunspell/src)
add_library(hunspell STATIC
  ${HUNSPELL_DIR}/affentry.cxx
  ${HUNSPELL_DIR}/affixmgr.cxx
  ${HUNSPELL_DIR}/csutil.cxx
  ${HUNSPELL_DIR}/filemgr.cxx
  ${HUNSPELL_DIR}/hashmgr.cxx
  ${HUNSPELL_DIR}/hunspell.cxx
  ${HUNSPELL_DIR}/hunzip.cxx
  ${HUNSPELL_DIR}/phonet.cxx
  ${HUNSPELL_DIR}/replist.cxx
  ${HUNSPELL_DIR}/suggestmgr.cxx)
target_include_directories(hunspell PUBLIC ${HUNSPELL_DIR})
target_compile_definitions(hunspell PUBLIC HUNSPELL_STATIC)
if(NOT MSVC)
  target_compile_options(hunspell PRIVATE -w)  # not our code to tidy
endif()

# --- embedded fonts and dictionaries -----------------------------------------
function(embed_file src name out_var)
  set(out ${CMAKE_BINARY_DIR}/generated/${name}.cpp)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -DIN=${src} -DOUT=${out} -DNAME=${name} -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${src} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Embedding ${name}")
  set(${out_var} ${out} PARENT_SCOPE)
endfunction()
embed_file(${CMAKE_SOURCE_DIR}/assets/NotoSans-Regular.ttf g_font_sans FONT_SANS_CPP)
embed_file(${CMAKE_SOURCE_DIR}/assets/NotoSansMono-Medium.ttf g_font_mono FONT_MONO_CPP)
embed_file(${CMAKE_SOURCE_DIR}/assets/NotoSans-SemiBold.ttf g_font_sans_bold FONT_SANS_BOLD_CPP)
embed_file(${CMAKE_SOURCE_DIR}/assets/NotoSansMono-SemiBold.ttf g_font_mono_bold FONT_MONO_BOLD_CPP)
embed_file(${CMAKE_SOURCE_DIR}/assets/dict/en_US.aff g_dict_en_us_aff DICT_EN_US_AFF_CPP)
embed_file(${CMAKE_SOURCE_DIR}/assets/dict/en_US.dic g_dict_en_us_dic DICT_EN_US_DIC_CPP)
embed_file(${CMAKE_SOURCE_DIR}/assets/dict/en_GB.aff g_dict_en_gb_aff DICT_EN_GB_AFF_CPP)
embed_file(${CMAKE_SOURCE_DIR}/assets/dict/en_GB.dic g_dict_en_gb_dic DICT_EN_GB_DIC_CPP)

# --- app ---------------------------------------------------------------------
add_library(delta_core STATIC
  src/editor.cpp
  src/markdown.cpp
  src/ini_syntax.cpp
  src/palette.cpp
  src/problems.cpp
  src/spell.cpp
  src/store.cpp
  src/config.cpp
  src/keys.cpp
  src/theme.cpp
  src/platform.cpp
  src/images.cpp
  ${FONT_SANS_CPP}
  ${FONT_MONO_CPP}
  ${FONT_SANS_BOLD_CPP}
  ${FONT_MONO_BOLD_CPP}
  ${DICT_EN_US_AFF_CPP}
  ${DICT_EN_US_DIC_CPP}
  ${DICT_EN_GB_AFF_CPP}
  ${DICT_EN_GB_DIC_CPP})
target_include_directories(delta_core PUBLIC src)
target_include_directories(delta_core PRIVATE third_party/stb third_party/glfw/deps)
find_package(Threads REQUIRED)
target_link_libraries(delta_core PUBLIC imgui hunspell Threads::Threads)
if(WIN32)
  target_link_libraries(delta_core PUBLIC dwmapi shell32 wininet)
endif()
if(MSVC)
  target_compile_options(delta_core PUBLIC /utf-8)
endif()

add_executable(delta WIN32 src/main.cpp src/app.cpp src/app_images.cpp src/image_cache.cpp)
target_include_directories(delta PRIVATE third_party/glfw/deps)
target_link_libraries(delta PRIVATE delta_core)
if(MINGW)
  # Self-contained exe: no libstdc++/winpthread DLLs needed next to it.
  target_link_options(delta PRIVATE -static)
endif()
if(MSVC)
  target_link_options(delta PRIVATE /ENTRY:mainCRTStartup)
endif()

# Headless tests (no window): build/delta_tests.exe
add_executable(delta_tests tests/test_main.cpp)
target_link_libraries(delta_tests PRIVATE delta_core)
if(MINGW)
  target_link_options(delta_tests PRIVATE -static)
endif()

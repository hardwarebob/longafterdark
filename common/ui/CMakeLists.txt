# adw_ui — the Windows 11 look LongAfterDark.scr's settings dialog and
# adimport's windows share (docs/COVERS.md §3): palettes for light,
# dark and high contrast, the accent, the Segoe UI Variable ramp, the
# custom-drawn stock controls, the header band, cover drawing and off-screen
# capture.
#
# A component built without it in AD_COMPONENTS pulls it in with
#   if(NOT TARGET adw_ui)
#     add_subdirectory(<path>/common/ui ${CMAKE_BINARY_DIR}/common/ui)
#   endif()
add_library(adw_ui STATIC
  src/theme.cc
  src/widgets.cc
  src/capture.cc
  src/image.cc)
target_include_directories(adw_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Windows 10 APIs (per-monitor-v2 DPI, DWM attributes).
target_compile_definitions(adw_ui PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_link_libraries(adw_ui PUBLIC gdiplus comctl32 uxtheme dwmapi windowscodecs ole32 oleacc user32 gdi32)

# ---- tests ------------------------------------------------------------------
# ui.theme (palettes, fonts, the theme-change filter, cards and the header
# band drawn into memory), ui.image (decoding, scaling and its cache, rounded
# corners and opacity, the generated cover, draw_cover), ui.capture (a
# parked, cloaked, never-activated window with a card button and a progress
# bar rendered to a PNG) and ui.slider (init_slider: its keys, its
# accessible name and value, and its drawing in light, dark and high
# contrast, enabled and disabled). Nothing is ever shown on the desktop.
enable_language(RC)
# The manifest activates Common Controls 6 (the subclass API is exported by
# name only there) and per-monitor DPI v2, as the apps' own manifests do.
add_executable(test_ui tests/test_ui.cc tests/test_ui.rc)
set_source_files_properties(tests/test_ui.rc PROPERTIES
  OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ui.manifest)
target_link_libraries(test_ui PRIVATE adw_ui)
set(_ui_scratch ${CMAKE_CURRENT_BINARY_DIR}/test-scratch)
foreach(_t theme image capture slider)
  add_test(NAME ui.${_t} COMMAND test_ui ${_t} ${_ui_scratch}/${_t})
  set_tests_properties(ui.${_t} PROPERTIES TIMEOUT 120)
endforeach()

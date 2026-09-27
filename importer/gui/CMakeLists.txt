# adw_import_gui — adimport's windows (docs/COVERS.md §4): the source
# chooser, the Internet Archive list, progress, the result, and the cover
# window, all themed with adw_ui. adimport.cc hands it a gui::Request (gui.h,
# frozen by COVERS.md §8.2); importer/CMakeLists.txt adds this directory and
# links adimport to it. This file never defines or changes `adimport`.
if(NOT TARGET adw_ui)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../common/ui ${CMAKE_BINARY_DIR}/common/ui)
endif()

add_library(adw_import_gui STATIC
  gui.cc
  model.cc
  page.cc
  pages.cc)
target_include_directories(adw_import_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(adw_import_gui PUBLIC adw_import PRIVATE adw_ui comctl32 shell32 ole32)

# ---- tests ------------------------------------------------------------------
# import.gui_model: the wording and data the windows show, and the file
#   dialogs' options and results (no windows).
# import.gui_shots: every page x light/dark/high contrast x 100/150/200%
#   rendered off-screen through adimport's screenshot hook (parked, cloaked
#   windows only, so it is not labelled gui); each PNG exists and its body
#   margin is pal.base.
# import.gui_flow (label gui: real windows on the desktop, briefly): Sources
#   -> 103 -> Back -> Cancel is exit 5, and Change cover on a scratch install
#   with a synthetic picture is exit 0 (skipped, 77, while covers.cc is C's
#   stub).
enable_language(RC)
add_executable(test_import_gui tests/test_gui.cc tests/test_gui.rc)
set_source_files_properties(tests/test_gui.rc PROPERTIES
  OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gui.manifest)
target_include_directories(test_import_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tests ${ADW_CORE_INCLUDE})
target_link_libraries(test_import_gui PRIVATE adw_import_gui adw_ui adw_import oleacc oleaut32 uuid)
set(_gui_scratch ${CMAKE_CURRENT_BINARY_DIR}/test-scratch)
add_test(NAME import.gui_model COMMAND test_import_gui model ${_gui_scratch}/model)
add_test(NAME import.gui_shots COMMAND test_import_gui shots $<TARGET_FILE:adimport> ${_gui_scratch}/shots)
add_test(NAME import.gui_flow COMMAND test_import_gui flow $<TARGET_FILE:adimport> ${_gui_scratch}/flow)
set_tests_properties(import.gui_shots PROPERTIES TIMEOUT 600)
set_tests_properties(import.gui_flow PROPERTIES LABELS gui SKIP_RETURN_CODE 77 TIMEOUT 300)

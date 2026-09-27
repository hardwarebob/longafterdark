# adw_lane_ne16 — the Classic lane: an After Dark 2.x/3.x module (NE .AD) run
# through the real OLDMOD16.DLL (+ AD_SND, ADXPL300, helpers) on the Win16
# guest runtime (host/win16), driven as AFTERDAR.SCR drove it through
# OLDMOD32 (docs/ABI.md §3). See lane.hh.

add_library(adw_lane_ne16 STATIC
  lane.cc
  bridge.cc
  package.cc
)
target_include_directories(adw_lane_ne16 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(adw_lane_ne16 PUBLIC adw_win16 adw_core)
target_compile_features(adw_lane_ne16 PUBLIC cxx_std_20)

# adhostwin links whatever library defines adw::make_ne16_lane() (core/README.md).
set_property(GLOBAL PROPERTY ADW_LANE_NE16_TARGET adw_lane_ne16)

# ---- tests ------------------------------------------------------------------
add_executable(adw_ne16_tests tests/test_ne16.cc)
target_link_libraries(adw_ne16_tests PRIVATE adw_lane_ne16)
# test_paths.h: the installed assets, and hosts kept off the real %LOCALAPPDATA%.
target_include_directories(adw_ne16_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests)
add_test(NAME ne16.unit COMMAND adw_ne16_tests)
# Runs real Classic modules from the imported assets through adhostwin; skips (77) when absent.
add_test(NAME ne16.assets COMMAND adw_ne16_tests --assets $<TARGET_FILE:adhostwin>)
set_tests_properties(ne16.assets PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
# The PACKAGES.md §4.4 package roots under AD_NE16_PKGROOTS (one root per
# package id: <root>\<id>\win\packages\<id>\…); skips (77) when unset or absent.
add_test(NAME ne16.pkg COMMAND adw_ne16_tests --pkg $<TARGET_FILE:adhostwin>)
set_tests_properties(ne16.pkg PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
# INTERACTION.md: the games' status (Caps Lock, keyboard hooks, LUNATIC's
# queue), the DOS Shell regression (18000 frames), and the module buttons in
# configure mode (hidden real dialogs, scripted; state in a temp ADSTATE).
# Needs the imported assets (all five packages for every case; each absent
# module is skipped); skips (77) when there are none.
add_test(NAME ne16.interaction COMMAND adw_ne16_tests --interaction $<TARGET_FILE:adhostwin>)
set_tests_properties(ne16.interaction PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)

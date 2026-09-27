# adw_core — screen surface, host protocol, env, virtual clock, pacing, and the
# adhostwin.exe host process. See host/core/README.md and DESIGN.md §1/§4.

add_library(adw_core STATIC
  src/audio_capture.cc
  src/audio_config.cc
  src/audio_engine.cc
  src/audio_format.cc
  src/audio_live.cc
  src/audio_smf.cc
  src/clock.cc
  src/env.cc
  src/host.cc
  src/log.cc
  src/module_probe.cc
  src/pacer.cc
  src/protocol.cc
  src/screen.cc
  src/status.cc
  src/test_pattern.cc
  src/text.cc
)
target_include_directories(adw_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(adw_core PUBLIC cxx_std_20)
# The audio engine's live sinks (AUDIO.md §6.6): WASAPI (ole32, uuid) and the
# waveOut / midiOut fallbacks (winmm).
target_link_libraries(adw_core PUBLIC ole32 winmm uuid)

add_executable(adhostwin src/adhostwin_main.cc)
target_link_libraries(adhostwin PRIVATE adw_core shell32)

# Lanes plug into adhostwin without core knowing their components: a lane
# component defines adw::make_pe32_lane() / adw::make_ne16_lane() (lane.h) in a
# library target and announces it with
#   set_property(GLOBAL PROPERTY ADW_LANE_PE32_TARGET <its target>)
#   set_property(GLOBAL PROPERTY ADW_LANE_NE16_TARGET <its target>)
# Core is configured before those components, so the linking is deferred to
# the end of the top-level directory, when every component has been added.
function(_adw_core_link_lanes)
  foreach(_lane PE32 NE16)
    get_property(_tgt GLOBAL PROPERTY ADW_LANE_${_lane}_TARGET)
    if(_tgt AND TARGET ${_tgt})
      target_link_libraries(adhostwin PRIVATE ${_tgt})
      target_compile_definitions(adhostwin PRIVATE ADW_HAVE_LANE_${_lane}=1)
      # The e2e test expects "lane not built in" (exit 3) only for lanes that
      # really are absent, and a routed run otherwise.
      target_compile_definitions(adw_core_e2e PRIVATE ADW_HAVE_LANE_${_lane}=1)
      message(STATUS "adhostwin: ${_lane} lane from ${_tgt}")
    elseif(_tgt)
      message(WARNING "ADW_LANE_${_lane}_TARGET names '${_tgt}', which is not a target; "
                      "adhostwin is built without the ${_lane} lane")
    endif()
  endforeach()
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL _adw_core_link_lanes)

# ---- tests ------------------------------------------------------------------
add_executable(adw_core_tests tests/test_core.cc)
target_link_libraries(adw_core_tests PRIVATE adw_core)
add_test(NAME core.unit COMMAND adw_core_tests)

# The audio engine (AUDIO.md §10.1). msacm32: the decoders are checked byte for
# byte against the host's own IMA / MS ADPCM codecs. The live-device check
# inside runs only with AD_AUDIO_LIVE_TEST=1 (a person listening; never CI).
add_executable(adw_core_audio_tests tests/test_audio.cc)
target_include_directories(adw_core_audio_tests PRIVATE src)
target_link_libraries(adw_core_audio_tests PRIVATE adw_core msacm32)
add_test(NAME core.audio COMMAND adw_core_audio_tests)
set_tests_properties(core.audio PROPERTIES TIMEOUT 120)

add_executable(adw_core_e2e tests/test_e2e.cc)
target_include_directories(adw_core_e2e PRIVATE src)
target_link_libraries(adw_core_e2e PRIVATE adw_core)
add_test(NAME core.e2e COMMAND adw_core_e2e $<TARGET_FILE:adhostwin>)
set_tests_properties(core.e2e PROPERTIES TIMEOUT 180)

# Lane detection against the real modules; skips (77) when the assets are absent.
add_test(NAME core.lane_detect_assets COMMAND adw_core_e2e $<TARGET_FILE:adhostwin> --assets)
set_tests_properties(core.lane_detect_assets PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# Real modules' sound (AUDIO.md §10.3): opt-in (AD_AUDIO_ASSETS=1 or a list of
# case names), and skips (77) without it, without the assets, or when no case
# can run in this build (its lane not linked). Each case's sound-on FBHASH
# stream and captures are also checked against tests/audio_baselines.txt
# (AD_AUDIO_BASELINE_WRITE=1 records them anew after an intended change).
target_compile_definitions(adw_core_e2e PRIVATE
  ADW_AUDIO_BASELINES="${CMAKE_CURRENT_SOURCE_DIR}/tests/audio_baselines.txt")
add_test(NAME core.audio_assets COMMAND adw_core_e2e $<TARGET_FILE:adhostwin> --audio-assets)
set_tests_properties(core.audio_assets PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600)

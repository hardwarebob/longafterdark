# adw_loader — PE32 / NE image parsing and placement (docs/DESIGN.md §3).
# Depends only on the standard library; no adw::cpu, no Windows API.
add_library(adw_loader STATIC image.cc pe.cc ne.cc)
# Consumers include "loader/pe.hh", "loader/ne.hh", "loader/image.hh".
target_include_directories(adw_loader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The dump logic is a library so the corpus test exercises it as well.
add_library(adw_loader_inspect STATIC tools/inspect.cc)
target_link_libraries(adw_loader_inspect PUBLIC adw_loader)

add_executable(adwinspect tools/adwinspect.cc)
target_link_libraries(adwinspect PRIVATE adw_loader_inspect)

# ---- tests ----------------------------------------------------------------
foreach(_t pe ne fuzz inspect corpus)
  add_executable(test_loader_${_t} tests/test_${_t}.cc)
  target_link_libraries(test_loader_${_t} PRIVATE adw_loader_inspect)
  add_test(NAME loader_${_t} COMMAND test_loader_${_t})
endforeach()
# The corpus test needs the original After Dark files; it exits 77 without them.
set_tests_properties(loader_corpus PROPERTIES SKIP_RETURN_CODE 77)

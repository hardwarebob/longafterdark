# adw_cpu — the x86 interpreter (flat 32-bit + 16-bit protected-mode segmentation).
# Vendored from resource_dasm's X86Emulator (MIT); see src/*.hh headers and
# LICENSE.resource_dasm. Contract: docs/DESIGN.md §2.

add_library(adw_cpu STATIC
  src/EmulatorBase.cc
  src/Expression.cc
  src/InterruptManager.cc
  src/MemoryContext.cc
  src/X86Emulator.cc
  src/X86Exec.cc
  src/X87.cc
)
target_include_directories(adw_cpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(adw_cpu PUBLIC phosg::phosg)

add_subdirectory(tests)

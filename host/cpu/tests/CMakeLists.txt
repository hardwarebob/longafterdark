# adw_cpu tests (docs/DESIGN.md §2).
#
#   cpu_segmentation  16-bit protected mode against a test LDT (far calls, retf n, SP wrap, lds/les, overrides,
#                     int n handlers, limit faults, lar/lsl/verr/verw/arpl, 0x66/0x67 inside 16-bit code)
#   cpu_memory_dib    DIB sections created over MemoryContext arenas alias emulated memory
#   cpu_x86_diff      differential test against the real CPU (x86_oracle, a 32-bit helper run under WOW64)
#   cpu_perf          flat-mode interpreter speed vs upstream resource_dasm's X86Emulator

add_executable(test_cpu_segmentation test_segmentation.cc)
target_link_libraries(test_cpu_segmentation PRIVATE adw_cpu)
add_test(NAME cpu_segmentation COMMAND test_cpu_segmentation)

add_executable(test_cpu_memory_dib test_memory_dib.cc)
target_link_libraries(test_cpu_memory_dib PRIVATE adw_cpu gdi32 user32)
add_test(NAME cpu_memory_dib COMMAND test_cpu_memory_dib)

# ---- Native oracle ------------------------------------------------------------------------------------------------
# x86_oracle must be a 32-bit program (it executes the snippets on the real CPU in 32-bit protected mode), so it is
# built with the i686 compiler from the same llvm-mingw rather than as a target of this (x86-64) project.
set(AD_ORACLE_EXE "${CMAKE_CURRENT_BINARY_DIR}/x86_oracle.exe")
if(AD_I686_CXX AND EXISTS "${AD_I686_CXX}")
  add_custom_command(
    OUTPUT "${AD_ORACLE_EXE}"
    COMMAND "${AD_I686_CXX}" -std=c++20 -O1 -static
            "${CMAKE_CURRENT_SOURCE_DIR}/x86_oracle.cc" -o "${AD_ORACLE_EXE}"
    DEPENDS x86_oracle.cc diff_proto.hh
    COMMENT "Building the 32-bit native oracle (x86_oracle.exe)"
    VERBATIM)
  add_custom_target(x86_oracle ALL DEPENDS "${AD_ORACLE_EXE}")
endif()

add_executable(test_cpu_x86_diff test_x86_diff.cc)
target_link_libraries(test_cpu_x86_diff PRIVATE adw_cpu)
if(TARGET x86_oracle)
  add_dependencies(test_cpu_x86_diff x86_oracle)
endif()
# Without the oracle (no i686 compiler, or no WOW64 to run it) the test exits 77 = skipped.
add_test(NAME cpu_x86_diff
         COMMAND test_cpu_x86_diff "${AD_ORACLE_EXE}" "${CMAKE_CURRENT_BINARY_DIR}/diff_scratch")
# A second, larger run with another seed (the cases are a pure function of the seed, so failures reproduce with
# `test_cpu_x86_diff <oracle> --seed ... --count-scale ... --explain`).
add_test(NAME cpu_x86_diff_seed2
         COMMAND test_cpu_x86_diff "${AD_ORACLE_EXE}" "${CMAKE_CURRENT_BINARY_DIR}/diff_scratch"
                 --seed 0x1996AD40 --count-scale 2)
set_tests_properties(cpu_x86_diff cpu_x86_diff_seed2 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

# ---- Performance ----------------------------------------------------------------------------------------------------
# Upstream's emulator, compiled straight from the resource_dasm clone (namespace ResourceDASM, so it links beside
# adw::cpu). Without the clone the benchmark still times adw::cpu and reports the comparison as skipped.
set(AD_RD_UPSTREAM "${AD_REPO_ROOT}/third_party/win/resource_dasm/src/Emulators")
add_executable(test_cpu_perf test_perf.cc)
target_link_libraries(test_cpu_perf PRIVATE adw_cpu)
if(EXISTS "${AD_RD_UPSTREAM}/X86Emulator.cc")
  add_library(rd_upstream_x86 STATIC
    "${AD_RD_UPSTREAM}/EmulatorBase.cc"
    "${AD_RD_UPSTREAM}/Expression.cc"
    "${AD_RD_UPSTREAM}/InterruptManager.cc"
    "${AD_RD_UPSTREAM}/MemoryContext.cc"
    "${AD_RD_UPSTREAM}/X86Emulator.cc")
  target_link_libraries(rd_upstream_x86 PUBLIC phosg::phosg)
  # Upstream's own code; its warnings are not ours to fix.
  target_compile_options(rd_upstream_x86 PRIVATE -w)
  # Upstream is written against C++23 (std::string::contains).
  set_target_properties(rd_upstream_x86 PROPERTIES CXX_STANDARD 23)
  target_link_libraries(test_cpu_perf PRIVATE rd_upstream_x86)
  target_include_directories(test_cpu_perf PRIVATE "${AD_RD_UPSTREAM}/..")
  target_compile_definitions(test_cpu_perf PRIVATE AD_HAVE_UPSTREAM_X86=1)
endif()
add_test(NAME cpu_perf COMMAND test_cpu_perf)
set_tests_properties(cpu_perf PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 600)

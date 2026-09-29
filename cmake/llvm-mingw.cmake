# Toolchain file: the portable llvm-mingw that tools/bootstrap.sh
# unpacks into third_party/toolchains. Supports both native Windows x86_64
# and cross-compilation from Linux to Windows x86_64.
#
# Exactly the pinned version (LLVM_MINGW_VER in tools/versions), never
# whichever llvm-mingw-* folder happens to be there: after a bump, with the
# old folder still present, the build must not quietly keep the old compiler
# (nor a build directory the one it was configured with: AD_LLVM_MINGW is
# computed on every run, never taken from the cache). -DAD_LLVM_MINGW_DIR=<dir>
# overrides it.
get_filename_component(_AD_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(AD_LLVM_MINGW_DIR)
  set(AD_LLVM_MINGW "${AD_LLVM_MINGW_DIR}")
else()
  file(STRINGS "${_AD_ROOT}/tools/versions" _AD_LLVM_VER REGEX "^LLVM_MINGW_VER=" LIMIT_COUNT 1)
  string(REGEX REPLACE "^LLVM_MINGW_VER=([^ \t\r#]*).*$" "\\1" _AD_LLVM_VER "${_AD_LLVM_VER}")
  if(NOT _AD_LLVM_VER)
    message(FATAL_ERROR "no LLVM_MINGW_VER in ${_AD_ROOT}/tools/versions")
  endif()
  set(AD_LLVM_MINGW "${_AD_ROOT}/third_party/toolchains/llvm-mingw-${_AD_LLVM_VER}-ucrt-x86_64")
  if(NOT EXISTS "${AD_LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++" AND
     NOT EXISTS "${AD_LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++.exe")
    message(FATAL_ERROR "llvm-mingw ${_AD_LLVM_VER} (tools/versions) is not installed at "
                        "${AD_LLVM_MINGW}; run: bash tools/bootstrap.sh")
  endif()
endif()

if(CMAKE_HOST_WIN32)
  set(_EXE ".exe")
else()
  set(_EXE "")
  set(CMAKE_SYSTEM_NAME Windows)
  set(CMAKE_SYSTEM_PROCESSOR x86_64)
  find_program(WINE_EXECUTABLE NAMES wine wine64 PATHS /usr/lib/wine)
  if(WINE_EXECUTABLE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${WINE_EXECUTABLE}")
  endif()
endif()

set(CMAKE_C_COMPILER   "${AD_LLVM_MINGW}/bin/x86_64-w64-mingw32-clang${_EXE}")
set(CMAKE_CXX_COMPILER "${AD_LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++${_EXE}")
set(CMAKE_RC_COMPILER  "${AD_LLVM_MINGW}/bin/x86_64-w64-mingw32-windres${_EXE}")
# 32-bit compiler, used only by test oracles that execute x86 natively under
# WOW64. A plain variable, like the compilers: it follows the pinned version
# (a cache entry would keep an older one in an existing build directory).
set(AD_I686_CXX "${AD_LLVM_MINGW}/bin/i686-w64-mingw32-clang++${_EXE}")

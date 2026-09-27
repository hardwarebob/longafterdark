// The smoke tests' levers (the AD_SCR_TEST_* and AD_SCR_TESTEXIT_* variables,
// AD_UI_TEST_HC_SCHEME for the dialog's screenshot; scr/README.md "Test
// hooks") are compiled only into LongAfterDark-test.scr, the build the tests
// run (scr/CMakeLists.txt defines AD_SCR_TEST_HOOKS=1 for it). The
// LongAfterDark.scr that is packaged and installed is the same code with
// AD_SCR_TEST_HOOKS=0: it never reads them, so nothing left in an environment
// can keep the full-screen saver from ending (AD_SCR_TEST_IGNORE_INPUT), stand
// in for the real monitors or input, or make it write files where it says.
// The diagnostic overrides (AD_SCR_LOG, AD_SCR_HOSTLOG, AD_SCR_STRETCH,
// AD_SCR_PRESENT, AD_SETTINGS, AD_ASSETS_DIR ...) are in both.
#pragma once

#ifndef AD_SCR_TEST_HOOKS
#define AD_SCR_TEST_HOOKS 0
#endif

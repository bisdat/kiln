// SPDX-License-Identifier: MIT
// regions_win_test's stand-in for another mod's DLL (othermod.dll, next to the test): a frame of a module that is neither
// the game, Kiln nor the system, between a "game" caller and the save-region hook.
extern "C" __declspec(dllexport) int othermod_call(int (*fn)(int), int value) {
    volatile int keep = value;
    const int r = fn(keep);
    return r + keep;  // not a tail call: this frame stays on the stack while fn runs
}

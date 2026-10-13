// TTEST=1 builds (test builds, Dolphin only): instead of the game, the BabySlime's oracle replay
// against the game's own code translated, then against the hand port, timed with the Gekko's
// timebase; their rows and the numbers go to Dolphin's log (Source/TranslatedTest.cpp).
#pragma once

#if MLGC_TTEST
// Runs the replays and reports; returns when they're done ("MLGC TTEST DONE" in the log).
void RunTranslatedTest();
#endif

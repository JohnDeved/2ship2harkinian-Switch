#pragma once

static const char* SWITCH_CPU_PROFILES[] = {
    "Maximum Performance", "High Performance",   "Boost Performance",  "Stock Performance",
    "Powersaving Mode 1",  "Powersaving Mode 2", "Powersaving Mode 3",
};

static unsigned SWITCH_CPU_SPEEDS_VALUES[] = { 1785000000, 1581000000, 1224000000, 1020000000,
                                               918000000,  816000000,  714000000 };

// GPU clock speeds (Hz) per performance profile.
// Docked max: 768 MHz, Handheld max: 460 MHz. Stock docked: 384 MHz, stock handheld: 307 MHz.
static unsigned SWITCH_GPU_SPEEDS_VALUES[] = { 768000000, 691000000, 537000000, 384000000,
                                               307000000, 307000000, 307000000 };

// EMC (memory controller) clock speeds (Hz) per performance profile.
// Max: 1600 MHz. Stock: 1065 MHz (handheld) / 1331 MHz (docked).
static unsigned SWITCH_EMC_SPEEDS_VALUES[] = { 1600000000, 1600000000, 1331000000, 1065000000,
                                               1065000000, 1065000000, 1065000000 };
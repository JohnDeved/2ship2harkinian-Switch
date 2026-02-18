#ifndef QUICKBAR_H
#define QUICKBAR_H

#include <vector>
#include <cstdint>

enum QuickBarCategory {
    QB_CAT_MASKS = 0,
    QB_CAT_TOOLS = 1,
    QB_CAT_SONGS = 2,
    QB_CAT_BOTTLES = 3,
    QB_CAT_COUNT = 4
};

struct QuickBarState {
    bool isOpen;
    QuickBarCategory openCategory;
    int selectionIndex;
    int activeToolItem;             // ItemId of the current Active Tool (or -1)
    int lastUsed[QB_CAT_COUNT];     // Last-used item per category (ItemId or -1)
    int dpadHoldFrames[QB_CAT_COUNT];
    bool dpadWasHeld[QB_CAT_COUNT]; // True if hold threshold was reached
    float fadeAlpha;                // For fade-out animation

    // Items currently visible in the open category
    std::vector<int> currentItems;
    std::vector<const char*> currentNames;
};

QuickBarState& GetQuickBarState();
bool IsQuickBarEnabled();

#endif // QUICKBAR_H

#include "BenDrownedDebugWindow.h"

void BenDrownedDebugWindow::DrawElement() {
    ImGui::PushID("BenDrownedDebugPopout");
    RenderBenDrownedDebugSection();
    ImGui::PopID();
}

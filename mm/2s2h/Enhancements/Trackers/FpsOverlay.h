#pragma once

#include <ship/window/gui/GuiWindow.h>

class FpsOverlayWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override;
    void DrawElement() override{};
    void Draw() override;
    void UpdateElement() override{};
};

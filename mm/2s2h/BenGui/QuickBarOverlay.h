#ifndef QUICKBAR_OVERLAY_H
#define QUICKBAR_OVERLAY_H

#include <ship/window/gui/GuiWindow.h>

class QuickBarOverlayWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override;
    void DrawElement() override {};
    void Draw() override;
    void UpdateElement() override {};
};

#endif // QUICKBAR_OVERLAY_H

#pragma once

#include "ship/controller/controldevice/controller/mapping/sdl/SDLMapping.h"
#include "ship/controller/controldevice/controller/mapping/ControllerInputMapping.h"

// Raw joystick buttons (not mapped by SDL GameController) are stored as
// SDL_CONTROLLER_BUTTON_MAX + joystickButtonIndex.  Helper to test/convert:
#define IS_RAW_JOYSTICK_BUTTON(v) ((v) >= SDL_CONTROLLER_BUTTON_MAX)
#define RAW_JOYSTICK_BUTTON_INDEX(v) ((v) - SDL_CONTROLLER_BUTTON_MAX)
#define MAKE_RAW_JOYSTICK_BUTTON(idx) (SDL_CONTROLLER_BUTTON_MAX + (idx))

namespace Ship {
class SDLButtonToAnyMapping : virtual public ControllerInputMapping {
  public:
    SDLButtonToAnyMapping(int32_t sdlControllerButton);
    virtual ~SDLButtonToAnyMapping();
    std::string GetPhysicalInputName() override;
    std::string GetPhysicalDeviceName() override;

  protected:
    int32_t mControllerButton;

  private:
    std::string GetGenericButtonName();
};
} // namespace Ship

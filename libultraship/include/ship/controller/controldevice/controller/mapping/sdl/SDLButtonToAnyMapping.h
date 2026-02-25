#pragma once

#include "ship/controller/controldevice/controller/mapping/sdl/SDLMapping.h"
#include "ship/controller/controldevice/controller/mapping/ControllerInputMapping.h"

// Raw joystick buttons (not mapped by SDL GameController) are stored as
// SDL_CONTROLLER_BUTTON_MAX + joystickButtonIndex.  Helper to test/convert:
#define IS_RAW_JOYSTICK_BUTTON(v) ((v) >= SDL_CONTROLLER_BUTTON_MAX)
#define RAW_JOYSTICK_BUTTON_INDEX(v) ((v) - SDL_CONTROLLER_BUTTON_MAX)
#define MAKE_RAW_JOYSTICK_BUTTON(idx) (SDL_CONTROLLER_BUTTON_MAX + (idx))

namespace Ship {

// Returns the first pressed raw joystick button index that is NOT already
// mapped by the SDL GameController layer, or -1 if none found.
int32_t FindFirstUnmappedRawJoystickButton(SDL_GameController* gamepad);

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

#pragma once

#include <cstdint>
#include <string>

#include "ship/controller/controldevice/controller/mapping/ControllerGyroMapping.h"

namespace Ship {

class SwitchGyroMapping final : public ControllerGyroMapping {
  public:
    SwitchGyroMapping(uint8_t portIndex, float sensitivity, float neutralPitch, float neutralYaw, float neutralRoll);
    void UpdatePad(float& x, float& y) override;
    void SaveToConfig() override;
    void EraseFromConfig() override;
    void Recalibrate() override;
    std::string GetGyroMappingId() override;
    std::string GetPhysicalDeviceName() override;

  private:
    float mNeutralPitch;
    float mNeutralYaw;
    float mNeutralRoll;

#ifdef __SWITCH__
    bool EnsureHidReady();
    bool EnsureSensorReady();
    bool RefreshSensorHandlesIfNeeded();
    bool ReadAngularVelocity(float& outPitch, float& outYaw, float& outRoll);

    // Store libnx handles as raw u32 values so this header stays portable.
    uint32_t mHandleValues[2] = { 0, 0 };
    int32_t mHandleCount = 0;
    uint32_t mCachedStyleSet = 0;
    int32_t mCachedNpadId = -1;
    uint32_t mCachedStyleTag = 0;
    bool mSensorsStarted = false;
    bool mHasNeutral = false;
#endif
};

} // namespace Ship

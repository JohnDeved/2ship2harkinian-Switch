#include "ship/controller/controldevice/controller/mapping/switch/SwitchGyroMapping.h"

#include <spdlog/spdlog.h>

#include "ship/Context.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/utils/StringHelper.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace Ship {

SwitchGyroMapping::SwitchGyroMapping(uint8_t portIndex, float sensitivity, float neutralPitch, float neutralYaw,
                                     float neutralRoll)
    : ControllerInputMapping(PhysicalDeviceType::SDLGamepad),
      ControllerGyroMapping(PhysicalDeviceType::SDLGamepad, portIndex, sensitivity),
      mNeutralPitch(neutralPitch),
      mNeutralYaw(neutralYaw),
      mNeutralRoll(neutralRoll) {
}

void SwitchGyroMapping::UpdatePad(float& x, float& y) {
#ifdef __SWITCH__
    if (Context::GetInstance()->GetControlDeck()->GamepadGameInputBlocked()) {
        x = 0;
        y = 0;
        return;
    }

    if (!EnsureSensorReady()) {
        x = 0;
        y = 0;
        return;
    }

    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    if (!ReadAngularVelocity(pitch, yaw, roll)) {
        x = 0;
        y = 0;
        return;
    }

    // Lazily calibrate on the first successful read so we don't touch HID state during early startup.
    if (!mHasNeutral) {
        mNeutralPitch = pitch;
        mNeutralYaw = yaw;
        mNeutralRoll = roll;
        mHasNeutral = true;
        x = 0;
        y = 0;
        return;
    }

    x = (pitch - mNeutralPitch) * mSensitivity;
    y = (yaw - mNeutralYaw) * mSensitivity;
    return;
#else
    (void)x;
    (void)y;
    x = 0;
    y = 0;
#endif
}

void SwitchGyroMapping::Recalibrate() {
#ifdef __SWITCH__
    if (!EnsureSensorReady()) {
        mNeutralPitch = 0;
        mNeutralYaw = 0;
        mNeutralRoll = 0;
        return;
    }

    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    if (!ReadAngularVelocity(pitch, yaw, roll)) {
        mNeutralPitch = 0;
        mNeutralYaw = 0;
        mNeutralRoll = 0;
        return;
    }

    mNeutralPitch = pitch;
    mNeutralYaw = yaw;
    mNeutralRoll = roll;
    mHasNeutral = true;
#else
    mNeutralPitch = 0;
    mNeutralYaw = 0;
    mNeutralRoll = 0;
#endif
}

std::string SwitchGyroMapping::GetGyroMappingId() {
    return StringHelper::Sprintf("P%d", mPortIndex);
}

std::string SwitchGyroMapping::GetPhysicalDeviceName() {
    return "Switch (SixAxis)";
}

void SwitchGyroMapping::SaveToConfig() {
    const std::string mappingCvarKey = CVAR_PREFIX_CONTROLLERS ".GyroMappings." + GetGyroMappingId();

    Ship::Context::GetInstance()->GetConsoleVariables()->SetString(
        StringHelper::Sprintf("%s.GyroMappingClass", mappingCvarKey.c_str()).c_str(), "SwitchGyroMapping");
    Ship::Context::GetInstance()->GetConsoleVariables()->SetFloat(
        StringHelper::Sprintf("%s.Sensitivity", mappingCvarKey.c_str()).c_str(), mSensitivity);
    Ship::Context::GetInstance()->GetConsoleVariables()->SetFloat(
        StringHelper::Sprintf("%s.NeutralPitch", mappingCvarKey.c_str()).c_str(), mNeutralPitch);
    Ship::Context::GetInstance()->GetConsoleVariables()->SetFloat(
        StringHelper::Sprintf("%s.NeutralYaw", mappingCvarKey.c_str()).c_str(), mNeutralYaw);
    Ship::Context::GetInstance()->GetConsoleVariables()->SetFloat(
        StringHelper::Sprintf("%s.NeutralRoll", mappingCvarKey.c_str()).c_str(), mNeutralRoll);

    Ship::Context::GetInstance()->GetConsoleVariables()->Save();
}

void SwitchGyroMapping::EraseFromConfig() {
    const std::string mappingCvarKey = CVAR_PREFIX_CONTROLLERS ".GyroMappings." + GetGyroMappingId();

    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.GyroMappingClass", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.Sensitivity", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.NeutralPitch", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.NeutralYaw", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.NeutralRoll", mappingCvarKey.c_str()).c_str());

    Ship::Context::GetInstance()->GetConsoleVariables()->Save();
}


#ifdef __SWITCH__
bool SwitchGyroMapping::EnsureHidReady() {
    // libnx no longer exposes hidScanInput() in all versions (padUpdate is the preferred wrapper),
    // but we only need to ensure Npad is initialized and configured for SixAxis reads.
    static bool sHidConfigured = false;
    if (!sHidConfigured) {
        sHidConfigured = true;

        hidInitializeNpad();

        // Allow the common controller styles/ids required for Joy-Cons / Pro / Handheld.
        hidSetSupportedNpadStyleSet(HidNpadStyleSet_NpadStandard);
        const HidNpadIdType ids[] = {
            HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
            HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8,
            HidNpadIdType_Handheld,
        };
        hidSetSupportedNpadIdType(ids, sizeof(ids) / sizeof(ids[0]));
    }
    return true;
}

static HidNpadStyleTag SelectStyleTag(u32 styleSet) {
    if (styleSet & HidNpadStyleTag_NpadHandheld) {
        return HidNpadStyleTag_NpadHandheld;
    }
    if (styleSet & HidNpadStyleTag_NpadFullKey) {
        return HidNpadStyleTag_NpadFullKey;
    }
    if (styleSet & HidNpadStyleTag_NpadJoyDual) {
        return HidNpadStyleTag_NpadJoyDual;
    }
    if (styleSet & HidNpadStyleTag_NpadJoyLeft) {
        return HidNpadStyleTag_NpadJoyLeft;
    }
    if (styleSet & HidNpadStyleTag_NpadJoyRight) {
        return HidNpadStyleTag_NpadJoyRight;
    }
    // Fallback.
    return HidNpadStyleTag_NpadFullKey;
}

bool SwitchGyroMapping::RefreshSensorHandlesIfNeeded() {
    auto stopAndClear = [this]() {
        if (mSensorsStarted) {
            for (int i = 0; i < mHandleCount; i++) {
                if (mHandleValues[i] == 0) {
                    continue;
                }
                HidSixAxisSensorHandle h{};
                h.type_value = mHandleValues[i];
                hidStopSixAxisSensor(h);
            }
        }

        mSensorsStarted = false;
        mHandleCount = 0;
        mHandleValues[0] = 0;
        mHandleValues[1] = 0;
        mCachedStyleSet = 0;
        mCachedNpadId = -1;
        mCachedStyleTag = 0;
    };

    // Prefer the player index controller id, but fall back to Handheld for port 0.
    const HidNpadIdType candidateIds[] = {
        static_cast<HidNpadIdType>(HidNpadIdType_No1 + mPortIndex),
        mPortIndex == 0 ? HidNpadIdType_Handheld : static_cast<HidNpadIdType>(-1),
    };

    for (const auto candidate : candidateIds) {
        if ((int)candidate < 0) {
            continue;
        }

        const u32 styleSet = hidGetNpadStyleSet(candidate);
        if (styleSet == 0) {
            continue;
        }
        const HidNpadStyleTag styleTag = SelectStyleTag(styleSet);
        const int32_t totalHandles = (styleTag == HidNpadStyleTag_NpadJoyDual) ? 2 : 1;

        if (mSensorsStarted && (mCachedStyleSet == styleSet) && (mCachedNpadId == (int)candidate) &&
            (mCachedStyleTag == (u32)styleTag) && (mHandleCount == totalHandles)) {
            return true;
        }

        // Stop any previously started sensors before re-binding.
        stopAndClear();

        HidSixAxisSensorHandle handles[2] = {};
        Result rc = hidGetSixAxisSensorHandles(handles, totalHandles, candidate, styleTag);
        if (R_FAILED(rc)) {
            continue;
        }

        mHandleCount = totalHandles;
        for (int i = 0; i < 2; i++) {
            mHandleValues[i] = 0;
        }
        for (int i = 0; i < totalHandles; i++) {
            mHandleValues[i] = handles[i].type_value;
        }

        // Start sensors.
        for (int i = 0; i < totalHandles; i++) {
            Result startRc = hidStartSixAxisSensor(handles[i]);
            if (R_FAILED(startRc)) {
                SPDLOG_WARN("hidStartSixAxisSensor failed: 0x{:X}", (u32)startRc);
            }
            // Standard drift mode tends to be a good default for aim.
            hidSetGyroscopeZeroDriftMode(handles[i], HidGyroscopeZeroDriftMode_Standard);
        }

        mCachedStyleSet = styleSet;
        mCachedNpadId = (int)candidate;
        mCachedStyleTag = (u32)styleTag;
        mSensorsStarted = true;
        return true;
    }

    // Nothing active for this port; stop any sensors that might still be running.
    stopAndClear();
    return false;
}

bool SwitchGyroMapping::EnsureSensorReady() {
    if (!EnsureHidReady()) {
        return false;
    }
    return RefreshSensorHandlesIfNeeded();
}

bool SwitchGyroMapping::ReadAngularVelocity(float& outPitch, float& outYaw, float& outRoll) {
    outPitch = 0.0f;
    outYaw = 0.0f;
    outRoll = 0.0f;

    int valid = 0;
    for (int i = 0; i < mHandleCount; i++) {
        if (mHandleValues[i] == 0) {
            continue;
        }
        HidSixAxisSensorHandle h{};
        h.type_value = mHandleValues[i];

        HidSixAxisSensorState states[4];
        const size_t n = hidGetSixAxisSensorStates(h, states, 4);
        if (n == 0) {
            continue;
        }
        const auto& s = states[n - 1];

        outPitch += s.angular_velocity.x;
        outYaw += s.angular_velocity.y;
        outRoll += s.angular_velocity.z;
        valid++;
    }

    if (valid == 0) {
        return false;
    }

    outPitch /= (float)valid;
    outYaw /= (float)valid;
    outRoll /= (float)valid;
    return true;
}
#endif

} // namespace Ship

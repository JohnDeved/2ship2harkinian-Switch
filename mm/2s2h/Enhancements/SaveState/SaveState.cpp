#include "SaveState.h"

#include <spdlog/spdlog.h>

#include "2s2h/BenPort.h"
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
#include "z64save.h"
#include "variables.h"
}

extern "C" PlayState* gPlayState;
extern "C" u8* gAudioHeap;
extern "C" u8* gSystemHeap;
extern "C" MtxF* sMatrixStack;
extern "C" MtxF* sCurrentMatrix;
extern "C" LightsBuffer sLightsBuffer;

#define MATRIX_STACK_SIZE 20

struct SaveStateInfo {
    unsigned char* sysHeapCopy;
    unsigned char* audioHeapCopy;

    SaveContext saveContextCopy;
    LightsBuffer lightBufferCopy;
    MtxF mtxStackCopy[MATRIX_STACK_SIZE];
    MtxF currentMtxCopy;
    AudioContext audioCtxCopy;

    bool occupied;
};

// SaveState

SaveState::SaveState(unsigned int slot) : slot(slot), info(std::make_unique<SaveStateInfo>()) {
    info->sysHeapCopy = new unsigned char[SYSTEM_HEAP_SIZE];
    info->audioHeapCopy = new unsigned char[AUDIO_HEAP_SIZE];
    info->occupied = false;
}

SaveState::~SaveState() {
    delete[] info->sysHeapCopy;
    delete[] info->audioHeapCopy;
}

void SaveState::Save() {
    memcpy(info->sysHeapCopy, gSystemHeap, SYSTEM_HEAP_SIZE);
    memcpy(info->audioHeapCopy, gAudioHeap, AUDIO_HEAP_SIZE);

    memcpy(&info->saveContextCopy, &gSaveContext, sizeof(SaveContext));
    memcpy(&info->lightBufferCopy, &sLightsBuffer, sizeof(LightsBuffer));
    memcpy(&info->mtxStackCopy, sMatrixStack, sizeof(MtxF) * MATRIX_STACK_SIZE);
    memcpy(&info->currentMtxCopy, sCurrentMatrix, sizeof(MtxF));
    memcpy(&info->audioCtxCopy, &gAudioCtx, sizeof(AudioContext));

    info->occupied = true;
}

void SaveState::Load() {
    if (!info->occupied) {
        return;
    }

    memcpy(gSystemHeap, info->sysHeapCopy, SYSTEM_HEAP_SIZE);
    memcpy(gAudioHeap, info->audioHeapCopy, AUDIO_HEAP_SIZE);

    memcpy(&gSaveContext, &info->saveContextCopy, sizeof(SaveContext));
    memcpy(&sLightsBuffer, &info->lightBufferCopy, sizeof(LightsBuffer));
    memcpy(sMatrixStack, &info->mtxStackCopy, sizeof(MtxF) * MATRIX_STACK_SIZE);
    memcpy(sCurrentMatrix, &info->currentMtxCopy, sizeof(MtxF));
    memcpy(&gAudioCtx, &info->audioCtxCopy, sizeof(AudioContext));
}

// SaveStateMgr

SaveStateMgr::SaveStateMgr() : currentSlot(0) {
}

SaveStateMgr::~SaveStateMgr() = default;

void SaveStateMgr::SetCurrentSlot(unsigned int slot) {
    this->currentSlot = slot;
}

unsigned int SaveStateMgr::GetCurrentSlot() {
    return this->currentSlot;
}

SaveStateReturn SaveStateMgr::AddRequest(const SaveStateRequest request) {
    if (gPlayState == nullptr) {
        SPDLOG_ERROR("[2S2H] Can not save or load a state outside of \"GamePlay\"");
        Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGameOverlay()->TextDrawNotification(
            1.0f, true, "states not available here");
        return SaveStateReturn::FAIL_WRONG_GAMESTATE;
    }

    switch (request.type) {
        case RequestType::SAVE:
            requests.push(request);
            return SaveStateReturn::SUCCESS;
        case RequestType::LOAD:
            if (states.contains(request.slot)) {
                requests.push(request);
                return SaveStateReturn::SUCCESS;
            } else {
                SPDLOG_ERROR("[2S2H] State Slot {} is empty", request.slot);
                Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGameOverlay()->TextDrawNotification(
                    1.0f, true, "state slot %u empty", request.slot);
                return SaveStateReturn::FAIL_STATE_EMPTY;
            }
        [[unlikely]] default:
            SPDLOG_ERROR("[2S2H] Invalid SaveState request type: Unknown ({})", static_cast<int>(request.type));
            return SaveStateReturn::FAIL_BAD_REQUEST;
    }
}

void SaveStateMgr::ProcessSaveStateRequests() {
    while (!this->requests.empty()) {
        const auto& request = this->requests.front();

        switch (request.type) {
            case RequestType::SAVE:
                if (!this->states.contains(request.slot)) {
                    this->states[request.slot] = std::make_shared<SaveState>(request.slot);
                }
                this->states[request.slot]->Save();
                Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGameOverlay()->TextDrawNotification(
                    1.0f, true, "saved state %u", request.slot);
                break;
            case RequestType::LOAD:
                if (this->states.contains(request.slot)) {
                    this->states[request.slot]->Load();
                    Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGameOverlay()->TextDrawNotification(
                        1.0f, true, "loaded state %u", request.slot);
                } else {
                    SPDLOG_ERROR("[2S2H] Invalid SaveState slot: {}", request.slot);
                }
                break;
            [[unlikely]] default:
                SPDLOG_ERROR("[2S2H] Invalid SaveState request type: Unknown ({})", static_cast<int>(request.type));
                break;
        }
        this->requests.pop();
    }
}

extern "C" void ProcessSaveStateRequests() {
    if (OTRGlobals::Instance && OTRGlobals::Instance->gSaveStateMgr) {
        OTRGlobals::Instance->gSaveStateMgr->ProcessSaveStateRequests();
    }
}

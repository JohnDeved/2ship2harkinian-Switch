#include "SaveState.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <spdlog/spdlog.h>

#include "2s2h/BenPort.h"
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
#include "z64save.h"
#include "variables.h"
#include "functions.h"
#include "sequence.h"
}

extern "C" PlayState* gPlayState;
extern "C" u8* gAudioHeap;
extern "C" u8* gSystemHeap;
extern "C" MtxF* sMatrixStack;
extern "C" MtxF* sCurrentMatrix;
extern "C" LightsBuffer sLightsBuffer;
extern "C" ActiveSequence gActiveSeqs[];

#define MATRIX_STACK_SIZE 20
#define NUM_SEQ_PLAYERS 5
#define SAVE_STATE_MAX_SLOTS 6

// Sparse page-based heap storage: only stores non-zero 4KB pages
// to minimize memory usage. Typical game heaps are 50-80% zero.
static constexpr size_t PAGE_SIZE = 4096;

struct SparseHeap {
    size_t heapSize;
    size_t pageCount;
    size_t storedPages; // number of non-zero pages
    uint8_t* bitmap;    // 1 bit per page: 1 = stored, 0 = zero
    uint8_t* data;      // packed non-zero page data

    SparseHeap() : heapSize(0), pageCount(0), storedPages(0), bitmap(nullptr), data(nullptr) {
    }

    ~SparseHeap() {
        Free();
    }

    void Free() {
        delete[] bitmap;
        delete[] data;
        bitmap = nullptr;
        data = nullptr;
        storedPages = 0;
    }

    static bool IsPageZero(const uint8_t* page, size_t len) {
        // Check in 8-byte chunks for speed, using memcpy to avoid strict-aliasing UB
        size_t count64 = len / sizeof(uint64_t);
        for (size_t i = 0; i < count64; i++) {
            uint64_t chunk = 0;
            std::memcpy(&chunk, page + i * sizeof(uint64_t), sizeof(uint64_t));
            if (chunk != 0) {
                return false;
            }
        }
        // Check remaining bytes
        for (size_t i = count64 * sizeof(uint64_t); i < len; i++) {
            if (page[i] != 0) {
                return false;
            }
        }
        return true;
    }

    bool Save(const uint8_t* heap, size_t size) {
        Free();
        heapSize = size;
        pageCount = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        size_t bitmapBytes = (pageCount + 7) / 8;

        try {
            bitmap = new uint8_t[bitmapBytes];
        } catch (const std::bad_alloc&) { return false; }
        memset(bitmap, 0, bitmapBytes);

        // First pass: count non-zero pages and build bitmap
        storedPages = 0;
        for (size_t i = 0; i < pageCount; i++) {
            size_t offset = i * PAGE_SIZE;
            size_t len = std::min(PAGE_SIZE, size - offset);
            if (!IsPageZero(heap + offset, len)) {
                bitmap[i / 8] |= (1 << (i % 8));
                storedPages++;
            }
        }

        // Second pass: copy non-zero pages into compact buffer
        if (storedPages > 0) {
            try {
                data = new uint8_t[storedPages * PAGE_SIZE];
            } catch (const std::bad_alloc&) {
                Free();
                return false;
            }
            size_t dst = 0;
            for (size_t i = 0; i < pageCount; i++) {
                if (bitmap[i / 8] & (1 << (i % 8))) {
                    size_t offset = i * PAGE_SIZE;
                    size_t len = std::min(PAGE_SIZE, size - offset);
                    memcpy(data + dst * PAGE_SIZE, heap + offset, len);
                    if (len < PAGE_SIZE) {
                        memset(data + dst * PAGE_SIZE + len, 0, PAGE_SIZE - len);
                    }
                    dst++;
                }
            }
        }
        return true;
    }

    void Load(uint8_t* heap) const {
        // Zero entire heap first
        memset(heap, 0, heapSize);
        // Restore non-zero pages
        size_t src = 0;
        for (size_t i = 0; i < pageCount; i++) {
            if (bitmap[i / 8] & (1 << (i % 8))) {
                size_t offset = i * PAGE_SIZE;
                size_t len = std::min(PAGE_SIZE, heapSize - offset);
                memcpy(heap + offset, data + src * PAGE_SIZE, len);
                src++;
            }
        }
    }

    size_t GetStoredBytes() const {
        size_t bitmapBytes = (pageCount + 7) / 8;
        return bitmapBytes + storedPages * PAGE_SIZE;
    }
};

struct SaveStateInfo {
    SparseHeap sysHeapCopy;
    SparseHeap audioHeapCopy;

    SaveContext saveContextCopy;
    LightsBuffer lightBufferCopy;
    MtxF mtxStackCopy[MATRIX_STACK_SIZE];
    MtxF currentMtxCopy;
    AudioContext audioCtxCopy;

    // Active sequence state (BSS globals, not on any heap)
    ActiveSequence activeSeqsCopy[NUM_SEQ_PLAYERS];

    // Unrelocated seq script state (pc/stack stored as offsets from gAudioHeap)
    SeqScriptState seqScriptStateCopy[NUM_SEQ_PLAYERS];

    bool occupied;
};

// SaveState

SaveState::SaveState(unsigned int slot) : slot(slot), info(std::make_unique<SaveStateInfo>()) {
    info->occupied = false;
}

SaveState::~SaveState() = default;

bool SaveState::Save() {
    if (!info->sysHeapCopy.Save(gSystemHeap, SYSTEM_HEAP_SIZE)) {
        SPDLOG_ERROR("[2S2H] Save state slot {}: failed to allocate system heap copy", slot);
        return false;
    }
    if (!info->audioHeapCopy.Save(gAudioHeap, AUDIO_HEAP_SIZE)) {
        SPDLOG_ERROR("[2S2H] Save state slot {}: failed to allocate audio heap copy", slot);
        info->sysHeapCopy.Free();
        return false;
    }

    memcpy(&info->saveContextCopy, &gSaveContext, sizeof(SaveContext));
    memcpy(&info->lightBufferCopy, &sLightsBuffer, sizeof(LightsBuffer));
    memcpy(&info->mtxStackCopy, sMatrixStack, sizeof(MtxF) * MATRIX_STACK_SIZE);
    memcpy(&info->currentMtxCopy, sCurrentMatrix, sizeof(MtxF));
    memcpy(&info->audioCtxCopy, &gAudioCtx, sizeof(AudioContext));
    memcpy(&info->activeSeqsCopy, gActiveSeqs, sizeof(ActiveSequence) * NUM_SEQ_PLAYERS);

    // Unrelocate seq script state: store pc/stack as offsets from gAudioHeap
    // so they remain valid after audio heap restore (following SoH's approach)
    for (int i = 0; i < NUM_SEQ_PLAYERS; i++) {
        SeqScriptState* src = &gAudioCtx.seqPlayers[i].scriptState;
        SeqScriptState* dst = &info->seqScriptStateCopy[i];
        dst->value = src->value;
        dst->depth = src->depth;
        memcpy(dst->remLoopIters, src->remLoopIters, sizeof(dst->remLoopIters));
        dst->pc = (u8*)((uintptr_t)src->pc - (uintptr_t)gAudioHeap);
        for (int j = 0; j < 4; j++) {
            dst->stack[j] = (u8*)((uintptr_t)src->stack[j] - (uintptr_t)gAudioHeap);
        }
    }

    info->occupied = true;

    size_t totalBytes = info->sysHeapCopy.GetStoredBytes() + info->audioHeapCopy.GetStoredBytes() +
                        sizeof(SaveContext) + sizeof(LightsBuffer) + sizeof(MtxF) * (MATRIX_STACK_SIZE + 1) +
                        sizeof(AudioContext) + sizeof(ActiveSequence) * NUM_SEQ_PLAYERS;
    SPDLOG_INFO("[2S2H] Save state slot {}: {:.1f} MB (sys {}/{} pages, audio {}/{} pages)", slot,
                totalBytes / (1024.0 * 1024.0), info->sysHeapCopy.storedPages, info->sysHeapCopy.pageCount,
                info->audioHeapCopy.storedPages, info->audioHeapCopy.pageCount);
    return true;
}

void SaveState::Load() {
    if (!info->occupied) {
        return;
    }

    info->sysHeapCopy.Load(gSystemHeap);
    info->audioHeapCopy.Load(gAudioHeap);

    memcpy(&gSaveContext, &info->saveContextCopy, sizeof(SaveContext));
    memcpy(&sLightsBuffer, &info->lightBufferCopy, sizeof(LightsBuffer));
    memcpy(sMatrixStack, &info->mtxStackCopy, sizeof(MtxF) * MATRIX_STACK_SIZE);
    memcpy(sCurrentMatrix, &info->currentMtxCopy, sizeof(MtxF));
    memcpy(&gAudioCtx, &info->audioCtxCopy, sizeof(AudioContext));
    memcpy(gActiveSeqs, &info->activeSeqsCopy, sizeof(ActiveSequence) * NUM_SEQ_PLAYERS);

    // Relocate seq script state: convert offsets back to absolute pointers
    for (int i = 0; i < NUM_SEQ_PLAYERS; i++) {
        SeqScriptState* src = &info->seqScriptStateCopy[i];
        SeqScriptState* dst = &gAudioCtx.seqPlayers[i].scriptState;
        dst->value = src->value;
        dst->depth = src->depth;
        memcpy(dst->remLoopIters, src->remLoopIters, sizeof(dst->remLoopIters));
        dst->pc = (u8*)((uintptr_t)src->pc + (uintptr_t)gAudioHeap);
        for (int j = 0; j < 4; j++) {
            dst->stack[j] = (u8*)((uintptr_t)src->stack[j] + (uintptr_t)gAudioHeap);
        }
    }

    // Force-close the pause menu to prevent glitchy rendering when loading
    // a state that was saved while the menu was open (the pre-rendered
    // pause background is not captured by the save state).
    if (gPlayState != nullptr) {
        gPlayState->pauseCtx.state = 0;       // PAUSE_STATE_OFF
        gPlayState->pauseCtx.debugEditor = 0; // DEBUG_EDITOR_NONE
    }

    // Kick the audio system to resync sequence playback after heap restore.
    Audio_Update();
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
    if (request.slot >= SAVE_STATE_MAX_SLOTS) {
        SPDLOG_ERROR("[2S2H] Invalid State Slot Number {}", request.slot);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGameOverlay()->TextDrawNotification(
            1.0f, true, "invalid slot %u", request.slot);
        return SaveStateReturn::FAIL_INVALID_SLOT;
    }

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
    // Process at most one request per frame to avoid large single-frame hitches
    if (this->requests.empty()) {
        return;
    }

    const auto& request = this->requests.front();

    switch (request.type) {
        case RequestType::SAVE:
            if (!this->states.contains(request.slot)) {
                this->states[request.slot] = std::make_shared<SaveState>(request.slot);
            }
            if (this->states[request.slot]->Save()) {
                Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGameOverlay()->TextDrawNotification(
                    1.0f, true, "saved state %u", request.slot);
            } else {
                Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGameOverlay()->TextDrawNotification(
                    1.0f, true, "save failed (out of memory)");
            }
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

extern "C" void ProcessSaveStateRequests() {
    if (OTRGlobals::Instance && OTRGlobals::Instance->gSaveStateMgr) {
        OTRGlobals::Instance->gSaveStateMgr->ProcessSaveStateRequests();
    }
}

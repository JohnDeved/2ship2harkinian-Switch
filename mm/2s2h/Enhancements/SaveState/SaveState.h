#ifndef SAVE_STATE_H
#define SAVE_STATE_H

#include <cstdint>
#include <memory>
#include <queue>
#include <unordered_map>

enum class SaveStateReturn {
    SUCCESS,
    FAIL_INVALID_SLOT,
    FAIL_STATE_EMPTY,
    FAIL_WRONG_GAMESTATE,
    FAIL_BAD_REQUEST,
};

enum class RequestType {
    SAVE,
    LOAD,
};

typedef struct SaveStateRequest {
    unsigned int slot;
    RequestType type;
} SaveStateRequest;

struct SaveStateInfo;

class SaveState {
  public:
    SaveState(unsigned int slot);
    ~SaveState();

    bool Save();
    void Load();

  private:
    unsigned int slot;
    std::unique_ptr<SaveStateInfo> info;
};

class SaveStateMgr {
  public:
    SaveStateMgr();
    ~SaveStateMgr();

    SaveStateReturn AddRequest(const SaveStateRequest request);
    void ProcessSaveStateRequests();

    void SetCurrentSlot(unsigned int slot);
    unsigned int GetCurrentSlot();

    SaveStateMgr& operator=(const SaveStateMgr& rhs) = delete;
    SaveStateMgr(const SaveStateMgr& rhs) = delete;

  private:
    unsigned int currentSlot;
    std::unordered_map<unsigned int, std::shared_ptr<SaveState>> states;
    std::queue<SaveStateRequest> requests;
};

#ifdef __cplusplus
extern "C" {
#endif

void ProcessSaveStateRequests(void);

#ifdef __cplusplus
}
#endif

#endif

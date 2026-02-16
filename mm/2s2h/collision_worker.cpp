#include "collision_worker.h"

#include <mutex>
#include <condition_variable>
#include <thread>

extern "C" void CollisionCheck_OC(struct PlayState* play, CollisionCheckContext* colChkCtx);

static struct {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv_submit;
    std::condition_variable cv_done;
    bool running{false};
    bool has_work{false};
    bool work_done{true};

    // Work parameters
    struct PlayState* play;
    CollisionCheckContext* colChkCtx;
} oc_worker;

static void CollisionWorker_Thread() {
    std::unique_lock<std::mutex> lock(oc_worker.mutex);
    while (oc_worker.running) {
        while (!oc_worker.has_work && oc_worker.running) {
            oc_worker.cv_submit.wait(lock);
        }
        if (!oc_worker.running) {
            break;
        }

        // Capture work params and release lock during execution
        struct PlayState* play = oc_worker.play;
        CollisionCheckContext* colChkCtx = oc_worker.colChkCtx;
        lock.unlock();

        CollisionCheck_OC(play, colChkCtx);

        lock.lock();
        oc_worker.has_work = false;
        oc_worker.work_done = true;
        oc_worker.cv_done.notify_one();
    }
}

static std::once_flag oc_init_flag;

extern "C" void CollisionWorker_Init(void) {
    std::call_once(oc_init_flag, [] {
        std::unique_lock<std::mutex> lock(oc_worker.mutex);
        oc_worker.running = true;
        oc_worker.thread = std::thread(CollisionWorker_Thread);
    });
}

extern "C" void CollisionWorker_Destroy(void) {
    {
        std::unique_lock<std::mutex> lock(oc_worker.mutex);
        oc_worker.running = false;
    }
    oc_worker.cv_submit.notify_all();
    if (oc_worker.thread.joinable()) {
        oc_worker.thread.join();
    }
}

extern "C" void CollisionWorker_SubmitOC(struct PlayState* play, CollisionCheckContext* colChkCtx) {
    {
        std::unique_lock<std::mutex> lock(oc_worker.mutex);
        oc_worker.play = play;
        oc_worker.colChkCtx = colChkCtx;
        oc_worker.has_work = true;
        oc_worker.work_done = false;
    }
    oc_worker.cv_submit.notify_one();
}

extern "C" void CollisionWorker_WaitOC(void) {
    std::unique_lock<std::mutex> lock(oc_worker.mutex);
    while (!oc_worker.work_done) {
        oc_worker.cv_done.wait(lock);
    }
}

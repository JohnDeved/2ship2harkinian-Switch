#include "collision_worker.h"

#include <mutex>
#include <condition_variable>
#include <thread>

#ifdef __SWITCH__
#include <switch.h>
#endif

static struct {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv_submit;
    std::condition_variable cv_done;
    bool running{ false };
    bool has_work{ false };
    bool work_done{ true };

    // Current task
    void (*task)(void*);
    void* arg;
} worker;

static void TaskWorker_Thread() {
#ifdef __SWITCH__
    // Pin collision worker to core 2 to avoid contention with the main thread
    // (core 0). Core 3 is reserved for the system. Note: when the pool
    // (TaskWorkerPool) is also active, its workers occupy cores 1 and 2; the
    // single TaskWorker is used for the legacy code path. If core 2 is
    // unavailable, fall back to any core.
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, (1U << 2));
    if (R_FAILED(rc)) {
        static const u64 ALL_CORES_MASK = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3);
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, ALL_CORES_MASK);
    }
#endif

    std::unique_lock<std::mutex> lock(worker.mutex);
    while (worker.running) {
        while (!worker.has_work && worker.running) {
            worker.cv_submit.wait(lock);
        }
        if (!worker.running) {
            break;
        }

        // Capture task and release lock during execution
        void (*current_task)(void*) = worker.task;
        void* task_arg = worker.arg;
        lock.unlock();

        current_task(task_arg);

        lock.lock();
        worker.has_work = false;
        worker.work_done = true;
        worker.cv_done.notify_one();
    }
}

static std::once_flag worker_init_flag;

extern "C" void TaskWorker_Init(void) {
    std::call_once(worker_init_flag, [] {
        std::unique_lock<std::mutex> lock(worker.mutex);
        worker.running = true;
        worker.thread = std::thread(TaskWorker_Thread);
    });
}

extern "C" void TaskWorker_Destroy(void) {
    {
        std::unique_lock<std::mutex> lock(worker.mutex);
        worker.running = false;
    }
    worker.cv_submit.notify_all();
    if (worker.thread.joinable()) {
        worker.thread.join();
    }
}

extern "C" void TaskWorker_Submit(void (*task)(void*), void* arg) {
    {
        std::unique_lock<std::mutex> lock(worker.mutex);
        if (!worker.running) {
            // Worker not started — run inline to avoid deadlock
            lock.unlock();
            task(arg);
            return;
        }
        // Keep release builds safe: never overwrite an in-flight task.
        // In normal use this loop is not hit (submit->wait discipline).
        while (!worker.work_done) {
            worker.cv_done.wait(lock);
        }
        worker.task = task;
        worker.arg = arg;
        worker.has_work = true;
        worker.work_done = false;
    }
    worker.cv_submit.notify_one();
}

extern "C" void TaskWorker_Wait(void) {
    std::unique_lock<std::mutex> lock(worker.mutex);
    if (!worker.running) {
        return; // Worker not started — Submit already ran inline
    }
    while (!worker.work_done) {
        worker.cv_done.wait(lock);
    }
}

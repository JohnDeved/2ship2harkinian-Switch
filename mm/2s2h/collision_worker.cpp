#include "collision_worker.h"
#include "DeveloperTools/FrameProfiler.h"

#include <cassert>
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
    bool running{false};
    bool has_work{false};
    bool work_done{true};

    // Current task
    void (*task)(void*);
    void* arg;
} worker;

static void TaskWorker_Thread() {
#ifdef __SWITCH__
    // Pin collision worker to core 1 to keep it off the main/render core (0).
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, (1U << 1));
#endif

    std::unique_lock<std::mutex> lock(worker.mutex);
    while (worker.running) {
        FrameProfiler_StartPhase(PROFILE_PHASE_WORKER_IDLE);
        while (!worker.has_work && worker.running) {
            worker.cv_submit.wait(lock);
        }
        FrameProfiler_EndPhase(PROFILE_PHASE_WORKER_IDLE);
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
        assert(worker.work_done && "TaskWorker_Submit: previous task still in flight");
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

// ── Worker Pool (2 threads: cores 1 + 3) ───────────────────────────────

struct PoolWorker {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv_submit;
    std::condition_variable cv_done;
    bool running{false};
    bool has_work{false};
    bool work_done{true};
    void (*task)(void*);
    void* arg;
};

static PoolWorker sPool[2];

static void PoolWorker_Thread(PoolWorker* pw, [[maybe_unused]] int coreId) {
#ifdef __SWITCH__
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, coreId, (1U << coreId));
#endif

    std::unique_lock<std::mutex> lock(pw->mutex);
    while (pw->running) {
        while (!pw->has_work && pw->running) {
            pw->cv_submit.wait(lock);
        }
        if (!pw->running) break;

        void (*current_task)(void*) = pw->task;
        void* task_arg = pw->arg;
        lock.unlock();

        current_task(task_arg);

        lock.lock();
        pw->has_work = false;
        pw->work_done = true;
        pw->cv_done.notify_one();
    }
}

static std::once_flag pool_init_flag;

extern "C" void TaskWorkerPool_Init(void) {
    std::call_once(pool_init_flag, [] {
        // Worker 0 → core 1, Worker 1 → core 3
        static const int coreIds[2] = { 1, 3 };
        for (int i = 0; i < 2; i++) {
            std::unique_lock<std::mutex> lock(sPool[i].mutex);
            sPool[i].running = true;
            sPool[i].thread = std::thread(PoolWorker_Thread, &sPool[i], coreIds[i]);
        }
    });
}

extern "C" void TaskWorkerPool_Submit2(void (*task1)(void*), void* arg1,
                                       void (*task2)(void*), void* arg2) {
    void (*tasks[2])(void*) = { task1, task2 };
    void* args[2] = { arg1, arg2 };
    for (int i = 0; i < 2; i++) {
        std::unique_lock<std::mutex> lock(sPool[i].mutex);
        if (!sPool[i].running) {
            lock.unlock();
            tasks[i](args[i]);
            continue;
        }
        assert(sPool[i].work_done && "TaskWorkerPool: previous task still in flight");
        sPool[i].task = tasks[i];
        sPool[i].arg = args[i];
        sPool[i].has_work = true;
        sPool[i].work_done = false;
        lock.unlock();
        sPool[i].cv_submit.notify_one();
    }
}

extern "C" void TaskWorkerPool_Wait(void) {
    for (int i = 0; i < 2; i++) {
        std::unique_lock<std::mutex> lock(sPool[i].mutex);
        if (!sPool[i].running) continue;
        while (!sPool[i].work_done) {
            sPool[i].cv_done.wait(lock);
        }
    }
}

extern "C" void TaskWorkerPool_Destroy(void) {
    for (int i = 0; i < 2; i++) {
        {
            std::unique_lock<std::mutex> lock(sPool[i].mutex);
            sPool[i].running = false;
        }
        sPool[i].cv_submit.notify_all();
        if (sPool[i].thread.joinable()) {
            sPool[i].thread.join();
        }
    }
}

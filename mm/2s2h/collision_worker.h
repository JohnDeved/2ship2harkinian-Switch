#ifndef COLLISION_WORKER_H
#define COLLISION_WORKER_H

#include "z64.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * General-purpose worker thread for offloading CPU work to idle cores.
 * Used for collision, effects, and other independent update phases.
 * Only one task may be in-flight at a time (submit → wait → submit → wait).
 */

/**
 * Initialize the worker thread. Called once at startup.
 */
void TaskWorker_Init(void);

/**
 * Shutdown the worker thread. Called at exit.
 */
void TaskWorker_Destroy(void);

/**
 * Submit a task to run on the worker thread.
 * Only one task can be in-flight at a time. The caller must call
 * TaskWorker_Wait() before submitting another task.
 * Submitting while a task is in-flight is undefined behavior.
 * @param task  Function pointer to execute on the worker thread
 * @param arg   Opaque argument passed to the task function. Must remain
 *              valid until TaskWorker_Wait() returns.
 */
void TaskWorker_Submit(void (*task)(void*), void* arg);

/**
 * Block until the current in-flight task completes.
 */
void TaskWorker_Wait(void);

/**
 * Worker pool API — manages 2 worker threads (cores 1 + 3 on Switch).
 * Allows submitting exactly 2 independent tasks that run in parallel.
 */

/**
 * Initialize the worker pool. Called once at startup (after TaskWorker_Init).
 * Creates a second worker thread pinned to core 3 (or runs on any available core
 * on non-Switch platforms).
 */
void TaskWorkerPool_Init(void);

/**
 * Submit two tasks to run in parallel on workers 1 and 2.
 * Both tasks must be independent (no shared mutable state).
 * @param task1/task2 Function pointers for the two tasks
 * @param arg1/arg2   Arguments for the tasks (must remain valid until wait)
 */
void TaskWorkerPool_Submit2(void (*task1)(void*), void* arg1,
                            void (*task2)(void*), void* arg2);

/**
 * Block until both pool tasks complete.
 */
void TaskWorkerPool_Wait(void);

/**
 * Shutdown the worker pool.
 */
void TaskWorkerPool_Destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* COLLISION_WORKER_H */

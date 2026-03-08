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

#ifdef __cplusplus
}
#endif

#endif /* COLLISION_WORKER_H */

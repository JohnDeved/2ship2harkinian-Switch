#ifndef COLLISION_WORKER_H
#define COLLISION_WORKER_H

#include "z64.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the collision worker thread. Called once at startup.
 */
void CollisionWorker_Init(void);

/**
 * Shutdown the collision worker thread. Called at exit.
 */
void CollisionWorker_Destroy(void);

/**
 * Submit CollisionCheck_OC to run on the worker thread.
 * Returns immediately; use CollisionWorker_WaitOC() to synchronize.
 */
void CollisionWorker_SubmitOC(struct PlayState* play, CollisionCheckContext* colChkCtx);

/**
 * Wait for the OC collision work to complete.
 */
void CollisionWorker_WaitOC(void);

#ifdef __cplusplus
}
#endif

#endif /* COLLISION_WORKER_H */

/*
 * The filesystem's lock: one holder at a time, for one store call.
 *
 * Before the scheduler starts there is one thread and the lock is free. The
 * flight task never gets it: it reads the one file it needs through
 * hal_fs_read_cached(). A lock not had in 10 s has a holder that is not
 * coming back, and the call fails rather than hang its task.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FS_LOCK_H
#define FS_LOCK_H

#include <stdbool.h>

/* Before the scheduler starts. */
void fs_lock_init(void);

bool fs_lock(void);
void fs_unlock(void);

/* The lock if it is free now; work that can wait for another pass takes it
 * this way, and never waits behind a long call. */
bool fs_try_lock(void);

#endif

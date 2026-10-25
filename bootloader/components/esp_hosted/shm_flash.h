/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

/* The flash mailbox for Linux.  The transport calls these. */
/* Map the window and start the mailbox task, which calls done() per request. */
void shm_flash_init(void (*done)(void));
/* Wake the mailbox task after a doorbell from Linux. */
void shm_flash_kick(void);
/* Take the cache sync lock of OpenSBI from now on.  Call before hart 1 starts. */
void shm_flash_share_cache(void);

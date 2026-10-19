/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

/* The flash mailbox for Linux.  The transport calls these. */
void shm_flash_init(void);
bool shm_flash_service(void);
/* Take the cache sync lock of OpenSBI from now on.  Call before hart 1 starts. */
void shm_flash_share_cache(void);

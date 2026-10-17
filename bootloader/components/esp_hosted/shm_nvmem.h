/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* The nvmem mailbox for the persist partition.  The transport calls these. */
void shm_nvmem_init(void);
void shm_nvmem_service(void);

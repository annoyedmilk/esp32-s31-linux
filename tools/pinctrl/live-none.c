// SPDX-License-Identifier: MIT
/* A build for the host has no hardware: only --regs works. */

#include <errno.h>

#include "board.h"

int live_open(struct regs *r)
{
	(void)r;
	return -ENOSYS;
}

int live_board(struct board *b)
{
	(void)b;
	return -ENOSYS;
}

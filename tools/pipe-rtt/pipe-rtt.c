// SPDX-License-Identifier: MIT
/*
 * The time of a pipe round trip between two processes: the parent writes
 * one byte, the child reads it and writes it back.  For the measurements
 * of a release, not for the image.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WARMUP 1000

static int echo(int in, int out)
{
	char c;

	while (read(in, &c, 1) == 1)
		if (write(out, &c, 1) != 1)
			return 1;
	return 0;
}

static int round_trip(int out, int in)
{
	char c = 'x';

	return write(out, &c, 1) == 1 && read(in, &c, 1) == 1 ? 0 : -1;
}

static double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int run(int out, int in, long rounds, double *us)
{
	double start = 0;
	long i;

	for (i = -WARMUP; i < rounds; i++) {
		if (!i)
			start = now_us();
		if (round_trip(out, in))
			return -1;
	}
	*us = (now_us() - start) / rounds;
	return 0;
}

int main(int argc, char **argv)
{
	long rounds = argc > 1 ? atol(argv[1]) : 10000;
	int to_child[2], to_parent[2];
	int ret, status;
	double us;
	pid_t pid;

	if (rounds < 1) {
		fprintf(stderr, "usage: %s [ROUNDS]\n", argv[0]);
		return 2;
	}
	if (pipe(to_child) || pipe(to_parent)) {
		perror("pipe");
		return 1;
	}
	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 1;
	}
	if (!pid) {
		close(to_child[1]);
		close(to_parent[0]);
		return echo(to_child[0], to_parent[1]);
	}
	close(to_child[0]);
	close(to_parent[1]);
	ret = run(to_child[1], to_parent[0], rounds, &us);
	close(to_child[1]);
	if (waitpid(pid, &status, 0) < 0 || ret) {
		perror("pipe round trip");
		return 1;
	}
	printf("pipe round trip: %.1f us (%ld rounds)\n", us, rounds);
	return 0;
}

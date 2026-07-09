// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <bpf/libbpf.h>

#include "scx_nop.skel.h"

static volatile sig_atomic_t exiting;

static void on_signal(int sig)
{
	exiting = 1;
}

static int libbpf_log(enum libbpf_print_level level, const char *fmt, va_list args)
{
	if (level == LIBBPF_DEBUG)
		return 0;
	return vfprintf(stderr, fmt, args);
}

int main(int argc, char **argv)
{
	struct scx_nop_bpf *skel = NULL;
	struct bpf_link *link = NULL;
	int duration = 3;
	int err;

	if (argc > 1)
		duration = atoi(argv[1]);
	if (duration <= 0 || duration > 15)
		duration = 3;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	libbpf_set_print(libbpf_log);

	skel = scx_nop_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "failed to open/load scx_nop BPF object\n");
		return 1;
	}

	link = bpf_map__attach_struct_ops(skel->maps.scx_nop_ops);
	if (!link) {
		err = -errno;
		fprintf(stderr, "failed to attach sched_ext struct_ops: %d\n", err);
		scx_nop_bpf__destroy(skel);
		return 1;
	}

	printf("scx_nop attached for %d seconds; no switch_all requested\n", duration);
	fflush(stdout);

	for (int i = 0; i < duration && !exiting; i++)
		sleep(1);

	bpf_link__destroy(link);
	scx_nop_bpf__destroy(skel);
	printf("scx_nop detached\n");
	return 0;
}

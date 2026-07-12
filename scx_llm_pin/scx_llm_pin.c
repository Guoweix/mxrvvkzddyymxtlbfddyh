// SPDX-License-Identifier: GPL-2.0
/*
 * scx_llm_pin - User-space loader
 *
 * 用法:
 *   scx_llm_pin --tgid <pid> --llm-cpus 0-3 --system-cpus 4-7 [--duration 60]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <dirent.h>
#include <sched.h>
#include <linux/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "scx_llm_pin.h"
#include "scx_llm_pin.skel.h"

#define NSEC_PER_MSEC 1000000ULL
#ifndef BPF_F_SLEEPABLE
#define BPF_F_SLEEPABLE (1U << 4)
#endif
#ifndef SCHED_EXT
#define SCHED_EXT 7
#endif
#ifndef SCHED_NORMAL
#define SCHED_NORMAL 0
#endif

static volatile sig_atomic_t exiting = 0;

static void on_signal(int sig)
{
	(void)sig;
	exiting = 1;
}

static int libbpf_log(enum libbpf_print_level level, const char *fmt, va_list args)
{
	if (level == LIBBPF_DEBUG)
		return 0;
	return vfprintf(stderr, fmt, args);
}

// 解析 CPU 范围字符串（如 "0-3" 或 "0,2,4"）
static __u32 parse_cpu_mask(const char *str)
{
	__u32 mask = 0;
	char *token, *tmp = strdup(str);
	char *saveptr;

	if (!tmp)
		return 0;

	token = strtok_r(tmp, ",", &saveptr);
	while (token) {
		int start, end;
		if (sscanf(token, "%d-%d", &start, &end) == 2) {
			// 范围格式：0-3
			for (int i = start; i <= end && i < 32; i++)
				mask |= (1U << i);
		} else if (sscanf(token, "%d", &start) == 1) {
			// 单个 CPU：0
			if (start < 32)
				mask |= (1U << start);
		}
		token = strtok_r(NULL, ",", &saveptr);
	}

	free(tmp);
	return mask;
}

static const char *strategy_name(__u32 strategy)
{
	switch (strategy) {
	case LLM_STRATEGY_GLOBAL:
		return "global";
	case LLM_STRATEGY_PIN:
		return "pin";
	case LLM_STRATEGY_LONG_SLICE:
		return "long_slice";
	case LLM_STRATEGY_LOCAL_DSQ:
		return "local_dsq";
	case LLM_STRATEGY_RVV_DIRTY:
		return "rvv_dirty";
	case LLM_STRATEGY_RVV_DIRTY_LOCAL:
		return "rvv_dirty_local";
	default:
		return "unknown";
	}
}

static int parse_strategy(const char *str, __u32 *strategy)
{
	if (!strcmp(str, "global") || !strcmp(str, "scx_global")) {
		*strategy = LLM_STRATEGY_GLOBAL;
		return 0;
	}
	if (!strcmp(str, "pin") || !strcmp(str, "llm_pin")) {
		*strategy = LLM_STRATEGY_PIN;
		return 0;
	}
	if (!strcmp(str, "long_slice")) {
		*strategy = LLM_STRATEGY_LONG_SLICE;
		return 0;
	}
	if (!strcmp(str, "local_dsq")) {
		*strategy = LLM_STRATEGY_LOCAL_DSQ;
		return 0;
	}
	if (!strcmp(str, "rvv_dirty") || !strcmp(str, "dirty")) {
		*strategy = LLM_STRATEGY_RVV_DIRTY;
		return 0;
	}
	if (!strcmp(str, "rvv_dirty_local") || !strcmp(str, "dirty_local")) {
		*strategy = LLM_STRATEGY_RVV_DIRTY_LOCAL;
		return 0;
	}

	return -EINVAL;
}

static int set_tgid_threads_policy(__u32 tgid, int policy)
{
	char path[64];
	struct sched_param param = { .sched_priority = 0 };
	struct dirent *de;
	DIR *dir;
	int count = 0;
	int failed = 0;

	snprintf(path, sizeof(path), "/proc/%u/task", tgid);
	dir = opendir(path);
	if (!dir) {
		fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
		return -errno;
	}

	while ((de = readdir(dir))) {
		char *end = NULL;
		long tid;

		errno = 0;
		tid = strtol(de->d_name, &end, 10);
		if (errno || !end || *end != '\0' || tid <= 0)
			continue;

		if (sched_setscheduler((pid_t)tid, policy, &param)) {
			fprintf(stderr, "failed to set tid %ld policy %d: %s\n",
				tid, policy, strerror(errno));
			failed++;
			continue;
		}
		count++;
	}

	closedir(dir);

	if (!count && failed)
		return -EPERM;

	return count;
}

static int set_tid_list_policy(const char *tid_list, int policy)
{
	struct sched_param param = { .sched_priority = 0 };
	char *tmp;
	char *saveptr = NULL;
	char *token;
	int count = 0;
	int failed = 0;

	if (!tid_list || !*tid_list)
		return -EINVAL;

	tmp = strdup(tid_list);
	if (!tmp)
		return -ENOMEM;

	for (token = strtok_r(tmp, ",", &saveptr); token;
	     token = strtok_r(NULL, ",", &saveptr)) {
		char *end = NULL;
		long tid;

		errno = 0;
		tid = strtol(token, &end, 10);
		if (errno || !end || *end != '\0' || tid <= 0) {
			fprintf(stderr, "invalid tid in --tid-list: %s\n", token);
			failed++;
			continue;
		}

		if (sched_setscheduler((pid_t)tid, policy, &param)) {
			fprintf(stderr, "failed to set tid %ld policy %d: %s\n",
				tid, policy, strerror(errno));
			failed++;
			continue;
		}
		count++;
	}

	free(tmp);

	if (!count && failed)
		return -EPERM;
	return count;
}


static void print_stats(struct scx_llm_pin_bpf *skel)
{
	struct llm_pin_stats stats = {0};
	__u32 key = 0;
	int err;

	err = bpf_map__lookup_elem(skel->maps.stats_map, &key, sizeof(key),
				    &stats, sizeof(stats), 0);
	if (err) {
		fprintf(stderr, "failed to read stats: %d\n", err);
		return;
	}

	printf("\n=== scx_llm_pin statistics ===\n");
	printf("LLM tasks scheduled:    %llu\n", stats.llm_scheduled);
	printf("LLM tasks pinned:       %llu\n", stats.llm_pinned);
	printf("System tasks scheduled: %llu\n", stats.system_scheduled);
	printf("CPU migrations:         %llu\n", stats.cpu_migrations);
	printf("LLM global dispatches:  %llu\n", stats.llm_global_dispatches);
	printf("LLM local dispatches:   %llu\n", stats.llm_local_dispatches);
	printf("LLM long slices:        %llu\n", stats.llm_long_slices);
	printf("System dispatches:      %llu\n", stats.system_dispatches);
	printf("RVV dirty samples:      %llu\n", stats.rvv_dirty_samples);
	printf("RVV vector threads:     %llu\n", stats.rvv_vector_threads);
	printf("RVV vector dispatches:  %llu\n", stats.rvv_vector_dispatches);
	printf("RVV aggressive dispatches: %llu\n", stats.rvv_aggressive_dispatches);
	printf("RVV fallback dispatches:   %llu\n", stats.rvv_fallback_dispatches);
	printf("RVV nonvector dispatches:  %llu\n", stats.rvv_nonvector_llm_dispatches);
	printf("Protected kernel dispatches: %llu\n", stats.protected_kernel_dispatches);
	printf("Protected kernel preempts:   %llu\n", stats.protected_kernel_preempts);
	printf("RVV vector DSQ dispatches:   %llu\n", stats.rvv_vector_dsq_dispatches);
	printf("RVV vector DSQ consumes:     %llu\n", stats.rvv_vector_dsq_consumes);
	printf("RVV vector local dispatches: %llu\n", stats.rvv_vector_local_dispatches);
	printf("RVV vector window starts:    %llu\n", stats.rvv_vector_window_starts);
	printf("RVV vector slice clamps:     %llu\n", stats.rvv_vector_slice_clamps);
	printf("RVV vector identify barrier dispatches: %llu\n",
	       stats.rvv_vector_identify_barrier_dispatches);
	fflush(stdout);
}

static void print_vector_cpu_assignments(struct scx_llm_pin_bpf *skel)
{
	struct rvv_task_class task_class;
	__s32 selected_cpu;
	__u32 key;
	__u32 next_key;
	int class_fd = bpf_map__fd(skel->maps.rvv_task_classes);
	int cpu_fd = bpf_map__fd(skel->maps.selected_cpu_map);
	int err;

	printf("\n=== RVV vector CPU assignments ===\n");
	err = bpf_map_get_next_key(class_fd, NULL, &next_key);
	while (!err) {
		key = next_key;
		if (!bpf_map_lookup_elem(class_fd, &key, &task_class) &&
		    task_class.is_vector) {
			selected_cpu = -1;
			bpf_map_lookup_elem(cpu_fd, &key, &selected_cpu);
			printf("tid=%u dirty_score=%u selected_cpu=%d\n",
			       key, task_class.dirty_score, selected_cpu);
		}
		err = bpf_map_get_next_key(class_fd, &key, &next_key);
	}
	fflush(stdout);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  --tgid <pid>           Target process TGID (llama-server PID)\n"
		"  --strategy <name>      global|pin|long_slice|local_dsq|rvv_dirty|rvv_dirty_local (default: pin)\n"
		"  --llm-cpus <range>     CPU mask for LLM threads (e.g., 0-3 or 0,1,2,3)\n"
		"  --system-cpus <range>  CPU mask for system threads (e.g., 4-7)\n"
		"  --vector-cpus <range>  Aggressive CPUs for RVV dirty vector threads\n"
		"  --ordinary-cpus <range> Ordinary/fallback CPUs for RVV dirty vector threads\n"
		"  --slice-ms <ms>        LLM time slice in milliseconds (0 = kernel default)\n"
		"  --system-slice-ms <ms> System task time slice in milliseconds (0 = kernel default)\n"
		"  --vector-slice-ms <ms> RVV vector slice on aggressive CPUs (0 = LLM/default)\n"
		"  --ordinary-vector-slice-ms <ms> RVV vector slice on ordinary CPUs (0 = LLM/default)\n"
		"  --vector-local-window-ms <ms> RVV local fast-path window after vector DSQ consume (default: 500)\n"
		"  --dirty-threshold <n>  Dirty samples before marking vector thread (default: 2)\n"
		"  --fallback-every <n>   Every Nth vector CPU pick uses ordinary CPUs (0 = disabled)\n"
		"  --llm-only            Do not switch all tasks; move target threads to SCHED_EXT only\n"
		"  --tid-list <list>     With --llm-only, move only comma-separated TIDs to SCHED_EXT\n"
		"  --duration <sec>       Run duration in seconds (0 = infinite, default: 0)\n"
		"  -h, --help             Show this help\n"
		"\n"
		"Example:\n"
		"  %s --tgid 1234 --strategy long_slice --llm-cpus 0-3 --system-cpus 4-7 --slice-ms 100\n"
		"\n",
		prog, prog);
}

int main(int argc, char **argv)
{
	struct scx_llm_pin_bpf *skel = NULL;
	struct bpf_link *link = NULL;
	struct llm_pin_config config = {0};
	const char *tid_list = NULL;
	int duration = 0;
	int moved_threads = 0;
	int err;
	__u32 key = 0;

	config.strategy = LLM_STRATEGY_PIN;
	config.llm_cpu_mask = 0xFF;
	config.system_cpu_mask = 0xFF;
	config.vector_cpu_mask = 0xFF;
	config.ordinary_cpu_mask = 0xFF;
	config.dirty_threshold = 2;
	config.vector_local_window_ns = 500 * NSEC_PER_MSEC;
	config.switch_all = 1;

	static struct option long_options[] = {
		{"tgid",        required_argument, 0, 't'},
		{"strategy",    required_argument, 0, 'p'},
		{"llm-cpus",    required_argument, 0, 'l'},
		{"system-cpus", required_argument, 0, 's'},
		{"vector-cpus", required_argument, 0, 'v'},
		{"ordinary-cpus", required_argument, 0, 'O'},
		{"slice-ms",    required_argument, 0, 'm'},
		{"system-slice-ms", required_argument, 0, 'S'},
		{"vector-slice-ms", required_argument, 0, 'M'},
		{"ordinary-vector-slice-ms", required_argument, 0, 'N'},
		{"vector-local-window-ms", required_argument, 0, 'W'},
		{"dirty-threshold", required_argument, 0, 'D'},
		{"fallback-every", required_argument, 0, 'F'},
		{"llm-only",    no_argument,       0, 'o'},
		{"tid-list",    required_argument, 0, 'T'},
		{"duration",    required_argument, 0, 'd'},
		{"help",        no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	// 解析命令行参数
	while (1) {
		int c = getopt_long(argc, argv, "h", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 't':
			config.target_tgid = atoi(optarg);
			break;
		case 'p':
			if (parse_strategy(optarg, &config.strategy)) {
				fprintf(stderr, "Error: unknown strategy '%s'\n", optarg);
				usage(argv[0]);
				return 1;
			}
			break;
		case 'l':
			config.llm_cpu_mask = parse_cpu_mask(optarg);
			break;
		case 's':
			config.system_cpu_mask = parse_cpu_mask(optarg);
			break;
		case 'v':
			config.vector_cpu_mask = parse_cpu_mask(optarg);
			break;
		case 'O':
			config.ordinary_cpu_mask = parse_cpu_mask(optarg);
			break;
		case 'm':
			config.llm_slice_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case 'S':
			config.system_slice_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case 'M':
			config.vector_slice_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case 'N':
			config.ordinary_vector_slice_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case 'W':
			config.vector_local_window_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case 'D':
			config.dirty_threshold = strtoul(optarg, NULL, 10);
			break;
		case 'F':
			config.fallback_every = strtoul(optarg, NULL, 10);
			break;
		case 'o':
			config.switch_all = 0;
			break;
		case 'T':
			tid_list = optarg;
			break;
		case 'd':
			duration = atoi(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}

	// 参数验证
	if (!config.target_tgid) {
		fprintf(stderr, "Error: --tgid is required\n\n");
		usage(argv[0]);
		return 1;
	}
	if (!config.llm_cpu_mask)
		config.llm_cpu_mask = 0xFF;

	if (!config.system_cpu_mask) {
		config.system_cpu_mask = 0xFF & ~config.llm_cpu_mask;  // 8核默认
		if (!config.system_cpu_mask)
			config.system_cpu_mask = 0xFF;
	}
	if (!config.vector_cpu_mask)
		config.vector_cpu_mask = config.llm_cpu_mask;
	if (!config.ordinary_cpu_mask)
		config.ordinary_cpu_mask = config.system_cpu_mask;
	if (!config.dirty_threshold)
		config.dirty_threshold = 2;

	if (config.strategy == LLM_STRATEGY_LONG_SLICE && !config.llm_slice_ns)
		config.llm_slice_ns = 100 * NSEC_PER_MSEC;
	if (config.strategy == LLM_STRATEGY_RVV_DIRTY ||
	    config.strategy == LLM_STRATEGY_RVV_DIRTY_LOCAL) {
		if (!config.vector_slice_ns)
			config.vector_slice_ns = 100 * NSEC_PER_MSEC;
		if (!config.ordinary_vector_slice_ns)
			config.ordinary_vector_slice_ns = config.llm_slice_ns;
	}

	config.enabled = 1;

	// 打印配置
	printf("scx_llm_pin configuration:\n");
	printf("  Target TGID:     %u\n", config.target_tgid);
	printf("  Strategy:        %s\n", strategy_name(config.strategy));
	printf("  LLM CPU mask:    0x%02x\n", config.llm_cpu_mask);
	printf("  System CPU mask: 0x%02x\n", config.system_cpu_mask);
	printf("  Vector CPU mask: 0x%02x\n", config.vector_cpu_mask);
	printf("  Ordinary CPU mask: 0x%02x\n", config.ordinary_cpu_mask);
	printf("  LLM slice:       %llums\n",
	       (unsigned long long)(config.llm_slice_ns / NSEC_PER_MSEC));
	printf("  System slice:    %llums\n",
	       (unsigned long long)(config.system_slice_ns / NSEC_PER_MSEC));
	printf("  Vector slice:    %llums\n",
	       (unsigned long long)(config.vector_slice_ns / NSEC_PER_MSEC));
	printf("  Ordinary vector slice: %llums\n",
	       (unsigned long long)(config.ordinary_vector_slice_ns / NSEC_PER_MSEC));
	printf("  Vector local window: %llums\n",
	       (unsigned long long)(config.vector_local_window_ns / NSEC_PER_MSEC));
	printf("  Dirty threshold: %u\n", config.dirty_threshold);
	printf("  Fallback every:  %u\n", config.fallback_every);
	printf("  Switch all:      %s\n", config.switch_all ? "yes" : "no");
	if (tid_list)
		printf("  TID list:        %s\n", tid_list);
	printf("  Duration:        %s\n", duration ? "limited" : "infinite");
	printf("\n");

	// 信号处理
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	libbpf_set_print(libbpf_log);

	// 加载 BPF 程序。init 里创建自定义 DSQ，需要 sleepable flag。
	skel = scx_llm_pin_bpf__open();
	if (!skel) {
		fprintf(stderr, "failed to open scx_llm_pin BPF object\n");
		return 1;
	}
	err = bpf_program__set_flags(skel->progs.scx_llm_pin_init, BPF_F_SLEEPABLE);
	if (err) {
		fprintf(stderr, "failed to mark init program sleepable: %d\n", err);
		goto cleanup;
	}
	err = scx_llm_pin_bpf__load(skel);
	if (err) {
		fprintf(stderr, "failed to load scx_llm_pin BPF object: %d\n", err);
		goto cleanup;
	}

	// 写入配置
	err = bpf_map__update_elem(skel->maps.config_map, &key, sizeof(key),
				    &config, sizeof(config), BPF_ANY);
	if (err) {
		fprintf(stderr, "failed to write config: %d\n", err);
		goto cleanup;
	}

	// 附加 struct_ops
	link = bpf_map__attach_struct_ops(skel->maps.scx_llm_pin_ops);
	if (!link) {
		err = -errno;
		fprintf(stderr, "failed to attach sched_ext struct_ops: %d\n", err);
		goto cleanup;
	}

	if (!config.switch_all) {
		if (tid_list)
			err = set_tid_list_policy(tid_list, SCHED_EXT);
		else
			err = set_tgid_threads_policy(config.target_tgid, SCHED_EXT);
		if (err < 0) {
			fprintf(stderr, "failed to move target threads to SCHED_EXT: %d\n", err);
			goto cleanup;
		}
		moved_threads = err;
		printf("Moved %d target threads to SCHED_EXT\n", moved_threads);
	}

	printf("scx_llm_pin attached successfully\n");
	printf("Press Ctrl-C to detach\n\n");
	fflush(stdout);

	// 运行主循环
	if (duration > 0) {
		for (int i = 0; i < duration && !exiting; i++) {
			sleep(1);
			if (i > 0 && i % 10 == 0)
				print_stats(skel);
		}
	} else {
		while (!exiting) {
			sleep(5);
			print_stats(skel);
		}
	}

	// 打印最终统计
	print_stats(skel);
	print_vector_cpu_assignments(skel);

cleanup:
	if (!config.switch_all && moved_threads > 0) {
		if (tid_list)
			set_tid_list_policy(tid_list, SCHED_NORMAL);
		else
			set_tgid_threads_policy(config.target_tgid, SCHED_NORMAL);
	}
	bpf_link__destroy(link);
	scx_llm_pin_bpf__destroy(skel);
	printf("\nscx_llm_pin detached\n");
	return err < 0 ? -err : 0;
}

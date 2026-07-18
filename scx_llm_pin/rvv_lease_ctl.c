// SPDX-License-Identifier: GPL-2.0 OR MIT
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "scx_llm_pin.h"

#define NSEC_PER_MSEC 1000000ULL

static int bpf_call(enum bpf_cmd cmd, union bpf_attr *attr)
{
	return (int)syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

static int bpf_obj_get_raw(const char *path)
{
	union bpf_attr attr = {};

	attr.pathname = (__u64)(uintptr_t)path;
	return bpf_call(BPF_OBJ_GET, &attr);
}

static int map_lookup(int fd, const void *key, void *value)
{
	union bpf_attr attr = {};

	attr.map_fd = fd;
	attr.key = (__u64)(uintptr_t)key;
	attr.value = (__u64)(uintptr_t)value;
	return bpf_call(BPF_MAP_LOOKUP_ELEM, &attr);
}

static int map_update(int fd, const void *key, const void *value)
{
	union bpf_attr attr = {};

	attr.map_fd = fd;
	attr.key = (__u64)(uintptr_t)key;
	attr.value = (__u64)(uintptr_t)value;
	attr.flags = BPF_ANY;
	return bpf_call(BPF_MAP_UPDATE_ELEM, &attr);
}

static const char *mode_name(__u32 mode)
{
	switch (mode) {
	case RVV_LEASE_CONTROL_OFF:
		return "off";
	case RVV_LEASE_CONTROL_OBSERVE:
		return "observe";
	case RVV_LEASE_CONTROL_ACTIVE:
		return "active";
	default:
		return "unknown";
	}
}

static int parse_mode(const char *value, __u32 *mode)
{
	if (!strcmp(value, "observe"))
		*mode = RVV_LEASE_CONTROL_OBSERVE;
	else if (!strcmp(value, "active"))
		*mode = RVV_LEASE_CONTROL_ACTIVE;
	else
		return -EINVAL;
	return 0;
}

static int parse_u32_range(const char *value, __u32 min, __u32 max,
			   __u32 *result)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno || !end || *end != '\0' || parsed < min || parsed > max)
		return -EINVAL;
	*result = (__u32)parsed;
	return 0;
}

static int parse_preferred_team(const char *value, __u32 *team_plus1)
{
	if (!strcmp(value, "off"))
		*team_plus1 = 0;
	else if (!strcmp(value, "0"))
		*team_plus1 = 1;
	else if (!strcmp(value, "1"))
		*team_plus1 = 2;
	else
		return -EINVAL;
	return 0;
}

static __u64 monotonic_ns(void)
{
	struct timespec ts = {};

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;
	return (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
}

static int parse_cpu_map(const char *value,
			 __u32 cpus[RVV_LEASE_CONTROL_WORKERS],
			 __u32 *enabled)
{
	bool seen[RVV_LEASE_CONTROL_WORKERS] = {};
	char *saveptr = NULL;
	char *token;
	char *copy;
	__u32 count = 0;

	if (!strcmp(value, "auto")) {
		*enabled = 0;
		return 0;
	}
	copy = strdup(value);
	if (!copy)
		return -ENOMEM;
	for (token = strtok_r(copy, ",", &saveptr); token;
	     token = strtok_r(NULL, ",", &saveptr)) {
		__u32 cpu;

		if (count >= RVV_LEASE_CONTROL_WORKERS ||
		    parse_u32_range(token, 0, RVV_LEASE_CONTROL_WORKERS - 1, &cpu) ||
		    seen[cpu]) {
			free(copy);
			return -EINVAL;
		}
		seen[cpu] = true;
		cpus[count++] = cpu;
	}
	free(copy);
	if (count != RVV_LEASE_CONTROL_WORKERS)
		return -EINVAL;
	*enabled = 1;
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s status [--control-dir PATH]\n"
		"  %s set [--control-dir PATH] [--mode observe|active]\n"
		"       [--slice-ms 1..2000] [--window-ms 1..5000]\n"
		"       [--dirty-threshold 1..64] [--cpu-map auto|0,1,2,3,4,5,6,7]\n"
		"       [--prefer-team off|0|1 [--prefer-ms 1..300000]]\n"
		"  %s reset [--control-dir PATH]\n",
		prog, prog, prog);
}

static void print_status(const struct llm_pin_config *config,
			 const struct rvv_lease_worker_page pages[RVV_LEASE_CONTROL_TEAMS])
{
	__u64 now = monotonic_ns();

	printf("mode=%s\n", mode_name(config->user_control_mode));
	printf("target_tgid=%u\n", config->target_tgid);
	printf("target_tgid2=%u\n", config->target_tgid2);
	printf("session=%llu\n", (unsigned long long)config->user_session_id);
	printf("policy_epoch=%llu\n", (unsigned long long)config->user_policy_epoch);
	printf("vector_slice_ms=%llu\n",
	       (unsigned long long)(config->vector_slice_ns / NSEC_PER_MSEC));
	printf("vector_local_window_ms=%llu\n",
	       (unsigned long long)(config->vector_local_window_ns / NSEC_PER_MSEC));
	printf("dirty_threshold=%u\n", config->dirty_threshold);
	if (config->user_preferred_team_plus1 &&
	    config->user_preferred_until_ns > now) {
		__u64 remaining = config->user_preferred_until_ns - now;

		printf("preferred_team=%u\n", config->user_preferred_team_plus1 - 1);
		printf("preference_remaining_ms=%llu\n",
		       (unsigned long long)(remaining / NSEC_PER_MSEC));
	} else {
		printf("preferred_team=off\n");
		printf("preference_remaining_ms=0\n");
	}
	if (config->user_cpu_map_enabled)
		printf("cpu_map=%u,%u,%u,%u,%u,%u,%u,%u\n",
		       config->user_cpu_map[0], config->user_cpu_map[1],
		       config->user_cpu_map[2], config->user_cpu_map[3],
		       config->user_cpu_map[4], config->user_cpu_map[5],
		       config->user_cpu_map[6], config->user_cpu_map[7]);
	else
		printf("cpu_map=auto\n");
	if (!pages)
		return;
	for (__u32 team = 0; team < RVV_LEASE_CONTROL_TEAMS; team++) {
		const struct rvv_lease_worker_page *page = &pages[team];

		printf("worker_page[%u]_active=%u\n", team, page->header.active);
		if (!page->header.active)
			continue;
		for (__u32 ith = 0; ith < RVV_LEASE_CONTROL_WORKERS; ith++) {
			const struct rvv_lease_worker_slot *slot = &page->workers[ith];

			if (!slot->sequence || !slot->tid || (slot->sequence & 1U))
				continue;
			printf("worker[%u][%u]=tid:%u,tgid:%u,n_threads:%u,session:%llu\n",
			       team, ith, slot->tid, slot->tgid, slot->n_threads,
			       (unsigned long long)slot->session_id);
		}
	}
}

int main(int argc, char **argv)
{
	const char *control_dir = RVV_LEASE_CONTROL_DEFAULT_DIR;
	struct rvv_lease_worker_page pages[RVV_LEASE_CONTROL_TEAMS] = {};
	struct llm_pin_config config = {};
	struct llm_pin_config requested = {};
	char config_path[512];
	char worker_path[512];
	const char *command;
	bool changed = false;
	bool set_mode = false;
	bool set_slice = false;
	bool set_window = false;
	bool set_threshold = false;
	bool set_cpu_map = false;
	bool set_prefer_team = false;
	bool set_prefer_ms = false;
	__u32 prefer_ms = 0;
	__u32 key = 0;
	int config_fd;
	int worker_fd;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}
	command = argv[1];
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--control-dir") && i + 1 < argc) {
			control_dir = argv[++i];
		} else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
			if (parse_mode(argv[++i], &requested.user_control_mode)) {
				fprintf(stderr, "invalid mode\n");
				return 1;
			}
			set_mode = true;
			changed = true;
		} else if (!strcmp(argv[i], "--slice-ms") && i + 1 < argc) {
			__u32 value;
			if (parse_u32_range(argv[++i], 1, 2000, &value)) {
				fprintf(stderr, "invalid slice-ms\n");
				return 1;
			}
			requested.vector_slice_ns = (__u64)value * NSEC_PER_MSEC;
			set_slice = true;
			changed = true;
		} else if (!strcmp(argv[i], "--window-ms") && i + 1 < argc) {
			__u32 value;
			if (parse_u32_range(argv[++i], 1, 5000, &value)) {
				fprintf(stderr, "invalid window-ms\n");
				return 1;
			}
			requested.vector_local_window_ns = (__u64)value * NSEC_PER_MSEC;
			set_window = true;
			changed = true;
		} else if (!strcmp(argv[i], "--dirty-threshold") && i + 1 < argc) {
			if (parse_u32_range(argv[++i], 1, 64, &requested.dirty_threshold)) {
				fprintf(stderr, "invalid dirty-threshold\n");
				return 1;
			}
			set_threshold = true;
			changed = true;
		} else if (!strcmp(argv[i], "--cpu-map") && i + 1 < argc) {
			if (parse_cpu_map(argv[++i], requested.user_cpu_map,
					  &requested.user_cpu_map_enabled)) {
				fprintf(stderr, "cpu-map must be auto or contain eight unique CPUs 0..7\n");
				return 1;
			}
			set_cpu_map = true;
			changed = true;
		} else if (!strcmp(argv[i], "--prefer-team") && i + 1 < argc) {
			if (parse_preferred_team(argv[++i],
						 &requested.user_preferred_team_plus1)) {
				fprintf(stderr, "prefer-team must be off, 0, or 1\n");
				return 1;
			}
			set_prefer_team = true;
			changed = true;
		} else if (!strcmp(argv[i], "--prefer-ms") && i + 1 < argc) {
			if (parse_u32_range(argv[++i], 1, 300000, &prefer_ms)) {
				fprintf(stderr, "prefer-ms must be in 1..300000\n");
				return 1;
			}
			set_prefer_ms = true;
			changed = true;
		} else {
			usage(argv[0]);
			return 1;
		}
	}
	if (set_prefer_ms && !set_prefer_team) {
		fprintf(stderr, "--prefer-ms requires --prefer-team\n");
		return 1;
	}
	if (set_prefer_team && requested.user_preferred_team_plus1 &&
	    !set_prefer_ms) {
		fprintf(stderr, "--prefer-team 0|1 requires --prefer-ms\n");
		return 1;
	}

	if (snprintf(config_path, sizeof(config_path), "%s/%s", control_dir,
		     RVV_LEASE_CONFIG_MAP_NAME) >= (int)sizeof(config_path) ||
	    snprintf(worker_path, sizeof(worker_path), "%s/%s", control_dir,
		     RVV_LEASE_WORKER_MAP_NAME) >= (int)sizeof(worker_path)) {
		fprintf(stderr, "control path too long\n");
		return 1;
	}
	config_fd = bpf_obj_get_raw(config_path);
	worker_fd = bpf_obj_get_raw(worker_path);
	if (config_fd < 0 || worker_fd < 0) {
		fprintf(stderr, "failed to open control maps under %s: %s\n",
			control_dir, strerror(errno));
		return 1;
	}
	if (map_lookup(config_fd, &key, &config)) {
		fprintf(stderr, "failed to read control maps: %s\n", strerror(errno));
		return 1;
	}
	for (key = 0; key < RVV_LEASE_CONTROL_TEAMS; key++) {
		__u32 target_tgid = key == 0 ? config.target_tgid : config.target_tgid2;

		if (map_lookup(worker_fd, &key, &pages[key])) {
			fprintf(stderr, "failed to read worker map team %u: %s\n",
				key, strerror(errno));
			return 1;
		}
		if (!target_tgid)
			continue;
		if (pages[key].header.magic != RVV_LEASE_CONTROL_MAGIC ||
		    pages[key].header.abi_version != RVV_LEASE_CONTROL_ABI_VERSION ||
		    pages[key].header.team_id != key ||
		    pages[key].header.target_tgid != target_tgid ||
		    pages[key].header.session_id != config.user_session_id) {
			fprintf(stderr, "control ABI/session mismatch for team %u\n", key);
			return 1;
		}
	}
	key = 0;

	if (!strcmp(command, "status")) {
		if (changed) {
			fprintf(stderr, "status does not accept policy updates\n");
			return 1;
		}
		print_status(&config, pages);
	} else if (!strcmp(command, "reset")) {
		config.user_control_mode = RVV_LEASE_CONTROL_ACTIVE;
		config.vector_slice_ns = 100 * NSEC_PER_MSEC;
		config.vector_local_window_ns = 500 * NSEC_PER_MSEC;
		config.dirty_threshold = 2;
		config.user_cpu_map_enabled = 0;
		config.user_preferred_team_plus1 = 0;
		config.user_preferred_until_ns = 0;
		for (__u32 ith = 0; ith < RVV_LEASE_CONTROL_WORKERS; ith++)
			config.user_cpu_map[ith] = ith;
		config.user_policy_epoch++;
		if (map_update(config_fd, &key, &config)) {
			fprintf(stderr, "failed to reset config: %s\n", strerror(errno));
			return 1;
		}
		print_status(&config, pages);
	} else if (!strcmp(command, "set")) {
		if (!changed) {
			fprintf(stderr, "set requires at least one update\n");
			return 1;
		}
		if (set_mode)
			config.user_control_mode = requested.user_control_mode;
		if (set_slice)
			config.vector_slice_ns = requested.vector_slice_ns;
		if (set_window)
			config.vector_local_window_ns = requested.vector_local_window_ns;
		if (set_threshold)
			config.dirty_threshold = requested.dirty_threshold;
		if (set_cpu_map)
			memcpy(config.user_cpu_map, requested.user_cpu_map,
			       sizeof(config.user_cpu_map));
		if (set_cpu_map)
			config.user_cpu_map_enabled = requested.user_cpu_map_enabled;
		if (set_prefer_team) {
			config.user_preferred_team_plus1 =
				requested.user_preferred_team_plus1;
			config.user_preferred_until_ns =
				requested.user_preferred_team_plus1 ?
				monotonic_ns() + (__u64)prefer_ms * NSEC_PER_MSEC : 0;
		}
		config.user_policy_epoch++;
		if (map_update(config_fd, &key, &config)) {
			fprintf(stderr, "failed to update config: %s\n", strerror(errno));
			return 1;
		}
		print_status(&config, pages);
	} else {
		usage(argv[0]);
		return 1;
	}

	close(worker_fd);
	close(config_fd);
	return 0;
}

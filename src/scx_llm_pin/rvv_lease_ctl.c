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

static const char *phase_name(__u32 phase)
{
	switch (phase) {
	case RVV_LEASE_PHASE_IDLE:
		return "idle";
	case RVV_LEASE_PHASE_PREFILL:
		return "prefill";
	case RVV_LEASE_PHASE_DECODE:
		return "decode";
	case RVV_LEASE_PHASE_MIXED:
		return "mixed";
	default:
		return "invalid";
	}
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

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s status [--control-dir PATH]\n"
		"  %s set [--control-dir PATH] [--slice-ms 1..2000] [--window-ms 1..5000]\n"
		"       [--dirty-threshold 1..64]\n"
		"  %s reset [--control-dir PATH]\n",
		prog, prog, prog);
}

static void print_status(const struct llm_pin_config *config,
			 const struct rvv_lease_worker_page pages[RVV_LEASE_CONTROL_TEAMS])
{
	printf("team_count=%u\n", config->team_count);
    for (unsigned t = 0; t < config->team_count && t < RVV_LEASE_CONTROL_TEAMS; t++) {
        printf("team[%u]_tgid=%u threads=%u threads_batch=%u cpu_mask=0x%x\n",
            t, config->teams[t].tgid, config->teams[t].threads,
            config->teams[t].threads_batch, config->teams[t].cpu_mask);
    }
	printf("target_tgid=%u\n", config->target_tgid);
	printf("target_tgid2=%u\n", config->target_tgid2);
	printf("session=%llu\n", (unsigned long long)config->user_session_id);
	printf("policy_epoch=%llu\n", (unsigned long long)config->user_policy_epoch);
	printf("vector_slice_ms=%llu\n",
	       (unsigned long long)(config->vector_slice_ns / NSEC_PER_MSEC));
	printf("vector_local_window_ms=%llu\n",
	       (unsigned long long)(config->vector_local_window_ns / NSEC_PER_MSEC));
	printf("dirty_threshold=%u\n", config->dirty_threshold);
	printf("phase_hint_timeout_ms=%llu\n",
	       (unsigned long long)(config->phase_hint_timeout_ns / NSEC_PER_MSEC));
	printf("prefill_starvation_ms=%llu\n",
	       (unsigned long long)(config->prefill_starvation_ns / NSEC_PER_MSEC));
	printf("phase_wakeup_preempt=%u\n", config->phase_wakeup_preempt_enabled);
	if (!pages)
		return;
	for (__u32 team = 0; team < RVV_LEASE_CONTROL_TEAMS; team++) {
		const struct rvv_lease_worker_page *page = &pages[team];

		printf("worker_page[%u]_active=%u\n", team, page->header.active);
		if (!page->header.active)
			continue;
        printf("membership[%u]=epoch:%llu,registered_mask:0x%x,limit:%u\n", team,
            (unsigned long long)page->team_hint.membership_epoch,
            page->header.registered_mask, page->header.worker_limit);
		printf("team_phase[%u]=phase:%s,workers:%u,prompt:%u,decode:%u,epoch:%llu,begin_ns:%llu,expires_ns:%llu,session:%llu,sequence:%u\n",
		       team, phase_name(page->team_hint.phase),
		       page->team_hint.active_workers, page->team_hint.prompt_tokens,
		       page->team_hint.decode_tokens,
		       (unsigned long long)page->team_hint.phase_epoch,
		       (unsigned long long)page->team_hint.begin_ns,
		       (unsigned long long)page->team_hint.expires_ns,
		       (unsigned long long)page->team_hint.session_id,
		       page->team_hint.sequence);
		for (__u32 ith = 0; ith < RVV_LEASE_CONTROL_WORKERS; ith++) {
			const struct rvv_lease_worker_slot *slot = &page->workers[ith];

			if (!slot->sequence || !slot->tid || (slot->sequence & 1U))
				continue;
			printf("worker[%u][%u]=tid:%u,tgid:%u,n_threads:%u,session:%llu,membership:%llu,current:%u\n",
			       team, ith, slot->tid, slot->tgid, slot->n_threads,
			       (unsigned long long)slot->session_id,
                   (unsigned long long)slot->membership_epoch,
                   slot->membership_epoch == page->team_hint.membership_epoch);
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
	bool set_slice = false;
	bool set_window = false;
	bool set_threshold = false;
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
		} else {
			usage(argv[0]);
			return 1;
		}
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
		__u32 target_tgid = key < config.team_count ? config.teams[key].tgid : 0;

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
		config.vector_slice_ns = 100 * NSEC_PER_MSEC;
		config.vector_local_window_ns = 500 * NSEC_PER_MSEC;
		config.dirty_threshold = 2;
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
		if (set_slice)
			config.vector_slice_ns = requested.vector_slice_ns;
		if (set_window)
			config.vector_local_window_ns = requested.vector_local_window_ns;
		if (set_threshold)
			config.dirty_threshold = requested.dirty_threshold;
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

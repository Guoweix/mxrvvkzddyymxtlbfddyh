// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "rvv_sched_cost.h"
#include "rvv_sched_cost.skel.h"

static volatile sig_atomic_t exiting;

struct env {
	unsigned int tgid;
	unsigned int tid;
	unsigned int duration;
	bool sample_vs;
	bool verbose_libbpf;
	const char *comm;
	const char *json_path;
};

static void on_signal(int sig)
{
	(void)sig;
	exiting = 1;
}

static int libbpf_log(enum libbpf_print_level level, const char *fmt,
		      va_list args)
{
	if (level == LIBBPF_DEBUG)
		return 0;
	return vfprintf(stderr, fmt, args);
}

static int libbpf_log_verbose(enum libbpf_print_level level, const char *fmt,
			      va_list args)
{
	(void)level;
	return vfprintf(stderr, fmt, args);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--tgid PID] [--tid TID] [--comm NAME] [--duration SEC] [--json PATH] [--no-vs]\n"
		"\n"
		"Defaults: --comm llama-server --duration 10\n"
		"Examples:\n"
		"  %s --tgid 531 --duration 60 --json result.json\n"
		"  %s --comm llama-server --duration 30\n",
		prog, prog, prog);
}

static int parse_uint_arg(const char *s, unsigned int *out)
{
	char *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || !end || *end || v > UINT_MAX)
		return -1;

	*out = (unsigned int)v;
	return 0;
}

static int read_first_line(const char *path, char *buf, size_t size)
{
	FILE *f;
	size_t len;

	f = fopen(path, "r");
	if (!f)
		return -errno;

	if (!fgets(buf, size, f)) {
		fclose(f);
		return -EIO;
	}
	fclose(f);

	len = strlen(buf);
	while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';
	return 0;
}

static int read_cmdline(const char *path, char *buf, size_t size)
{
	ssize_t n;
	int fd;

	if (!size)
		return -EINVAL;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	n = read(fd, buf, size - 1);
	close(fd);
	if (n <= 0)
		return -EIO;

	buf[n] = '\0';
	for (ssize_t i = 0; i < n; i++) {
		if (buf[i] == '\0')
			buf[i] = ' ';
	}
	return 0;
}

static bool all_digits(const char *s)
{
	if (!s || !*s)
		return false;

	for (; *s; s++) {
		if (!isdigit((unsigned char)*s))
			return false;
	}
	return true;
}

static int find_tgid_by_comm(const char *comm, unsigned int *tgid)
{
	char path[PATH_MAX];
	char proc_comm[RVV_COMM_LEN];
	char cmdline[PATH_MAX];
	unsigned int found = 0;
	unsigned int first = 0;
	pid_t self = getpid();
	struct dirent *de;
	DIR *dir;

	dir = opendir("/proc");
	if (!dir)
		return -errno;

	while ((de = readdir(dir))) {
		unsigned int pid;

		if (!all_digits(de->d_name))
			continue;
		pid = (unsigned int)strtoul(de->d_name, NULL, 10);
		if (pid == (unsigned int)self)
			continue;

		snprintf(path, sizeof(path), "/proc/%u/comm", pid);
		if (read_first_line(path, proc_comm, sizeof(proc_comm)) < 0)
			proc_comm[0] = '\0';

		snprintf(path, sizeof(path), "/proc/%u/cmdline", pid);
		if (read_cmdline(path, cmdline, sizeof(cmdline)) < 0)
			cmdline[0] = '\0';

		if (strcmp(proc_comm, comm) && !strstr(cmdline, comm))
			continue;

		if (!found)
			first = pid;
		found++;
	}

	closedir(dir);
	if (!found)
		return -ENOENT;

	*tgid = first;
	if (found > 1)
		fprintf(stderr, "warning: found %u processes named %s, using pid %u\n",
			found, comm, first);
	return 0;
}

static int bump_memlock_rlimit(void)
{
	struct rlimit rlim = {
		.rlim_cur = RLIM_INFINITY,
		.rlim_max = RLIM_INFINITY,
	};

	if (setrlimit(RLIMIT_MEMLOCK, &rlim))
		return -errno;
	return 0;
}

static double avg_ns(unsigned long long total, unsigned long long events)
{
	if (!events)
		return 0.0;
	return (double)total / (double)events;
}

static const char *vs_name(unsigned int bucket)
{
	switch (bucket) {
	case RVV_VS_OFF_BUCKET:
		return "off";
	case RVV_VS_INITIAL_BUCKET:
		return "initial";
	case RVV_VS_CLEAN_BUCKET:
		return "clean";
	case RVV_VS_DIRTY_BUCKET:
		return "dirty";
	default:
		return "unknown";
	}
}

static void print_hist(FILE *out, const char *name, const struct rvv_hist *hist)
{
	bool any = false;

	fprintf(out, "  %s:", name);
	for (unsigned int i = 0; i < RVV_HIST_BUCKETS; i++) {
		if (!hist->buckets[i])
			continue;
		if (!any) {
			fputc('\n', out);
			any = true;
		}
		if (!i)
			fprintf(out, "    <1us: %llu\n", hist->buckets[i]);
		else
			fprintf(out, "    [%lluus,%lluus]: %llu\n",
				1ULL << (i - 1), (1ULL << i) - 1,
				hist->buckets[i]);
	}
	if (!any)
		fprintf(out, " empty\n");
}

static void print_vs_samples(FILE *out, const char *name,
			     const unsigned long long samples[RVV_VS_BUCKETS])
{
	fprintf(out, "  %s:", name);
	for (unsigned int i = 0; i < RVV_VS_BUCKETS; i++)
		fprintf(out, " %s=%llu", vs_name(i), samples[i]);
	fputc('\n', out);
}

static void print_thread_line(FILE *out, const struct rvv_thread_stats *t)
{
	fprintf(out,
		"  tid=%u tgid=%u comm=%s in=%llu out=%llu wakeups=%llu "
		"offcpu_avg=%.0fns wake_avg=%.0fns save_cand=%llu restore_cand=%llu\n",
		t->tid, t->tgid, t->comm, t->switches_in, t->switches_out,
		t->wakeups, avg_ns(t->offcpu_total_ns, t->offcpu_events),
		avg_ns(t->wake_to_run_total_ns, t->wake_to_run_events),
		t->v_save_candidates, t->v_restore_candidates);
}

static int dump_text(struct rvv_sched_cost_bpf *skel,
		     const struct rvv_global_stats *g,
		     const struct rvv_hist hists[RVV_HIST_MAX],
		     const struct env *env)
{
	struct rvv_thread_stats t = {};
	struct rvv_task_key key = {};
	struct rvv_task_key next_key = {};
	struct rvv_cpu_stats c = {};
	int thread_fd = bpf_map__fd(skel->maps.thread_stats);
	int cpu_fd = bpf_map__fd(skel->maps.cpu_stats);
	int err;

	printf("rvv_sched_cost summary\n");
	printf("  target_tgid=%u target_tid=%u duration=%us sample_vs=%u\n",
	       env->tgid, env->tid, env->duration, env->sample_vs ? 1 : 0);
	printf("  switch_events=%llu in=%llu out=%llu wakeups=%llu\n",
	       g->switch_events, g->switches_in, g->switches_out, g->wakeups);
	printf("  offcpu_events=%llu offcpu_avg=%.0fns offcpu_max=%lluns\n",
	       g->offcpu_events, avg_ns(g->offcpu_total_ns, g->offcpu_events),
	       g->offcpu_max_ns);
	printf("  wake_to_run_events=%llu wake_avg=%.0fns wake_max=%lluns\n",
	       g->wake_to_run_events,
	       avg_ns(g->wake_to_run_total_ns, g->wake_to_run_events),
	       g->wake_to_run_max_ns);
	printf("  switch_interval_events=%llu interval_avg=%.0fns interval_max=%lluns\n",
	       g->switch_interval_events,
	       avg_ns(g->switch_interval_total_ns, g->switch_interval_events),
	       g->switch_interval_max_ns);
	printf("  v_save_candidates=%llu v_restore_candidates=%llu lost_ts_updates=%llu\n",
	       g->v_save_candidates, g->v_restore_candidates,
	       g->lost_ts_updates);
	print_vs_samples(stdout, "prev_vs_samples", g->prev_vs_samples);
	print_vs_samples(stdout, "next_vs_samples", g->next_vs_samples);

	printf("histograms (log2 usec buckets)\n");
	print_hist(stdout, "offcpu", &hists[RVV_HIST_OFFCPU]);
	print_hist(stdout, "wake_to_run", &hists[RVV_HIST_WAKE_TO_RUN]);
	print_hist(stdout, "switch_interval", &hists[RVV_HIST_SWITCH_INTERVAL]);

	printf("per-cpu\n");
	for (unsigned int i = 0; i < RVV_MAX_CPUS; i++) {
		if (bpf_map_lookup_elem(cpu_fd, &i, &c))
			continue;
		if (!c.switch_events && !c.wakeups)
			continue;
		printf("  cpu=%u switch_events=%llu in=%llu out=%llu wakeups=%llu save_cand=%llu restore_cand=%llu\n",
		       i, c.switch_events, c.switches_in, c.switches_out,
		       c.wakeups, c.v_save_candidates, c.v_restore_candidates);
	}

	printf("per-thread\n");
	err = bpf_map_get_next_key(thread_fd, NULL, &next_key);
	while (!err) {
		key = next_key;
		memset(&t, 0, sizeof(t));
		if (!bpf_map_lookup_elem(thread_fd, &key, &t))
			print_thread_line(stdout, &t);
		err = bpf_map_get_next_key(thread_fd, &key, &next_key);
	}

	return 0;
}

static void json_string(FILE *out, const char *s)
{
	fputc('"', out);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\')
			fprintf(out, "\\%c", c);
		else if (c >= 0x20 && c < 0x7f)
			fputc(c, out);
		else
			fprintf(out, "\\u%04x", c);
	}
	fputc('"', out);
}

static void json_u64_array(FILE *out, const unsigned long long *arr,
			   unsigned int count)
{
	fputc('[', out);
	for (unsigned int i = 0; i < count; i++) {
		if (i)
			fputc(',', out);
		fprintf(out, "%llu", arr[i]);
	}
	fputc(']', out);
}

static int dump_json(struct rvv_sched_cost_bpf *skel,
		     const struct rvv_global_stats *g,
		     const struct rvv_hist hists[RVV_HIST_MAX],
		     const struct env *env)
{
	struct rvv_thread_stats t = {};
	struct rvv_task_key key = {};
	struct rvv_task_key next_key = {};
	struct rvv_cpu_stats c = {};
	int thread_fd = bpf_map__fd(skel->maps.thread_stats);
	int cpu_fd = bpf_map__fd(skel->maps.cpu_stats);
	bool first = true;
	FILE *out;
	int err;

	out = fopen(env->json_path, "w");
	if (!out)
		return -errno;

	fprintf(out, "{\n");
	fprintf(out, "  \"target_tgid\": %u,\n", env->tgid);
	fprintf(out, "  \"target_tid\": %u,\n", env->tid);
	fprintf(out, "  \"duration_sec\": %u,\n", env->duration);
	fprintf(out, "  \"sample_vs\": %s,\n", env->sample_vs ? "true" : "false");
	fprintf(out, "  \"sched\": {\n");
	fprintf(out, "    \"switch_events\": %llu,\n", g->switch_events);
	fprintf(out, "    \"switches_in\": %llu,\n", g->switches_in);
	fprintf(out, "    \"switches_out\": %llu,\n", g->switches_out);
	fprintf(out, "    \"wakeups\": %llu,\n", g->wakeups);
	fprintf(out, "    \"offcpu_events\": %llu,\n", g->offcpu_events);
	fprintf(out, "    \"offcpu_total_ns\": %llu,\n", g->offcpu_total_ns);
	fprintf(out, "    \"offcpu_avg_ns\": %.0f,\n",
		avg_ns(g->offcpu_total_ns, g->offcpu_events));
	fprintf(out, "    \"offcpu_max_ns\": %llu,\n", g->offcpu_max_ns);
	fprintf(out, "    \"wake_to_run_events\": %llu,\n", g->wake_to_run_events);
	fprintf(out, "    \"wake_to_run_total_ns\": %llu,\n", g->wake_to_run_total_ns);
	fprintf(out, "    \"wake_to_run_avg_ns\": %.0f,\n",
		avg_ns(g->wake_to_run_total_ns, g->wake_to_run_events));
	fprintf(out, "    \"wake_to_run_max_ns\": %llu,\n", g->wake_to_run_max_ns);
	fprintf(out, "    \"switch_interval_events\": %llu,\n", g->switch_interval_events);
	fprintf(out, "    \"switch_interval_total_ns\": %llu,\n",
		g->switch_interval_total_ns);
	fprintf(out, "    \"switch_interval_avg_ns\": %.0f,\n",
		avg_ns(g->switch_interval_total_ns, g->switch_interval_events));
	fprintf(out, "    \"switch_interval_max_ns\": %llu\n", g->switch_interval_max_ns);
	fprintf(out, "  },\n");

	fprintf(out, "  \"vector\": {\n");
	fprintf(out, "    \"v_save_candidates\": %llu,\n", g->v_save_candidates);
	fprintf(out, "    \"v_restore_candidates\": %llu,\n", g->v_restore_candidates);
	fprintf(out, "    \"prev_vs_samples\": ");
	json_u64_array(out, g->prev_vs_samples, RVV_VS_BUCKETS);
	fprintf(out, ",\n    \"next_vs_samples\": ");
	json_u64_array(out, g->next_vs_samples, RVV_VS_BUCKETS);
	fprintf(out, "\n  },\n");

	fprintf(out, "  \"histograms\": {\n");
	fprintf(out, "    \"unit\": \"log2_us\",\n");
	fprintf(out, "    \"offcpu\": ");
	json_u64_array(out, hists[RVV_HIST_OFFCPU].buckets, RVV_HIST_BUCKETS);
	fprintf(out, ",\n    \"wake_to_run\": ");
	json_u64_array(out, hists[RVV_HIST_WAKE_TO_RUN].buckets,
		       RVV_HIST_BUCKETS);
	fprintf(out, ",\n    \"switch_interval\": ");
	json_u64_array(out, hists[RVV_HIST_SWITCH_INTERVAL].buckets,
		       RVV_HIST_BUCKETS);
	fprintf(out, "\n  },\n");

	fprintf(out, "  \"per_cpu\": [");
	first = true;
	for (unsigned int i = 0; i < RVV_MAX_CPUS; i++) {
		if (bpf_map_lookup_elem(cpu_fd, &i, &c))
			continue;
		if (!c.switch_events && !c.wakeups)
			continue;
		fprintf(out,
			"%s\n    {\"cpu\": %u, \"switch_events\": %llu, \"switches_in\": %llu, \"switches_out\": %llu, \"wakeups\": %llu, \"v_save_candidates\": %llu, \"v_restore_candidates\": %llu}",
			first ? "" : ",", i, c.switch_events, c.switches_in,
			c.switches_out, c.wakeups, c.v_save_candidates,
			c.v_restore_candidates);
		first = false;
	}
	fprintf(out, "%s  ],\n", first ? "" : "\n");

	fprintf(out, "  \"threads\": [");
	first = true;
	err = bpf_map_get_next_key(thread_fd, NULL, &next_key);
	while (!err) {
		key = next_key;
		memset(&t, 0, sizeof(t));
		if (!bpf_map_lookup_elem(thread_fd, &key, &t)) {
			fprintf(out,
				"%s\n    {\"tgid\": %u, \"tid\": %u, \"comm\": ",
				first ? "" : ",", t.tgid, t.tid);
			json_string(out, t.comm);
			fprintf(out,
				", \"switches_in\": %llu, \"switches_out\": %llu, \"wakeups\": %llu, \"offcpu_events\": %llu, \"offcpu_avg_ns\": %.0f, \"wake_to_run_events\": %llu, \"wake_to_run_avg_ns\": %.0f, \"v_save_candidates\": %llu, \"v_restore_candidates\": %llu}",
				t.switches_in, t.switches_out, t.wakeups,
				t.offcpu_events,
				avg_ns(t.offcpu_total_ns, t.offcpu_events),
				t.wake_to_run_events,
				avg_ns(t.wake_to_run_total_ns,
				       t.wake_to_run_events),
				t.v_save_candidates, t.v_restore_candidates);
			first = false;
		}
		err = bpf_map_get_next_key(thread_fd, &key, &next_key);
	}
	fprintf(out, "%s  ],\n", first ? "" : "\n");
	fprintf(out, "  \"lost_ts_updates\": %llu\n", g->lost_ts_updates);
	fprintf(out, "}\n");

	fclose(out);
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option long_options[] = {
		{ "tgid", required_argument, NULL, 'p' },
		{ "pid", required_argument, NULL, 'p' },
		{ "tid", required_argument, NULL, 't' },
		{ "comm", required_argument, NULL, 'c' },
		{ "duration", required_argument, NULL, 'd' },
		{ "json", required_argument, NULL, 'j' },
		{ "no-vs", no_argument, NULL, 'n' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{}
	};
	struct env env = {
		.duration = 10,
		.sample_vs = true,
		.comm = "llama-server",
	};
	struct rvv_sched_cost_bpf *skel = NULL;
	struct rvv_global_stats g = {};
	struct rvv_hist hists[RVV_HIST_MAX] = {};
	struct rvv_config cfg = {};
	__u32 zero = 0;
	int opt;
	int err;

	while ((opt = getopt_long(argc, argv, "p:t:c:d:j:nvh", long_options,
				  NULL)) != -1) {
		switch (opt) {
		case 'p':
			if (parse_uint_arg(optarg, &env.tgid)) {
				fprintf(stderr, "invalid --tgid: %s\n", optarg);
				return 1;
			}
			break;
		case 't':
			if (parse_uint_arg(optarg, &env.tid)) {
				fprintf(stderr, "invalid --tid: %s\n", optarg);
				return 1;
			}
			break;
		case 'c':
			env.comm = optarg;
			break;
		case 'd':
			if (parse_uint_arg(optarg, &env.duration) ||
			    env.duration < 1 || env.duration > 86400) {
				fprintf(stderr, "invalid --duration: %s\n", optarg);
				return 1;
			}
			break;
		case 'j':
			env.json_path = optarg;
			break;
		case 'n':
			env.sample_vs = false;
			break;
		case 'v':
			env.verbose_libbpf = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!env.tgid && !env.tid && env.comm) {
		err = find_tgid_by_comm(env.comm, &env.tgid);
		if (err) {
			fprintf(stderr, "failed to find process comm=%s: %s\n",
				env.comm, strerror(-err));
			fprintf(stderr, "pass --tgid explicitly if the process is not running yet\n");
			return 1;
		}
	}

	if (!env.tgid && !env.tid) {
		usage(argv[0]);
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	libbpf_set_print(env.verbose_libbpf ? libbpf_log_verbose : libbpf_log);

	err = bump_memlock_rlimit();
	if (err)
		fprintf(stderr, "warning: failed to raise RLIMIT_MEMLOCK: %s\n",
			strerror(-err));

	skel = rvv_sched_cost_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "failed to open/load rvv_sched_cost BPF object\n");
		return 1;
	}

	cfg.target_tgid = env.tgid;
	cfg.target_tid = env.tid;
	cfg.sample_vs = env.sample_vs ? 1 : 0;
	err = bpf_map_update_elem(bpf_map__fd(skel->maps.cfg_map), &zero, &cfg,
				  BPF_ANY);
	if (err) {
		err = -errno;
		fprintf(stderr, "failed to update config map: %s\n",
			strerror(-err));
		goto cleanup;
	}

	err = rvv_sched_cost_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "failed to attach BPF programs: %s\n",
			strerror(-err));
		goto cleanup;
	}

	printf("attached: tgid=%u tid=%u duration=%us sample_vs=%u\n",
	       env.tgid, env.tid, env.duration, env.sample_vs ? 1 : 0);
	fflush(stdout);

	for (unsigned int i = 0; i < env.duration && !exiting; i++)
		sleep(1);

	err = bpf_map_lookup_elem(bpf_map__fd(skel->maps.global_stats), &zero,
				  &g);
	if (err) {
		err = -errno;
		fprintf(stderr, "failed to read global_stats: %s\n",
			strerror(-err));
		goto cleanup;
	}

	for (unsigned int i = 0; i < RVV_HIST_MAX; i++) {
		if (bpf_map_lookup_elem(bpf_map__fd(skel->maps.hists), &i,
					&hists[i])) {
			err = -errno;
			fprintf(stderr, "failed to read hist %u: %s\n", i,
				strerror(-err));
			goto cleanup;
		}
	}

	dump_text(skel, &g, hists, &env);
	if (env.json_path) {
		err = dump_json(skel, &g, hists, &env);
		if (err) {
			fprintf(stderr, "failed to write json %s: %s\n",
				env.json_path, strerror(-err));
			goto cleanup;
		}
		printf("json written: %s\n", env.json_path);
	}

cleanup:
	rvv_sched_cost_bpf__destroy(skel);
	return err ? 1 : 0;
}

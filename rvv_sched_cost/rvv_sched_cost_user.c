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
	bool sample_switch_out;
	bool sample_out_state;
	bool sample_futex;
	bool sample_futex_detail;
	bool sample_futex_stack;
	bool sample_futex_time;
	bool futex_only;
	bool rvv_only;
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
		"Usage: %s [--tgid PID] [--tid TID] [--comm NAME] [--duration SEC] [--json PATH] [--rvv-only] [--no-vs] [--no-switch-out] [--no-switch-out-state] [--futex] [--futex-detail] [--futex-stack] [--futex-time] [--futex-only]\n"
		"\n"
		"Defaults: --comm llama-server --duration 10\n"
		"  --futex-time pairs futex wait enter/exit and records elapsed time\n"
		"  --futex-only enables futex time/detail and disables sched tracepoints\n"
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

static const char *out_state_name(unsigned int bucket)
{
	switch (bucket) {
	case RVV_OUT_RUNNING_BUCKET:
		return "running";
	case RVV_OUT_PREEMPT_BUCKET:
		return "preempt";
	case RVV_OUT_SLEEPING_BUCKET:
		return "sleeping";
	case RVV_OUT_UNINTERRUPTIBLE_BUCKET:
		return "uninterruptible";
	default:
		return "other";
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

static void print_out_state_samples(FILE *out, const char *name,
				    const unsigned long long samples[RVV_OUT_STATE_BUCKETS])
{
	fprintf(out, "  %s:", name);
	for (unsigned int i = 0; i < RVV_OUT_STATE_BUCKETS; i++)
		fprintf(out, " %s=%llu", out_state_name(i), samples[i]);
	fputc('\n', out);
}

static void print_futex_stats(FILE *out, const char *name,
			      unsigned long long syscalls,
			      unsigned long long wait_ops,
			      unsigned long long wake_ops,
			      unsigned long long requeue_ops,
			      unsigned long long wake_op_ops,
			      unsigned long long pi_ops,
			      unsigned long long waitv_syscalls,
			      unsigned long long other_ops)
{
	fprintf(out,
		"  %s: syscalls=%llu wait=%llu wake=%llu requeue=%llu wake_op=%llu pi=%llu waitv=%llu other=%llu\n",
		name, syscalls, wait_ops, wake_ops, requeue_ops, wake_op_ops,
		pi_ops, waitv_syscalls, other_ops);
}

static void print_futex_wait_stats(
	FILE *out, const char *name, unsigned long long completed_ops,
	unsigned long long completed_total_ns, unsigned long long blocked_ops,
	unsigned long long blocked_total_ns, unsigned long long blocked_max_ns,
	const unsigned long long ret[RVV_FUTEX_RET_BUCKETS])
{
	fprintf(out,
		"  %s: completed=%llu elapsed_avg=%.0fns blocked=%llu blocked_total=%lluns blocked_avg=%.0fns blocked_max=%lluns ret[woken=%llu eagain=%llu eintr=%llu etimedout=%llu other=%llu]\n",
		name, completed_ops, avg_ns(completed_total_ns, completed_ops),
		blocked_ops, blocked_total_ns, avg_ns(blocked_total_ns, blocked_ops),
		blocked_max_ns, ret[RVV_FUTEX_RET_WOKEN],
		ret[RVV_FUTEX_RET_EAGAIN], ret[RVV_FUTEX_RET_EINTR],
		ret[RVV_FUTEX_RET_ETIMEDOUT], ret[RVV_FUTEX_RET_OTHER]);
}

static void print_thread_line(FILE *out, const struct rvv_thread_stats *t)
{
	fprintf(out,
		"  tid=%u tgid=%u comm=%s in=%llu out=%llu wakeups=%llu "
		"offcpu_avg=%.0fns wake_avg=%.0fns save_cand=%llu restore_cand=%llu "
		"futex[syscalls=%llu wait=%llu wake=%llu requeue=%llu wake_op=%llu pi=%llu waitv=%llu other=%llu] "
		"out_state[running=%llu preempt=%llu sleeping=%llu uninterruptible=%llu other=%llu] "
		"dirty_out_state[running=%llu preempt=%llu sleeping=%llu uninterruptible=%llu other=%llu]\n",
		t->tid, t->tgid, t->comm, t->switches_in, t->switches_out,
		t->wakeups, avg_ns(t->offcpu_total_ns, t->offcpu_events),
		avg_ns(t->wake_to_run_total_ns, t->wake_to_run_events),
		t->v_save_candidates, t->v_restore_candidates,
		t->futex_syscalls, t->futex_wait_ops, t->futex_wake_ops,
		t->futex_requeue_ops, t->futex_wake_op_ops, t->futex_pi_ops,
		t->futex_waitv_syscalls, t->futex_other_ops,
		t->switches_out_state[RVV_OUT_RUNNING_BUCKET],
		t->switches_out_state[RVV_OUT_PREEMPT_BUCKET],
		t->switches_out_state[RVV_OUT_SLEEPING_BUCKET],
		t->switches_out_state[RVV_OUT_UNINTERRUPTIBLE_BUCKET],
		t->switches_out_state[RVV_OUT_OTHER_BUCKET],
		t->v_save_out_state[RVV_OUT_RUNNING_BUCKET],
		t->v_save_out_state[RVV_OUT_PREEMPT_BUCKET],
		t->v_save_out_state[RVV_OUT_SLEEPING_BUCKET],
		t->v_save_out_state[RVV_OUT_UNINTERRUPTIBLE_BUCKET],
		t->v_save_out_state[RVV_OUT_OTHER_BUCKET]);
}

static void print_waker_line(FILE *out, const struct rvv_waker_stats *w)
{
	fprintf(out,
		"  wakee=%u/%u(%s) waker=%u/%u(%s) wakeups=%llu dirty_wakeups=%llu\n",
		w->wakee_tgid, w->wakee_tid, w->wakee_comm,
		w->waker_tgid, w->waker_tid, w->waker_comm,
		w->wakeups, w->wakee_dirty_wakeups);
}

static void print_futex_addr_line(FILE *out,
				  const struct rvv_futex_addr_stats *s)
{
	fprintf(out,
		"  uaddr=0x%llx syscalls=%llu wait=%llu wake=%llu requeue=%llu wake_op=%llu pi=%llu waitv=%llu other=%llu wait_completed=%llu wait_blocked=%llu blocked_total=%lluns blocked_avg=%.0fns blocked_max=%lluns last=%u/%u(%s)\n",
		s->uaddr, s->syscalls, s->wait_ops, s->wake_ops,
		s->requeue_ops, s->wake_op_ops, s->pi_ops,
		s->waitv_syscalls, s->other_ops, s->wait_completed_ops,
		s->wait_blocked_ops, s->wait_blocked_total_ns,
		avg_ns(s->wait_blocked_total_ns, s->wait_blocked_ops),
		s->wait_blocked_max_ns, s->last_tgid, s->last_tid, s->last_comm);
}

static void print_futex_tid_line(FILE *out,
				 const struct rvv_futex_tid_stats *s)
{
	fprintf(out,
		"  uaddr=0x%llx tgid=%u tid=%u comm=%s syscalls=%llu wait=%llu wake=%llu requeue=%llu wake_op=%llu pi=%llu waitv=%llu other=%llu wait_completed=%llu wait_blocked=%llu blocked_total=%lluns blocked_avg=%.0fns blocked_max=%lluns\n",
		s->uaddr, s->tgid, s->tid, s->comm, s->syscalls,
		s->wait_ops, s->wake_ops, s->requeue_ops, s->wake_op_ops,
		s->pi_ops, s->waitv_syscalls, s->other_ops,
		s->wait_completed_ops, s->wait_blocked_ops,
		s->wait_blocked_total_ns,
		avg_ns(s->wait_blocked_total_ns, s->wait_blocked_ops),
		s->wait_blocked_max_ns);
}

static void read_futex_addr_hist(int map_fd, unsigned long long uaddr,
				 struct rvv_hist *hist)
{
	struct rvv_futex_addr_hist_key key = { .uaddr = uaddr };

	memset(hist, 0, sizeof(*hist));
	for (unsigned int i = 0; i < RVV_HIST_BUCKETS; i++) {
		key.bucket = i;
		(void)bpf_map_lookup_elem(map_fd, &key, &hist->buckets[i]);
	}
}

static void print_futex_stack_line(FILE *out,
				   const struct rvv_futex_stack_stats *s,
				   const unsigned long long ips[RVV_STACK_DEPTH],
				   bool have_stack)
{
	fprintf(out,
		"  uaddr=0x%llx kind=%u stack_id=%d syscalls=%llu wait=%llu wake=%llu requeue=%llu wake_op=%llu pi=%llu waitv=%llu other=%llu last=%u/%u(%s)",
		s->uaddr, s->kind, s->stack_id, s->syscalls, s->wait_ops,
		s->wake_ops, s->requeue_ops, s->wake_op_ops, s->pi_ops,
		s->waitv_syscalls, s->other_ops, s->last_tgid, s->last_tid,
		s->last_comm);
	if (!have_stack) {
		fprintf(out, " stack=<missing>\n");
		return;
	}
	fprintf(out, " stack=");
	for (unsigned int i = 0; i < RVV_STACK_DEPTH && ips[i]; i++)
		fprintf(out, "%s0x%llx", i ? "," : "", ips[i]);
	fputc('\n', out);
}

static int dump_text(struct rvv_sched_cost_bpf *skel,
		     const struct rvv_global_stats *g,
		     const struct rvv_hist hists[RVV_HIST_MAX],
		     const struct env *env)
{
	struct rvv_thread_stats t = {};
	struct rvv_task_key key = {};
	struct rvv_task_key next_key = {};
	struct rvv_waker_key wkey = {};
	struct rvv_waker_key next_wkey = {};
	struct rvv_cpu_stats c = {};
	struct rvv_waker_stats w = {};
	struct rvv_futex_addr_key fkey = {};
	struct rvv_futex_addr_key next_fkey = {};
	struct rvv_futex_addr_stats faddr = {};
	struct rvv_futex_tid_key ftkey = {};
	struct rvv_futex_tid_key next_ftkey = {};
	struct rvv_futex_tid_stats ftid = {};
	struct rvv_futex_stack_key fskey = {};
	struct rvv_futex_stack_key next_fskey = {};
	struct rvv_futex_stack_stats fstack = {};
	unsigned long long ips[RVV_STACK_DEPTH] = {};
	int thread_fd = bpf_map__fd(skel->maps.thread_stats);
	int waker_fd = bpf_map__fd(skel->maps.waker_stats);
	int futex_addr_fd = bpf_map__fd(skel->maps.futex_addr_stats);
	int futex_tid_fd = bpf_map__fd(skel->maps.futex_tid_stats);
	int futex_stack_fd = bpf_map__fd(skel->maps.futex_stack_stats);
	int stack_fd = bpf_map__fd(skel->maps.futex_user_stacks);
	int cpu_fd = bpf_map__fd(skel->maps.cpu_stats);
	int err;

	printf("rvv_sched_cost summary\n");
	printf("  target_tgid=%u target_tid=%u duration=%us rvv_only=%u futex_only=%u sample_vs=%u sample_switch_out=%u sample_out_state=%u sample_futex=%u sample_futex_detail=%u sample_futex_stack=%u sample_futex_time=%u\n",
	       env->tgid, env->tid, env->duration, env->rvv_only ? 1 : 0,
	       env->futex_only ? 1 : 0,
	       env->sample_vs ? 1 : 0,
	       env->sample_switch_out ? 1 : 0,
	       env->sample_out_state ? 1 : 0, env->sample_futex ? 1 : 0,
	       env->sample_futex_detail ? 1 : 0,
	       env->sample_futex_stack ? 1 : 0,
	       env->sample_futex_time ? 1 : 0);
	printf("  switch_events=%llu in=%llu out=%llu wakeups=%llu\n",
	       g->switch_events, g->switches_in, g->switches_out, g->wakeups);
	print_out_state_samples(stdout, "switches_out_state",
				g->switches_out_state);
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
	print_out_state_samples(stdout, "v_save_out_state",
				g->v_save_out_state);
	print_vs_samples(stdout, "prev_vs_samples", g->prev_vs_samples);
	print_vs_samples(stdout, "next_vs_samples", g->next_vs_samples);
	print_futex_stats(stdout, "futex", g->futex_syscalls,
			  g->futex_wait_ops, g->futex_wake_ops,
			  g->futex_requeue_ops, g->futex_wake_op_ops,
			  g->futex_pi_ops, g->futex_waitv_syscalls,
			  g->futex_other_ops);
	print_futex_wait_stats(stdout, "futex_wait_time",
			       g->futex_wait_completed_ops,
			       g->futex_wait_completed_total_ns,
			       g->futex_wait_blocked_ops,
			       g->futex_wait_blocked_total_ns,
			       g->futex_wait_blocked_max_ns,
			       g->futex_wait_ret);
	printf("  futex_wait_start_failures=%llu\n",
	       g->futex_wait_start_failures);

	printf("histograms (log2 usec buckets)\n");
	print_hist(stdout, "offcpu", &hists[RVV_HIST_OFFCPU]);
	print_hist(stdout, "wake_to_run", &hists[RVV_HIST_WAKE_TO_RUN]);
	print_hist(stdout, "switch_interval", &hists[RVV_HIST_SWITCH_INTERVAL]);
	print_hist(stdout, "futex_wait_blocked",
		   &hists[RVV_HIST_FUTEX_WAIT_BLOCKED]);

	printf("per-cpu\n");
	for (unsigned int i = 0; i < RVV_MAX_CPUS; i++) {
		if (bpf_map_lookup_elem(cpu_fd, &i, &c))
			continue;
		if (!c.switch_events && !c.wakeups)
			continue;
		printf("  cpu=%u switch_events=%llu in=%llu out=%llu wakeups=%llu save_cand=%llu restore_cand=%llu "
		       "out_state[running=%llu preempt=%llu sleeping=%llu uninterruptible=%llu other=%llu] "
		       "dirty_out_state[running=%llu preempt=%llu sleeping=%llu uninterruptible=%llu other=%llu]\n",
		       i, c.switch_events, c.switches_in, c.switches_out,
		       c.wakeups, c.v_save_candidates, c.v_restore_candidates,
		       c.switches_out_state[RVV_OUT_RUNNING_BUCKET],
		       c.switches_out_state[RVV_OUT_PREEMPT_BUCKET],
		       c.switches_out_state[RVV_OUT_SLEEPING_BUCKET],
		       c.switches_out_state[RVV_OUT_UNINTERRUPTIBLE_BUCKET],
		       c.switches_out_state[RVV_OUT_OTHER_BUCKET],
		       c.v_save_out_state[RVV_OUT_RUNNING_BUCKET],
		       c.v_save_out_state[RVV_OUT_PREEMPT_BUCKET],
		       c.v_save_out_state[RVV_OUT_SLEEPING_BUCKET],
		       c.v_save_out_state[RVV_OUT_UNINTERRUPTIBLE_BUCKET],
		       c.v_save_out_state[RVV_OUT_OTHER_BUCKET]);
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

	printf("wakers\n");
	err = bpf_map_get_next_key(waker_fd, NULL, &next_wkey);
	while (!err) {
		wkey = next_wkey;
		memset(&w, 0, sizeof(w));
		if (!bpf_map_lookup_elem(waker_fd, &wkey, &w))
			print_waker_line(stdout, &w);
		err = bpf_map_get_next_key(waker_fd, &wkey, &next_wkey);
	}

	printf("futex-addresses\n");
	err = bpf_map_get_next_key(futex_addr_fd, NULL, &next_fkey);
	while (!err) {
		fkey = next_fkey;
		memset(&faddr, 0, sizeof(faddr));
		if (!bpf_map_lookup_elem(futex_addr_fd, &fkey, &faddr))
			print_futex_addr_line(stdout, &faddr);
		err = bpf_map_get_next_key(futex_addr_fd, &fkey, &next_fkey);
	}

	printf("futex-thread-addresses\n");
	err = bpf_map_get_next_key(futex_tid_fd, NULL, &next_ftkey);
	while (!err) {
		ftkey = next_ftkey;
		memset(&ftid, 0, sizeof(ftid));
		if (!bpf_map_lookup_elem(futex_tid_fd, &ftkey, &ftid))
			print_futex_tid_line(stdout, &ftid);
		err = bpf_map_get_next_key(futex_tid_fd, &ftkey, &next_ftkey);
	}

	printf("futex-stacks\n");
	err = bpf_map_get_next_key(futex_stack_fd, NULL, &next_fskey);
	while (!err) {
		bool have_stack;

		fskey = next_fskey;
		memset(&fstack, 0, sizeof(fstack));
		memset(ips, 0, sizeof(ips));
		if (!bpf_map_lookup_elem(futex_stack_fd, &fskey, &fstack)) {
			have_stack = !bpf_map_lookup_elem(stack_fd, &fstack.stack_id,
							  ips);
			print_futex_stack_line(stdout, &fstack, ips, have_stack);
		}
		err = bpf_map_get_next_key(futex_stack_fd, &fskey, &next_fskey);
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
	struct rvv_waker_key wkey = {};
	struct rvv_waker_key next_wkey = {};
	struct rvv_cpu_stats c = {};
	struct rvv_waker_stats w = {};
	struct rvv_futex_addr_key fkey = {};
	struct rvv_futex_addr_key next_fkey = {};
	struct rvv_futex_addr_stats faddr = {};
	struct rvv_futex_tid_key ftkey = {};
	struct rvv_futex_tid_key next_ftkey = {};
	struct rvv_futex_tid_stats ftid = {};
	struct rvv_futex_stack_key fskey = {};
	struct rvv_futex_stack_key next_fskey = {};
	struct rvv_futex_stack_stats fstack = {};
	unsigned long long ips[RVV_STACK_DEPTH] = {};
	int thread_fd = bpf_map__fd(skel->maps.thread_stats);
	int waker_fd = bpf_map__fd(skel->maps.waker_stats);
	int futex_addr_fd = bpf_map__fd(skel->maps.futex_addr_stats);
	int futex_addr_hist_fd = bpf_map__fd(skel->maps.futex_addr_hists);
	int futex_tid_fd = bpf_map__fd(skel->maps.futex_tid_stats);
	int futex_stack_fd = bpf_map__fd(skel->maps.futex_stack_stats);
	int stack_fd = bpf_map__fd(skel->maps.futex_user_stacks);
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
	fprintf(out, "  \"rvv_only\": %s,\n", env->rvv_only ? "true" : "false");
	fprintf(out, "  \"futex_only\": %s,\n",
		env->futex_only ? "true" : "false");
	fprintf(out, "  \"sample_vs\": %s,\n", env->sample_vs ? "true" : "false");
	fprintf(out, "  \"sample_switch_out\": %s,\n",
		env->sample_switch_out ? "true" : "false");
	fprintf(out, "  \"sample_out_state\": %s,\n",
		env->sample_out_state ? "true" : "false");
	fprintf(out, "  \"sample_futex\": %s,\n",
		env->sample_futex ? "true" : "false");
	fprintf(out, "  \"sample_futex_detail\": %s,\n",
		env->sample_futex_detail ? "true" : "false");
	fprintf(out, "  \"sample_futex_stack\": %s,\n",
		env->sample_futex_stack ? "true" : "false");
	fprintf(out, "  \"sample_futex_time\": %s,\n",
		env->sample_futex_time ? "true" : "false");
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
	fprintf(out, "    \"switch_interval_max_ns\": %llu,\n", g->switch_interval_max_ns);
	fprintf(out, "    \"switches_out_state_order\": [\"running\", \"preempt\", \"sleeping\", \"uninterruptible\", \"other\"],\n");
	fprintf(out, "    \"switches_out_state\": ");
	json_u64_array(out, g->switches_out_state, RVV_OUT_STATE_BUCKETS);
	fprintf(out, "\n");
	fprintf(out, "  },\n");

	fprintf(out, "  \"futex\": {\n");
	fprintf(out, "    \"syscalls\": %llu,\n", g->futex_syscalls);
	fprintf(out, "    \"wait_ops\": %llu,\n", g->futex_wait_ops);
	fprintf(out, "    \"wake_ops\": %llu,\n", g->futex_wake_ops);
	fprintf(out, "    \"requeue_ops\": %llu,\n", g->futex_requeue_ops);
	fprintf(out, "    \"wake_op_ops\": %llu,\n", g->futex_wake_op_ops);
	fprintf(out, "    \"pi_ops\": %llu,\n", g->futex_pi_ops);
	fprintf(out, "    \"waitv_syscalls\": %llu,\n", g->futex_waitv_syscalls);
	fprintf(out, "    \"other_ops\": %llu,\n", g->futex_other_ops);
	fprintf(out, "    \"wait_completed_ops\": %llu,\n",
		g->futex_wait_completed_ops);
	fprintf(out, "    \"wait_completed_total_ns\": %llu,\n",
		g->futex_wait_completed_total_ns);
	fprintf(out, "    \"wait_blocked_ops\": %llu,\n",
		g->futex_wait_blocked_ops);
	fprintf(out, "    \"wait_blocked_total_ns\": %llu,\n",
		g->futex_wait_blocked_total_ns);
	fprintf(out, "    \"wait_blocked_max_ns\": %llu,\n",
		g->futex_wait_blocked_max_ns);
	fprintf(out, "    \"wait_ret_order\": [\"woken\", \"eagain\", \"eintr\", \"etimedout\", \"other\"],\n");
	fprintf(out, "    \"wait_ret\": ");
	json_u64_array(out, g->futex_wait_ret, RVV_FUTEX_RET_BUCKETS);
	fprintf(out, ",\n    \"wait_ret_total_ns\": ");
	json_u64_array(out, g->futex_wait_ret_total_ns,
		       RVV_FUTEX_RET_BUCKETS);
	fprintf(out, ",\n    \"wait_start_failures\": %llu\n",
		g->futex_wait_start_failures);
	fprintf(out, "  },\n");

	fprintf(out, "  \"vector\": {\n");
	fprintf(out, "    \"v_save_candidates\": %llu,\n", g->v_save_candidates);
	fprintf(out, "    \"v_restore_candidates\": %llu,\n", g->v_restore_candidates);
	fprintf(out, "    \"v_save_out_state_order\": [\"running\", \"preempt\", \"sleeping\", \"uninterruptible\", \"other\"],\n");
	fprintf(out, "    \"v_save_out_state\": ");
	json_u64_array(out, g->v_save_out_state, RVV_OUT_STATE_BUCKETS);
	fprintf(out, ",\n");
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
	fprintf(out, ",\n    \"futex_wait_blocked\": ");
	json_u64_array(out, hists[RVV_HIST_FUTEX_WAIT_BLOCKED].buckets,
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
			"%s\n    {\"cpu\": %u, \"switch_events\": %llu, \"switches_in\": %llu, \"switches_out\": %llu, \"wakeups\": %llu, \"v_save_candidates\": %llu, \"v_restore_candidates\": %llu, \"switches_out_state\": ",
			first ? "" : ",", i, c.switch_events, c.switches_in,
			c.switches_out, c.wakeups, c.v_save_candidates,
			c.v_restore_candidates);
		json_u64_array(out, c.switches_out_state, RVV_OUT_STATE_BUCKETS);
		fprintf(out, ", \"v_save_out_state\": ");
		json_u64_array(out, c.v_save_out_state, RVV_OUT_STATE_BUCKETS);
		fprintf(out, "}");
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
				", \"switches_in\": %llu, \"switches_out\": %llu, \"wakeups\": %llu, \"offcpu_events\": %llu, \"offcpu_avg_ns\": %.0f, \"wake_to_run_events\": %llu, \"wake_to_run_avg_ns\": %.0f, \"v_save_candidates\": %llu, \"v_restore_candidates\": %llu, \"futex_syscalls\": %llu, \"futex_wait_ops\": %llu, \"futex_wake_ops\": %llu, \"futex_requeue_ops\": %llu, \"futex_wake_op_ops\": %llu, \"futex_pi_ops\": %llu, \"futex_waitv_syscalls\": %llu, \"futex_other_ops\": %llu, \"futex_wait_completed_ops\": %llu, \"futex_wait_completed_total_ns\": %llu, \"futex_wait_blocked_ops\": %llu, \"futex_wait_blocked_total_ns\": %llu, \"futex_wait_blocked_max_ns\": %llu, \"futex_wait_ret\": ",
				t.switches_in, t.switches_out, t.wakeups,
				t.offcpu_events,
				avg_ns(t.offcpu_total_ns, t.offcpu_events),
				t.wake_to_run_events,
				avg_ns(t.wake_to_run_total_ns,
				       t.wake_to_run_events),
				t.v_save_candidates, t.v_restore_candidates,
				t.futex_syscalls, t.futex_wait_ops,
				t.futex_wake_ops, t.futex_requeue_ops,
				t.futex_wake_op_ops, t.futex_pi_ops,
				t.futex_waitv_syscalls, t.futex_other_ops,
				t.futex_wait_completed_ops,
				t.futex_wait_completed_total_ns,
				t.futex_wait_blocked_ops,
				t.futex_wait_blocked_total_ns,
				t.futex_wait_blocked_max_ns);
			json_u64_array(out, t.futex_wait_ret,
				       RVV_FUTEX_RET_BUCKETS);
			fprintf(out, ", \"futex_wait_ret_total_ns\": ");
			json_u64_array(out, t.futex_wait_ret_total_ns,
				       RVV_FUTEX_RET_BUCKETS);
			fprintf(out, ", \"switches_out_state\": ");
			json_u64_array(out, t.switches_out_state,
				       RVV_OUT_STATE_BUCKETS);
			fprintf(out, ", \"v_save_out_state\": ");
			json_u64_array(out, t.v_save_out_state,
				       RVV_OUT_STATE_BUCKETS);
			fprintf(out, "}");
			first = false;
		}
		err = bpf_map_get_next_key(thread_fd, &key, &next_key);
	}
	fprintf(out, "%s  ],\n", first ? "" : "\n");

	fprintf(out, "  \"wakers\": [");
	first = true;
	err = bpf_map_get_next_key(waker_fd, NULL, &next_wkey);
	while (!err) {
		wkey = next_wkey;
		memset(&w, 0, sizeof(w));
		if (!bpf_map_lookup_elem(waker_fd, &wkey, &w)) {
			fprintf(out,
				"%s\n    {\"wakee_tgid\": %u, \"wakee_tid\": %u, \"wakee_comm\": ",
				first ? "" : ",", w.wakee_tgid, w.wakee_tid);
			json_string(out, w.wakee_comm);
			fprintf(out,
				", \"waker_tgid\": %u, \"waker_tid\": %u, \"waker_comm\": ",
				w.waker_tgid, w.waker_tid);
			json_string(out, w.waker_comm);
			fprintf(out,
				", \"wakeups\": %llu, \"dirty_wakeups\": %llu}",
				w.wakeups, w.wakee_dirty_wakeups);
			first = false;
		}
		err = bpf_map_get_next_key(waker_fd, &wkey, &next_wkey);
	}
	fprintf(out, "%s  ],\n", first ? "" : "\n");
	fprintf(out, "  \"futex_addrs\": [");
	first = true;
	err = bpf_map_get_next_key(futex_addr_fd, NULL, &next_fkey);
	while (!err) {
		fkey = next_fkey;
		memset(&faddr, 0, sizeof(faddr));
		if (!bpf_map_lookup_elem(futex_addr_fd, &fkey, &faddr)) {
			struct rvv_hist addr_hist;

			read_futex_addr_hist(futex_addr_hist_fd, faddr.uaddr,
					     &addr_hist);
			fprintf(out,
				"%s\n    {\"uaddr\": \"0x%llx\", \"syscalls\": %llu, \"wait_ops\": %llu, \"wake_ops\": %llu, \"requeue_ops\": %llu, \"wake_op_ops\": %llu, \"pi_ops\": %llu, \"waitv_syscalls\": %llu, \"other_ops\": %llu, \"wait_completed_ops\": %llu, \"wait_completed_total_ns\": %llu, \"wait_blocked_ops\": %llu, \"wait_blocked_total_ns\": %llu, \"wait_blocked_max_ns\": %llu, \"wait_ret\": ",
				first ? "" : ",", faddr.uaddr, faddr.syscalls,
				faddr.wait_ops, faddr.wake_ops,
				faddr.requeue_ops, faddr.wake_op_ops,
				faddr.pi_ops, faddr.waitv_syscalls,
				faddr.other_ops, faddr.wait_completed_ops,
				faddr.wait_completed_total_ns,
				faddr.wait_blocked_ops,
				faddr.wait_blocked_total_ns,
				faddr.wait_blocked_max_ns);
			json_u64_array(out, faddr.wait_ret,
				       RVV_FUTEX_RET_BUCKETS);
			fprintf(out, ", \"wait_ret_total_ns\": ");
			json_u64_array(out, faddr.wait_ret_total_ns,
				       RVV_FUTEX_RET_BUCKETS);
			fprintf(out, ", \"wait_blocked_hist\": ");
			json_u64_array(out, addr_hist.buckets, RVV_HIST_BUCKETS);
			fprintf(out,
				", \"last_tgid\": %u, \"last_tid\": %u, \"last_comm\": ",
				faddr.last_tgid, faddr.last_tid);
			json_string(out, faddr.last_comm);
			fprintf(out, "}");
			first = false;
		}
		err = bpf_map_get_next_key(futex_addr_fd, &fkey, &next_fkey);
	}
	fprintf(out, "%s  ],\n", first ? "" : "\n");

	fprintf(out, "  \"futex_tid_addrs\": [");
	first = true;
	err = bpf_map_get_next_key(futex_tid_fd, NULL, &next_ftkey);
	while (!err) {
		ftkey = next_ftkey;
		memset(&ftid, 0, sizeof(ftid));
		if (!bpf_map_lookup_elem(futex_tid_fd, &ftkey, &ftid)) {
			fprintf(out,
				"%s\n    {\"uaddr\": \"0x%llx\", \"tgid\": %u, \"tid\": %u, \"comm\": ",
				first ? "" : ",", ftid.uaddr, ftid.tgid,
				ftid.tid);
			json_string(out, ftid.comm);
			fprintf(out,
				", \"syscalls\": %llu, \"wait_ops\": %llu, \"wake_ops\": %llu, \"requeue_ops\": %llu, \"wake_op_ops\": %llu, \"pi_ops\": %llu, \"waitv_syscalls\": %llu, \"other_ops\": %llu, \"wait_completed_ops\": %llu, \"wait_completed_total_ns\": %llu, \"wait_blocked_ops\": %llu, \"wait_blocked_total_ns\": %llu, \"wait_blocked_max_ns\": %llu, \"wait_ret\": ",
				ftid.syscalls, ftid.wait_ops, ftid.wake_ops,
				ftid.requeue_ops, ftid.wake_op_ops,
				ftid.pi_ops, ftid.waitv_syscalls,
				ftid.other_ops, ftid.wait_completed_ops,
				ftid.wait_completed_total_ns,
				ftid.wait_blocked_ops,
				ftid.wait_blocked_total_ns,
				ftid.wait_blocked_max_ns);
			json_u64_array(out, ftid.wait_ret,
				       RVV_FUTEX_RET_BUCKETS);
			fprintf(out, ", \"wait_ret_total_ns\": ");
			json_u64_array(out, ftid.wait_ret_total_ns,
				       RVV_FUTEX_RET_BUCKETS);
			fprintf(out, "}");
			first = false;
		}
		err = bpf_map_get_next_key(futex_tid_fd, &ftkey, &next_ftkey);
	}
	fprintf(out, "%s  ],\n", first ? "" : "\n");

	fprintf(out, "  \"futex_stacks\": [");
	first = true;
	err = bpf_map_get_next_key(futex_stack_fd, NULL, &next_fskey);
	while (!err) {
		bool have_stack;

		fskey = next_fskey;
		memset(&fstack, 0, sizeof(fstack));
		memset(ips, 0, sizeof(ips));
		if (!bpf_map_lookup_elem(futex_stack_fd, &fskey, &fstack)) {
			have_stack = !bpf_map_lookup_elem(stack_fd, &fstack.stack_id,
							  ips);
			fprintf(out,
				"%s\n    {\"uaddr\": \"0x%llx\", \"kind\": %u, \"stack_id\": %d, \"syscalls\": %llu, \"wait_ops\": %llu, \"wake_ops\": %llu, \"requeue_ops\": %llu, \"wake_op_ops\": %llu, \"pi_ops\": %llu, \"waitv_syscalls\": %llu, \"other_ops\": %llu, \"last_tgid\": %u, \"last_tid\": %u, \"last_comm\": ",
				first ? "" : ",", fstack.uaddr, fstack.kind,
				fstack.stack_id, fstack.syscalls, fstack.wait_ops,
				fstack.wake_ops, fstack.requeue_ops,
				fstack.wake_op_ops, fstack.pi_ops,
				fstack.waitv_syscalls, fstack.other_ops,
				fstack.last_tgid, fstack.last_tid);
			json_string(out, fstack.last_comm);
			fprintf(out, ", \"ips\": [");
			if (have_stack) {
				bool first_ip = true;

				for (unsigned int i = 0; i < RVV_STACK_DEPTH && ips[i]; i++) {
					fprintf(out, "%s\"0x%llx\"",
						first_ip ? "" : ", ", ips[i]);
					first_ip = false;
				}
			}
			fprintf(out, "]}");
			first = false;
		}
		err = bpf_map_get_next_key(futex_stack_fd, &fskey, &next_fskey);
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
		{ "no-switch-out", no_argument, NULL, 'o' },
		{ "no-switch-out-state", no_argument, NULL, 'r' },
		{ "no-switch-reason", no_argument, NULL, 'r' },
		{ "rvv-only", no_argument, NULL, 'R' },
		{ "futex", no_argument, NULL, 'f' },
		{ "futex-detail", no_argument, NULL, 'F' },
		{ "futex-stack", no_argument, NULL, 'K' },
		{ "futex-time", no_argument, NULL, 'T' },
		{ "futex-only", no_argument, NULL, 'O' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{}
	};
	struct env env = {
		.duration = 10,
		.sample_vs = true,
		.sample_switch_out = true,
		.sample_out_state = true,
		.comm = "llama-server",
	};
	struct rvv_sched_cost_bpf *skel = NULL;
	struct rvv_global_stats g = {};
	struct rvv_hist hists[RVV_HIST_MAX] = {};
	struct rvv_config cfg = {};
	__u32 zero = 0;
	int opt;
	int err;

	while ((opt = getopt_long(argc, argv, "p:t:c:d:j:norRfFKTOvh", long_options,
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
		case 'o':
			env.sample_switch_out = false;
			break;
		case 'r':
			env.sample_out_state = false;
			break;
		case 'R':
			env.rvv_only = true;
			break;
		case 'f':
			env.sample_futex = true;
			break;
		case 'F':
			env.sample_futex = true;
			env.sample_futex_detail = true;
			break;
		case 'K':
			env.sample_futex = true;
			env.sample_futex_stack = true;
			break;
		case 'T':
			env.sample_futex = true;
			env.sample_futex_time = true;
			break;
		case 'O':
			env.futex_only = true;
			env.sample_futex = true;
			env.sample_futex_detail = true;
			env.sample_futex_time = true;
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
	if (env.rvv_only && env.futex_only) {
		fprintf(stderr, "--rvv-only and --futex-only are mutually exclusive\n");
		return 1;
	}
	if (env.rvv_only) {
		env.sample_vs = true;
		env.sample_switch_out = true;
		env.sample_out_state = false;
		env.sample_futex = false;
		env.sample_futex_detail = false;
		env.sample_futex_stack = false;
		env.sample_futex_time = false;
	}
	if (env.futex_only) {
		env.sample_vs = false;
		env.sample_switch_out = false;
		env.sample_out_state = false;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	libbpf_set_print(env.verbose_libbpf ? libbpf_log_verbose : libbpf_log);

	err = bump_memlock_rlimit();
	if (err)
		fprintf(stderr, "warning: failed to raise RLIMIT_MEMLOCK: %s\n",
			strerror(-err));

	skel = rvv_sched_cost_bpf__open();
	if (!skel) {
		fprintf(stderr, "failed to open rvv_sched_cost BPF object\n");
		return 1;
	}

	if (!env.sample_futex)
		bpf_program__set_autoload(skel->progs.on_sys_enter, false);
	if (!env.sample_futex_time)
		bpf_program__set_autoload(skel->progs.on_sys_exit, false);
	if (env.rvv_only || env.futex_only)
		bpf_program__set_autoload(skel->progs.on_sched_wakeup, false);
	if (env.futex_only)
		bpf_program__set_autoload(skel->progs.on_sched_switch, false);

	err = rvv_sched_cost_bpf__load(skel);
	if (err) {
		fprintf(stderr, "failed to load rvv_sched_cost BPF object: %s\n",
			strerror(-err));
		goto cleanup;
	}

	cfg.target_tgid = env.tgid;
	cfg.target_tid = env.tid;
	cfg.sample_vs = env.sample_vs ? 1 : 0;
	cfg.sample_switch_out = env.sample_switch_out ? 1 : 0;
	cfg.sample_out_state = env.sample_out_state ? 1 : 0;
	cfg.sample_futex = env.sample_futex ? 1 : 0;
	cfg.sample_futex_detail = env.sample_futex_detail ? 1 : 0;
	cfg.sample_futex_stack = env.sample_futex_stack ? 1 : 0;
	cfg.sample_futex_time = env.sample_futex_time ? 1 : 0;
	cfg.rvv_only = env.rvv_only ? 1 : 0;
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

	printf("attached: tgid=%u tid=%u duration=%us rvv_only=%u futex_only=%u sample_vs=%u sample_switch_out=%u sample_out_state=%u sample_futex=%u sample_futex_detail=%u sample_futex_stack=%u sample_futex_time=%u\n",
	       env.tgid, env.tid, env.duration, env.rvv_only ? 1 : 0,
	       env.futex_only ? 1 : 0,
	       env.sample_vs ? 1 : 0,
	       env.sample_switch_out ? 1 : 0,
	       env.sample_out_state ? 1 : 0, env.sample_futex ? 1 : 0,
	       env.sample_futex_detail ? 1 : 0,
	       env.sample_futex_stack ? 1 : 0,
	       env.sample_futex_time ? 1 : 0);
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

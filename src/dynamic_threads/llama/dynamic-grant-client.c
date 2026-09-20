#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dynamic-grant-client.h"
#include "dynamic-client.h"
#include "dynamic-worker-team.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    pthread_mutex_t mutex;
    struct dt_client client;
    bool initialized;
    bool configured;
    bool active;
    bool in_graph;
    bool members;
    bool failed;
    bool team_initialized;
    bool retain_shrink;
    struct dt_team team;
} runtime = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .client = {.fd = -1},
    .retain_shrink = true,
};

static int release_persistent(void) {
    if (!runtime.client.held) return 0;
    if (dt_team_detach(&runtime.team, &runtime.client)) return -1;
    return dt_release(&runtime.client);
}

static int revoke_persistent(void) {
    return runtime.retain_shrink ? dt_team_revoke(&runtime.team, &runtime.client)
                                 : release_persistent();
}

static int initialize_locked(void) {
    if (!runtime.initialized) {
        const char * path = getenv("GGML_DYNAMIC_THREADS_SOCKET");
        const char * retain = getenv("GGML_DYNAMIC_THREADS_RETAIN_SHRINK");
        runtime.initialized = true;
        /* Experimental ablation only. Production/default behavior retains the
         * surviving worker bindings; exactly "0" restores full release. */
        runtime.retain_shrink = retain == NULL || strcmp(retain, "0") != 0;
        runtime.configured = path != NULL && path[0] != '\0';
        if (runtime.configured) {
            if (dt_connect(&runtime.client, path, DT_WORKERS)) {
                fprintf(stderr, "GGML_DYNAMIC_THREADS connect failed: %s\n", strerror(errno));
                runtime.failed = true;
                return -1;
            }
            fprintf(stderr, "GGML_DYNAMIC_THREADS connected slot=%u session=%llu\n",
                    runtime.client.slot, (unsigned long long) runtime.client.session);
        }
    }
    return runtime.failed ? -1 : 0;
}

bool ggml_dynamic_threads_set_active(bool active) {
    pthread_mutex_lock(&runtime.mutex);
    int result = initialize_locked();
    if (!result && runtime.configured && runtime.active != active) {
        if (dt_activity(&runtime.client, active)) {
            fprintf(stderr, "GGML_DYNAMIC_THREADS activity failed: %s\n", strerror(errno));
            runtime.failed = true;
            result = -1;
        } else {
            runtime.active = active;
            if (!active && runtime.client.lifetime == DT_PERSISTENT && release_persistent()) {
                runtime.failed = true;
                result = -1;
            }
        }
    }
    pthread_mutex_unlock(&runtime.mutex);
    return result == 0;
}

int ggml_dynamic_threads_begin(unsigned maximum, unsigned * workers,
                              uint32_t backend_phase, uint32_t prompt_tokens, uint32_t decode_tokens) {
    if (workers == NULL || maximum == 0) {
        errno = EINVAL;
        return -1;
    }
    const unsigned effective = *workers;
    if (!effective || effective > maximum) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&runtime.mutex);
    if (initialize_locked() || runtime.in_graph) {
        pthread_mutex_unlock(&runtime.mutex);
        errno = runtime.in_graph ? EBUSY : EIO;
        return -1;
    }
    if (!runtime.configured || !runtime.active) {
        pthread_mutex_unlock(&runtime.mutex);
        return 0;
    }
    if (maximum > DT_WORKERS) {
        runtime.failed = true;
        pthread_mutex_unlock(&runtime.mutex);
        errno = EINVAL;
        return -1;
    }
    int error = 0;
    if (runtime.client.lifetime == DT_PERSISTENT) {
        if (!runtime.team_initialized) {
            error = dt_team_init(&runtime.team);
            runtime.team_initialized = !error;
        }
        /* Report NEW intent before releasing/waiting: graph end never waits. */
        if (!error) error = dt_phase(&runtime.client, backend_phase, prompt_tokens, decode_tokens);
        if (!error) error = dt_poll(&runtime.client);
        if (!error && runtime.client.revoked) error = revoke_persistent();
        if (!error && !runtime.client.held && !runtime.client.pending)
            error = dt_request(&runtime.client, maximum);
        if (!error) error = dt_wait_grant(&runtime.client);
        /* A grant can already be revoked when delivered. Never compute with it. */
        while (!error && runtime.client.revoked) {
            error = revoke_persistent();
            if (!error) error = dt_wait_grant(&runtime.client);
        }
    } else {
        error = dt_acquire(&runtime.client, effective);
    }
    if (error) {
        fprintf(stderr, "GGML_DYNAMIC_THREADS acquire failed: %s\n", strerror(errno));
        runtime.failed = true;
        pthread_mutex_unlock(&runtime.mutex);
        return -1;
    }
    runtime.in_graph = true;
    *workers = runtime.client.grant.workers < effective ? runtime.client.grant.workers : effective;
    /* Keep the mutex held until end(), serializing graph and activity messages. */
    return 1;
}

bool ggml_dynamic_threads_in_graph(void) {
    return runtime.in_graph;
}

bool ggml_dynamic_threads_persistent(void) {
    return runtime.in_graph && runtime.client.lifetime == DT_PERSISTENT;
}

int ggml_dynamic_threads_observe(unsigned ith) {
    return dt_team_observe(&runtime.team, ith);
}
void ggml_dynamic_threads_fail(void) { runtime.failed = true; }

int ggml_dynamic_threads_register(const uint32_t * tids, unsigned count) {
    if (ggml_dynamic_threads_persistent()) {
        int result = dt_team_commit(&runtime.team, &runtime.client, count);
        if (result) {
            fprintf(stderr, "GGML_DYNAMIC_THREADS persistent team failed: %s\n", strerror(errno));
            runtime.failed = true;
        }
        return result;
    }
    if (!runtime.in_graph || runtime.members || count != runtime.client.grant.workers ||
            dt_members(&runtime.client, tids, count)) {
        fprintf(stderr, "GGML_DYNAMIC_THREADS member registration failed: %s\n", strerror(errno));
        runtime.failed = true;
        return -1;
    }
    runtime.members = true;
    return 0;
}

int ggml_dynamic_threads_unregister(void) {
    if (ggml_dynamic_threads_persistent()) return 0;
    if (!runtime.in_graph || !runtime.members) return 0;
    if (dt_unmembers(&runtime.client)) {
        fprintf(stderr, "GGML_DYNAMIC_THREADS member removal failed: %s\n", strerror(errno));
        runtime.failed = true;
        return -1;
    }
    runtime.members = false;
    return 0;
}

int ggml_dynamic_threads_cpu(unsigned ith) {
    return runtime.in_graph ? dt_cpu(&runtime.client, ith) : -1;
}

int ggml_dynamic_threads_end(void) {
    if (!runtime.in_graph) return 0;
    int result = runtime.failed ? -1 : 0;
    if (!result && ggml_dynamic_threads_persistent()) {
        result = dt_poll(&runtime.client);
        if (!result && runtime.client.revoked) result = revoke_persistent();
    } else if (!result) {
        result = ggml_dynamic_threads_unregister();
        if (!result) result = dt_release(&runtime.client);
    }
    if (result) {
        fprintf(stderr, "GGML_DYNAMIC_THREADS release failed: %s\n", strerror(errno));
        runtime.failed = true;
    }
    runtime.in_graph = false;
    pthread_mutex_unlock(&runtime.mutex);
    return result;
}

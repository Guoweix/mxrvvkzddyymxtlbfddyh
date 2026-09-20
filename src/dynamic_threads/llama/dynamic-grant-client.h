#ifndef GGML_DYNAMIC_GRANT_CLIENT_H
#define GGML_DYNAMIC_GRANT_CLIENT_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All functions are no-ops when GGML_DYNAMIC_THREADS_SOCKET is unset. */
bool ggml_dynamic_threads_set_active(bool active);
/* Returns 1 for an acquired grant, 0 when disabled/inactive, -1 on failure. */
int  ggml_dynamic_threads_begin(unsigned maximum, unsigned * workers,
                               uint32_t backend_phase, uint32_t prompt_tokens, uint32_t decode_tokens);
bool ggml_dynamic_threads_in_graph(void);
bool ggml_dynamic_threads_persistent(void);
int  ggml_dynamic_threads_observe(unsigned ith);
void ggml_dynamic_threads_fail(void);
int  ggml_dynamic_threads_register(const uint32_t * tids, unsigned count);
int  ggml_dynamic_threads_unregister(void);
int  ggml_dynamic_threads_cpu(unsigned ith);
int  ggml_dynamic_threads_end(void);

#ifdef __cplusplus
}
#endif

#endif

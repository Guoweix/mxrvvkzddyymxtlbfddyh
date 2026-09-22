# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
#
# x86 版 dynamic_threads：BPF 用本机 BTF，用户态用 host 工具链。
# 不修改 ../dynamic_threads（RISC-V 主线）。

# 统一从 tests/config.sh 取配置（环境变量优先）；相对路径以仓库根为基准。
CFG := ../../tests/config.sh
REPO := ../..
cfgval = $(shell bash -c 'source $(CFG) >/dev/null 2>&1; printf "%s" "$$$(1)"')
cfgpath = $(if $(call cfgval,$(1)),$(if $(filter /%,$(call cfgval,$(1))),$(call cfgval,$(1)),$(abspath $(REPO)/$(call cfgval,$(1)))),)

ROOT ?= $(or $(call cfgpath,TOOLCHAIN_ROOT),$(abspath ../..))
BUILD := build
TESTS := ../../tests/x86_dynamic_threads
BPFTOOL ?= $(ROOT)/jupiter-buildroot/bsp-src/linux-6.6/tools/bpf/bpftool/bpftool
CLANG ?= clang
HOST_CC ?= cc
VMLINUX_BTF ?= /sys/kernel/btf/vmlinux

# phase_probe 需要已应用 0001→0002→llama-dynamic 的 llama.cpp ggml-cpu 目录。
LLAMA_CPU_SRC ?= $(or $(call cfgpath,DT_X86_LLAMA_CPU_SRC),$(abspath ../../../experiments/x86_dynamic_threads/llama/ggml/src/ggml-cpu))

BPF_FLAGS := -g -O2 -target bpf -D__TARGET_ARCH_x86 -DSCX_API_DSQ_INSERT -I$(BUILD) -I.
USER_FLAGS := -g -O2 -Wall -Wextra -Werror -I. -I$(BUILD) -I$(TESTS)
USER_LIBS := $(shell pkg-config --libs libbpf) -lpthread

.DELETE_ON_ERROR:
.PHONY: all host check-llama clean

all: $(BUILD)/dt_controller $(BUILD)/dt_probe $(BUILD)/dt_persistent_probe

host: $(BUILD)/dt_controller_host $(BUILD)/dt_probe_host $(BUILD)/dt_persistent_probe_host \
      $(BUILD)/test_transport $(BUILD)/test_allocation $(BUILD)/test_worker_team

check-llama:
	@test -f "$(LLAMA_CPU_SRC)/dynamic-grant-client.h" || { \
	  echo "缺少 $(LLAMA_CPU_SRC)/dynamic-grant-client.h"; \
	  echo "请先把 src/llama_cpp_patches 的 0001→0002→llama-dynamic.patch 应用到 llama.cpp，"; \
	  echo "再用 LLAMA_CPU_SRC=<llama.cpp>/ggml/src/ggml-cpu 指定该目录。"; exit 1; }

PHASE_PROBE_SRC := $(TESTS)/phase_probe.c worker_team.c client.c $(wildcard $(LLAMA_CPU_SRC)/dynamic-grant-client.c)
PHASE_PROBE_HEADERS := worker_team.h client.h control.h $(wildcard $(LLAMA_CPU_SRC)/dynamic-grant-client.h $(LLAMA_CPU_SRC)/dynamic-client.h $(LLAMA_CPU_SRC)/dynamic-control.h $(LLAMA_CPU_SRC)/dynamic-worker-team.h)

$(BUILD)/dt_phase_probe_host: $(PHASE_PROBE_SRC) $(PHASE_PROBE_HEADERS) | check-llama $(BUILD)
	$(HOST_CC) $(USER_FLAGS) -I$(LLAMA_CPU_SRC) -fopenmp -pthread $(PHASE_PROBE_SRC) -o $@

$(BUILD)/dt_phase_probe: $(PHASE_PROBE_SRC) $(PHASE_PROBE_HEADERS) | check-llama $(BUILD)
	$(HOST_CC) $(USER_FLAGS) -I$(LLAMA_CPU_SRC) -fopenmp -pthread $(PHASE_PROBE_SRC) -o $@

$(BUILD)/control.bpf.o: control.bpf.c control.h $(BUILD)/vmlinux.h
	$(CLANG) $(BPF_FLAGS) -c $< -o $@

$(BUILD)/control.skel.h: $(BUILD)/control.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

$(BUILD)/dt_controller: controller.c control.h allocation.c allocation.h $(BUILD)/control.skel.h
	$(HOST_CC) $(USER_FLAGS) controller.c allocation.c -o $@ $(USER_LIBS)

$(BUILD)/dt_probe: $(TESTS)/probe.c client.c client.h control.h | $(BUILD)
	$(HOST_CC) $(USER_FLAGS) -fopenmp $(TESTS)/probe.c client.c -o $@

$(BUILD)/dt_persistent_probe: $(TESTS)/persistent_probe.c worker_team.c worker_team.h client.c client.h control.h | $(BUILD)
	$(HOST_CC) $(USER_FLAGS) -fopenmp -pthread $(TESTS)/persistent_probe.c worker_team.c client.c -o $@

$(BUILD)/dt_controller_host: controller.c control.h allocation.c allocation.h | $(BUILD)
	$(HOST_CC) $(USER_FLAGS) -DDT_NO_BPF controller.c allocation.c -o $@

$(BUILD)/dt_probe_host: $(TESTS)/probe.c client.c client.h control.h | $(BUILD)
	$(HOST_CC) $(USER_FLAGS) -fopenmp $(TESTS)/probe.c client.c -o $@

$(BUILD)/dt_persistent_probe_host: $(TESTS)/persistent_probe.c worker_team.c worker_team.h client.c client.h control.h | $(BUILD)
	$(HOST_CC) $(USER_FLAGS) -fopenmp -pthread $(TESTS)/persistent_probe.c worker_team.c client.c -o $@

$(BUILD)/test_transport: $(TESTS)/test_transport.c controller.c client.c client.h control.h allocation.c allocation.h | $(BUILD)
	$(HOST_CC) $(USER_FLAGS) $(TESTS)/test_transport.c client.c allocation.c -o $@

$(BUILD)/test_allocation: $(TESTS)/test_allocation.c allocation.c allocation.h | $(BUILD)
	$(HOST_CC) $(USER_FLAGS) $(TESTS)/test_allocation.c allocation.c -o $@

$(BUILD)/test_worker_team: $(TESTS)/test_worker_team.c worker_team.c worker_team.h client.h control.h | $(BUILD)
	$(HOST_CC) $(USER_FLAGS) -fopenmp -pthread $(TESTS)/test_worker_team.c worker_team.c -Wl,--wrap=sched_setaffinity -o $@

$(BUILD)/vmlinux.h: $(VMLINUX_BTF) $(BPFTOOL) | $(BUILD)
	$(BPFTOOL) btf dump file $(VMLINUX_BTF) format c > $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

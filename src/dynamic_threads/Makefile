ROOT := ../../..
BUILD := build
KERNEL := $(ROOT)/jupiter-buildroot/bsp-src/linux-6.6
HOST := $(ROOT)/jupiter-buildroot/output/k1_v2/host
SYSROOT := $(HOST)/riscv64-buildroot-linux-gnu/sysroot
CLANG ?= clang
BPFTOOL ?= $(KERNEL)/tools/bpf/bpftool/bpftool
CROSS_CC ?= $(HOST)/bin/riscv64-unknown-linux-gnu-gcc
HOST_CC ?= cc

# 测试/探针源码位于 tests/dynamic_threads；llama runtime 源位于 llama/。
TESTS := ../../tests/dynamic_threads
DYNAMIC_CPU := llama

BPF_FLAGS := -g -O2 -target bpf -mcpu=v3 -D__TARGET_ARCH_riscv -I$(BUILD) -I$(KERNEL)/tools/lib -I$(HOST)/include
USER_FLAGS := -g -O2 -Wall -Wextra -Werror -I. -I$(BUILD) -I$(DYNAMIC_CPU) -I$(SYSROOT)/usr/include -I$(HOST)/include
USER_LIBS := $(SYSROOT)/usr/lib/libbpf.a $(SYSROOT)/usr/lib/libelf.a -lz
HOST_FLAGS := -g -O2 -Wall -Wextra -Werror -I. -I$(DYNAMIC_CPU) -I$(TESTS)

.PHONY: all host

all: $(BUILD)/dt_controller $(BUILD)/dt_probe $(BUILD)/dt_persistent_probe $(BUILD)/dt_phase_probe

host: $(BUILD)/dt_controller_host $(BUILD)/dt_probe_host $(BUILD)/dt_persistent_probe_host $(BUILD)/test_transport $(BUILD)/test_allocation $(BUILD)/dt_phase_probe_host $(BUILD)/test_worker_team

$(BUILD)/test_worker_team: $(TESTS)/test_worker_team.c worker_team.c worker_team.h client.h control.h | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) -fopenmp -pthread $(TESTS)/test_worker_team.c worker_team.c -Wl,--wrap=sched_setaffinity -o $@

$(BUILD)/test_worker_team_riscv: $(TESTS)/test_worker_team.c worker_team.c worker_team.h client.h control.h | $(BUILD)
	$(CROSS_CC) $(USER_FLAGS) -fopenmp -pthread $(TESTS)/test_worker_team.c worker_team.c -Wl,--wrap=sched_setaffinity -o $@

PHASE_PROBE_SRC := $(TESTS)/phase_probe.c worker_team.c client.c $(DYNAMIC_CPU)/dynamic-grant-client.c
PHASE_PROBE_HEADERS := worker_team.h client.h control.h $(DYNAMIC_CPU)/dynamic-grant-client.h $(DYNAMIC_CPU)/dynamic-client.h $(DYNAMIC_CPU)/dynamic-control.h $(DYNAMIC_CPU)/dynamic-worker-team.h

$(BUILD)/dt_phase_probe_host: $(PHASE_PROBE_SRC) $(PHASE_PROBE_HEADERS) | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) -fopenmp -pthread $(PHASE_PROBE_SRC) -o $@

$(BUILD)/dt_phase_probe: $(PHASE_PROBE_SRC) $(PHASE_PROBE_HEADERS) | $(BUILD)
	$(CROSS_CC) $(USER_FLAGS) -fopenmp -pthread $(PHASE_PROBE_SRC) -o $@

$(BUILD)/test_allocation: $(TESTS)/test_allocation.c allocation.c allocation.h | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) $(TESTS)/test_allocation.c allocation.c -o $@

$(BUILD)/test_transport: $(TESTS)/test_transport.c controller.c client.c client.h control.h allocation.c allocation.h | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) $(TESTS)/test_transport.c client.c allocation.c -o $@

$(BUILD)/dt_persistent_probe: $(TESTS)/persistent_probe.c worker_team.c worker_team.h client.c client.h control.h | $(BUILD)
	$(CROSS_CC) $(USER_FLAGS) -fopenmp -pthread $(TESTS)/persistent_probe.c worker_team.c client.c -o $@

$(BUILD)/dt_persistent_probe_host: $(TESTS)/persistent_probe.c worker_team.c worker_team.h client.c client.h control.h | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) -fopenmp -pthread $(TESTS)/persistent_probe.c worker_team.c client.c -o $@

$(BUILD)/control.bpf.o: control.bpf.c control.h $(BUILD)/vmlinux.h
	$(CLANG) $(BPF_FLAGS) -c $< -o $@

$(BUILD)/control.skel.h: $(BUILD)/control.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

$(BUILD)/dt_controller: controller.c control.h allocation.c allocation.h $(BUILD)/control.skel.h
	$(CROSS_CC) $(USER_FLAGS) $< allocation.c -o $@ $(USER_LIBS)

$(BUILD)/dt_probe: $(TESTS)/probe.c client.c client.h control.h | $(BUILD)
	$(CROSS_CC) $(USER_FLAGS) -fopenmp $(TESTS)/probe.c client.c -o $@

$(BUILD)/dt_controller_host: controller.c control.h allocation.c allocation.h | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) -DDT_NO_BPF $< allocation.c -o $@

$(BUILD)/dt_probe_host: $(TESTS)/probe.c client.c client.h control.h | $(BUILD)
	$(HOST_CC) $(HOST_FLAGS) -fopenmp $(TESTS)/probe.c client.c -o $@

$(BUILD):
	mkdir -p $@

# 先用 run_gate 从运行中的板卡抓取 BTF，再构建 BPF。
$(BUILD)/vmlinux.h: $(BUILD)/vmlinux.btf
	$(BPFTOOL) btf dump file $< format c > $@

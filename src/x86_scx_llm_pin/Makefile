# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
#
# Self-contained x86_64 port of scx_llm_pin.
# Builds against the RUNNING host kernel BTF (/sys/kernel/btf/vmlinux) and
# depends only on files inside this directory.

TARGET    := x86_scx_llm
BPF_SRC   := scx_llm_pin.bpf.c
USER_SRC  := scx_llm_pin.c
BPF_OBJ   := scx_llm_pin.bpf.o
SKEL_H    := scx_llm_pin.skel.h
USER_OBJ  := scx_llm_pin.o

VMLINUX_BTF ?= /sys/kernel/btf/vmlinux
VMLINUX_H   ?= vmlinux.h

CLANG ?= clang
CC    ?= cc

# Prefer bpftool from PATH. Fall back to the bundled copy under tools/ so the
# project stays self-contained. check-bpftool fails loudly if neither exists.
BPFTOOL ?= $(shell command -v bpftool 2>/dev/null)
ifeq ($(BPFTOOL),)
BPFTOOL := $(CURDIR)/tools/bpftool
endif

BPF_CFLAGS  := -g -O2 -target bpf -D__TARGET_ARCH_x86 -DSCX_ARCH_X86 \
	       -DSCX_API_DSQ_INSERT -I.
USER_CFLAGS := -g -O2 -Wall -Wextra -DSCX_ARCH_X86 -I.
USER_LDLIBS := $(shell pkg-config --libs libbpf)

.PHONY: all clean test check-bpftool

all: $(TARGET)

check-bpftool:
	@command -v "$(BPFTOOL)" >/dev/null 2>&1 || { \
		echo "ERROR: bpftool not found in PATH and no bundled tools/bpftool."; \
		echo "       Install bpftool (e.g. the libbpf/bpftool package), or place an"; \
		echo "       executable x86_64 bpftool at $(CURDIR)/tools/bpftool."; \
		exit 1; \
	}

$(VMLINUX_H): $(VMLINUX_BTF) check-bpftool
	$(BPFTOOL) btf dump file $< format c > $@

$(BPF_OBJ): $(BPF_SRC) $(VMLINUX_H) scx_llm_pin.h scx_rvv_lease_control.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

$(SKEL_H): $(BPF_OBJ) check-bpftool
	$(BPFTOOL) gen skeleton $< > $@

$(USER_OBJ): $(USER_SRC) scx_llm_pin.h scx_rvv_lease_control.h team_config.h $(SKEL_H)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(TARGET): $(USER_OBJ)
	$(CC) $< -o $@ $(USER_LDLIBS)

# Does not require sched_ext or root: config validation + skeleton generation.
test: all
	./$(TARGET) --help >/dev/null
	./$(TARGET) --check-config --team 100:4:4:0xff --team 200:4:4:0xff >/dev/null
	@! ./$(TARGET) --check-config --team 100:4:4:0x0f --team 200:4:4:0x0f \
		>/dev/null 2>&1 \
		|| { echo "FAIL: spread accepted a non-0xff CPU mask"; exit 1; }
	@! ./$(TARGET) --check-config --team 100:4:4:0xff --team 200:4:4:0xff \
		--team 300:4:4:0xff >/dev/null 2>&1 \
		|| { echo "FAIL: spread accepted more than two teams"; exit 1; }
	$(CC) -Wall -Wextra -Werror -I. tests/test_team_config.c -o /tmp/x86_scx_llm_test_team_config
	/tmp/x86_scx_llm_test_team_config
	$(BPFTOOL) gen skeleton $(BPF_OBJ) >/dev/null
	@echo "scx_llm_pin x86 build + config + skeleton tests passed"

clean:
	rm -f $(TARGET) $(BPF_OBJ) $(SKEL_H) $(USER_OBJ) $(VMLINUX_H)

# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause

ROOT ?= ../../..
KERNEL_SRC ?= $(ROOT)/jupiter-buildroot/bsp-src/linux-6.6
BUILDROOT_OUT ?= $(ROOT)/jupiter-buildroot/output/k1_v2
STAGING_DIR ?= $(BUILDROOT_OUT)/host/riscv64-buildroot-linux-gnu/sysroot

CLANG ?= clang
HOST_CC ?= cc
CROSS_COMPILE ?= $(BUILDROOT_OUT)/host/bin/riscv64-unknown-linux-gnu-
CROSS_CC ?= $(CROSS_COMPILE)gcc
CROSS_STRIP ?= $(CROSS_COMPILE)strip
BPFTOOL ?= $(KERNEL_SRC)/tools/bpf/bpftool/bpftool

# 统一从 tests/benchmark/config.sh 取板端信息（环境变量优先）。
CFG := ../../tests/benchmark/config.sh
cfgval = $(shell bash -c 'source $(CFG) >/dev/null 2>&1; printf "%s" "$$$(1)"')
BOARD_IP ?= $(or $(call cfgval,REMOTE_IP),192.168.3.67)
BOARD_USER ?= $(or $(call cfgval,REMOTE_USER),root)
BOARD_PASS ?= $(or $(call cfgval,REMOTE_PASS),bianbu)
BOARD ?= $(BOARD_USER)@$(BOARD_IP)
SSH_OPTS ?= -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
SSHPASS ?= sshpass -p $(BOARD_PASS)

VMLINUX_BTF ?= vmlinux.btf
VMLINUX_H ?= vmlinux.h
LIBBPF_A ?= $(STAGING_DIR)/usr/lib/libbpf.a
LLAMA_CPU_SRC ?= $(or $(call cfgval,LLAMA_CPU_SRC),../../../kbuild/llama.cpp/ggml/src/ggml-cpu)

BPF_CFLAGS := -g -O2 -target bpf -mcpu=v3 -D__TARGET_ARCH_riscv
BPF_CFLAGS += -I. -I$(KERNEL_SRC)/tools/lib -I$(BUILDROOT_OUT)/host/include
USER_CFLAGS := -g -O2 -Wall -Wextra -I. -I$(STAGING_DIR)/usr/include -I$(BUILDROOT_OUT)/host/include
USER_LDFLAGS := -static-libgcc -Wl,-Bstatic -L$(STAGING_DIR)/usr/lib
USER_LDLIBS := $(LIBBPF_A) $(STAGING_DIR)/usr/lib/libelf.a -Wl,-Bdynamic -lz

TARGET := scx_llm_pin
CONTROL_TARGET := rvv_lease_ctl
BPF_OBJ := $(TARGET).bpf.o
SKEL_H := $(TARGET).skel.h
USER_OBJ := $(TARGET).o

.PHONY: all clean fetch-btf deploy strip host-bpftool test

all: $(TARGET) $(CONTROL_TARGET) rvv_team_exec

# 从板卡获取 BTF 信息
fetch-btf:
	$(SSHPASS) scp $(SSH_OPTS) $(BOARD):/sys/kernel/btf/vmlinux $(VMLINUX_BTF)

$(VMLINUX_BTF):
	$(MAKE) fetch-btf

# 生成 vmlinux.h
host-bpftool $(BPFTOOL):
	$(MAKE) EXTRA_CFLAGS="-g -O2 -Wno-error=discarded-qualifiers" -C $(KERNEL_SRC)/tools/bpf/bpftool

$(VMLINUX_H): $(VMLINUX_BTF) $(BPFTOOL)
	$(BPFTOOL) btf dump file $(VMLINUX_BTF) format c > $@

# 编译 BPF 程序
$(BPF_OBJ): $(TARGET).bpf.c $(VMLINUX_H) $(TARGET).h scx_rvv_lease_control.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

# 生成 skeleton
$(SKEL_H): $(BPF_OBJ) $(BPFTOOL)
	$(BPFTOOL) gen skeleton $< > $@

# 编译用户态程序
$(USER_OBJ): $(TARGET).c team_config.h $(SKEL_H) $(TARGET).h scx_rvv_lease_control.h
	$(CROSS_CC) $(USER_CFLAGS) -c $< -o $@

# 链接最终程序
$(TARGET): $(USER_OBJ)
	$(CROSS_CC) $(USER_LDFLAGS) $< -o $@ $(USER_LDLIBS)

$(CONTROL_TARGET): rvv_lease_ctl.c $(TARGET).h scx_rvv_lease_control.h
	$(CROSS_CC) $(USER_CFLAGS) $< -o $@

rvv_team_exec: rvv_team_exec.c scx_rvv_lease_control.h
	$(CROSS_CC) $(USER_CFLAGS) $< -o $@

test:
	$(HOST_CC) -Wall -Wextra -Werror -I. tests/test_team_config.c -o /tmp/scx_llm_pin_test_team_config
	/tmp/scx_llm_pin_test_team_config
	$(HOST_CC) -Wall -Wextra -Werror -I$(LLAMA_CPU_SRC) tests/test_hint_client.c -o /tmp/scx_llm_pin_test_hint_client
	/tmp/scx_llm_pin_test_hint_client

# 剥离调试符号（可选）
strip: $(TARGET)
	$(CROSS_STRIP) $(TARGET)

# 部署到板卡
deploy: all
	$(SSHPASS) ssh $(SSH_OPTS) $(BOARD) "mkdir -p /tmp/scx_llm_pin"
	$(SSHPASS) scp $(SSH_OPTS) $(TARGET) $(BOARD):/tmp/scx_llm_pin/
	$(SSHPASS) scp $(SSH_OPTS) $(CONTROL_TARGET) $(BOARD):/tmp/scx_llm_pin/
	$(SSHPASS) scp $(SSH_OPTS) rvv_team_exec $(BOARD):/tmp/scx_llm_pin/
	@echo "Deployed to $(BOARD):/tmp/scx_llm_pin/$(TARGET)"

clean:
	rm -f $(TARGET) $(CONTROL_TARGET) rvv_team_exec $(BPF_OBJ) $(SKEL_H) $(USER_OBJ)
	rm -f $(VMLINUX_BTF) $(VMLINUX_H)

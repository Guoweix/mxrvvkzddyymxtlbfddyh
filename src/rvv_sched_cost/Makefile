# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause

ROOT ?= ../../..
KERNEL_SRC ?= $(ROOT)/jupiter-buildroot/bsp-src/linux-6.6
BUILDROOT_OUT ?= $(ROOT)/jupiter-buildroot/output/k1_v2
BUILDROOT_DIR ?= $(ROOT)/jupiter-buildroot/buildroot
BR2_EXTERNAL ?= $(ROOT)/jupiter-buildroot/buildroot-ext
STAGING_DIR ?= $(BUILDROOT_OUT)/host/riscv64-buildroot-linux-gnu/sysroot

CLANG ?= clang
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
LIBELF_A ?= $(STAGING_DIR)/usr/lib/libelf.a
LIBZ_A ?= $(STAGING_DIR)/usr/lib/libz.a
LIBELF_SO ?= $(BUILDROOT_OUT)/target/usr/lib/libelf-0.186.so

BPF_CFLAGS := -g -O2 -target bpf -D__TARGET_ARCH_riscv
BPF_CFLAGS += -I. -I$(KERNEL_SRC)/tools/lib -I$(BUILDROOT_OUT)/host/include
USER_CFLAGS := -g -O2 -Wall -Wextra -I. -I$(STAGING_DIR)/usr/include -I$(BUILDROOT_OUT)/host/include
USER_LDLIBS := $(LIBBPF_A) -lelf -lz

.PHONY: all clean fetch-btf deploy target-libbpf host-bpftool strip

all: rvv_sched_cost

fetch-btf:
	$(SSHPASS) scp $(SSH_OPTS) $(BOARD):/sys/kernel/btf/vmlinux $(VMLINUX_BTF)

$(VMLINUX_BTF):
	$(MAKE) fetch-btf

host-bpftool $(BPFTOOL):
	$(MAKE) EXTRA_CFLAGS="-g -O2 -Wno-error=discarded-qualifiers" -C $(KERNEL_SRC)/tools/bpf/bpftool

$(VMLINUX_H): $(VMLINUX_BTF) $(BPFTOOL)
	$(BPFTOOL) btf dump file $(VMLINUX_BTF) format c > $@

rvv_sched_cost.bpf.o: rvv_sched_cost.bpf.c rvv_sched_cost.h $(VMLINUX_H)
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

rvv_sched_cost.skel.h: rvv_sched_cost.bpf.o $(BPFTOOL)
	$(BPFTOOL) gen skeleton $< > $@

target-libbpf:
	sudo $(MAKE) -C $(BUILDROOT_DIR) O=$(abspath $(BUILDROOT_OUT)) BR2_EXTERNAL=$(abspath $(BR2_EXTERNAL)) GLOBAL_INSTRUMENTATION_HOOKS=step_time libbpf

$(LIBBPF_A):
	$(MAKE) target-libbpf

rvv_sched_cost: rvv_sched_cost_user.c rvv_sched_cost.h rvv_sched_cost.skel.h $(LIBBPF_A)
	$(CROSS_CC) $(USER_CFLAGS) -o $@ rvv_sched_cost_user.c $(USER_LDLIBS)

strip: rvv_sched_cost
	$(CROSS_STRIP) rvv_sched_cost

deploy: rvv_sched_cost
	$(SSHPASS) ssh $(SSH_OPTS) $(BOARD) 'mkdir -p /tmp/rvv_sched_cost/lib'
	$(SSHPASS) scp $(SSH_OPTS) rvv_sched_cost $(BOARD):/tmp/rvv_sched_cost/rvv_sched_cost
	$(SSHPASS) scp $(SSH_OPTS) $(LIBELF_SO) $(BOARD):/tmp/rvv_sched_cost/lib/libelf-0.186.so
	$(SSHPASS) ssh $(SSH_OPTS) $(BOARD) 'ln -sf libelf-0.186.so /tmp/rvv_sched_cost/lib/libelf.so.1'

clean:
	rm -f rvv_sched_cost rvv_sched_cost.bpf.o rvv_sched_cost.skel.h

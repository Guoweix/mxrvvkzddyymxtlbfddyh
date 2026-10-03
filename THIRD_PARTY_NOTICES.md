# Third-Party Notices / 第三方组件声明

本文件列出本仓库所派生、打补丁、链接或再分发的第三方组件，及其版本（或基线
提交）、版权人和许可证。许可证正文见 [`LICENSES/`](LICENSES/)。本项目自身的
默认许可证见 [`LICENSE`](LICENSE)。

> 说明：`LICENSES/` 只存放许可证正文；本文件负责「组件 → 版本 → 版权人 →
> 许可证」的对应关系。MIT/BSD 类许可证要求复现**具体版权行**，仅靠许可证正文
> 不足以满足署名义务，因此两者都需要。

---

## 1. Linux 内核（K1 厂商基线 6.6）

- 组件：Linux kernel 及 K1（SpacemiT / Bianbu）厂商补丁树
- 基线提交：`46ef2dae4ac50c7e556137e4cbbdcc024921d9e3`
- 应用补丁后的参考 tree：`4f0f6bfe8de9de50d70ee62886507dd393dfaa9f`
- 本仓库文件：`src/kernel_patches/0001-*.patch`、`0002-*.patch`
- 版权人：Linux 内核众多作者（见各文件头部版权声明）
- 许可证：`GPL-2.0-only`（内核根目录 `COPYING` 说明为 GPL-2.0 并附带
  Linux syscall note 例外）
- 许可证正文：`LICENSES/GPL-2.0-only.txt`
- 说明：补丁不改变被修改文件原有的 SPDX 标识或版权声明；使用、分发打补丁后的
  内核时须继续遵守内核源码树 `COPYING` 及各文件头部声明。详见
  `src/kernel_patches/README.md` 的「开源许可证」。

## 2. sched_ext（非主线发布树）

- 组件：`kernel/sched/ext.c`、`include/linux/sched/ext.h` 及其集成改动
- 来源：<https://github.com/sched-ext/scx-kernel-releases>
- 版本：`v6.6.4-scx2`（tag 对象 `e07b52f8ba172c234edcf45a1fa52735430578f9`）
- 版权人：Meta Platforms、Tejun Heo、David Vernet 等（见文件头部）
- 许可证：`GPL-2.0-only`
- 许可证正文：`LICENSES/GPL-2.0-only.txt`
- 说明：`v6.6.4-scx2` 不是 vanilla Linux 6.6，也不是 Linux 6.12 主线提交的直接
  回放；`0001` 是面向 K1 厂商树的移植补丁。

## 3. llama.cpp

- 来源：<https://github.com/ggerganov/llama.cpp>
- 基线提交：`678d43d7205cffc3a8a026cb60b1de3868cc70a2`
- 应用补丁后的参考 tree：`f560bb73bc657ec728e9d6a980ea1e20c9c7f378`
- 版权人：`Copyright (c) 2023-2026 The ggml authors`
- 许可证：MIT
- 许可证正文：`LICENSES/MIT.txt`
- 本仓库文件：
  - 补丁：`src/llama_cpp_patches/0001-*.patch`、`0002-*.patch`、`llama-dynamic.patch`
  - 再分发的二进制运行时：`demo/riscv/runtime/*`、`demo/x86/runtime/*`
    （`llama-server`、`libllama*`、`libggml*`、`libmtmd*`）
- 说明：
  - 对既有 llama.cpp 文件的修改继续使用 MIT。
  - 新增 `scx-barrier-hint-client.c/.h` 为 `MIT`。
  - `scx-rvv-barrier-hint.h`、`scx-rvv-lease-control.h` 同时供 MIT 用户态与 GPL
    eBPF 使用，标记为 `GPL-2.0 OR MIT`；在 llama.cpp 用户态构建中可按 MIT 使用。
  - 分发修改后的源码或二进制时，须保留上游 MIT 版权与许可文本，以及新增文件的
    SPDX 标识。详见 `src/llama_cpp_patches/README.md` 的「开源许可证」。

## 4. libbpf

- 组件：用于生成 BPF 骨架 `*.skel.h` 的 libbpf / bpftool
- 本仓库文件：由 bpftool 生成的 BPF 骨架 `*.skel.h`；随仓库纳入版本管理的是
  `src/rvv_sched_cost/rvv_sched_cost.skel.h`（其余 `*.skel.h` 为构建产物，未纳入版本管理）
- 版权人：The libbpf authors（见各骨架文件头部）
- 许可证：`LGPL-2.1 OR BSD-2-Clause`（生成文件头部的 SPDX 标识）
- 许可证正文：`LICENSES/LGPL-2.1-only.txt`、`LICENSES/BSD-2-Clause.txt`
- 说明：libbpf 未以源码形式随本仓库分发，仅其生成产物 `*.skel.h` 在仓库中。

## 5. elfutils（libelf）与 zlib

- 组件：`src/rvv_sched_cost` 链接的 `libelf` 与 `zlib`
- 本仓库文件：`src/rvv_sched_cost/Makefile`（链接）、`demo/riscv/bin/rvv_sched_cost`（二进制）
- 许可证：
  - elfutils / libelf：`LGPL-3.0-or-later OR GPL-2.0-or-later`
  - zlib：Zlib
- 说明：libelf/zlib 未随本仓库以源码或共享库形式分发，`rvv_sched_cost` 在目标板上
  动态链接目标系统提供的 `libelf.so.1`。若再分发该二进制，须确保目标环境提供
  相应库，并遵守其许可证。
- 许可证正文：本仓库未附带（如需分发请补充 `LICENSES/LGPL-3.0-or-later.txt`、
  `LICENSES/Zlib.txt`）。

## 6. 板卡运行环境

- K1 Buildroot manifest：<https://github.com/milkv-jupiter/manifests>
- Board OS：Bianbu（SpacemiT K1）busybox 根文件系统
- 说明：该运行环境不随本仓库源码分发；本仓库仅提供面向它的构建/部署脚本。

---

## 许可证正文文件索引

| 文件 | 许可证 |
|---|---|
| `LICENSES/GPL-2.0-only.txt` | GNU GPL v2 only |
| `LICENSES/MIT.txt` | MIT |
| `LICENSES/BSD-2-Clause.txt` | BSD 2-Clause "Simplified" |
| `LICENSES/LGPL-2.1-only.txt` | GNU LGPL v2.1 only |

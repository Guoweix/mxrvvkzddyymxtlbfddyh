# Linux sched_ext 与 RVV lazy restore 补丁集

本目录保存针对 K1 原厂 Linux 6.6 内核的两个顺序补丁。它们解决两个不同层次
的问题：

1. `0001` 先让原厂内核具备 sched_ext 内核功能。没有这个补丁，
   `scx_llm_pin` 等 BPF 调度器无法注册和接管任务。
2. `0002` 补齐 sched_ext 在该厂商树上的兼容代码，并加入可选的 RISC-V RVV
   lazy restore 实验功能。

Linux 6.6 主线和 K1 厂商基线都不包含 sched_ext；该功能在 Linux 6.12 才正式
合入主线。本补丁集采用 sched-ext 项目在主线化之前维护的非主线
`v6.6.4-scx2` 发布树作为实现来源，并将其移植到 K1 厂商 Linux 6.6。

这两个补丁从本项目 Linux 分支 `k1-bl-v2.1.y` 导出，必须按编号顺序应用。
`0002` 依赖 `0001`，不能单独应用。

## 补丁 1：为原厂内核添加 sched_ext

文件：`0001-sched_ext-port-sched_ext-from-scx-v6.6.4-scx2.patch`

### 作用

K1 原厂 Linux 6.6 基线没有 sched_ext 实现。该补丁完成以下内核能力移植：

- 加入 `SCHED_CLASS_EXT` 调度类及 `sched_ext_ops` BPF struct_ops 接口。
- 将 sched_ext 接入 `core`、`fair`、`idle`、`deadline`、SysRq 和调度类链接布局。
- 加入 `include/linux/sched/ext.h` 和 `kernel/sched/ext.c` 核心实现。
- 在 `k1_defconfig` 中启用 `CONFIG_SCHED_CLASS_EXT` 和
  `CONFIG_DEBUG_INFO_BTF`，供 CO-RE eBPF 调度器使用。

该补丁只提供内核调度框架，不包含具体 LLM 调度策略。实际策略由
`experiments/scx_llm_pin/` 中的用户态程序和 BPF struct_ops 调度器加载；未加载
sched_ext 调度器时，系统仍按默认 Linux 调度路径运行。

### 上游来源

sched_ext 主体来自 sched-ext 项目的非主线 Linux 内核发布仓库：

- 仓库：<https://github.com/sched-ext/scx-kernel-releases>
- 参考版本：[`v6.6.4-scx2`](https://github.com/sched-ext/scx-kernel-releases/tree/v6.6.4-scx2)
- tag 对象：`e07b52f8ba172c234edcf45a1fa52735430578f9`

其中 `kernel/sched/ext.c` 和 `include/linux/sched/ext.h` 以该版本为移植源；其余
调度器文件的修改是把该版本的 sched_ext 接口接入 K1 原厂 6.6 内核时所做的
适配。`v6.6.4-scx2` 不是 vanilla Linux 6.6，也不是 Linux 6.12 主线提交的直接
回放；`0001` 是面向 K1 厂商树的移植补丁，不能直接应用到任意 Linux 6.6 分支。

### 开源许可证

- Linux 内核整体由源码根目录 `COPYING` 说明为 GPL version 2 only，并带有
  Linux syscall note 对用户空间接口的例外说明。
- 上游 `kernel/sched/ext.c` 和 `include/linux/sched/ext.h` 均保留
  `SPDX-License-Identifier: GPL-2.0`。按照 Linux 内核
  `LICENSES/preferred/GPL-2.0`，这里表示 GPL-2.0-only。
- 上述文件保留 Meta Platforms、Tejun Heo 和 David Vernet 等原作者的版权声明。
- 补丁对其他内核文件的修改不改变这些文件原有的 SPDX 标识或版权声明；使用和
  分发打补丁后的内核时，应继续遵守内核源码树 `COPYING` 和各文件头部标明的
  许可证。

## 补丁 2：厂商树集成与 RVV 实验

文件：`0002-riscv-complete-sched_ext-integration-and-add-lazy-ve.patch`

该补丁建立在 `0001` 之上，内容包括：

- 补齐厂商树 cgroup、fork/init task、RT、topology 和调度器内部头文件集成。
- 增大 sched_ext dispatch loop budget，并补齐独立构建需要的本地调度器头文件。
- 修复 host libbpf const 类型和 bcmdhd out-of-tree 源码路径问题。
- 加入可选的 RVV userspace owner lazy restore、debugfs 统计和正确性自测。

RVV 优化仍保留 eager save，仅在同一 task 返回同一 CPU 且 owner 状态有效时
跳过 `v0-v31` payload restore；`vl`、`vtype`、`vstart` 和 `vcsr` 仍会恢复。
新增的 RVV owner 内核文件和自测使用 `GPL-2.0-only` SPDX 标识；对既有文件的
修改继续遵守各文件原许可证。

## 目标基线

- 内核：K1 厂商 Linux 6.6 源码树
- 基线提交：`46ef2dae4ac50c7e556137e4cbbdcc024921d9e3`
- 应用两个补丁后的参考 tree：`4f0f6bfe8de9de50d70ee62886507dd393dfaa9f`

补丁会修改调度器内部接口。应用到其他 Linux 版本或不同厂商分支时，即使
`git am --3way` 能自动合并，也必须重新完成编译和板端调度验证。

## 应用补丁

先设置源码和补丁目录。示例路径仅为占位符：

```bash
export KERNEL_SRC=/path/to/linux-6.6
export PATCH_DIR=/path/to/experiments/kernel_patches
```

建议从准确基线创建独立分支，并确认工作树没有本地修改：

```bash
git -C "$KERNEL_SRC" status --short
git -C "$KERNEL_SRC" switch -c sched-ext-rvv \
    46ef2dae4ac50c7e556137e4cbbdcc024921d9e3
git -C "$KERNEL_SRC" am --3way \
    "$PATCH_DIR/0001-sched_ext-port-sched_ext-from-scx-v6.6.4-scx2.patch" \
    "$PATCH_DIR/0002-riscv-complete-sched_ext-integration-and-add-lazy-ve.patch"
```

检查补丁是否完整应用：

```bash
git -C "$KERNEL_SRC" log -2 --oneline
git -C "$KERNEL_SRC" diff --check HEAD~2..HEAD
test "$(git -C "$KERNEL_SRC" rev-parse 'HEAD^{tree}')" = \
    4f0f6bfe8de9de50d70ee62886507dd393dfaa9f
```

如果应用过程中发生冲突，可以查看冲突后继续，或者放弃本次应用：

```bash
git -C "$KERNEL_SRC" am --continue
# 或者
git -C "$KERNEL_SRC" am --abort
```

## 配置与构建

准备 RISC-V 交叉工具链。`CROSS_COMPILE` 必须是以连字符结尾的工具前缀：

```bash
export ARCH=riscv
export CROSS_COMPILE=/path/to/toolchain/bin/riscv64-linux-gnu-
export BUILD_DIR=/path/to/build/linux-k1

make -C "$KERNEL_SRC" O="$BUILD_DIR" k1_defconfig
```

`k1_defconfig` 已启用 `CONFIG_SCHED_CLASS_EXT` 和 `CONFIG_DEBUG_INFO_BTF`。
RVV lazy restore 默认不编译；需要该实验功能时再显式启用：

```bash
"$KERNEL_SRC/scripts/config" --file "$BUILD_DIR/.config" \
    --enable RISCV_ISA_V_USER_OWNER
make -C "$KERNEL_SRC" O="$BUILD_DIR" olddefconfig
```

构建内核和全部模块：

```bash
make -C "$KERNEL_SRC" O="$BUILD_DIR" \
    -j"$(nproc)" Image modules
```

参考提交已通过全新输出目录的 `Image + modules` 构建，共生成 671 个模块，
其中包括 `drivers/net/wireless/bcmdhd/bcmdhd.ko`。部署时必须同时安装本次构建的
Image 和匹配模块；不要把新 Image 与旧 `/lib/modules` 混用。

## 运行方式

sched_ext 编译进内核后，未加载 BPF 调度器时系统仍使用默认调度类。加载本项目
`scx_llm_pin` 等 struct_ops 调度器后，目标任务才会进入对应 sched_ext 策略。

RVV lazy restore 还需要内核启动参数：

```text
riscv_v_user_owner=restore
```

不指定参数或设置以下值时使用原始 eager restore 行为：

```text
riscv_v_user_owner=off
```

启动后可以检查模式与统计：

```bash
grep riscv_v_user_owner /proc/cmdline
grep riscv_v_user_owner /sys/kernel/debug/riscv_v_user_owner_stats
```

统计文件的 `elided` 表示成功跳过 `v0-v31` payload restore 的次数。`off` 模式
应保持 `elided=0`。

## RVV 正确性自测

补丁包含 `tools/testing/selftests/riscv/vector/v_owner_context.c`。可以交叉编译后
复制到板卡，在 `off` 和 `restore` 两种启动模式下分别运行：

```bash
export SELFTEST_OUT=/path/to/build/selftests
mkdir -p "$SELFTEST_OUT"
"${CROSS_COMPILE}gcc" -static -pthread -Wall \
    "$KERNEL_SRC/tools/testing/selftests/riscv/vector/v_owner_context.c" \
    -o "$SELFTEST_OUT/v_owner_context"
```

该测试覆盖同核 owner 命中、不同 Vector task、CPU 迁移、signal 和 Vector CSR
恢复。部署前还应执行一次 sched_ext 调度器 attach/detach smoke，并检查 dmesg 中
不存在 watchdog、stall、BUG、Oops 或模块版本不匹配。

## 回退

- 尚在 `git am` 阶段：执行 `git am --abort`。
- 已在独立分支应用：切回原内核分支即可保留补丁分支供后续检查。
- 只关闭 RVV 实验功能：把启动参数改为 `riscv_v_user_owner=off` 并重启。
- 板端回退内核：恢复部署前备份的 Image 和与其严格匹配的模块目录。

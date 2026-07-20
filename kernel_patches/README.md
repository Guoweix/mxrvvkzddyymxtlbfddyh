# Linux sched_ext 与 RVV lazy restore 补丁集

本目录保存 K1 厂商 Linux 6.6 内核使用的两个补丁。补丁从
`k1-bl-v2.1.y` 分支导出，必须按编号顺序应用。

## 补丁内容

| 文件 | 内容 |
|------|------|
| `0001-sched_ext-add-sched_ext-backport-from-v6.6.4-scx2.patch` | 从 `v6.6.4-scx2` 回移 sched_ext 主体和调度类集成，并在 `k1_defconfig` 中启用 sched_ext 与 BTF。 |
| `0002-riscv-complete-sched_ext-integration-and-add-lazy-ve.patch` | 补齐厂商内核的 sched_ext/cgroup/任务生命周期集成，加入 RVV userspace owner lazy restore、自测，以及 libbpf 和 bcmdhd 构建修复。 |

第二个补丁依赖第一个补丁，不能单独应用。RVV 优化仍保留 eager save，仅在同一
task 返回同一 CPU 且 owner 状态有效时跳过 `v0-v31` payload restore；Vector CSR
仍会恢复。

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
    "$PATCH_DIR/0001-sched_ext-add-sched_ext-backport-from-v6.6.4-scx2.patch" \
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

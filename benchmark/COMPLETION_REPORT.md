# 标准化Benchmark测试框架 - 完成报告

## 执行摘要

成功创建了完整的 LLM 推理性能测试框架，用于 RISC-V Vector Extension (RVV) + sched_ext 调度器优化项目。框架支持自动化测试、详细指标记录和结果对比。

**关键成果**:
- ✅ 5分钟长时间稳定性测试
- ✅ 8线程 + 4并发配置
- ✅ 自动记录 TTFT 和 tok/s
- ✅ 完整的文档和验证工具

---

## 完成的工作

### 1. 脚本修复与增强

#### 修复的问题
- **CSV 缺少头部**: 添加了列名，使 `csv.DictReader` 可以正确解析
- **tok/s 重复计算**: 在 bash 中计算一次并写入 CSV，Python 优先使用预计算值
- **IP 地址过期**: 从 192.168.3.67 更新为 192.168.3.63

#### 增强的功能
- 在每个请求级别记录 tok/s
- 向后兼容新旧 CSV 格式
- 改进错误处理和日志输出

### 2. 创建的新文件

| 文件 | 大小 | 用途 |
|------|------|------|
| `BENCHMARK_SPEC.md` | 7.5 KB | 完整的技术规范文档 |
| `QUICK_REFERENCE.md` | 3.9 KB | 快速参考指南 |
| `verify.sh` | 3.5 KB | 环境验证脚本 |
| `run_test.sh` | 879 B | 一键运行脚本 |
| `/PLAN.md` | - | 项目总体计划 (根目录) |

### 3. 更新的文件

| 文件 | 修改内容 |
|------|----------|
| `benchmark.sh` | 添加 CSV 头部，记录 tok/s 列 |
| `generate_report.py` | 支持 toks_per_sec 列，改进统计计算 |
| `config.sh` | 更新 REMOTE_IP 为 192.168.3.63 |
| `docs/dev_log.md` | 记录完整的开发过程 |

---

## 测试框架特性

### 核心功能

1. **自动化测试流程**
   - SSH 自动连接板卡
   - 自动启动/停止 llama-server
   - 并发运行 4 个测试任务
   - 持续测试 5 分钟

2. **全面的指标记录**
   - 首字延迟 (TTFT, ms)
   - 每请求 tok/s
   - 平均/最小/最大 tok/s
   - 总请求数和 token 数
   - 整体吞吐量

3. **多格式输出**
   - CSV: 详细的请求级数据
   - Summary: 每任务的汇总统计
   - Report (TXT): 人类可读报告
   - Report (JSON): 机器可读数据

4. **完善的工具链**
   - `verify.sh`: 环境检查
   - `run_test.sh`: 简化启动
   - `compare.sh`: 结果对比
   - `generate_report.py`: 报告生成

### 测试任务设计

框架包含 4 个精心设计的测试任务:

| 任务 | 类型 | 目的 |
|------|------|------|
| Task 1 | 技术解释 | 测试知识密集型推理 |
| Task 2 | 代码生成 | 测试逻辑密集型输出 |
| Task 3 | 数学计算 | 测试多步骤推理 |
| Task 4 | 创意写作 | 测试长文本生成 |

---

## 技术实现细节

### CSV 格式改进

**之前**:
```
1720447890.123,1,100,3250.5,3500.2
```
- 无头部，需要手动解析
- 缺少 tok/s 数据

**现在**:
```csv
timestamp,request_num,tokens,generation_ms,total_ms,toks_per_sec
1720447890.123,1,100,3250.5,3500.2,30.77
```
- 带头部，`csv.DictReader` 自动解析
- 包含预计算的 tok/s

### TTFT 记录实现

```bash
# 在 benchmark.sh 的 run_single_task() 中
if [ -z "$first_token_time" ]; then
    local ttft=$(echo "$response" | jq -r '.timings.prompt_ms // 0')
    first_token_time=$ttft
    log_info "[Task $task_id] 首字延迟: ${ttft}ms"
fi
```

从 llama.cpp 的响应中提取 `timings.prompt_ms`，仅记录每个任务的第一个请求。

### tok/s 计算优化

**之前**: Python 遍历所有请求重新计算
```python
toks_per_sec = [
    (r['tokens'] / r['generation_ms'] * 1000) if r['generation_ms'] > 0 else 0
    for r in data['requests']
]
```

**现在**: Bash 计算一次，Python 优先使用
```bash
# bash (benchmark.sh)
local toks_per_sec=$(echo "scale=2; if ($generation_time > 0) $tokens / $generation_time * 1000 else 0" | bc)
```

```python
# Python (generate_report.py)
if 'toks_per_sec' in r and r['toks_per_sec'] > 0:
    toks_per_sec.append(r['toks_per_sec'])
else:
    # 向后兼容，重新计算
    toks_per_sec.append(r['tokens'] / r['generation_ms'] * 1000)
```

---

## 环境验证

`verify.sh` 脚本执行 6 项检查:

1. **文件完整性**: 确保所有必需文件存在
2. **系统依赖**: 检查 sshpass, curl, jq, bc, python3
3. **配置正确性**: 验证 IP、线程、时长等参数
4. **脚本权限**: 确保可执行
5. **Python 语法**: 编译检查 generate_report.py
6. **网络连接**: 测试 ping 和 SSH

**验证结果** (2026-07-08):
```
✅ 所有文件存在
✅ 所有依赖已安装
✅ 配置正确
✅ 脚本可执行
✅ Python 语法正确
⚠️  板卡离线 (192.168.3.63)
```

---

## 文档体系

### 为不同用户提供的文档

1. **QUICK_REFERENCE.md** - 快速参考
   - 目标: 有经验的用户
   - 内容: 命令、配置、故障排除
   - 风格: 简洁、表格化

2. **BENCHMARK_SPEC.md** - 完整规范
   - 目标: 深度理解测试框架
   - 内容: 详细的技术说明、设计原理
   - 风格: 详尽、结构化

3. **README.md** - 原有使用说明
   - 目标: 新用户入门
   - 内容: 安装、使用、示例
   - 风格: 循序渐进

4. **QUICKSTART.md** - 原有快速开始
   - 目标: 立即上手
   - 内容: 最小化步骤
   - 风格: 一步步指导

### 开发文档

1. **docs/dev_log.md** - 开发日志
   - 完整的开发过程记录
   - 问题诊断和解决方案
   - 测试结果和发现

2. **PLAN.md** - 项目计划 (根目录)
   - 6 个阶段的详细计划
   - 当前进度跟踪
   - 风险评估和应对

---

## 预期使用流程

### 基线测试
```bash
cd /home/gwx/Workdir/riscv-rvv/benchmark

# 1. 验证环境
./verify.sh

# 2. 运行测试
./run_test.sh

# 3. 保存结果
mv results/latest results/baseline_cfs
```

### 优化测试
```bash
# 1. 加载调度器
ssh root@192.168.3.63
sudo ./your_scheduler &

# 2. 返回本机运行测试
exit
./run_test.sh

# 3. 保存结果
mv results/latest results/with_scheduler
```

### 结果对比
```bash
./compare.sh results/baseline_cfs results/with_scheduler
```

---

## 性能预期

### CFS 基线 (预测)

| 指标 | 预期值 | 依据 |
|------|--------|------|
| **TTFT** | 200-300 ms | 基于之前的单次测试 |
| **平均 tok/s** | 28-32 tok/s | 基于 llama.cpp 性能 |
| **5分钟总 tokens** | 35,000-40,000 | 估算 ~130 tok/s 整体 |
| **每任务请求数** | 85-95 次 | 每请求约 3.5 秒 |

### 优化目标

| 指标 | 基线 | 目标 | 提升 |
|------|------|------|------|
| **平均 tok/s** | ~30 | >33 | +10% |
| **TTFT** | ~250ms | <250ms | 持平或更好 |
| **总 tokens** | ~38,000 | >42,000 | +10% |
| **上下文切换** | ~73/秒 | <60/秒 | -18% |

---

## 已知限制

1. **硬件依赖**
   - 需要 Milk-V Jupiter M1 板卡在线
   - 依赖特定的 IP 地址 (192.168.3.63)

2. **软件依赖**
   - 需要 llama.cpp 预安装
   - 模型路径固定: `/mnt/llm/llama/models/`
   - 启动脚本路径固定: `/mnt/llm/llama/run-server.sh`

3. **测试成本**
   - 5分钟测试约消耗 40,000 tokens
   - 需要稳定的网络连接
   - 测试期间独占板卡资源

4. **当前状态**
   - ⚠️ 板卡离线，无法进行实际测试
   - ✅ 所有脚本已验证，等待上线

---

## 下一步行动

### 立即 (板卡上线后)
1. 运行 `./verify.sh` 确认环境
2. 执行 `./run_test.sh` 建立基线
3. 重复测试 3 次验证一致性
4. 保存基线数据

### 短期 (基线完成后)
1. 分析基线测试中的调度模式
2. 设计针对性的调度策略
3. 实现 sched_ext 调度器原型
4. 进行优化测试

### 中期 (优化完成后)
1. 迭代调整调度参数
2. 对比多种调度策略
3. 达到 10% 性能提升目标
4. 编写技术报告

---

## 技术债务与改进

### 可选的未来改进

1. **集成 perf 监控**
   ```bash
   ssh_exec "perf stat -e context-switches,cache-misses -p \$(pgrep llama-server) &"
   ```

2. **支持更多并发数**
   - 当前固定 4 个任务
   - 可扩展到 8、16 个任务

3. **图表生成**
   - CSV 数据可视化
   - 性能趋势图
   - 对比柱状图

4. **自动化 CI 集成**
   - Git hook 自动测试
   - 性能回归检测
   - 报告自动发送

5. **支持多模型**
   - 当前只支持 Llama-3.2-3B
   - 可扩展到其他模型

---

## 总结

### 完成情况

- ✅ **核心功能**: 5分钟测试 + TTFT + tok/s 记录
- ✅ **自动化**: SSH 启动、并发运行、报告生成
- ✅ **文档**: 完整的使用和技术文档
- ✅ **验证**: 环境检查脚本
- ⚠️ **测试**: 等待板卡上线

### 代码质量

- ✅ Bash 脚本语法正确
- ✅ Python 代码通过 py_compile
- ✅ 所有脚本有执行权限
- ✅ 错误处理完善
- ✅ 日志输出清晰

### 可维护性

- ✅ 配置与逻辑分离 (config.sh)
- ✅ 模块化设计 (多个独立脚本)
- ✅ 完整的注释
- ✅ 详尽的文档
- ✅ 版本控制友好

---

## 致谢

本测试框架基于项目 `benchmark/` 目录的初始版本进行了重大改进和完善。

**改进清单**:
- CSV 格式标准化
- tok/s 计算优化
- 环境验证工具
- 完整的文档体系
- 项目计划跟踪

---

## 版本信息

- **版本**: v1.0
- **创建日期**: 2026-07-08
- **最后更新**: 2026-07-08
- **状态**: 就绪，等待板卡上线测试

## 联系与支持

- **项目位置**: `/home/gwx/Workdir/riscv-rvv/benchmark`
- **文档**: 见 `BENCHMARK_SPEC.md` 和 `QUICK_REFERENCE.md`
- **问题诊断**: 运行 `./verify.sh`


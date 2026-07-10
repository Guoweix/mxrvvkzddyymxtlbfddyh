# 标准化Benchmark测试脚本 - 快速参考

## 一句话总结

基于llama.cpp的5分钟LLM推理性能测试脚本，支持8线程、4并发、自动记录TTFT和tok/s。

## 快速开始

```bash
cd /home/gwx/Workdir/riscv-rvv/benchmark

# 1. 验证环境
./verify.sh

# 2. 运行测试（5分钟）
./run_test.sh

# 3. 查看结果
cat results/latest/report.txt
```

## 测试配置

| 参数 | 值 | 说明 |
|------|-----|------|
| **板卡IP** | 192.168.3.63 | SSH启动llama-server |
| **计算线程** | 8 | 匹配8核CPU |
| **并行槽位** | 4 | 同时处理4个对话 |
| **测试时长** | 300秒 | 5分钟长时间测试 |
| **并发任务** | 4个 | 不同类型prompt |

## 4个测试任务

1. **Task 1**: 技术解释（RVV详解）
2. **Task 2**: 代码生成（二叉搜索树）
3. **Task 3**: 数学计算（平均速度）
4. **Task 4**: 创意写作（AI自我意识）

## 记录的指标

### 每个任务
- ✅ 首字延迟 (TTFT, ms)
- ✅ 平均 tok/s
- ✅ 最小/最大 tok/s
- ✅ 总请求数
- ✅ 总token数

### 整体
- ✅ 5分钟总吞吐量
- ✅ 平均每请求时间
- ✅ 请求完成数

## 输出文件

```
results/YYYYMMDD_HHMMSS/
├── config.txt           # 测试配置
├── task_1.csv           # 任务1详细数据（带头部）
├── task_1.summary       # 任务1总结
├── task_2.csv
├── task_2.summary
├── task_3.csv
├── task_3.summary
├── task_4.csv
├── task_4.summary
├── report.txt           # 文本报告
└── report.json          # JSON报告
```

## CSV格式

```csv
timestamp,request_num,tokens,generation_ms,total_ms,toks_per_sec
1720447890.123,1,100,3250.5,3500.2,30.77
1720447893.456,2,98,3180.2,3420.1,30.82
```

## 对比测试

```bash
# 基线测试
./run_test.sh
mv results/latest results/baseline_cfs

# 启用调度器后
./run_test.sh
mv results/latest results/with_scx

# 对比
./compare.sh results/baseline_cfs results/with_scx
```

## 预期性能（CFS基线）

| 指标 | 预期范围 |
|------|----------|
| **TTFT** | 200-300 ms |
| **平均tok/s** | 28-32 tok/s |
| **5分钟总tokens** | 35,000-40,000 |
| **每任务请求数** | 85-95 次 |

## 故障排除

### 板卡离线
```bash
ping 192.168.3.63
ssh root@192.168.3.63  # 密码: bianbu
```

### SSH失败
检查 `config.sh` 中的 `REMOTE_PASS`

### llama-server启动失败
```bash
ssh root@192.168.3.63
cd /mnt/llm/llama
./run-server.sh
tail -f /tmp/llama-server.log
```

### 性能异常
- 检查是否有其他进程在运行
- 检查内存是否充足
- 查看 `logs/benchmark_*.log`

## 修改配置

编辑 `config.sh`:

```bash
export TEST_DURATION=600      # 改为10分钟
export LLM_THREADS=16         # 改为16线程
export CONCURRENT_TASKS=8     # 改为8并发
```

## 关键文件

| 文件 | 作用 |
|------|------|
| `config.sh` | 配置参数 |
| `benchmark.sh` | 主测试脚本 |
| `generate_report.py` | 报告生成器 |
| `verify.sh` | 环境验证 |
| `run_test.sh` | 一键运行 |
| `compare.sh` | 结果对比 |
| `BENCHMARK_SPEC.md` | 完整规范文档 |

## 技术细节

### TTFT记录
从llama.cpp的 `timings.prompt_ms` 字段获取，仅记录每个任务的第一个请求。

### tok/s计算
每个请求在bash中计算：`tokens / (generation_ms / 1000)`，直接写入CSV。

### 并发实现
使用bash后台进程 `&`，4个任务并行运行5分钟，互不干扰。

### SSH自动化
使用 `sshpass` 免交互SSH，自动启动/停止远程llama-server。

## 已知限制

- ⚠️ 需要板卡在线 (192.168.3.63)
- ⚠️ 需要llama-server已部署在板卡
- ⚠️ 模型路径: `/mnt/llm/llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf`
- ⚠️ 5分钟测试约消耗40,000 tokens

## 版本信息

- **创建日期**: 2026-07-08
- **版本**: v1.0
- **依赖**: sshpass, curl, jq, bc, python3
- **测试环境**: Milk-V Jupiter M1, Linux 6.6.63 + sched_ext

## 更多信息

详细文档见 `BENCHMARK_SPEC.md`

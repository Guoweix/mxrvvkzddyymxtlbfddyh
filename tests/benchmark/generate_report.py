#!/usr/bin/env python3
"""
RVV LLM Benchmark - 报告生成器
生成详细的测试报告，包括图表和统计分析
"""

import sys
import json
import csv
import math
from pathlib import Path
from datetime import datetime
from collections import defaultdict

def parse_float(value, default=0.0):
    if value in (None, "", "null"):
        return default
    try:
        return float(value)
    except ValueError:
        return default

def parse_int(value, default=0):
    if value in (None, "", "null"):
        return default
    try:
        return int(float(value))
    except ValueError:
        return default

def percentile_ceil(values, percentile):
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = max(0, math.ceil(percentile * len(ordered)) - 1)
    return ordered[idx]

def load_task_data(result_dir, task_id):
    """加载单个任务的数据"""
    csv_file = result_dir / f"task{task_id}.csv"
    summary_file = result_dir / f"task{task_id}.summary"
    if not csv_file.exists():
        csv_file = result_dir / f"task_{task_id}.csv"
    if not summary_file.exists():
        summary_file = result_dir / f"task_{task_id}.summary"

    data = {
        'task_id': task_id,
        'requests': [],
        'summary': {}
    }

    # 读取CSV数据
    if csv_file.exists():
        with open(csv_file, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                data['requests'].append({
                    'timestamp': parse_float(row.get('timestamp')),
                    'request_num': parse_int(row.get('request_num')),
                    'tokens': parse_int(row.get('tokens')),
                    'generation_ms': parse_float(row.get('generation_ms')),
                    'ttft_ms': None if row.get('ttft_ms') in (None, "", "null") else parse_float(row.get('ttft_ms')),
                    'last_token_ms': None if row.get('last_token_ms') in (None, "", "null") else parse_float(row.get('last_token_ms')),
                    'total_ms': parse_float(row.get('total_ms')),
                    'toks_per_sec': parse_float(row.get('toks_per_sec')),  # 支持新旧格式
                    'completed': parse_int(row.get('completed'), 1),
                    'stream_cutoff': parse_int(row.get('stream_cutoff')),
                })

    # 读取总结
    if summary_file.exists():
        with open(summary_file, 'r') as f:
            for line in f:
                if ':' in line:
                    key, value = line.strip().split(':', 1)
                    data['summary'][key.strip()] = value.strip()

    return data

def calculate_statistics(data):
    """计算统计数据"""
    if not data['requests']:
        return {}

    tokens_list = [r['tokens'] for r in data['requests']]
    gen_times = [r['generation_ms'] for r in data['requests']]
    total_times = [r['total_ms'] for r in data['requests']]
    ttfts = [r['ttft_ms'] for r in data['requests'] if r.get('ttft_ms') is not None]

    # 使用预计算的tok/s（如果有），否则自己计算
    toks_per_sec = []
    for r in data['requests']:
        if 'toks_per_sec' in r and r['toks_per_sec'] > 0:
            toks_per_sec.append(r['toks_per_sec'])
        elif r['generation_ms'] > 0:
            toks_per_sec.append(r['tokens'] / r['generation_ms'] * 1000)
        else:
            toks_per_sec.append(0)

    stats = {
        'total_requests': len(data['requests']),
        'total_tokens': sum(tokens_list),
        'avg_tokens_per_req': sum(tokens_list) / len(tokens_list),
        'avg_gen_time_ms': sum(gen_times) / len(gen_times),
        'avg_total_time_ms': sum(total_times) / len(total_times),
        'avg_toks_per_sec': sum(toks_per_sec) / len(toks_per_sec),
        'min_toks_per_sec': min(toks_per_sec),
        'max_toks_per_sec': max(toks_per_sec),
        'avg_ttft_ms': sum(ttfts) / len(ttfts) if ttfts else 0.0,
        'p95_ttft_ms': percentile_ceil(ttfts, 0.95),
        'ttft_samples': len(ttfts),
        'completed_requests': sum(1 for r in data['requests'] if r.get('completed') == 1),
        'stream_cutoffs': sum(1 for r in data['requests'] if r.get('stream_cutoff') == 1),
        'first_token_time': data['summary'].get('First Token Time', 'N/A')
    }

    return stats

def generate_text_report(result_dir, all_data):
    """生成文本报告"""
    report_file = result_dir / "report.txt"

    with open(report_file, 'w') as f:
        f.write("=" * 80 + "\n")
        f.write("RVV LLM BENCHMARK REPORT\n")
        f.write("=" * 80 + "\n\n")

        # 读取配置
        config_file = result_dir / "config.txt"
        if config_file.exists():
            f.write("Configuration:\n")
            f.write("-" * 80 + "\n")
            with open(config_file, 'r') as cf:
                f.write(cf.read())
            f.write("\n")

        # 整体统计
        f.write("Overall Statistics:\n")
        f.write("-" * 80 + "\n")

        total_requests = sum(len(d['requests']) for d in all_data)
        total_tokens = sum(sum(r['tokens'] for r in d['requests']) for d in all_data)
        all_ttfts = [
            r['ttft_ms']
            for d in all_data
            for r in d['requests']
            if r.get('ttft_ms') is not None
        ]
        completed_requests = sum(
            1 for d in all_data for r in d['requests'] if r.get('completed') == 1
        )
        stream_cutoffs = sum(
            1 for d in all_data for r in d['requests'] if r.get('stream_cutoff') == 1
        )

        # 优先从summary中获取真实 token 时间范围；旧结果回退到 Duration。
        test_duration = 0
        starts = []
        lasts = []
        for d in all_data:
            starts.append(parse_float(d['summary'].get('Start Timestamp'), None))
            lasts.append(parse_float(d['summary'].get('Last Token Timestamp'), None))
            duration_str = d['summary'].get('Duration', '0s')
            duration = float(duration_str.replace('s', ''))
            test_duration = max(test_duration, duration)
        starts = [x for x in starts if x is not None]
        lasts = [x for x in lasts if x is not None]
        if starts and lasts:
            test_duration = max(lasts) - min(starts)

        overall_toks = total_tokens / test_duration if test_duration > 0 else 0

        f.write(f"Total Requests:    {total_requests}\n")
        f.write(f"Total Tokens:      {total_tokens}\n")
        f.write(f"Test Duration:     {test_duration:.2f}s\n")
        f.write(f"Overall Throughput: {overall_toks:.2f} tok/s\n")
        f.write(f"Avg TTFT:          {(sum(all_ttfts) / len(all_ttfts) if all_ttfts else 0):.2f}ms\n")
        f.write(f"p95 TTFT:          {percentile_ceil(all_ttfts, 0.95):.2f}ms\n")
        f.write(f"TTFT Samples:      {len(all_ttfts)}\n")
        f.write(f"Completed Requests: {completed_requests}\n")
        f.write(f"Stream Cutoffs:    {stream_cutoffs}\n")
        f.write("\n")

        # 每个任务的详细统计
        f.write("Per-Task Statistics:\n")
        f.write("=" * 80 + "\n\n")

        for data in all_data:
            stats = calculate_statistics(data)
            task_id = data['task_id']

            f.write(f"Task {task_id}:\n")
            f.write("-" * 80 + "\n")

            prompt = data['summary'].get('Prompt', 'N/A')
            f.write(f"Prompt:            {prompt}\n")
            f.write(f"First Token Time:  {stats['first_token_time']}\n")
            f.write(f"Avg TTFT:          {stats['avg_ttft_ms']:.2f}ms\n")
            f.write(f"p95 TTFT:          {stats['p95_ttft_ms']:.2f}ms\n")
            f.write(f"TTFT Samples:      {stats['ttft_samples']}\n")
            f.write(f"Total Requests:    {stats['total_requests']}\n")
            f.write(f"Completed Requests: {stats['completed_requests']}\n")
            f.write(f"Stream Cutoffs:    {stats['stream_cutoffs']}\n")
            f.write(f"Total Tokens:      {stats['total_tokens']}\n")
            f.write(f"Avg Tokens/Req:    {stats['avg_tokens_per_req']:.2f}\n")
            f.write(f"Avg Gen Time:      {stats['avg_gen_time_ms']:.2f}ms\n")
            f.write(f"Avg Total Time:    {stats['avg_total_time_ms']:.2f}ms\n")
            f.write(f"Avg tok/s:         {stats['avg_toks_per_sec']:.2f}\n")
            f.write(f"Min tok/s:         {stats['min_toks_per_sec']:.2f}\n")
            f.write(f"Max tok/s:         {stats['max_toks_per_sec']:.2f}\n")
            f.write("\n")

        # 读取CPU使用情况
        cpu_log = result_dir / "cpu_usage.log"
        if cpu_log.exists():
            f.write("CPU Usage During Test:\n")
            f.write("-" * 80 + "\n")
            with open(cpu_log, 'r') as cf:
                lines = cf.readlines()
                if lines:
                    f.write("First sample:\n")
                    f.write(lines[0])
                    if len(lines) > 1:
                        f.write("\nLast sample:\n")
                        f.write(lines[-1])
            f.write("\n")

    print(f"✓ 文本报告已生成: {report_file}")
    return report_file

def generate_json_report(result_dir, all_data):
    """生成JSON格式的报告"""
    report_file = result_dir / "report.json"

    report = {
        'timestamp': datetime.now().isoformat(),
        'tasks': []
    }

    for data in all_data:
        stats = calculate_statistics(data)
        task_report = {
            'task_id': data['task_id'],
            'summary': data['summary'],
            'statistics': stats,
            'requests': data['requests']
        }
        report['tasks'].append(task_report)

    with open(report_file, 'w') as f:
        json.dump(report, f, indent=2)

    print(f"✓ JSON报告已生成: {report_file}")
    return report_file

def main():
    if len(sys.argv) < 2:
        print("Usage: generate_report.py <result_directory>")
        sys.exit(1)

    result_dir = Path(sys.argv[1])
    if not result_dir.exists():
        print(f"Error: Directory not found: {result_dir}")
        sys.exit(1)

    print(f"分析结果目录: {result_dir}")

    # 加载所有任务数据
    all_data = []
    for task_id in range(1, 5):  # 4个任务
        data = load_task_data(result_dir, task_id)
        if data['requests']:
            all_data.append(data)
            print(f"✓ 加载 Task {task_id}: {len(data['requests'])} 个请求")

    if not all_data:
        print("Error: No data found")
        sys.exit(1)

    # 生成报告
    print("\n生成报告...")
    text_report = generate_text_report(result_dir, all_data)
    json_report = generate_json_report(result_dir, all_data)

    print("\n" + "=" * 80)
    print("报告生成完成!")
    print("=" * 80)
    print(f"文本报告: {text_report}")
    print(f"JSON报告: {json_report}")

    # 显示摘要
    print("\n快速摘要:")
    print("-" * 80)
    for data in all_data:
        stats = calculate_statistics(data)
        print(f"Task {data['task_id']}: {stats['avg_toks_per_sec']:.2f} tok/s "
              f"(avg TTFT: {stats['avg_ttft_ms']:.2f}ms, "
              f"p95 TTFT: {stats['p95_ttft_ms']:.2f}ms)")

if __name__ == '__main__':
    main()

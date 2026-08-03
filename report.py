#!/usr/bin/env python3
"""Summarize native one-minute Codex Monitor history without dependencies."""

from __future__ import annotations

import argparse
import csv
import glob
import math
import os
import statistics
import tempfile
import time
from collections import Counter
from datetime import datetime


NUMERIC_FIELDS = (
    "system_cpu_avg", "system_cpu_max", "codex_cpu_avg", "codex_cpu_max",
    "codex_memory_gib_avg", "codex_memory_gib_max", "codex_memory_pct_avg",
    "codex_memory_pct_max", "codex_processes_avg", "codex_processes_max",
    "codex_largest_gib_max", "memory_pressure_worst", "swap_used_gib",
    "swap_delta_10m_mib", "thermal_worst", "network_down_mbps_avg",
    "network_down_mbps_max", "network_up_mbps_avg", "network_up_mbps_max",
    "codex_disk_read_mbps_avg", "codex_disk_write_mbps_avg",
)


def fmt(value: float, decimals: int = 1) -> str:
    return "无数据" if math.isnan(value) else f"{value:.{decimals}f}"


def percentile(items: list[float], fraction: float) -> float:
    values = sorted(items)
    if not values:
        return math.nan
    position = (len(values) - 1) * fraction
    lower, upper = math.floor(position), math.ceil(position)
    if lower == upper:
        return values[lower]
    return values[lower] + (values[upper] - values[lower]) * (position - lower)


def read_rows(data_dir: str, cutoff: int) -> list[dict[str, float | str]]:
    rows: list[dict[str, float | str]] = []
    for path in sorted(glob.glob(os.path.join(data_dir, "*.csv"))):
        with open(path, newline="", encoding="utf-8") as handle:
            for raw in csv.DictReader(handle):
                try:
                    epoch = float(raw["epoch"])
                except (KeyError, TypeError, ValueError):
                    continue
                if epoch < cutoff or "system_cpu_avg" not in raw:
                    continue
                row: dict[str, float | str] = {
                    "timestamp": raw.get("timestamp", ""), "epoch": epoch,
                    "bottleneck": raw.get("bottleneck", "unknown"),
                }
                for field in NUMERIC_FIELDS:
                    try:
                        row[field] = float(raw.get(field, ""))
                    except (TypeError, ValueError):
                        row[field] = math.nan
                rows.append(row)
    return sorted(rows, key=lambda item: float(item["epoch"]))


def read_events(data_dir: str, cutoff: int) -> list[dict[str, float | str]]:
    path = os.path.join(data_dir, "events.csv")
    if not os.path.exists(path):
        return []
    events: list[dict[str, float | str]] = []
    with open(path, newline="", encoding="utf-8") as handle:
        for raw in csv.DictReader(handle):
            try:
                epoch = float(raw["epoch"])
            except (KeyError, TypeError, ValueError):
                continue
            if epoch < cutoff:
                continue
            events.append({
                "timestamp": raw.get("timestamp", ""), "epoch": epoch,
                "event": raw.get("event", "unknown"),
                "instance_id": raw.get("instance_id", "unknown"),
            })
    return sorted(events, key=lambda item: float(item["epoch"]))


def values(rows: list[dict[str, float | str]], field: str) -> list[float]:
    result = [float(row[field]) for row in rows]
    return [value for value in result if not math.isnan(value)]


def main() -> int:
    parser = argparse.ArgumentParser(description="汇总最近一段时间的整机与 Codex 资源趋势")
    parser.add_argument("--hours", type=float, default=24.0, help="回看小时数，默认 24")
    parser.add_argument(
        "--data-dir",
        default=os.path.expanduser("~/Library/Application Support/CodexSystemMonitor/native-history"),
        help="原生一分钟历史目录；默认读取当前用户的应用支持目录",
    )
    parser.add_argument("--self-test", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.self_test:
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "events.csv")
            with open(path, "w", newline="", encoding="utf-8") as handle:
                writer = csv.writer(handle)
                writer.writerow(("timestamp", "epoch", "event", "instance_id"))
                writer.writerow(("2026-01-01 00:00:00", 100, "start", "one"))
                writer.writerow(("2026-01-01 00:10:00", 700, "wake", "one"))
                writer.writerow(("2026-01-01 00:20:00", 1300, "terminate", "one"))
                writer.writerow(("2026-01-01 00:21:00", 1360, "restart", "two"))
            passed = [event["event"] for event in read_events(directory, 500)] == ["wake", "terminate", "restart"]
        print(f"lifecycle_event_test={'pass' if passed else 'fail'}")
        return 0 if passed else 6
    cutoff = int(time.time() - args.hours * 3600)
    rows = read_rows(args.data_dir, cutoff)
    events = read_events(args.data_dir, cutoff)
    if not rows:
        print("没有找到指定时间范围内的原生一分钟趋势记录。")
        return 1

    epochs = values(rows, "epoch")
    gaps = [later - earlier for earlier, later in zip(epochs, epochs[1:])]
    gap_windows = [(earlier, later) for earlier, later in zip(epochs, epochs[1:]) if later - earlier > 120]
    explained_gaps = sum(
        any(earlier - 60 <= float(event["epoch"]) <= later + 60 for event in events)
        for earlier, later in gap_windows
    )
    start = datetime.fromtimestamp(epochs[0]).astimezone().strftime("%Y-%m-%d %H:%M:%S %z")
    end = datetime.fromtimestamp(epochs[-1]).astimezone().strftime("%Y-%m-%d %H:%M:%S %z")
    coverage_hours = (epochs[-1] - epochs[0]) / 3600 if len(epochs) > 1 else 0
    expected = max(1, round(coverage_hours * 60) + 1)

    system_avg = values(rows, "system_cpu_avg")
    system_max = values(rows, "system_cpu_max")
    codex_avg = values(rows, "codex_cpu_avg")
    codex_max = values(rows, "codex_cpu_max")
    memory_avg = values(rows, "codex_memory_gib_avg")
    memory_max = values(rows, "codex_memory_gib_max")
    memory_pct = values(rows, "codex_memory_pct_max")
    pressure = values(rows, "memory_pressure_worst")
    swap = values(rows, "swap_used_gib")
    swap_delta = values(rows, "swap_delta_10m_mib")
    thermal = values(rows, "thermal_worst")
    processes = values(rows, "codex_processes_max")
    bottlenecks = Counter(str(row["bottleneck"]) for row in rows)
    labels = {
        "normal": "正常", "cpu_high": "CPU较高", "cpu_codex": "CPU吃紧且Codex占比较高",
        "cpu_other": "CPU吃紧且主要是其他程序", "memory_warning": "内存注意",
        "memory_critical": "内存严重", "thermal": "温度限制性能",
    }

    print(f"# 最近 {args.hours:g} 小时电脑与 Codex 趋势\n")
    print(f"- 实际覆盖：{start} 至 {end}（{coverage_hours:.2f} 小时）")
    print(f"- 一分钟摘要：{len(rows)} 条；完整度约 {min(100, len(rows) / expected * 100):.1f}%")
    print(f"- 超过 2 分钟的缺口：{sum(gap > 120 for gap in gaps)} 个\n")

    if events:
        event_counts = Counter(str(event["event"]) for event in events)
        instance_count = len({str(event["instance_id"]) for event in events})
        print("## 运行连续性\n")
        print(f"- 实例 {instance_count} 个；启动 {event_counts['start']} 次；异常结束或系统重启后的再启动 {event_counts['restart']} 次；唤醒 {event_counts['wake']} 次；正常退出 {event_counts['terminate']} 次")
        print(f"- 有启动或唤醒标记可解释的长缺口：{explained_gaps}/{len(gap_windows)} 个\n")
    else:
        print("## 运行连续性\n")
        print("- 当前时间范围没有生命周期标记；旧版历史无法区分睡眠、重启与意外中断。\n")

    print("## 一眼结论\n")
    print("- 状态分钟数：" + "；".join(f"{labels.get(code, code)} {count}" for code, count in bottlenecks.most_common()))
    print(f"- 内存压力告警：{sum(item >= 1 for item in pressure)} 分钟；严重 {sum(item >= 2 for item in pressure)} 分钟")
    print(f"- 温度进入较高或严重：{sum(item >= 2 for item in thermal)} 分钟\n")

    print("## 整机与 Codex\n")
    print(f"- 整机 CPU：一分钟平均的中位数 {fmt(statistics.median(system_avg))}%；最高 5 秒采样 {fmt(max(system_max))}%")
    print(f"- Codex CPU：一分钟平均的中位数 {fmt(statistics.median(codex_avg))}%；第 95 百分位 {fmt(percentile(codex_max, .95))}%；峰值 {fmt(max(codex_max))}%")
    print(f"- Codex 常驻内存：平均中位数 {fmt(statistics.median(memory_avg), 2)} GiB；峰值 {fmt(max(memory_max), 2)} GiB；占整机最高 {fmt(max(memory_pct))}%")
    print(f"- Codex 进程数峰值：{fmt(max(processes), 0)}")
    print(f"- Swap：最高 {fmt(max(swap), 2)} GiB；十分钟最大增加 {fmt(max(swap_delta), 0)} MiB\n")

    print("## 判读边界\n")
    print("- CPU 和内存用于判断趋势与瓶颈，不是记账级绝对值；Codex 内存采用 macOS 原生常驻内存口径。")
    print("- CPU 百分比按整机总算力归一化，100% 表示整台电脑的逻辑处理器都接近占满。")
    print("- 这份性能历史不保存账号额度或Token用量，因此报告不能统计套餐额度、模型名称或单个任务成本。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

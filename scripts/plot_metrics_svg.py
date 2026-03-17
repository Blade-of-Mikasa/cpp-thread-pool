#!/usr/bin/env python3
"""
Generate simple SVG charts from thread_pool CSV exports.

Inputs (default filenames):
  - metrics_time_series.csv
  - metrics_wait_histogram.csv
  - metrics_exec_histogram.csv

Outputs (default directory):
  - docs/metrics_time_series.svg
  - docs/metrics_wait_histogram.svg
  - docs/metrics_exec_histogram.svg
"""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class TimePoint:
    uptime_ms: float
    threads: float
    busy_threads: float
    pending_tasks: float
    throughput_per_sec: float


def _read_time_series(path: Path) -> list[TimePoint]:
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        points: list[TimePoint] = []
        for row in reader:
            points.append(
                TimePoint(
                    uptime_ms=float(row["uptime_ms"]),
                    threads=float(row["threads"]),
                    busy_threads=float(row["busy_threads"]),
                    pending_tasks=float(row["pending_tasks"]),
                    throughput_per_sec=float(row["throughput_per_sec"]),
                )
            )
        return points


def _read_histogram(path: Path) -> list[tuple[str, int]]:
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        rows: list[tuple[str, int]] = []
        for row in reader:
            upper = row["upper_bound_ns"].strip()
            count = int(row["count"])
            rows.append((upper, count))
        return rows


def _fmt_ns_label(ns_text: str) -> str:
    if ns_text == "inf":
        return ">5s"

    ns = int(ns_text)
    if ns >= 1_000_000_000 and ns % 1_000_000_000 == 0:
        return f"{ns // 1_000_000_000}s"
    if ns >= 1_000_000 and ns % 1_000_000 == 0:
        return f"{ns // 1_000_000}ms"
    if ns >= 1_000 and ns % 1_000 == 0:
        return f"{ns // 1_000}us"
    return f"{ns}ns"


def _ticks(max_value: float, tick_count: int = 5) -> list[float]:
    if max_value <= 0:
        return [0.0]
    raw_step = max_value / tick_count
    magnitude = 10 ** math.floor(math.log10(raw_step))
    step = math.ceil(raw_step / magnitude) * magnitude
    return [i * step for i in range(tick_count + 1)]


def _polyline(points: Iterable[tuple[float, float]]) -> str:
    return " ".join(f"{x:.2f},{y:.2f}" for x, y in points)


def _svg_header(width: int, height: int) -> list[str]:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        '<style>',
        "  .axis{stroke:#111;stroke-width:1;fill:none}",
        "  .grid{stroke:#ddd;stroke-width:1;fill:none}",
        "  .label{font-family:ui-sans-serif,system-ui,Segoe UI,Arial;font-size:12px;fill:#111}",
        "  .title{font-family:ui-sans-serif,system-ui,Segoe UI,Arial;font-size:16px;font-weight:600;fill:#111}",
        "</style>",
    ]


def _svg_footer() -> str:
    return "</svg>"


def write_time_series_svg(points: list[TimePoint], out_path: Path) -> None:
    if not points:
        raise SystemExit("time series CSV is empty")

    width = 1000
    height = 640
    margin_left = 70
    margin_right = 20
    margin_top = 50
    margin_bottom = 40
    gap = 40

    plot_h = (height - margin_top - margin_bottom - gap) / 2.0
    plot_w = width - margin_left - margin_right

    x0 = margin_left
    x1 = margin_left + plot_w

    t_min = min(p.uptime_ms for p in points)
    t_max = max(p.uptime_ms for p in points)
    if t_max <= t_min:
        t_max = t_min + 1.0

    def x_of(t_ms: float) -> float:
        return x0 + (t_ms - t_min) / (t_max - t_min) * plot_w

    # subplot 1: threads vs busy_threads
    threads_max = max(max(p.threads for p in points), max(p.busy_threads for p in points), 1.0)
    y1_top = margin_top
    y1_bottom = margin_top + plot_h

    def y1_of(v: float) -> float:
        return y1_bottom - (v / threads_max) * plot_h

    # subplot 2: pending tasks + throughput (same axis scale is not ideal; keep pending only).
    pending_max = max(max(p.pending_tasks for p in points), 1.0)
    y2_top = y1_bottom + gap
    y2_bottom = y2_top + plot_h

    def y2_of(v: float) -> float:
        return y2_bottom - (v / pending_max) * plot_h

    lines: list[str] = []
    lines += _svg_header(width, height)
    lines.append('<text class="title" x="20" y="28">thread_pool metrics (time series)</text>')

    # x-axis ticks (shared)
    t_ticks = _ticks(t_max - t_min, 5)
    for dt in t_ticks:
        t = t_min + dt
        x = x_of(t)
        lines.append(f'<line class="grid" x1="{x:.2f}" y1="{y1_top:.2f}" x2="{x:.2f}" y2="{y2_bottom:.2f}"/>')
        lines.append(f'<text class="label" x="{x:.2f}" y="{(y2_bottom + 18):.2f}" text-anchor="middle">{dt:.0f}</text>')
    lines.append(
        f'<text class="label" x="{((x0 + x1) / 2):.2f}" y="{(height - 10):.2f}" text-anchor="middle">uptime (ms)</text>'
    )

    # y grids + labels for subplot 1
    for v in _ticks(threads_max, 4):
        y = y1_of(v)
        lines.append(f'<line class="grid" x1="{x0:.2f}" y1="{y:.2f}" x2="{x1:.2f}" y2="{y:.2f}"/>')
        lines.append(f'<text class="label" x="{(x0 - 8):.2f}" y="{(y + 4):.2f}" text-anchor="end">{v:.0f}</text>')
    lines.append(f'<text class="label" x="{x0:.2f}" y="{(y1_top - 12):.2f}">threads / busy</text>')

    # y grids + labels for subplot 2
    for v in _ticks(pending_max, 4):
        y = y2_of(v)
        lines.append(f'<line class="grid" x1="{x0:.2f}" y1="{y:.2f}" x2="{x1:.2f}" y2="{y:.2f}"/>')
        lines.append(f'<text class="label" x="{(x0 - 8):.2f}" y="{(y + 4):.2f}" text-anchor="end">{v:.0f}</text>')
    lines.append(f'<text class="label" x="{x0:.2f}" y="{(y2_top - 12):.2f}">pending tasks</text>')

    # axes
    lines.append(f'<rect class="axis" x="{x0:.2f}" y="{y1_top:.2f}" width="{plot_w:.2f}" height="{plot_h:.2f}"/>')
    lines.append(f'<rect class="axis" x="{x0:.2f}" y="{y2_top:.2f}" width="{plot_w:.2f}" height="{plot_h:.2f}"/>')

    # series polylines
    threads_points = [(x_of(p.uptime_ms), y1_of(p.threads)) for p in points]
    busy_points = [(x_of(p.uptime_ms), y1_of(p.busy_threads)) for p in points]
    pending_points = [(x_of(p.uptime_ms), y2_of(p.pending_tasks)) for p in points]

    lines.append(f'<polyline fill="none" stroke="#2563eb" stroke-width="2" points="{_polyline(threads_points)}"/>')
    lines.append(f'<polyline fill="none" stroke="#dc2626" stroke-width="2" points="{_polyline(busy_points)}"/>')
    lines.append(f'<polyline fill="none" stroke="#16a34a" stroke-width="2" points="{_polyline(pending_points)}"/>')

    # legend
    lx = x1 - 180
    ly = y1_top + 18
    lines.append(f'<rect x="{lx:.2f}" y="{(ly - 14):.2f}" width="170" height="62" fill="white" stroke="#ddd"/>')
    lines.append(f'<line x1="{(lx + 10):.2f}" y1="{ly:.2f}" x2="{(lx + 35):.2f}" y2="{ly:.2f}" stroke="#2563eb" stroke-width="3"/>')
    lines.append(f'<text class="label" x="{(lx + 45):.2f}" y="{(ly + 4):.2f}">threads</text>')
    ly += 20
    lines.append(f'<line x1="{(lx + 10):.2f}" y1="{ly:.2f}" x2="{(lx + 35):.2f}" y2="{ly:.2f}" stroke="#dc2626" stroke-width="3"/>')
    lines.append(f'<text class="label" x="{(lx + 45):.2f}" y="{(ly + 4):.2f}">busy_threads</text>')
    ly += 20
    lines.append(f'<line x1="{(lx + 10):.2f}" y1="{ly:.2f}" x2="{(lx + 35):.2f}" y2="{ly:.2f}" stroke="#16a34a" stroke-width="3"/>')
    lines.append(f'<text class="label" x="{(lx + 45):.2f}" y="{(ly + 4):.2f}">pending_tasks</text>')

    lines.append(_svg_footer())
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")


def write_histogram_svg(hist: list[tuple[str, int]], title: str, out_path: Path) -> None:
    width = 1000
    height = 360
    margin_left = 70
    margin_right = 20
    margin_top = 50
    margin_bottom = 60

    plot_w = width - margin_left - margin_right
    plot_h = height - margin_top - margin_bottom

    x0 = margin_left
    y0 = margin_top
    x1 = x0 + plot_w
    y1 = y0 + plot_h

    labels = [_fmt_ns_label(upper) for upper, _ in hist]
    counts = [c for _, c in hist]
    max_c = max(counts) if counts else 1

    bar_w = plot_w / max(len(hist), 1)

    def y_of(c: float) -> float:
        return y1 - (c / max_c) * plot_h

    lines: list[str] = []
    lines += _svg_header(width, height)
    lines.append(f'<text class="title" x="20" y="28">{title}</text>')

    # grid + y labels
    for v in _ticks(float(max_c), 4):
        y = y_of(v)
        lines.append(f'<line class="grid" x1="{x0:.2f}" y1="{y:.2f}" x2="{x1:.2f}" y2="{y:.2f}"/>')
        lines.append(f'<text class="label" x="{(x0 - 8):.2f}" y="{(y + 4):.2f}" text-anchor="end">{v:.0f}</text>')

    # axes
    lines.append(f'<rect class="axis" x="{x0:.2f}" y="{y0:.2f}" width="{plot_w:.2f}" height="{plot_h:.2f}"/>')

    # bars
    for i, (label, count) in enumerate(zip(labels, counts)):
        x = x0 + i * bar_w + 2
        y = y_of(count)
        h = y1 - y
        lines.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{(bar_w - 4):.2f}" height="{h:.2f}" fill="#4b5563"/>')

        # x label (rotate for density)
        lx = x0 + i * bar_w + bar_w / 2.0
        ly = y1 + 38
        lines.append(
            f'<text class="label" x="{lx:.2f}" y="{ly:.2f}" text-anchor="end" transform="rotate(-45 {lx:.2f} {ly:.2f})">{label}</text>'
        )

    lines.append(_svg_footer())
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--time-series", type=Path, default=Path("metrics_time_series.csv"))
    parser.add_argument("--wait-hist", type=Path, default=Path("metrics_wait_histogram.csv"))
    parser.add_argument("--exec-hist", type=Path, default=Path("metrics_exec_histogram.csv"))
    parser.add_argument("--out-dir", type=Path, default=Path("docs"))
    args = parser.parse_args()

    time_points = _read_time_series(args.time_series)
    wait_hist = _read_histogram(args.wait_hist)
    exec_hist = _read_histogram(args.exec_hist)

    write_time_series_svg(time_points, args.out_dir / "metrics_time_series.svg")
    write_histogram_svg(wait_hist, "Task wait time histogram", args.out_dir / "metrics_wait_histogram.svg")
    write_histogram_svg(exec_hist, "Task execution time histogram", args.out_dir / "metrics_exec_histogram.svg")

    print(f"Wrote SVG charts to: {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Run installed PS1 games against the local emulator and summarize failures.

The goal is triage, not pass/fail certification.  Each game runs headless with
GPU frame stats enabled; the runner extracts the final CPU/GPU state, display
window, obvious error markers, and optional VRAM screenshots.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "tests" / "out" / "game-smoke"


SMOKE_RE = re.compile(
    r"Smoke: ran (?P<instr>\d+) instructions .* PC=0x(?P<pc>[0-9A-Fa-f]+) "
    r"OP=0x(?P<op>[0-9A-Fa-f]+).* CYC=(?P<cycles>\d+) GPU_FRAMES=(?P<frames>\d+) "
    r"I_STAT=0x(?P<istat>[0-9A-Fa-f]+) I_MASK=0x(?P<imask>[0-9A-Fa-f]+)"
)
GPU_FRAME_RE = re.compile(
    r"GPU_FRAME (?P<frame>\d+) writes=(?P<writes>\d+) display_nonzero=(?P<nonzero>\d+) "
    r"display=\((?P<x>\d+),(?P<y>\d+) (?P<w>\d+)x(?P<h>\d+)\) disabled=(?P<disabled>\d+)"
    r"(?: depth=(?P<depth>\d+|15|24) hres=(?P<hres>\d+))?"
)


@dataclass
class GameCase:
    name: str
    disc: str


@dataclass
class GameResult:
    name: str
    disc: str
    status: str
    returncode: int
    elapsed_s: float
    instructions: int | None
    pc: str | None
    op: str | None
    cycles: int | None
    gpu_frames: int | None
    display: str | None
    display_nonzero: int | None
    display_disabled: int | None
    display_depth: str | None
    unhandled_count: int
    error_markers: list[str]
    log: str
    screenshot: str | None
    screenshot_frame: int | None
    trace: str | None


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def discover_games(games_dir: Path) -> list[GameCase]:
    files = sorted(
        p for p in games_dir.rglob("*")
        if p.is_file() and p.suffix.lower() in {".cue", ".bin", ".iso", ".img"}
    )
    cue_dirs = {p.parent for p in files if p.suffix.lower() == ".cue"}
    cases: list[GameCase] = []
    for path in files:
        suffix = path.suffix.lower()
        if suffix == ".cue":
            pass
        elif path.parent in cue_dirs:
            continue
        name = path.parent.name if path.parent != games_dir else path.stem
        cases.append(GameCase(name=name, disc=rel(path)))
    return cases


def safe_name(name: str) -> str:
    out = []
    for ch in name.lower():
        if ch.isalnum():
            out.append(ch)
        elif out and out[-1] != "-":
            out.append("-")
    return "".join(out).strip("-") or "game"


def parse_result(case: GameCase, proc: subprocess.CompletedProcess[str], elapsed: float,
                 log_path: Path, screenshot: Path | None, screenshot_frame: int | None,
                 trace_path: Path | None) -> GameResult:
    text = (proc.stdout or "") + (proc.stderr or "")
    smoke = None
    for match in SMOKE_RE.finditer(text):
        smoke = match

    last_gpu = None
    for match in GPU_FRAME_RE.finditer(text):
        last_gpu = match

    markers: list[str] = []
    for line in text.splitlines():
        lower = line.lower()
        if lower.startswith("smoke: ran ") and "without abort" in lower:
            continue
        if ("unhandled" in lower or "abort" in lower or "failed" in lower or
                "couldn't" in lower or "unknown" in lower or "error" in lower):
            markers.append(line[:240])
        if len(markers) >= 20:
            break

    timeout = proc.returncode == 124
    if timeout:
        status = "TIMEOUT"
    elif proc.returncode != 0:
        status = "CRASH"
    elif not smoke:
        status = "NO_SMOKE_LINE"
    else:
        nonzero = int(last_gpu.group("nonzero")) if last_gpu else None
        disabled = int(last_gpu.group("disabled")) if last_gpu else None
        if disabled:
            status = "DISPLAY_DISABLED"
        elif nonzero == 0:
            status = "BLACK_DISPLAY"
        elif markers:
            status = "RUNS_WITH_WARNINGS"
        else:
            status = "RUNS"

    display = None
    display_nonzero = None
    display_disabled = None
    display_depth = None
    if last_gpu:
        display = (
            f"{last_gpu.group('x')},{last_gpu.group('y')} "
            f"{last_gpu.group('w')}x{last_gpu.group('h')}"
        )
        display_nonzero = int(last_gpu.group("nonzero"))
        display_disabled = int(last_gpu.group("disabled"))
        display_depth = last_gpu.group("depth")

    return GameResult(
        name=case.name,
        disc=case.disc,
        status=status,
        returncode=proc.returncode,
        elapsed_s=round(elapsed, 3),
        instructions=int(smoke.group("instr")) if smoke else None,
        pc=f"0x{smoke.group('pc').upper()}" if smoke else None,
        op=f"0x{smoke.group('op').upper()}" if smoke else None,
        cycles=int(smoke.group("cycles")) if smoke else None,
        gpu_frames=int(smoke.group("frames")) if smoke else None,
        display=display,
        display_nonzero=display_nonzero,
        display_disabled=display_disabled,
        display_depth=display_depth,
        unhandled_count=sum(1 for line in text.splitlines() if "Unhandled" in line),
        error_markers=markers,
        log=rel(log_path),
        screenshot=rel(screenshot) if screenshot and screenshot.exists() else None,
        screenshot_frame=screenshot_frame if screenshot and screenshot.exists() else None,
        trace=rel(trace_path) if trace_path and trace_path.exists() else None,
    )


def convert_ppm_to_png(ppm: Path, png: Path, scale: int) -> None:
    sys.path.insert(0, str(ROOT / "tests"))
    from run_psx_tests import ppm_to_png  # type: ignore
    ppm_to_png(ppm, png, scale)


def run_case(case: GameCase, args: argparse.Namespace) -> GameResult:
    out_dir = args.out / safe_name(case.name)
    out_dir.mkdir(parents=True, exist_ok=True)
    log_path = out_dir / "run.log"
    ppm_path = out_dir / "display.ppm"
    png_path = out_dir / "display.png"
    trace_path = out_dir / "visual_trace.jsonl"
    for old_capture in (ppm_path, png_path, trace_path):
        try:
            old_capture.unlink()
        except FileNotFoundError:
            pass

    env = os.environ.copy()
    env["PS1_GPU_FRAME_STATS"] = str(args.stats_period)
    if args.pad_script:
        env["PS1_PAD_SCRIPT"] = args.pad_script
    if args.pad_held:
        env["PS1_PAD_HELD"] = args.pad_held
    if args.dump_frame and not args.no_screenshot:
        env["PS1_DUMP_VRAM_PPM"] = str(ppm_path)
        env["PS1_DUMP_FRAME"] = str(args.dump_frame)
        if args.full_vram:
            env["PS1_DUMP_FULL_VRAM"] = "1"
    if args.trace_gpu_gte:
        env["PS1_TRACE_VISUAL"] = str(trace_path)
        if args.trace_limit:
            env["PS1_TRACE_VISUAL_LIMIT"] = str(args.trace_limit)
        if args.trace_start_frame:
            env["PS1_TRACE_VISUAL_START_FRAME"] = str(args.trace_start_frame)
        if args.trace_events:
            env["PS1_TRACE_VISUAL_EVENTS"] = args.trace_events

    cmd = [
        str(args.emulator),
        "--bios", str(args.bios),
        "--disc", str(ROOT / case.disc),
        "--headless",
        "--max-instructions", str(args.max_instructions),
    ]

    start = time.monotonic()
    proc = subprocess.run(
        cmd,
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=args.timeout,
    )
    elapsed = time.monotonic() - start
    log_path.write_text((proc.stdout or "") + (proc.stderr or ""), encoding="utf-8")

    screenshot = None
    if ppm_path.exists():
        try:
            convert_ppm_to_png(ppm_path, png_path, args.png_scale)
            screenshot = png_path
        except Exception as exc:  # noqa: BLE001 - keep triage running
            with log_path.open("a", encoding="utf-8") as f:
                f.write(f"\n[runner] failed to convert screenshot: {exc}\n")

    screenshot_frame = args.dump_frame if screenshot else None
    trace = trace_path if args.trace_gpu_gte else None
    return parse_result(case, proc, elapsed, log_path, screenshot, screenshot_frame, trace)


def write_outputs(results: list[GameResult], out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / "summary.json").write_text(
        json.dumps([asdict(r) for r in results], indent=2, ensure_ascii=False),
        encoding="utf-8",
    )

    columns = [
        "status", "name", "pc", "gpu_frames", "display", "display_nonzero",
        "display_depth", "unhandled_count", "elapsed_s", "log", "screenshot", "screenshot_frame", "trace",
    ]
    with (out / "summary.tsv").open("w", encoding="utf-8") as f:
        f.write("\t".join(columns) + "\n")
        for r in results:
            row = [str(getattr(r, c) if getattr(r, c) is not None else "") for c in columns]
            f.write("\t".join(row) + "\n")

    counts: dict[str, int] = {}
    for r in results:
        counts[r.status] = counts.get(r.status, 0) + 1

    lines = ["# Game Smoke Summary", "", "## Counts", ""]
    for status, count in sorted(counts.items()):
        lines.append(f"- {status}: {count}")
    lines.extend(["", "## Results", ""])
    for r in results:
        shot = f" screenshot={r.screenshot} frame={r.screenshot_frame}" if r.screenshot else ""
        trace = f" trace={r.trace}" if r.trace else ""
        lines.append(
            f"- {r.status}: {r.name} pc={r.pc or '-'} frames={r.gpu_frames or '-'} "
            f"display={r.display or '-'} nonzero={r.display_nonzero if r.display_nonzero is not None else '-'}"
            f"{shot}{trace}"
        )
        if r.error_markers:
            lines.append(f"  first_marker={r.error_markers[0]}")
    lines.append("")
    (out / "summary.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--games-dir", type=Path, default=ROOT / "games")
    parser.add_argument("--bios", type=Path, default=ROOT / "bios" / "BIOS.ROM")
    parser.add_argument("--emulator", type=Path, default=ROOT / "ps1_boot")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--name", action="append", default=[], help="substring filter; can be repeated")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--max-instructions", type=int, default=500_000_000)
    parser.add_argument("--stats-period", type=int, default=500)
    parser.add_argument(
        "--dump-frame",
        type=int,
        default=int(os.getenv("GAME_SMOKE_DUMP_FRAME", "2000")),
        help="GPU frame to capture. Default 2000 with the corrected CPU/device clock ratio.",
    )
    parser.add_argument("--no-screenshot", action="store_true", help="Do not capture display.ppm/png")
    parser.add_argument("--full-vram", action="store_true")
    parser.add_argument("--png-scale", type=int, default=2)
    parser.add_argument("--pad-script", default="")
    parser.add_argument("--pad-held", default="")
    parser.add_argument("--trace-gpu-gte", action="store_true", help="Write per-game visual JSONL traces")
    parser.add_argument("--trace-limit", type=int, default=0, help="Maximum visual trace rows per game")
    parser.add_argument("--trace-start-frame", type=int, default=0, help="Skip visual trace rows before this GPU frame")
    parser.add_argument(
        "--trace-events",
        default="",
        help="Comma-separated visual trace events to keep: frame,gp0,gte",
    )
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    cases = discover_games(args.games_dir)
    if args.name:
        needles = [n.lower() for n in args.name]
        cases = [c for c in cases if any(n in c.name.lower() or n in c.disc.lower() for n in needles)]

    if args.list:
        for case in cases:
            print(f"{case.name}\t{case.disc}")
        return 0

    if not cases:
        print("No games selected", file=sys.stderr)
        return 2

    results: list[GameResult] = []
    for case in cases:
        print(f"[RUN] {case.name}")
        try:
            result = run_case(case, args)
        except subprocess.TimeoutExpired as exc:
            log_dir = args.out / safe_name(case.name)
            log_dir.mkdir(parents=True, exist_ok=True)
            log_path = log_dir / "run.log"
            stdout = exc.stdout if isinstance(exc.stdout, str) else ""
            stderr = exc.stderr if isinstance(exc.stderr, str) else ""
            log_path.write_text(stdout + stderr + "\n[runner] timeout\n", encoding="utf-8")
            result = GameResult(
                name=case.name, disc=case.disc, status="TIMEOUT", returncode=124,
                elapsed_s=float(args.timeout), instructions=None, pc=None, op=None,
                cycles=None, gpu_frames=None, display=None, display_nonzero=None,
                display_disabled=None, display_depth=None, unhandled_count=0,
                error_markers=["timeout"], log=rel(log_path), screenshot=None,
                screenshot_frame=None, trace=None,
            )
        results.append(result)
        print(f"[{result.status}] {case.name} pc={result.pc or '-'} frames={result.gpu_frames or '-'}")

    write_outputs(results, args.out)
    print(f"summary: {rel(args.out / 'summary.md')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

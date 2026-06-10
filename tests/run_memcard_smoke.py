#!/usr/bin/env python3
"""Validate memory-card persistence across two emulator processes."""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CARD_SIZE = 128 * 1024
SECTOR_SIZE = 128
TEST_SECTOR = 63


@dataclass(frozen=True)
class GameCase:
    name: str
    disc: Path


CASES = (
    GameCase(
        "Crash Bandicoot",
        ROOT / "games/Crash Bandicoot (USA)/Crash Bandicoot (USA).cue",
    ),
    GameCase(
        "Gran Turismo",
        ROOT
        / "games/Gran Turismo (USA) (Rev 1)/Gran Turismo (USA) (Rev 1).cue",
    ),
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_boot(
    emulator: Path,
    bios: Path,
    case: GameCase,
    card: Path,
    instructions: int,
    timeout: int,
) -> str:
    env = os.environ.copy()
    env["PS1_LOG"] = "SIO"
    proc = subprocess.run(
        [
            str(emulator),
            "--bios",
            str(bios),
            "--disc",
            str(case.disc),
            "--memcard",
            str(card),
            "--headless",
            "--max-instructions",
            str(instructions),
        ],
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"emulator exited with {proc.returncode}")
    if "Smoke: ran " not in proc.stdout:
        raise RuntimeError("emulator did not complete the requested smoke")
    return proc.stdout


def count(log: str, pattern: str) -> int:
    return len(re.findall(pattern, log))


def validate_case(
    emulator: Path,
    bios: Path,
    out: Path,
    case: GameCase,
    instructions: int,
    timeout: int,
) -> None:
    slug = case.name.lower().replace(" ", "-")
    card = out / f"{slug}.mcr"
    card.unlink(missing_ok=True)

    first = run_boot(emulator, bios, case, card, instructions, timeout)
    if card.stat().st_size != CARD_SIZE:
        raise RuntimeError(f"card has {card.stat().st_size} bytes")
    first_hash = digest(card)
    test_frame = card.read_bytes()[
        TEST_SECTOR * SECTOR_SIZE : (TEST_SECTOR + 1) * SECTOR_SIZE
    ]
    if test_frame == bytes(SECTOR_SIZE):
        raise RuntimeError("game did not persist its card test sector")
    if count(first, r"SIO TX\[1\] = 0x57") < 1:
        raise RuntimeError("first boot did not issue a card write")
    if count(first, r"SIO RX\[0\] = 0x47") < 1:
        raise RuntimeError("first boot did not complete a card command")
    if "SIO RX[0] = 0x4E" in first:
        raise RuntimeError("first boot received a card write error")

    second = run_boot(emulator, bios, case, card, instructions, timeout)
    second_hash = digest(card)
    if "Memory card loaded:" not in second:
        raise RuntimeError("second boot did not load the existing card")
    if count(second, r"SIO TX\[1\] = 0x52") < 1:
        raise RuntimeError("second boot did not read the existing card")
    if count(second, r"SIO RX\[0\] = 0x47") < 1:
        raise RuntimeError("second boot did not complete a card command")
    if "SIO RX[0] = 0x4E" in second:
        raise RuntimeError("second boot received a card error")
    if card.stat().st_size != CARD_SIZE:
        raise RuntimeError("card size changed after reopening")

    (out / f"{slug}-first.log").write_text(first, encoding="utf-8")
    (out / f"{slug}-second.log").write_text(second, encoding="utf-8")
    print(
        f"[PASS] {case.name}: write/read/reopen "
        f"sha256={first_hash[:12]}->{second_hash[:12]}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", type=Path, default=ROOT / "ps1_boot")
    parser.add_argument("--bios", type=Path, default=ROOT / "bios/BIOS.ROM")
    parser.add_argument(
        "--out", type=Path, default=ROOT / "tests/out/memcard-smoke"
    )
    parser.add_argument("--max-instructions", type=int, default=200_000_000)
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()

    missing = [str(case.disc) for case in CASES if not case.disc.is_file()]
    if missing:
        print("Missing required game images:", file=sys.stderr)
        for path in missing:
            print(f"  {path}", file=sys.stderr)
        return 2
    if not args.bios.is_file():
        print(f"Missing BIOS: {args.bios}", file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    failed = 0
    for case in CASES:
        try:
            validate_case(
                args.emulator,
                args.bios,
                args.out,
                case,
                args.max_instructions,
                args.timeout,
            )
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
            failed += 1
            print(f"[FAIL] {case.name}: {exc}", file=sys.stderr)

    print(f"memory-card games: pass={len(CASES) - failed} fail={failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Run bundled ps1-tests against the local emulator.

The runner has two validation levels:
  * every discovered PS-X EXE is executed headless as a smoke test;
  * tests with a reference VRAM PNG dump the emulator VRAM and compare it with
    tests/tools/diffvram.

It intentionally avoids external Python packages so the Makefile target works
on a fresh checkout.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
DEFAULT_OUT = TESTS / "out" / "psx-tests"
DIFFVRAM = TESTS / "tools" / "diffvram" / "diffvram-linux-amd64"
VRAM_COMPARE_REGIONS = {
    ("gpu", "transparency"): (0, 0, 320, 240),
}
VRAM_UNINITIALIZED_COLOR_REGIONS = {
    ("gpu", "lines"): (150, 140, 92, 32),
}
CASE_DUMP_FRAMES = {
    ("mdec", "movie/movie-15bit"): 536,
    ("mdec", "movie/movie-24bit"): 531,
}
CASE_MAX_INSTRUCTIONS = {
    ("mdec", "movie/movie-15bit"): 180_000_000,
    ("mdec", "movie/movie-24bit"): 180_000_000,
}


@dataclass(frozen=True)
class TestCase:
    category: str
    name: str
    exe: Path
    ref_vram: Path | None = None


def rel(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT))
    except ValueError:
        return str(path)


def find_tests() -> list[TestCase]:
    cases: list[TestCase] = []
    skip_dirs = {
        TESTS / "tools",
        TESTS / "out",
    }

    for exe in sorted(TESTS.rglob("*.exe")):
        if any(exe.is_relative_to(skip) for skip in skip_dirs):
            continue
        rel_parts = exe.relative_to(TESTS).parts
        if len(rel_parts) < 2:
            continue

        category = rel_parts[0]
        test_dir = exe.parent
        name_parts = list(exe.relative_to(TESTS / category).with_suffix("").parts)
        if len(name_parts) > 1 and name_parts[-1] == name_parts[-2]:
            name_parts.pop()
        name = "/".join(name_parts)
        ref = reference_for_exe(exe, test_dir)
        cases.append(TestCase(category=category, name=name, exe=exe, ref_vram=ref))

    return cases


def reference_for_exe(exe: Path, test_dir: Path) -> Path | None:
    exact = test_dir / "vram.png"
    if exact.exists():
        return exact

    stem = exe.stem
    if "15bit" in stem:
        ref = test_dir / "vram-15bit.png"
        if ref.exists():
            return ref
    if "24bit" in stem:
        ref = test_dir / "vram-24bit.png"
        if ref.exists():
            return ref

    return None


def filter_cases(cases: Iterable[TestCase], categories: set[str], names: set[str]) -> list[TestCase]:
    out = []
    for case in cases:
        if categories and case.category not in categories:
            continue
        full_name = f"{case.category}/{case.name}"
        if names and case.name not in names and full_name not in names:
            continue
        out.append(case)
    return out


def read_ppm(path: Path) -> tuple[int, int, bytes, int]:
    """Read P6 (RGB) or P7/PAM (RGBA) image.  Returns (width, height, pixels, channels)."""
    data = path.read_bytes()
    pos = 0

    def token() -> bytes:
        nonlocal pos
        while pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
        if pos < len(data) and data[pos] == ord("#"):
            while pos < len(data) and data[pos] not in b"\r\n":
                pos += 1
            return token()
        start = pos
        while pos < len(data) and data[pos] not in b" \t\r\n":
            pos += 1
        return data[start:pos]

    magic = token()
    if magic == b"P6":
        width = int(token())
        height = int(token())
        maxval = int(token())
        if maxval != 255:
            raise ValueError(f"{path}: unsupported maxval {maxval}")
        if pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
        pixels = data[pos:]
        expected = width * height * 3
        if len(pixels) != expected:
            raise ValueError(f"{path}: expected {expected} RGB bytes, got {len(pixels)}")
        return width, height, pixels, 3
    elif magic == b"P7":
        # PAM format: key-value header lines until ENDHDR
        width = height = depth = 0
        while pos < len(data):
            # read a line
            line_start = pos
            while pos < len(data) and data[pos] not in b"\r\n":
                pos += 1
            line = data[line_start:pos].decode("ascii", errors="replace").strip()
            while pos < len(data) and data[pos] in b"\r\n":
                pos += 1
            if line == "ENDHDR":
                break
            if line.startswith("WIDTH"):
                width = int(line.split()[1])
            elif line.startswith("HEIGHT"):
                height = int(line.split()[1])
            elif line.startswith("DEPTH"):
                depth = int(line.split()[1])
        pixels = data[pos:]
        expected = width * height * depth
        if len(pixels) != expected:
            raise ValueError(f"{path}: PAM expected {expected} bytes, got {len(pixels)}")
        return width, height, pixels, depth
    else:
        raise ValueError(f"{path}: unsupported image magic {magic!r}")


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    crc = zlib.crc32(kind)
    crc = zlib.crc32(payload, crc)
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc & 0xFFFFFFFF)


def png_color_type(png: Path) -> int:
    data = png.read_bytes()
    if len(data) < 26 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        return 2
    return data[25]


def png_rgba_has_variable_alpha(png: Path) -> bool:
    data = png.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return False

    pos = 8
    width = height = bit_depth = color_type = None
    idat = b""
    while pos < len(data):
        size = struct.unpack(">I", data[pos : pos + 4])[0]
        kind = data[pos + 4 : pos + 8]
        payload = data[pos + 8 : pos + 8 + size]
        pos += 12 + size
        if kind == b"IHDR":
            width, height, bit_depth, color_type, _, _, _ = struct.unpack(">IIBBBBB", payload)
        elif kind == b"IDAT":
            idat += payload
        elif kind == b"IEND":
            break

    if width is None or height is None or bit_depth != 8 or color_type != 6:
        return False

    raw = zlib.decompress(idat)
    stride = width * 4
    prev = bytearray(stride)
    off = 0
    first_alpha = None
    for _y in range(height):
        filt = raw[off]
        off += 1
        cur = bytearray(raw[off : off + stride])
        off += stride
        for i in range(stride):
            left = cur[i - 4] if i >= 4 else 0
            up = prev[i]
            up_left = prev[i - 4] if i >= 4 else 0
            if filt == 1:
                cur[i] = (cur[i] + left) & 0xFF
            elif filt == 2:
                cur[i] = (cur[i] + up) & 0xFF
            elif filt == 3:
                cur[i] = (cur[i] + ((left + up) >> 1)) & 0xFF
            elif filt == 4:
                p = left + up - up_left
                pa = abs(p - left)
                pb = abs(p - up)
                pc = abs(p - up_left)
                pred = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                cur[i] = (cur[i] + pred) & 0xFF
            elif filt != 0:
                return False
        for i in range(3, stride, 4):
            if first_alpha is None:
                first_alpha = cur[i]
            elif cur[i] != first_alpha:
                return True
        prev = cur

    return first_alpha is not None and first_alpha != 255


def png_palette_to_rgb_png(src: Path, dst: Path) -> None:
    data = src.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{src}: not a PNG")

    pos = 8
    width = height = bit_depth = color_type = None
    palette: list[tuple[int, int, int]] = []
    idat = b""
    while pos < len(data):
        size = struct.unpack(">I", data[pos : pos + 4])[0]
        kind = data[pos + 4 : pos + 8]
        payload = data[pos + 8 : pos + 8 + size]
        pos += 12 + size
        if kind == b"IHDR":
            width, height, bit_depth, color_type, _, _, _ = struct.unpack(">IIBBBBB", payload)
        elif kind == b"PLTE":
            palette = [tuple(payload[i : i + 3]) for i in range(0, len(payload), 3)]
        elif kind == b"IDAT":
            idat += payload
        elif kind == b"IEND":
            break

    if width is None or height is None or bit_depth not in (1, 2, 4, 8) or color_type != 3:
        raise ValueError(f"{src}: unsupported indexed PNG")

    raw = zlib.decompress(idat)
    rows = []
    stride = (width * bit_depth + 7) // 8
    prev = bytearray(stride)
    off = 0
    for _y in range(height):
        filt = raw[off]
        off += 1
        cur = bytearray(raw[off : off + stride])
        off += stride
        for i in range(stride):
            left = cur[i - 1] if i > 0 else 0
            up = prev[i]
            up_left = prev[i - 1] if i > 0 else 0
            if filt == 1:
                cur[i] = (cur[i] + left) & 0xFF
            elif filt == 2:
                cur[i] = (cur[i] + up) & 0xFF
            elif filt == 3:
                cur[i] = (cur[i] + ((left + up) >> 1)) & 0xFF
            elif filt == 4:
                p = left + up - up_left
                pa = abs(p - left)
                pb = abs(p - up)
                pc = abs(p - up_left)
                pred = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                cur[i] = (cur[i] + pred) & 0xFF
            elif filt != 0:
                raise ValueError(f"{src}: unsupported PNG filter {filt}")
        indices = []
        if bit_depth == 8:
            indices = list(cur[:width])
        else:
            mask = (1 << bit_depth) - 1
            for byte in cur:
                for shift in range(8 - bit_depth, -1, -bit_depth):
                    indices.append((byte >> shift) & mask)
                    if len(indices) == width:
                        break
                if len(indices) == width:
                    break

        rgb = bytearray(width * 3)
        for x, idx in enumerate(indices):
            r, g, b = palette[idx]
            rgb[x * 3 : x * 3 + 3] = bytes((r, g, b))
        rows.append(b"\x00" + bytes(rgb))
        prev = cur

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    dst.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
        + png_chunk(b"IEND", b"")
    )

def png_rgb_pixels(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")

    pos = 8
    width = height = bit_depth = color_type = interlace = None
    palette: list[tuple[int, int, int]] = []
    idat = b""
    while pos < len(data):
        size = struct.unpack(">I", data[pos : pos + 4])[0]
        kind = data[pos + 4 : pos + 8]
        payload = data[pos + 8 : pos + 8 + size]
        pos += 12 + size
        if kind == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", payload
            )
        elif kind == b"PLTE":
            palette = [tuple(payload[i : i + 3]) for i in range(0, len(payload), 3)]
        elif kind == b"IDAT":
            idat += payload
        elif kind == b"IEND":
            break

    if width is None or height is None or bit_depth != 8 or interlace != 0:
        raise ValueError(f"{path}: unsupported PNG layout")
    if color_type not in (0, 2, 3):
        raise ValueError(f"{path}: unsupported PNG color type {color_type}")

    channels = 3 if color_type == 2 else 1
    stride = width * channels
    raw = zlib.decompress(idat)
    prev = bytearray(stride)
    off = 0
    rgb = bytearray(width * height * 3)
    for y in range(height):
        filt = raw[off]
        off += 1
        cur = bytearray(raw[off : off + stride])
        off += stride
        for i in range(stride):
            left = cur[i - channels] if i >= channels else 0
            up = prev[i]
            up_left = prev[i - channels] if i >= channels else 0
            if filt == 1:
                cur[i] = (cur[i] + left) & 0xFF
            elif filt == 2:
                cur[i] = (cur[i] + up) & 0xFF
            elif filt == 3:
                cur[i] = (cur[i] + ((left + up) >> 1)) & 0xFF
            elif filt == 4:
                p = left + up - up_left
                pa = abs(p - left)
                pb = abs(p - up)
                pc = abs(p - up_left)
                pred = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                cur[i] = (cur[i] + pred) & 0xFF
            elif filt != 0:
                raise ValueError(f"{path}: unsupported PNG filter {filt}")

        dst = y * width * 3
        if color_type == 2:
            rgb[dst : dst + width * 3] = cur
        elif color_type == 3:
            for x, index in enumerate(cur):
                rgb[dst + x * 3 : dst + x * 3 + 3] = bytes(palette[index])
        else:
            for x, value in enumerate(cur):
                rgb[dst + x * 3 : dst + x * 3 + 3] = bytes((value, value, value))
        prev = cur
    return width, height, bytes(rgb)


def compare_png_region(ref: Path, got: Path, region: tuple[int, int, int, int]) -> int:
    ref_w, ref_h, ref_pixels = png_rgb_pixels(ref)
    got_w, got_h, got_pixels = png_rgb_pixels(got)
    if (ref_w, ref_h) != (got_w, got_h):
        return -1

    x0, y0, width, height = region
    differences = 0
    for y in range(y0, min(y0 + height, ref_h)):
        start = (y * ref_w + x0) * 3
        end = (y * ref_w + min(x0 + width, ref_w)) * 3
        ref_row = ref_pixels[start:end]
        got_row = got_pixels[start:end]
        differences += sum(
            ref_row[i : i + 3] != got_row[i : i + 3]
            for i in range(0, len(ref_row), 3)
        )
    return differences


def compare_png_allow_uninitialized_color(
    ref: Path, got: Path, region: tuple[int, int, int, int]
) -> tuple[int, int]:
    ref_w, ref_h, ref_pixels = png_rgb_pixels(ref)
    got_w, got_h, got_pixels = png_rgb_pixels(got)
    if (ref_w, ref_h) != (got_w, got_h):
        return -1, 0

    x0, y0, width, height = region
    differences = 0
    tolerated = 0
    for y in range(ref_h):
        for x in range(ref_w):
            offset = (y * ref_w + x) * 3
            ref_pixel = ref_pixels[offset : offset + 3]
            got_pixel = got_pixels[offset : offset + 3]
            if ref_pixel == got_pixel:
                continue
            inside = x0 <= x < x0 + width and y0 <= y < y0 + height
            if inside and ref_pixel != b"\x00\x00\x00" and got_pixel != b"\x00\x00\x00":
                tolerated += 1
            else:
                differences += 1
    return differences, tolerated


def ppm_to_png(ppm: Path, png: Path, color_type: int = 2) -> None:
    width, height, pixels, channels = read_ppm(ppm)
    rows = []
    stride = width * channels
    if color_type == 6:
        for y in range(height):
            row = pixels[y * stride : (y + 1) * stride]
            rgba = bytearray(width * 4)
            for x in range(width):
                src = x * channels
                dst = x * 4
                rgba[dst] = row[src]
                rgba[dst + 1] = row[src + 1]
                rgba[dst + 2] = row[src + 2]
                rgba[dst + 3] = row[src + 3] if channels >= 4 else 255
            rows.append(b"\x00" + bytes(rgba))
    elif color_type == 0:
        for y in range(height):
            row = pixels[y * stride : (y + 1) * stride]
            gray = bytearray(width)
            for x in range(width):
                gray[x] = row[x * channels]
            rows.append(b"\x00" + bytes(gray))
    else:
        color_type = 2
        for y in range(height):
            row = pixels[y * stride : (y + 1) * stride]
            if channels == 3:
                rows.append(b"\x00" + row)
            else:
                # extract RGB from RGBA
                rgb = bytearray(width * 3)
                for x in range(width):
                    rgb[x * 3 : x * 3 + 3] = row[x * channels : x * channels + 3]
                rows.append(b"\x00" + bytes(rgb))
    raw = b"".join(rows)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    png.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", zlib.compress(raw, 9))
        + png_chunk(b"IEND", b"")
    )


def run_case(args: argparse.Namespace, case: TestCase) -> bool:
    case_out = args.out / case.category / case.name.replace("/", "__")
    case_out.mkdir(parents=True, exist_ok=True)
    log_path = case_out / "run.log"
    ppm_path = case_out / "vram.ppm"
    png_path = case_out / "vram.png"
    diff_path = case_out / "diff.png"
    ref_compare_path = case.ref_vram
    for stale in (
        ppm_path,
        png_path,
        diff_path,
        case_out / "diffvram.log",
        case_out / "ref-rgb.png",
    ):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    env = os.environ.copy()
    case_key = (case.category, case.name)
    dump_frame = CASE_DUMP_FRAMES.get(case_key, args.dump_frame)
    max_instructions = CASE_MAX_INSTRUCTIONS.get(case_key, args.max_instructions)
    if case.ref_vram and not args.no_diff:
        env["PS1_DUMP_VRAM_PPM"] = str(ppm_path)
        env["PS1_DUMP_FULL_VRAM"] = "1"
        env["PS1_DUMP_FRAME"] = str(dump_frame)
        if case.ref_vram and png_color_type(case.ref_vram) == 6 and png_rgba_has_variable_alpha(case.ref_vram):
            env["PS1_DUMP_STP_ALPHA"] = "1"

    cmd = [
        str(args.emulator),
        "--bios",
        str(args.bios),
        "--exe",
        str(case.exe),
        "--headless",
        "--max-instructions",
        str(max_instructions),
    ]

    print(f"[RUN] {case.category}/{case.name}")
    with log_path.open("wb") as log_file:
        proc = subprocess.run(
            cmd,
            cwd=ROOT,
            env=env,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            timeout=args.timeout,
            check=False,
        )

    if proc.returncode != 0:
        print(f"[FAIL] {case.category}/{case.name}: emulator exit {proc.returncode} ({rel(log_path)})")
        return False

    if case.ref_vram and not args.no_diff:
        if not ppm_path.exists():
            print(f"[FAIL] {case.category}/{case.name}: missing VRAM dump ({rel(log_path)})")
            return False
        ref_color_type = png_color_type(case.ref_vram)
        ppm_to_png(ppm_path, png_path, ref_color_type)
        if ref_color_type == 3:
            ref_compare_path = case_out / "ref-rgb.png"
            png_palette_to_rgb_png(case.ref_vram, ref_compare_path)
        compare_region = VRAM_COMPARE_REGIONS.get((case.category, case.name))
        if compare_region is not None:
            differences = compare_png_region(ref_compare_path, png_path, compare_region)
            if differences != 0:
                print(
                    f"[FAIL] {case.category}/{case.name}: "
                    f"{differences} pixels differ inside VRAM region {compare_region}"
                )
                return False
            print(
                f"[ OK ] {case.category}/{case.name}: "
                f"VRAM region {compare_region} match"
            )
            return True
        color_region = VRAM_UNINITIALIZED_COLOR_REGIONS.get((case.category, case.name))
        if color_region is not None:
            differences, tolerated = compare_png_allow_uninitialized_color(
                ref_compare_path, png_path, color_region
            )
            if differences != 0:
                print(
                    f"[FAIL] {case.category}/{case.name}: {differences} deterministic "
                    f"pixels differ; tolerated={tolerated} in {color_region}"
                )
                return False
            print(
                f"[ OK ] {case.category}/{case.name}: geometry match; "
                f"tolerated {tolerated} uninitialized-color pixels in {color_region}"
            )
            return True
        diff_cmd = [str(args.diffvram), str(ref_compare_path), str(png_path), str(diff_path)]
        diff = subprocess.run(diff_cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if diff.returncode != 0:
            (case_out / "diffvram.log").write_bytes(diff.stdout)
            ref_w, ref_h, ref_pixels = png_rgb_pixels(ref_compare_path)
            got_w, got_h, got_pixels = png_rgb_pixels(png_path)
            differences = -1
            if (ref_w, ref_h) == (got_w, got_h):
                differences = sum(
                    ref_pixels[i : i + 3] != got_pixels[i : i + 3]
                    for i in range(0, len(ref_pixels), 3)
                )
            print(
                f"[FAIL] {case.category}/{case.name}: VRAM differs "
                f"pixels={differences} "
                f"ref={rel(case.ref_vram)} got={rel(png_path)} diff={rel(diff_path)}"
            )
            return False
        print(f"[ OK ] {case.category}/{case.name}: VRAM match")
    else:
        print(f"[ OK ] {case.category}/{case.name}: smoke")

    return True


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=Path, default=ROOT / "ps1_boot")
    parser.add_argument("--bios", type=Path, default=ROOT / "bios" / "BIOS.ROM")
    parser.add_argument("--diffvram", type=Path, default=DIFFVRAM)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--category", action="append", default=[])
    parser.add_argument("--name", action="append", default=[])
    parser.add_argument("--max-instructions", type=int, default=int(os.getenv("PSX_TEST_MAX_INSTRUCTIONS", "40000000")))
    parser.add_argument("--timeout", type=int, default=int(os.getenv("PSX_TEST_TIMEOUT", "20")))
    parser.add_argument("--dump-frame", type=int, default=int(os.getenv("PSX_TEST_DUMP_FRAME", "120")))
    parser.add_argument("--no-diff", action="store_true")
    parser.add_argument("--keep-going", action="store_true")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args(argv)

    cases = filter_cases(find_tests(), set(args.category), set(args.name))
    if args.list:
        for case in cases:
            suffix = f" -> {rel(case.ref_vram)}" if case.ref_vram else ""
            print(f"{case.category}/{case.name}{suffix}")
        return 0

    if not cases:
        print("No tests selected", file=sys.stderr)
        return 2

    if not args.emulator.exists():
        print(f"Missing emulator: {args.emulator}", file=sys.stderr)
        return 2
    if not args.bios.exists():
        print(f"Missing BIOS: {args.bios}", file=sys.stderr)
        return 2
    if not args.no_diff and any(case.ref_vram for case in cases) and not args.diffvram.exists():
        print(f"Missing diffvram: {args.diffvram}", file=sys.stderr)
        return 2

    passed = 0
    failed = 0
    for case in cases:
        try:
            ok = run_case(args, case)
        except subprocess.TimeoutExpired:
            print(f"[FAIL] {case.category}/{case.name}: timeout after {args.timeout}s")
            ok = False
        except Exception as exc:
            print(f"[FAIL] {case.category}/{case.name}: {exc}")
            ok = False

        if ok:
            passed += 1
        else:
            failed += 1
            if not args.keep_going:
                break

    print(f"psx-tests: pass={passed} fail={failed} total={passed + failed}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

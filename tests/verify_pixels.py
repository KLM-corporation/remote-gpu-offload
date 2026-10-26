"""Mathematical verification of a captured frame (standard library only).

Usage: python tests/verify_pixels.py <frame.bmp> [--png preview.png] [--json metrics.json]

Pass criteria (TEST_INFRA.md):
  * mean luminance in [10, 245]
  * luminance standard deviation >= 3.0
  * non-clear (non-background) pixel ratio >= 2.0 %
  * at least 2 distinct colour clusters among non-background pixels
Exit code 0 when every criterion passes, 1 otherwise, 2 when the file cannot be read.
"""
import argparse
import json
import math
import struct
import sys
import zlib
from collections import Counter

MEAN_LUMA_RANGE = (10.0, 245.0)
MIN_LUMA_STDDEV = 3.0
MIN_NON_CLEAR_RATIO = 0.02
MIN_COLOR_CLUSTERS = 2
CLEAR_TOLERANCE = 12        # max per-channel distance still counted as background
CLUSTER_LEVELS = 4          # quantisation levels per channel (64 bins)
CLUSTER_MIN_SHARE = 0.005   # a cluster must hold >= 0.5 % of foreground pixels


def read_bmp(path):
    """Returns (width, height, [(r, g, b), ...]) in top-down row order for 24/32-bit BI_RGB BMPs."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("not a BMP file")
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bpp, compression = struct.unpack_from("<HI", data, 28)
    if bpp not in (24, 32) or compression not in (0, 3):
        raise ValueError(f"unsupported BMP format: {bpp} bpp, compression {compression}")
    top_down = height < 0
    height = abs(height)
    bytes_pp = bpp // 8
    stride = (width * bytes_pp + 3) & ~3
    if offset + stride * height > len(data):
        raise ValueError("truncated BMP pixel data")
    pixels = []
    for row in range(height):
        src_row = row if top_down else height - 1 - row
        base = offset + src_row * stride
        for x in range(width):
            b, g, r = data[base + x * bytes_pp: base + x * bytes_pp + 3]
            pixels.append((r, g, b))
    return width, height, pixels


def write_png(path, width, height, pixels):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for r, g, b in pixels[y * width:(y + 1) * width]:
            raw += bytes((r, g, b))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def analyse(width, height, pixels):
    total = width * height
    lumas = [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in pixels]
    mean = sum(lumas) / total
    stddev = math.sqrt(sum((l - mean) ** 2 for l in lumas) / total)

    # The clear colour is the most frequent exact colour (the background dominates a rendered frame).
    clear, clear_count = Counter(pixels).most_common(1)[0]
    foreground = [p for p in pixels
                  if max(abs(p[0] - clear[0]), abs(p[1] - clear[1]), abs(p[2] - clear[2])) > CLEAR_TOLERANCE]
    non_clear_ratio = len(foreground) / total

    step = 256 // CLUSTER_LEVELS
    bins = Counter((r // step, g // step, b // step) for r, g, b in foreground)
    min_count = max(1, int(len(foreground) * CLUSTER_MIN_SHARE))
    clusters = sum(1 for count in bins.values() if count >= min_count)

    checks = {
        "mean_luminance": MEAN_LUMA_RANGE[0] <= mean <= MEAN_LUMA_RANGE[1],
        "luminance_stddev": stddev >= MIN_LUMA_STDDEV,
        "non_clear_ratio": non_clear_ratio >= MIN_NON_CLEAR_RATIO,
        "color_clusters": clusters >= MIN_COLOR_CLUSTERS,
    }
    return {
        "width": width,
        "height": height,
        "mean_luminance": round(mean, 2),
        "luminance_stddev": round(stddev, 2),
        "clear_color": list(clear),
        "clear_pixel_ratio": round(clear_count / total, 4),
        "non_clear_ratio": round(non_clear_ratio, 4),
        "color_clusters": clusters,
        "checks": checks,
        "passed": all(checks.values()),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("frame", help="captured frame (.bmp)")
    parser.add_argument("--png", help="also write a PNG preview of the frame")
    parser.add_argument("--json", help="write the metrics as JSON")
    args = parser.parse_args(argv)

    try:
        width, height, pixels = read_bmp(args.frame)
    except (OSError, ValueError) as exc:
        print(f"[verify_pixels] ERROR: cannot read {args.frame}: {exc}")
        return 2
    if width == 0 or height == 0:
        print("[verify_pixels] ERROR: empty frame")
        return 2

    metrics = analyse(width, height, pixels)
    if args.png:
        write_png(args.png, width, height, pixels)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(metrics, f, indent=2)

    print(f"[verify_pixels] {width}x{height}  mean luma {metrics['mean_luminance']}  "
          f"stddev {metrics['luminance_stddev']}  non-clear {metrics['non_clear_ratio'] * 100:.1f}%  "
          f"clusters {metrics['color_clusters']}  clear rgb {tuple(metrics['clear_color'])}")
    for name, passed in metrics["checks"].items():
        print(f"  [{'PASS' if passed else 'FAIL'}] {name}")
    print(f"[verify_pixels] {'PASSED' if metrics['passed'] else 'FAILED'}")
    return 0 if metrics["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())

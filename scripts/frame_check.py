#!/usr/bin/env python3
"""Check a captured frame against a known-good reference.

Usage:
  frame_check.py CAPTURE.png REFERENCE.png
  frame_check.py --selftest

Exists because the perflog cannot see a picture. A vertically mirrored frame
and a frame full of corrupted glyphs both keep perfect frame times, so every
numeric gate passes them; both shipped that way. This compares a capture
against a reference and, crucially, also against the reference flipped, so an
orientation regression is reported as an orientation regression rather than
as "somewhat different".

Reports one of:
  MATCH               close enough to the reference
  FLIPPED-VERTICAL    matches the reference far better upside down
  FLIPPED-HORIZONTAL  matches the reference far better mirrored
  ROTATED-180         matches the reference far better rotated
  CORRUPT             gross block noise not present in the reference
  MISMATCH            differs, and not by any of the above
"""
import sys

try:
    from PIL import Image, ImageChops, ImageStat
except ImportError:
    sys.exit("frame_check: needs Pillow (pip install pillow)")


def load(path, size=(480, 270)):
    return Image.open(path).convert("RGB").resize(size, Image.BILINEAR)


def difference(a, b):
    """Mean absolute per-channel difference, 0 (identical) to 255."""
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3.0


def block_noise(img, block=8):
    """Fraction of blocks whose internal variance is extreme.

    Corrupted texture data reads as dense high-frequency colour noise, which
    shows up as many small blocks with very high variance - unlike normal
    detail, which is spatially coherent.
    """
    w, h = img.size
    hot = total = 0
    for y in range(0, h - block, block):
        for x in range(0, w - block, block):
            tile = img.crop((x, y, x + block, y + block))
            var = sum(ImageStat.Stat(tile).stddev) / 3.0
            total += 1
            if var > 60.0:
                hot += 1
    return hot / max(total, 1)


def check(capture_path, reference_path):
    cap = load(capture_path)
    ref = load(reference_path)

    candidates = {
        "MATCH": ref,
        "FLIPPED-VERTICAL": ref.transpose(Image.FLIP_TOP_BOTTOM),
        "FLIPPED-HORIZONTAL": ref.transpose(Image.FLIP_LEFT_RIGHT),
        "ROTATED-180": ref.transpose(Image.ROTATE_180),
    }
    scores = {name: difference(cap, img) for name, img in candidates.items()}
    best = min(scores, key=scores.get)

    print("  difference against reference orientations:")
    for name in sorted(scores, key=scores.get):
        print(f"    {name:<20}{scores[name]:6.2f}")

    cap_noise, ref_noise = block_noise(cap), block_noise(ref)
    print(f"  block noise: capture {cap_noise:.3f}  reference {ref_noise:.3f}")

    # An orientation regression only counts when the flipped match is clearly
    # better, otherwise a symmetric frame flips the verdict on noise alone.
    if best != "MATCH" and scores[best] < scores["MATCH"] * 0.6:
        print(f"  VERDICT: {best}")
        return 2

    if cap_noise > ref_noise * 3 and cap_noise > 0.05:
        print("  VERDICT: CORRUPT")
        return 3

    if scores["MATCH"] > 24.0:
        print("  VERDICT: MISMATCH")
        return 1

    print("  VERDICT: MATCH")
    return 0


def selftest():
    """Prove the checker catches the two defects that actually shipped."""
    import random
    random.seed(7)
    ref = Image.new("RGB", (480, 270), (30, 40, 90))
    px = ref.load()
    for y in range(40, 90):          # an asymmetric bright bar near the top
        for x in range(60, 300):
            px[x, y] = (240, 240, 250)
    for y in range(150, 250):        # coherent detail lower down
        for x in range(0, 480):
            px[x, y] = (60 + (x // 8) % 40, 70, 120)
    ref.save("/tmp/_fc_ref.png")

    ref.transpose(Image.FLIP_TOP_BOTTOM).save("/tmp/_fc_flip.png")

    corrupt = ref.copy()
    cp = corrupt.load()
    for y in range(100, 140):        # dense per-pixel noise, as garbled glyphs
        for x in range(100, 380):
            cp[x, y] = (random.randrange(256), random.randrange(256),
                        random.randrange(256))
    corrupt.save("/tmp/_fc_corrupt.png")

    cases = [("identical", "/tmp/_fc_ref.png", 0),
             ("flipped", "/tmp/_fc_flip.png", 2),
             ("corrupt", "/tmp/_fc_corrupt.png", 3)]
    bad = 0
    for name, path, want in cases:
        print(f"  --- {name} (expect exit {want}) ---")
        got = check(path, "/tmp/_fc_ref.png")
        if got != want:
            print(f"  SELFTEST FAILED: {name} gave {got}")
            bad += 1
    print("  selftest: " + ("FAILED" if bad else "all cases detected"))
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        sys.exit(selftest())
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    sys.exit(check(sys.argv[1], sys.argv[2]))

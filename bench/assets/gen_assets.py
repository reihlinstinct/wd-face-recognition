#!/usr/bin/env python3
"""Generates the synthetic benchmark assets (no real photos, ever).

Outputs into the directory given as argv[1]:
  photo_12mp.jpg  - 4000x3000 (12 MP) JPEG, quality 90, deterministic noise
  exif.jpg        - small JPEG carrying EXIF tags (Make/Model/DateTime/ISO)
"""
import random
import sys
from pathlib import Path

from PIL import Image

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)

W, H = 4000, 3000
rng = random.Random(42)
raw = rng.randbytes(W * H * 3)
img = Image.frombytes("RGB", (W, H), raw)
img.save(out / "photo_12mp.jpg", "JPEG", quality=90)

thumb = Image.frombytes("RGB", (64, 64), raw[: 64 * 64 * 3])
exif = Image.Exif()
exif[0x010F] = "SyntheticCam"          # Make
exif[0x0110] = "BenchMark 3000"       # Model
exif[0x0132] = "2026:10:27 12:00:00"  # DateTime
exif[0x8827] = 200                    # ISOSpeedRatings
thumb.save(out / "exif.jpg", "JPEG", exif=exif)

for f in sorted(out.iterdir()):
    print(f"{f.name}: {f.stat().st_size} bytes")

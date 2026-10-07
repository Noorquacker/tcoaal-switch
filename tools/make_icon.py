#!/usr/bin/env python3
"""Convert any image to the 256x256 baseline JPEG that elf2nro expects.

  make_icon.py <input image> <output.jpg>

Uses Pillow if installed, otherwise ImageMagick.
"""
import shutil
import subprocess
import sys


def main(src: str, dst: str) -> None:
    try:
        from PIL import Image
    except ImportError:
        tool = shutil.which("magick") or shutil.which("convert")
        if not tool:
            raise SystemExit("need Pillow or ImageMagick to convert the icon")
        subprocess.run([tool, src, "-resize", "256x256!", "-background", "black", "-flatten",
                        "-strip", "-interlace", "none", "-quality", "92", dst], check=True)
        return
    img = Image.open(src).convert("RGBA").resize((256, 256), Image.LANCZOS)
    bg = Image.new("RGB", img.size, (0, 0, 0))
    bg.paste(img, mask=img.split()[3])
    bg.save(dst, "JPEG", quality=92, progressive=False)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

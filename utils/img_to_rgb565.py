"""Convert a JPG/PNG to an RGB565 PROGMEM C++ header for Arduino."""

import argparse
import sys
import typing
from pathlib import Path
from PIL import Image


def to_rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def convert(src: Path, width: int, height: int, var: str) -> str:
    with Image.open(src) as img:
        pixels = [
            to_rgb565(*typing.cast(tuple[int, int, int], px))
            for px in img.convert("RGB")
            .resize((width, height), Image.Resampling.LANCZOS)
            .get_flattened_data()
        ]

    return (
        """#if defined(__AVR__)
#include <avr/pgmspace.h>
#elif defined(__PIC32MX__)
#define PROGMEM
#elif defined(__arm__)
#define PROGMEM
#endif
"""
        f"\n// {src.name} -> {width} x {height} RGB565\n\n"
        f"const unsigned short {var}[{len(pixels)}] PROGMEM = \u007b\n  "
        + ",\n  ".join(
            ", ".join(
                f"0x{pixels[i + j]:04X}" for j in range(min(width, len(pixels) - i))
            )
            for i in range(0, len(pixels), width)
        )
        + "\n};\n"
    )


def main():
    parser = argparse.ArgumentParser(description="Image -> RGB565 PROGMEM header")
    parser.add_argument("image", type=Path, help="Input JPG/PNG")
    parser.add_argument("-W", "--width", type=int, default=32)
    parser.add_argument("-H", "--height", type=int, default=32)
    parser.add_argument("-v", "--var", default="image_data", help="C variable name")
    args = parser.parse_args()

    if not args.image.is_file():
        sys.exit(f"error: {args.image} not found")

    result = convert(args.image, args.width, args.height, args.var)

    print(result)


if __name__ == "__main__":
    # uv run python utils/img_to_rgb565.py panda.jpg --var panda >> panda.h
    main()

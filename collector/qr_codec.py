"""Convert a local black-and-white QR image into a small packed matrix."""

import io
import math
from PIL import Image, UnidentifiedImageError


def encode_png(raw):
    if not isinstance(raw, bytes) or not 0 < len(raw) <= 65536:
        raise ValueError('invalid image')
    try:
        with Image.open(io.BytesIO(raw)) as image:
            if image.format != 'PNG' or image.width != image.height or not 29 <= image.width <= 1024:
                raise ValueError('invalid dimensions')
            image = image.convert('L')
            width = image.width
            scale = 0
            for y in range(width):
                prior = image.getpixel((0, y))
                if prior not in (0, 255):
                    raise ValueError('not monochrome')
                run = 1
                for x in range(1, width):
                    pixel = image.getpixel((x, y))
                    if pixel not in (0, 255):
                        raise ValueError('not monochrome')
                    if pixel == prior:
                        run += 1
                    else:
                        scale = math.gcd(scale, run)
                        prior, run = pixel, 1
                scale = math.gcd(scale, run)
            if not scale or width % scale:
                raise ValueError('invalid module size')
            size = width // scale
            if not 29 <= size <= 65 or (size - 25) % 4:
                raise ValueError('unsupported QR size')
            modules = [[image.getpixel((x*scale, y*scale)) == 0 for x in range(size)] for y in range(size)]
            if any(modules[y][x] for y in range(size) for x in range(size)
                   if x < 4 or y < 4 or x >= size-4 or y >= size-4):
                raise ValueError('missing quiet zone')
            for x0, y0 in [(4, 4), (size-11, 4), (4, size-11)]:
                for y in range(7):
                    for x in range(7):
                        expected = x in (0, 6) or y in (0, 6) or 2 <= x <= 4 and 2 <= y <= 4
                        if modules[y0+y][x0+x] != expected:
                            raise ValueError('invalid finder')
            bits = bytearray((size*size+7)//8)
            for y, row in enumerate(modules):
                for x, black in enumerate(row):
                    if black:
                        index = y*size+x
                        bits[index//8] |= 128 >> (index%8)
            return {'size': size, 'bits': bits.hex()}
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError) as exc:
        raise ValueError('invalid image') from exc

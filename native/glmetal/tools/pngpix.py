#!/usr/bin/env python3
"""Prints pixels of 8-bit RGB/RGBA PNGs (stdlib only), for comparing glcompare
outputs: pngpix.py IMAGE [IMAGE...] --at X,Y [--at X,Y...] | --grid N.
Coordinates are GL-style (origin bottom-left)."""
import argparse, struct, zlib


def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, width = 8, b'', 0
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            width, height, depth, color = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and color in (2, 6), 'only 8-bit RGB/RGBA'
            channels = 4 if color == 6 else 3
        elif kind == b'IDAT':
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b, c = prev[i], prev[i - channels] if i >= channels else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return width, height, channels, rows


def pixel(image, x, y):
    width, height, channels, rows = image
    row = rows[height - 1 - y]
    return tuple(row[x * channels:(x + 1) * channels])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('images', nargs='+')
    parser.add_argument('--at', action='append', default=[])
    parser.add_argument('--grid', type=int)
    args = parser.parse_args()
    images = [read_png(p) for p in args.images]
    points = [tuple(int(v) for v in a.split(',')) for a in args.at]
    if args.grid:
        w, h = images[0][0], images[0][1]
        points += [(int((i + 0.5) * w / args.grid), int((j + 0.5) * h / args.grid))
                   for j in range(args.grid - 1, -1, -1) for i in range(args.grid)]
    for x, y in points:
        print(f'{x:4},{y:<4}', '  '.join(str(pixel(img, x, y)) for img in images))


if __name__ == '__main__':
    main()

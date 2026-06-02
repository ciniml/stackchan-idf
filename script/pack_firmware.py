#!/usr/bin/env python3
# Pack the per-partition binaries that `idf.py build` produced (referenced
# from <build-dir>/flash_args at their target addresses) into a single
# firmware.bin padded with 0xFF. This single image is convenient for
# M5Burner-style flash tools that want one file + one offset.

import argparse
import pathlib
import re

target_pattern = re.compile(r'^(0x[0-9a-fA-F]{1,8})\s+([\w\./-]+)')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build-dir', default='build',
                   help='IDF build directory (default: build). Use build-cores3 / '
                        'build-atoms3r for the BOARD-split layout.')
    p.add_argument('--out', default='firmware.bin',
                   help='Output filename (default: firmware.bin).')
    args = p.parse_args()

    build_dir = pathlib.Path(args.build_dir)
    flash_args = build_dir / 'flash_args'

    targets = []
    with open(flash_args) as f:
        for line in iter(f.readline, ''):
            m = target_pattern.match(line)
            if m:
                start_address = int(m.group(1), 16)
                path = m.group(2)
                targets.append((start_address, path))

    targets.sort(key=lambda x: x[0])
    print(targets)

    with open(args.out, 'wb') as f:
        current_address = 0
        for start_address, path in targets:
            if current_address < start_address:
                pad = b'\xff' * (start_address - current_address)
                written = 0
                while written < len(pad):
                    written += f.write(pad[written:])
            current_address = start_address
            bin_path = build_dir / path
            with open(bin_path, 'rb') as g:
                while True:
                    data = g.read()
                    if not data:
                        break
                    written = 0
                    while written < len(data):
                        written += f.write(data[written:])
                    current_address += len(data)


if __name__ == '__main__':
    main()

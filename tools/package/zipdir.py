#!/usr/bin/env python3
"""Zip a folder, keeping Unix file modes (the launcher and the program stay executable), with the folder itself as the
top level of the archive:  zipdir.py FOLDER OUT.zip"""
import os
import sys
import zipfile


def main():
    src, out = os.path.abspath(sys.argv[1]), sys.argv[2]
    base = os.path.dirname(src)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for d, dirs, files in os.walk(src):
            dirs.sort()
            for name in sorted(files):
                path = os.path.join(d, name)
                info = zipfile.ZipInfo.from_file(path, os.path.relpath(path, base))
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(path, "rb") as f:
                    z.writestr(info, f.read())


if __name__ == "__main__":
    main()

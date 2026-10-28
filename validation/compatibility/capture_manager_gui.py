#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Capture the actual Tk manager under X11 using disposable Portage settings."""
import argparse
import ctypes as c
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
from unittest import mock
import zlib


class XImage(c.Structure):
    _fields_ = [(name, c.c_int) for name in ('width','height','xoffset','format')]
    _fields_ += [('data',c.c_void_p)]
    _fields_ += [(name,c.c_int) for name in ('byte_order','bitmap_unit','bitmap_bit_order','bitmap_pad','depth','bytes_per_line','bits_per_pixel')]
    _fields_ += [(name,c.c_ulong) for name in ('red_mask','green_mask','blue_mask')]


def capture(window, path):
    window.update()
    x = c.CDLL('libX11.so.6')
    x.XOpenDisplay.restype = c.c_void_p
    x.XOpenDisplay.argtypes = [c.c_char_p]
    display = x.XOpenDisplay(None)
    if not display: raise RuntimeError('X11 display unavailable')
    x.XGetImage.restype = c.POINTER(XImage)
    x.XGetImage.argtypes = [c.c_void_p,c.c_ulong,c.c_int,c.c_int,c.c_uint,c.c_uint,c.c_ulong,c.c_int]
    x.XDestroyImage.argtypes = [c.POINTER(XImage)]
    x.XCloseDisplay.argtypes = [c.c_void_p]
    width, height = window.winfo_width(), window.winfo_height()
    handle = x.XGetImage(display, window.winfo_id(), 0, 0, width, height, c.c_ulong(-1), 2)
    if not handle: raise RuntimeError('cannot capture Tk window')
    try:
        image = handle.contents
        if image.bits_per_pixel not in (24,32): raise RuntimeError('requires a true-color X11 display')
        data = c.string_at(image.data, image.bytes_per_line * height)
        size = image.bits_per_pixel // 8
        order = 'little' if image.byte_order == 0 else 'big'
        masks = [image.red_mask,image.green_mask,image.blue_mask]
        shifts = [(mask & -mask).bit_length()-1 for mask in masks]
        rows = bytearray()
        for row in range(height):
            rows.append(0)
            for col in range(width):
                offset = row*image.bytes_per_line + col*size
                pixel = int.from_bytes(data[offset:offset+size],order)
                rows.extend(((pixel & mask) >> shift) * 255 // (mask >> shift) for mask,shift in zip(masks,shifts))
        def chunk(kind,data):
            return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
        path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))
    finally:
        x.XDestroyImage(handle)
        x.XCloseDisplay(display)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root_path = Path(__file__).resolve().parents[2]
    sys.path = [p for p in sys.path if Path(p).resolve() != Path(__file__).resolve().parent]
    spec = importlib.util.spec_from_file_location('manager', root_path/'tools/omktr_manager.py')
    manager = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manager)
    import tkinter as tk
    app = tk.Tk()
    app.tk.call('tk', 'scaling', 1.3)
    failures = []
    app.report_callback_exception = lambda *args: (failures.append(args[1]), app.destroy())
    with tempfile.TemporaryDirectory(prefix='omktr-gui-capture-') as directory:
        backend = manager.Backend(Path(directory))
        (backend.root/'bashrc').write_text(manager.BEGIN)
        backend.apply(backend.plan('configure', ['app-arch/gzip'], dict(mode='ninja', jobs='8', strict=True, workspace=True)))
        def ready():
            view = app.manager
            if not view['table'].get_children():
                app.after(100, ready)
                return
            view['query'].set('app-arch/')
            view['table'].selection_set('app-arch/gzip')
            app.update()
            view['table'].see('app-arch/gzip')
            capture(app, args.output/'portage-manager-packages.png')
            view['tabs'].select(view['pages']['Performance'])
            app.update()
            capture(app, args.output/'portage-manager-settings.png')
            view['variables']['jobs'].set('4')
            view['stage']('configure')
            app.update()
            capture(app, args.output/'portage-manager-review.png')
            view['clear']()
            view['table'].selection_set(('app-arch/gzip', 'app-arch/hardlink'))
            app.update()
            view['tabs'].select(view['pages']['Build'])
            app.update()
            capture(app, args.output/'portage-manager-mixed.png')
            view['navigation'].select(1)
            app.update()
            capture(app, args.output/'portage-manager-workspaces.png')
            app.destroy()
        app.after(150, ready)
        with mock.patch.object(tk, 'Tk', return_value=app):
            manager.gui(backend)
    if failures: raise failures[0]
    print('Captured real widgets with live Gentoo catalog and disposable configuration; no changes saved.')

if __name__ == '__main__': main()
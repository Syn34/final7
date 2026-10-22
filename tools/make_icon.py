"""Builds src/app.ico from Nitemare's in-game portrait (tools/icon_source/nitemare.png, 48x48).
Needs Pillow:  python tools/make_icon.py"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = Image.open(os.path.join(ROOT, "tools", "icon_source", "nitemare.png")).convert("RGBA")
big = src.resize((256, 256), Image.NEAREST)   # keep the pixel-art look
big.save(os.path.join(ROOT, "src", "app.ico"),
         sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print("ok")

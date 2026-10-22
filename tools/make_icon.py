"""Draws src/app.ico (a gold pyramid on navy). Needs Pillow:  python tools/make_icon.py"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S = 256
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle([8, 8, S - 8, S - 8], radius=48, fill=(14, 19, 32, 255), outline=(217, 173, 79, 255), width=10)
d.polygon([(S / 2, 44), (S - 44, S - 60), (44, S - 60)], fill=(217, 173, 79, 255))
d.polygon([(S / 2, 84), (S - 88, S - 82), (88, S - 82)], fill=(29, 38, 59, 255))
d.ellipse([S / 2 - 34, 128, S / 2 + 34, 172], fill=(217, 173, 79, 255))
d.ellipse([S / 2 - 14, 136, S / 2 + 14, 164], fill=(14, 19, 32, 255))
img.save(os.path.join(ROOT, "src", "app.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print("ok")

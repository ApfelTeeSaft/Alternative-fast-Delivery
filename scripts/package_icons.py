"""Regenerate platform icons from the application's SVG export (Pillow required)."""

import json
from pathlib import Path
from PIL import Image

root = Path(__file__).resolve().parents[1]
image = Image.open(root / "resources/afd.png").convert("RGBA")
windows = root / "packaging/windows"
macos = root / "packaging/macos"
android = root / "packaging/android/res/drawable"
ios = root / "packaging/ios/Assets.xcassets/AppIcon.appiconset"
for folder in (windows, macos, android, ios):
    folder.mkdir(parents=True, exist_ok=True)
image.save(windows / "afd.ico", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
image.save(macos / "afd.icns", format="ICNS")
image.resize((192, 192), Image.Resampling.LANCZOS).save(android / "afd.png")
flat = Image.new("RGB", image.size, "#dd935f")
flat.paste(image, mask=image.getchannel("A"))
entries = []
for idiom, sizes, scales in [
    ("iphone", [20, 29, 40, 60], [2, 3]),
    ("ipad", [20, 29, 40, 76], [1, 2]),
    ("ipad", [83.5], [2]),
    ("ios-marketing", [1024], [1]),
]:
    for size in sizes:
        for scale in scales:
            pixels = int(size * scale)
            filename = f"icon-{pixels}.png"
            flat.resize((pixels, pixels), Image.Resampling.LANCZOS).save(ios / filename)
            entries.append(
                {
                    "idiom": idiom,
                    "size": f"{size}x{size}",
                    "scale": f"{scale}x",
                    "filename": filename,
                }
            )
(ios / "Contents.json").write_text(
    json.dumps({"images": entries, "info": {"author": "xcode", "version": 1}}, indent=2)
    + "\n"
)
(ios.parent / "Contents.json").write_text(
    json.dumps({"info": {"author": "xcode", "version": 1}}, indent=2) + "\n"
)

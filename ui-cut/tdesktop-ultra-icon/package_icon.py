from pathlib import Path
import subprocess

from PIL import Image

root = Path(__file__).resolve().parent
out = root / "final"
out.mkdir(exist_ok=True)
source = Image.open(root / "raw/icon__app__tdesktop-ultra.png").convert("RGBA")
alpha = source.getchannel("A")
alpha = alpha.point(lambda value: 0 if value < 8 else value)
source.putalpha(alpha)
left, top, right, bottom = alpha.getbbox()
padding = 12
crop = source.crop((
    max(0, left - padding),
    max(0, top - padding),
    min(source.width, right + padding),
    min(source.height, bottom + padding),
))
crop.thumbnail((864, 864), Image.Resampling.LANCZOS)
master = Image.new("RGBA", (1024, 1024))
master.alpha_composite(crop, ((1024 - crop.width) // 2, (1024 - crop.height) // 2))
master_path = out / "tdesktop-ultra.png"
master.save(master_path)
iconset = out / "tdesktop-ultra.iconset"
iconset.mkdir(exist_ok=True)
for points in (16, 32, 128, 256, 512):
    for scale in (1, 2):
        suffix = "@2x" if scale == 2 else ""
        pixels = points * scale
        master.resize((pixels, pixels), Image.Resampling.LANCZOS).save(
            iconset / f"icon_{points}x{points}{suffix}.png"
        )
icns = out / "tdesktop-ultra.icns"
subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(icns)], check=True)
with Image.open(icns) as check:
    check.load()
    assert check.size == (1024, 1024), check.size
assert master.getpixel((0, 0))[3] == 0
assert len(list(iconset.glob("*.png"))) == 10
preview = Image.new("RGB", (1024, 512))
for offset, color in ((0, "#eeeeee"), (512, "#20242c")):
    background = Image.new("RGBA", (512, 512), color)
    background.alpha_composite(master.resize((512, 512), Image.Resampling.LANCZOS))
    preview.paste(background.convert("RGB"), (offset, 0))
preview.save(out / "preview.png")
print(f"PNG: {master_path}")
print(f"ICNS: {icns}")
print("Verified: 1024×1024 RGBA, 10 iconset sizes, readable ICNS.")

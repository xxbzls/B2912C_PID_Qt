"""Create the original probe/feedback icon as SVG, PNG and multi-size Windows ICO."""
from pathlib import Path
import math
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
ASSETS.mkdir(exist_ok=True)
SCALE = 4
image = Image.new("RGBA", (256 * SCALE, 256 * SCALE))
draw = ImageDraw.Draw(image)
svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">']


def box(bounds, radius, color):
    draw.rounded_rectangle(tuple(v * SCALE for v in bounds), radius * SCALE, fill=color)
    x, y, right, bottom = bounds
    svg.append(f'<rect x="{x}" y="{y}" width="{right-x}" height="{bottom-y}" rx="{radius}" fill="{color}"/>')


def polygon(points, color):
    draw.polygon([(round(x * SCALE), round(y * SCALE)) for x, y in points], fill=color)
    coords = " ".join(f"{x:.3f},{y:.3f}" for x, y in points)
    svg.append(f'<polygon points="{coords}" fill="{color}"/>')


def line(points, color, width):
    draw.line([(round(x * SCALE), round(y * SCALE)) for x, y in points], fill=color, width=width * SCALE, joint="curve")
    radius = width * SCALE / 2
    for x, y in (points[0], points[-1]):
        cx, cy = x * SCALE, y * SCALE
        draw.ellipse((cx-radius, cy-radius, cx+radius, cy+radius), fill=color)
    coords = " ".join(f"{x:.3f},{y:.3f}" for x, y in points)
    svg.append(f'<polyline points="{coords}" fill="none" stroke="{color}" stroke-width="{width}" stroke-linecap="round" stroke-linejoin="round"/>')


# A clear silhouette without text remains readable at 16 pixels.
box((8, 8, 248, 248), 54, "#102d46")
arc = [(128 + 84 * math.cos(math.radians(a)), 126 + 84 * math.sin(math.radians(a))) for a in range(64, 346, 2)]
line(arc, "#45d9d0", 14)
polygon([(191, 111), (227, 101), (218, 137)], "#45d9d0")
box((110, 59, 146, 137), 9, "#f4f9ff")
polygon([(110, 129), (146, 129), (128, 164)], "#f4f9ff")
box((123, 73, 133, 117), 4, "#23789b")
line([(78, 181), (99, 181), (111, 167), (131, 196), (145, 181), (169, 181)], "#ffc568", 10)
svg.append("</svg>")
(ASSETS / "probe-control.svg").write_text("\n".join(svg) + "\n", encoding="utf-8")
master = image.resize((256, 256), Image.Resampling.LANCZOS)
master.save(ASSETS / "probe-control.png")
sizes = [(n, n) for n in (16, 20, 24, 32, 40, 48, 64, 128, 256)]
master.save(ASSETS / "probe-control.ico", sizes=sizes)
with Image.open(ASSETS / "probe-control.ico") as icon:
    assert icon.ico.sizes() == set(sizes)
print(f"Created {ASSETS / 'probe-control.ico'} ({len(sizes)} sizes)")

# gen_ico.py — make app.ico from icon_src.jpg (white background -> transparent, autocrop)
from PIL import Image

SRC = "icon_src.jpg"
im = Image.open(SRC).convert("RGBA")
px = im.load()
W, H = im.size
for y in range(H):
    for x in range(W):
        r, g, b, a = px[x, y]
        if r > 238 and g > 238 and b > 238:
            px[x, y] = (r, g, b, 0)
# 裁掉四周留白，让图标内容充满画布（保留 2% 边距）
bbox = im.getbbox()
if bbox:
    pad = int(max(bbox[2] - bbox[0], bbox[3] - bbox[1]) * 0.02)
    bbox = (max(0, bbox[0] - pad), max(0, bbox[1] - pad),
            min(W, bbox[2] + pad), min(H, bbox[3] + pad))
    im = im.crop(bbox)
# 补成正方形
w, h = im.size
side = max(w, h)
sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
sq.paste(im, ((side - w) // 2, (side - h) // 2))
sq.save("app.ico", sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)])
print("app.ico written", sq.size)

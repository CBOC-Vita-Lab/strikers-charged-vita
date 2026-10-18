#!/usr/bin/env python3
"""Builds the package art of the single native app (W5-ONE) from the multiplayer build's art (mp-*.png here, made for this game by
the other chat's build: a football with radio waves). Run: python3 -I platform/vita/vpk/art/make_art.py   (needs Pillow, DejaVu fonts).

  icon0.png                 128x128  final art (MSCH00002): the ball, "MSC", "CHARGED"
  icon0-test.png            128x128  the test id (MSCN00003): the same with the label "NATIVE" in yellow, so that it is told apart from the
                                     final app on the home screen
  livearea/bg.png           840x500  the multiplayer build's background with its text replaced: "Mario Strikers Charged" (the other chat's
                                     art said "Multiplayer test build")
  livearea/startup.png      280x158  the multiplayer build's start button, unchanged (copied)
  livearea/settings.png     280x124  the LiveArea Settings button (psla:-settings, like Mario Kart Wii Vita 1.3.0), same palette
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.dirname(HERE)
BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
NAVY_D, NAVY_L = (17, 24, 32), (26, 39, 56)
BLUE, YELLOW, WHITE = (110, 198, 255), (255, 204, 51), (240, 244, 250)


def font(size):
    return ImageFont.truetype(BOLD, size)


def centered(draw, box, text, fnt, fill, spacing=0):
    x0, y0, x1, y1 = box
    w = draw.textlength(text, font=fnt)
    bbox = draw.textbbox((0, 0), text, font=fnt)
    h = bbox[3] - bbox[1]
    draw.text((x0 + (x1 - x0 - w) / 2, y0 + (y1 - y0 - h) / 2 - bbox[1]), text, font=fnt, fill=fill)


def icon(label, colour, path):
    im = Image.open(os.path.join(HERE, 'mp-icon0.png')).convert('RGBA')
    d = ImageDraw.Draw(im)
    d.rectangle([8, 106, 119, 121], fill=NAVY_L + (255,))          # where "MULTIPLAYER" was
    # keep the label at the width the old one had (about 94 px): the font size follows the label
    size = 15
    f = font(size)
    while d.textlength(label, font=f) > 94 and size > 8:
        size -= 1
        f = font(size)
    centered(d, (6, 105, 122, 122), label, f, colour + (255,))
    im.convert('P', palette=Image.ADAPTIVE, colors=64).save(path, optimize=True)


def background(path):
    im = Image.open(os.path.join(HERE, 'mp-bg.png')).convert('RGB')
    px = im.load()
    # repaint the band between the two stripes (the old title, subtitle and tagline) with the checkerboard (squares 60 x 62): the middle of the
    # page belongs to the Start button (the gate, 280 x 158, in the centre) and the Settings button under it, so the title goes next to the ball,
    # above the blue stripe, and the tagline into the bottom corner
    for y in range(152, 351):
        for x in range(im.width):
            px[x, y] = NAVY_D if ((x // 60) + (y // 62)) % 2 == 0 else NAVY_L
    d = ImageDraw.Draw(im)
    # the ball's old position is kept; whatever of the old title was above the stripe stays (nothing: the logo only)
    for y in range(12, 126):
        for x in range(222, im.width):
            px[x, y] = NAVY_D if ((x // 60) + (y // 62)) % 2 == 0 else NAVY_L
    d.text((232, 26), 'Mario Strikers Charged', font=font(42), fill=WHITE)
    d.text((234, 82), 'Native PS Vita port', font=font(26), fill=BLUE)
    d.text((44, 452), 'Nearby play: no router needed  \u00b7  Same Wi-Fi play', font=font(19), fill=YELLOW)
    im.convert('P', palette=Image.ADAPTIVE, colors=128).save(path, optimize=True)


def gear(d, cx, cy, r, fill, hole):
    import math
    pts = []
    teeth = 8
    for i in range(teeth * 4):
        a = 2 * math.pi * i / (teeth * 4)
        rr = r if (i % 4) in (0, 1) else r * 0.74
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    d.polygon(pts, fill=fill)
    d.ellipse([cx - r * 0.42, cy - r * 0.42, cx + r * 0.42, cy + r * 0.42], fill=hole)


def settings_button(path):
    S = 4  # supersample
    W, H = 280, 124
    im = Image.new('RGBA', (W * S, H * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    top, bottom = 4 * S, 62 * S
    d.rounded_rectangle([4 * S, top, (W - 4) * S, bottom], radius=12 * S, fill=NAVY_L + (255,), outline=BLUE + (255,), width=3 * S)
    d.line([(22 * S, (bottom - 7 * S)), ((W - 22) * S, (bottom - 7 * S))], fill=YELLOW + (255,), width=2 * S)
    gear(d, 46 * S, (top + bottom) // 2 - 2 * S, 17 * S, BLUE + (255,), NAVY_L + (255,))
    f = font(27 * S)
    d.text((82 * S, 12 * S), 'Settings', font=f, fill=WHITE + (255,))
    im = im.resize((W, H), Image.LANCZOS)
    im.save(path, optimize=True)


def main():
    os.makedirs(os.path.join(OUT, 'livearea'), exist_ok=True)
    icon('CHARGED', BLUE, os.path.join(OUT, 'icon0.png'))
    icon('NATIVE', YELLOW, os.path.join(OUT, 'icon0-test.png'))
    background(os.path.join(OUT, 'livearea', 'bg.png'))
    settings_button(os.path.join(OUT, 'livearea', 'settings.png'))
    Image.open(os.path.join(HERE, 'mp-startup.png')).save(os.path.join(OUT, 'livearea', 'startup.png'), optimize=True)
    print('art written to', OUT)


if __name__ == '__main__':
    main()

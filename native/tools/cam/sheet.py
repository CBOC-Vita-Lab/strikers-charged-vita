#!/usr/bin/env python3
"""W5-CAM: the picture set for the maintainer: the same moments of the same match at several camera settings (python3 -I, needs PIL).

    python3 -I tools/cam/sheet.py <out dir> <moments.json> <label>=<run dir> [<label>=<run dir> ...] [--title "..."] [--name contact]

Each run dir holds frame_NNNNNN.png (960x544 dumps, MSC_VGX_SCREEN) and run.log (the [cam] lines). moments.json: {"picks": {name: frame}}.
Writes <out dir>/<moment>_<label>.png (the dump itself), <out dir>/<moment>_side.png (the settings side by side at full size) and
<out dir>/<name>.png: one row per moment, one column per setting, pictures at half size (480x272) with the share of the pitch in view under
each, sized to read on a phone.
"""
import json
import os
import shutil
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import camview  # noqa: E402

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONTB = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
NAMES = {
    "kickoff": "Kick-off (the camera leaves the kick-off view)",
    "midfield": "Build-up in midfield",
    "attack_far_goal": "Our attack at the far goal",
    "attack_own_goal": "The CPU attacks our goal",
    "touchline_far": "Play along the far touchline",
    "touchline_near": "Play along the near touchline",
    "megastrike": "Mega Strike set-up",
}


def font(size, bold=False):
    try:
        return ImageFont.truetype(FONTB if bold else FONT, size)
    except OSError:
        return ImageFont.load_default()


def main(argv):
    args = [a for a in argv[1:]]
    title = "Match camera: the same moments at each zoom level (Auto Zoom on)"
    name = "contact"
    if "--title" in args:
        i = args.index("--title"); title = args[i + 1]; del args[i:i + 2]
    if "--name" in args:
        i = args.index("--name"); name = args[i + 1]; del args[i:i + 2]
    out, mjson = args[0], args[1]
    runs = [a.split("=", 1) for a in args[2:]]
    os.makedirs(out, exist_ok=True)
    picks = json.load(open(mjson))["picks"]
    order = [m for m in NAMES if m in picks]
    logs = {label: camview.parse(os.path.join(d, "run.log")) for label, d in runs}

    W, H = 480, 272
    pad, head, cap, rowhead = 16, 70, 46, 34
    sheet_w = pad + len(runs) * (W + pad)
    sheet_h = head + len(order) * (rowhead + H + cap + pad) + pad
    sheet = Image.new("RGB", (sheet_w, sheet_h), (24, 26, 30))
    dr = ImageDraw.Draw(sheet)
    dr.text((pad, 14), title, fill=(240, 240, 240), font=font(24, True))
    dr.text((pad, 44), "Vita screen 960x544, shown at half size. Under each picture: share of the pitch area / length / width in view.",
            fill=(170, 175, 185), font=font(16))
    y = head
    summary = []
    for m in order:
        f = picks[m]
        dr.text((pad, y + 6), "%s   (frame %d)" % (NAMES[m], f), fill=(255, 214, 90), font=font(20, True))
        y += rowhead
        side = Image.new("RGB", (len(runs) * 960 + (len(runs) - 1) * 8, 544 + 40), (24, 26, 30))
        sd = ImageDraw.Draw(side)
        for k, (label, d) in enumerate(runs):
            src = os.path.join(d, "frame_%06d.png" % f)
            x = pad + k * (W + pad)
            if not os.path.exists(src):
                dr.text((x, y + H // 2), "no picture", fill=(255, 80, 80), font=font(18))
                continue
            im = Image.open(src).convert("RGB")
            slug = "".join(ch for ch in label.split("(")[0].lower() if ch.isalnum())
            shutil.copyfile(src, os.path.join(out, "%s_%s.png" % (m, slug)))
            side.paste(im, (k * (960 + 8), 40))
            sheet.paste(im.resize((W, H), Image.LANCZOS), (x, y))
            rec = logs[label].get(f)
            line = label
            if rec is not None:
                area, length, width = camview.visible_pitch(rec)
                ch = camview.checks(rec)
                off = [n for n, ok, _ in ch if not ok]
                line2 = "pitch %d%% / %d%% / %d%%%s" % (round(100 * area), round(100 * length), round(100 * width),
                                                       ("   off screen: " + ", ".join(off)) if off else "")
                summary.append({"moment": m, "frame": f, "setting": label, "zoom": rec.get("z"), "area": area, "length": length, "width": width,
                                "off": off, "edge": min((dd for _, _, dd in ch), default=0)})
            else:
                line2 = "(no camera line)"
            dr.text((x, y + H + 4), line, fill=(235, 235, 235), font=font(17, True))
            dr.text((x, y + H + 24), line2, fill=(200, 205, 215), font=font(15))
            sd.text((k * (960 + 8) + 8, 8), "%s   %s" % (label, line2), fill=(235, 235, 235), font=font(22, True))
        side.save(os.path.join(out, "%s_side.png" % m))
        y += H + cap + pad
    sheet.save(os.path.join(out, "%s.png" % name))
    with open(os.path.join(out, "%s.json" % name), "w") as f:
        json.dump(summary, f, indent=1)
    print("wrote %s (%dx%d), %d moments x %d settings" % (os.path.join(out, name + ".png"), sheet_w, sheet_h, len(order), len(runs)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

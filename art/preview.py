#!/usr/bin/env python3
"""Approximate render of one Sculpt tab, for checking the skin on a computer.

  python3 plugins/Sculpt/art/preview.py [tab 0-19] [out.png] [param=value ...]

Reads skins/TUI.json and build/params.json (run a native build first). Values are normalised
0..1 and default to each parameter's default. Text uses a system font, so it only approximates
the MPC's Titillium Web. Not part of the plugin.
"""
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SK = os.path.join(HERE, "..", "skins")
FONTS = ["/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/HelveticaNeue.ttc"]


def font(h, bold=False):
    for f in FONTS:
        if os.path.exists(f):
            try:
                return ImageFont.truetype(f, int(h * 0.8), index=2 if bold else 0)
            except OSError:
                pass
    return ImageFont.load_default()


def argb(s):
    if not s or s == "0":
        return (0, 0, 0, 0)
    v = int(s, 16)
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255, (v >> 24) & 255)


class R:
    def __init__(self, values, params):
        self.v = values
        self.params = params
        self.img = Image.new("RGBA", (1280, 628), (16, 16, 16, 255))
        self.defs = {}
        self.rs = {}
        self.cache = {}

    def png(self, name):
        if name not in self.cache:
            self.cache[name] = Image.open(os.path.join(SK, name)).convert("RGBA")
        return self.cache[name]

    def resolve(self, h, remap):
        for k, val in remap.items():
            h = h.replace(k, val)
        return h

    def value(self, h):
        inv = False
        while "/" in h and not h.startswith("Parameter"):
            mod, h = h.split("/", 1)
            if mod == "Invert":
                inv = not inv
        if not h.startswith("Parameter "):
            return None
        x = self.v.get(int(h.split()[1]), 0.0)
        return 1 - x if inv else x

    def valid(self, handles):
        for h in handles:
            parts = h.split("/")
            if parts[0] == "IndexedEnabling":
                i, n = int(parts[1]), int(parts[2])
                x = self.value("/".join(parts[3:]))
                if min(n - 1, int(round(x * (n - 1)))) != i:
                    return False
        return True

    def text(self, h):
        x = self.value(h)
        p = self.params[int(h.split("Parameter ")[1])]
        if p["kind"] in ("choice", "bool") and p["choices"]:
            n = len(p["choices"])
            return p["choices"][min(n - 1, int(round(x * (n - 1))))]
        return "%d%%" % round(x * 100) if p["display"].endswith("%") else p["display"]

    def comp(self, cd, ox, oy, remap):
        for e in cd["componentsData"]:
            b = e["bounds"]
            if b.get("whenVisible") == "WhenFocussed":
                continue
            rm = dict(remap)
            for m in e["handle remapping"]["map"]:
                rm[m["key"]] = self.resolve(m["value"], remap)
            if not self.valid([self.resolve(h, rm) for h in b.get("additionalInvalidatingHandles", [])]):
                continue
            x, y, w, h = [float(t) for t in b["bounds"].split()]
            x += ox
            y += oy
            self.draw(e["componentData"], int(x), int(y), int(w), int(h), rm)

    def draw(self, c, x, y, w, h, rm):
        t, d = c["type"], c["data"]
        H = lambda k="handleName": self.resolve(d[k], rm)
        if t in self.defs:
            self.comp(self.defs[t], x, y, dict(rm))
            return
        if t == "Image":
            im = self.png(d["image"])
            if d.get("imageType") == "FilmStrip":
                n = d["numFrames"]
                fh = im.height // n
                f = min(n - 1, int(self.value(H()) * (n - 1) + 0.5))
                im = im.crop((0, f * fh, im.width, (f + 1) * fh))
            self.img.alpha_composite(im.resize((w, h)), (x, y))
        elif t == "Slider":
            v = self.value(H())
            if d.get("sliderType") == "ColourBar":
                layer = Image.new("RGBA", self.img.size, (0, 0, 0, 0))
                dr = ImageDraw.Draw(layer)
                dr.rectangle([x, y, x + w, y + h], fill=argb(d["backgroundColour"]))
                a, b = (min(v, .5), abs(v - .5)) if d["isBipolar"] else (0, v)
                if d.get("direction") == "Vertical":
                    dr.rectangle([x, y + h - (a + b) * h, x + w, y + h - a * h], fill=argb(d["valueColour"]))
                else:
                    dr.rectangle([x + a * w, y, x + (a + b) * w, y + h], fill=argb(d["valueColour"]))
                self.img.alpha_composite(layer)
            elif d.get("revealedImage"):
                im = self.png(d["revealedImage"])
                rt = d["revealType"]
                if d["direction"] == "Horizontal":
                    lo, hi = {"Normal": (0, v), "Inverted": (v, 1), "Bipolar": (min(v, .5), max(v, .5))}[rt]
                    box = (int(lo * im.width), 0, int(hi * im.width), im.height)
                else:
                    lo, hi = {"Normal": (1 - v, 1), "Inverted": (0, 1 - v), "Bipolar": (0, 1)}[rt]
                    box = (0, int(lo * im.height), im.width, int(hi * im.height))
                if box[2] > box[0] and box[3] > box[1]:
                    self.img.alpha_composite(im.crop(box), (x + box[0], y + box[1]))
            elif d.get("thumbImage"):
                im = self.png(d["thumbImage"])
                if d.get("direction") == "Vertical":
                    self.img.alpha_composite(im, (x, int(y + (1 - v) * (h - im.height))))
                else:
                    self.img.alpha_composite(im, (int(x + v * (w - im.width)), y))
        elif t == "Label":
            ts = d["textStyle"]
            if d["type"] == "Text":
                txt = d.get("text", "")
            elif d["type"] == "Name":
                hn = H()
                if "WithName[" in hn:   # the skin can rename what a Name label shows
                    txt = hn.split("WithName[", 1)[1].split("]", 1)[0]
                else:
                    txt = self.params[int(hn.split("Parameter ")[1])]["name"]
            else:
                txt = self.text(H())
            if ts["case"] == "Upper Case":
                txt = txt.upper()
            f = font(ts["font"]["height"], ts["font"]["style"] in ("SemiBold", "Bold"))
            dr = ImageDraw.Draw(self.img)
            tw = dr.textlength(txt, font=f)
            j = ts["justification"]
            tx = x + (w - tw) / 2 if "horizontallyCentred" in j else (x + w - tw if "right" in j else x)
            dr.text((tx, y + h / 2), txt, font=f, fill=argb(ts["colour"]), anchor="lm")
        elif t == "Button":
            v = self.value(H())
            n = d.get("numButtonsInGroup", 1)
            lit = round(v * (n - 1)) == d["buttonId"] if n > 1 else v >= 0.5
            im = self.png(d["onImage"] if lit else d["offImage"])
            self.img.alpha_composite(im, (x + (w - im.width) // 2, y + (h - im.height) // 2))
        elif t == "Knob":
            im = self.png(d["filmStrip"])
            n = d["numFrames"]
            fh = im.height // n
            f = min(n - 1, int(self.value(H()) * (n - 1) + 0.5))
            self.img.alpha_composite(im.crop((0, f * fh, im.width, (f + 1) * fh)), (x, y))


def main():
    tab = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/sculpt_preview.png"
    params = json.load(open(os.path.join(HERE, "..", "build", "params.json")))["params"]
    values = {p["index"]: p["default"] for p in params}
    for a in sys.argv[3:]:
        k, v = a.split("=")
        values[int(k)] = float(v)
    tui = json.load(open(os.path.join(SK, "TUI.json")))["pageData"]
    r = R(values, params)
    for d in tui["componentDefinitions"]["localComponentDefinitions"]:
        r.defs[d["key"]] = d["value"]
    t = tui["tabs"][tab]
    root = t["componentDefinition"]
    bg = root["backgroundData"]["unfocussed"]["image"]
    if bg:
        r.img.alpha_composite(r.png(bg).resize((1280, 628)))
    r.comp(root, 0, 0, {})
    full = Image.new("RGB", (1280, 800), (22, 24, 29))
    full.paste(r.img.convert("RGB"), (0, 110))
    dr = ImageDraw.Draw(full)
    tabs = ["SCULPT", "MIX"]
    for i, n in enumerate(tabs):
        x0 = i * 1280 // 6
        if i == t["fnKeyIndex"]:
            dr.rectangle([x0, 738, x0 + 1280 // 6, 800], fill=(31, 35, 43))
            dr.rectangle([x0, 738, x0 + 1280 // 6, 741], fill=(70, 227, 255))
        dr.text((x0 + 1280 // 12, 769), n, font=font(20), anchor="mm",
                fill=(255, 255, 255) if i == t["fnKeyIndex"] else (93, 101, 119))
    full.save(out)
    print(out)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build Sculpt's touch-screen skin into ../skins/ (TUI.json, Q-Links.json and every PNG).

Run:  python3 plugins/Sculpt/art/make_skin.py      (needs Pillow; reads ../Content/Samples/*.wav)

Layout (1280 x 628 tab area), after the Sculpt mockup:
  one full-width glass display: a short header, one wide visual for the module, a 2 x 4 grid of
  cells, and a row of keys: 4 track keys on the left, 5 module keys and the toggle key on the right

The five function keys are SOURCE / GRAIN / FILTER / COLOR / SPACE. Track keys 1-4 on the screen
pick the track being edited (the Edit Track parameter); every control on a tab is bound to the
"view" parameters, which the plugin keeps as copies of that track's settings, so the screen and
the hardware Q-Links always follow the picked track.

On every tab the module keys switch each module of the shown track on or off, the track keys
pick the track to edit, and the round key is the module's toggle (Sync, Lock, Freeze).

Every moving part of a visual follows exactly one parameter, using the cheap MPC widgets:
sliders that reveal an image up to the value, sliders with a thumb, images picked by a
choice parameter, and a few small film strips.
"""
import json
import math
import re
import os
import random
import struct
import wave

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "skins")
SAMPLES = os.path.join(HERE, "..", "Content", "Samples")

SS = 3  # supersampling for smooth lines

# ------------------------------------------------------------------ palette (from the mockup)
BODY = (16, 16, 16)
SCR = (10, 10, 10)     # the screen: a neutral near-black (no blue cast)
CYAN = (95, 220, 243)
ICE = (200, 240, 248)
PINK = (248, 102, 205)
DEEP = (59, 103, 158)
WHITE = (242, 248, 252)
WAVE_GREY = (226, 226, 226)       # the Source waveform: neutral, no tint
WAVE_GREY_DIM = (118, 118, 118)   # the Grain waveform, behind its grains
WHITE_N = (236, 236, 236)         # a neutral white (WHITE has a slight blue cast)
MUTED = "ff6f6d68"
DIMTXT = "ff8c8c8c"
LIT = "ffececec"
BRIGHT = "fff2f2f2"

# ------------------------------------------------------------------ geometry
DX, DY, DW, DH = 10, 8, 1260, 612            # display panel: the whole tab
VX, VY, VW, VH = 32, 52, 1216, 264           # the module visual
WAVE_H = VH - 54                             # waveform band inside the visual (Source, Grain)
CELL_W, CELL_H = 296, 84


ROW_STEP = CELL_H + 12
CELL_Y0 = 310   # the first row of settings (moved up into the space the waveform ruler used)


def cell_xy(i, rows=2):
    """Cell i (four to a row). A tab with a third row starts its cells one row higher."""
    return VX + (i % 4) * (CELL_W + 10), CELL_Y0 - (rows - 2) * ROW_STEP + (i // 4) * ROW_STEP


LOWER_LIFT = 19          # how much lighter the lower panel is on Source, Grain, Filter, Color (0-255)
SPACE_H = VH - ROW_STEP   # (no longer used: Space has no picture)

# Tabs laid out as columns, one per effect, each headed by its name (baked into the background:
# fixed text does not draw on the MPC). Module index -> column headings.
COLUMNS = {4: ["CHORUS", "DELAY", "REVERB"]}
COL_GAP = 16
COL_W = (VW - 2 * COL_GAP) // 3
COL_HEAD_Y = 100                # headings (under the browser row)
COL_Y0, COL_STEP = 144, 88      # the first cell under a heading, and cell to cell


def cell_rect(m, i):
    """Where cell i of module m sits: rows of four, or (Space) columns of four."""
    if m in COLUMNS:
        col, row = divmod(i, 4)
        return VX + col * (COL_W + COL_GAP), COL_Y0 + row * COL_STEP, COL_W, CELL_H
    x, y = cell_xy(i, len(MODULES[m][1]) // 4)
    return x, y, CELL_W, CELL_H


KEY_W, KEY_H = 96, 96    # key face size; the images add 6 px of shadow all round
KEY_PITCH = KEY_W + 8    # key to key
KEY_Y = 504              # one row of keys under the cells, with room above and below it
KEY_RECT = lambda x: (x - 6, KEY_Y - 6, KEY_W + 12, KEY_H + 12)


KEY_GROUP_GAP = 40       # extra room between the key groups (tracks | ON, toggle | pages)


def key_x(slot):
    """The key row, left to right: track keys 1-4 (slots 0-3), the page's ON and toggle keys (4, 5),
    then the five module page keys (6-10)."""
    return VX + slot * KEY_PITCH + ((slot >= 4) + (slot >= 6)) * KEY_GROUP_GAP


def track_key_x(i):
    """Track keys 1-4 on the left of the key row."""
    return key_x(i)




# ------------------------------------------------------------------ parameters (match src/Sculpt.cpp)
TRACK_BASE, PER_TRACK = 5, 48


def pidx(track, off):
    return TRACK_BASE + track * PER_TRACK + off


# sample browser parameters, after the track blocks (match src/Sculpt.cpp)
BROWSE_BASE, PER_BROWSE = TRACK_BASE + 4 * PER_TRACK, 8
B_FOLDER, B_FILE, B_LOAD, B_FOLD_PREV, B_FOLD_NEXT, B_FILE_PREV, B_FILE_NEXT, B_HAS_FILE = range(8)


def bidx(track, b):
    return BROWSE_BASE + track * PER_BROWSE + b


# The screen edits one track at a time: Edit Track picks it, and the "view" parameters mirror that
# track's settings (the plugin copies changes both ways). Everything on the screen binds to the view.
EDIT_TRACK = BROWSE_BASE + 4 * PER_BROWSE         # 229
VIEW_BASE = EDIT_TRACK + 1                        # 230


VIEW_ROOT = VIEW_BASE + PER_TRACK + PER_BROWSE + 4   # 290 (after Root of tracks 1-4 at 286..289)


PLAYHEAD = VIEW_ROOT + 1 + 4 * 5 + 1   # 312, after the mix settings and the master meter
BROWSER_OPEN = PLAYHEAD + 1            # 313
LIST_ROWS, LIST_MID = 10, 4
FOLDER_ROW0 = BROWSER_OPEN + 1         # 314..323
FILE_ROW0 = FOLDER_ROW0 + LIST_ROWS    # 324..333
ROW_H = 49
LIST_TOP = VY + 48                     # the open browser covers everything under the browser row
LIST_BOTTOM = DY + DH - 8
PANES = [(VX, 596), (VX + 620, 596)]   # folders, files
STEP_W = 76                            # the up/down keys at the right of each pane


def vidx(off):
    """The view twin of track-block offset `off` (0..47), of Root (56) or of Start (57)."""
    if off == ROOT:
        return VIEW_ROOT
    if off == START:
        return WAVE0 + WAVE_COLS + 4   # 466: after the waveform columns and Start of tracks 1-4
    if CHORUS <= off < CHORUS + 4:
        return WAVE0 + WAVE_COLS + 7 + 16 + (off - CHORUS)   # 485..488: after the markers and 4 tracks' chorus
    if off == REVMIX:
        return WAVE0 + WAVE_COLS + 7 + 16 + 4 + 4            # 493: after the view's chorus and 4 tracks' Mix
    if off == REVTYPE:
        return WAVE0 + WAVE_COLS + 7 + 16 + 4 + 4 + 1 + 4    # 498: after the view's Mix and 4 tracks' Type
    return VIEW_BASE + off


def vbidx(b):
    """The view twin of browser parameter `b`."""
    return VIEW_BASE + PER_TRACK + b


# Source tab: a browser row on top of the visual, the waveform under it
BR_Y, BR_H = DY + 4, 80   # the browser row: in the header's space, twice the old height
SRC_TOP = VY + 48
SRC_H = WAVE_H - 44
LIST_KEY = (VX, BR_Y + 4, 136, 72)           # BROWSE: opens the list browser (files load by themselves)
FOLDER_PLATE = (VX + 146, BR_Y, 504, BR_H)
FILE_PLATE = (VX + 662, BR_Y, 554, BR_H)
STEP_KEY = (60, 72)                          # the < > keys in the browser row
NAME_X = STEP_KEY[0] + 12                    # where the caption and name start inside a plate


ROOT = 56   # view setting 56 is Root (not in the 48-setting track block; no longer on screen)
START = 57  # view setting 57 is Start
CHORUS = 58  # view settings 58..61: the chorus's Amount, Rate, Depth and Width
REVMIX = 62  # view setting 62: the reverb's Mix
REVTYPE = 63  # view setting 63: the reverb's Type
REV_TYPES = ["Hall", "Plate", "Bloom", "Cloud", "Echoes", "Canyon", "Ripple", "Vast"]

# (title, cells: (label, offset, bipolar, steps or 0), on-switch offset, toggle (offset, label) or None)
MODULES = [
    ("SOURCE", [("START", START, False, 0), ("LENGTH", 2, False, 0), ("ROTATE", 1, False, 0), ("SPEED", 0, True, 0),
                ("LOOP", 6, False, 5), ("FADE", 5, False, 0), ("PITCH", 3, True, 49), ("LEVEL", 4, False, 0)],
     40, (45, "SYNC")),
    # Grain: Grainbloom's engine (Mode picks how grains are chosen; Size is a tempo division in the rhythmic modes)
    ("GRAIN", [("MODE", 14, False, 7), ("PITCH", 8, True, 49), ("SIZE", 10, False, 0), ("DENSITY", 9, False, 0),
               ("SPREAD", 11, False, 0), ("REVERSE", 12, False, 0), ("FEEDBACK", 13, False, 0), ("WET", 15, False, 0)],
     41, (46, "LOCK")),
    ("FILTER", [("CUTOFF", 16, False, 0), ("RESO", 17, False, 0), ("DECAY", 18, False, 0), ("PITCH", 19, True, 25),
                ("SLOPE", 20, False, 3), ("TONE", 21, True, 0), ("SCALE", 22, False, 4), ("WET", 23, False, 0)],
     42, None),
    ("COLOR", [("DRIVE", 24, False, 0), ("COMP", 25, False, 0), ("CRUSH", 26, False, 0), ("TILT", 27, True, 0),
               ("NOISE", 28, False, 0), ("N DECAY", 29, False, 0), ("N TONE", 30, False, 0), ("WET", 31, False, 0)],
     43, None),
    # Space is three columns, one per effect (see COLUMNS): chorus, delay, reverb, four settings each
    ("SPACE", [("RATE", CHORUS + 1, False, 0), ("DEPTH", CHORUS + 2, False, 0), ("WIDTH", CHORUS + 3, False, 0),
               ("MIX", CHORUS, False, 0),
               ("TIME", 33, False, 0), ("FEEDBACK", 36, False, 0), ("SPREAD", 37, False, 0), ("MIX", 32, False, 0),
               ("TYPE", REVTYPE, False, 8), ("SIZE", 35, False, 0), ("DECAY", 39, False, 0), ("MIX", REVMIX, False, 0)],
     44, (47, "FREEZE")),
]
CHOICES = {6: ["Off", "Fwd", "Ping", "Fwd Tape", "Ping Tape"], 7: ["Kalimba", "Tape", "Haze", "Glass", "Drone", "Analog Pad", "Synth Arp", "Air Pad", "Dark Drone", "Club Kick"],
           14: ["Collage", "Cloud", "Stutter", "Shimmer", "Shatter", "Arpeggio", "Cascade"], 20: ["Low", "Band", "High"],
           22: ["Chrom", "Major", "Minor", "Penta"]}
BOOL_OFFSETS = [40, 41, 42, 43, 44, 45, 46, 47]


# ------------------------------------------------------------------ drawing helpers
def canvas(w, h, colour=(0, 0, 0, 0)):
    return Image.new("RGBA", (w * SS, h * SS), colour)


def down(img):
    return img.resize((img.width // SS, img.height // SS), Image.LANCZOS)


def rgba(c, a):
    return (c[0], c[1], c[2], int(max(0, min(1, a)) * 255))


def glow(img, radius, strength=1.0):
    b = img.filter(ImageFilter.GaussianBlur(radius * SS))
    if strength != 1.0:
        r, g, bl, a = b.split()
        a = a.point(lambda v: int(min(255, v * strength)))
        b = Image.merge("RGBA", (r, g, bl, a))
    return Image.alpha_composite(b, img)


def save(img, name):
    img.save(os.path.join(OUT, name), optimize=True)
    return name


def strip(frames, name):
    w, h = frames[0].size
    s = Image.new("RGBA", (w, h * len(frames)), (0, 0, 0, 0))
    for i, f in enumerate(frames):
        s.paste(f, (0, i * h))
    return save(s, name)


def read_peaks(path, cols):
    with wave.open(path, "rb") as w:
        n, ch, sw = w.getnframes(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(n)
    vals = struct.unpack("<%dh" % (n * ch), raw) if sw == 2 else [0] * n
    mono = [vals[i * ch] / 32768.0 for i in range(n)]
    per = max(1, n // cols)
    peaks = [max(abs(x) for x in mono[c * per:(c + 1) * per]) for c in range(cols)]
    m = max(peaks) or 1.0
    return [p / m for p in peaks]


def bars(peaks, w, h, colour, alpha, mid=None, gap=0.6):
    img = canvas(w, h)
    d = ImageDraw.Draw(img)
    n = len(peaks)
    cw = w / n
    mid = h / 2 if mid is None else mid
    for i, p in enumerate(peaks):
        a = p * h * 0.44
        x0 = i * cw
        d.rectangle([x0 * SS, (mid - a) * SS, (x0 + max(1, cw - gap)) * SS, (mid + a) * SS], fill=rgba(colour, alpha))
    return down(img)


def ruler(w, h, colour, alpha_major=0.9, alpha_minor=0.5):
    img = canvas(w, h)
    d = ImageDraw.Draw(img)
    for k in range(33):
        major = k % 8 == 0
        x = k / 32 * (w - 2)
        hh = 18 if major else 10
        d.rectangle([x * SS, (h - hh) * SS, (x + 2) * SS, h * SS], fill=rgba(colour, alpha_major if major else alpha_minor))
    return down(img)


# ------------------------------------------------------------------ backgrounds
def rounded_panel(img, box, radius, fill, outline=None):
    d = ImageDraw.Draw(img)
    x, y, w, h = box
    d.rounded_rectangle([x * SS, y * SS, (x + w) * SS, (y + h) * SS], radius * SS, fill=fill, outline=outline,
                        width=SS if outline else 0)


def background(module_art, cells=True, badge=False, art_at=(VX, VY), overlay=None, m=None):
    W, H = 1280, 628
    img = Image.new("RGBA", (W * SS, H * SS), BODY + (255,))
    d = ImageDraw.Draw(img)
    # body: a soft vertical gradient
    for y in range(H):
        c = int(19 - 6 * y / H)
        d.rectangle([0, y * SS, W * SS, (y + 1) * SS], fill=(c, c, c, 255))
    # display bezel and glass
    rounded_panel(img, (DX - 8, DY - 8, DW + 16, DH + 16), 20, (8, 8, 8, 255))
    rounded_panel(img, (DX, DY, DW, DH), 14, SCR + (255,), outline=(0, 0, 0, 255))
    glass = canvas(DW, DH)
    gd = ImageDraw.Draw(glass)
    # a cool radial lift near the top of the glass
    for r in range(260, 0, -4):
        a = 0.16 * (1 - r / 260)
        gd.ellipse([(DW / 2 - r * 2) * SS, (DH * 0.3 - r * 1.15) * SS, (DW / 2 + r * 2) * SS, (DH * 0.3 + r * 1.15) * SS],
                   fill=rgba((24, 24, 24), a + 0.02))   # a soft neutral lift near the top
    # a fine dot grid
    for yy in range(0, DH, 5):
        for xx in range(0, DW, 5):
            gd.point([(xx * SS, yy * SS)], fill=(200, 200, 200, 10))   # the fine dot grid, neutral
    mask = Image.new("L", glass.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, DW * SS - 1, DH * SS - 1], 14 * SS, fill=255)
    img.paste(glass, (DX * SS, DY * SS), Image.composite(glass, Image.new("RGBA", glass.size), mask))
    # header track badge
    d = ImageDraw.Draw(img)
    if badge:
        d.rounded_rectangle([32 * SS, 18 * SS, 60 * SS, 44 * SS], 4 * SS, outline=(207, 207, 207, 255), width=int(1.5 * SS))
    # cell plates (translucent: drawn on their own layer and composited)
    plates = Image.new("RGBA", img.size, (0, 0, 0, 0))
    pd = ImageDraw.Draw(plates)
    if m is not None and m < 4 and cells:
        # Source, Grain, Filter and Color: a lighter panel under the picture, behind the boxes and
        # keys, with a faint line along its top edge
        top = cell_rect(m, 0)[1] - 10
        pd.rounded_rectangle([DX * SS, top * SS, (DX + DW) * SS, (DY + DH) * SS], 14 * SS, fill=(255, 255, 255, LOWER_LIFT))
        pd.rectangle([DX * SS, top * SS, (DX + DW) * SS, (top + 14) * SS], fill=(255, 255, 255, LOWER_LIFT))   # square top corners
        pd.line([DX * SS, top * SS, (DX + DW) * SS, top * SS], fill=(255, 255, 255, 22), width=SS)
    # boxes behind the settings, except on the tabs with the lighter lower panel (there the settings
    # sit straight on the panel)
    for i in range(len(MODULES[m][1]) if cells and m is not None and m >= 4 else 0):
        x, y, w, h = cell_rect(m, i)
        pd.rounded_rectangle([x * SS, y * SS, (x + w) * SS, (y + h) * SS], 6 * SS, fill=(255, 255, 255, 9))
    if m is not None and cells:   # plates behind the folder and file names, on every module page
        for x, y, w, h in (FOLDER_PLATE, FILE_PLATE):
            pd.rounded_rectangle([x * SS, y * SS, (x + w) * SS, (y + h) * SS], 8 * SS, fill=(255, 255, 255, 12))
    for c, head in enumerate(COLUMNS.get(m, [])):
        # each effect's name above its column, with a hairline under it
        x = VX + c * (COL_W + COL_GAP)
        pd.text(((x + 4) * SS, (COL_HEAD_Y + 16) * SS), "  ".join(head), font=label_font(22 * SS), fill=rgba(ICE, 0.9),
                anchor="lm")
        pd.line([(x + 2) * SS, (COL_HEAD_Y + 36) * SS, (x + COL_W - 2) * SS, (COL_HEAD_Y + 36) * SS],
                fill=rgba(CYAN, 0.35), width=SS)
    img = Image.alpha_composite(img, plates)
    # wells under the round toggle key on the right are drawn by the key images themselves
    out = down(img)
    if module_art is not None:
        out.alpha_composite(module_art, art_at)
    if overlay is not None:
        out.alpha_composite(overlay)
    return out.convert("RGB")


# ------------------------------------------------------------------ knob and keys
def key_image(lit, icon=None, round_key=False, scale=1.5):
    o = 6
    w, h = KEY_W + 2 * o, KEY_H + 2 * o
    img = canvas(w, h)
    sh = canvas(w, h)
    box = [o * SS, (o + 4) * SS, (w - o) * SS, (h - o + 4) * SS]
    ImageDraw.Draw(sh).rounded_rectangle(box, 10 * SS, fill=(0, 0, 0, 220))
    img = Image.alpha_composite(img, sh.filter(ImageFilter.GaussianBlur(4 * SS)))
    d = ImageDraw.Draw(img)
    x0, y0, x1, y1 = o, o, w - o, h - o
    d.rounded_rectangle([(x0 - 4) * SS, (y0 - 4) * SS, (x1 + 4) * SS, (y1 + 4) * SS], 12 * SS, fill=(9, 9, 9, 255))
    d.rounded_rectangle([x0 * SS, (y0 + 2) * SS, x1 * SS, (y1 + 2) * SS], 8 * SS, fill=(12, 12, 12, 255))
    d.rounded_rectangle([x0 * SS, y0 * SS, x1 * SS, y1 * SS], 8 * SS, fill=(46, 46, 46, 255))
    d.line([(x0 + 8) * SS, (y0 + 1) * SS, (x1 - 8) * SS, (y0 + 1) * SS], fill=(64, 64, 64, 255), width=SS)
    col = (255, 255, 255, 255) if lit else (92, 91, 88, 255)
    # on keys show a white icon, off keys a grey one (no border). The icon is drawn at its usual
    # 32 px size and then enlarged to suit the bigger key.
    ic = canvas(32, 32)
    di = ImageDraw.Draw(ic)
    if round_key:
        di.ellipse([8 * SS, 8 * SS, 24 * SS, 24 * SS], outline=col, width=int(1.8 * SS))
        if lit:
            di.ellipse([12 * SS, 12 * SS, 20 * SS, 20 * SS], fill=col)
    if icon is not None:
        icon(di, 1, 0, col)
    n = int(32 * scale * SS)
    ic = ic.resize((n, n), Image.LANCZOS)
    img.alpha_composite(ic, (int((w * SS - n) / 2), int((h * SS - n) / 2)))
    return down(img)


def icon_poly(points):
    def draw(d, ox, oy, col):
        pts = [((ox + x) * SS, (oy + y) * SS) for x, y in points]
        d.line(pts, fill=col, width=int(1.8 * SS), joint="curve")
    return draw


def icon_source(d, ox, oy, col):
    # a waveform drawn as vertical lines, mirrored round a centre, louder in the middle
    heights = [3, 6, 9, 5, 8, 4]       # sized to match the other icons (about 20 px tall)
    cy = 16
    for i, h in enumerate(heights):
        x = 7 + i * 3.6
        d.line([((ox + x) * SS, (oy + cy - h) * SS), ((ox + x) * SS, (oy + cy + h) * SS)], fill=col, width=int(1.7 * SS))


def icon_grain(d, ox, oy, col):
    for cx, cy, r in [(8, 9, 2), (17, 7, 1), (13, 15, 3), (23, 13, 1.4), (7, 22, 1), (21, 22, 2)]:
        d.ellipse([(ox + cx - r) * SS, (oy + cy - r) * SS, (ox + cx + r) * SS, (oy + cy + r) * SS], outline=col,
                  width=int(1.8 * SS))


def icon_filter(d, ox, oy, col):
    pts = [(3, 20), (11, 20)]
    for k in range(13):
        t = k / 12
        pts.append((11 + 7 * t, 20 - 12 * math.sin(t * math.pi / 2) ** 0.7))
    for k in range(13):
        t = k / 12
        pts.append((18 + 4 * t, 8 + 12 * t ** 1.3))
    pts.append((27, 20))
    icon_poly(pts)(d, ox, oy, col)


def icon_color(d, ox, oy, col):
    # two interlocking circles, the left one filled
    r = 8.5
    for i, cx in enumerate((11.0, 20.0)):
        box = [(ox + cx - r) * SS, (oy + 16 - r) * SS, (ox + cx + r) * SS, (oy + 16 + r) * SS]
        if i == 0:
            d.ellipse(box, fill=col)
        else:
            d.ellipse(box, outline=col, width=int(1.8 * SS))


def icon_space(d, ox, oy, col):
    # a 3D wireframe cube: front square, back square offset up and right, joined at the corners
    w = int(1.7 * SS)
    f0, s_, o = (5, 11), 14, 6          # front square's top-left, its size, the depth offset
    front = [(f0[0], f0[1]), (f0[0] + s_, f0[1]), (f0[0] + s_, f0[1] + s_), (f0[0], f0[1] + s_)]
    back = [(x + o, y - o) for x, y in front]
    P = lambda pts: [((ox + x) * SS, (oy + y) * SS) for x, y in pts]
    d.line(P(back + back[:1]), fill=col, width=w)
    for a_, b_ in zip(front, back):
        d.line(P([a_, b_]), fill=col, width=w)
    d.line(P(front + front[:1]), fill=col, width=w)


ICONS = [icon_source, icon_grain, icon_filter, icon_color, icon_space]


def icon_word(word):
    """A key's name drawn into the key itself (fixed text does not draw on the MPC)."""
    def draw(d, ox, oy, col):
        size = 11 if len(word) <= 4 else 8.5
        d.text(((ox + 15) * SS, (oy + 16) * SS), word, font=label_font(size * SS), fill=col, anchor="mm")
    return draw


def icon_digit(n):
    """A number drawn into the key itself (text labels over keys do not draw, or block taps, on the MPC)."""
    def draw(d, ox, oy, col):
        d.text(((ox + 15) * SS, (oy + 16) * SS), str(n), font=label_font(18 * SS), fill=col, anchor="mm")
    return draw


# ------------------------------------------------------------------ visuals, one per module
def thumb_line(h, colour, width=2, tri=False, dash=0):
    w = 26
    img = canvas(w, h)
    d = ImageDraw.Draw(img)
    c = w / 2
    bottom = h - (14 if tri else 0)
    if dash:
        for y in range(0, bottom, dash * 2):
            d.rectangle([(c - width / 2) * SS, y * SS, (c + width / 2) * SS, min(y + dash, bottom) * SS], fill=rgba(colour, 1))
    else:
        d.rectangle([(c - width / 2) * SS, 0, (c + width / 2) * SS, bottom * SS], fill=rgba(colour, 1))
    if tri:
        d.polygon([((c - 7) * SS, h * SS), (c * SS, (h - 12) * SS), ((c + 7) * SS, h * SS)], fill=rgba(ICE, 1))
    return down(glow(img, 4, 0.9))


def chevrons(w, h, colour):
    """Chevrons pointing away from the centre: revealed bipolar by Speed."""
    img = canvas(w, h)
    d = ImageDraw.Draw(img)
    mid, cy = w / 2, h / 2
    step = 13
    for side in (-1, 1):
        k = 0
        while True:
            cx = mid + side * (12 + k * step)
            if cx < 8 or cx > w - 8:
                break
            pts = [(cx - side * 5, cy - 9), (cx + side * 3, cy), (cx - side * 5, cy + 9)]
            d.line([(x * SS, y * SS) for x, y in pts], fill=rgba(colour, 0.95), width=int(2.4 * SS))
            k += 1
    return down(img)


def loop_glyph(mode):
    """Off: play once to an end stop. Fwd: a loop that returns to its start. Ping: there and back."""
    w, h = 70, 22
    img = canvas(w, h)
    d = ImageDraw.Draw(img)
    L = lambda pts, c: d.line([(x * SS, y * SS) for x, y in pts], fill=c, width=2 * SS, joint="curve")
    col, dim = rgba(CYAN, 0.95), rgba(DEEP, 0.95)
    if mode == 0:
        L([(10, 11), (56, 11)], dim)
        L([(48, 5), (56, 11), (48, 17)], dim)
        L([(60, 4), (60, 18)], dim)
    elif mode == 1:
        L([(14, 6), (56, 6)], col)
        L([(50, 2), (56, 6), (50, 10)], col)
        L([(56, 6), (60, 9), (60, 13), (56, 16), (14, 16), (10, 13), (10, 9), (14, 6)], col)
    else:
        L([(10, 11), (60, 11)], col)
        L([(18, 5), (10, 11), (18, 17)], col)
        L([(52, 5), (60, 11), (52, 17)], col)
    return down(img)


def ring_glyph():
    img = canvas(14, 14)
    ImageDraw.Draw(img).ellipse([1 * SS, 1 * SS, 13 * SS, 13 * SS], outline=(156, 156, 156, 255), width=int(1.5 * SS))
    return down(img)


def label_font(size):
    from PIL import ImageFont
    for f, i in (("/System/Library/Fonts/Avenir Next.ttc", 2), ("/System/Library/Fonts/HelveticaNeue.ttc", 1)):
        if os.path.exists(f):
            try:
                return ImageFont.truetype(f, size, index=i)
            except OSError:
                pass
    return ImageFont.load_default()


def small_key(w, h, lit, glyph=None, text_size=20):
    img = canvas(w, h)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([1 * SS, 1 * SS, (w - 1) * SS, (h - 1) * SS], 6 * SS, fill=(34, 34, 34, 255),
                        outline=rgba(CYAN, 0.8) if lit else (12, 12, 12, 255), width=2 * SS if lit else SS)
    col = (255, 255, 255, 255) if lit else (150, 150, 150, 255)
    if glyph in ("<", ">"):
        c, cy = w / 2, h / 2
        sgn = -1 if glyph == "<" else 1
        d.line([((c - sgn * 4) * SS, (cy - 7) * SS), ((c + sgn * 4) * SS, cy * SS), ((c - sgn * 4) * SS, (cy + 7) * SS)],
               fill=col, width=int(2.2 * SS), joint="curve")
    elif glyph in ("up", "down"):
        c, cy = w / 2, h / 2
        sgn = -1 if glyph == "up" else 1
        d.polygon([((c - 16) * SS, (cy - sgn * 9) * SS), ((c + 16) * SS, (cy - sgn * 9) * SS), (c * SS, (cy + sgn * 11) * SS)],
                  fill=col)
    elif glyph == "list":
        for k in range(3):
            y = h / 2 - 7 + k * 7
            d.line([((w / 2 - 10) * SS, y * SS), ((w / 2 + 10) * SS, y * SS)], fill=col, width=int(2.2 * SS))
    elif glyph:
        f = label_font(text_size * SS)
        d.text((w / 2 * SS, h / 2 * SS), glyph, font=f, fill=col, anchor="mm")
    return down(img)


def make_source(peaks_by_sample):
    art = Image.new("RGBA", (VW, VH), (0, 0, 0, 0))
    files = {}
    # the waveform itself is drawn live from the plugin's columns (wave_bars)

    files["start"] = save(thumb_line(SRC_H + 14, CYAN), "src_start.png")
    files["dim"] = save(Image.new("RGBA", (VW, SRC_H), rgba(SCR, 0.7)), "src_dim.png")
    files["playhead"] = save(thumb_line(SRC_H + 14, WHITE, 2), "src_playhead.png")
    files["end"] = save(thumb_line(SRC_H + 14, CYAN), "src_end.png")
    files["rotate"] = save(thumb_line(SRC_H + 14, PINK, 2, dash=6), "src_rotate.png")
    for lit in (0, 1):
        files["prev_%d" % lit] = save(small_key(STEP_KEY[0], STEP_KEY[1], lit, "<"), "br_prev_%d.png" % lit)
        files["next_%d" % lit] = save(small_key(STEP_KEY[0], STEP_KEY[1], lit, ">"), "br_next_%d.png" % lit)
        files["list_%d" % lit] = save(small_key(LIST_KEY[2], LIST_KEY[3], lit, "BROWSE", 24), "br_list_%d.png" % lit)
    step_h = LIST_ROWS * ROW_H // 2 - 6
    files["up"] = save(small_key(STEP_W - 6, step_h, False, "up"), "br_up.png")
    files["down"] = save(small_key(STEP_W - 6, step_h, False, "down"), "br_down.png")
    # the list browser's panel: two panes, the middle row of each marked as the current choice
    panel = canvas(VW, LIST_BOTTOM - LIST_TOP + 2)
    ImageDraw.Draw(panel).rounded_rectangle([0, 0, VW * SS, panel.height], 8 * SS, fill=(10, 12, 16, 255))
    over = Image.new("RGBA", panel.size, (0, 0, 0, 0))   # translucent parts go on their own layer
    pd = ImageDraw.Draw(over)
    for px, pw in PANES:
        x0 = px - VX
        pd.rounded_rectangle([x0 * SS, 2 * SS, (x0 + pw) * SS, (2 + LIST_ROWS * ROW_H) * SS], 6 * SS, fill=(255, 255, 255, 10))
        for k in range(1, LIST_ROWS):
            y = 2 + k * ROW_H
            pd.line([(x0 + 10) * SS, y * SS, (x0 + pw - STEP_W - 10) * SS, y * SS], fill=(255, 255, 255, 18), width=SS)
        y = 2 + LIST_MID * ROW_H
        pd.rounded_rectangle([(x0 + 2) * SS, (y + 2) * SS, (x0 + pw - STEP_W - 4) * SS, (y + ROW_H - 2) * SS], 5 * SS,
                             fill=rgba(CYAN, 0.14), outline=rgba(CYAN, 0.9), width=2 * SS)
    panel = Image.alpha_composite(panel, over)
    files["panel"] = save(down(panel), "br_panel.png")
    files["speed"] = save(chevrons(VW, 30, ICE), "src_speed.png")
    for m in range(3):
        files["loop_%d" % m] = save(loop_glyph(m), "src_loop_%d.png" % m)
    return art, files


GR_TOP, GR_H = VY + 22, 196     # the Grain picture: the last ~3 s of sound the grains pick from


# ---- "Tide": the Grain picture as a landscape of the last ~3 s of sound, seen from above, with
# older sound sinking into the distance, the pick zone lit along the front line, and ripples where
# grains land. Everything is a small film strip at a fixed spot, switched by values the plugin sends.
TIDE_ROWS = 7               # rows of landscape, front (0) to back (6)
TIDE_FRONT_Y = 252          # where the front row's silence sits (tab coordinates)
TIDE_ROW_STEP = 26          # each row further back sits this much higher...
TIDE_ROW_SHRINK = 0.075     # ...and this much narrower (perspective)
TIDE_AMP = 70               # how high the loudest sound rises on the front row
TIDE_F = 20                 # heights per landscape strip
TIDE_SHIFT = 2              # each row back shows sound this many spots older
RIPPLE_W, RIPPLE_PAD = 150, 25
RIPPLE_FRAMES = 81          # 0 = none, then 8 heights x 10 spreads (matches the plugin)


def tide_row(b):
    """Row b's geometry: (baseline y, scale, amplitude, spot width)."""
    sc = 1.0 - b * TIDE_ROW_SHRINK
    return TIDE_FRONT_Y - b * TIDE_ROW_STEP, sc, TIDE_AMP * sc, VW * sc / GRAIN_COLS


def tide_x(c, sc):
    return VX + VW / 2 + ((c + 0.5) / GRAIN_COLS - 0.5) * VW * sc


def tide_spot(sp):
    """A spot's picture is twice as wide as its spacing, so neighbouring arches overlap and blend."""
    return int(sp * 2) + 2


def arch_frame(w, h, y, colour, alpha, thick, fill, shine=False):
    """One spot of a ridge: a gentle arch at height y that fades out at both ends, so overlapping
    neighbours read as one rolling line; with fill, solid screen colour below it hides what is behind."""
    img = canvas(w, h)
    d = ImageDraw.Draw(img)
    n = 24
    pts = []
    for i in range(n + 1):
        u = i / n * 2 - 1                        # -1 .. 1 across the spot
        pts.append((i / n * w, y + 3.5 * u * u))  # a little lower at the ends
    if fill:
        d.polygon([(px * SS, py * SS) for px, py in pts] + [(w * SS, h * SS), (0, h * SS)], fill=SCR + (255,))
    for i in range(n):
        u = (i + 0.5) / n * 2 - 1
        fade = alpha * max(0.0, 1 - abs(u) ** 2.2)
        (x1, y1), (x2, y2) = pts[i], pts[i + 1]
        d.line([(x1 * SS, y1 * SS), (x2 * SS, y2 * SS)], fill=rgba(colour, fade), width=max(1, int(thick * SS)))
    out = glow(img, 3, 0.55) if shine else img
    return down(out)


def tide_strips():
    files = {}
    for b in range(TIDE_ROWS):
        _y, sc, amp, sp = tide_row(b)
        w, h = tide_spot(sp), int(amp) + 8
        a = 0.85 - b * 0.09                       # nearer ridges are brighter
        frames = []
        for k in range(TIDE_F):
            frames.append(arch_frame(w, h, h - 4 - k / (TIDE_F - 1) * amp, (210, 210, 210), a, 2.2 if b == 0 else 1.7, fill=True))
        files["tide_%d" % b] = strip(frames, "grn_tide_%d.png" % b)
    # the front row, lit inside the pick zone: frame 0 is empty (not in the zone)
    _y, sc, amp, sp = tide_row(0)
    w, h = tide_spot(sp), int(amp) + 8
    frames = [down(canvas(w, h))]   # frame 0: empty (at the final size, like every other frame)
    for k in range(TIDE_F):
        frames.append(arch_frame(w, h, h - 4 - k / (TIDE_F - 1) * amp, WHITE_N, 1.0, 3.2, fill=False, shine=True))
    files["tide_lit"] = strip([f if i == 0 else f for i, f in enumerate(frames)], "grn_tide_lit.png")
    # the ripple: a drop lands at the line's height, then a ring spreads out and fades
    rh = int(TIDE_AMP) + 2 * RIPPLE_PAD
    frames = [down(canvas(RIPPLE_W, rh))]   # frame 0: no ripple
    for hgt in range(8):
        cy = RIPPLE_PAD + TIDE_AMP - hgt / 7 * TIDE_AMP
        for sp_ in range(10):
            u = sp_ / 9
            img = canvas(RIPPLE_W, rh)
            d = ImageDraw.Draw(img)
            cx = RIPPLE_W / 2
            rx, ry = 6 + u * 62, 2 + u * 15
            d.ellipse([(cx - rx) * SS, (cy - ry) * SS, (cx + rx) * SS, (cy + ry) * SS],
                      outline=rgba(WHITE_N, 0.85 * (1 - u * 0.85)), width=2 * SS)
            if sp_ < 4:   # the drop itself, bright at first
                r = 5 - sp_
                d.ellipse([(cx - r) * SS, (cy - 4 - r) * SS, (cx + r) * SS, (cy - 4 + r) * SS], fill=rgba(WHITE_N, 1 - sp_ / 4))
            frames.append(down(glow(img, 4, 0.7)))
    files["ripple"] = strip(frames, "grn_ripple.png")
    return files


PIX_COLS, PIX_HEIGHTS, PIX_DISSOLVE = 128, 18, 8   # match the plugin
PIX_TOP, PIX_ROW = VY + 52, 5        # the pixel grid's top (under the browser row), and row pitch
PIX_MID = PIX_TOP + 17 * PIX_ROW + 3 # the centre row
PIX_W = 7                            # a pixel's width (columns are VW / 128 apart)
PIX_VARIANTS = 12                    # different random dissolve patterns, mixed across columns


def pix_x(c):
    return int(round(VX + c * VW / PIX_COLS + (VW / PIX_COLS - PIX_W) / 2))


def pixel_strips():
    """A column of the pixel waveform, one strip per random pattern: frame = height x 8 + dissolve.
    Height h lights h rows above and below the centre row; dissolve d drops pixels at random (the
    same ones first every time, so they fade away steadily) and dims what is left."""
    rows = 2 * (PIX_HEIGHTS - 1) + 1
    h_px = rows * PIX_ROW
    files = {}
    for v in range(PIX_VARIANTS):
        rnd = random.Random(1000 + v)
        thresh = [rnd.random() for _ in range(rows)]
        frames = []
        for hgt in range(PIX_HEIGHTS):
            for dis in range(PIX_DISSOLVE):
                img = Image.new("RGBA", (PIX_W + 1, h_px), (0, 0, 0, 0))
                d = ImageDraw.Draw(img)
                gone = dis / (PIX_DISSOLVE - 1) * 0.86           # share of pixels dropped (some always stay)
                a = 0.95 * (1 - 0.5 * dis / (PIX_DISSOLVE - 1))  # and the rest dim a little
                for r in range(rows):
                    off = r - (PIX_HEIGHTS - 1)                  # -17..17 around the centre
                    if abs(off) > hgt or thresh[r] < gone:
                        continue
                    y = r * PIX_ROW
                    d.rectangle([0, y, PIX_W - 1, y + PIX_ROW - 2], fill=rgba(WHITE_N, a))
                frames.append(img)
        files["pix_%d" % v] = strip(frames, "grn_pix_%d.png" % v)
    return files


def make_grain(_peaks):
    """The Grain picture: the sample's waveform as a grid of pixels (drawn live), with a faint grid
    of unlit pixels behind it."""
    art = Image.new("RGBA", (VW, VH), (0, 0, 0, 0))
    d = ImageDraw.Draw(art)
    rows = 2 * (PIX_HEIGHTS - 1) + 1
    for c in range(PIX_COLS):
        x = pix_x(c) - VX
        for r in range(rows):
            y = PIX_TOP - VY + r * PIX_ROW
            d.rectangle([x, y, x + PIX_W - 1, y + PIX_ROW - 2], fill=(255, 255, 255, 9))
    return art, pixel_strips()


SPEC_TOP, SPEC_H = VY + 52, 168      # the Filter spectrum's area (under the browser row): lines grow up from its bottom
SPEC_IN_W, SPEC_OUT_W = 15, 15       # line widths: the same, so each band is one line: white up to what comes out, dark above it


def spec_x(f):
    """Where frequency f sits across the picture: Cutoff's own scale, 20 Hz to 20 kHz, logarithmic."""
    return VX + math.log(f / 20.0) / math.log(1000.0) * VW


def make_filter():
    """The Filter picture: a faint frequency grid (the live spectrum is drawn by the plugin's values)."""
    art = Image.new("RGBA", (VW, VH), (0, 0, 0, 0))
    img = canvas(VW, VH)
    d = ImageDraw.Draw(img)
    f = label_font(14 * SS)
    for hz, lab in ((50, ""), (100, "100"), (200, ""), (500, ""), (1000, "1k"), (2000, ""), (5000, ""), (10000, "10k")):
        x = spec_x(hz) - VX
        d.line([(x * SS, (SPEC_TOP - VY) * SS), (x * SS, (SPEC_TOP - VY + SPEC_H) * SS)],
               fill=(255, 255, 255, 30 if lab else 14), width=SS)
        if lab:
            d.text((x * SS, (SPEC_TOP - VY + SPEC_H + 12) * SS), lab, font=f, fill=rgba(WHITE_N, 0.45), anchor="mm")
    d.line([(0, (SPEC_TOP - VY + SPEC_H) * SS), (VW * SS, (SPEC_TOP - VY + SPEC_H) * SS)], fill=(255, 255, 255, 26), width=SS)
    art.alpha_composite(down(img))
    files = {"cut": save(thumb_line(SPEC_H + 14, WHITE_N, 2, dash=5), "flt_cut.png")}
    # slope: low pass, band pass, high pass glyphs
    for k in range(3):
        img = canvas(90, 40)
        d = ImageDraw.Draw(img)
        pts = []
        for j in range(61):
            x = (j / 60 - 0.5) * 6
            g = (1 / (1 + math.exp(4 * x)), math.exp(-1.5 * x * x), 1 / (1 + math.exp(-4 * x)))[k]
            pts.append(((4 + j / 60 * 82) * SS, (36 - 28 * g) * SS))
        d.line(pts, fill=rgba(WAVE_GREY, 1), width=2 * SS)
        files["slope_%d" % k] = save(down(img), "flt_slope_%d.png" % k)
    return art, files


COLOR_TOP, COLOR_H = 48, 196   # the Color picture's band inside the visual (under the browser row)


COLOR_FRAMES = 8    # pattern frames per layer (picked by the setting's value); few, so the page loads quickly
COLOR_CELL = 4      # the pixel grid: one dot per 4 x 4 px


def color_fade(x, W):
    """Full brightness in the middle, fading to nothing at the left and right edges."""
    e = min(1.0, abs(x - W / 2) / (W / 2))
    t = max(0.0, (e - 0.25) / 0.75)
    return 1.0 - t * t * (3 - 2 * t)


def color_layer(kind, val, rnd):
    """One layer of the Color picture. Two sets of half circles spread from two points on the bottom
    edge and overlap, like waves from two sources; where their rings cross they make an
    interference pattern. Drive sets the left rings' spacing, Crush the right rings' spacing and
    how blocky their pixels are, and Noise scatters loose pixels over both."""
    W, H = VW, COLOR_H
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    if kind == "noise":   # loose pixels, more of them with more Noise
        for _ in range(int(1400 * val)):
            x = rnd.randrange(0, W // COLOR_CELL) * COLOR_CELL
            y = rnd.randrange(0, H // COLOR_CELL) * COLOR_CELL
            d.rectangle([x, y, x + COLOR_CELL - 2, y + COLOR_CELL - 2], fill=(236, 236, 236, int((70 + 110 * rnd.random()) * color_fade(x, W))))
        return img
    left = kind == "drive"
    sx = W * (0.36 if left else 0.64)          # the two sources, on the bottom edge
    wave = (36.0 - 14.0 * val) if left else (32.0 - 8.0 * val)
    cell = COLOR_CELL if left else COLOR_CELL + 2 * int(round(val * 2))   # Crush: bigger, blockier pixels
    a = 235 if left else 175
    for cy in range(0, H, cell):
        for cx in range(0, W, cell):
            r = math.hypot(cx + cell / 2 - sx, H - (cy + cell / 2))
            if r < wave * 0.5:
                continue
            k = r / wave
            if abs(k - round(k)) * wave < cell * 0.5:   # on a ring
                d.rectangle([cx, cy, cx + cell - 2, cy + cell - 2], fill=(236, 236, 236, int(a * color_fade(cx + cell / 2, W))))
    return img


def make_color():
    art = Image.new("RGBA", (VW, VH), (0, 0, 0, 0))
    files = {}
    for kind in ("drive", "noise", "crush"):
        rnd = random.Random(7)
        frames = [color_layer(kind, i / (COLOR_FRAMES - 1), rnd) for i in range(COLOR_FRAMES)]
        files[kind] = strip(frames, "col_%s.png" % kind)
    return art, files


# ---- Space: small wireframe projections (after the mockup's View)
def rot(p, yaw, pitch, roll=0.0):
    x, y, z = p
    c, s = math.cos(roll), math.sin(roll)
    x, y = x * c - y * s, x * s + y * c
    c, s = math.cos(yaw), math.sin(yaw)
    x, z = x * c + z * s, -x * s + z * c
    c, s = math.cos(pitch), math.sin(pitch)
    y, z = y * c - z * s, y * s + z * c
    return x, y, z


class View:
    def __init__(self, w, h, yaw, pitch, scale, cy=0.5):
        self.W, self.H, self.yaw, self.pitch, self.k, self.f = w, h, yaw, pitch, h * scale, 4.2
        self.cy = cy

    def proj(self, p):
        q = rot(p, self.yaw, self.pitch)
        s = self.f / (self.f + q[2])
        return self.W / 2 + q[0] * s * self.k, self.H * self.cy - q[1] * s * self.k, q[2]


def depth_a(z):
    return max(0.12, min(1.0, 0.95 - (z + 1.4) * 0.32))


def mesh(d, view, verts, edges, colour, alpha, width):
    P = [view.proj(v) for v in verts]
    for a, b in edges:
        A, B = P[a], P[b]
        da = depth_a((A[2] + B[2]) / 2) * alpha
        if da < 0.01:
            continue
        d.line([(A[0] * SS, A[1] * SS), (B[0] * SS, B[1] * SS)], fill=rgba(colour, da), width=max(1, int(width * SS)))


def ring(R, y, n=72, tilt=0.0):
    verts = []
    for i in range(n):
        u = i / n * 2 * math.pi
        q = rot((R * math.cos(u), 0, R * math.sin(u)), 0, 0, tilt)
        verts.append((q[0], q[1] + y, q[2]))
    return verts, [(i, (i + 1) % n) for i in range(n)]


def box(w, h, dd, y0):
    v = [(-w, y0, -dd), (w, y0, -dd), (w, y0, dd), (-w, y0, dd), (-w, y0 + h, -dd), (w, y0 + h, -dd), (w, y0 + h, dd),
         (-w, y0 + h, dd)]
    e = [(0, 1), (1, 2), (2, 3), (3, 0), (4, 5), (5, 6), (6, 7), (7, 4), (0, 4), (1, 5), (2, 6), (3, 7)]
    return v, e


SPACE_PANEL = 460


def make_space():
    S = SPACE_PANEL
    base = -0.35
    # static art: the delay floor disc on the left, the room floor grid on the right
    art = Image.new("RGBA", (VW, SPACE_H), (0, 0, 0, 0))
    left = canvas(S, SPACE_H)
    d = ImageDraw.Draw(left)
    view = View(S, SPACE_H, 0.35, 0.78, 0.44, cy=0.56)
    v, e = ring(1.15, base, 96)
    mesh(d, view, [(x + 0, y, z) for x, y, z in v], e, DEEP, 0.6, 1)
    for k in range(24):
        a = k / 24 * 2 * math.pi
        r0, r1 = (1.08, 1.3) if k % 6 == 0 else (1.15, 1.22)
        mesh(d, view, [(math.cos(a) * r0, base, math.sin(a) * r0), (math.cos(a) * r1, base, math.sin(a) * r1)], [(0, 1)],
             ICE if k % 6 == 0 else DEEP, 0.9 if k % 6 == 0 else 0.5, 1.1)
    art.alpha_composite(down(left), (int(VW * 0.3 - S / 2), 0))
    right = canvas(S, SPACE_H)
    d = ImageDraw.Draw(right)
    rview = View(S, SPACE_H, 0.62, 0.42, 0.33)
    w = 0.95
    y0 = -0.45
    G, GE = [], []
    for k in range(9):
        u = -w + k / 8 * 2 * w
        G += [(u, y0, -w), (u, y0, w), (-w, y0, u), (w, y0, u)]
        GE += [(len(G) - 4, len(G) - 3), (len(G) - 2, len(G) - 1)]
    mesh(d, rview, G, GE, DEEP, 0.45, 0.9)
    art.alpha_composite(down(right), (int(VW * 0.7 - S / 2), 0))

    files = {}
    # feedback: echo rings rising into a cone, more rings as feedback grows (film strip)
    frames = []
    for i in range(16):
        fb = i / 15
        img = canvas(S, SPACE_H)
        d = ImageDraw.Draw(img)
        n = 1 + int(round(fb * 9))
        for r in range(n):
            a = 1.0 if r == 0 else max(0.15, fb ** r * 0.9 + 0.15)
            R = 0.16 + r * 0.11
            tilt = 0 if r == 0 else (0.12 if r % 2 else -0.12)
            v, e = ring(R, base + 0.06 + r * 0.06, 56, tilt)
            mesh(d, view, v, e, ICE if r == 0 else (PINK if r % 2 else CYAN), min(1, 0.3 + 0.9 * a), 1.4 if r == 0 else 1)
        frames.append(down(glow(img, 3, 0.8)))
    files["feedback"] = strip(frames, "spc_feedback.png")
    files["feedback_pos"] = (int(VW * 0.3 - S / 2), 0)
    # size: the room box grows (film strip)
    frames = []
    for i in range(16):
        sz = i / 15
        img = canvas(S, SPACE_H)
        d = ImageDraw.Draw(img)
        bw = 0.5 + sz * 0.45
        hh = 0.45 + sz * 0.45
        v, e = box(bw, hh, bw, -hh / 2)
        mesh(d, rview, v, e, ICE, 0.95, 1.4)
        v, e = box(bw * 0.6, hh * 0.6, bw * 0.6, -hh / 2)
        mesh(d, rview, v, e, CYAN, 0.3, 0.8)
        frames.append(down(glow(img, 3, 0.7)))
    files["size"] = strip(frames, "spc_size.png")
    files["size_pos"] = (int(VW * 0.7 - S / 2), 0)
    # reverb: air in the room, revealed from the bottom
    rnd = random.Random(3)
    img = canvas(S, SPACE_H)
    d = ImageDraw.Draw(img)
    for _ in range(520):
        p = ((rnd.random() * 2 - 1) * 0.9, -0.42 + rnd.random() * 0.85, (rnd.random() * 2 - 1) * 0.9)
        q = rview.proj(p)
        a = depth_a(q[2]) * 0.8
        col = ICE if rnd.random() < 0.35 else (PINK if rnd.random() < 0.1 else CYAN)
        d.rectangle([q[0] * SS, q[1] * SS, (q[0] + 1.8) * SS, (q[1] + 1.8) * SS], fill=rgba(col, a))
    files["air"] = save(down(img), "spc_air.png")
    # freeze: a crystal shown while Freeze is on
    img = canvas(60, 60)
    d = ImageDraw.Draw(img)
    for k in range(6):
        a = k * math.pi / 3
        d.line([(30 * SS, 30 * SS), ((30 + 24 * math.cos(a)) * SS, (30 + 24 * math.sin(a)) * SS)],
               fill=rgba(ICE, 1), width=2 * SS)
        for t in (0.55,):
            bx, by = 30 + 24 * t * math.cos(a), 30 + 24 * t * math.sin(a)
            for s in (-1, 1):
                b = a + s * 0.7
                d.line([(bx * SS, by * SS), ((bx + 8 * math.cos(b)) * SS, (by + 8 * math.sin(b)) * SS)],
                       fill=rgba(ICE, 1), width=2 * SS)
    files["freeze"] = save(down(glow(img, 4, 1.0)), "spc_freeze.png")
    return art, files


# ------------------------------------------------------------------ JSON builders
def text_style(height, colour, just="left verticallyCentred", style="Regular", case="Original"):
    return {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": style, "height": float(height)},
            "colour": colour, "justification": just, "case": case}


def bounds(rect, focus="No", show="Show", invalidating=None, visible="Always"):
    b = {"version": 2 if invalidating else 1, "acceptsHWFocus": focus, "showWhenDataModelInvalid": show,
         "whenVisible": visible, "boundsType": "Absolute", "bounds": " ".join(str(int(round(v))) for v in rect)}
    if invalidating:
        b["additionalInvalidatingHandles"] = invalidating
    return b


def entry(name, ctype, data, rect, remap=None, **kw):
    return {"version": 2, "componentData": {"version": 1, "name": name, "type": ctype, "data": data},
            "handle remapping": {"version": 1, "map": [{"key": k, "value": v} for k, v in (remap or {}).items()]},
            "bounds": bounds(rect, **kw)}


def label_text(name, text, rect, height, colour, just="left verticallyCentred", style="Regular", case="Original"):
    return entry(name, "Label", {"version": 1, "textStyle": text_style(height, colour, just, style, case), "type": "Text",
                                 "handleName": "Invalid", "text": text}, rect)


def label_value(name, handle, rect, height, colour, just="right verticallyCentred", style="SemiBold"):
    return entry(name, "Label", {"version": 1, "textStyle": text_style(height, colour, just, style), "type": "Value",
                                 "handleName": handle}, rect)


def image(name, img, rect, invalidating=None):
    return entry(name, "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": img}, rect,
                 invalidating=invalidating, show="Show")


def film(name, img, frames, handle, rect):
    return entry(name, "Image", {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": img,
                                 "numFrames": frames, "handleName": handle}, rect)


def reveal(name, img, handle, rect, direction="Horizontal", rtype="Normal", invalidating=None):
    data = {"version": 4, "sliderType": "TrackAndThumb", "direction": direction, "thumbImage": "",
            "thumbTrackVisible": False, "revealedImage": img, "revealType": rtype, "handleName": handle}
    return entry(name, "Slider", data, rect, invalidating=invalidating)


def thumb(name, img, handle, rect, direction="Horizontal"):
    data = {"version": 4, "sliderType": "TrackAndThumb", "direction": direction, "thumbImage": img,
            "thumbTrackVisible": False, "revealedImage": "", "revealType": "Normal", "handleName": handle}
    return entry(name, "Slider", data, rect)


def button(name, on, off, handle, rect, focus="Yes", button_id=1, group=1):
    # Keys accept focus: on the MPC a control that cannot take focus may never get the tap.
    return entry(name, "Button", {"version": 2, "onImage": on, "offImage": off, "buttonId": button_id, "numButtonsInGroup": group,
                                  "handleName": handle, "gestureBehaviour": "Instant"}, rect, focus=focus)


def enabled(choice, n, param):
    return ["IndexedEnabling/%d/%d/Parameter %d" % (choice, n, param)]


def slot_component(kind, W=CELL_W):
    """A display cell: its name, value and bar; touching it banks the Q-Links to its row."""
    actions = [{"version": 1, "onAction": "Mouse Down", "handler": "Q-Link", "handleName": "QLink", "additionalData": ""},
               {"version": 1, "onAction": "Double Click", "handler": "Show Overlay", "handleName": "Data",
                "additionalData": "knob overlay"},
               {"version": 1, "onAction": "Enter Pressed", "handler": "Show Overlay", "handleName": "Data",
                "additionalData": "knob overlay"}]
    focus = entry("focus", "Focus", {"version": 1, "backgroundColour": "14ffffff", "outlineColour": "ff5fdcf3",
                                     "backgroundInset": 2.0, "outlineThickness": 1.0}, (0, 0, W, CELL_H),
                  visible="WhenFocussed")
    # The name comes from the bound parameter (a "Name" label): fixed-text labels inside a cell do not
    # draw on the MPC, parameter-bound ones do. The plugin names the view settings after the captions.
    name_label = label_value("name", "Data", (14, 2, 156, 48), 26, "ffd2d2d2", "left verticallyCentred", "SemiBold")
    name_label["componentData"]["data"]["type"] = "Name"
    name_label["componentData"]["data"]["textStyle"]["case"] = "Upper Case"
    children = [focus, name_label]
    children += [label_value("value", "Data", (W - 150, 0, 136, 52), 32, "ffffffff"),
                entry("bar", "Slider", {"version": 2, "sliderType": "ColourBar", "direction": "Horizontal",
                                            "backgroundColour": "10ffffff", "outlineColour": "0",
                                            "valueColour": "ffd6d6d6", "isBipolar": kind == "cellBi",
                                            "handleName": "Data"}, (14, 56, W - 28, 8))]
    return {"version": 2, "actions": actions,
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "0", "image": ""},
                               "unfocussed": {"version": 1, "colour": "0", "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": children}


KNOB_OVERLAY = {"version": 2, "actions": [], "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "ff080a0d", "image": ""},
                                                                 "unfocussed": {"version": 1, "colour": "ff080a0d", "image": ""}},
                "ignoreMousePresses": True, "disableCoarseDataWheel": False, "componentsData": [
                    {"version": 2, "componentData": {"version": 1, "name": "overlay", "type": "KnobOverlay", "data": {
                        "version": 1, "actions": [{"version": 1, "onAction": "Focus Lost", "handler": "Focus Lost",
                                                   "handleName": "Focus Lost", "additionalData": ""}],
                        "overlayType": "Knob With Header", "scaleType": "Linear", "polarityType": "Unipolar",
                        "showMinusSign": True, "handleName": "Data"}},
                     "handle remapping": {"version": 1, "map": []},
                     "bounds": {"version": 1, "acceptsHWFocus": "No", "showWhenDataModelInvalid": "Show",
                                "whenVisible": "Always", "boundsType": "Absolute", "bounds": "0 0 1280 800"}}]}


WAVE0 = FILE_ROW0 + LIST_ROWS   # 334..461: the waveform columns the plugin writes
WAVE_COLS = 128
LOOP_END = WAVE0 + WAVE_COLS + 5   # 467: where the loop ends (written by the plugin)
ROTATE_AT = LOOP_END + 1           # 468: where Rotate makes it begin (0 = hidden)
GRAIN_COL0 = WAVE0 + WAVE_COLS + 7 + 16 + 4 + 4 + 1 + 4 + 1   # 499..562: written by the plugin
GRAIN_COLS = 64
GRAIN_HOT0 = GRAIN_COL0 + GRAIN_COLS    # 563..626: the recording, lit inside the pick zone only
GRAIN_HIT0 = GRAIN_HOT0 + GRAIN_COLS    # 627..690: a full bar where a grain is playing
GRAIN_PIX0 = GRAIN_HIT0 + GRAIN_COLS     # 691..818: the pixel waveform, written by the plugin
PAGE0 = GRAIN_PIX0 + 128 + 2 * 64        # 947..950: which module page each track's tab shows
KNOB0 = PAGE0 + 4                        # 951..998: each track's 12 knobs (they follow the page's cells)
N_KNOBS = 12


def knob_idx(t, k):
    return KNOB0 + t * N_KNOBS + k


SPEC_BANDS = 64
SPEC_IN0 = GRAIN_PIX0 + 128              # 819..882: the Filter screen, sound going into the filter
SPEC_OUT0 = SPEC_IN0 + SPEC_BANDS        # 883..946: ...and coming out of it


def wave_bars(name, rect, colour, alpha, bg_file, base=None, ncols=None):
    """The waveform as 128 mirrored bars. Each column's top half is a bar that grows up from the centre
    line. A screen bar can only grow from its bottom, so the lower half is a bar in the waveform colour
    that the column covers from below with the background colour, leaving the waveform's height showing
    under the centre line."""
    x0, y0, w, h = rect
    bg = Image.open(os.path.join(OUT, bg_file)).convert("RGB")
    mid = y0 + h // 2
    half = h // 2
    base = WAVE0 if base is None else base
    ncols = WAVE_COLS if ncols is None else ncols
    col_w = w / ncols
    bar_w = max(2, int(col_w) - 2)
    wave = "%02x%02x%02x%02x" % (int(alpha * 255), colour[0], colour[1], colour[2])
    E = []
    for c in range(ncols):
        x = int(round(x0 + c * col_w + (col_w - bar_w) / 2))
        # the background behind this column's lower half: what the cover must match
        px = [bg.getpixel((min(1279, x + i), min(627, mid + 4 + j))) for i in range(bar_w) for j in range(0, half - 4, 4)]
        cover = "ff%02x%02x%02x" % tuple(int(sum(p[k] for p in px) / len(px)) for k in range(3))
        handle = "Parameter %d" % (base + c)
        up = entry("%s up %d" % (name, c), "Slider", {"version": 2, "sliderType": "ColourBar", "direction": "Vertical",
                                                    "backgroundColour": "0", "outlineColour": "0", "valueColour": wave,
                                                    "isBipolar": False, "handleName": handle}, (x, mid - half, bar_w, half))
        down_ = entry("%s down %d" % (name, c), "Slider", {"version": 2, "sliderType": "ColourBar", "direction": "Vertical",
                                                         "backgroundColour": wave, "outlineColour": "0", "valueColour": cover,
                                                         "isBipolar": False, "handleName": "Invert/" + handle}, (x, mid, bar_w, half))
        E += [up, down_]
    return E


def visual_entries(m, t, files):
    """The moving layers of module m's visual on track t."""
    P = lambda off: "Parameter %d" % vidx(off)
    E = []
    if m == 0:   # SOURCE
        # the real waveform of the edited track's sound, drawn from the plugin's 128 columns
        E += wave_bars("wave", (VX, SRC_TOP, VW, SRC_H), WAVE_GREY, 0.92, files["bg"])
        # dim everything before START
        E.append(reveal("before start", files["dim"], P(START), (VX, SRC_TOP, VW, SRC_H)))
        # ... and everything after the loop's end (the plugin writes where that is)
        E.append(reveal("after end", files["dim"], "Parameter %d" % LOOP_END, (VX, SRC_TOP, VW, SRC_H), rtype="Inverted"))
        E.append(reveal("speed", files["speed"], P(0), (VX, VY + WAVE_H + 4, VW, 30), rtype="Bipolar"))
        # the playhead: where the track is playing, moved by the plugin; hidden while nothing plays
        ph = thumb("playhead", files["playhead"], "Parameter %d" % PLAYHEAD, (VX - 13, SRC_TOP - 6, VW + 26, SRC_H + 14))
        ph["bounds"] = bounds((VX - 13, SRC_TOP - 6, VW + 26, SRC_H + 14), invalidating=["NonZero/Parameter %d" % PLAYHEAD])
        E.append(ph)
        E.append(thumb("start", files["start"], P(START), (VX - 13, SRC_TOP - 6, VW + 26, SRC_H + 14)))
        E.append(thumb("end", files["end"], "Parameter %d" % LOOP_END, (VX - 13, SRC_TOP - 6, VW + 26, SRC_H + 14)))
        # Rotate: where in the loop playback begins; hidden while Rotate is 0
        ra = thumb("rotate", files["rotate"], "Parameter %d" % ROTATE_AT, (VX - 13, SRC_TOP - 6, VW + 26, SRC_H + 14))
        ra["bounds"] = bounds((VX - 13, SRC_TOP - 6, VW + 26, SRC_H + 14), invalidating=["NonZero/Parameter %d" % ROTATE_AT])
        E.append(ra)
        for k in range(5):   # the tape modes show the same glyph as Fwd / Ping
            E.append(image("loop %d" % k, files["loop_%d" % (k if k < 3 else k - 2)], (VX + VW - 74, SRC_TOP + 2, 70, 22),
                           invalidating=enabled(k, 5, vidx(6))))
    elif m == 1:  # GRAIN: the sample's waveform as pixels, solid where grains pick, dissolving around it
        rows = 2 * (PIX_HEIGHTS - 1) + 1
        for c in range(PIX_COLS):
            v = (c * 7) % PIX_VARIANTS
            E.append(film("pix %d" % c, files["pix_%d" % v], PIX_HEIGHTS * PIX_DISSOLVE, "Parameter %d" % (GRAIN_PIX0 + c),
                          (pix_x(c), PIX_TOP, PIX_W + 1, rows * PIX_ROW)))
    elif m == 2:  # FILTER
        # the live spectrum, 64 bands from 20 Hz to 20 kHz: a wide dim line for the sound going into
        # the filter, a thin bright one on top for what comes out (the gap is what the filter took)
        col = VW / SPEC_BANDS
        for name, base, colour, alpha, bw in (("in", SPEC_IN0, (58, 58, 58), 1.0, SPEC_IN_W),
                                              ("out", SPEC_OUT0, WHITE_N, 1.0, SPEC_OUT_W)):
            c_hex = "%02x%02x%02x%02x" % (int(alpha * 255), colour[0], colour[1], colour[2])
            for b_ in range(SPEC_BANDS):
                x = int(round(VX + (b_ + 0.5) * col - bw / 2))
                E.append(entry("spec %s %d" % (name, b_), "Slider",
                               {"version": 2, "sliderType": "ColourBar", "direction": "Vertical", "backgroundColour": "0",
                                "outlineColour": "0", "valueColour": c_hex, "isBipolar": False,
                                "handleName": "Parameter %d" % (base + b_)}, (x, SPEC_TOP, bw, SPEC_H)))
        # where Cutoff sits, on the same scale
        E.append(thumb("cutoff", files["cut"], P(16), (VX - 13, SPEC_TOP - 7, VW + 26, SPEC_H + 14)))
        for k in range(3):
            E.append(image("slope %d" % k, files["slope_%d" % k], (VX + VW - 94, SPEC_TOP - 4, 90, 40),
                           invalidating=enabled(k, 3, vidx(20))))
    elif m == 3:  # COLOR
        # three layers of pixel contour lines, each picked by its setting; their overlap makes the
        # interference pattern
        rect = (VX, VY + COLOR_TOP, VW, COLOR_H)
        E.append(film("crush", files["crush"], COLOR_FRAMES, P(26), rect))
        E.append(film("noise", files["noise"], COLOR_FRAMES, P(28), rect))
        E.append(film("drive", files["drive"], COLOR_FRAMES, P(24), rect))
    # SPACE has no picture: its three effect columns fill the screen
    # visuals never take hardware focus
    for e in E:
        e["bounds"]["acceptsHWFocus"] = "No"
    return E


def browse_slot(w, h):
    """A focusable value (folder or file name): touching it banks the Q-Links to the browser row."""
    focus = entry("focus", "Focus", {"version": 1, "backgroundColour": "14ffffff", "outlineColour": "ff5fdcf3",
                                     "backgroundInset": 1.0, "outlineThickness": 1.0}, (0, 0, w, h), visible="WhenFocussed")
    # the caption ("FOLDER" / "FILE") is the setting's own name: a Name label draws inside a control
    caption = label_value("caption", "Data", (0, 4, w, 20), 15, MUTED, "horizontallyCentred verticallyCentred", "Regular")
    caption["componentData"]["data"]["type"] = "Name"
    caption["componentData"]["data"]["textStyle"]["case"] = "Upper Case"
    value = label_value("value", "Data", (0, 0, w, h), 32, "ffffffff", "horizontallyCentred verticallyCentred", "SemiBold")
    return {"version": 2,
            "actions": [{"version": 1, "onAction": "Mouse Down", "handler": "Q-Link", "handleName": "Data", "additionalData": ""}],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "0", "image": ""},
                               "unfocussed": {"version": 1, "colour": "0", "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": True, "componentsData": [focus, caption, value]}


FOLDER_SLOT_W = FOLDER_PLATE[2] - 2 * NAME_X
FILE_SLOT_W = FILE_PLATE[2] - 2 * NAME_X
SLOT_Y, SLOT_H = 0, BR_H      # the name fills the plate between the < > keys, centred


def list_row(w):
    """One row of the list browser. It carries two handles:
      Data - the list's own Folder or File setting: a tapped row takes focus, so the data wheel and the
             Q-Links (banked by the tap) scroll that list
      Row  - this row's text, written by the plugin; a tap also flips it (the highlighted file row loads)"""
    value = label_value("name", "Row", (14, 0, w - 28, ROW_H), 32, "ffffffff", "left verticallyCentred", "Regular")
    return {"version": 2,
            "actions": [{"version": 1, "onAction": "Mouse Down", "handler": "Q-Link", "handleName": "Data", "additionalData": ""},
                        {"version": 1, "onAction": "Mouse Down", "handler": "Toggle Switch", "handleName": "Row", "additionalData": ""}],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "0", "image": ""},
                               "unfocussed": {"version": 1, "colour": "0", "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": True, "componentsData": [value]}


def browser_entries(t, files):
    E = []
    kw, kh = STEP_KEY
    for (x, y, w, h), caption, prev, nxt, val, slot in (
            (FOLDER_PLATE, "FOLDER", B_FOLD_PREV, B_FOLD_NEXT, B_FOLDER, "browseFolder"),
            (FILE_PLATE, "FILE", B_FILE_PREV, B_FILE_NEXT, B_FILE, "browseFile")):
        E.append(button("%s prev" % caption, files["prev_0"], files["prev_0"],
                        "Parameter %d" % vbidx(prev), (x + 4, y + 4, kw, kh)))
        E.append(entry("%s value" % caption, slot, {"version": 1, "handleName": "Data"},
                       (x + NAME_X, y + SLOT_Y, w - 2 * NAME_X, SLOT_H),
                       remap={"Data": "Parameter %d" % vbidx(val)}, focus="Yes"))
        E.append(button("%s next" % caption, files["next_0"], files["next_0"],
                        "Parameter %d" % vbidx(nxt), (x + w - kw - 4, y + 4, kw, kh)))
    lx, ly, lw, lh = LIST_KEY
    E.append(button("list", files["list_1"], files["list_0"], "Parameter %d" % BROWSER_OPEN, (lx, ly, lw, lh)))
    return E


def list_entries(files):
    """The open list browser: drawn last on the Source tab so it covers everything under the browser row."""
    E = []
    open_ = enabled(1, 2, BROWSER_OPEN)
    E.append(image("list panel", files["panel"], (VX, LIST_TOP - 2, VW, LIST_BOTTOM - LIST_TOP + 2), invalidating=open_))
    step_h = LIST_ROWS * ROW_H // 2 - 6
    steps = ((B_FOLD_PREV, B_FOLD_NEXT), (B_FILE_PREV, B_FILE_NEXT))
    for pane, ((px, pw), row0) in enumerate(zip(PANES, (FOLDER_ROW0, FILE_ROW0))):
        for k in range(LIST_ROWS):
            E.append(entry("list row %d %d" % (pane, k), "listRow%d" % pane, {"version": 1, "handleName": "Data"},
                           (px + 6, LIST_TOP + k * ROW_H, pw - STEP_W - 12, ROW_H),
                           remap={"Data": "Parameter %d" % vbidx(B_FOLDER if pane == 0 else B_FILE),
                                  "Row": "Parameter %d" % (row0 + k)},
                           focus="Yes", invalidating=open_))
        # one item up / down per tap
        kx = px + pw - STEP_W + 3
        for j, (b, img) in enumerate(((steps[pane][0], files["up"]), (steps[pane][1], files["down"]))):
            key = button("list %s %d" % ("up" if j == 0 else "down", pane), img, img, "Parameter %d" % vbidx(b),
                         (kx, LIST_TOP + 4 + j * (step_h + 8), STEP_W - 6, step_h))
            key["bounds"] = bounds((kx, LIST_TOP + 4 + j * (step_h + 8), STEP_W - 6, step_h), focus="Yes", invalidating=open_)
            E.append(key)
    return E


def view_map(t):
    """The shared view settings, and track t's own settings they stood for."""
    v = {VIEW_BASE + off: pidx(t, off) for off in range(PER_TRACK)}
    v.update({VIEW_BASE + PER_TRACK + b: bidx(t, b) for b in range(PER_BROWSE)})
    v[VIEW_ROOT] = VIEW_ROOT - 4 + t
    v[vidx(START)] = vidx(START) - 4 + t
    for k in range(4):
        v[vidx(CHORUS + k)] = vidx(CHORUS) - 16 + t * 4 + k
    v[vidx(REVMIX)] = vidx(REVMIX) - 4 + t
    v[vidx(REVTYPE)] = vidx(REVTYPE) - 4 + t
    return v


def retarget(obj, vmap):
    """Point every 'Parameter N' that names a view setting at the track's own setting."""
    if isinstance(obj, dict):
        return {k: retarget(v, vmap) for k, v in obj.items()}
    if isinstance(obj, list):
        return [retarget(v, vmap) for v in obj]
    if isinstance(obj, str):
        return re.sub(r"Parameter (\d+)", lambda mo: "Parameter %d" % vmap.get(int(mo.group(1)), int(mo.group(1))), obj)
    return obj


def on_page(entries, m, t):
    """Show these entries only while track t's tab is on module page m."""
    cond = "IndexedEnabling/%d/5/Parameter %d" % (m, PAGE0 + t)
    for e in entries:
        b = e["bounds"]
        b["version"] = 2
        b["additionalInvalidatingHandles"] = b.get("additionalInvalidatingHandles", []) + [cond]
    return entries


def page_key_x(k):
    """The five module page keys sit at the right end of the key row."""
    return key_x(6 + k)


def build_main_tab(files, keys):
    """The SCULPT tab: one page per module (only the picked one shows). Track keys 1-4 pick the track
    everything on screen works on (the view, the knobs and the pictures follow it); the module keys
    pick the page; ON/OFF and Sync / Lock / Freeze work the page's module on that track."""
    C = []
    for m, (title, cells, on_off, toggle) in enumerate(MODULES):
        P = [image("page bg %d" % m, files[m]["bg"], (0, 0, 1280, 628))]
        P += visual_entries(m, 0, files[m])
        for i, (name, off, bip, _steps) in enumerate(cells):
            x, y, w, h = cell_rect(m, i)
            comp = ("cellBi" if bip else "cellUni") + ("Wide" if w != CELL_W else "")
            P.append(entry("%s %d" % (comp, i), comp, {"version": 1, "handleName": "Data"}, (x, y, w, h),
                           remap={"Data": "WithName[%s]/Parameter %d" % (name.title(), knob_idx(0, i)),
                                  "QLink": "Parameter %d" % knob_idx(0, i)}, focus="Yes"))
        P.append(button("on key", keys["on_on"], keys["on_off"], "Parameter %d" % vidx(on_off), KEY_RECT(key_x(4))))
        if toggle:
            tag = toggle[1].lower()
            P.append(button("toggle key", keys["tg_%s_on" % tag], keys["tg_%s_off" % tag], "Parameter %d" % vidx(toggle[0]),
                            KEY_RECT(key_x(5))))
        C += on_page(P, m, 0)
    for k in range(4):   # one tap picks the track; the picked one is lit
        C.append(button("track key %d" % k, keys["trk_on_%d" % k], keys["trk_off_%d" % k], "Parameter %d" % EDIT_TRACK,
                        KEY_RECT(key_x(k)), button_id=k, group=4))
    for k in range(5):   # one tap shows the module's page
        C.append(button("page key %d" % k, keys["mod_on_%d" % k], keys["mod_off_%d" % k], "Parameter %d" % PAGE0,
                        KEY_RECT(page_key_x(k)), button_id=k, group=5))
    # the browser row sits at the top of every page
    B = browser_entries(0, files[0])
    for e in B:   # it reads FOLDER and FILE
        kind = e["componentData"]["type"]
        if kind in ("browseFolder", "browseFile"):
            for pair in e["handle remapping"]["map"]:
                if pair["key"] == "Data" and "WithName" not in pair["value"]:
                    pair["value"] = "WithName[%s]/%s" % ("Folder" if kind == "browseFolder" else "File", pair["value"])
    C += B
    C += list_entries(files[0])   # last, so the open browser covers everything
    root = {"version": 2, "actions": [],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "ff101010", "image": files[0]["bg"]},
                               "unfocussed": {"version": 1, "colour": "ff101010", "image": files[0]["bg"]}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": C}
    return {"version": 1, "tabName": "SCULPT", "fnKeyIndex": 0, "fnKeySubIndex": 0, "componentDefinition": root,
            "qlinkBoundsData": []}


# ------------------------------------------------------------------ the MIX tab
MIX_BASE = VIEW_ROOT + 1            # 291: per track Volume, Pan, Mute, Solo, Meter
MASTER_METER = MIX_BASE + 4 * 5     # 311
M_VOL, M_PAN, M_MUTE, M_SOLO, M_METER = range(5)
MIX_MIN_DB, MIX_MAX_DB = -60.0, 6.0


def midx(t, m):
    return MIX_BASE + t * 5 + m


STRIP_W, MASTER_X, MASTER_W = 246, 1064, 184
FT, FH = 128, 336                   # fader / meter area: top and height
CAP_W, CAP_H = 64, 30


def strip_x(t):
    return VX + t * (STRIP_W + 12)


def db_y(db):
    return FT + FH * (1.0 - (db - MIX_MIN_DB) / (MIX_MAX_DB - MIX_MIN_DB))


def make_mix():
    """Static art for the MIX tab: strip plates, fader grooves and dB ticks. Returns (art, files)."""
    art = canvas(1280, 628)
    d = ImageDraw.Draw(art)
    for k in range(5):
        x, w = (strip_x(k), STRIP_W) if k < 4 else (MASTER_X, MASTER_W)
        d.rounded_rectangle([x * SS, 56 * SS, (x + w) * SS, 604 * SS], 8 * SS, fill=(255, 255, 255, 9))
        mx = x + 24
        gx = x + (130 if k < 4 else 120)
        d.rounded_rectangle([(gx - 2) * SS, FT * SS, (gx + 2) * SS, (FT + FH) * SS], 2 * SS, fill=(0, 0, 0, 255))
        for db in (6, 0, -6, -12, -24, -36, -48, -60):
            y = db_y(db)
            d.line([(mx + 18) * SS, y * SS, (mx + 26) * SS, y * SS], fill=rgba(DEEP, 0.9 if db in (0, -12, -24, -48) else 0.5), width=SS)
            d.line([(gx - 12) * SS, y * SS, (gx - 6) * SS, y * SS], fill=rgba(DEEP, 0.6), width=SS)
            d.line([(gx + 6) * SS, y * SS, (gx + 12) * SS, y * SS], fill=rgba(DEEP, 0.6), width=SS)
        y0 = db_y(0)
        d.line([(gx - 16) * SS, y0 * SS, (gx + 16) * SS, y0 * SS], fill=rgba(CYAN, 0.6), width=SS)
    art = down(art)
    files = {}
    cap = canvas(CAP_W, CAP_H)
    cd = ImageDraw.Draw(cap)
    cd.rounded_rectangle([2 * SS, 2 * SS, (CAP_W - 2) * SS, (CAP_H - 2) * SS], 5 * SS, fill=(44, 44, 44, 255),
                         outline=(10, 10, 10, 255), width=SS)
    cd.line([6 * SS, CAP_H / 2 * SS, (CAP_W - 6) * SS, CAP_H / 2 * SS], fill=rgba(CYAN, 1), width=2 * SS)
    files["cap"] = save(down(glow(cap, 2, 0.5)), "mix_cap.png")
    for lit in (0, 1):
        files["mute_%d" % lit] = save(small_key(106, 36, lit, "MUTE"), "mix_mute_%d.png" % lit)
        files["solo_%d" % lit] = save(small_key(106, 36, lit, "SOLO"), "mix_solo_%d.png" % lit)
    return art, files


def fader_slot():
    """A vertical fader (thumb on an invisible track); touching it banks the Q-Links."""
    h = FH + CAP_H
    focus = entry("focus", "Focus", {"version": 1, "backgroundColour": "10ffffff", "outlineColour": "ff5fdcf3",
                                     "backgroundInset": 0.0, "outlineThickness": 1.0}, (0, 0, CAP_W, h), visible="WhenFocussed")
    slider = entry("fader", "Slider", {"version": 4, "sliderType": "TrackAndThumb", "direction": "Vertical",
                                       "thumbImage": "mix_cap.png", "thumbTrackVisible": False, "revealedImage": "",
                                       "revealType": "Normal", "handleName": "Data"}, (0, 0, CAP_W, h))
    return {"version": 2,
            "actions": [{"version": 1, "onAction": "Mouse Down", "handler": "Q-Link", "handleName": "Data", "additionalData": ""},
                        {"version": 1, "onAction": "Double Click", "handler": "Show Overlay", "handleName": "Data",
                         "additionalData": "knob overlay"}],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "0", "image": ""},
                               "unfocussed": {"version": 1, "colour": "0", "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": [focus, slider]}


def pan_slot(w):
    focus = entry("focus", "Focus", {"version": 1, "backgroundColour": "10ffffff", "outlineColour": "ff5fdcf3",
                                     "backgroundInset": 0.0, "outlineThickness": 1.0}, (0, 0, w, 40), visible="WhenFocussed")
    value = label_value("value", "Data", (0, 0, w, 26), 20, LIT)
    bar = entry("bar", "Slider", {"version": 2, "sliderType": "ColourBar", "direction": "Horizontal",
                                  "backgroundColour": "10ffffff", "outlineColour": "0", "valueColour": "ffd6d6d6",
                                  "isBipolar": True, "handleName": "Data"}, (0, 32, w, 6))
    return {"version": 2,
            "actions": [{"version": 1, "onAction": "Mouse Down", "handler": "Q-Link", "handleName": "Data", "additionalData": ""},
                        {"version": 1, "onAction": "Double Click", "handler": "Show Overlay", "handleName": "Data",
                         "additionalData": "knob overlay"}],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "0", "image": ""},
                               "unfocussed": {"version": 1, "colour": "0", "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": [focus, value, bar]}


def meter(name, param, x):
    return entry(name, "Slider", {"version": 2, "sliderType": "ColourBar", "direction": "Vertical",
                                  "backgroundColour": "ff05070a", "outlineColour": "ff1a2230", "valueColour": "ff5fdcf3",
                                  "isBipolar": False, "handleName": "Parameter %d" % param}, (x, FT, 14, FH))


def build_mix_tab(files):
    C = [label_text("title", "SCULPT  ·  MIX", (32, 14, 400, 34), 22, "ffa6a6a6")]
    strips = [(strip_x(t), STRIP_W, "TRACK %d" % (t + 1), t) for t in range(4)] + [(MASTER_X, MASTER_W, "MASTER", None)]
    for x, w, title, t in strips:
        C.append(label_text("name " + title, title, (x + 14, 58, w - 28, 28), 20, BRIGHT, style="SemiBold"))
        if t is not None:
            C.append(label_value("sound " + title, "Parameter %d" % pidx(t, 7), (x + 14, 86, w - 28, 26), 18, DIMTXT,
                                 "left verticallyCentred", "Regular"))
        C.append(meter("meter " + title, midx(t, M_METER) if t is not None else MASTER_METER, x + 24))
        for db in (6, 0, -12, -24, -48):
            C.append(label_text("db %s %d" % (title, db), ("+%d" % db) if db > 0 else str(db), (x + 50, db_y(db) - 9, 36, 18),
                                14, MUTED))
        gx = x + (130 if t is not None else 120)
        vol = midx(t, M_VOL) if t is not None else 0
        C.append(entry("fader " + title, "faderSlot", {"version": 1, "handleName": "Data"},
                       (gx - CAP_W // 2, FT - CAP_H // 2, CAP_W, FH + CAP_H), remap={"Data": "Parameter %d" % vol}, focus="Yes"))
        C.append(label_value("volume " + title, "Parameter %d" % vol, (x + 14, 480, w - 28, 30), 24, LIT,
                             "horizontallyCentred verticallyCentred"))
        if t is None:
            continue
        C.append(label_text("pan label " + title, "PAN", (x + 14, 514, 44, 26), 16, DIMTXT))
        C.append(entry("pan " + title, "panSlot", {"version": 1, "handleName": "Data"}, (x + 60, 514, w - 74, 40),
                       remap={"Data": "Parameter %d" % midx(t, M_PAN)}, focus="Yes"))
        C.append(button("mute " + title, files["mute_1"], files["mute_0"], "Parameter %d" % midx(t, M_MUTE),
                        (x + 14, 562, 106, 36)))
        C.append(button("solo " + title, files["solo_1"], files["solo_0"], "Parameter %d" % midx(t, M_SOLO),
                        (x + 126, 562, 106, 36)))
    root = {"version": 2, "actions": [],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "ff101010", "image": files["bg"]},
                               "unfocussed": {"version": 1, "colour": "ff101010", "image": files["bg"]}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": C}
    q = ["%d %d %d %d" % (strip_x(0) + 80, FT - 20, 4 * (STRIP_W + 12) - 20, FH + 40),
         "%d %d %d %d" % (strip_x(0) + 8, 508, 4 * (STRIP_W + 12) - 20, 52)]
    return {"version": 1, "tabName": "MIX", "fnKeyIndex": 1, "fnKeySubIndex": 0, "componentDefinition": root,
            "qlinkBoundsData": q}


def replacement_strings():
    rs = []
    for t in range(4):
        for off, ch in CHOICES.items():
            rs.append({"key": "Parameter %d" % pidx(t, off), "value": ch})
        for off in BOOL_OFFSETS:
            rs.append({"key": "Parameter %d" % pidx(t, off), "value": ["Off", "On"]})
    for k in range(4):
        rs.append({"key": "Parameter %d" % (1 + k), "value": ["Off", "On"]})
        for b in (B_LOAD, B_FOLD_PREV, B_FOLD_NEXT, B_FILE_PREV, B_FILE_NEXT, B_HAS_FILE):
            rs.append({"key": "Parameter %d" % bidx(k, b), "value": ["Off", "On"]})
    for t in range(4):
        for m in (M_MUTE, M_SOLO):
            rs.append({"key": "Parameter %d" % midx(t, m), "value": ["Off", "On"]})
    rs.append({"key": "Parameter %d" % BROWSER_OPEN, "value": ["Off", "On"]})
    # the view and Edit Track
    rs.append({"key": "Parameter %d" % EDIT_TRACK, "value": ["1", "2", "3", "4"]})
    for off, ch in CHOICES.items():
        rs.append({"key": "Parameter %d" % vidx(off), "value": ch})
    # reverb Type: per track (494..497) and the view's
    for t in range(4):
        rs.append({"key": "Parameter %d" % (vidx(REVTYPE) - 4 + t), "value": REV_TYPES})
    rs.append({"key": "Parameter %d" % vidx(REVTYPE), "value": REV_TYPES})
    rs.append({"key": "Parameter %d" % PAGE0, "value": ["Source", "Grain", "Filter", "Color", "Space"]})
    for off in BOOL_OFFSETS:
        rs.append({"key": "Parameter %d" % vidx(off), "value": ["Off", "On"]})
    for b in (B_LOAD, B_FOLD_PREV, B_FOLD_NEXT, B_FILE_PREV, B_FILE_NEXT, B_HAS_FILE):
        rs.append({"key": "Parameter %d" % vbidx(b), "value": ["Off", "On"]})
    return rs


def qlinks():
    m = []
    # the SCULPT tab: the screen's knobs, page cells 1-4, 5-8, 9-12 (Source's 9-10 browse folders and files)
    q = {"Q-Link %d" % n: -1 for n in range(1, 17)}
    for i in range(N_KNOBS):
        q["Q-Link %d" % (13 - 4 * (i // 4) + i % 4)] = knob_idx(0, i)
    m.append({"Tab": 1, "SubTab": 1, "Bank Direction": "RowTopDown", "Q-Links": q})
    # MIX: the four volumes, then the four pans, then the master volume
    q = {"Q-Link %d" % n: -1 for n in range(1, 17)}
    for t in range(4):
        q["Q-Link %d" % (13 + t)] = midx(t, M_VOL)
        q["Q-Link %d" % (9 + t)] = midx(t, M_PAN)
    q["Q-Link 5"] = 0
    m.append({"Tab": 2, "SubTab": 1, "Bank Direction": "RowTopDown", "Q-Links": q})
    prog = {}
    # program Q-Links: the four track volumes, then cutoff, reverb send and speed per track
    for t in range(4):
        prog["Q-Link %d" % (1 + t)] = midx(t, M_VOL)
    n = 5
    for off in (16, None, 0):   # cutoff, reverb mix, speed
        for t in range(4):
            prog["Q-Link %d" % n] = pidx(t, off) if off is not None else vidx(REVMIX) - 4 + t
            n += 1
    return {"version": 4, "info": {"version": 1, "type": "CompleteDescription"},
            "Screen Mode Q-Links": {"version": 4, "map": m}, "Program Mode Q-Links": prog}


def main():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".png") or f.endswith(".json"):
            os.remove(os.path.join(OUT, f))

    names = ["1 Kalimba", "2 Tape", "3 Haze", "4 Glass"]
    peaks = [read_peaks(os.path.join(SAMPLES, n + ".wav"), 404) for n in names]

    files = []
    for m, maker in enumerate([lambda: make_source(peaks), lambda: make_grain(peaks[0]), make_filter, make_color, lambda: (Image.new("RGBA", (VW, VH), (0, 0, 0, 0)), {})]):
        art, f = maker()
        f["bg"] = save(background(art, overlay=f.pop("overlay", None), m=m),
                       "bg_%s.png" % MODULES[m][0].lower())
        files.append(f)
        print("module", MODULES[m][0], "->", len(f), "images")

    keys = {"key_on": save(key_image(True), "key_on.png"), "key_off": save(key_image(False), "key_off.png"),
            "round_on": save(key_image(True, round_key=True), "round_on.png"),
            "round_off": save(key_image(False, round_key=True), "round_off.png")}
    keys["on_on"] = save(key_image(True, icon_word("ON")), "key_onoff_on.png")
    keys["on_off"] = save(key_image(False, icon_word("OFF")), "key_onoff_off.png")
    for word in ("SYNC", "LOCK", "FREEZE"):
        keys["tg_%s_on" % word.lower()] = save(key_image(True, icon_word(word)), "key_%s_on.png" % word.lower())
        keys["tg_%s_off" % word.lower()] = save(key_image(False, icon_word(word)), "key_%s_off.png" % word.lower())
    for k in range(4):
        keys["trk_on_%d" % k] = save(key_image(True, icon_digit(k + 1)), "trk_%d_on.png" % k)
        keys["trk_off_%d" % k] = save(key_image(False, icon_digit(k + 1)), "trk_%d_off.png" % k)
    for k in range(5):
        keys["mod_on_%d" % k] = save(key_image(True, ICONS[k]), "mod_%d_on.png" % k)
        keys["mod_off_%d" % k] = save(key_image(False, ICONS[k]), "mod_%d_off.png" % k)

    tabs = [build_main_tab(files, keys)]
    mix_art, mix_files = make_mix()
    mix_files["bg"] = save(background(mix_art, cells=False, badge=False, art_at=(0, 0)), "bg_mix.png")
    tabs.append(build_mix_tab(mix_files))
    defs = [{"key": k, "value": slot_component(k)} for k in ("cellUni", "cellBi")]
    defs += [{"key": k + "Wide", "value": slot_component(k, COL_W)} for k in ("cellUni", "cellBi")]
    defs.append({"key": "browseFolder", "value": browse_slot(FOLDER_SLOT_W, SLOT_H)})
    defs.append({"key": "browseFile", "value": browse_slot(FILE_SLOT_W, SLOT_H)})
    defs.append({"key": "faderSlot", "value": fader_slot()})
    for pane, (_px, pw) in enumerate(PANES):
        defs.append({"key": "listRow%d" % pane, "value": list_row(pw - STEP_W - 12)})
    defs.append({"key": "panSlot", "value": pan_slot(STRIP_W - 74)})
    defs.append({"key": "knob overlay", "value": KNOB_OVERLAY})
    tui = {"pageData": {"version": 1,
                        "componentDefinitions": {"version": 2, "importFiles": [], "localComponentDefinitions": defs},
                        "replacementStrings": replacement_strings(),
                        "info": {"version": 1, "type": "CompleteDescription"},
                        "tabs": tabs}}
    with open(os.path.join(OUT, "TUI.json"), "w") as fh:
        json.dump(tui, fh, indent=1)
    with open(os.path.join(OUT, "Q-Links.json"), "w") as fh:
        json.dump(qlinks(), fh, indent=1)
    total = sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT))
    print("wrote %d tabs, %d files, %.1f MB into skins/" % (len(tabs), len(os.listdir(OUT)), total / 1e6))


if __name__ == "__main__":
    main()

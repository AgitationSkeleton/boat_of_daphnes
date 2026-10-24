#!/usr/bin/env python3
"""Make the data the game needs to recolour Link at run time, from the player's own files.

    python link_colors.py <GZLE01 iso> <output folder> [--masks <wwrando link_color_masks folder>]

The runtime recolours Link the way the Wind Waker Randomizer does (wwrando, MIT,
Copyright (c) 2018 LagoLunatic; its custom colour system and its colour masks).
It needs two things that are not code and are not in this repository:

  - the vanilla pixels of the textures wwrando recolours, decoded to RGBA, and
  - wwrando's colour masks (one picture per colour, pure red FF0000 marks the
    pixels that colour owns).

This script reads the textures straight from the disc (files/res/Object/Link.arc)
and the masks from the player's own wwrando download, and writes a folder:

    index.json            what is in the folder (see below)
    <texture>.rgba        u32 width, u32 height (little endian), then RGBA8 rows, top row first
    masks/*.mask          u32 width, u32 height (little endian), then one byte per pixel, 1 = red in the mask

index.json lists every texture with the renderer's replacement key and every
mask with its outfit ("hero" or "casual"), its colour name (exactly as wwrando
names it) and its texture. The key is the stem of the file name the renderer's
Dolphin-format texture replacement uses,

    tex1_<W>x<H>_<texhash>[_<tluthash>]_<GX format>

so the runtime can register a recoloured texture under it. The hashes follow
find_replacement_for_guest in
runtime/recompcore/GXRuntime/graphics/aurora/lib/gfx/texture_replacement.cpp:
XXH64 of the base level bytes and, for palette formats, XXH64 of only the
palette entries the texture references (lowest to highest index used).

The output comes from the player's disc and the player's wwrando download. It
is never committed to the repository, and the script writes only into the
output folder it is given.

The disc and archive readers, the hash and the colour decoders are in
swift_sail.py. This file adds the other GX texture formats Link uses (C4 and the
rest) and the wwrando mask mapping. Standard library plus Pillow only.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import sys as _bod_sys
from pathlib import Path as _BodPath
_bod_sys.path.insert(0, str(_BodPath(__file__).resolve().parents[1]))
try:
    from bod_paths import local_path, scratch  # noqa: E402  (scripts/bod_paths.py)
except ImportError:  # a release's copy of this script, without the repository's scripts/ beside it
    import os as _bod_os

    def local_path(name, fallback=""):
        return _bod_os.environ.get(name) or fallback

    def scratch():
        return _BodPath(local_path("BOD_SCRATCH", "scratch"))

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swift_sail as S  # noqa: E402

DEFAULT_MASKS = str(_BodPath(local_path("BOD_WWRANDO")) / "assets" / "link_color_masks") if local_path("BOD_WWRANDO") else ""
LINK_ARC = "res/Object/Link.arc"
SHIP_ARC = "res/Object/Ship.arc"
ZL_ARC = "res/Object/Zl.arc"  # Tetra (the texture inside is named zelda_all)
PZ_ARC = "res/Object/Pz.arc"  # Princess Zelda

OUTFITS = ("hero", "casual")

# Textures to export, in index order: (name, archive member, texture name inside it or None for a bare BTI).
TEXTURES = (
    [("linktexS3TC", "bdl/cl.bdl", "linktexS3TC"),
     ("hitomi", "bdl/cl.bdl", "hitomi")]
    + [("mouthS3TC.%d" % n, "bdl/cl.bdl", "mouthS3TC.%d" % n) for n in range(1, 10)]
    + [("handsS3TC", "bdl/hands.bdl", "handsS3TC"),
       ("katsuraS3TC", "bdl/katsura.bdl", "katsuraS3TC"),
       ("linktexbci4", "tex/linktexbci4.bti", None)]
)

# Equipment (Cosmetics Editor > Equipment): no wwrando masks, the runtime shifts a whole texture's hue.
# Listed in index.json under "equipment".
EQUIPMENT = (
    ("SWAS3TC", "bdl/swa.bdl", "SWAS3TC"),
    ("SWgripA", "bdl/swgripa.bdl", "SWgripA"),
    ("SWMSA", "bdlm/swms.bdl", "SWMSA"),
    ("SWMSBS3TC", "bdlm/swms.bdl", "SWMSBS3TC"),
    ("SWgripAMSCI4", "bdlm/swgripms.bdl", "SWgripAMSCI4"),
    ("SWgripBMS", "bdlm/swgripms.bdl", "SWgripBMS"),
    ("SHA", "bdl/sha.bdl", "SHA"),
    ("SHgripAS3TC", "bdl/sha.bdl", "SHgripAS3TC"),
    ("SHinA", "bdl/sha.bdl", "SHinA"),
    ("SHMSACI4", "bdlm/shms.bdl", "SHMSACI4"),
    ("SHMSBI4", "bdlm/shms.bdl", "SHMSBI4"),
    ("SHMSCCI8", "bdlm/shms.bdl", "SHMSCCI8"),
    # The King of Red Lions (res/Object/Ship.arc, a 4th field names the archive): hull, figurehead and the sail's picture.
    ("ShipHull", "bdl/fn_body.bdl", "fn_main1", SHIP_ARC),
    ("ShipHead", "bdl/fn_head_h.bdl", "fn_head1", SHIP_ARC),
    ("ShipSail", "tex/new_ho1.bti", None, SHIP_ARC),
    # The rest of Link's gear (Boat of Daphnes on the PC port; Zelda's bow in Pz.arc shares the bow's texture).
    ("podAS3TC", "bdl/cl.bdl", "podAS3TC"),
    ("podMSCI4", "bdl/podms.bdl", "podMSCI4"),
    ("BowS3TC", "bdl/bow.bdl", "BowS3TC"),
    ("arrowS3TC", "bdlm/arrow.bdl", "arrowS3TC"),
    ("Boomerang", "bdl/boomerang.bdl", "Boomerang"),
    ("hookshotCI8", "bdl/hookshot.bdl", "hookshotCI8"),
    ("fanAS3TC", "bdl/fan.bdl", "fanAS3TC"),
    ("fanBS3TC", "bdl/fan.bdl", "fanBS3TC"),
    ("fanCS3TC", "bdl/fansmall.bdl", "fanCS3TC"),
    ("Bomb", "bdlm/bomb.bdl", "Bomb"),
    ("Telescoe", "bdlc/telescope.bdl", "Telescoe"),
    ("PRingS3TC", "bdl/pring.bdl", "PRingS3TC"),
    # The Skull Hammer and Iron Boots, the grappling hook's rope end, the baton, and the picto box, tuner and pear (BR-0029).
    ("hammerCI8", "bdl/hammer.bdl", "hammerCI8"),
    ("HeavybootsI4", "bdlc/hboots.bdl", "HeavybootsI4"),
    ("ropeEND", "bdlc/ropeend.bdl", "ropeEND"),
    ("taktCI4", "bdlm/takt.bdl", "taktCI4"),
    ("cameraS3TC", "bdl/camera.bdl", "cameraS3TC"),
    ("TceiverCI8", "bdl/tceiver.bdl", "TceiverCI8"),
    ("hyoinomiS3TC", "bdl/hyoinomi.bdl", "hyoinomiS3TC"),
)

# Which texture the body mask of each outfit paints.
BODY_TEXTURE = {"hero": "linktexS3TC", "casual": "linktexbci4"}

# wwrando gives a texture with no mask of its own the colour named here (its
# {hero,casual}_{hands,...}_color_name defaults, assets/link_metadata.txt has no override).
# The whole texture is that colour, so the runtime gets a mask of all ones.
DEFAULT_COLOR = {("hands", "hero"): ("handsS3TC", "Skin"),
                 ("hands", "casual"): ("handsS3TC", "Skin"),
                 ("katsura", "casual"): ("katsuraS3TC", "Hair")}


class LinkColorsError(S.SailError):
    pass


# ---------------------------------------------------------------------------
# GX texture formats beyond what swift_sail.py needs (C8 and CMPR).
# ---------------------------------------------------------------------------

FMT_I4, FMT_I8, FMT_IA4, FMT_IA8 = 0, 1, 2, 3
FMT_RGB565, FMT_RGB5A3, FMT_RGBA8 = 4, 5, 6
FMT_C4, FMT_C8, FMT_C14X2, FMT_CMPR = 8, 9, 10, 14
PALETTE_FORMATS = (FMT_C4, FMT_C8, FMT_C14X2)
PAL_IA8 = 0

# format -> (tile width, tile height, bytes per tile). Same sizes the renderer
# uses for the base level (GXGetTexBufferSize with no mipmaps).
TILE = {FMT_I4: (8, 8, 32), FMT_I8: (8, 4, 32), FMT_IA4: (8, 4, 32), FMT_IA8: (4, 4, 32),
        FMT_RGB565: (4, 4, 32), FMT_RGB5A3: (4, 4, 32), FMT_RGBA8: (4, 4, 64),
        FMT_C4: (8, 8, 32), FMT_C8: (8, 4, 32), FMT_C14X2: (4, 4, 32), FMT_CMPR: (8, 8, 32)}

FORMAT_NAMES = {0: "I4", 1: "I8", 2: "IA4", 3: "IA8", 4: "RGB565", 5: "RGB5A3", 6: "RGBA8",
                8: "C4", 9: "C8", 10: "C14X2", 14: "CMPR"}


class Tex(S.Bti):
    """One BTI header with the base level image and palette bytes, any GX format Link uses."""

    def __init__(self, blob: bytes, at: int = 0):
        h = blob[at:at + 32]
        if len(h) < 32:
            raise LinkColorsError("truncated BTI header")
        self.format = h[0]
        self.width, self.height = struct.unpack_from(">HH", h, 2)
        self.palette_format = h[9]
        self.num_colors = struct.unpack_from(">H", h, 10)[0]
        pal_off = struct.unpack_from(">I", h, 12)[0]
        img_off = struct.unpack_from(">I", h, 28)[0]
        if self.format not in TILE:
            raise LinkColorsError("unsupported GX format %d" % self.format)
        tw, th, ts = TILE[self.format]
        size = ((self.width + tw - 1) // tw) * ((self.height + th - 1) // th) * ts
        self.image = blob[at + img_off:at + img_off + size]
        if len(self.image) != size:
            raise LinkColorsError("truncated BTI image")
        self.palette = b""
        if self.format in PALETTE_FORMATS:
            self.palette = blob[at + pal_off:at + pal_off + self.num_colors * 2]
            if len(self.palette) != self.num_colors * 2:
                raise LinkColorsError("truncated BTI palette")

    def used_indices(self) -> tuple:
        """(lowest, highest) palette index in the image bytes. The renderer scans every byte
        of the base level, padding included, so this does too."""
        if self.format == FMT_C4:
            lo = min(min(b & 15, b >> 4) for b in set(self.image))
            hi = max(max(b & 15, b >> 4) for b in set(self.image))
        elif self.format == FMT_C8:
            lo, hi = min(self.image), max(self.image)
        else:  # C14X2: 14 bit index in the low bits of each 16 bit word
            vals = [v & 0x3FFF for (v,) in struct.iter_unpack(">H", self.image)]
            lo, hi = min(vals), max(vals)
        return lo, hi

    def key(self) -> str:
        """The renderer's replacement key (file name stem, no extension)."""
        parts = ["tex1_%dx%d" % (self.width, self.height), S.xxh64_hex(self.image)]
        if self.format in PALETTE_FORMATS:
            lo, hi = self.used_indices()
            end = (hi + 1) * 2
            if end > len(self.palette):
                raise LinkColorsError("palette too short for the indices the texture uses")
            parts.append(S.xxh64_hex(self.palette[lo * 2:end]))
        parts.append(str(self.format))
        return "_".join(parts)


def _ia8_to_rgba(v: int) -> tuple:
    return (v & 255, v & 255, v & 255, v >> 8)


def palette_rgba(tex: Tex) -> list:
    if tex.palette_format == PAL_IA8:
        return [_ia8_to_rgba(v) for (v,) in struct.iter_unpack(">H", tex.palette)]
    return S.palette_colors(tex.palette, tex.palette_format)


def decode_rgba(tex: Tex) -> bytes:
    """The texture as RGBA8, rows top to bottom (width * height * 4 bytes)."""
    w, h, fmt = tex.width, tex.height, tex.format
    if fmt == FMT_CMPR:
        return bytes(c for p in S.decode_cmpr(tex.image, w, h) for c in p)
    tw, th, ts = TILE[fmt]
    pal = palette_rgba(tex) if fmt in PALETTE_FORMATS else None
    out = bytearray(w * h * 4)
    img = tex.image
    pos = 0
    for ty in range(0, h, th):
        for tx in range(0, w, tw):
            blk = img[pos:pos + ts]
            pos += ts
            for i in range(tw * th):
                x, y = tx + i % tw, ty + i // tw
                if x >= w or y >= h:
                    continue
                if fmt == FMT_I4:
                    v = (blk[i >> 1] >> 4) if not i & 1 else blk[i >> 1] & 15
                    px = (S._e4(v),) * 4
                elif fmt == FMT_I8:
                    px = (blk[i],) * 4
                elif fmt == FMT_IA4:
                    px = (S._e4(blk[i] & 15),) * 3 + (S._e4(blk[i] >> 4),)
                elif fmt == FMT_IA8:
                    px = (blk[2 * i + 1],) * 3 + (blk[2 * i],)
                elif fmt == FMT_RGB565:
                    px = S.rgb565_to_rgba((blk[2 * i] << 8) | blk[2 * i + 1])
                elif fmt == FMT_RGB5A3:
                    px = S.rgb5a3_to_rgba((blk[2 * i] << 8) | blk[2 * i + 1])
                elif fmt == FMT_RGBA8:
                    px = (blk[2 * i + 1], blk[32 + 2 * i], blk[33 + 2 * i], blk[2 * i])
                elif fmt == FMT_C4:
                    v = (blk[i >> 1] >> 4) if not i & 1 else blk[i >> 1] & 15
                    px = pal[v]
                elif fmt == FMT_C8:
                    px = pal[blk[i]]
                else:  # C14X2
                    px = pal[((blk[2 * i] << 8) | blk[2 * i + 1]) & 0x3FFF]
                o = (y * w + x) * 4
                out[o:o + 4] = bytes(px)
    return bytes(out)


# ---------------------------------------------------------------------------
# Finding the textures in Link.arc.
# ---------------------------------------------------------------------------

def bdl_headers(blob: bytes, name: str) -> list:
    """Every TEX1 header called `name` in a J3D2 model (the list can hold the same name more than once)."""
    if blob[:4] != b"J3D2" or blob[4:8] not in (b"bdl4", b"bmd3", b"bmd2"):
        raise LinkColorsError("not a J3D2 model")
    count = struct.unpack_from(">I", blob, 12)[0]
    at = 32
    found = []
    for _ in range(count):
        magic, size = struct.unpack_from(">4sI", blob, at)
        if size < 8:
            raise LinkColorsError("bad J3D chunk")
        if magic == b"TEX1":
            _n, hdr, strs = struct.unpack_from(">HxxII", blob, at + 8)
            tbl = at + strs
            nn = struct.unpack_from(">H", blob, tbl)[0]
            for i in range(nn):
                so = struct.unpack_from(">H", blob, tbl + 6 + i * 4)[0]
                s = tbl + so
                if blob[s:blob.index(b"\0", s)].decode("shift_jis") == name:
                    found.append(Tex(blob, at + hdr + i * 32))
        at += size
    if not found:
        raise LinkColorsError("texture %r not found in model" % name)
    return found


def load_textures(disc: S.Disc, warn, table=None) -> list:
    """[(name, Tex)] for every texture in TEXTURES, one per distinct key.
    A name whose headers disagree about the pixels is a surprise: the extra
    versions are dropped with a warning (only the first is exported)."""
    archives = {}
    out = []
    for entry in (TEXTURES if table is None else table):
        name, member, tex_name = entry[:3]
        arc = entry[3] if len(entry) > 3 else LINK_ARC
        if arc not in archives:
            archives[arc] = S.open_archive(disc, arc)
        files = archives[arc]
        if member not in files:
            raise LinkColorsError("%s has no member %s" % (arc, member))
        if tex_name is None:
            headers = [Tex(files[member])]
        else:
            headers = bdl_headers(files[member], tex_name)
        keys = []
        for t in headers:
            k = t.key()
            if k not in keys:
                keys.append(k)
                if len(keys) == 1:
                    out.append((name, t))
        if len(keys) > 1:
            warn("%s has %d headers with different pixels; exporting the first (%s)" % (name, len(keys), keys[0]))
    return out


# ---------------------------------------------------------------------------
# Masks.
# ---------------------------------------------------------------------------

RED = (255, 0, 0, 255)


def load_mask(path: str):
    """(width, height, bytes) with 1 where the pixel is pure red FF0000FF, else 0."""
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    raw = im.tobytes()
    data = bytearray(im.width * im.height)
    for i in range(len(data)):
        if tuple(raw[i * 4:i * 4 + 4]) == RED:
            data[i] = 1
    return im.width, im.height, bytes(data)


PATTERNS = (
    ("body", re.compile(r"^(hero|casual)_(.+)\.png$", re.I)),
    ("hands", re.compile(r"^hands_(hero|casual)_(.+)\.png$", re.I)),
    ("hitomi", re.compile(r"^hitomi_(hero|casual)_(.+)\.png$", re.I)),
)
MOUTH = re.compile(r"^(mouthS3TC\.[1-9])_(.+)\.png$", re.I)


def find_masks(folder: str, warn) -> list:
    """[(outfit, color, texture name, png path)] for every mask file wwrando ships.
    The mouth masks apply to both outfits, so they appear once per outfit."""
    found = []
    for fn in sorted(os.listdir(folder)):
        path = os.path.join(folder, fn)
        if not os.path.isfile(path) or not fn.lower().endswith(".png"):
            continue
        for kind, rx in PATTERNS:
            m = rx.match(fn)
            if m:
                outfit, color = m.group(1).lower(), m.group(2)
                tex = {"body": BODY_TEXTURE[outfit], "hands": "handsS3TC", "hitomi": "hitomi"}[kind]
                found.append((outfit, color, tex, path))
                break
        else:
            warn("ignoring unrecognised file %s" % fn)
    mouths = os.path.join(folder, "mouths")
    if os.path.isdir(mouths):
        for fn in sorted(os.listdir(mouths)):
            m = MOUTH.match(fn)
            if not m:
                if fn.lower().endswith(".png"):
                    warn("ignoring unrecognised file mouths/%s" % fn)
                continue
            for outfit in OUTFITS:
                found.append((outfit, m.group(2), "mouthS3TC" + m.group(1)[len("mouthS3TC"):],
                              os.path.join(mouths, fn)))
    return found


def safe(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9.]+", "_", s)


def write_sized(path: str, width: int, height: int, data: bytes):
    with open(path, "wb") as f:
        f.write(struct.pack("<II", width, height))
        f.write(data)


# ---------------------------------------------------------------------------
# Tetra and Princess Zelda (Cosmetics Editor > Tetra / Princess Zelda).
# ---------------------------------------------------------------------------
# Each character's textures (the clothes atlas, the iris, the white of the eye, the eyebrows, the mouths, Zelda's eye
# shadow) are exported with one mask per part, found by looking at the pixels (hue, saturation, brightness, and a few
# rectangles of the atlas where two parts share a tone). The runtime shifts each masked part to the chosen colour with the
# same exchange it uses for Link. index.json lists them under "chars": per texture its key, its file and its parts
# ({id, mask file, base colour}); the editor's ids are "Equip.<Tetra|Zelda>.<Part>".

import colorsys

CHARS = (
    # (id prefix, archive, model, texture-name prefix)
    ("Tetra", ZL_ARC, "bdlm/zl.bdl", "zelda_"),
    ("Zelda", PZ_ARC, "bdlm/pz.bdl", "pz_"),
)
CHAR_STEMS = ("all", "hitomi", "eye.", "mayu.", "mouth.", "shadow.")


def _hsv_px(raw: bytes, w: int, h: int) -> list:
    """[(h 0-360, s 0-100, v 0-100, alpha)] per pixel."""
    out = []
    for i in range(w * h):
        r, g, b, a = raw[i * 4:i * 4 + 4]
        hh, ss, vv = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
        out.append((hh * 360.0, ss * 100.0, vv * 100.0, a))
    return out


def _hue_in(hh: float, lo: float, hi: float) -> bool:
    return lo <= hh <= hi if lo <= hi else (hh >= lo or hh <= hi)


def _atlas_parts(char: str, w: int, h: int, hsv: list) -> dict:
    """{part: bytearray mask} for the clothes atlas. Pixel (x, y) is hsv[y * w + x]."""
    names = (("Skin", "Hair", "Bandana", "Shirt", "Trim", "Leather", "Gold") if char == "Tetra"
             else ("Skin", "Hair", "Gold", "Dress", "Blue", "Cloth", "Emblem"))
    masks = {n: bytearray(w * h) for n in names}
    for y in range(h):
        for x in range(w):
            hh, s, v, a = hsv[y * w + x]
            part = None
            if char == "Tetra":
                if 4 <= x < 61 and y < 42:
                    part = "Skin"
                elif x < 4:
                    part = None  # the lavender padding column
                elif _hue_in(hh, 58, 95) and s >= 50:
                    part = "Hair"
                elif _hue_in(hh, 330, 12) and s >= 60:
                    part = "Bandana"
                elif _hue_in(hh, 195, 240) and s >= 55:
                    part = "Trim"
                elif _hue_in(hh, 38, 56) and s >= 75 and v >= 50:
                    part = "Gold"
                elif _hue_in(hh, 15, 46) and s >= 30 and v < 50:
                    part = "Leather"
                elif _hue_in(hh, 25, 52) and 25 <= s < 75 and v >= 50 and x >= 60:
                    part = "Skin"
                elif s < 30 and v >= 62 and ((x >= 62 and y < 42) or (x < 38 and y >= 88)):
                    part = "Shirt"
            else:
                if (x < 39 and y < 28) or (x < 10 and y >= 84):
                    part = "Skin" if hh < 45 and v > 55 else None
                elif _hue_in(hh, 58, 95) and s >= 50:
                    part = "Hair"
                elif _hue_in(hh, 38, 58) and s >= 60:
                    part = "Gold"
                elif _hue_in(hh, 195, 240) and s >= 30:
                    part = "Blue"
                elif s < 30 and v >= 78 and x >= 105 and y >= 64:
                    part = "Emblem"
                elif s < 30 and v >= 62 and ((x < 39 and 28 <= y < 84) or (48 <= x < 60 and 64 <= y < 84)):
                    part = "Cloth"
                elif _hue_in(hh, 250, 345) and s >= 8:
                    part = "Dress"
            if part:
                masks[part][y * w + x] = 1
    return masks


def _median(vals: list) -> int:
    vals = sorted(vals)
    return vals[len(vals) // 2]


def _median_rgb(raw: bytes, mask) -> int:
    px = [i for i, m in enumerate(mask) if m and raw[i * 4 + 3] > 128]
    if not px:
        return 0
    return ((_median([raw[i * 4] for i in px]) << 16) | (_median([raw[i * 4 + 1] for i in px]) << 8)
            | _median([raw[i * 4 + 2] for i in px]))


def _split_by_background(raw: bytes, w: int, h: int, far: int, bg_part: str, fg_part: str) -> dict:
    """A face texture on a flat background (mouth and eye shadow: skin; iris: the white). The background colour is the
    median of the four corners; pixels within `far` of it (sum of channel differences) are the background."""
    c = [raw[((y * w + x) * 4):((y * w + x) * 4) + 3] for (x, y) in ((1, 1), (w - 2, 1), (1, h - 2), (w - 2, h - 2))]
    bg = [_median([p[k] for p in c]) for k in range(3)]
    bgm, fgm = bytearray(w * h), bytearray(w * h)
    for i in range(w * h):
        d = sum(abs(raw[i * 4 + k] - bg[k]) for k in range(3))
        (bgm if d <= far else fgm)[i] = 1
    return {bg_part: bgm, fg_part: fgm}


def char_texture_parts(char: str, stem: str, raw: bytes, w: int, h: int) -> dict:
    """{part: mask} for one texture of a character; {} when no part is on it."""
    if stem == "all":
        return _atlas_parts(char, w, h, _hsv_px(raw, w, h))
    if stem == "hitomi":
        return _split_by_background(raw, w, h, 90, "Sclera", "Eyes")
    if stem == "eye.":
        hsv = _hsv_px(raw, w, h)
        white = sum(1 for (_h, s, v, a) in hsv if a > 128 and v > 85 and s < 15)
        return {"Sclera": bytearray(1 if a > 0 else 0 for (_h, _s, _v, a) in hsv)} if white > 300 else {}
    if stem == "mayu.":
        return {"Hair": bytearray(1 if raw[i * 4 + 3] > 0 else 0 for i in range(w * h))}
    if stem == "mouth.":
        return _split_by_background(raw, w, h, 40, "Skin", "Lips")
    if stem == "shadow." and char == "Zelda":
        return _split_by_background(raw, w, h, 30, "Skin", "Eyeshadow")
    return {}


def _tex_names(blob: bytes) -> list:
    names = []
    count = struct.unpack_from(">I", blob, 12)[0]
    at = 32
    for _ in range(count):
        magic, size = struct.unpack_from(">4sI", blob, at)
        if magic == b"TEX1":
            _n, _hdr, strs = struct.unpack_from(">HxxII", blob, at + 8)
            tbl = at + strs
            nn = struct.unpack_from(">H", blob, tbl)[0]
            for i in range(nn):
                so = struct.unpack_from(">H", blob, tbl + 6 + i * 4)[0]
                s = tbl + so
                nm = blob[s:blob.index(b"\0", s)].decode("shift_jis")
                if nm not in names:
                    names.append(nm)
        at += size
    return names


def load_char_textures(disc) -> list:
    """[(char, texture name, Tex, stem)] for every distinct texture of Tetra and Zelda that may have parts to colour."""
    out = []
    for char, arc, model, prefix in CHARS:
        blob = S.open_archive(disc, arc)[model]
        for stem in CHAR_STEMS:
            for name in _tex_names(blob):
                if not name.startswith(prefix + stem):
                    continue
                seen = []
                for t in bdl_headers(blob, name):
                    if t.width < 32 or t.height < 32:
                        continue  # the 8x8 stand-ins the model also lists
                    k = t.key()
                    if k not in seen:
                        seen.append(k)
                        out.append((char, name, t, stem))
    return out


def char_items(disc) -> list:
    """[(char, name, Tex, rgba, {part: mask}, bases)] with bases {(char, part): 0xRRGGBB} shared across the character."""
    items = []
    bases = {}
    for char, name, t, stem in load_char_textures(disc):
        raw = decode_rgba(t)
        parts = {p: m for p, m in char_texture_parts(char, stem, raw, t.width, t.height).items() if any(m)}
        if not parts:
            continue
        items.append((char, name, t, raw, parts))
        for p, m in parts.items():
            # A part's reference colour: the clothes atlas for the clothes, hair and skin; the iris texture for the eyes.
            if (char, p) not in bases or stem in ("all", "hitomi"):
                bases[(char, p)] = _median_rgb(raw, m)
    return items, bases


def build_chars(disc, out_dir: str) -> list:
    """Write the characters' textures and masks into out_dir/chars; returns the index's "chars" list."""
    items, bases = char_items(disc)
    os.makedirs(os.path.join(out_dir, "chars"), exist_ok=True)
    entries = []
    for n, (char, name, t, raw, parts) in enumerate(items):
        fn = "chars/%s_%d.rgba" % (safe(name), n)
        write_sized(os.path.join(out_dir, fn), t.width, t.height, raw)
        plist = []
        for p, m in sorted(parts.items()):
            mf = "chars/%s_%d_%s.mask" % (safe(name), n, p)
            write_sized(os.path.join(out_dir, mf), t.width, t.height, bytes(m))
            plist.append({"id": "Equip.%s.%s" % (char, p), "mask": mf, "base": "%06X" % bases[(char, p)]})
        entries.append({"name": name, "key": t.key(), "width": t.width, "height": t.height, "file": fn, "parts": plist})
    return entries


# ---------------------------------------------------------------------------
# The build.
# ---------------------------------------------------------------------------

def build(iso_path: str, out_dir: str, masks_dir: str = None, warn=None) -> dict:
    """Write the folder. Returns the index (also written as index.json)."""
    if warn is None:
        def warn(msg):
            print("link_colors: warning: " + msg, file=sys.stderr)
    disc = S.Disc(iso_path)
    try:
        if disc.game_id != "GZLE01" or disc.revision != 0:
            raise LinkColorsError("expected GZLE01 rev 0 (USA), found %s rev %d" % (disc.game_id, disc.revision))
        textures = load_textures(disc, warn)
        equipment = load_textures(disc, warn, EQUIPMENT)
        os.makedirs(out_dir, exist_ok=True)
        chars = build_chars(disc, out_dir)
    finally:
        disc.close()

    os.makedirs(os.path.join(out_dir, "masks"), exist_ok=True)
    index = {"version": 1, "textures": [], "masks": []}
    sizes = {}
    for name, t in textures:
        fn = name + ".rgba"
        write_sized(os.path.join(out_dir, fn), t.width, t.height, decode_rgba(t))
        sizes[name] = (t.width, t.height)
        index["textures"].append({"name": name, "key": t.key(), "width": t.width, "height": t.height, "file": fn})

    index["chars"] = chars
    index["equipment"] = []
    for name, t in equipment:
        fn = name + ".rgba"
        write_sized(os.path.join(out_dir, fn), t.width, t.height, decode_rgba(t))
        index["equipment"].append({"name": name, "key": t.key(), "width": t.width, "height": t.height, "file": fn})

    if masks_dir is None or not os.path.isdir(masks_dir):
        warn("wwrando colour masks folder not found (%s); writing textures only" % masks_dir)
    else:
        written = {}  # png path -> mask file (the mouth masks are shared by both outfits)
        wanted = find_masks(masks_dir, warn)
        have = set()
        for outfit, color, tex, path in wanted:
            if tex not in sizes:
                warn("%s: texture %s was not exported; skipped" % (os.path.basename(path), tex))
                continue
            if path not in written:
                w, h, data = load_mask(path)
                if (w, h) != sizes[tex]:
                    warn("%s is %dx%d but %s is %dx%d; skipped" % (
                        os.path.basename(path), w, h, tex, sizes[tex][0], sizes[tex][1]))
                    written[path] = None
                    continue
                rel = "masks/%s.mask" % (safe(tex + "_" + color) if tex.startswith("mouth")
                                         else safe(outfit + "_" + color + "_" + tex))
                write_sized(os.path.join(out_dir, rel), w, h, data)
                written[path] = rel
            if written[path] is None:
                continue
            have.add((tex, outfit))
            index["masks"].append({"outfit": outfit, "color": color, "texture": tex, "file": written[path]})
        # Textures wwrando colours without a mask of their own: the whole texture takes
        # a named colour (hands: Skin, casual back hair: Hair). A mask of all ones does that.
        for (_kind, outfit), (tex, color) in DEFAULT_COLOR.items():
            if tex in sizes and (tex, outfit) not in have:
                w, h = sizes[tex]
                rel = "masks/%s_all.mask" % tex
                write_sized(os.path.join(out_dir, rel), w, h, b"\x01" * (w * h))
                index["masks"].append({"outfit": outfit, "color": color, "texture": tex, "file": rel})

    with open(os.path.join(out_dir, "index.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(index, f, indent=1)
        f.write("\n")
    return index


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Make Link's recolour data from your own disc and wwrando's colour masks.")
    ap.add_argument("iso", help="path to the GZLE01 (USA) disc image")
    ap.add_argument("out", help="output folder (index.json, textures, masks/)")
    ap.add_argument("--masks", default=DEFAULT_MASKS,
                    help="wwrando assets/link_color_masks folder (default: %(default)s)")
    args = ap.parse_args(argv)
    try:
        index = build(args.iso, args.out, args.masks)
    except S.SailError as e:
        print("link_colors: " + str(e), file=sys.stderr)
        return 2
    except OSError as e:
        print("link_colors: " + str(e), file=sys.stderr)
        return 1
    for t in index["textures"]:
        fmt = int(t["key"].rsplit("_", 1)[1])
        print("%-12s %-5s %4dx%-3d %s" % (t["name"], FORMAT_NAMES[fmt], t["width"], t["height"], t["key"]))
    per = {}
    for m in index["masks"]:
        per[(m["outfit"], m["texture"])] = per.get((m["outfit"], m["texture"]), 0) + 1
    for (outfit, tex), n in sorted(per.items()):
        print("masks %-7s %-12s %d" % (outfit, tex, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Generate ports/amiga/micropython.info -- the AmigaOS Workbench icon
shipped alongside the binary.

The icon image comes from `logo/1bit-logo.png` (48x48, 1-bit) packed
into AmigaOS's planar bitmap layout (one bitplane, msb-first per
row, padded to a 16-bit row boundary).

Output is a WBTOOL .info file with a small ToolTypes block carrying
the documented defaults (`HEAP=`, `MAXHEAP=`) commented out so
end-users see them in the Information requester without having to
remember the names.

Standard library only -- no Pillow, so we hand-parse the PNG IHDR /
IDAT chunks and inflate via zlib.
"""

import os
import struct
import sys
import zlib


PNG_SIG = b"\x89PNG\r\n\x1a\n"


def parse_png_1bit(path):
    """Read a 1-bit grayscale or 1-bit palette PNG into a 2D list of
    0/1 ints, indexed [y][x]. Black pixels become 1, white become 0.

    Doesn't try to be a general PNG decoder -- the project logo is the
    only thing we feed it.
    """
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(PNG_SIG):
        raise ValueError("not a PNG")
    pos = len(PNG_SIG)
    width = height = bit_depth = color_type = None
    idat = bytearray()
    palette = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        chunk_type = data[pos + 4:pos + 8]
        chunk_data = data[pos + 8:pos + 8 + length]
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(
                ">IIBB", chunk_data[:10]
            )
        elif chunk_type == b"PLTE":
            # Palette: triples of RGB. We only care about index 0 vs 1.
            palette = [chunk_data[i:i + 3] for i in range(0, length, 3)]
        elif chunk_type == b"IDAT":
            idat.extend(chunk_data)
        elif chunk_type == b"IEND":
            break
        pos += 8 + length + 4  # length + type + data + CRC

    if width is None:
        raise ValueError("missing IHDR")
    if bit_depth != 1:
        raise ValueError("expected 1-bit PNG, got bit_depth=%d" % bit_depth)

    raw = zlib.decompress(bytes(idat))
    row_bytes = (width + 7) // 8
    expected = (row_bytes + 1) * height  # +1 for the per-row filter byte
    if len(raw) != expected:
        raise ValueError("IDAT size %d != expected %d" % (len(raw), expected))

    # Decide which palette index is "ink". For color_type=3 (palette),
    # pick the darker entry; for color_type=0 (grayscale), index 1 is
    # white in a typical 1-bit PNG so ink=0. We bias: black -> 1.
    if color_type == 3 and palette is not None and len(palette) >= 2:
        # Lower-sum RGB = darker.
        sum0 = sum(palette[0])
        sum1 = sum(palette[1])
        ink_index = 0 if sum0 < sum1 else 1
    else:
        # Grayscale: 1 in PNG = white, so 0 = ink.
        ink_index = 0

    rows = []
    for y in range(height):
        # Skip the filter byte (must be 0 == None for our 1-bit input).
        filter_byte = raw[y * (row_bytes + 1)]
        if filter_byte != 0:
            raise ValueError("unsupported PNG filter type %d on row %d"
                             % (filter_byte, y))
        row_data = raw[y * (row_bytes + 1) + 1:
                       y * (row_bytes + 1) + 1 + row_bytes]
        row = []
        for x in range(width):
            byte = row_data[x // 8]
            bit = (byte >> (7 - (x % 8))) & 1
            row.append(1 if bit == ink_index else 0)
        rows.append(row)
    return width, height, rows


def pack_planar(width, height, rows):
    """Pack the [y][x] bit array into AmigaOS planar bitmap layout.

    One bitplane, msb-first within each byte, rows padded to the next
    even byte (16-bit boundary). Returns raw bytes.
    """
    row_bytes = ((width + 15) // 16) * 2  # pad to word boundary
    out = bytearray()
    for y in range(height):
        row = rows[y]
        for byte_idx in range(row_bytes):
            byte = 0
            for bit in range(8):
                x = byte_idx * 8 + bit
                if x < width and row[x] == 1:
                    byte |= 1 << (7 - bit)
            out.append(byte)
    return bytes(out)


# ---------- AmigaOS .info file format ----------
#
# Big-endian throughout. Sentinel ULONGs of 0x00000001 mark pointers
# whose actual data follows in serialised order; 0 means "no such
# field". `icon.library` parses left-to-right and ignores numeric
# pointer values entirely.

WB_DISKMAGIC = 0xE310
WB_DISKVERSION = 1

WBDISK = 1
WBDRAWER = 2
WBTOOL = 3
WBPROJECT = 4

# Gadget flags
GFLG_GADGIMAGE = 0x0004
GFLG_GADGHCOMP = 0x0000  # default complement-on-select

# Gadget activation
GACT_RELVERIFY = 0x0001
GACT_IMMEDIATE = 0x0002

# Gadget type
GTYPE_BOOLGADGET = 0x0001

NO_ICON_POSITION = 0x80000000


def serialise_disk_object(width, height, plane_data, tooltypes,
                          do_type=WBTOOL, stack_size=16384,
                          default_tool=None):
    """Build the byte stream for a WBTOOL .info file.

    Structure:
      DiskObject header (78 bytes)
      do_Gadget.GadgetRender follow-on: struct Image header (20 bytes)
      bitmap planar data
      do_DefaultTool string (if non-null and non-empty)
      do_ToolTypes block (if any entries)
    """
    out = bytearray()

    # ---------- DiskObject header ----------
    out += struct.pack(">HH", WB_DISKMAGIC, WB_DISKVERSION)

    # do_Gadget (44 bytes)
    next_gadget = 0
    left_edge = 0
    top_edge  = 0
    flags = GFLG_GADGIMAGE
    activation = GACT_RELVERIFY
    gtype = GTYPE_BOOLGADGET
    # GadgetRender = non-null sentinel; SelectRender = 0 (no dual image).
    gadget_render = 0x00000001
    select_render = 0
    gadget_text   = 0
    mutual_excl   = 0
    special_info  = 0
    gadget_id     = 0
    user_data     = 0
    out += struct.pack(
        ">IhhhhHHHIIIiIHI",
        next_gadget, left_edge, top_edge, width, height,
        flags, activation, gtype,
        gadget_render, select_render, gadget_text,
        mutual_excl, special_info, gadget_id, user_data,
    )

    # do_Type, pad
    out += struct.pack(">BB", do_type, 0)

    # do_DefaultTool: pointer sentinel if non-empty, else 0
    out += struct.pack(">I", 0x00000001 if default_tool else 0)

    # do_ToolTypes: sentinel if any entries
    out += struct.pack(">I", 0x00000001 if tooltypes else 0)

    # Snap position. NO_ICON_POSITION is 0x80000000 which Python
    # treats as positive; pack as unsigned and let icon.library
    # interpret it as the signed sentinel.
    out += struct.pack(">II", NO_ICON_POSITION, NO_ICON_POSITION)

    # do_DrawerData (WBTOOL has none)
    out += struct.pack(">I", 0)

    # do_ToolWindow (only valid for WBTOOL but optional)
    out += struct.pack(">I", 0)

    # do_StackSize
    out += struct.pack(">i", stack_size)

    # ---------- Embedded struct Image (20 bytes) for GadgetRender ----------
    img_left   = 0
    img_top    = 0
    img_width  = width
    img_height = height
    img_depth  = 1
    img_data_ptr  = 0x00000001     # sentinel; actual data follows
    img_pick   = 0x01              # plane 0
    img_onoff  = 0x00
    img_next   = 0
    out += struct.pack(
        ">hhhhhIBBI",
        img_left, img_top, img_width, img_height, img_depth,
        img_data_ptr, img_pick, img_onoff, img_next,
    )

    # ---------- Planar bitmap data ----------
    out += plane_data

    # ---------- DefaultTool string ----------
    if default_tool:
        s = default_tool.encode("ascii") + b"\x00"
        out += struct.pack(">I", len(s)) + s

    # ---------- ToolTypes block ----------
    if tooltypes:
        # Header: 4 bytes = (n + 1) * 4 -- size of the pointer array
        # including a NULL terminator.
        out += struct.pack(">I", (len(tooltypes) + 1) * 4)
        for tt in tooltypes:
            s = tt.encode("ascii") + b"\x00"
            out += struct.pack(">I", len(s)) + s

    return bytes(out)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    png_path = os.path.join(repo_root, "logo", "1bit-logo.png")
    out_path = os.path.join(repo_root, "ports", "amiga", "micropython.info")

    width, height, rows = parse_png_1bit(png_path)
    print("loaded %s: %dx%d, %d ink pixels"
          % (png_path, width, height,
             sum(sum(r) for r in rows)))

    plane_data = pack_planar(width, height, rows)
    print("packed %d bytes of planar bitmap" % len(plane_data))

    tooltypes = [
        # The cached SCRIPT= tooltype lets a double-click on the icon
        # launch a Python script. Edit the value before use.
        "(SCRIPT=PROGDIR:hello.py)",
        # HEAP= / MAXHEAP= drive the GC's initial and upper-bound
        # sizes. Defaults baked into the binary are fine for most
        # scripts; uncomment and edit to tune.
        "(HEAP=131072)",
        "(MAXHEAP=8388608)",
        # CON= overrides the console window the binary opens on
        # Workbench launch. The value below is the binary's
        # compiled-in default written out as a tooltype -- enabling
        # this line as-is changes nothing functionally, but makes
        # the geometry / title / flags editable from the Workbench
        # Information requester. Replace AUTO/CLOSE with
        # AUTO/CLOSE/WAIT to keep the window open after exit() (the
        # standard convention for one-shot SCRIPT= scripts whose
        # output would otherwise flash by).
        "(CON=CON:0/30/640/200/MicroPython/AUTO/CLOSE)",
        # DONOTWAIT keeps the launcher from blocking on this icon.
        "DONOTWAIT",
    ]

    info_bytes = serialise_disk_object(
        width, height, plane_data,
        tooltypes,
        do_type=WBTOOL,
        stack_size=65536,
    )
    with open(out_path, "wb") as f:
        f.write(info_bytes)
    print("wrote %s (%d bytes)" % (out_path, len(info_bytes)))


if __name__ == "__main__":
    sys.exit(main() or 0)

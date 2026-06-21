"""GadTools GUI demo for the MicroPython Amiga port.

Opens a normal Intuition window on the Workbench (public) screen and
populates it with a gallery of GadTools gadgets -- a string field, an
integer field, a checkbox, a cycle gadget, a slider, a mutually-exclusive
radio group, a read-only number display and two push buttons.

The point of this script is to show *how the UI is wired up* using the
port's low-level library proxy (`amiga.Library`) and `amiga.taglist()`.
The gadgets are fully live -- you can click and drag them -- but none of
them are hooked up to real behaviour beyond printing the event to the
console.  Close the window (or press Esc) to quit.

Run it from the REPL or a script on a real Kickstart (intuition.library
v37+, gadtools.library v37+):

    >>> import gadtools_demo
    >>> gadtools_demo.main()

It needs a genuine Workbench screen, so it won't do anything useful under
vamos -- launch it under Amiberry / on real hardware.
"""

import amiga
from amiga import peek_b, peek_w, peek_l, poke_w, poke_l, poke_bytes

# --- GadTools gadget "kind" codes (the d0 arg to CreateGadgetA) ----------
# These are small ordinals from <gadtools.h>, not tags.
GENERIC_KIND = 0
BUTTON_KIND = 1
CHECKBOX_KIND = 2
INTEGER_KIND = 3
LISTVIEW_KIND = 4
MX_KIND = 5
NUMBER_KIND = 6
CYCLE_KIND = 7
PALETTE_KIND = 8
SCROLLER_KIND = 9
SLIDER_KIND = 11
STRING_KIND = 12
TEXT_KIND = 13

# --- NewGadget.ng_Flags label placement (<intuition/gadtools.h>) ---------
PLACETEXT_LEFT = 0x01
PLACETEXT_RIGHT = 0x02
PLACETEXT_ABOVE = 0x04
PLACETEXT_BELOW = 0x08
PLACETEXT_IN = 0x10

# --- IDCMP event classes we care about (<intuition/intuition.h>) ---------
IDCMP_NEWSIZE = 0x00000002
IDCMP_REFRESHWINDOW = 0x00000004
IDCMP_MOUSEMOVE = 0x00000010
IDCMP_GADGETDOWN = 0x00000020
IDCMP_GADGETUP = 0x00000040
IDCMP_CLOSEWINDOW = 0x00000200
IDCMP_VANILLAKEY = 0x00200000

# --- Fixed struct offsets (AmigaOS NDK, packed/32-bit) -------------------
# struct Window: only the few fields we read.
WD_RPORT = 50  # struct RastPort *RPort
WD_USERPORT = 86  # struct MsgPort  *UserPort
# struct MsgPort: signal bit used to Wait() on the port.
MP_SIGBIT = 15  # UBYTE mp_SigBit
# struct Gadget: the two fields we touch directly.
GG_ACTIVATION = 14  # UWORD Activation
GG_GADGETID = 38  # UWORD GadgetID

# Gadget Activation flag: with this set, Tab / Shift-Tab move the cursor
# between string gadgets automatically (Intuition handles the cycling).
GACT_TABCYCLE = 0x0200
KEY_TAB = 0x09  # VANILLAKEY code for Tab

# topaz.font ROM-font flag, for the TextAttr GadTools lays text out with.
FPF_ROMFONT = 0x40


class Arena:
    """Tracks every AllocVec / TagList we make so cleanup is one call.

    GadTools keeps *pointers* into some of the things we pass it (the
    cycle/MX label arrays, the slider format string, the initial string
    contents), and dereferences them again every time it redraws the
    gadget.  So all of this memory has to stay alive for as long as the
    window is open -- we can only free it after CloseWindow().
    """

    def __init__(self):
        self._mem = []
        self._taglists = []

    def alloc(self, size):
        addr = amiga.alloc_vec(size, amiga.MEMF_ANY | amiga.MEMF_CLEAR)
        if not addr:
            raise MemoryError("AllocVec(%d) failed" % size)
        self._mem.append(addr)
        return addr

    def cstring(self, text):
        """Copy `text` into a NUL-terminated buffer and return its address."""
        raw = text.encode("ascii") + b"\x00"
        buf = self.alloc(len(raw))
        poke_bytes(buf, raw)
        return buf

    def string_array(self, items):
        """Build a NULL-terminated array of char* (GadTools label lists)."""
        arr = self.alloc((len(items) + 1) * 4)
        for i, s in enumerate(items):
            poke_l(arr + i * 4, self.cstring(s))
        poke_l(arr + len(items) * 4, 0)  # terminator
        return arr

    def keep(self, taglist):
        """Retain a TagList (and the buffers it owns) until teardown."""
        self._taglists.append(taglist)
        return taglist

    def free_all(self):
        for tl in self._taglists:
            tl.close()
        self._taglists = []
        for addr in self._mem:
            amiga.free_vec(addr)
        self._mem = []


def _text_attr(arena, name="topaz.font", ysize=8, style=0, flags=FPF_ROMFONT):
    """Build a struct TextAttr (8 bytes) for GadTools text layout."""
    ta = arena.alloc(8)
    poke_l(ta + 0, arena.cstring(name))  # ta_Name
    poke_w(ta + 4, ysize)  # ta_YSize
    poke_b = amiga.poke_b
    poke_b(ta + 6, style)  # ta_Style
    poke_b(ta + 7, flags)  # ta_Flags
    return ta


def _new_gadget(arena, left, top, width, height, label, place, gid, textattr, vi):
    """Build a struct NewGadget (30 bytes) ready for CreateGadgetA."""
    ng = arena.alloc(32)
    poke_w(ng + 0, left)  # ng_LeftEdge
    poke_w(ng + 2, top)  # ng_TopEdge
    poke_w(ng + 4, width)  # ng_Width
    poke_w(ng + 6, height)  # ng_Height
    if label is not None:
        poke_l(ng + 8, arena.cstring(label))  # ng_GadgetText
    poke_l(ng + 12, textattr)  # ng_TextAttr
    poke_w(ng + 16, gid)  # ng_GadgetID
    poke_l(ng + 18, place)  # ng_Flags
    poke_l(ng + 22, vi)  # ng_VisualInfo
    return ng


def _build_gadgets(arena, gt, textattr, vi):
    """Create the whole gadget gallery; return (glist, info).

    Gadgets are chained: CreateContext() seeds the list, and each
    CreateGadgetA() takes the previous gadget so they link together.  We
    pass the head of that list to OpenWindow via WA_Gadgets.

    `info` is the bookkeeping the event loop needs, keyed by gadget id:
    `names` (label), `ptrs` (struct Gadget*), `kinds` (the *_KIND code),
    `nopts` (number of choices, for cycle/MX), `state` (current value of
    the stateful gadgets) and `shortcuts` (typed-letter -> gadget id,
    derived from the '_' in each label).
    """
    # CreateContext wants a `struct Gadget **` -- a 4-byte slot it writes
    # the list head into.  The returned context gadget is also that head.
    glist_slot = arena.alloc(4)
    context = gt.CreateContext(glist_slot)
    if not context:
        raise RuntimeError("CreateContext failed")

    gx = 150  # gadgets start here (labels sit to the LEFT of this)
    gw = 240  # input gadget width
    h = 14  # row height
    info = {"names": {}, "ptrs": {}, "kinds": {}, "nopts": {}, "state": {}, "shortcuts": {}}

    def add(
        gad,
        kind,
        top,
        width,
        label,
        place,
        gid,
        name,
        left=gx,
        height=h,
        nopts=0,
        state=None,
        **tags,
    ):
        if not gad:  # a previous CreateGadgetA already failed
            return gad
        ng = _new_gadget(arena, left, top, width, height, label, place, gid, textattr, vi)
        # GT_Underscore tells GadTools that '_' in a label marks the
        # following character as the keyboard shortcut (and underlines
        # it).  Without it the underscore is just drawn literally.
        tl = arena.keep(amiga.taglist(GT_VisualInfo=vi, GT_Underscore=ord("_"), **tags))
        g = gt.CreateGadgetA(kind, gad, ng, tl)
        info["names"][gid] = name
        info["ptrs"][gid] = g
        info["kinds"][gid] = kind
        if nopts:
            info["nopts"][gid] = nopts
        if state is not None:
            info["state"][gid] = state
        # The character after '_' in the label is the keyboard shortcut.
        u = label.find("_") if label else -1
        if 0 <= u < len(label) - 1:
            info["shortcuts"][ord(label[u + 1].lower())] = gid
        return g

    gad = context
    gad = add(
        gad,
        STRING_KIND,
        20,
        gw,
        "_Name:",
        PLACETEXT_LEFT,
        1,
        "string",
        GTST_String="MicroPython",
        GTST_MaxChars=64,
    )
    # "Coun_t" (shortcut 't'), not "_Count" -- 'c' belongs to Cancel below.
    gad = add(
        gad,
        INTEGER_KIND,
        40,
        80,
        "Coun_t:",
        PLACETEXT_LEFT,
        2,
        "integer",
        GTIN_Number=42,
        GTIN_MaxChars=8,
    )
    gad = add(
        gad,
        CHECKBOX_KIND,
        60,
        26,
        "_Enabled:",
        PLACETEXT_LEFT,
        3,
        "checkbox",
        state=1,
        GTCB_Checked=1,
    )
    gad = add(
        gad,
        CYCLE_KIND,
        80,
        gw,
        "_Mode:",
        PLACETEXT_LEFT,
        4,
        "cycle",
        nopts=3,
        state=1,
        GTCY_Labels=arena.string_array(["Draft", "Normal", "Final"]),
        GTCY_Active=1,
    )
    gad = add(
        gad,
        SLIDER_KIND,
        100,
        gw,
        "_Volume:",
        PLACETEXT_LEFT,
        5,
        "slider",
        state=70,
        GTSL_Min=0,
        GTSL_Max=100,
        GTSL_Level=70,
        GTSL_LevelFormat="%3ld",
        GTSL_MaxLevelLen=3,
        GTSL_LevelPlace=PLACETEXT_RIGHT,
    )
    # MX (radio): GTMX_Labels is the NULL-terminated choice list, `height`
    # is each choice's row height and GTMX_Spacing the gap between rows.
    # The radio images are ~14px tall, so the pitch (height + spacing) has
    # to clear that or the three buttons overlap into one unclickable blob.
    gad = add(
        gad,
        MX_KIND,
        122,
        18,
        "_Align:",
        PLACETEXT_LEFT,
        6,
        "radio",
        height=16,
        nopts=3,
        state=0,
        GTMX_Labels=arena.string_array(["Left", "Centre", "Right"]),
        GTMX_Spacing=2,
        GTMX_Active=0,
    )
    gad = add(
        gad,
        NUMBER_KIND,
        186,
        gw,
        "Pixels:",
        PLACETEXT_LEFT,
        7,
        "number",
        GTNM_Number=1024,
        GTNM_Border=1,
    )

    # Two push buttons along the bottom; label drawn IN the button.
    gad = add(gad, BUTTON_KIND, 210, 90, "_OK", PLACETEXT_IN, 101, "ok")
    gad = add(gad, BUTTON_KIND, 210, 90, "_Cancel", PLACETEXT_IN, 102, "cancel", left=gx + 150)

    if not gad:
        raise RuntimeError("a CreateGadgetA call failed")

    glist = peek_l(glist_slot)  # == context; this is what WA_Gadgets wants
    return glist, info


def main():
    arena = Arena()
    intuition = amiga.library("intuition.library", 37)
    gadtools = amiga.library("gadtools.library", 37)

    screen = 0
    vi = 0
    window = 0
    glist = 0
    try:
        # GadTools renders relative to a screen's visual context, so lock
        # the default public (Workbench) screen and ask for its VisualInfo.
        screen = intuition.LockPubScreen(0)
        if not screen:
            raise RuntimeError("LockPubScreen failed (no Workbench screen?)")
        vi = gadtools.GetVisualInfoA(screen, 0)
        if not vi:
            raise RuntimeError("GetVisualInfoA failed")

        textattr = _text_attr(arena)
        glist, info = _build_gadgets(arena, gadtools, textattr, vi)

        # Turn on Tab / Shift-Tab cycling between the two text-entry
        # fields.  GadTools doesn't expose this, so we OR the flag into
        # each gadget's Activation field by hand before the window opens.
        for gid in (1, 2):  # Name (string), Count (integer)
            g = info["ptrs"][gid]
            poke_w(g + GG_ACTIVATION, peek_w(g + GG_ACTIVATION) | GACT_TABCYCLE)

        wtags = arena.keep(
            amiga.taglist(
                WA_Left=40,
                WA_Top=20,
                WA_Width=420,
                WA_Height=246,
                WA_Title="MicroPython GadTools Demo",
                WA_PubScreen=screen,
                WA_Gadgets=glist,
                WA_DragBar=1,
                WA_DepthGadget=1,
                WA_CloseGadget=1,
                WA_Activate=1,
                WA_NewLookMenus=1,
                # MX reports via GADGETDOWN, the others via GADGETUP; listen
                # for both so every gadget's events reach us.
                WA_IDCMP=(
                    IDCMP_CLOSEWINDOW
                    | IDCMP_REFRESHWINDOW
                    | IDCMP_GADGETUP
                    | IDCMP_GADGETDOWN
                    | IDCMP_VANILLAKEY
                ),
            )
        )
        window = intuition.OpenWindowTagList(0, wtags)
        if not window:
            raise RuntimeError("OpenWindowTagList failed")

        # GadTools wants this once after the window is up so the gadgets'
        # imagery is drawn correctly (3D bevels, the active cycle text...).
        gadtools.GT_RefreshWindow(window, 0)

        _event_loop(intuition, gadtools, window, info)

    finally:
        # Teardown order matters: drop the window before the gadget list
        # it points at, then the VisualInfo, then unlock the screen, and
        # only then free the memory GadTools was still dereferencing.
        if window:
            intuition.CloseWindow(window)
        if glist:
            gadtools.FreeGadgets(glist)
        if vi:
            gadtools.FreeVisualInfo(vi)
        if screen:
            intuition.UnlockPubScreen(0, screen)
        arena.free_all()
        gadtools.close()
        intuition.close()


def _gt_set(gt, gad, window, **attrs):
    """GT_SetGadgetAttrs with a throwaway tag list, redrawing the gadget.

    Passing the window makes GadTools refresh the gadget's imagery on the
    spot, so a keyboard-driven change is reflected immediately.  The tag
    list only carries scalars (GadTools copies them), so it's safe to free
    as soon as the call returns.
    """
    with amiga.taglist(**attrs) as tl:
        gt.GT_SetGadgetAttrsA(gad, window, 0, tl)


def _activate_shortcut(intuition, gt, window, gid, info):
    """Do whatever pressing a gadget's keyboard shortcut should do."""
    kind = info["kinds"][gid]
    gad = info["ptrs"][gid]
    name = info["names"][gid]
    state = info["state"]
    if kind == STRING_KIND or kind == INTEGER_KIND:
        # Drop the cursor into the field -- as if the user clicked it.
        intuition.ActivateGadget(gad, window, 0)
        print("focus -> %s field" % name)
    elif kind == CHECKBOX_KIND:
        state[gid] ^= 1
        _gt_set(gt, gad, window, GTCB_Checked=state[gid])
        print("checkbox %s -> %d" % (name, state[gid]))
    elif kind == CYCLE_KIND:
        state[gid] = (state[gid] + 1) % info["nopts"][gid]
        _gt_set(gt, gad, window, GTCY_Active=state[gid])
        print("cycle %s -> %d" % (name, state[gid]))
    elif kind == MX_KIND:
        state[gid] = (state[gid] + 1) % info["nopts"][gid]
        _gt_set(gt, gad, window, GTMX_Active=state[gid])
        print("radio %s -> %d" % (name, state[gid]))
    elif kind == SLIDER_KIND:
        state[gid] = 0 if state[gid] >= 100 else min(100, state[gid] + 10)
        _gt_set(gt, gad, window, GTSL_Level=state[gid])
        print("slider %s -> %d" % (name, state[gid]))
    else:  # BUTTON_KIND
        print("button %s activated" % name)


def _event_loop(intuition, gadtools, window, info):
    """Wait on the window's UserPort and drain GadTools IntuiMessages.

    We never open exec.library: the window's UserPort carries a signal
    bit, and `amiga.wait_signal` blocks on it (and folds in Ctrl-C for a
    clean break).  GT_GetIMsg both dequeues the message and pre-processes
    it for GadTools; GT_ReplyIMsg hands it back.
    """
    userport = peek_l(window + WD_USERPORT)
    sigmask = 1 << peek_b(userport + MP_SIGBIT)
    names = info["names"]
    shortcuts = info["shortcuts"]
    state = info["state"]

    print("GadTools demo running -- close the window or press Esc to quit.")
    print("Shortcuts: underlined letters; Tab / Shift-Tab cycle the fields.")
    running = True
    while running:
        try:
            amiga.wait_signal(sigmask)
        except KeyboardInterrupt:
            break
        while True:
            msg = gadtools.GT_GetIMsg(userport)
            if not msg:
                break
            m = amiga.IntuiMessage(msg)
            cls = m.Class
            code = m.Code
            iaddr = m.IAddress
            gadtools.GT_ReplyIMsg(msg)

            if cls == IDCMP_CLOSEWINDOW:
                running = False
            elif cls == IDCMP_REFRESHWINDOW:
                gadtools.GT_BeginRefresh(window)
                gadtools.GT_EndRefresh(window, 1)
            elif cls == IDCMP_VANILLAKEY:
                # Keys only reach us as VANILLAKEY when no string gadget is
                # active -- while editing a field they go into the gadget,
                # and Tab is consumed by Intuition's TABCYCLE handling.
                if code in (27, ord("q"), ord("Q")):  # Esc / q
                    running = False
                elif code == KEY_TAB:
                    # Tab with nothing focused: jump into the first field.
                    intuition.ActivateGadget(info["ptrs"][1], window, 0)
                else:
                    key = code + 32 if 65 <= code <= 90 else code  # fold to lower
                    gid = shortcuts.get(key)
                    if gid is not None:
                        _activate_shortcut(intuition, gadtools, window, gid, info)
            elif cls == IDCMP_GADGETUP or cls == IDCMP_GADGETDOWN:
                gid = peek_w(iaddr + GG_GADGETID)
                # `code` carries the gadget's new value for cycle/slider/
                # checkbox/mx; keep our shadow state in step with the mouse.
                if gid in state:
                    state[gid] = code
                print("gadget %r (id %d) -> code %d" % (names.get(gid, "?"), gid, code))


if __name__ == "__main__":
    main()

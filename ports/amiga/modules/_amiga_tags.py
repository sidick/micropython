"""Hand-curated AmigaOS tag-ID table for `amiga.taglist`.

Tag IDs in the NDK are typically `<NAME>_Dummy + offset` where
`<NAME>_Dummy = TAG_USER + N`, so they can't be regex-extracted from
the headers without a real C preprocessor pass.  This file ships the
most commonly used tags as resolved 32-bit constants; users can pass
any unknown tag ID as a literal integer via positional pairs.

The mapping covers the universal `TAG_*` markers, plus `WA_*`
(OpenWindowTagList), `SA_*` (OpenScreenTagList), and the `GA_*` /
`GT_*` bases.  Extend as needs grow — or write a small tools/
preprocessor pass to harvest more from the NDK.
"""

# Universal tag-protocol markers (utility/tagitem.h).
TAG_USER = 0x80000000
TAG_DONE = 0
TAG_IGNORE = 1
TAG_MORE = 2
TAG_SKIP = 3

# Per-namespace bases (intuition.h, screens.h).
_WA = TAG_USER + 99   # WA_Dummy   = 0x80000063
_SA = TAG_USER + 32   # SA_Dummy   = 0x80000020
_GA = TAG_USER + 0x30000  # GA_Dummy = 0x80030000 (gadgetclass.h)
_GT = TAG_USER + 0x20000  # GT_TagBase = 0x80020000 (gadtools.h)

TAGS = {
    # Universal
    "TAG_DONE":          TAG_DONE,
    "TAG_IGNORE":        TAG_IGNORE,
    "TAG_MORE":          TAG_MORE,
    "TAG_SKIP":          TAG_SKIP,
    "TAG_USER":          TAG_USER,

    # Window attributes (OpenWindowTagList).
    "WA_Left":           _WA + 0x01,
    "WA_Top":            _WA + 0x02,
    "WA_Width":          _WA + 0x03,
    "WA_Height":         _WA + 0x04,
    "WA_DetailPen":      _WA + 0x05,
    "WA_BlockPen":       _WA + 0x06,
    "WA_IDCMP":          _WA + 0x07,
    "WA_Flags":          _WA + 0x08,
    "WA_Gadgets":        _WA + 0x09,
    "WA_Checkmark":      _WA + 0x0A,
    "WA_Title":          _WA + 0x0B,
    "WA_ScreenTitle":    _WA + 0x0C,
    "WA_CustomScreen":   _WA + 0x0D,
    "WA_SuperBitMap":    _WA + 0x0E,
    "WA_MinWidth":       _WA + 0x0F,
    "WA_MinHeight":      _WA + 0x10,
    "WA_MaxWidth":       _WA + 0x11,
    "WA_MaxHeight":      _WA + 0x12,
    "WA_InnerWidth":     _WA + 0x13,
    "WA_InnerHeight":    _WA + 0x14,
    "WA_PubScreenName":  _WA + 0x15,
    "WA_PubScreen":      _WA + 0x16,
    "WA_PubScreenFallBack": _WA + 0x17,
    "WA_WindowName":     _WA + 0x18,
    "WA_Colors":         _WA + 0x19,
    "WA_Zoom":           _WA + 0x1A,
    "WA_MouseQueue":     _WA + 0x1B,
    "WA_BackFill":       _WA + 0x1C,
    "WA_RptQueue":       _WA + 0x1D,
    "WA_SizeGadget":     _WA + 0x1E,
    "WA_DragBar":        _WA + 0x1F,
    "WA_DepthGadget":    _WA + 0x20,
    "WA_CloseGadget":    _WA + 0x21,
    "WA_Backdrop":       _WA + 0x22,
    "WA_ReportMouse":    _WA + 0x23,
    "WA_NoCareRefresh":  _WA + 0x24,
    "WA_Borderless":     _WA + 0x25,
    "WA_Activate":       _WA + 0x26,
    "WA_RMBTrap":        _WA + 0x27,
    "WA_SimpleRefresh":  _WA + 0x29,
    "WA_SmartRefresh":   _WA + 0x2A,
    "WA_SizeBRight":     _WA + 0x2B,
    "WA_SizeBBottom":    _WA + 0x2C,
    "WA_AutoAdjust":     _WA + 0x2D,
    "WA_GimmeZeroZero":  _WA + 0x2E,
    "WA_MenuHelp":       _WA + 0x2F,
    "WA_NewLookMenus":   _WA + 0x30,
    "WA_AmigaKey":       _WA + 0x31,
    "WA_NotifyDepth":    _WA + 0x32,
    "WA_Pointer":        _WA + 0x34,
    "WA_BusyPointer":    _WA + 0x35,
    "WA_PointerDelay":   _WA + 0x36,
    "WA_HelpGroup":      _WA + 0x38,
    "WA_Hidden":         _WA + 0x3C,

    # Screen attributes (OpenScreenTagList).
    "SA_Left":           _SA + 0x01,
    "SA_Top":            _SA + 0x02,
    "SA_Width":          _SA + 0x03,
    "SA_Height":         _SA + 0x04,
    "SA_Depth":          _SA + 0x05,
    "SA_DetailPen":      _SA + 0x06,
    "SA_BlockPen":       _SA + 0x07,
    "SA_Title":          _SA + 0x08,
    "SA_Colors":         _SA + 0x09,
    "SA_ErrorCode":      _SA + 0x0A,
    "SA_Font":           _SA + 0x0B,
    "SA_SysFont":        _SA + 0x0C,
    "SA_Type":           _SA + 0x0D,
    "SA_BitMap":         _SA + 0x0E,
    "SA_PubName":        _SA + 0x0F,
    "SA_PubSig":         _SA + 0x10,
    "SA_PubTask":        _SA + 0x11,
    "SA_DisplayID":      _SA + 0x12,
    "SA_DClip":          _SA + 0x13,
    "SA_Overscan":       _SA + 0x14,
    "SA_ShowTitle":      _SA + 0x16,
    "SA_Behind":         _SA + 0x17,
    "SA_Quiet":          _SA + 0x18,
    "SA_AutoScroll":     _SA + 0x19,
    "SA_Pens":           _SA + 0x1A,
    "SA_FullPalette":    _SA + 0x1B,
    "SA_ColorMapEntries": _SA + 0x1C,
    "SA_Parent":         _SA + 0x1D,
    "SA_Draggable":      _SA + 0x1E,
    "SA_Exclusive":      _SA + 0x1F,
}

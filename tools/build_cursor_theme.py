#!/usr/bin/env python3
"""
Aura-Circle Cursor Theme Generator
Creates an Apple iPadOS/VisionOS circular dot cursor theme:
- Outer circular zone with 50% opacity
- Solid black center dot
- Hotspot at the center of the black dot
- Multi-resolution support: 24px, 32px, 48px, 64px
"""

import os
import math
import struct
import shutil
import cairo

SIZES = [16, 20, 21, 22, 24, 28, 32, 36, 40, 48, 64]

def pack_xcursor(images):
    """
    Pack a list of image dicts into XCursor binary format:
    [{'size': int, 'width': int, 'height': int, 'xhot': int, 'yhot': int, 'delay': int, 'data': bytes}, ...]
    """
    ntoc = len(images)
    header_size = 16
    toc_size = ntoc * 12
    pos = header_size + toc_size
    toc_bytes = bytearray()
    img_bytes = bytearray()
    for img in images:
        toc_bytes.extend(struct.pack('<III', 0xfffd0002, img['size'], pos))
        ichunk = struct.pack('<IIIIIIIII', 36, 0xfffd0002, img['size'], 1,
                             img['width'], img['height'], img['xhot'], img['yhot'], img['delay'])
        img_bytes.extend(ichunk)
        img_bytes.extend(img['data'])
        pos += len(ichunk) + len(img['data'])
    header = struct.pack('<IIII', 0x72756358, 16, 0x00010000, ntoc)
    return header + toc_bytes + img_bytes

def draw_puck(cr, cx, cy, r_outer, r_inner, puck_alpha=0.50):
    # 1. Outer circular zone: 50% opacity grey (#CACACA = 0.792)
    cr.arc(cx, cy, r_outer, 0, 2 * math.pi)
    cr.set_source_rgba(0.792, 0.792, 0.792, puck_alpha)
    cr.fill()

    # 2. Subtle crisp perimeter border (visible over pure white or dark backgrounds)
    cr.arc(cx, cy, r_outer, 0, 2 * math.pi)
    cr.set_source_rgba(1.0, 1.0, 1.0, 0.35)
    cr.set_line_width(max(0.65, r_outer * 0.075))
    cr.stroke()

    # 3. Inner solid black dot
    cr.arc(cx, cy, r_inner, 0, 2 * math.pi)
    cr.set_source_rgba(0.0, 0.0, 0.0, 1.0)
    cr.fill()

def draw_default(cr, size):
    cx, cy = size / 2.0, size / 2.0
    r_outer = size * 0.35
    r_inner = size * 0.136
    draw_puck(cr, cx, cy, r_outer, r_inner, puck_alpha=0.50)

def draw_pointer(cr, size):
    cx, cy = size / 2.0, size / 2.0
    r_outer = size * 0.40
    r_inner = size * 0.145
    draw_puck(cr, cx, cy, r_outer, r_inner, puck_alpha=0.58)
    # Subtle accent rim
    cr.arc(cx, cy, r_outer, 0, 2 * math.pi)
    cr.set_source_rgba(0.04, 0.52, 1.0, 0.50)
    cr.set_line_width(max(0.8, r_outer * 0.08))
    cr.stroke()

def draw_grab(cr, size):
    cx, cy = size / 2.0, size / 2.0
    r_outer = size * 0.38
    r_inner = size * 0.136
    draw_puck(cr, cx, cy, r_outer, r_inner, puck_alpha=0.45)

def draw_grabbing(cr, size):
    cx, cy = size / 2.0, size / 2.0
    r_outer = size * 0.28
    r_inner = size * 0.14
    draw_puck(cr, cx, cy, r_outer, r_inner, puck_alpha=0.65)

def draw_text(cr, size):
    cx, cy = size / 2.0, size / 2.0
    h = size * 0.70
    w = size * 0.26
    r_outer = size * 0.24
    r_inner = size * 0.10
    draw_puck(cr, cx, cy, r_outer, r_inner, puck_alpha=0.45)

    cr.set_source_rgba(0.05, 0.05, 0.05, 0.95)
    cr.set_line_cap(cairo.LINE_CAP_ROUND)
    cr.set_line_width(max(1.3, size * 0.055))
    # Vertical beam
    cr.move_to(cx, cy - h/2.0)
    cr.line_to(cx, cy + h/2.0)
    cr.stroke()
    # Top and bottom serifs
    cr.move_to(cx - w/2.0, cy - h/2.0)
    cr.line_to(cx + w/2.0, cy - h/2.0)
    cr.stroke()
    cr.move_to(cx - w/2.0, cy + h/2.0)
    cr.line_to(cx + w/2.0, cy + h/2.0)
    cr.stroke()

def draw_crosshair(cr, size):
    cx, cy = size / 2.0, size / 2.0
    r_outer = size * 0.28
    r_inner = size * 0.11
    draw_puck(cr, cx, cy, r_outer, r_inner, puck_alpha=0.48)

    cr.set_source_rgba(0.0, 0.0, 0.0, 0.85)
    cr.set_line_cap(cairo.LINE_CAP_ROUND)
    cr.set_line_width(max(1.0, size * 0.045))
    reach = size * 0.44
    gap = r_outer + max(1.0, size * 0.03)

    # 4 tick lines
    cr.move_to(cx, cy - gap); cr.line_to(cx, cy - reach); cr.stroke()
    cr.move_to(cx, cy + gap); cr.line_to(cx, cy + reach); cr.stroke()
    cr.move_to(cx - gap, cy); cr.line_to(cx - reach, cy); cr.stroke()
    cr.move_to(cx + gap, cy); cr.line_to(cx + reach, cy); cr.stroke()

def draw_arrow_head(cr, tip_x, tip_y, angle, length, width):
    cr.save()
    cr.translate(tip_x, tip_y)
    cr.rotate(angle)
    cr.move_to(0, 0)
    cr.line_to(-length, -width/2.0)
    cr.line_to(-length * 0.7, 0)
    cr.line_to(-length, width/2.0)
    cr.close_path()
    cr.set_source_rgba(0.1, 0.1, 0.1, 0.95)
    cr.fill()
    cr.restore()

def draw_resize(cr, size, angle):
    cx, cy = size / 2.0, size / 2.0
    r_outer = size * 0.28
    r_inner = size * 0.11
    draw_puck(cr, cx, cy, r_outer, r_inner, puck_alpha=0.48)

    arrow_len = size * 0.20
    arrow_w = size * 0.16
    reach = size * 0.46

    # 2 opposing arrows
    tip1_x = cx + reach * math.cos(angle)
    tip1_y = cy + reach * math.sin(angle)
    draw_arrow_head(cr, tip1_x, tip1_y, angle, arrow_len, arrow_w)

    tip2_x = cx + reach * math.cos(angle + math.pi)
    tip2_y = cy + reach * math.sin(angle + math.pi)
    draw_arrow_head(cr, tip2_x, tip2_y, angle + math.pi, arrow_len, arrow_w)

def draw_all_scroll(cr, size):
    cx, cy = size / 2.0, size / 2.0
    r_outer = size * 0.26
    r_inner = size * 0.10
    draw_puck(cr, cx, cy, r_outer, r_inner, puck_alpha=0.48)

    arrow_len = size * 0.18
    arrow_w = size * 0.14
    reach = size * 0.45
    for a in [0, math.pi/2, math.pi, 3*math.pi/2]:
        tx = cx + reach * math.cos(a)
        ty = cy + reach * math.sin(a)
        draw_arrow_head(cr, tx, ty, a, arrow_len, arrow_w)

def draw_not_allowed(cr, size):
    cx, cy = size / 2.0, size / 2.0
    r_outer = size * 0.35
    r_inner = size * 0.136
    draw_puck(cr, cx, cy, r_outer, r_inner, puck_alpha=0.50)

    # Red prohibition rim and slash
    cr.arc(cx, cy, r_outer, 0, 2 * math.pi)
    cr.set_source_rgba(0.90, 0.20, 0.20, 0.85)
    cr.set_line_width(max(1.2, size * 0.065))
    cr.stroke()

    slash = r_outer * 0.70
    cr.move_to(cx - slash, cy - slash)
    cr.line_to(cx + slash, cy + slash)
    cr.stroke()

def draw_badge(cr, size, kind):
    cx, cy = size / 2.0, size / 2.0
    r_outer = size * 0.32
    r_inner = size * 0.125
    draw_puck(cr, cx, cy, r_outer, r_inner, puck_alpha=0.50)

    # Badge circle at top right
    bx = cx + r_outer * 0.75
    by = cy - r_outer * 0.75
    br = size * 0.14
    cr.arc(bx, by, br, 0, 2 * math.pi)
    cr.set_source_rgba(1.0, 1.0, 1.0, 0.95)
    cr.fill()
    cr.arc(bx, by, br, 0, 2 * math.pi)
    cr.set_source_rgba(0.2, 0.2, 0.2, 0.6)
    cr.set_line_width(max(0.6, size * 0.025))
    cr.stroke()

    if kind == 'copy':
        # Plus icon
        cr.set_source_rgba(0.1, 0.6, 0.1, 0.95)
        cr.set_line_width(max(1.2, size * 0.05))
        cr.move_to(bx - br*0.5, by); cr.line_to(bx + br*0.5, by); cr.stroke()
        cr.move_to(bx, by - br*0.5); cr.line_to(bx, by + br*0.5); cr.stroke()
    elif kind == 'help':
        # Question mark
        cr.set_source_rgba(0.1, 0.1, 0.8, 0.95)
        cr.select_font_face("Sans", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
        cr.set_font_size(br * 1.5)
        cr.move_to(bx - br*0.35, by + br*0.45)
        cr.show_text("?")
    elif kind == 'alias':
        # Arrow
        draw_arrow_head(cr, bx + br*0.4, by - br*0.4, -math.pi/4, br*0.9, br*0.7)

def draw_progress(cr, size):
    cx, cy = size / 2.0, size / 2.0
    r_outer = size * 0.32
    r_inner = size * 0.125
    draw_puck(cr, cx, cy, r_outer, r_inner, puck_alpha=0.50)

    # Spinner arc
    cr.arc(cx, cy, r_outer + max(1.5, size * 0.06), 0, 1.5 * math.pi)
    cr.set_source_rgba(0.04, 0.52, 1.0, 0.85)
    cr.set_line_width(max(1.0, size * 0.05))
    cr.set_line_cap(cairo.LINE_CAP_ROUND)
    cr.stroke()

CURSOR_GENERATORS = {
    'default': draw_default,
    'pointer': draw_pointer,
    'openhand': draw_grab,
    'closedhand': draw_grabbing,
    'text': draw_text,
    'crosshair': draw_crosshair,
    'all-scroll': draw_all_scroll,
    'col-resize': lambda cr, s: draw_resize(cr, s, 0),
    'row-resize': lambda cr, s: draw_resize(cr, s, math.pi/2),
    'nesw-resize': lambda cr, s: draw_resize(cr, s, -math.pi/4),
    'nwse-resize': lambda cr, s: draw_resize(cr, s, math.pi/4),
    'not-allowed': draw_not_allowed,
    'copy': lambda cr, s: draw_badge(cr, s, 'copy'),
    'help': lambda cr, s: draw_badge(cr, s, 'help'),
    'alias': lambda cr, s: draw_badge(cr, s, 'alias'),
    'progress': draw_progress,
    'wait': draw_progress,
}

SYMLINKS = {
    # Default pointer aliases
    'arrow': 'default',
    'left_ptr': 'default',
    'top_left_arrow': 'default',
    'center_ptr': 'default',

    # Pointer / clicking aliases
    'pointing_hand': 'pointer',
    'hand1': 'pointer',
    'hand2': 'pointer',
    '9d800788f1b08800ae810202380a0822': 'pointer',
    'e29285e634086352946a0e7090d73106': 'pointer',

    # Hand / Grab aliases
    'grab': 'openhand',
    'grabbing': 'closedhand',
    'dnd-move': 'closedhand',
    'move': 'all-scroll',
    'fleur': 'all-scroll',
    'size_all': 'all-scroll',
    '4498f0e0c1937ffe01fd06f973665830': 'closedhand',
    '9081237383d90e509aa00f00170e968f': 'closedhand',
    'fcf21c00b30f7e3f83fe0dfd12e71cff': 'closedhand',

    # Text / I-Beam aliases
    'xterm': 'text',
    'ibeam': 'text',

    # Crosshair aliases
    'cross': 'crosshair',
    'cell': 'crosshair',
    'plus': 'crosshair',

    # Resize aliases
    'ew-resize': 'col-resize',
    'w-resize': 'col-resize',
    'e-resize': 'col-resize',
    'size_hor': 'col-resize',
    'h_double_arrow': 'col-resize',
    'sb_h_double_arrow': 'col-resize',
    'split_h': 'col-resize',
    'left_side': 'col-resize',
    'right_side': 'col-resize',

    'ns-resize': 'row-resize',
    'n-resize': 'row-resize',
    's-resize': 'row-resize',
    'size_ver': 'row-resize',
    'v_double_arrow': 'row-resize',
    'sb_v_double_arrow': 'row-resize',
    'split_v': 'row-resize',
    '00008160000006810000408080010102': 'row-resize',
    'top_side': 'row-resize',
    'bottom_side': 'row-resize',

    'size_bdiag': 'nesw-resize',
    'top_right_corner': 'nesw-resize',
    'bottom_left_corner': 'nesw-resize',
    'ne-resize': 'nesw-resize',
    'sw-resize': 'nesw-resize',
    'ur_angle': 'nesw-resize',
    'll_angle': 'nesw-resize',
    'bd_double_arrow': 'nesw-resize',

    'size_fdiag': 'nwse-resize',
    'top_left_corner': 'nwse-resize',
    'bottom_right_corner': 'nwse-resize',
    'nw-resize': 'nwse-resize',
    'se-resize': 'nwse-resize',
    'ul_angle': 'nwse-resize',
    'lr_angle': 'nwse-resize',
    'fd_double_arrow': 'nwse-resize',
    'all-resize': 'all-scroll',

    # Forbidden / No-drop aliases
    'circle': 'not-allowed',
    'crossed_circle': 'not-allowed',
    'forbidden': 'not-allowed',
    'no-drop': 'not-allowed',
    'dnd-no-drop': 'not-allowed',
    'dnd-none': 'not-allowed',
    'pirate': 'not-allowed',
    '03b6e0fcb3499374a867c041f52298f0': 'not-allowed',

    # Badges & Actions
    'dnd-copy': 'copy',
    'dnd-ask': 'copy',
    '1081e37283d90000800003c07f3ef6bf': 'copy',
    '6407b0e94181790501fd1e167b474872': 'copy',
    'b66166c04f8c3109214a4fbd64a50fc8': 'copy',

    'whats_this': 'help',
    'left_ptr_help': 'help',
    'question_arrow': 'help',
    '5c6cd98b3f3ebcb1f9c7f1c204630408': 'help',
    'd9ce0ab605698f320427677b458ad60b': 'help',

    'link': 'alias',
    '3085a0e285430894940527032f8b26df': 'alias',
    '640fb0e74195791501fd1ed57b41487f': 'alias',
    'a2a266d0498c3104214a47bd64ab0fc8': 'alias',

    # Progress & Wait
    'watch': 'wait',
    'half-busy': 'progress',
    'left_ptr_watch': 'progress',
    '00000000000000020006000e7e9ffc3f': 'progress',
    '08e8e1c95fe2fc01f976f1e063a24ccd': 'progress',
    '3ecb610c1bf2410f44200f48c40d3599': 'progress',

    # Precision & Tools
    'draft': 'crosshair',
    'pencil': 'crosshair',
    'color-picker': 'crosshair',
    'zoom-in': 'crosshair',
    'zoom-out': 'crosshair',
    'tcross': 'crosshair',
    'cross_reverse': 'crosshair',
    'diamond_cross': 'crosshair',

    # General navigation & Wayland / X11 fallbacks
    'context-menu': 'default',
    'vertical-text': 'text',
    'wayland-cursor': 'default',
    'x-cursor': 'default',
    'X_cursor': 'default',
    'right_ptr': 'default',
    'left-arrow': 'default',
    'right-arrow': 'default',
    'up-arrow': 'default',
    'down-arrow': 'default',
}

def generate_theme(out_dir):
    cursors_dir = os.path.join(out_dir, "cursors")
    os.makedirs(cursors_dir, exist_ok=True)

    # 1. Generate base cursor files
    for name, gen_fn in CURSOR_GENERATORS.items():
        images = []
        for s in SIZES:
            surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, s, s)
            cr = cairo.Context(surf)
            cr.set_antialias(cairo.ANTIALIAS_BEST)
            gen_fn(cr, s)
            surf.flush()
            data = bytes(surf.get_data())
            # Hotspot is at the exact center of the black dot
            xhot = s // 2
            yhot = s // 2
            images.append({
                'size': s,
                'width': s,
                'height': s,
                'xhot': xhot,
                'yhot': yhot,
                'delay': 0,
                'data': data
            })

        xcur_bytes = pack_xcursor(images)
        target_path = os.path.join(cursors_dir, name)
        with open(target_path, 'wb') as f:
            f.write(xcur_bytes)

    # 2. Create symlinks
    for alias, target in SYMLINKS.items():
        alias_path = os.path.join(cursors_dir, alias)
        if os.path.lexists(alias_path):
            os.remove(alias_path)
        os.symlink(target, alias_path)

    # 3. Write index.theme
    index_theme = """[Icon Theme]
Name=Aura-Circle
Comment=Apple iPadOS/VisionOS circular dot cursor theme (50% translucent outer ring, solid black center dot)
Inherits=default
"""
    with open(os.path.join(out_dir, "index.theme"), "w") as f:
        f.write(index_theme)

    print(f"Successfully generated Aura-Circle cursor theme at {out_dir}")

def generate_preview(out_png):
    # Generate a nice preview grid of the cursors
    cols = 4
    rows = 4
    cell_w, cell_h = 100, 100
    w = cols * cell_w
    h = rows * cell_h

    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, w, h)
    cr = cairo.Context(surf)
    cr.set_antialias(cairo.ANTIALIAS_BEST)

    # Checkerboard background so 50% opacity is clearly visible
    check_size = 10
    for y in range(0, h, check_size):
        for x in range(0, w, check_size):
            if ((x // check_size) + (y // check_size)) % 2 == 0:
                cr.set_source_rgb(0.92, 0.92, 0.94)
            else:
                cr.set_source_rgb(0.98, 0.98, 1.0)
            cr.rectangle(x, y, check_size, check_size)
            cr.fill()

    keys = list(CURSOR_GENERATORS.keys())[:16]
    for idx, key in enumerate(keys):
        r = idx // cols
        c = idx % cols
        bx = c * cell_w
        by = r * cell_h

        cr.save()
        cr.translate(bx + cell_w/2 - 24, by + cell_h/2 - 28)
        CURSOR_GENERATORS[key](cr, 48)
        cr.restore()

        # Label
        cr.select_font_face("Sans", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_NORMAL)
        cr.set_font_size(11)
        cr.set_source_rgb(0.2, 0.2, 0.2)
        ext = cr.text_extents(key)
        cr.move_to(bx + (cell_w - ext.width)/2, by + cell_h - 12)
        cr.show_text(key)

    surf.write_to_png(out_png)
    print(f"Saved preview grid to {out_png}")

if __name__ == '__main__':
    user_theme_dir1 = os.path.expanduser("~/.local/share/icons/Aura-Circle")
    user_theme_dir2 = os.path.expanduser("~/.icons/Aura-Circle")

    generate_theme(user_theme_dir1)
    if os.path.lexists(user_theme_dir2):
        if os.path.islink(user_theme_dir2):
            os.remove(user_theme_dir2)
        else:
            shutil.rmtree(user_theme_dir2)
    os.makedirs(os.path.dirname(user_theme_dir2), exist_ok=True)
    os.symlink(user_theme_dir1, user_theme_dir2)

    # Universal theme overrides:
    # Ensure that even if any app hardcodes 'default', 'WhiteSur-cursors', or 'Adwaita',
    # it gets Aura-Circle directly.
    for override_name in ['default', 'WhiteSur-cursors', 'Adwaita']:
        odir1 = os.path.expanduser(f"~/.local/share/icons/{override_name}")
        odir2 = os.path.expanduser(f"~/.icons/{override_name}")
        for odir in [odir1, odir2]:
            if os.path.islink(odir):
                os.remove(odir)
            elif os.path.isdir(odir):
                shutil.rmtree(odir)
            os.makedirs(odir, exist_ok=True)
            with open(os.path.join(odir, "index.theme"), "w") as f:
                f.write(f"[Icon Theme]\nName={override_name}\nComment=Redirect to Aura-Circle\nInherits=Aura-Circle\n")
            curs_link = os.path.join(odir, "cursors")
            if not os.path.lexists(curs_link):
                os.symlink("../Aura-Circle/cursors", curs_link)

    preview_path = "/tmp/aura_circle_preview.png"
    generate_preview(preview_path)

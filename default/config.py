# wate configuration. Reloaded on save; on an error the old settings stay.
# Colors: "0xRRGGBB", "0xRRGGBBAA" or "#RRGGBB".

font = "monospace:size=11"
pad = "12x12"
size = "80x24"
scrollback = 1000
shell = ""                         # "" = $SHELL, then /bin/sh
term = "xterm-256color"
title = "wate"                     # fallback; a program's own title wins
app_id = "wate"
alpha: float = 1.0                 # background opacity, 0.0 .. 1.0
process_saver = False              # True: programs outlive the window

class Mouse:
    hide_when_typing: bool = True

class Cursor:
    style: str = "block"           # block, beam or underline
    color: str = ""                # "" = the colours of the cell, swapped

class Colors:
    fg: str = "0xE2E5DAff"
    bg: str = "0x1C1E17ff"

class Ansi_Colors:
    regular0: str = "0x3A3F30ff"
    regular1: str = "0xB3766Aff"
    regular2: str = "0x7FA67Cff"
    regular3: str = "0xB8AD88ff"
    regular4: str = "0x7F98B8ff"
    regular5: str = "0x9E90AEff"
    regular6: str = "0x78A6A3ff"
    regular7: str = "0xC3C8B6ff"
    bright0: str = "0x7A8168ff"
    bright1: str = "0xC98B7Eff"
    bright2: str = "0x9CC49Aff"
    bright3: str = "0xCBC29Fff"
    bright4: str = "0xA2B8D6ff"
    bright5: str = "0xB5A8C4ff"
    bright6: str = "0x93C0BCff"
    bright7: str = "0xEEF0E6ff"

class Selection_Links_colors:
    selection_fg: str = "0xEEF0E6ff"
    selection_bg: str = "0x3A3F30ff"
    urls: str = "0xB8AD88ff"

keys = [
    ["CTRL", "equal", "fontsize", 1],
    ["CTRL", "minus", "fontsize", -1],
]

buttons = [
    ["CTRL", "wheel_up", "fontsize", 1],
    ["CTRL", "wheel_down", "fontsize", -1],
]

# wate

**wate** (*Wayland terminal*) is a small terminal emulator for Wayland, written in C.

* Configured with a plain Python file (`~/.config/wate/config.py`), live reload in every open window.
* One thread, no timers while idle, no Python or fontconfig staying in memory.
* Text reflow on resize, fallback fonts, true colour, mouse for programs, clipboard and primary selection, fractional scaling.

## Build

Needs `gcc`, `make`, `pkg-config`, the development files of `wayland-client`, `wayland-cursor`, `xkbcommon`, `freetype2`, `fontconfig`, and `python3` at run time.

    make
    sudo make install        # PREFIX=/usr to change
    sudo make uninstall

## Usage

    wate [OPTION]... [COMMAND [ARGUMENT]...]

| Option | Meaning |
|---|---|
| `-c FILE` | configuration file (`-c ""` = built-in defaults) |
| `-s SHELL` | shell to use |
| `-t TITLE` | window title |
| `-a APP_ID` | Wayland application id |
| `-l` | keep the window open after the program exits |
| `-v`, `-h` | version, help |

## Configuration

`~/.config/wate/config.py` is created from `default/config.py` on first run. It is ordinary Python; everything is optional:

```python
font = "Libertinus Mono:size=11"
pad = "12x12"
size = "80x24"
scrollback = 1000
alpha = 1.0

class Colors:
    fg: str = "0xE2E5DAff"
    bg: str = "0x1C1E17ff"

keys = [["CTRL", "equal", "fontsize", 1]]
buttons = [["CTRL", "wheel_up", "fontsize", 1]]
```

Key and mouse bindings are `[modifiers, key, action, argument]`. Actions: `copy`, `paste`, `paste_primary`, `fontsize`, `fontsize_reset`, `scroll_page_up`, `scroll_page_down`, `scroll_up`, `scroll_down`, `scroll_top`, `scroll_bottom`, `reset`, `new_terminal`, `none`.

A file with an error is ignored and the old settings stay.

## Files

    default/config.py  default configuration
    src/               source (libtsm in src/tsm, Wayland bindings in src/protocol)

# CMatrix for Windows

A native Windows port of [CMatrix](https://github.com/abishekvashok/cmatrix) — the classic Matrix rain terminal animation.

This version uses the **Win32 Console API** directly with **Virtual Terminal Sequence** support. No ncurses, no PDCurses, no Cygwin/MSYS required. Just compile and run.

## Building

### MSVC (Visual Studio Developer Command Prompt)
```
cl /O2 cmatrix_win.c /Fe:cmatrix.exe
```

### MinGW / GCC
```
gcc -O2 -o cmatrix.exe cmatrix_win.c
```

### Zig CC
```
zig cc -O2 -o cmatrix.exe cmatrix_win.c
```

## Usage

```
cmatrix [-abBcfhnosmrkV] [-u delay] [-C color] [-M message]
```

### Options
| Flag | Description |
|------|-------------|
| `-a` | Asynchronous scroll |
| `-b` | Bold characters on |
| `-B` | All bold characters (overrides -b) |
| `-c` | Use Japanese half-width katakana characters |
| `-h` | Print usage and exit |
| `-n` | No bold characters (default) |
| `-o` | Old-style scrolling |
| `-s` | Screensaver mode — exits on first keystroke |
| `-r` | Rainbow mode |
| `-m` | Lambda mode |
| `-k` | Characters change while scrolling |
| `-V` | Print version and exit |
| `-u N` | Update delay (0–10, default 4) |
| `-C color` | Set color: green, red, blue, white, yellow, cyan, magenta, black |
| `-M msg` | Display a message in the center of the screen |

### Runtime Keys
| Key | Action |
|-----|--------|
| `q` / `ESC` | Quit |
| `a` | Toggle async scroll |
| `b` / `B` / `n` | Bold on / all bold / bold off |
| `0`–`9` | Set update speed |
| `r` | Toggle rainbow mode |
| `m` | Toggle lambda mode |
| `p` | Pause / resume |
| `!@#$%^&` | Set color (red/green/yellow/blue/magenta/cyan/white) |

## Compatibility

- **Windows Terminal** — Full color, VT sequences, resize support ✓
- **Modern conhost** (Win10 1607+) — Full color via VT ✓
- **Older conhost** — Falls back to `SetConsoleTextAttribute` ✓
- **CMD / PowerShell** — Works in both ✓

## Credits

Original CMatrix by Chris Allegretta & Abishek V Ashok.  
Licensed under GPL v3.

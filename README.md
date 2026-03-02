# CMatrix for Windows

A native Windows port of [CMatrix](https://github.com/abishekvashok/cmatrix) — the classic Matrix rain terminal animation.

Built entirely on the **Win32 Console API** with **Virtual Terminal Sequence** support. No ncurses, no PDCurses, no Cygwin, no MSYS, no WSL. One C file, zero dependencies — just compile and run.

---

## Building from Source

You need **one** of the following C compilers installed. Pick whichever you already have.

### Option 1 — Use the Build Scripts (Easiest)

Both scripts auto-detect your compiler. Just run one from inside the project folder.

**PowerShell** (recommended — has extra features):
```powershell
.\build.ps1                          # auto-detect compiler, build
.\build.ps1 -Run                     # build and launch immediately
.\build.ps1 -Run -RunArgs '-c -B'    # build and launch with flags
.\build.ps1 -Compiler gcc            # force a specific compiler
.\build.ps1 -Clean                   # remove build artifacts
```

The PowerShell script will also try to find and import the Visual Studio developer environment automatically if `cl.exe` isn't already on your PATH. If no compiler is found, it prints `winget` install commands for each option.

**CMD / Batch**:
```cmd
build.bat
```

Tries MSVC first, then GCC, then Zig. Prints the result and some example commands to try.

### Option 2 — Compile Manually

Open your preferred terminal and run one of these:

**MSVC** (Visual Studio Developer Command Prompt or after running `vcvarsall.bat`):
```
cl /O2 cmatrix_win.c /Fe:cmatrix.exe
```

**MinGW / GCC**:
```
gcc -O2 -o cmatrix.exe cmatrix_win.c
```

**Zig CC**:
```
zig cc -O2 -o cmatrix.exe cmatrix_win.c
```

All three produce the same standalone `cmatrix.exe` with no runtime dependencies.

### Installing a Compiler

If you don't have a C compiler yet, the fastest options via `winget`:

```
winget install -e --id Microsoft.VisualStudio.2022.BuildTools
winget install -e --id MingW-w64.MingW-w64
winget install -e --id zig.zig
```

---

## Usage

```
cmatrix.exe [-abBchnosmrkV] [-u delay] [-C color] [-M message]
```

### Command-Line Flags

| Flag | Description |
|------|-------------|
| `-a` | Asynchronous scroll (columns fall at different speeds) |
| `-b` | Bold characters on |
| `-B` | All bold characters (overrides `-b`) |
| `-c` | Use Japanese half-width katakana characters |
| `-k` | Characters change while scrolling (mutation effect) |
| `-m` | Lambda mode (λ characters) |
| `-n` | No bold characters (default) |
| `-o` | Old-style scrolling (all columns at same speed) |
| `-r` | Rainbow mode |
| `-s` | Screensaver mode — exits on first keystroke |
| `-h` | Print usage and exit |
| `-V` | Print version and exit |
| `-u N` | Screen update delay, `0`–`10` (default `4`, lower = faster) |
| `-C color` | Set color: `green` `red` `blue` `white` `yellow` `cyan` `magenta` `black` |
| `-M msg` | Display a message centered on screen |

### Examples

```
cmatrix.exe                    # default green rain
cmatrix.exe -c -B              # katakana characters, all bold
cmatrix.exe -r                 # rainbow mode
cmatrix.exe -C red -s          # red screensaver (exits on keypress)
cmatrix.exe -c -B -r           # katakana + bold + rainbow
cmatrix.exe -u 0 -k            # max speed with mutating characters
cmatrix.exe -M "WAKE UP NEO"   # centered message overlay
```

### Runtime Keys

These keys work while the animation is running:

| Key | Action |
|-----|--------|
| `q` / `ESC` / `Ctrl+C` | Quit |
| `p` | Pause / resume |
| `a` | Toggle async scroll |
| `b` | Bold on |
| `B` | All bold |
| `n` | Bold off |
| `r` | Toggle rainbow mode |
| `m` | Toggle lambda mode |
| `0`–`9` | Set update speed (0 = fastest) |
| `!` | Set color: red |
| `@` | Set color: green |
| `#` | Set color: yellow |
| `$` | Set color: blue |
| `%` | Set color: magenta |
| `^` | Set color: cyan |
| `&` | Set color: white |

---

## Compatibility

| Terminal | Support |
|----------|---------|
| **Windows Terminal** | Full color, VT sequences, resize support |
| **Modern conhost** (Win10 1607+) | Full color via VT |
| **Older conhost** (pre-1607) | Legacy fallback via `SetConsoleTextAttribute` |
| **CMD** | Works |
| **PowerShell** | Works |

The program detects VT support at startup and falls back to the legacy Win32 console color API automatically. No configuration needed.

---

## Technical Notes

- Single-file C source, ~770 lines
- Entire frame composed in a memory buffer, single `WriteConsole` call per frame
- Compact 4-byte `Cell` struct for cache efficiency
- Fast xorshift32 PRNG (avoids CRT `rand()` mutex overhead on MSVC)
- Color state tracking to minimize VT escape output
- `Sleep` floor prevents busy-loop at `update=0`

---

## Credits

Original CMatrix by Chris Allegretta & Abishek V Ashok.
Windows port by BlackoutLLC.

Licensed under [GPL v3](./COPYING).

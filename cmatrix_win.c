/*
    cmatrix_win.c  v2.3

    Windows-native port of CMatrix
    Original Copyright (C) 1999-2017 Chris Allegretta
    Original Copyright (C) 2017-Present Abishek V Ashok
    Windows port: 2026

    cmatrix is free software under the GNU General Public License v3.

    Uses the Windows Console API directly - no ncurses dependency.
    Supports VT sequences (Win10+) with legacy WriteConsoleOutputW fallback.

    Performance notes:
    - Entire frame composed in memory buffer, single WriteConsole per frame
    - Compact 4-byte Cell struct for cache efficiency
    - Fast xorshift32 PRNG (no CRT mutex overhead from rand())
    - Color state tracking to minimize VT escape output
    - Only allocates the buffer path actually in use (VT or legacy)
    - Sleep floor prevents busy-loop at update=0

    Build:  cl /O2 cmatrix_win.c /Fe:cmatrix.exe
            gcc -O2 -o cmatrix.exe cmatrix_win.c
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <locale.h>

#define CMATRIX_VERSION "2.3-win"

/* ---- Matrix cell (4 bytes, cache-friendly) --------------------------- */
typedef struct {
    short val;      /* character code: -1=empty, 0=head marker, >0=char */
    char  is_head;  /* 1=bright white leading char */
    char  pad_;     /* alignment padding */
} Cell;

/* ---- Color indices --------------------------------------------------- */
enum {
    C_BLACK = 0, C_RED, C_GREEN, C_YELLOW, C_BLUE, C_MAGENTA, C_CYAN, C_WHITE
};

/* ---- Win32 console attribute colors (legacy fallback) ---------------- */
static const WORD win_colors[] = {
    0,
    FOREGROUND_RED,
    FOREGROUND_GREEN,
    FOREGROUND_RED | FOREGROUND_GREEN,
    FOREGROUND_BLUE,
    FOREGROUND_RED | FOREGROUND_BLUE,
    FOREGROUND_GREEN | FOREGROUND_BLUE,
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
};

/* ---- Fast PRNG (xorshift32) ----------------------------------------- */
/* Avoids the CRT rand() mutex on MSVC. Inlined for hot-path speed.     */
static unsigned int g_rng_state;

static void rng_seed(unsigned int s) {
    g_rng_state = s ? s : 1;  /* must be nonzero */
}

static __inline unsigned int rng_next(void) {
    unsigned int x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

/* Fast modulo for small ranges (avoids division when possible) */
static __inline int rng_mod(int n) {
    return (int)(rng_next() % (unsigned int)n);
}

/* ---- Global state ---------------------------------------------------- */
static int    g_use_vt = 0;
static HANDLE g_hOut   = INVALID_HANDLE_VALUE;
static HANDLE g_hIn    = INVALID_HANDLE_VALUE;
static DWORD  g_origOutMode = 0;
static DWORD  g_origInMode  = 0;
static CONSOLE_CURSOR_INFO g_origCursor;

static int g_rows = 0, g_cols = 0;

static Cell  *g_cells   = NULL;  /* flat array: (g_rows+1) * g_cols */
static Cell **matrix    = NULL;  /* row pointers into g_cells */
static int   *lengths   = NULL;
static int   *spaces    = NULL;
static int   *updates   = NULL;

/* ---- Frame buffers (only one is allocated based on g_use_vt) --------- */
static char      *g_vt_buf     = NULL;
static int        g_vt_buf_cap = 0;
static int        g_vt_pos     = 0;
static CHAR_INFO *g_legacy_buf = NULL;

/* ---- VT buffer helpers ----------------------------------------------- */
static void vt_reset_buf(void) { g_vt_pos = 0; }

static __inline void vt_append(const char *s, int len) {
    memcpy(g_vt_buf + g_vt_pos, s, len);
    g_vt_pos += len;
}

static void vt_appends(const char *s) {
    int len = (int)strlen(s);
    memcpy(g_vt_buf + g_vt_pos, s, len);
    g_vt_pos += len;
}

static __inline void vt_appendc(char c) {
    g_vt_buf[g_vt_pos++] = c;
}

/* Pre-computed SGR sequences — no sprintf in the hot path */
static const char *vt_color_seqs[] = {
    "\x1b[30m", "\x1b[31m", "\x1b[32m", "\x1b[33m",
    "\x1b[34m", "\x1b[35m", "\x1b[36m", "\x1b[37m",
};
static const char *vt_bold_color_seqs[] = {
    "\x1b[1;30m", "\x1b[1;31m", "\x1b[1;32m", "\x1b[1;33m",
    "\x1b[1;34m", "\x1b[1;35m", "\x1b[1;36m", "\x1b[1;37m",
};

static __inline void vt_set_color(int color, int is_bold) {
    if (is_bold)
        vt_append(vt_bold_color_seqs[color], 7);
    else
        vt_append(vt_color_seqs[color], 5);
}

static __inline void vt_put_char(int val) {
    if (val <= 0x7F) {
        vt_appendc((char)val);
    } else if (val <= 0x7FF) {
        vt_appendc((char)(0xC0 | (val >> 6)));
        vt_appendc((char)(0x80 | (val & 0x3F)));
    } else {
        vt_appendc((char)(0xE0 | (val >> 12)));
        vt_appendc((char)(0x80 | ((val >> 6) & 0x3F)));
        vt_appendc((char)(0x80 | (val & 0x3F)));
    }
}

static void vt_flush(void) {
    if (g_vt_pos > 0) {
        DWORD written;
        WriteConsoleA(g_hOut, g_vt_buf, g_vt_pos, &written, NULL);
        g_vt_pos = 0;
    }
}

/* ---- Console helpers ------------------------------------------------- */
static void get_console_size(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(g_hOut, &csbi)) {
        g_cols = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
        g_rows = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
    } else {
        g_cols = 80;
        g_rows = 25;
    }
    if (g_cols < 10) g_cols = 10;
    if (g_rows < 10) g_rows = 10;
}

static void alloc_frame_buffers(void) {
    if (g_use_vt) {
        /* ~40 bytes worst case per cell + overhead */
        int needed = g_rows * g_cols * 40 + 4096;
        if (needed > g_vt_buf_cap) {
            free(g_vt_buf);
            g_vt_buf = (char *)malloc(needed);
            g_vt_buf_cap = needed;
        }
        /* Don't allocate legacy buffer in VT mode */
        free(g_legacy_buf);
        g_legacy_buf = NULL;
    } else {
        /* Don't allocate VT buffer in legacy mode */
        free(g_vt_buf);
        g_vt_buf = NULL;
        g_vt_buf_cap = 0;

        free(g_legacy_buf);
        g_legacy_buf = (CHAR_INFO *)malloc(sizeof(CHAR_INFO) * g_rows * g_cols);
        /* Zero-fill so any untouched cells render as blank */
        memset(g_legacy_buf, 0, sizeof(CHAR_INFO) * g_rows * g_cols);
    }
}

static void clear_screen(void) {
    if (g_use_vt) {
        DWORD written;
        WriteConsoleA(g_hOut, "\x1b[2J\x1b[H", 7, &written, NULL);
    } else {
        COORD origin = {0, 0};
        DWORD written, size = g_rows * g_cols;
        FillConsoleOutputCharacterW(g_hOut, L' ', size, origin, &written);
        FillConsoleOutputAttribute(g_hOut,
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
            size, origin, &written);
        SetConsoleCursorPosition(g_hOut, origin);
    }
}

static void hide_cursor(void) {
    if (g_use_vt) {
        DWORD w;
        WriteConsoleA(g_hOut, "\x1b[?25l", 6, &w, NULL);
    } else {
        CONSOLE_CURSOR_INFO ci = { 1, FALSE };
        SetConsoleCursorInfo(g_hOut, &ci);
    }
}

static void show_cursor(void) {
    if (g_use_vt) {
        DWORD w;
        WriteConsoleA(g_hOut, "\x1b[?25h\x1b[0m", 10, &w, NULL);
    } else {
        SetConsoleCursorInfo(g_hOut, &g_origCursor);
        SetConsoleTextAttribute(g_hOut, FOREGROUND_RED|FOREGROUND_GREEN|FOREGROUND_BLUE);
    }
}

/* ---- Matrix state management ----------------------------------------- */
static void var_init(void) {
    int i, j;
    int total_cells = (g_rows + 1) * g_cols;

    /* Free old allocations and NULL them */
    free(g_cells);   g_cells  = NULL;
    free(matrix);    matrix   = NULL;
    free(lengths);   lengths  = NULL;
    free(spaces);    spaces   = NULL;
    free(updates);   updates  = NULL;

    /* Single contiguous allocation for all cells */
    g_cells = (Cell *)malloc(sizeof(Cell) * total_cells);
    matrix  = (Cell **)malloc(sizeof(Cell *) * (g_rows + 1));

    if (!g_cells || !matrix) {
        fprintf(stderr, "cmatrix: out of memory\n");
        exit(1);
    }

    /* Zero-fill entire cell array (val=-1 needs explicit set, but
       is_head=0 and pad_=0 are handled by this) */
    memset(g_cells, 0, sizeof(Cell) * total_cells);

    /* Set up row pointers */
    for (i = 0; i <= g_rows; i++)
        matrix[i] = g_cells + i * g_cols;

    lengths = (int *)malloc(g_cols * sizeof(int));
    spaces  = (int *)malloc(g_cols * sizeof(int));
    updates = (int *)malloc(g_cols * sizeof(int));

    if (!lengths || !spaces || !updates) {
        fprintf(stderr, "cmatrix: out of memory\n");
        exit(1);
    }

    /* Initialize matrix: only even columns are used (step 2) */
    for (i = 0; i <= g_rows; i++)
        for (j = 0; j < g_cols; j += 2)
            matrix[i][j].val = -1;

    for (j = 0; j < g_cols; j += 2) {
        spaces[j]  = rng_mod(g_rows) + 1;
        lengths[j] = rng_mod(g_rows - 3) + 3;
        matrix[1][j].val = ' ';
        updates[j] = rng_mod(3) + 1;
    }

    alloc_frame_buffers();
}

/* ---- Cleanup and exit ------------------------------------------------ */
static void finish(int code) {
    show_cursor();
    clear_screen();
    SetConsoleMode(g_hOut, g_origOutMode);
    SetConsoleMode(g_hIn,  g_origInMode);

    free(g_cells);      g_cells     = NULL;
    free(matrix);       matrix      = NULL;
    free(lengths);      lengths     = NULL;
    free(spaces);       spaces      = NULL;
    free(updates);      updates     = NULL;
    free(g_vt_buf);     g_vt_buf    = NULL;
    free(g_legacy_buf); g_legacy_buf = NULL;

    exit(code);
}

/* ---- Usage ----------------------------------------------------------- */
static void usage(void) {
    printf("Usage: cmatrix [-abBcfhnosmrkV] [-u delay] [-C color] [-M message]\n");
    printf("  -a  Asynchronous scroll\n");
    printf("  -b  Bold characters on\n");
    printf("  -B  All bold characters (overrides -b)\n");
    printf("  -c  Use Japanese half-width katakana characters\n");
    printf("  -h  Print usage and exit\n");
    printf("  -n  No bold characters (default)\n");
    printf("  -o  Old-style scrolling\n");
    printf("  -s  Screensaver mode, exits on first keystroke\n");
    printf("  -V  Print version information and exit\n");
    printf("  -M message  Display message in center of screen\n");
    printf("  -u delay    Screen update delay (0-10, default 4)\n");
    printf("  -C color    Matrix color (green,red,blue,white,yellow,cyan,magenta,black)\n");
    printf("  -r  Rainbow mode\n");
    printf("  -m  Lambda mode\n");
    printf("  -k  Characters change while scrolling\n");
}

static int parse_color(const char *name) {
    if (!_stricmp(name, "green"))   return C_GREEN;
    if (!_stricmp(name, "red"))     return C_RED;
    if (!_stricmp(name, "blue"))    return C_BLUE;
    if (!_stricmp(name, "white"))   return C_WHITE;
    if (!_stricmp(name, "yellow"))  return C_YELLOW;
    if (!_stricmp(name, "cyan"))    return C_CYAN;
    if (!_stricmp(name, "magenta")) return C_MAGENTA;
    if (!_stricmp(name, "black"))   return C_BLACK;
    return -1;
}

/* ==== RENDER: VT path ================================================= */
static void render_frame_vt(int y_start, int y_end,
                            int mcolor, int bold, int rainbow, int lambda,
                            const char *msg)
{
    static const int rainbow_colors[] = {
        C_GREEN, C_BLUE, C_BLACK, C_YELLOW, C_CYAN, C_MAGENTA
    };
    int i, j, row;
    int cur_color = -1, cur_bold = -1;

    vt_reset_buf();
    vt_append("\x1b[H", 3);

    for (row = 0; row < g_rows; row++) {
        i = row + y_start;
        if (i > y_end) {
            /* Fill remaining rows with spaces (blank line + newline) */
            for (j = 0; j < g_cols; j++) vt_appendc(' ');
            if (row < g_rows - 1) vt_append("\r\n", 2);
            continue;
        }

        for (j = 0; j < g_cols; j += 2) {
            int val  = matrix[i][j].val;
            int head = matrix[i][j].is_head;

            if (val == -1 || val == ' ') {
                /* Empty cell — only emit reset on first empty after colored */
                if (cur_color != -1) {
                    vt_append("\x1b[0m", 4);
                    cur_color = -1;
                    cur_bold  = -1;
                }
                vt_appendc(' ');
            } else if (val == 0 || (head && !rainbow)) {
                int wb = bold ? 1 : 0;
                if (cur_color != C_WHITE || cur_bold != wb) {
                    vt_set_color(C_WHITE, wb);
                    cur_color = C_WHITE;
                    cur_bold  = wb;
                }
                vt_appendc(val == 0 ? '&' : (char)val);
            } else {
                int color = mcolor;
                if (rainbow)
                    color = rainbow_colors[rng_mod(6)];

                int wb = (bold == 2) || (bold == 1 && val % 2 == 0);
                if (cur_color != color || cur_bold != wb) {
                    vt_set_color(color, wb);
                    cur_color = color;
                    cur_bold  = wb;
                }

                if (val == 1) {
                    vt_appendc('|');
                } else if (lambda) {
                    vt_appendc((char)0xCE);
                    vt_appendc((char)0xBB);
                } else {
                    vt_put_char(val);
                }
            }
            vt_appendc(' ');  /* column spacing */
        }

        if (row < g_rows - 1)
            vt_append("\r\n", 2);
    }

    /* Center message overlay */
    if (msg[0] != '\0') {
        int mx = g_rows / 2;
        int my = g_cols / 2 - (int)strlen(msg) / 2;
        int mlen = (int)strlen(msg);
        char esc[32];
        int elen;

        vt_append("\x1b[0m", 4);

        elen = sprintf(esc, "\x1b[%d;%dH", mx, my - 1);
        vt_append(esc, elen);
        for (i = 0; i < mlen + 4; i++) vt_appendc(' ');

        elen = sprintf(esc, "\x1b[%d;%dH", mx + 1, my - 1);
        vt_append(esc, elen);
        vt_append("  ", 2);
        vt_appends(msg);
        vt_append("  ", 2);

        elen = sprintf(esc, "\x1b[%d;%dH", mx + 2, my - 1);
        vt_append(esc, elen);
        for (i = 0; i < mlen + 4; i++) vt_appendc(' ');
    }

    vt_flush();
}

/* ==== RENDER: Legacy path ============================================= */
static void render_frame_legacy(int y_start, int y_end,
                                int mcolor, int bold, int rainbow, int lambda,
                                const char *msg)
{
    static const int rainbow_colors[] = {
        C_GREEN, C_BLUE, C_BLACK, C_YELLOW, C_CYAN, C_MAGENTA
    };
    int i, j, row, idx;
    int buf_size = g_rows * g_cols;

    /* Zero the whole buffer first — handles gaps and any
       rows beyond y_end cleanly with no stale data */
    memset(g_legacy_buf, 0, sizeof(CHAR_INFO) * buf_size);

    for (row = 0; row < g_rows; row++) {
        i = row + y_start;
        if (i > y_end) break;

        for (j = 0; j < g_cols; j += 2) {
            idx = row * g_cols + j;

            int val  = matrix[i][j].val;
            int head = matrix[i][j].is_head;

            if (val == -1 || val == ' ') {
                /* Already zeroed by memset — skip */
                continue;
            } else if (val == 0 || (head && !rainbow)) {
                WORD attr = win_colors[C_WHITE];
                if (bold) attr |= FOREGROUND_INTENSITY;
                g_legacy_buf[idx].Char.UnicodeChar = (val == 0) ? L'&' : (wchar_t)val;
                g_legacy_buf[idx].Attributes = attr;
            } else {
                int color = mcolor;
                if (rainbow)
                    color = rainbow_colors[rng_mod(6)];

                WORD attr = win_colors[color];
                if ((bold == 2) || (bold == 1 && val % 2 == 0))
                    attr |= FOREGROUND_INTENSITY;

                wchar_t ch;
                if (val == 1)       ch = L'|';
                else if (lambda)    ch = L'\x03BB';
                else                ch = (wchar_t)val;

                g_legacy_buf[idx].Char.UnicodeChar = ch;
                g_legacy_buf[idx].Attributes = attr;
            }
        }
    }

    /* Center message overlay */
    if (msg[0] != '\0') {
        int mx = g_rows / 2;
        int my = g_cols / 2 - (int)strlen(msg) / 2;
        int mlen = (int)strlen(msg);
        WORD ma = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        int line, c;

        for (line = mx - 1; line <= mx + 1; line++) {
            if (line < 0 || line >= g_rows) continue;
            for (c = my - 2; c < my + mlen + 2; c++) {
                if (c < 0 || c >= g_cols) continue;
                idx = line * g_cols + c;
                g_legacy_buf[idx].Char.UnicodeChar =
                    (line == mx && c >= my && c < my + mlen)
                        ? (wchar_t)msg[c - my] : L' ';
                g_legacy_buf[idx].Attributes = ma;
            }
        }
    }

    COORD bufSize  = { (SHORT)g_cols, (SHORT)g_rows };
    COORD bufCoord = { 0, 0 };
    SMALL_RECT wr  = { 0, 0, (SHORT)(g_cols - 1), (SHORT)(g_rows - 1) };
    WriteConsoleOutputW(g_hOut, g_legacy_buf, bufSize, bufCoord, &wr);
}

/* ==== MAIN ============================================================ */
int main(int argc, char *argv[]) {
    int i, j, y, z;
    int count        = 0;
    int screensaver  = 0;
    int asynch       = 0;
    int bold         = 0;
    int oldstyle     = 0;
    int update       = 4;
    int mcolor       = C_GREEN;
    int rainbow      = 0;
    int lambda       = 0;
    int paused       = 0;
    int classic      = 0;
    int changes      = 0;
    int randnum, randmin, highnum;
    int firstcoldone;
    char *msg = "";

    rng_seed((unsigned int)time(NULL) ^ (unsigned int)GetCurrentProcessId());
    setlocale(LC_ALL, "");

    /* ---- Parse arguments --------------------------------------------- */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' || argv[i][0] == '/') {
            char *p = &argv[i][1];
            while (*p) {
                switch (*p) {
                case 'a': asynch = 1; break;
                case 'b': if (bold != 2) bold = 1; break;
                case 'B': bold = 2; break;
                case 'c': classic = 1; break;
                case 'h': case '?': usage(); return 0;
                case 'n': bold = -1; break;
                case 'o': oldstyle = 1; break;
                case 's': screensaver = 1; break;
                case 'r': rainbow = 1; break;
                case 'm': lambda = 1; break;
                case 'k': changes = 1; break;
                case 'V':
                    printf("CMatrix version %s (Windows native)\n", CMATRIX_VERSION);
                    return 0;
                case 'u':
                    if (i + 1 < argc) { update = atoi(argv[++i]); }
                    goto next_arg;
                case 'C':
                    if (i + 1 < argc) {
                        int c = parse_color(argv[++i]);
                        if (c < 0) {
                            fprintf(stderr, "Invalid color. Valid: green,red,blue,white,yellow,cyan,magenta,black\n");
                            return 1;
                        }
                        mcolor = c;
                    }
                    goto next_arg;
                case 'M':
                    if (i + 1 < argc) { msg = argv[++i]; }
                    goto next_arg;
                default:
                    fprintf(stderr, "Unknown option: -%c\n", *p);
                    usage();
                    return 1;
                }
                p++;
            }
        }
        next_arg:;
    }

    if (update < 0)  update = 0;
    if (update > 10) update = 10;

    /* ---- Set up console ---------------------------------------------- */
    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    g_hIn  = GetStdHandle(STD_INPUT_HANDLE);

    GetConsoleMode(g_hOut, &g_origOutMode);
    GetConsoleMode(g_hIn,  &g_origInMode);
    GetConsoleCursorInfo(g_hOut, &g_origCursor);

    /* Try to enable Virtual Terminal Processing (Windows 10 1607+) */
    {
        DWORD outMode = g_origOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                                      | DISABLE_NEWLINE_AUTO_RETURN;
        if (SetConsoleMode(g_hOut, outMode)) {
            g_use_vt = 1;
        } else {
            outMode = g_origOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            if (SetConsoleMode(g_hOut, outMode))
                g_use_vt = 1;
        }
    }

    SetConsoleMode(g_hIn, ENABLE_WINDOW_INPUT);

    if (g_use_vt) {
        SetConsoleOutputCP(65001);
        SetConsoleCP(65001);
    }

    get_console_size();
    hide_cursor();
    clear_screen();

    /* ---- Character ranges -------------------------------------------- */
    if (classic) {
        randmin = 0xFF66;
        highnum = 0xFF9D;
    } else {
        randmin = 33;
        highnum = 123;
    }
    randnum = highnum - randmin;

    var_init();

    /* ---- Main loop --------------------------------------------------- */
    while (1) {
        /* Non-blocking input */
        while (WaitForSingleObject(g_hIn, 0) == WAIT_OBJECT_0) {
            INPUT_RECORD ir;
            DWORD nread;
            if (!ReadConsoleInput(g_hIn, &ir, 1, &nread) || nread == 0)
                break;

            if (ir.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                get_console_size();
                var_init();
                clear_screen();
                continue;
            }

            if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown)
                continue;

            int key = ir.Event.KeyEvent.uChar.AsciiChar;
            if (screensaver && key) finish(0);

            switch (key) {
            case 'q': case 'Q': case 27: case 3:
                finish(0); break;
            case 'a': asynch = !asynch; break;
            case 'b': bold = 1; break;
            case 'B': bold = 2; break;
            case 'n': bold = 0; break;
            case 'r': rainbow = !rainbow; break;
            case 'm': lambda = !lambda; break;
            case 'p': case 'P': paused = !paused; break;
            case '!': mcolor = C_RED;     rainbow = 0; break;
            case '@': mcolor = C_GREEN;   rainbow = 0; break;
            case '#': mcolor = C_YELLOW;  rainbow = 0; break;
            case '$': mcolor = C_BLUE;    rainbow = 0; break;
            case '%': mcolor = C_MAGENTA; rainbow = 0; break;
            case '^': mcolor = C_CYAN;    rainbow = 0; break;
            case '&': mcolor = C_WHITE;   rainbow = 0; break;
            default:
                if (key >= '0' && key <= '9') update = key - '0';
                break;
            }
        }

        if (paused) { Sleep(50); continue; }

        count++;
        if (count > 4) count = 1;

        /* ---- Update columns ------------------------------------------ */
        for (j = 0; j < g_cols; j += 2) {
            if (!(count > updates[j] || asynch == 0))
                continue;

            if (oldstyle) {
                for (i = g_rows - 1; i >= 1; i--)
                    matrix[i][j].val = matrix[i - 1][j].val;

                int rnd = rng_mod(randnum + 8) + randmin;

                if (matrix[1][j].val == 0) {
                    matrix[0][j].val = 1;
                } else if (matrix[1][j].val == ' ' || matrix[1][j].val == -1) {
                    if (spaces[j] > 0) {
                        matrix[0][j].val = ' ';
                        spaces[j]--;
                    } else {
                        matrix[0][j].val = (short)((rng_mod(3) == 1)
                            ? 0 : rng_mod(randnum) + randmin);
                        spaces[j] = rng_mod(g_rows) + 1;
                    }
                } else if (rnd > highnum && matrix[1][j].val != 1) {
                    matrix[0][j].val = ' ';
                } else {
                    matrix[0][j].val = (short)(rng_mod(randnum) + randmin);
                }
            } else {
                if (matrix[0][j].val == -1 && matrix[1][j].val == ' ' && spaces[j] > 0) {
                    spaces[j]--;
                } else if (matrix[0][j].val == -1 && matrix[1][j].val == ' ') {
                    lengths[j] = rng_mod(g_rows - 3) + 3;
                    matrix[0][j].val = (short)(rng_mod(randnum) + randmin);
                    spaces[j] = rng_mod(g_rows) + 1;
                }

                i = 0; y = 0; firstcoldone = 0;
                while (i <= g_rows) {
                    while (i <= g_rows && (matrix[i][j].val == ' ' || matrix[i][j].val == -1))
                        i++;
                    if (i > g_rows) break;

                    z = i; y = 0;
                    while (i <= g_rows && matrix[i][j].val != ' ' && matrix[i][j].val != -1) {
                        matrix[i][j].is_head = 0;
                        if (changes && rng_mod(8) == 0)
                            matrix[i][j].val = (short)(rng_mod(randnum) + randmin);
                        i++; y++;
                    }

                    if (i > g_rows) { matrix[z][j].val = ' '; continue; }

                    matrix[i][j].val = (short)(rng_mod(randnum) + randmin);
                    matrix[i][j].is_head = 1;

                    if (y > lengths[j] || firstcoldone) {
                        matrix[z][j].val = ' ';
                        matrix[0][j].val = -1;
                    }
                    firstcoldone = 1;
                    i++;
                }
            }
        }

        /* ---- Render frame (single write) ----------------------------- */
        {
            int y_start = oldstyle ? 0 : 1;
            int y_end   = oldstyle ? g_rows - 1 : g_rows;

            if (g_use_vt)
                render_frame_vt(y_start, y_end, mcolor, bold, rainbow, lambda, msg);
            else
                render_frame_legacy(y_start, y_end, mcolor, bold, rainbow, lambda, msg);
        }

        /* Sleep with a floor of 1ms to prevent busy-looping at update=0.
           Without this, Sleep(0) just yields the timeslice and spins at
           100% CPU on one core. */
        {
            DWORD ms = (DWORD)(update * 10);
            Sleep(ms > 0 ? ms : 1);
        }
    }

    finish(0);
    return 0;
}

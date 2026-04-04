/*
 * renderer.c — ANSI streaming markdown renderer
 *
 * Renders streamed markdown tokens to an ANSI terminal with:
 *   - Word wrap at terminal width (TIOCGWINSZ, fallback 80)
 *   - Bold (**...**), italic (*...*), inline code (`...`)
 *   - Fenced code blocks (```lang\n...\n```) with keyword highlighting
 *   - Headers (# / ## / ###) in bold + colour
 *   - Unordered lists (- item, * item) and ordered lists (1. item)
 *   - 16-colour default; 256-colour when TERM contains "256color"
 *
 * Design: char-by-char state machine with a 4-char lookahead pending buffer.
 * Style codes are emitted immediately (not word-buffered) so they are
 * re-applied automatically after a word-wrap newline.
 * The word buffer holds only visible chars for wrap-width accounting.
 */

#include "tui/renderer.h"
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>

/* =========================================================================
 * ANSI escape sequences
 * ====================================================================== */
#define SGR_RESET   "\033[0m"
#define SGR_BOLD    "\033[1m"
#define SGR_ITALIC  "\033[3m"
#define SGR_REV     "\033[7m"   /* reverse video — inline code */
#define SGR_DIM     "\033[2m"   /* dim — separators, labels   */
#define FG_RED      "\033[31m"
#define FG_GREEN    "\033[32m"
#define FG_YELLOW   "\033[33m"
#define FG_BLUE     "\033[34m"
#define FG_CYAN     "\033[36m"
#define FG_MAGENTA  "\033[35m"

/* 256-colour foreground */
#define FG256_KW    "\033[38;5;208m"  /* orange   — keywords    */
#define FG256_STR   "\033[38;5;114m"  /* lt-green — strings     */
#define FG256_NUM   "\033[38;5;117m"  /* lt-blue  — numbers     */
#define FG256_CMT   "\033[38;5;242m"  /* gray     — comments    */
#define FG256_H1    "\033[38;5;214m"  /* bright orange — h1     */
#define FG256_H2    "\033[38;5;220m"  /* yellow        — h2/h3  */

/* =========================================================================
 * Internal constants
 * ====================================================================== */
#define PEND_CAP      8      /* lookahead bytes                  */
#define WORD_CAP      2048   /* visible chars in one word        */
#define LINE_CAP      4096   /* one code-fence line              */
#define LANG_CAP      32     /* fence language tag               */
#define OUT_CAP       8192   /* output write-buffer              */
#define FRAME_CAP     65536  /* frame accumulation buffer (64 KB) */
#define FRAME_NS      16000000LL  /* 16 ms in nanoseconds */
#define DEFAULT_WIDTH 80

/* =========================================================================
 * State machine
 * ====================================================================== */
typedef enum {
    RS_NORMAL      = 0,
    RS_BOLD,         /* inside **...** */
    RS_ITALIC,       /* inside *...* */
    RS_CODE_INLINE,  /* inside `...` */
    RS_FENCE_LANG,   /* collecting lang after ``` */
    RS_CODE_FENCE,   /* inside ``` block */
} RendState;

/* =========================================================================
 * Renderer struct
 * ====================================================================== */
struct Renderer {
    int        fd;
    int        term_width;
    int        col;          /* current output column */
    RendState  state;
    bool       is_256color;
    bool       bol;          /* beginning of line */

    /* Active inline-style flags — re-applied after a wrap newline */
    bool       bold_active;
    bool       italic_active;
    bool       code_active;
    bool       header_active;
    int        header_level;

    /* Small lookahead — never more than PEND_CAP bytes held */
    char       pend[PEND_CAP];
    int        plen;

    /* Current word accumulator (visible chars only, for wrap width) */
    char       word[WORD_CAP];
    int        wlen;

    /* Line buffer used inside code fences */
    char       line[LINE_CAP];
    int        llen;

    /* Fence language tag (NUL-terminated after RS_FENCE_LANG) */
    char       lang[LANG_CAP];
    int        langlen;

    /* Outgoing write buffer */
    char       out[OUT_CAP];
    int        olen;

    /* Write-batching frame state */
    bool       frame_active;        /* inside renderer_frame_begin/end        */
    int64_t    frame_deadline_ns;   /* auto-flush if clock passes this value  */
    char       frame_buf[FRAME_CAP];/* accumulated output for current frame   */
    int        frame_len;           /* bytes in frame_buf                     */
};

/* =========================================================================
 * Low-level I/O
 * ====================================================================== */

static int64_t get_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * Write bytes from `out` to fd — or, when inside a frame, accumulate them
 * into the frame buffer for a single bulk write at renderer_frame_end().
 *
 * If the frame deadline has passed the data is written to fd immediately so
 * the display never stalls longer than 16 ms regardless of how long the
 * caller keeps the frame open.
 */
static void out_flush(Renderer *r)
{
    if (r->olen == 0) return;

    if (r->frame_active) {
        /* Auto-flush if the 16 ms deadline has passed */
        if (r->frame_deadline_ns > 0 &&
            get_monotonic_ns() >= r->frame_deadline_ns) {
            if (r->frame_len > 0) {
                (void)write(r->fd, r->frame_buf, (size_t)r->frame_len);
                r->frame_len = 0;
            }
            (void)write(r->fd, r->out, (size_t)r->olen);
            r->olen              = 0;
            r->frame_active      = false;
            r->frame_deadline_ns = 0;
            return;
        }

        /* Accumulate into frame buffer */
        if (r->frame_len + r->olen <= FRAME_CAP) {
            memcpy(r->frame_buf + r->frame_len, r->out, (size_t)r->olen);
            r->frame_len += r->olen;
        } else {
            /* Frame buffer full — spill to fd to avoid stalling */
            if (r->frame_len > 0) {
                (void)write(r->fd, r->frame_buf, (size_t)r->frame_len);
                r->frame_len = 0;
            }
            (void)write(r->fd, r->out, (size_t)r->olen);
        }
        r->olen = 0;
        return;
    }

    (void)write(r->fd, r->out, (size_t)r->olen);
    r->olen = 0;
}

static void out_raw(Renderer *r, const char *s, int n)
{
    if (n <= 0) return;
    if (r->olen + n > OUT_CAP) out_flush(r);
    if (n > OUT_CAP) { (void)write(r->fd, s, (size_t)n); return; }
    memcpy(r->out + r->olen, s, (size_t)n);
    r->olen += n;
}

static void out_str(Renderer *r, const char *s) { out_raw(r, s, (int)strlen(s)); }

static void out_nl(Renderer *r)
{
    out_raw(r, "\n", 1);
    r->col = 0;
    r->bol = true;
}

/* =========================================================================
 * Style helpers
 * ====================================================================== */

/* Re-emit all currently active style codes (called after a wrap newline). */
static void apply_style(Renderer *r)
{
    out_str(r, SGR_RESET);
    if (r->header_active) {
        out_str(r, SGR_BOLD);
        if (r->header_level == 1)
            out_str(r, r->is_256color ? FG256_H1 : FG_CYAN);
        else
            out_str(r, r->is_256color ? FG256_H2 : FG_YELLOW);
    }
    if (r->bold_active)   out_str(r, SGR_BOLD);
    if (r->italic_active) out_str(r, SGR_ITALIC);
    if (r->code_active)   out_str(r, SGR_REV);
}

/* =========================================================================
 * Word-wrap helpers
 * ====================================================================== */

static void word_push(Renderer *r, char c)
{
    if (r->wlen < WORD_CAP - 1)
        r->word[r->wlen++] = c;
}

/* Emit the accumulated word, wrapping first if it won't fit. */
static void word_flush(Renderer *r)
{
    if (r->wlen == 0) return;
    /* Wrap: only if word fits on a fresh line (avoid infinite loop for huge words) */
    if (r->col > 0 && r->col + r->wlen > r->term_width &&
        r->wlen <= r->term_width) {
        out_str(r, SGR_RESET);
        out_nl(r);
        apply_style(r);
    }
    out_raw(r, r->word, r->wlen);
    r->col += r->wlen;
    r->bol  = false;
    r->wlen = 0;
}

/* =========================================================================
 * Syntax highlighting for fenced code blocks
 * ====================================================================== */

static const char * const kw_c[] = {
    "auto","break","case","char","const","continue","default","do","double",
    "else","enum","extern","float","for","goto","if","inline","int","long",
    "register","restrict","return","short","signed","sizeof","static","struct",
    "switch","typedef","union","unsigned","void","volatile","while",
    "NULL","true","false","bool",
    "uint8_t","int8_t","uint16_t","int16_t","uint32_t","int32_t",
    "uint64_t","int64_t","size_t","ptrdiff_t","uintptr_t",NULL
};

static const char * const kw_py[] = {
    "False","None","True","and","as","assert","async","await","break","class",
    "continue","def","del","elif","else","except","finally","for","from",
    "global","if","import","in","is","lambda","nonlocal","not","or","pass",
    "raise","return","try","while","with","yield",NULL
};

static const char * const kw_js[] = {
    "break","case","catch","class","const","continue","debugger","default",
    "delete","do","else","export","extends","finally","for","function","if",
    "import","in","instanceof","let","new","null","return","static","super",
    "switch","this","throw","true","false","try","typeof","undefined","var",
    "void","while","with","yield","async","await","of","from",NULL
};

static const char * const kw_bash[] = {
    "if","then","else","elif","fi","case","esac","for","select","while",
    "until","do","done","in","function","time","return","export","local",
    "readonly","declare","typeset","unset","source","echo","exit",NULL
};

static const char * const kw_go[] = {
    "break","case","chan","const","continue","default","defer","else",
    "fallthrough","for","func","go","goto","if","import","interface","map",
    "package","range","return","select","struct","switch","type","var",
    "nil","true","false","iota","make","new","len","cap","append","copy",
    "delete","close","panic","recover","print","println","error",
    "int","int8","int16","int32","int64","uint","uint8","uint16","uint32",
    "uint64","uintptr","float32","float64","complex64","complex128",
    "bool","byte","rune","string","any","comparable",NULL
};

static const char * const kw_rust[] = {
    "as","async","await","break","const","continue","crate","dyn","else",
    "enum","extern","false","fn","for","if","impl","in","let","loop",
    "match","mod","move","mut","pub","ref","return","self","Self","static",
    "struct","super","trait","true","type","union","unsafe","use","where",
    "while","i8","i16","i32","i64","i128","isize","u8","u16","u32","u64",
    "u128","usize","f32","f64","bool","char","str","String","Vec","Option",
    "Result","Box","None","Some","Ok","Err",NULL
};

static bool is_kw(const char *s, int n, const char * const *kws)
{
    for (int i = 0; kws[i]; i++) {
        int klen = (int)strlen(kws[i]);
        if (klen == n && memcmp(kws[i], s, (size_t)n) == 0) return true;
    }
    return false;
}

/*
 * Emit a dim language label right-aligned in the first line of a code fence.
 * e.g.:  ──────────────────────────────────── python
 * No-op when lang is empty.
 */
static void emit_fence_header(Renderer *r)
{
    if (r->langlen == 0) return;

    const char *col = r->is_256color ? FG256_CMT : SGR_DIM;
    /* Number of ─ glyphs (each 1 column wide): fill up to (term_width - langlen - 1) */
    int dashes = r->term_width - r->langlen - 1;
    if (dashes < 0) dashes = 0;

    out_str(r, col);
    for (int i = 0; i < dashes; i++)
        out_raw(r, "\xe2\x94\x80", 3); /* U+2500 BOX DRAWINGS LIGHT HORIZONTAL */
    out_raw(r, " ", 1);
    out_raw(r, r->lang, r->langlen);
    out_str(r, SGR_RESET);
    out_nl(r);
}

/*
 * Emit one line from a fenced code block with syntax highlighting.
 * Always appends a newline.
 */
static void emit_code_line(Renderer *r, const char *line, int len,
                           const char *lang)
{
    bool is_c    = strcmp(lang,"c")==0 || strcmp(lang,"cpp")==0 ||
                   strcmp(lang,"c++")==0;
    bool is_py   = strcmp(lang,"python")==0 || strcmp(lang,"py")==0;
    bool is_js   = strcmp(lang,"javascript")==0 || strcmp(lang,"js")==0 ||
                   strcmp(lang,"typescript")==0 || strcmp(lang,"ts")==0;
    bool is_bash = strcmp(lang,"bash")==0 || strcmp(lang,"sh")==0 ||
                   strcmp(lang,"shell")==0 || strcmp(lang,"zsh")==0;
    bool is_json = strcmp(lang,"json")==0;
    bool is_go   = strcmp(lang,"go")==0 || strcmp(lang,"golang")==0;
    bool is_rust = strcmp(lang,"rust")==0 || strcmp(lang,"rs")==0;

    const char * const *kws  = NULL;
    const char *kw_col  = r->is_256color ? FG256_KW  : FG_CYAN;
    const char *str_col = r->is_256color ? FG256_STR : FG_GREEN;
    const char *num_col = r->is_256color ? FG256_NUM : FG_YELLOW;
    const char *cmt_col = r->is_256color ? FG256_CMT : SGR_ITALIC;

    if (is_c)    kws = kw_c;
    if (is_py)   kws = kw_py;
    if (is_js)   kws = kw_js;
    if (is_bash) kws = kw_bash;
    if (is_go)   kws = kw_go;
    if (is_rust) kws = kw_rust;

    int i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)line[i];

        /* ---- JSON ---------------------------------------------------- */
        if (is_json) {
            if (c == '"') {
                out_str(r, str_col);
                out_raw(r, (char *)&c, 1); i++;
                while (i < len) {
                    char ch = line[i++];
                    out_raw(r, &ch, 1);
                    if (ch == '\\' && i < len) { out_raw(r, &line[i], 1); i++; }
                    else if (ch == '"') break;
                }
                out_str(r, SGR_RESET);
            } else if (i+4 <= len && memcmp(line+i,"true",4)==0 &&
                       (i+4>=len || !isalnum((unsigned char)line[i+4]))) {
                out_str(r, FG_GREEN);
                out_raw(r, "true", 4); i += 4;
                out_str(r, SGR_RESET);
            } else if (i+5 <= len && memcmp(line+i,"false",5)==0 &&
                       (i+5>=len || !isalnum((unsigned char)line[i+5]))) {
                out_str(r, FG_RED);
                out_raw(r, "false", 5); i += 5;
                out_str(r, SGR_RESET);
            } else if (i+4 <= len && memcmp(line+i,"null",4)==0 &&
                       (i+4>=len || !isalnum((unsigned char)line[i+4]))) {
                out_str(r, FG_MAGENTA);
                out_raw(r, "null", 4); i += 4;
                out_str(r, SGR_RESET);
            } else if (isdigit(c) || (c=='-' && i+1<len &&
                                      isdigit((unsigned char)line[i+1]))) {
                out_str(r, num_col);
                while (i < len) {
                    unsigned char d = (unsigned char)line[i];
                    if (!isdigit(d) && d!='-' && d!='.' &&
                        d!='e' && d!='E' && d!='+') break;
                    out_raw(r, &line[i], 1); i++;
                }
                out_str(r, SGR_RESET);
            } else {
                out_raw(r, &line[i], 1); i++;
            }
            continue;
        }

        /* ---- Language with keyword list ------------------------------ */
        if (kws) {
            /* C/C++/Go/JS line comment */
            if ((is_c || is_js || is_go) && c=='/' && i+1<len && line[i+1]=='/') {
                out_str(r, cmt_col);
                out_raw(r, line+i, len-i);
                out_str(r, SGR_RESET);
                i = len; continue;
            }
            /* C/Go block comment */
            if ((is_c || is_go) && c=='/' && i+1<len && line[i+1]=='*') {
                out_str(r, cmt_col);
                while (i < len) {
                    if (i+1<len && line[i]=='*' && line[i+1]=='/') {
                        out_raw(r, "*/", 2); i += 2; break;
                    }
                    out_raw(r, &line[i], 1); i++;
                }
                out_str(r, SGR_RESET); continue;
            }
            /* Python/Bash # comment */
            if ((is_py || is_bash) && c=='#') {
                out_str(r, cmt_col);
                out_raw(r, line+i, len-i);
                out_str(r, SGR_RESET);
                i = len; continue;
            }
            /* String literals */
            if (c=='"' || (c=='\'' && !is_bash)) {
                char delim = line[i];
                out_str(r, str_col);
                out_raw(r, &line[i], 1); i++;
                while (i < len && line[i] != delim) {
                    if (line[i]=='\\' && i+1<len) {
                        out_raw(r, line+i, 2); i += 2;
                    } else {
                        out_raw(r, &line[i], 1); i++;
                    }
                }
                if (i < len) { out_raw(r, &line[i], 1); i++; }
                out_str(r, SGR_RESET); continue;
            }
            /* Identifiers / keywords */
            if (isalpha(c) || c=='_') {
                int s = i;
                while (i<len && (isalnum((unsigned char)line[i]) || line[i]=='_'))
                    i++;
                if (is_kw(line+s, i-s, kws)) {
                    out_str(r, kw_col);
                    out_raw(r, line+s, i-s);
                    out_str(r, SGR_RESET);
                } else {
                    out_raw(r, line+s, i-s);
                }
                continue;
            }
            /* Numbers */
            if (isdigit(c)) {
                out_str(r, num_col);
                while (i<len && (isalnum((unsigned char)line[i]) ||
                                 line[i]=='.' || line[i]=='_'))
                    { out_raw(r, &line[i], 1); i++; }
                out_str(r, SGR_RESET); continue;
            }
        }

        /* Default: emit as-is */
        out_raw(r, &line[i], 1); i++;
    }
    out_nl(r);
}

/* =========================================================================
 * Pending-buffer helpers
 * ====================================================================== */
static void pend_consume(Renderer *r, int n)
{
    if (n >= r->plen) { r->plen = 0; return; }
    memmove(r->pend, r->pend + n, (size_t)(r->plen - n));
    r->plen -= n;
}

/* =========================================================================
 * Core: process one "unit" from the pending buffer.
 * Returns number of chars consumed, or 0 if more lookahead is needed.
 * ====================================================================== */
static int process_one(Renderer *r)
{
    if (r->plen == 0) return 0;
    char c = r->pend[0];

    /* ------------------------------------------------------------------
     * RS_CODE_FENCE — buffer lines, syntax-highlight on each newline
     * ---------------------------------------------------------------- */
    if (r->state == RS_CODE_FENCE) {
        /* Closing ``` must be at BOL */
        if (r->bol && c == '`') {
            if (r->plen < 3) return 0;
            if (r->pend[1]=='`' && r->pend[2]=='`') {
                if (r->llen > 0) {
                    emit_code_line(r, r->line, r->llen, r->lang);
                    r->llen = 0;
                }
                out_str(r, SGR_RESET);
                r->state  = RS_NORMAL;
                r->bol    = true;
                r->lang[0]  = '\0';
                r->langlen  = 0;
                /* consume optional trailing newline */
                if (r->plen >= 4 && r->pend[3] == '\n') return 4;
                return 3;
            }
        }
        if (c == '\n') {
            emit_code_line(r, r->line, r->llen, r->lang);
            r->llen = 0;
            r->col  = 0;
            r->bol  = true;
            return 1;
        }
        if (c != '\r' && r->llen < LINE_CAP - 1) r->line[r->llen++] = c;
        r->bol = false;
        return 1;
    }

    /* ------------------------------------------------------------------
     * RS_FENCE_LANG — collect language tag until newline
     * ---------------------------------------------------------------- */
    if (r->state == RS_FENCE_LANG) {
        if (c == '\n') {
            r->lang[r->langlen] = '\0';
            for (int j = 0; j < r->langlen; j++)
                r->lang[j] = (char)tolower((unsigned char)r->lang[j]);
            r->state = RS_CODE_FENCE;
            r->llen  = 0;
            r->col   = 0;
            r->bol   = true;
            emit_fence_header(r);  /* right-aligned dim language label */
            return 1;
        }
        if (c != '\r' && r->langlen < LANG_CAP - 1)
            r->lang[r->langlen++] = c;
        return 1;
    }

    /* ------------------------------------------------------------------
     * RS_CODE_INLINE — everything literal until closing `
     * ---------------------------------------------------------------- */
    if (r->state == RS_CODE_INLINE) {
        if (c == '`') {
            word_flush(r);
            r->code_active = false;
            apply_style(r);
            r->state = RS_NORMAL;
            return 1;
        }
        if (c == '\n') {
            word_flush(r);
            out_str(r, SGR_RESET);
            out_nl(r);
            apply_style(r);
            return 1;
        }
        word_push(r, c);
        return 1;
    }

    /* ------------------------------------------------------------------
     * RS_BOLD — text + watch for closing **
     * ---------------------------------------------------------------- */
    if (r->state == RS_BOLD) {
        if (c == '*') {
            if (r->plen < 2) return 0;
            if (r->pend[1] == '*') {
                word_flush(r);
                r->bold_active = false;
                apply_style(r);
                r->state = RS_NORMAL;
                return 2;
            }
        }
        if (c == '\n') {
            word_flush(r);
            out_str(r, SGR_RESET);
            out_nl(r);
            apply_style(r);
            return 1;
        }
        if (c == ' ' || c == '\t') { word_flush(r); out_raw(r, &c, 1); r->col++; return 1; }
        word_push(r, c);
        return 1;
    }

    /* ------------------------------------------------------------------
     * RS_ITALIC — text + watch for closing *
     * ---------------------------------------------------------------- */
    if (r->state == RS_ITALIC) {
        if (c == '*') {
            if (r->plen < 2) return 0;
            if (r->pend[1] != '*') {
                word_flush(r);
                r->italic_active = false;
                apply_style(r);
                r->state = RS_NORMAL;
                return 1;
            }
            /* ** inside italic — literal */
            word_push(r, '*'); word_push(r, '*');
            return 2;
        }
        if (c == '\n') {
            word_flush(r);
            out_str(r, SGR_RESET);
            out_nl(r);
            apply_style(r);
            return 1;
        }
        if (c == ' ' || c == '\t') { word_flush(r); out_raw(r, &c, 1); r->col++; return 1; }
        word_push(r, c);
        return 1;
    }

    /* ------------------------------------------------------------------
     * RS_NORMAL
     * ---------------------------------------------------------------- */

    /* BOL: fenced code block ``` */
    if (r->bol && c == '`') {
        if (r->plen < 3) return 0;
        if (r->pend[1]=='`' && r->pend[2]=='`') {
            word_flush(r);
            r->state   = RS_FENCE_LANG;
            r->langlen = 0;
            r->lang[0] = '\0';
            return 3;
        }
        /* fewer than 3 backticks — fall through to inline ` handling */
    }

    /* BOL: ATX header (# / ## / ###) */
    if (r->bol && c == '#') {
        int hc = 0;
        while (hc < r->plen && r->pend[hc] == '#') hc++;
        if (hc == r->plen) return 0; /* need more */
        if (r->pend[hc] == ' ') {
            word_flush(r);
            r->header_active = true;
            r->header_level  = hc;
            apply_style(r);
            r->col += hc + 1;
            r->bol  = false;
            return hc + 1;
        }
        /* Not a header — output first # literally */
        word_push(r, c);
        return 1;
    }

    /* BOL: unordered list (- item  or  * item) */
    if (r->bol && c == '-' && r->plen >= 2 && r->pend[1] == ' ') {
        word_flush(r);
        out_str(r, "  \xe2\x80\xa2 "); /* "  • " */
        r->col += 4;
        r->bol  = false;
        return 2;
    }
    if (r->bol && c == '*' && r->plen >= 2 && r->pend[1] == ' ') {
        word_flush(r);
        out_str(r, "  \xe2\x80\xa2 ");
        r->col += 4;
        r->bol  = false;
        return 2;
    }

    /* BOL: ordered list (single digit N followed by ". ") */
    if (r->bol && isdigit((unsigned char)c) && r->plen >= 3 &&
        r->pend[1] == '.' && r->pend[2] == ' ') {
        word_flush(r);
        out_str(r, "  ");
        out_raw(r, r->pend, 3); /* "N. " */
        r->col += 5;
        r->bol  = false;
        return 3;
    }

    /* Inline: bold ** */
    if (c == '*') {
        if (r->plen < 2) return 0;
        if (r->pend[1] == '*') {
            if (r->plen < 3) return 0; /* distinguish ** from *** */
            word_flush(r);
            r->bold_active = true;
            apply_style(r);
            r->state = RS_BOLD;
            return 2;
        }
        /* Single * — italic */
        word_flush(r);
        r->italic_active = true;
        apply_style(r);
        r->state = RS_ITALIC;
        return 1;
    }

    /* Inline: code ` */
    if (c == '`') {
        if (r->plen < 2) return 0;
        if (r->pend[1] == '`') {
            if (r->plen < 3) return 0;
            if (r->pend[2] != '`') {
                /* `` — literal pair */
                word_push(r, '`'); word_push(r, '`');
                return 2;
            }
            /* ``` mid-line (unusual) — treat as inline code */
            word_flush(r);
            r->code_active = true;
            apply_style(r);
            r->state = RS_CODE_INLINE;
            return 3;
        }
        /* Single ` */
        word_flush(r);
        r->code_active = true;
        apply_style(r);
        r->state = RS_CODE_INLINE;
        return 1;
    }

    /* Newline */
    if (c == '\n') {
        word_flush(r);
        if (r->header_active) {
            out_str(r, SGR_RESET);
            r->header_active = false;
        }
        out_nl(r);
        return 1;
    }

    /* Carriage return — skip */
    if (c == '\r') return 1;

    /* Space / tab */
    if (c == ' ' || c == '\t') {
        word_flush(r);
        out_raw(r, &c, 1);
        r->col += (c == '\t') ? 4 : 1;
        return 1;
    }

    /* Regular visible char */
    word_push(r, c);
    return 1;
}

static void drain_pending(Renderer *r)
{
    while (r->plen > 0) {
        int n = process_one(r);
        if (n == 0) break;
        pend_consume(r, n);
    }
}

/* =========================================================================
 * Public API
 * ====================================================================== */

Renderer *renderer_new(int fd, Arena *arena)
{
    Renderer *r = arena_alloc(arena, sizeof(*r));
    memset(r, 0, sizeof(*r));
    r->fd         = fd;
    r->bol        = true;
    r->term_width = DEFAULT_WIDTH;

    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        r->term_width = ws.ws_col;

    const char *term = getenv("TERM");
    if (term && strstr(term, "256color"))
        r->is_256color = true;
    const char *cterm = getenv("COLORTERM");
    if (cterm && (strcmp(cterm, "truecolor") == 0 ||
                  strcmp(cterm, "24bit")      == 0))
        r->is_256color = true;

    return r;
}

void renderer_set_width(Renderer *r, int width)
{
    r->term_width = width;
}

void renderer_update_width(Renderer *r)
{
    struct winsize ws;
    if (ioctl(r->fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        r->term_width = ws.ws_col;
}

void renderer_token(Renderer *r, const char *tok, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (r->plen >= PEND_CAP) {
            drain_pending(r);
            /* Pathological: still full — force-consume as literal */
            if (r->plen >= PEND_CAP) {
                word_push(r, r->pend[0]);
                r->col++;
                pend_consume(r, 1);
            }
        }
        r->pend[r->plen++] = tok[i];
    }
    drain_pending(r);
}

void renderer_flush(Renderer *r)
{
    /* Force any held pending chars as literal text */
    for (int i = 0; i < r->plen; i++)
        word_push(r, r->pend[i]);
    r->plen = 0;

    /* Flush a partial code-fence line */
    if (r->state == RS_CODE_FENCE && r->llen > 0) {
        emit_code_line(r, r->line, r->llen, r->lang);
        r->llen = 0;
    }

    word_flush(r);
    out_str(r, SGR_RESET);
    if (r->col > 0) out_nl(r);
    out_flush(r);

    r->state         = RS_NORMAL;
    r->bold_active   = false;
    r->italic_active = false;
    r->code_active   = false;
    r->header_active = false;
    r->bol           = true;
    r->col           = 0;
    r->wlen          = 0;
}

void renderer_free(Renderer *r)
{
    out_flush(r);
    (void)r; /* arena-allocated; no individual free */
}

/*
 * Begin a write-batching frame.  All writes issued via renderer_token() are
 * accumulated in an internal 64 KB frame buffer instead of being written to
 * the file descriptor directly.  The frame is automatically closed (and the
 * buffer flushed to fd) if the 16 ms deadline expires before
 * renderer_frame_end() is called, so the display never stalls.
 *
 * Calling renderer_frame_begin() while a frame is already active is a no-op
 * (the deadline is not reset).
 */
void renderer_frame_begin(Renderer *r)
{
    if (r->frame_active) return;
    r->frame_active      = true;
    r->frame_len         = 0;
    r->frame_deadline_ns = get_monotonic_ns() + FRAME_NS;
}

/*
 * End the current write-batching frame and flush all accumulated output to fd
 * in a single write syscall.  This is the "incremental line update" commit
 * point: callers accumulate an entire rendered frame then issue one bulk
 * write, minimising the number of partial-line updates visible on screen.
 *
 * Safe to call even when no frame is active (no-op).
 */
void renderer_frame_end(Renderer *r)
{
    if (!r->frame_active) return;

    /* Drain the small out buffer into the frame buffer first */
    if (r->olen > 0) {
        if (r->frame_len + r->olen <= FRAME_CAP) {
            memcpy(r->frame_buf + r->frame_len, r->out, (size_t)r->olen);
            r->frame_len += r->olen;
        } else {
            if (r->frame_len > 0) {
                (void)write(r->fd, r->frame_buf, (size_t)r->frame_len);
                r->frame_len = 0;
            }
            (void)write(r->fd, r->out, (size_t)r->olen);
        }
        r->olen = 0;
    }

    /* Single bulk write for the entire frame */
    if (r->frame_len > 0) {
        (void)write(r->fd, r->frame_buf, (size_t)r->frame_len);
        r->frame_len = 0;
    }

    r->frame_active      = false;
    r->frame_deadline_ns = 0;
}

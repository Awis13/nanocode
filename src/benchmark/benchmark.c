/*
 * benchmark.c — auto-benchmark system (CMP-404)
 *
 * Runs a 20-case test suite against the configured model, scores results,
 * and stores them as JSON.  Optionally compares stored results or auto-tunes
 * temperature based on scores.
 */

#include "../../include/benchmark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>     /* strcasecmp */
#include <ctype.h>

#ifndef BENCHMARK_TEST
#include "../../include/config.h"
#include "../../src/api/provider.h"
#include "../../src/api/message.h"
#include "../../src/core/loop.h"
#include "../../src/util/buf.h"
#endif
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

/* -------------------------------------------------------------------------
 * Pass threshold — a test needs score >= this to count as passed.
 * ---------------------------------------------------------------------- */

#define BENCH_PASS_THRESHOLD 0.5

/* -------------------------------------------------------------------------
 * Built-in test cases (20 total, 5 per category)
 * ---------------------------------------------------------------------- */

static const BenchCase s_builtin[] = {
    /* ---- tool_calling (5) ---- */
    {
        "tc_json_read",
        BENCH_CAT_TOOL_CALLING,
        "Respond with only the following JSON, no other text: "
        "{\"tool\": \"read_file\", \"path\": \"main.c\"}",
        "read_file",
        BENCH_SCORE_FUZZY_MATCH
    },
    {
        "tc_json_bash",
        BENCH_CAT_TOOL_CALLING,
        "Output only this exact JSON: "
        "{\"tool\": \"bash\", \"command\": \"ls -la\"}",
        "bash",
        BENCH_SCORE_FUZZY_MATCH
    },
    {
        "tc_json_write",
        BENCH_CAT_TOOL_CALLING,
        "Respond only with: {\"tool\": \"write_file\", \"path\": \"out.txt\"}",
        "write_file",
        BENCH_SCORE_FUZZY_MATCH
    },
    {
        "tc_tool_name_grep",
        BENCH_CAT_TOOL_CALLING,
        "What is the standard tool name for searching file contents using "
        "regular expressions? Respond with only the tool name, lowercase.",
        "grep",
        BENCH_SCORE_FUZZY_MATCH
    },
    {
        "tc_tool_name_edit",
        BENCH_CAT_TOOL_CALLING,
        "What tool name is used to modify an existing file by replacing text? "
        "Respond with only one word: edit",
        "edit",
        BENCH_SCORE_FUZZY_MATCH
    },

    /* ---- code_gen (5) ---- */
    {
        "cg_sum",
        BENCH_CAT_CODE_GEN,
        "Write a C function that takes two int parameters and returns their "
        "sum. Include only the function definition, no includes, no main.",
        "int",
        BENCH_SCORE_COMPILATION_CHECK
    },
    {
        "cg_is_prime",
        BENCH_CAT_CODE_GEN,
        "Write a C function 'int is_prime(int n)' that returns 1 if n is "
        "prime and 0 otherwise. Include only the function, no main, no includes.",
        "is_prime",
        BENCH_SCORE_COMPILATION_CHECK
    },
    {
        "cg_factorial",
        BENCH_CAT_CODE_GEN,
        "Write a recursive C function 'int factorial(int n)' that returns "
        "n factorial. Include only the function definition.",
        "factorial",
        BENCH_SCORE_COMPILATION_CHECK
    },
    {
        "cg_str_len",
        BENCH_CAT_CODE_GEN,
        "Write a C function 'int my_strlen(const char *s)' that counts "
        "string length without using strlen. Include only the function.",
        "my_strlen",
        BENCH_SCORE_COMPILATION_CHECK
    },
    {
        "cg_max",
        BENCH_CAT_CODE_GEN,
        "Write a C function 'int max_of_three(int a, int b, int c)' that "
        "returns the largest of three integers. Include only the function.",
        "max_of_three",
        BENCH_SCORE_COMPILATION_CHECK
    },

    /* ---- instruction_following (5) ---- */
    {
        "if_number_42",
        BENCH_CAT_INSTRUCTION_FOLLOWING,
        "Respond with exactly the number 42, nothing else. No explanation, "
        "no punctuation, just the digits.",
        "42",
        BENCH_SCORE_EXACT_MATCH
    },
    {
        "if_word_banana",
        BENCH_CAT_INSTRUCTION_FOLLOWING,
        "Respond with only the single lowercase word: banana. "
        "No punctuation, no newlines after it.",
        "banana",
        BENCH_SCORE_EXACT_MATCH
    },
    {
        "if_math_56",
        BENCH_CAT_INSTRUCTION_FOLLOWING,
        "What is 7 multiplied by 8? Respond with only the numeric answer.",
        "56",
        BENCH_SCORE_EXACT_MATCH
    },
    {
        "if_count_csv",
        BENCH_CAT_INSTRUCTION_FOLLOWING,
        "Count from 1 to 5, separated by commas, no spaces. "
        "Output only the sequence.",
        "1,2,3,4,5",
        BENCH_SCORE_EXACT_MATCH
    },
    {
        "if_uppercase",
        BENCH_CAT_INSTRUCTION_FOLLOWING,
        "Convert 'hello' to uppercase. Output only the result, nothing else.",
        "HELLO",
        BENCH_SCORE_EXACT_MATCH
    },

    /* ---- context_utilization (5) ---- */
    {
        "cu_secret_code",
        BENCH_CAT_CONTEXT_UTILIZATION,
        "The secret code is ALPHA-7. What is the secret code? "
        "Reply with only the code, exactly as given.",
        "ALPHA-7",
        BENCH_SCORE_EXACT_MATCH
    },
    {
        "cu_project_name",
        BENCH_CAT_CONTEXT_UTILIZATION,
        "Remember: the project is called NANOCODE. "
        "What is the project name? Reply with only the name.",
        "NANOCODE",
        BENCH_SCORE_FUZZY_MATCH
    },
    {
        "cu_number_99",
        BENCH_CAT_CONTEXT_UTILIZATION,
        "The answer is 99. Repeat the answer. Reply with only the number.",
        "99",
        BENCH_SCORE_EXACT_MATCH
    },
    {
        "cu_color",
        BENCH_CAT_CONTEXT_UTILIZATION,
        "The color is cerulean. What color was mentioned? "
        "Reply with only the color name.",
        "cerulean",
        BENCH_SCORE_FUZZY_MATCH
    },
    {
        "cu_theme",
        BENCH_CAT_CONTEXT_UTILIZATION,
        "User preference: theme=dark. What theme is set? "
        "Reply with only the theme name.",
        "dark",
        BENCH_SCORE_FUZZY_MATCH
    },
};

#define BUILTIN_COUNT ((int)(sizeof(s_builtin) / sizeof(s_builtin[0])))

/* -------------------------------------------------------------------------
 * Category name helper
 * ---------------------------------------------------------------------- */

const char *benchmark_category_name(BenchCategory cat)
{
    switch (cat) {
    case BENCH_CAT_TOOL_CALLING:         return "tool_calling";
    case BENCH_CAT_CODE_GEN:             return "code_gen";
    case BENCH_CAT_INSTRUCTION_FOLLOWING: return "instruction_following";
    case BENCH_CAT_CONTEXT_UTILIZATION:  return "context_utilization";
    default:                             return "unknown";
    }
}

/* -------------------------------------------------------------------------
 * benchmark_builtin_count / benchmark_builtin_cases
 * ---------------------------------------------------------------------- */

int benchmark_builtin_count(void)
{
    return BUILTIN_COUNT;
}

int benchmark_builtin_cases(BenchCase *out, int cap)
{
    int n = BUILTIN_COUNT < cap ? BUILTIN_COUNT : cap;
    memcpy(out, s_builtin, (size_t)n * sizeof(BenchCase));
    return n;
}

/* -------------------------------------------------------------------------
 * Scoring helpers
 * ---------------------------------------------------------------------- */

/* Trim leading/trailing whitespace from s into buf (in-place trim view). */
static const char *trim(const char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

/*
 * strip_and_compare — compare response (after whitespace trim) to expected.
 * Returns 1 on match.
 */
static int exact_match(const char *response, const char *expected)
{
    const char *r = trim(response);
    /* Strip trailing whitespace from r for comparison. */
    char buf[BENCH_EXPECTED_MAX * 2];
    size_t rlen = strlen(r);
    if (rlen >= sizeof(buf))
        rlen = sizeof(buf) - 1;
    memcpy(buf, r, rlen);
    buf[rlen] = '\0';
    /* Trim trailing whitespace. */
    while (rlen > 0 && isspace((unsigned char)buf[rlen - 1]))
        buf[--rlen] = '\0';
    return strcmp(buf, expected) == 0;
}

static int fuzzy_match(const char *response, const char *expected)
{
    /* Case-insensitive substring search. */
    size_t elen = strlen(expected);
    size_t rlen = strlen(response);
    if (elen == 0)
        return 1;
    if (rlen < elen)
        return 0;
    for (size_t i = 0; i <= rlen - elen; i++) {
        if (strncasecmp(response + i, expected, elen) == 0)
            return 1;
    }
    return 0;
}

/*
 * compilation_check — write response's C code block to a temp file and
 * try to compile it with cc.  Returns 1 if compilation succeeds.
 *
 * Extracts text between ```c ... ``` markers if present; otherwise uses
 * the full response as the source.  Wraps the snippet in a minimal
 * translation unit if it doesn't start with '#' or 'int'/'void'/'static'.
 */
static int compilation_check(const char *response)
{
    /* Extract code block from markdown if present. */
    const char *src_start = response;
    const char *marker = strstr(response, "```c");
    if (!marker)
        marker = strstr(response, "```C");
    if (marker) {
        src_start = marker + 4;
        /* Skip optional newline. */
        if (*src_start == '\n')
            src_start++;
    }
    const char *src_end = NULL;
    if (marker) {
        src_end = strstr(src_start, "```");
        if (!src_end)
            src_end = src_start + strlen(src_start);
    } else {
        src_end = src_start + strlen(src_start);
    }

    size_t src_len = (size_t)(src_end - src_start);
    if (src_len == 0)
        return 0;
    if (src_len > BENCH_RESPONSE_MAX)
        src_len = BENCH_RESPONSE_MAX;

    /* Write to a temp file. */
    char tmppath[] = "/tmp/nanocode_bench_XXXXXX.c";
    /* mkstemp requires XXXXXX suffix — use a manual temp name instead. */
    snprintf(tmppath, sizeof(tmppath), "/tmp/nanocode_bench_%d.c", (int)getpid());

    FILE *f = fopen(tmppath, "w");
    if (!f)
        return 0;

    /* Provide a minimal wrapper if the snippet looks like just a function body. */
    fprintf(f, "#include <stddef.h>\n#include <string.h>\n");
    fwrite(src_start, 1, src_len, f);
    fputc('\n', f);
    fclose(f);

    /* Compile to /dev/null; suppress output. */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "cc -std=c11 -fsyntax-only -Wall -o /dev/null %s 2>/dev/null",
             tmppath);
    int rc = system(cmd);
    unlink(tmppath);
    return (rc == 0) ? 1 : 0;
}

int benchmark_score(const BenchCase *tc, const char *response,
                    double *score_out)
{
    int passed = 0;

    switch (tc->scoring) {
    case BENCH_SCORE_EXACT_MATCH:
        passed = exact_match(response, tc->expected);
        break;
    case BENCH_SCORE_FUZZY_MATCH:
        passed = fuzzy_match(response, tc->expected);
        break;
    case BENCH_SCORE_COMPILATION_CHECK:
        /* For compilation_check, expected is used as a secondary fuzzy check
         * on the response so we can also require the function name appears. */
        passed = compilation_check(response);
        if (passed && tc->expected[0])
            passed = fuzzy_match(response, tc->expected);
        break;
    }

    *score_out = passed ? 1.0 : 0.0;
    return passed;
}

/* -------------------------------------------------------------------------
 * Runtime — provider streaming, result storage, compare, tune.
 * Excluded when BENCHMARK_TEST is defined so unit tests link without
 * pulling in loop/provider/bearssl/config dependencies.
 * ---------------------------------------------------------------------- */

#ifndef BENCHMARK_TEST

/* ---- Stream state for collecting a full response ---- */

typedef struct {
    Loop       *loop;
    Buf        *response_buf;
    int         done;
    int         error;
} BenchStreamCtx;

static void bench_on_token(const char *token, size_t len, void *ctx)
{
    BenchStreamCtx *s = ctx;
    buf_append(s->response_buf, token, len);
}

static void bench_on_done(int error, const char *stop_reason, void *ctx)
{
    (void)stop_reason;
    BenchStreamCtx *s = ctx;
    s->error = error;
    s->done  = 1;
    loop_stop(s->loop);
}

/* -------------------------------------------------------------------------
 * Path helpers
 * ---------------------------------------------------------------------- */

static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return;
    /* Try to create intermediate dirs manually for the two expected paths. */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void get_results_dir(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0])
        snprintf(out, cap, "%s/nanocode/benchmark-results", xdg);
    else {
        const char *home = getenv("HOME");
        snprintf(out, cap, "%s/.local/share/nanocode/benchmark-results",
                 home ? home : ".");
    }
}

static void get_benchmarks_dir(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        snprintf(out, cap, "%s/nanocode/benchmarks", xdg);
    else {
        const char *home = getenv("HOME");
        snprintf(out, cap, "%s/.config/nanocode/benchmarks",
                 home ? home : ".");
    }
}

/* -------------------------------------------------------------------------
 * TOML user test case loader
 *
 * Format (one [test] block per file, or multiple in one file):
 *
 *   [test]
 *   name     = "my_test"
 *   category = "tool_calling"     # or code_gen / instruction_following / context_utilization
 *   prompt   = "..."
 *   expected = "..."
 *   scoring  = "fuzzy_match"      # or exact_match / compilation_check
 * ---------------------------------------------------------------------- */

static BenchCategory parse_category(const char *s)
{
    if (strcmp(s, "tool_calling") == 0)         return BENCH_CAT_TOOL_CALLING;
    if (strcmp(s, "code_gen") == 0)             return BENCH_CAT_CODE_GEN;
    if (strcmp(s, "instruction_following") == 0) return BENCH_CAT_INSTRUCTION_FOLLOWING;
    if (strcmp(s, "context_utilization") == 0)  return BENCH_CAT_CONTEXT_UTILIZATION;
    return BENCH_CAT_TOOL_CALLING;
}

static BenchScoring parse_scoring(const char *s)
{
    if (strcmp(s, "exact_match") == 0)       return BENCH_SCORE_EXACT_MATCH;
    if (strcmp(s, "fuzzy_match") == 0)       return BENCH_SCORE_FUZZY_MATCH;
    if (strcmp(s, "compilation_check") == 0) return BENCH_SCORE_COMPILATION_CHECK;
    return BENCH_SCORE_FUZZY_MATCH;
}

/* Strip surrounding quotes from a TOML string value. */
static void strip_quotes(char *s)
{
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

/*
 * Load user test cases from a TOML file.
 * Appends to `cases[*count]`; stops when count reaches cap.
 * Returns number of cases added.
 */
static int load_toml_file(const char *path, BenchCase *cases, int *count, int cap)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    int added = 0;
    int in_test = 0;
    BenchCase cur;
    memset(&cur, 0, sizeof(cur));

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline. */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip comments and blank lines. */
        const char *p = trim(line);
        if (!p[0] || p[0] == '#')
            continue;

        if (strcmp(p, "[test]") == 0) {
            /* Save previous if complete. */
            if (in_test && cur.name[0] && cur.prompt[0] && *count < cap) {
                cases[(*count)++] = cur;
                added++;
            }
            memset(&cur, 0, sizeof(cur));
            in_test = 1;
            continue;
        }

        if (!in_test)
            continue;

        /* Parse key = value. */
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        /* Trim key and val. */
        while (*key && isspace((unsigned char)*key)) key++;
        char *ke = key + strlen(key) - 1;
        while (ke > key && isspace((unsigned char)*ke)) *ke-- = '\0';
        while (*val && isspace((unsigned char)*val)) val++;
        char *ve = val + strlen(val) - 1;
        while (ve > val && isspace((unsigned char)*ve)) *ve-- = '\0';

        /* Strip trailing comment from val. */
        char *hash = strchr(val, '#');
        if (hash && hash > val) {
            *hash = '\0';
            ve = val + strlen(val) - 1;
            while (ve > val && isspace((unsigned char)*ve)) *ve-- = '\0';
        }

        strip_quotes(val);

        if (strcmp(key, "name") == 0)
            snprintf(cur.name, sizeof(cur.name), "%s", val);
        else if (strcmp(key, "category") == 0)
            cur.category = parse_category(val);
        else if (strcmp(key, "prompt") == 0)
            snprintf(cur.prompt, sizeof(cur.prompt), "%s", val);
        else if (strcmp(key, "expected") == 0 ||
                 strcmp(key, "expected_tool") == 0)
            snprintf(cur.expected, sizeof(cur.expected), "%s", val);
        else if (strcmp(key, "scoring") == 0)
            cur.scoring = parse_scoring(val);
    }

    /* Save last test. */
    if (in_test && cur.name[0] && cur.prompt[0] && *count < cap) {
        cases[(*count)++] = cur;
        added++;
    }

    fclose(f);
    return added;
}

/*
 * Load all user-defined test cases from the benchmarks config dir.
 * Returns total count added.
 */
static int load_user_cases(BenchCase *cases, int *count, int cap)
{
    char dir[512];
    get_benchmarks_dir(dir, sizeof(dir));

    DIR *d = opendir(dir);
    if (!d)
        return 0;

    int added = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t nlen = strlen(name);
        if (nlen < 6 || strcmp(name + nlen - 5, ".toml") != 0)
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        added += load_toml_file(path, cases, count, cap);
    }
    closedir(d);
    return added;
}

/* -------------------------------------------------------------------------
 * JSON result writer
 * ---------------------------------------------------------------------- */

/*
 * Write a JSON-escaped string into buf at pos; returns updated pos.
 * Simple version without external helpers.
 */
static size_t write_json_str(char *buf, size_t cap, size_t pos, const char *s)
{
    if (pos < cap) buf[pos] = '"'; pos++;
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            if (pos < cap) buf[pos] = '\\'; pos++;
            if (pos < cap) buf[pos] = (char)c; pos++;
        } else if (c == '\n') {
            if (pos < cap) buf[pos] = '\\'; pos++;
            if (pos < cap) buf[pos] = 'n'; pos++;
        } else if (c == '\r') {
            if (pos < cap) buf[pos] = '\\'; pos++;
            if (pos < cap) buf[pos] = 'r'; pos++;
        } else if (c == '\t') {
            if (pos < cap) buf[pos] = '\\'; pos++;
            if (pos < cap) buf[pos] = 't'; pos++;
        } else if (c < 0x20) {
            /* Skip other control characters. */
        } else {
            if (pos < cap) buf[pos] = (char)c; pos++;
        }
    }
    if (pos < cap) buf[pos] = '"'; pos++;
    return pos;
}

static int save_report_json(const BenchReport *report)
{
    char dir[512];
    get_results_dir(dir, sizeof(dir));
    ensure_dir(dir);

    /* Sanitize model name for filename (replace / and spaces). */
    char safe_model[BENCH_MODEL_MAX];
    snprintf(safe_model, sizeof(safe_model), "%s", report->model);
    for (char *p = safe_model; *p; p++) {
        if (*p == '/' || *p == ' ' || *p == ':')
            *p = '_';
    }

    char path[768];
    snprintf(path, sizeof(path), "%s/%s-%s.json",
             dir, safe_model, report->timestamp);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "nanocode: benchmark: cannot write results to %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    /* Build JSON. */
    char buf[65536];
    size_t cap = sizeof(buf);
    size_t pos = 0;

#define WS(s)  do { size_t _l = strlen(s); if (pos + _l < cap) { memcpy(buf + pos, s, _l); pos += _l; } } while(0)
#define WC(c)  do { if (pos < cap) buf[pos++] = (c); } while(0)
#define WJSTR(s) do { pos = write_json_str(buf, cap, pos, s); } while(0)

    WS("{");
    WS("\"model\":");   WJSTR(report->model);   WC(',');
    WS("\"timestamp\":"); WJSTR(report->timestamp); WC(',');
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%.4f", report->overall_score);
    WS("\"overall_score\":"); WS(tmp); WC(',');
    snprintf(tmp, sizeof(tmp), "%d", report->n_passed);
    WS("\"n_passed\":"); WS(tmp); WC(',');
    snprintf(tmp, sizeof(tmp), "%d", report->n_failed);
    WS("\"n_failed\":"); WS(tmp); WC(',');
    WS("\"results\":[");

    for (int i = 0; i < report->n_results; i++) {
        const BenchResult *r = &report->results[i];
        if (i > 0) WC(',');
        WS("{");
        WS("\"name\":"); WJSTR(r->name); WC(',');
        WS("\"category\":"); WJSTR(benchmark_category_name(r->category)); WC(',');
        snprintf(tmp, sizeof(tmp), "%.4f", r->score);
        WS("\"score\":"); WS(tmp); WC(',');
        WS("\"passed\":"); WS(r->passed ? "true" : "false"); WC(',');
        WS("\"response\":"); WJSTR(r->response);
        if (r->error[0]) {
            WC(','); WS("\"error\":"); WJSTR(r->error);
        }
        WS("}");
    }

    WS("]}");

    /* NUL-terminate (but don't count it). */
    if (pos < cap)
        buf[pos] = '\0';
    else
        buf[cap - 1] = '\0';

    fwrite(buf, 1, pos, f);
    fputc('\n', f);
    fclose(f);

    printf("nanocode: benchmark results saved to %s\n", path);
    return 0;

#undef WS
#undef WC
#undef WJSTR
}

/* -------------------------------------------------------------------------
 * Report printer
 * ---------------------------------------------------------------------- */

void benchmark_print_report(const BenchReport *report)
{
    printf("\n=== Benchmark Report: %s (%s) ===\n\n",
           report->model, report->timestamp);

    /* Category totals. */
    int cat_passed[BENCH_CAT_COUNT] = {0};
    int cat_total[BENCH_CAT_COUNT]  = {0};

    for (int i = 0; i < report->n_results; i++) {
        const BenchResult *r = &report->results[i];
        int cat = (int)r->category;
        if (cat >= 0 && cat < BENCH_CAT_COUNT) {
            cat_total[cat]++;
            if (r->passed)
                cat_passed[cat]++;
        }
    }

    printf("%-30s %6s %6s  %s\n", "Category", "Passed", "Total", "Score");
    printf("%-30s %6s %6s  %s\n",
           "------------------------------", "------", "-----", "-----");
    for (int c = 0; c < BENCH_CAT_COUNT; c++) {
        if (cat_total[c] == 0)
            continue;
        double sc = (double)cat_passed[c] / (double)cat_total[c];
        printf("%-30s %6d %6d  %.0f%%\n",
               benchmark_category_name((BenchCategory)c),
               cat_passed[c], cat_total[c], sc * 100.0);
    }

    printf("\n");
    printf("%-30s %6s %6s  %s\n", "Test", "Pass", "Score", "");
    printf("%-30s %6s %6s\n",
           "------------------------------", "------", "-----");

    for (int i = 0; i < report->n_results; i++) {
        const BenchResult *r = &report->results[i];
        printf("%-30s %6s %.0f%%",
               r->name, r->passed ? "PASS" : "FAIL", r->score * 100.0);
        if (r->error[0])
            printf("  [%s]", r->error);
        printf("\n");
    }

    printf("\nOverall: %d/%d passed (%.0f%%)\n",
           report->n_passed, report->n_results,
           report->overall_score * 100.0);
}

/* -------------------------------------------------------------------------
 * --compare: read and display stored results
 * ---------------------------------------------------------------------- */

static void do_compare(void)
{
    char dir[512];
    get_results_dir(dir, sizeof(dir));

    DIR *d = opendir(dir);
    if (!d) {
        printf("nanocode: no benchmark results found in %s\n", dir);
        return;
    }

    printf("\n=== Benchmark Comparison ===\n\n");
    printf("%-35s %-28s %6s %6s  %s\n",
           "Model", "Timestamp", "Passed", "Total", "Score");
    printf("%-35s %-28s %6s %6s  %s\n",
           "-----------------------------------",
           "----------------------------",
           "------", "-----", "-----");

    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t nlen = strlen(name);
        if (nlen < 6 || strcmp(name + nlen - 5, ".json") != 0)
            continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;

        /* Read up to 64 KB. */
        char buf[65536];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (n == 0)
            continue;
        buf[n] = '\0';

        /* Extract fields with simple string search (no full JSON parser needed). */
        char model[BENCH_MODEL_MAX]   = "(unknown)";
        char ts[BENCH_TIME_MAX]       = "";
        char n_passed_s[16]           = "0";
        char n_failed_s[16]           = "0";
        char score_s[16]              = "0";

        /* Find "model":"..." */
        const char *p = strstr(buf, "\"model\":\"");
        if (p) {
            p += 9;
            const char *e = strchr(p, '"');
            if (e) {
                size_t l = (size_t)(e - p);
                if (l >= sizeof(model)) l = sizeof(model) - 1;
                memcpy(model, p, l);
                model[l] = '\0';
            }
        }

        p = strstr(buf, "\"timestamp\":\"");
        if (p) {
            p += 13;
            const char *e = strchr(p, '"');
            if (e) {
                size_t l = (size_t)(e - p);
                if (l >= sizeof(ts)) l = sizeof(ts) - 1;
                memcpy(ts, p, l);
                ts[l] = '\0';
            }
        }

        p = strstr(buf, "\"n_passed\":");
        if (p) {
            p += 11;
            size_t l = 0;
            while (p[l] && (isdigit((unsigned char)p[l]) || p[l] == '-'))
                l++;
            if (l >= sizeof(n_passed_s)) l = sizeof(n_passed_s) - 1;
            memcpy(n_passed_s, p, l);
            n_passed_s[l] = '\0';
        }

        p = strstr(buf, "\"n_failed\":");
        if (p) {
            p += 11;
            size_t l = 0;
            while (p[l] && (isdigit((unsigned char)p[l]) || p[l] == '-'))
                l++;
            if (l >= sizeof(n_failed_s)) l = sizeof(n_failed_s) - 1;
            memcpy(n_failed_s, p, l);
            n_failed_s[l] = '\0';
        }

        p = strstr(buf, "\"overall_score\":");
        if (p) {
            p += 16;
            size_t l = 0;
            while (p[l] && (isdigit((unsigned char)p[l]) || p[l] == '.' || p[l] == '-'))
                l++;
            if (l >= sizeof(score_s)) l = sizeof(score_s) - 1;
            memcpy(score_s, p, l);
            score_s[l] = '\0';
        }

        int np = atoi(n_passed_s);
        int nf = atoi(n_failed_s);
        int total = np + nf;
        double sc = total > 0 ? (double)np / (double)total : 0.0;

        printf("%-35s %-28s %6d %6d  %.0f%%\n",
               model, ts, np, total, sc * 100.0);
        found++;
    }
    closedir(d);

    if (!found)
        printf("(no results found — run 'nanocode --benchmark' first)\n");
    printf("\n");
}

/* -------------------------------------------------------------------------
 * --tune: adjust temperature based on score
 * ---------------------------------------------------------------------- */

static void do_tune(Config *cfg, double overall_score)
{
    /* Current temperature: -1 means unset, 0–2000 are x1000 values.
     * We interpret 0 as the default (1000 = 1.0).
     * Strategy:
     *   score < 0.6 → lower temperature to 0.5 (more deterministic)
     *   score < 0.8 → lower temperature to 0.7
     *   score >= 0.9 → keep or raise to 0.8 (slightly more creative)
     */
    const char *cur = config_get_str(cfg, "provider.temperature");
    int cur_val = cur ? atoi(cur) : 0; /* 0 = unset */

    char new_val[16];
    const char *action = NULL;

    if (overall_score < 0.6) {
        if (cur_val != 500) {
            snprintf(new_val, sizeof(new_val), "500");
            action = "lowering temperature to 0.5 (poor scores)";
        }
    } else if (overall_score < 0.8) {
        if (cur_val != 700) {
            snprintf(new_val, sizeof(new_val), "700");
            action = "lowering temperature to 0.7 (below threshold)";
        }
    } else {
        if (cur_val != 800 && cur_val != 0) {
            snprintf(new_val, sizeof(new_val), "800");
            action = "setting temperature to 0.8 (good scores)";
        }
    }

    if (!action) {
        printf("nanocode: benchmark --tune: no adjustment needed "
               "(score=%.0f%%, temp=%d)\n",
               overall_score * 100.0, cur_val);
        return;
    }

    config_set(cfg, "provider.temperature", new_val);

    /* Save to default config path. */
    const char *home = getenv("HOME");
    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/.nanocode/config.toml",
             home ? home : ".");
    if (config_save(cfg, cfg_path) == 0)
        printf("nanocode: benchmark --tune: %s (saved to %s)\n",
               action, cfg_path);
    else
        fprintf(stderr,
                "nanocode: benchmark --tune: %s (warning: could not save config)\n",
                action);
}


/* -------------------------------------------------------------------------
 * benchmark_run — public entry point
 * ---------------------------------------------------------------------- */

int benchmark_run(const BenchFlags *flags, const ProviderConfig *pcfg,
                  Config *cfg)
{
    /* --compare only: skip running tests. */
    if (flags->do_compare && !flags->do_tune) {
        do_compare();
        return 0;
    }

    /* Load test cases: built-ins + user-defined. */
#define MAX_CASES 128
    BenchCase cases[MAX_CASES];
    int n_cases = 0;

    n_cases += benchmark_builtin_cases(cases + n_cases, MAX_CASES - n_cases);
    n_cases += load_user_cases(cases + n_cases, &n_cases, MAX_CASES);

    if (n_cases == 0) {
        fprintf(stderr, "nanocode: benchmark: no test cases found\n");
        return 1;
    }

    /* Allocate result array. */
    BenchResult *results = calloc((size_t)n_cases, sizeof(BenchResult));
    if (!results) {
        fprintf(stderr, "nanocode: benchmark: out of memory\n");
        return 1;
    }

    /* Create one event loop reused across all tests. */
    Loop *loop = loop_new();
    if (!loop) {
        fprintf(stderr, "nanocode: benchmark: failed to create event loop\n");
        free(results);
        return 1;
    }

    printf("nanocode: running %d benchmark test(s) against model '%s'...\n\n",
           n_cases, pcfg->model);

    int n_passed = 0;
    int n_failed = 0;

    for (int i = 0; i < n_cases; i++) {
        const BenchCase *tc = &cases[i];
        BenchResult     *res = &results[i];

        snprintf(res->name, sizeof(res->name), "%s", tc->name);
        res->category = tc->category;

        printf("  [%d/%d] %-30s ... ", i + 1, n_cases, tc->name);
        fflush(stdout);

        /* Create provider for this test. */
        Provider *prov = provider_new(loop, pcfg);
        if (!prov) {
            snprintf(res->error, sizeof(res->error), "failed to create provider");
            res->passed = 0;
            res->score  = 0.0;
            n_failed++;
            printf("ERROR (provider)\n");
            continue;
        }

        /* Response accumulation buffer. */
        Buf rbuf;
        buf_init(&rbuf);

        BenchStreamCtx sctx;
        sctx.loop         = loop;
        sctx.response_buf = &rbuf;
        sctx.done         = 0;
        sctx.error        = 0;

        Message msg = { "user", tc->prompt };

        int start_rc = provider_stream(prov, &msg, 1,
                                       bench_on_token, NULL,
                                       bench_on_done, &sctx);
        if (start_rc != 0) {
            snprintf(res->error, sizeof(res->error), "stream start failed");
            res->passed = 0;
            res->score  = 0.0;
            n_failed++;
            provider_free(prov);
            buf_destroy(&rbuf);
            printf("ERROR (stream)\n");
            continue;
        }

        /* Poll until done (5-second step timeout). */
        while (!sctx.done) {
            if (loop_step(loop, 5000) < 0)
                break;
        }

        provider_free(prov);

        if (sctx.error) {
            snprintf(res->error, sizeof(res->error), "stream error %d", sctx.error);
            res->passed = 0;
            res->score  = 0.0;
            n_failed++;
            buf_destroy(&rbuf);
            printf("ERROR (stream %d)\n", sctx.error);
            continue;
        }

        /* Copy response (truncated to buffer limit). */
        const char *rtext = buf_str(&rbuf);
        if (rtext) {
            size_t rlen = strlen(rtext);
            if (rlen >= sizeof(res->response))
                rlen = sizeof(res->response) - 1;
            memcpy(res->response, rtext, rlen);
            res->response[rlen] = '\0';
        }
        buf_destroy(&rbuf);

        /* Score. */
        res->passed = benchmark_score(tc, res->response, &res->score);
        if (res->passed)
            n_passed++;
        else
            n_failed++;

        printf("%s (%.0f%%)\n", res->passed ? "PASS" : "FAIL",
               res->score * 100.0);
    }

    loop_free(loop);

    /* Build report. */
    BenchReport report;
    memset(&report, 0, sizeof(report));
    snprintf(report.model, sizeof(report.model), "%s", pcfg->model);

    {
        time_t now = time(NULL);
        struct tm *tm_utc = gmtime(&now);
        if (tm_utc)
            strftime(report.timestamp, sizeof(report.timestamp),
                     "%Y%m%dT%H%M%SZ", tm_utc);
        else
            snprintf(report.timestamp, sizeof(report.timestamp), "unknown");
    }

    report.results      = results;
    report.n_results    = n_cases;
    report.n_passed     = n_passed;
    report.n_failed     = n_failed;
    report.overall_score = n_cases > 0
                         ? (double)n_passed / (double)n_cases
                         : 0.0;

    /* Print report. */
    benchmark_print_report(&report);

    /* Save JSON. */
    save_report_json(&report);

    /* --compare: show comparison table after saving. */
    if (flags->do_compare)
        do_compare();

    /* --tune: adjust temperature. */
    if (flags->do_tune && cfg)
        do_tune(cfg, report.overall_score);

    int exit_code = (n_failed > 0) ? 1 : 0;

    benchmark_report_free(&report);
    return exit_code;

#undef MAX_CASES
}

#endif /* !BENCHMARK_TEST */

/* -------------------------------------------------------------------------
 * benchmark_report_free — available in all build modes
 * ---------------------------------------------------------------------- */

void benchmark_report_free(BenchReport *report)
{
    if (report && report->results) {
        free(report->results);
        report->results = NULL;
        report->n_results = 0;
    }
}

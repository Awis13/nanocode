/*
 * json_output.c — JSON output serializer for nanocode --json mode
 *
 * Writes a single JSON object to stdout. No arena needed — uses malloc/free
 * directly since this is a one-shot serializer at process exit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/json_output.h"

void json_output_init(JsonOutput *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
}

void json_output_free(JsonOutput *out)
{
    if (!out) return;
    /* Caller owns the string pointers — we only free the arrays we allocate
     * in json_output_add_* helpers if they were used. For now the struct
     * fields are set directly, so free is a no-op beyond zeroing. */
    memset(out, 0, sizeof(*out));
}

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/* Write a JSON-escaped string to stdout. Handles the common ASCII subset plus
 * the mandatory escapes from RFC 8259. */
static void write_json_string(const char *s)
{
    if (!s) {
        fputs("null", stdout);
        return;
    }
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\b': fputs("\\b",  stdout); break;
            case '\f': fputs("\\f",  stdout); break;
            case '\n': fputs("\\n",  stdout); break;
            case '\r': fputs("\\r",  stdout); break;
            case '\t': fputs("\\t",  stdout); break;
            default:
                if (*p < 0x20) {
                    printf("\\u%04x", (unsigned)*p);
                } else {
                    putchar((int)*p);
                }
                break;
        }
    }
    putchar('"');
}

/* Write a JSON string array. Handles NULL array (emits []). */
static void write_string_array(char **arr, int n)
{
    putchar('[');
    for (int i = 0; i < n; i++) {
        if (i) putchar(',');
        write_json_string(arr ? arr[i] : NULL);
    }
    putchar(']');
}

/* --------------------------------------------------------------------------
 * json_output_print
 * -------------------------------------------------------------------------- */

void json_output_print(const JsonOutput *out)
{
    if (!out) {
        puts("{}");
        return;
    }

    putchar('{');

    /* status */
    fputs("\"status\":", stdout);
    write_json_string(out->status ? out->status : "error");

    /* result */
    fputs(",\"result\":", stdout);
    write_json_string(out->result);

    /* files_modified */
    fputs(",\"files_modified\":", stdout);
    write_string_array(out->files_modified, out->n_files_modified);

    /* files_read */
    fputs(",\"files_read\":", stdout);
    write_string_array(out->files_read, out->n_files_read);

    /* tool_calls */
    fputs(",\"tool_calls\":[", stdout);
    for (int i = 0; i < out->n_tool_calls; i++) {
        if (i) putchar(',');
        putchar('{');
        fputs("\"tool\":", stdout);
        write_json_string(out->tool_calls[i].tool);
        fputs(",\"args\":", stdout);
        /* args_json is already a JSON object string — emit verbatim if set */
        if (out->tool_calls[i].args_json)
            fputs(out->tool_calls[i].args_json, stdout);
        else
            fputs("{}", stdout);
        printf(",\"result_size\":%ld", out->tool_calls[i].result_size);
        putchar('}');
    }
    putchar(']');

    /* duration_ms */
    printf(",\"duration_ms\":%ld", out->duration_ms);

    puts("}");
}

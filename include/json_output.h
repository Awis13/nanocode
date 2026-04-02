/*
 * json_output.h — JSON output mode for nanocode (--json flag)
 *
 * When stdout is consumed by another process or NANOCODE_JSON=1,
 * nanocode emits a single JSON object on exit instead of TUI chrome.
 */

#ifndef JSON_OUTPUT_H
#define JSON_OUTPUT_H

/* Exit codes */
#define NC_EXIT_OK               0
#define NC_EXIT_ERROR            1
#define NC_EXIT_SANDBOX_VIOLATION 2
#define NC_EXIT_TIMEOUT          3

typedef struct {
    char   *status;          /* "done" | "error" | "timeout" | "sandbox_violation" */
    char   *result;          /* assistant final message text, or NULL */
    char  **files_modified;  /* NULL-terminated array of modified paths */
    int     n_files_modified;
    char  **files_read;      /* NULL-terminated array of read paths */
    int     n_files_read;
    struct {
        char *tool;
        char *args_json;     /* raw JSON object string */
        long  result_size;
    } *tool_calls;
    int     n_tool_calls;
    long    duration_ms;
} JsonOutput;

void json_output_init(JsonOutput *out);
void json_output_free(JsonOutput *out);
void json_output_print(const JsonOutput *out);  /* writes JSON to stdout */

#endif /* JSON_OUTPUT_H */

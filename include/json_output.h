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

/*
 * Ownership: all pointer fields are owned by the caller.  json_output_print()
 * reads but does not free them.  json_output_free() zeroes the struct but does
 * not free caller-owned pointers.  files_modified, files_read, tool_calls, and
 * all strings they reference must remain valid for the duration of
 * json_output_print().
 */
typedef struct {
    char   *status;          /* "done" | "error" | "timeout" | "sandbox_violation" */
    char   *result;          /* assistant final message text, or NULL */
    char  **files_modified;  /* caller-owned array of modified path strings */
    int     n_files_modified;
    char  **files_read;      /* caller-owned array of read path strings */
    int     n_files_read;
    struct {
        char *tool;
        char *args_json;     /* caller-owned raw JSON object string */
        long  result_size;
    } *tool_calls;           /* caller-owned array of n_tool_calls entries */
    int     n_tool_calls;
    long    duration_ms;
} JsonOutput;

void json_output_init(JsonOutput *out);
void json_output_free(JsonOutput *out);
void json_output_print(const JsonOutput *out);  /* writes JSON to stdout */

#endif /* JSON_OUTPUT_H */

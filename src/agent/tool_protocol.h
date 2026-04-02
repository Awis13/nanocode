/*
 * tool_protocol.h — provider-agnostic tool call parsing and dispatch
 *
 * Handles:
 *   - Parsing Claude tool_use blocks from a completed response JSON
 *   - Parsing OpenAI/Ollama function_call blocks from a completed response JSON
 *   - Building the tools JSON array for inclusion in a provider request
 *   - Dispatching parsed tool calls through the executor and recording results
 */

#ifndef TOOL_PROTOCOL_H
#define TOOL_PROTOCOL_H

#include "../api/provider.h"
#include "../util/arena.h"
#include "conversation.h"

/* -------------------------------------------------------------------------
 * ToolCall — one parsed tool invocation
 * ---------------------------------------------------------------------- */

typedef struct {
    char *id;     /* tool_use_id (Claude) or call id (OpenAI) */
    char *name;   /* tool name */
    char *input;  /* JSON args string — NUL-terminated, arena-allocated */
} ToolCall;

/* -------------------------------------------------------------------------
 * tool_parse_response
 *
 * Parse tool calls out of a completed provider response JSON body.
 *
 * `type`      — PROVIDER_CLAUDE or PROVIDER_OPENAI / PROVIDER_OLLAMA
 * `json`      — NUL-terminated response JSON string
 * `json_len`  — byte length of json (excluding the NUL)
 * `arena`     — all allocations (ToolCall array + strings) go here
 * `calls_out` — on success, set to an arena-allocated ToolCall array
 *
 * Returns:
 *   >= 0  number of tool calls parsed (0 means the response has no tool calls)
 *   -1    on parse error
 * ---------------------------------------------------------------------- */
int tool_parse_response(ProviderType type, const char *json, size_t json_len,
                        Arena *arena, ToolCall **calls_out);

/* -------------------------------------------------------------------------
 * tool_build_schema_payload
 *
 * Build the tools JSON array for inclusion in a provider request body.
 *
 *   PROVIDER_CLAUDE         — returns the array from tool_schemas_json():
 *                             [{"name":...,"description":...,"input_schema":{}},...
 *   PROVIDER_OPENAI / _OLLAMA — wraps each schema entry as:
 *                             [{"type":"function","function":{...}},...
 *
 * Returns an arena-allocated NUL-terminated JSON string, or NULL on failure.
 * ---------------------------------------------------------------------- */
char *tool_build_schema_payload(ProviderType type, Arena *arena);

/* -------------------------------------------------------------------------
 * tool_dispatch_all
 *
 * For each ToolCall in calls[0..ncalls):
 *   1. Records an assistant tool_use turn in conv.
 *   2. Calls tool_invoke() to get the result.
 *   3. Records a user tool_result turn in conv.
 *
 * Returns the number of calls dispatched (== ncalls on full success).
 * ---------------------------------------------------------------------- */
int tool_dispatch_all(const ToolCall *calls, int ncalls,
                      Conversation *conv, Arena *arena);

#endif /* TOOL_PROTOCOL_H */

/*
 * mcp.h — MCP client: JSON-RPC 2.0 over STDIO pipes
 *
 * Implements a Model Context Protocol client that spawns MCP servers as child
 * processes and communicates via their stdin/stdout using JSON-RPC 2.0.
 *
 * Lifecycle:
 *   MCPClient *c = mcp_client_new(arena, "mcp-server-filesystem", argv);
 *   if (!c || mcp_client_init(c) < 0) { ... handle error ... }
 *   ToolList  *tools = mcp_list_tools(c, arena);
 *   ToolResult r     = mcp_call_tool(c, arena, "read_file", "{\"path\":\"/tmp\"}");
 *   mcp_client_free(c);
 *
 * All allocations for ToolList and ToolResult content go through the
 * caller-supplied Arena. MCPClient itself is heap-allocated (malloc/free).
 *
 * Sub-process calls are guarded by #ifndef __SANITIZE_ADDRESS__ to prevent
 * fork() hangs under AddressSanitizer on macOS.
 */

#ifndef MCP_H
#define MCP_H

#include "../util/arena.h"
#include "../tools/executor.h"

/* Maximum tools discoverable across all MCP servers combined. */
#define MCP_MAX_TOTAL_TOOLS 32

/* Maximum number of MCP servers in mcp.json. */
#define MCP_SERVERS_MAX 16

/* Maximum argv entries per server command (including argv[0]). */
#define MCP_ARGS_MAX 16

/* A single discovered MCP tool. */
typedef struct {
    char name[128];
    char description[512];
    char schema_json[2048]; /* full Claude tool schema: {"name":...,"input_schema":{...}} */
} MCPToolEntry;

/* List of tools returned by mcp_list_tools(). Memory is arena-allocated. */
typedef struct {
    MCPToolEntry *tools;
    int           count;
} ToolList;

/* Opaque MCP client handle (heap-allocated). */
typedef struct MCPClient MCPClient;

/*
 * Spawn `server_cmd` as a child process, wiring up stdin/stdout pipes.
 * `argv` must be a NULL-terminated array with argv[0] as the program name.
 * Returns a heap-allocated MCPClient on success, NULL on failure.
 * The `arena` parameter is reserved for future use.
 */
MCPClient *mcp_client_new(Arena *arena, const char *server_cmd,
                          char *const argv[]);

/*
 * Perform the MCP initialize / notifications/initialized handshake.
 * Must be called before mcp_list_tools() or mcp_call_tool().
 * Returns 0 on success, -1 on failure.
 */
int mcp_client_init(MCPClient *c);

/*
 * Call tools/list and return all discovered tools.
 * Returns an arena-allocated ToolList on success, NULL on failure.
 * Each MCPToolEntry.schema_json is a Claude-compatible tool JSON object.
 */
ToolList *mcp_list_tools(MCPClient *c, Arena *arena);

/*
 * Call tools/call for `name` with `args_json` (a JSON object string, e.g.
 * "{\"path\":\"/tmp\"}").  Returns a ToolResult; result.error != 0 indicates
 * failure (server error, server crash, parse failure, etc.).
 * All result memory is arena-allocated.
 */
ToolResult mcp_call_tool(MCPClient *c, Arena *arena, const char *name,
                         const char *args_json);

/*
 * Send SIGTERM to the child process, close all pipe file descriptors, wait
 * for the child, and free the MCPClient struct.  Safe to call with NULL.
 */
void mcp_client_free(MCPClient *c);

/*
 * Load ~/.nanocode/mcp.json, spawn each configured MCP server, run the
 * initialize handshake, discover tools, and register them in the executor
 * so they are available to the AI via tool_invoke().
 *
 * Safe to call at startup: silently does nothing if the config file is
 * absent, malformed, or contains no valid servers.
 */
void mcp_tools_register(void);

#endif /* MCP_H */

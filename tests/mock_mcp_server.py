#!/usr/bin/env python3
"""
Minimal MCP server mock for testing mcp.c.

Speaks JSON-RPC 2.0 over stdin/stdout.  Handles:
  - initialize
  - notifications/initialized  (silently ignored)
  - tools/list
  - tools/call  (name "mock_echo" echoes the "text" argument)

Any other method returns a JSON-RPC error.

Usage (normally invoked by the C test):
  python3 tests/mock_mcp_server.py
"""

import sys
import json


def send(obj):
    line = json.dumps(obj, separators=(',', ':'))
    sys.stdout.write(line + '\n')
    sys.stdout.flush()


def handle(req):
    method  = req.get('method', '')
    req_id  = req.get('id')      # None for notifications
    params  = req.get('params', {}) or {}

    if method == 'initialize':
        send({
            'jsonrpc': '2.0',
            'id': req_id,
            'result': {
                'protocolVersion': '2024-11-05',
                'capabilities': {},
                'serverInfo': {'name': 'mock', 'version': '0.1'}
            }
        })

    elif method == 'notifications/initialized':
        pass  # notification — no response

    elif method == 'tools/list':
        send({
            'jsonrpc': '2.0',
            'id': req_id,
            'result': {
                'tools': [
                    {
                        'name': 'mock_echo',
                        'description': 'Echoes the text argument back',
                        'inputSchema': {
                            'type': 'object',
                            'properties': {
                                'text': {'type': 'string', 'description': 'Text to echo'}
                            },
                            'required': ['text']
                        }
                    }
                ]
            }
        })

    elif method == 'tools/call':
        tool_name = params.get('name', '')
        args      = params.get('arguments', {}) or {}
        if tool_name == 'mock_echo':
            text = args.get('text', '')
            send({
                'jsonrpc': '2.0',
                'id': req_id,
                'result': {
                    'content': [{'type': 'text', 'text': text}],
                    'isError': False
                }
            })
        else:
            send({
                'jsonrpc': '2.0',
                'id': req_id,
                'error': {'code': -32601, 'message': f'unknown tool: {tool_name}'}
            })

    elif req_id is not None:
        send({
            'jsonrpc': '2.0',
            'id': req_id,
            'error': {'code': -32601, 'message': f'method not found: {method}'}
        })


def main():
    for raw_line in sys.stdin:
        raw_line = raw_line.strip()
        if not raw_line:
            continue
        try:
            req = json.loads(raw_line)
        except json.JSONDecodeError:
            continue
        handle(req)


if __name__ == '__main__':
    main()

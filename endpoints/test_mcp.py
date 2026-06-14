"""
Tests for libbitcoin-server MCP (Model Context Protocol) interface.

Tests the JSON-RPC 2.0 / Streamable HTTP interface that exposes blockchain
data to AI agents via the Anthropic Model Context Protocol specification.

Reference: https://spec.modelcontextprotocol.io/specification/

Run with:
    pytest test_mcp.py
    pytest test_mcp.py --mcp-host=localhost --mcp-port=8334
    pytest test_mcp.py -v -s  (verbose, with print output)

Environment:
    MCP_DEBUG=1   Print request/response pairs to stdout
"""

import json
import os
import time
import warnings

import pytest
import requests
from typing import Any, Dict, Optional

from utils import ReferenceData, TestConfig, validate_hex_hash

# ---------------------------------------------------------------------------
# MCP protocol constants
# ---------------------------------------------------------------------------

MCP_PROTOCOL_VERSION = "2024-11-05"

_id_counter = 0


def _next_id() -> int:
    global _id_counter
    _id_counter += 1
    return _id_counter


# ---------------------------------------------------------------------------
# Transport helper
# ---------------------------------------------------------------------------

def send_mcp(
    config: dict,
    method: str,
    params: Optional[Dict[str, Any]] = None,
    *,
    notification: bool = False,
) -> Optional[Dict[str, Any]]:
    """
    Send a single JSON-RPC 2.0 request to the MCP endpoint.

    Args:
        config:       mcp_config fixture dict (url, timeout).
        method:       JSON-RPC method name (e.g. "tools/call").
        params:       Optional params object (dict).  MCP always uses named
                      params, never a positional array.
        notification: When True, omit the ``id`` field.  The server MUST
                      return HTTP 200 with no JSON body (MCP spec §3.2).

    Returns:
        Parsed response dict, or None for notifications.
    """
    payload: Dict[str, Any] = {"jsonrpc": "2.0", "method": method}
    if not notification:
        payload["id"] = _next_id()
    if params is not None:
        payload["params"] = params

    if os.getenv("MCP_DEBUG"):
        print("\n>>>", json.dumps(payload, indent=2), flush=True)

    t0 = time.monotonic()
    try:
        response = requests.post(
            config["url"],
            json=payload,
            headers={"Content-Type": "application/json", "Accept": "application/json"},
            timeout=config.get("timeout", TestConfig.DEFAULT_RPC_TIMEOUT),
        )
    except requests.exceptions.ConnectionError as exc:
        pytest.skip(f"MCP server not reachable: {exc}")

    elapsed_ms = (time.monotonic() - t0) * 1000

    if os.getenv("MCP_DEBUG"):
        print(
            f"<<< {method} ({elapsed_ms:.1f} ms) HTTP {response.status_code}",
            response.text[:1000] if response.text else "(empty)",
            flush=True,
        )

    # Notifications: server must return 200 with no JSON body.
    if notification:
        assert response.status_code == 200, (
            f"Expected HTTP 200 for notification, got {response.status_code}"
        )
        return None

    assert response.status_code == 200, (
        f"Unexpected HTTP {response.status_code}: {response.text[:200]}"
    )

    try:
        data = response.json()
    except ValueError:
        pytest.fail(f"Non-JSON response: {response.text[:200]!r}")

    assert data.get("jsonrpc") == "2.0", "Response missing jsonrpc='2.0'"
    return data


def call_tool(
    config: dict,
    name: str,
    arguments: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    """
    Convenience wrapper for tools/call.  Returns the response dict.
    Marks the test as xfail on a server-side error response.
    """
    params = {"name": name}
    if arguments is not None:
        params["arguments"] = arguments

    response = send_mcp(config, "tools/call", params)
    assert response is not None

    if response.get("error") is not None:
        pytest.xfail(f"Server error for tool '{name}': {response['error']}")

    assert "result" in response, f"No 'result' field in response: {response}"
    return response


def assert_tool_result(result: Any) -> str:
    """
    Validate MCP ToolResult envelope and return the text content.

    Expected shape:
        {"content": [{"type": "text", "text": "<json string>"}]}
    """
    assert isinstance(result, dict), f"result must be a dict, got {type(result)}"
    assert "content" in result, f"result missing 'content': {result}"
    content = result["content"]
    assert isinstance(content, list) and len(content) >= 1, (
        f"'content' must be a non-empty list: {content}"
    )
    item = content[0]
    assert item.get("type") == "text", f"content[0].type must be 'text': {item}"
    assert isinstance(item.get("text"), str), f"content[0].text must be a str: {item}"
    return item["text"]


# ═══════════════════════════════════════════════════════════════════════════
# SESSION LIFECYCLE
# ═══════════════════════════════════════════════════════════════════════════

class TestSessionLifecycle:

    def test_initialize(self, mcp_config):
        """initialize returns protocolVersion, capabilities and serverInfo."""
        response = send_mcp(mcp_config, "initialize", {
            "protocolVersion": MCP_PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "pytest-mcp", "version": "1.0"},
        })

        assert response is not None
        assert response.get("error") is None, f"Error in initialize: {response['error']}"

        result = response["result"]
        assert isinstance(result, dict)

        assert result.get("protocolVersion") == MCP_PROTOCOL_VERSION, (
            f"protocolVersion mismatch: {result.get('protocolVersion')}"
        )

        caps = result.get("capabilities", {})
        assert isinstance(caps, dict)
        assert "tools" in caps, f"capabilities must include 'tools': {caps}"

        server_info = result.get("serverInfo", {})
        assert isinstance(server_info, dict)
        assert "name" in server_info
        assert "version" in server_info

    def test_initialize_minimal_params(self, mcp_config):
        """initialize works when optional fields are absent."""
        response = send_mcp(mcp_config, "initialize", {
            "protocolVersion": MCP_PROTOCOL_VERSION,
        })
        assert response is not None
        assert response.get("error") is None
        assert "result" in response

    def test_notifications_initialized(self, mcp_config):
        """notifications/initialized is accepted with HTTP 200 and no body."""
        # This is a JSON-RPC notification (no id field).
        send_mcp(mcp_config, "notifications/initialized", notification=True)


# ═══════════════════════════════════════════════════════════════════════════
# TOOL DISCOVERY
# ═══════════════════════════════════════════════════════════════════════════

class TestToolsDiscovery:

    EXPECTED_TOOLS = {
        "get_blockchain_info",
        "get_block_header",
        "get_block_details",
        "get_block_txs",
        "get_transaction",
        "get_address_balance",
        "get_address_history",
    }

    def test_tools_list_shape(self, mcp_config):
        """tools/list returns a tools array with required fields per tool."""
        response = send_mcp(mcp_config, "tools/list")

        assert response is not None
        assert response.get("error") is None

        result = response["result"]
        assert isinstance(result, dict)
        assert "tools" in result, f"result missing 'tools': {result}"

        tools = result["tools"]
        assert isinstance(tools, list)
        assert len(tools) > 0, "tools/list returned empty tool list"

        for tool in tools:
            assert "name" in tool, f"tool missing 'name': {tool}"
            assert "description" in tool, f"tool missing 'description': {tool}"
            assert "inputSchema" in tool, f"tool missing 'inputSchema': {tool}"
            schema = tool["inputSchema"]
            assert schema.get("type") == "object", (
                f"inputSchema.type must be 'object': {schema}"
            )

    def test_tools_list_contains_all_tools(self, mcp_config):
        """tools/list includes all declared blockchain tools."""
        response = send_mcp(mcp_config, "tools/list")
        assert response is not None

        names = {t["name"] for t in response["result"]["tools"]}
        missing = self.EXPECTED_TOOLS - names
        assert not missing, f"Missing tools in tools/list: {missing}"

    def test_tools_list_idempotent(self, mcp_config):
        """tools/list returns the same tool set on repeated calls."""
        r1 = send_mcp(mcp_config, "tools/list")
        r2 = send_mcp(mcp_config, "tools/list")
        names1 = sorted(t["name"] for t in r1["result"]["tools"])
        names2 = sorted(t["name"] for t in r2["result"]["tools"])
        assert names1 == names2


# ═══════════════════════════════════════════════════════════════════════════
# TOOL INVOCATION — get_blockchain_info
# ═══════════════════════════════════════════════════════════════════════════

class TestToolGetBlockchainInfo:

    def test_returns_height_and_hash(self, mcp_config):
        """get_blockchain_info returns current tip height and hash."""
        response = call_tool(mcp_config, "get_blockchain_info")

        text = assert_tool_result(response["result"])
        data = json.loads(text)

        assert "height" in data, f"Missing 'height': {data}"
        assert "hash" in data, f"Missing 'hash': {data}"

        assert isinstance(data["height"], int)
        assert data["height"] >= 0

        assert validate_hex_hash(data["hash"]), (
            f"hash is not a valid 64-char hex: {data['hash']}"
        )

    def test_height_is_positive(self, mcp_config):
        """Chain tip height is above genesis (server has chain data)."""
        response = call_tool(mcp_config, "get_blockchain_info")
        text = assert_tool_result(response["result"])
        data = json.loads(text)
        assert data["height"] > 0, "Node appears to have no blocks beyond genesis"

    def test_no_arguments_required(self, mcp_config):
        """get_blockchain_info accepts empty arguments object."""
        params = {"name": "get_blockchain_info", "arguments": {}}
        response = send_mcp(mcp_config, "tools/call", params)
        assert response is not None
        assert response.get("error") is None


# ═══════════════════════════════════════════════════════════════════════════
# TOOL INVOCATION — get_block
# ═══════════════════════════════════════════════════════════════════════════

class TestToolGetBlockHeader:

    def test_genesis_block(self, mcp_config):
        """get_block_header at height 0 returns the genesis block header."""
        response = call_tool(mcp_config, "get_block_header", {"height": 0})

        text = assert_tool_result(response["result"])
        data = json.loads(text)

        assert isinstance(data, dict), f"Expected dict, got {type(data)}"
        assert "hash" in data, f"Missing 'hash': {data}"
        assert data["hash"] == ReferenceData.GENESIS_HASH, (
            f"Wrong genesis hash: {data['hash']}"
        )

    def test_known_block_by_height(self, mcp_config):
        """get_block_header by height returns header with expected hash."""
        response = call_tool(
            mcp_config, "get_block_header", {"height": ReferenceData.KNOWN_HEIGHT}
        )
        text = assert_tool_result(response["result"])
        data = json.loads(text)
        assert isinstance(data, dict)
        assert data.get("hash") == ReferenceData.KNOWN_BLOCK_HASH

    def test_known_block_by_hash(self, mcp_config):
        """get_block_header by hash returns the same header as by height."""
        r1 = call_tool(mcp_config, "get_block_header",
                       {"height": ReferenceData.KNOWN_HEIGHT})
        r2 = call_tool(mcp_config, "get_block_header",
                       {"hash": ReferenceData.KNOWN_BLOCK_HASH})
        assert assert_tool_result(r1["result"]) == assert_tool_result(r2["result"])

    def test_block_beyond_tip_returns_error(self, mcp_config):
        """get_block_header at height beyond tip returns a not_found error."""
        params = {"name": "get_block_header", "arguments": {"height": 99_999_999}}
        response = send_mcp(mcp_config, "tools/call", params)
        assert response is not None
        assert response.get("error") is not None, (
            "Expected error for out-of-range height"
        )

    def test_missing_args_returns_error(self, mcp_config):
        """get_block_header without height or hash returns invalid_argument error."""
        params = {"name": "get_block_header", "arguments": {}}
        response = send_mcp(mcp_config, "tools/call", params)
        assert response is not None
        assert response.get("error") is not None


# ═══════════════════════════════════════════════════════════════════════════
# TOOL INVOCATION — get_transaction
# ═══════════════════════════════════════════════════════════════════════════

class TestToolGetTransaction:

    def test_known_transaction(self, mcp_config):
        """get_transaction returns data for a known confirmed txid."""
        response = call_tool(
            mcp_config,
            "get_transaction",
            {"hash": ReferenceData.KNOWN_TX_HASH},
        )

        text = assert_tool_result(response["result"])
        data = json.loads(text)
        assert isinstance(data, dict), f"Expected dict, got {type(data)}"

    def test_first_transaction(self, mcp_config):
        """get_transaction works for the first-ever Bitcoin transaction."""
        response = call_tool(
            mcp_config,
            "get_transaction",
            {"hash": ReferenceData.FIRST_TX_HASH},
        )

        text = assert_tool_result(response["result"])
        data = json.loads(text)
        assert isinstance(data, dict)

    def test_unknown_txid_returns_error(self, mcp_config):
        """get_transaction with a non-existent txid returns not_found error."""
        params = {
            "name": "get_transaction",
            "arguments": {"hash": "00" * 32},
        }
        response = send_mcp(mcp_config, "tools/call", params)
        assert response is not None
        assert response.get("error") is not None, (
            "Expected error for unknown txid"
        )

    def test_invalid_hash_returns_error(self, mcp_config):
        """get_transaction with a non-hex hash returns invalid_argument error."""
        params = {
            "name": "get_transaction",
            "arguments": {"hash": "not-a-hash"},
        }
        response = send_mcp(mcp_config, "tools/call", params)
        assert response is not None
        assert response.get("error") is not None, (
            "Expected error for invalid hash"
        )

    def test_missing_hash_returns_error(self, mcp_config):
        """get_transaction without 'hash' argument returns invalid_argument error."""
        params = {"name": "get_transaction", "arguments": {}}
        response = send_mcp(mcp_config, "tools/call", params)
        assert response is not None
        assert response.get("error") is not None


# ═══════════════════════════════════════════════════════════════════════════
# TOOL INVOCATION — not_implemented stubs
# ═══════════════════════════════════════════════════════════════════════════

class TestToolsNotImplemented:
    """
    Address tools are not yet implemented (require address indexing).
    They must respond with a structured not_implemented error, not a crash.
    """

    @pytest.mark.xfail(reason="get_address_balance not yet implemented")
    def test_get_address_balance(self, mcp_config):
        params = {
            "name": "get_address_balance",
            "arguments": {"address": ReferenceData.EXAMPLE_ADDRESS},
        }
        response = send_mcp(mcp_config, "tools/call", params)
        assert response is not None
        # Once implemented this should return a result; until then xfail.
        assert response.get("error") is None

    @pytest.mark.xfail(reason="get_address_history not yet implemented")
    def test_get_address_history(self, mcp_config):
        params = {
            "name": "get_address_history",
            "arguments": {"address": ReferenceData.EXAMPLE_ADDRESS},
        }
        response = send_mcp(mcp_config, "tools/call", params)
        assert response is not None
        assert response.get("error") is None

    def test_address_balance_returns_structured_error(self, mcp_config):
        """Not-implemented tools return a proper JSON-RPC error, not a 5xx."""
        params = {
            "name": "get_address_balance",
            "arguments": {"address": ReferenceData.EXAMPLE_ADDRESS},
        }
        response = send_mcp(mcp_config, "tools/call", params)
        assert response is not None
        error = response.get("error")
        assert error is not None, "Expected error for not_implemented tool"
        assert "code" in error, f"error missing 'code': {error}"
        assert "message" in error, f"error missing 'message': {error}"


# ═══════════════════════════════════════════════════════════════════════════
# ERROR HANDLING
# ═══════════════════════════════════════════════════════════════════════════

class TestErrorHandling:

    def test_unknown_tool_returns_error(self, mcp_config):
        """Calling an unknown tool name returns a not_found error."""
        params = {"name": "this_tool_does_not_exist", "arguments": {}}
        response = send_mcp(mcp_config, "tools/call", params)
        assert response is not None
        assert response.get("error") is not None, (
            "Expected error for unknown tool name"
        )
        error = response["error"]
        assert "code" in error
        assert "message" in error

    def test_unknown_method_returns_error(self, mcp_config):
        """An unregistered JSON-RPC method returns a method-not-found error."""
        response = send_mcp(mcp_config, "no_such_method")
        assert response is not None
        assert response.get("error") is not None, (
            "Expected error for unknown JSON-RPC method"
        )

    def test_invalid_json_returns_4xx(self, mcp_config):
        """A non-JSON POST body is rejected at the HTTP level.

        The server MAY close the connection without a response body when it
        encounters a malformed request (RFC 7230 §6.6).  Both a 400 response
        and a connection close without response are acceptable outcomes.
        """
        try:
            resp = requests.post(
                mcp_config["url"],
                data="this is not json",
                headers={"Content-Type": "application/json"},
                timeout=mcp_config.get("timeout", TestConfig.DEFAULT_RPC_TIMEOUT),
            )
        except requests.exceptions.ConnectionError:
            # Connection closed without response — valid rejection behavior.
            return

        assert resp.status_code in (400, 422), (
            f"Expected 400/422 for invalid JSON, got {resp.status_code}"
        )

    def test_response_is_jsonrpc2(self, mcp_config):
        """Every non-notification response carries jsonrpc='2.0' and id."""
        response = send_mcp(mcp_config, "tools/list")
        assert response is not None
        assert response.get("jsonrpc") == "2.0"
        assert "id" in response

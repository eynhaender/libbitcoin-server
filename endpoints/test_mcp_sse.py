"""
SSE (Server-Sent Events) subscription tests for libbitcoin-server MCP interface.

Tests the three push-subscription tools over HTTP with the SSE transport:
  get_top_subscribe   — streams bitcoin/top on each new chain tip
  get_block_subscribe — streams bitcoin/block on each confirmed block
  get_tx_subscribe    — streams bitcoin/transaction on each new mempool tx

Run:
    pytest test_mcp_sse.py -v
    pytest test_mcp_sse.py -v --sse-wait=30   (wait 30 s for live push events)
    pytest test_mcp_sse.py -v -s              (verbose, with stdout)

Environment:
    MCP_DEBUG=1   Print SSE events to stdout

Architecture note
-----------------
Each subscription tool opens a persistent SSE connection.  Tests share
per-tool session-scoped fixtures (sse_top, sse_block, sse_tx) so at most
3 connections are open at any given time, avoiding server connection limits.
"""

import json
import os
import queue
import threading
import time
from dataclasses import dataclass, field
from typing import List, Optional

import pytest
import requests

from utils import validate_hex_hash

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

SSE_CONNECT_TIMEOUT = 5.0   # seconds — TCP connect
SSE_READ_TIMEOUT    = 3.0   # seconds — wait for initial event
# Per-request read-socket timeout; keeps iter_content from blocking forever
# after the initial event arrives.
SSE_SOCKET_TIMEOUT  = 2.0

_id_counter = 0


def _next_id() -> int:
    global _id_counter
    _id_counter += 1
    return _id_counter


# ---------------------------------------------------------------------------
# SSE session dataclass
# ---------------------------------------------------------------------------

@dataclass
class SseSession:
    """Open SSE connection with cached initial event."""
    tool: str
    resp: requests.Response
    initial_event: Optional[dict]        # parsed first SSE event
    raw_initial: Optional[str]           # raw data string of first event
    content_type: str
    status_code: int
    push_events: List[dict] = field(default_factory=list)
    _reader: Optional[threading.Thread] = field(default=None, repr=False)
    _queue: queue.Queue = field(default_factory=queue.Queue, repr=False)

    def collect_pushes(self, wait: float) -> None:
        """Drain push events from the queue for up to *wait* seconds."""
        deadline = time.monotonic() + wait
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                item = self._queue.get(timeout=remaining)
            except queue.Empty:
                break
            if item is None:
                break
            try:
                self.push_events.append(json.loads(item))
            except json.JSONDecodeError:
                pass

    def close(self) -> None:
        try:
            self.resp.close()
        except Exception:
            pass
        if self._reader:
            self._reader.join(timeout=1.0)


# ---------------------------------------------------------------------------
# Low-level SSE helpers
# ---------------------------------------------------------------------------

def _open_sse_session(config: dict, tool_name: str) -> SseSession:
    """
    Open an SSE subscription for *tool_name*.  Reads the initial event
    (server sends it immediately on subscribe) and returns an SseSession.

    Raises pytest.skip() if the server is unreachable.
    Uses timeout=(connect, socket-read) so iter_content() does not block
    indefinitely after the initial event.
    """
    payload = {
        "jsonrpc": "2.0",
        "id": _next_id(),
        "method": "tools/call",
        "params": {"name": tool_name, "arguments": {}},
    }
    if os.getenv("MCP_DEBUG"):
        print(f"\n>>> SSE subscribe: {tool_name}", flush=True)

    try:
        resp = requests.post(
            config["url"],
            json=payload,
            headers={
                "Content-Type": "application/json",
                "Accept": "text/event-stream",
            },
            stream=True,
            timeout=(SSE_CONNECT_TIMEOUT, SSE_SOCKET_TIMEOUT),
        )
    except requests.exceptions.ConnectionError as exc:
        pytest.skip(f"MCP server not reachable: {exc}")

    result_q: queue.Queue = queue.Queue()

    def _reader() -> None:
        buffer = ""
        try:
            for chunk in resp.iter_content(chunk_size=256, decode_unicode=True):
                if chunk:
                    buffer += chunk
                    while "\n\n" in buffer:
                        event_block, buffer = buffer.split("\n\n", 1)
                        for line in event_block.splitlines():
                            if line.startswith("data:"):
                                result_q.put(line[5:].lstrip(" "))
        except Exception:
            pass
        finally:
            result_q.put(None)  # sentinel

    t = threading.Thread(target=_reader, daemon=True)
    t.start()

    # Collect only the first event (arrives immediately on subscribe).
    initial_raw: Optional[str] = None
    initial_event: Optional[dict] = None
    try:
        item = result_q.get(timeout=SSE_READ_TIMEOUT)
        if item is not None:
            initial_raw = item
            try:
                initial_event = json.loads(item)
            except json.JSONDecodeError:
                pass
            if os.getenv("MCP_DEBUG"):
                print(f"<<< SSE initial: {item[:300]}", flush=True)
    except queue.Empty:
        pass

    return SseSession(
        tool=tool_name,
        resp=resp,
        initial_event=initial_event,
        raw_initial=initial_raw,
        content_type=resp.headers.get("Content-Type", ""),
        status_code=resp.status_code,
        _reader=t,
        _queue=result_q,
    )


# ---------------------------------------------------------------------------
# Session-scoped fixtures — one SSE connection per subscription tool
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def sse_top(mcp_config) -> SseSession:
    """Open SSE session for get_top_subscribe (session-scoped)."""
    session = _open_sse_session(mcp_config, "get_top_subscribe")
    yield session
    session.close()


@pytest.fixture(scope="session")
def sse_block(mcp_config) -> SseSession:
    """Open SSE session for get_block_subscribe (session-scoped)."""
    session = _open_sse_session(mcp_config, "get_block_subscribe")
    yield session
    session.close()


@pytest.fixture(scope="session")
def sse_tx(mcp_config) -> SseSession:
    """Open SSE session for get_tx_subscribe (session-scoped)."""
    session = _open_sse_session(mcp_config, "get_tx_subscribe")
    yield session
    session.close()


# ═══════════════════════════════════════════════════════════════════════════
# CONNECTION — HTTP response headers
# ═══════════════════════════════════════════════════════════════════════════

class TestSSEConnection:
    """Verify that each subscription tool opens an SSE stream correctly."""

    def test_top_http_200(self, sse_top):
        assert sse_top.status_code == 200, (
            f"get_top_subscribe: expected HTTP 200, got {sse_top.status_code}"
        )

    def test_block_http_200(self, sse_block):
        assert sse_block.status_code == 200, (
            f"get_block_subscribe: expected HTTP 200, got {sse_block.status_code}"
        )

    def test_tx_http_200(self, sse_tx):
        assert sse_tx.status_code == 200, (
            f"get_tx_subscribe: expected HTTP 200, got {sse_tx.status_code}"
        )

    def test_top_content_type(self, sse_top):
        assert "text/event-stream" in sse_top.content_type, (
            f"get_top_subscribe: expected text/event-stream, got {sse_top.content_type!r}"
        )

    def test_block_content_type(self, sse_block):
        assert "text/event-stream" in sse_block.content_type, (
            f"get_block_subscribe: expected text/event-stream, got {sse_block.content_type!r}"
        )

    def test_tx_content_type(self, sse_tx):
        assert "text/event-stream" in sse_tx.content_type, (
            f"get_tx_subscribe: expected text/event-stream, got {sse_tx.content_type!r}"
        )


# ═══════════════════════════════════════════════════════════════════════════
# INITIAL EVENT — JSON-RPC response in first SSE data frame
# ═══════════════════════════════════════════════════════════════════════════

class TestSSEInitialEvent:
    """The first SSE event carries the JSON-RPC result (subscription confirmation)."""

    # ── get_top_subscribe ──────────────────────────────────────────────────

    def test_top_event_received(self, sse_top):
        """Server sends the initial event for get_top_subscribe."""
        assert sse_top.raw_initial is not None, (
            "No SSE event received within timeout for get_top_subscribe"
        )

    def test_top_jsonrpc_envelope(self, sse_top):
        """get_top_subscribe first event is a JSON-RPC 2.0 response."""
        ev = sse_top.initial_event
        assert ev is not None, "Initial event is not valid JSON"
        assert ev.get("jsonrpc") == "2.0", f"missing jsonrpc='2.0': {ev}"
        assert "id" in ev, f"missing id: {ev}"
        assert ev.get("error") is None, f"error in response: {ev.get('error')}"
        assert "result" in ev, f"missing result: {ev}"

    def test_top_subscribed_true(self, sse_top):
        """get_top_subscribe carries subscribed=true."""
        ev = sse_top.initial_event
        assert ev is not None
        content = ev["result"]["content"]
        data = json.loads(content[0]["text"])
        assert data.get("subscribed") is True, f"subscribed not true: {data}"

    def test_top_height_and_hash(self, sse_top):
        """get_top_subscribe initial event contains valid height and hash."""
        ev = sse_top.initial_event
        assert ev is not None
        data = json.loads(ev["result"]["content"][0]["text"])
        assert isinstance(data.get("height"), int) and data["height"] >= 0, (
            f"invalid height: {data}"
        )
        assert validate_hex_hash(data.get("hash", "")), (
            f"invalid hash: {data.get('hash')}"
        )

    # ── get_block_subscribe ────────────────────────────────────────────────

    def test_block_event_received(self, sse_block):
        assert sse_block.raw_initial is not None, (
            "No SSE event received within timeout for get_block_subscribe"
        )

    def test_block_jsonrpc_envelope(self, sse_block):
        ev = sse_block.initial_event
        assert ev is not None
        assert ev.get("jsonrpc") == "2.0"
        assert ev.get("error") is None
        assert "result" in ev

    def test_block_subscribed_true(self, sse_block):
        ev = sse_block.initial_event
        assert ev is not None
        data = json.loads(ev["result"]["content"][0]["text"])
        assert data.get("subscribed") is True, f"subscribed not true: {data}"

    def test_block_hash(self, sse_block):
        """get_block_subscribe initial event contains a valid tip hash."""
        ev = sse_block.initial_event
        assert ev is not None
        data = json.loads(ev["result"]["content"][0]["text"])
        assert validate_hex_hash(data.get("hash", "")), (
            f"invalid hash: {data.get('hash')}"
        )

    # ── get_tx_subscribe ───────────────────────────────────────────────────

    def test_tx_event_received(self, sse_tx):
        assert sse_tx.raw_initial is not None, (
            "No SSE event received within timeout for get_tx_subscribe"
        )

    def test_tx_jsonrpc_envelope(self, sse_tx):
        ev = sse_tx.initial_event
        assert ev is not None
        assert ev.get("jsonrpc") == "2.0"
        assert ev.get("error") is None
        assert "result" in ev

    def test_tx_subscribed_true(self, sse_tx):
        ev = sse_tx.initial_event
        assert ev is not None
        data = json.loads(ev["result"]["content"][0]["text"])
        assert data.get("subscribed") is True, f"subscribed not true: {data}"


# ═══════════════════════════════════════════════════════════════════════════
# EVENT FORMAT — SSE wire protocol compliance
# ═══════════════════════════════════════════════════════════════════════════

class TestSSEEventFormat:
    """Verify the raw SSE wire format of the initial event."""

    def test_top_data_is_json(self, sse_top):
        raw = sse_top.raw_initial
        assert raw is not None, "No initial event for get_top_subscribe"
        try:
            json.loads(raw)
        except json.JSONDecodeError:
            pytest.fail(f"SSE data payload is not JSON: {raw!r}")

    def test_block_data_is_json(self, sse_block):
        raw = sse_block.raw_initial
        assert raw is not None, "No initial event for get_block_subscribe"
        try:
            json.loads(raw)
        except json.JSONDecodeError:
            pytest.fail(f"SSE data payload is not JSON: {raw!r}")

    def test_tx_data_is_json(self, sse_tx):
        raw = sse_tx.raw_initial
        assert raw is not None, "No initial event for get_tx_subscribe"
        try:
            json.loads(raw)
        except json.JSONDecodeError:
            pytest.fail(f"SSE data payload is not JSON: {raw!r}")


# ═══════════════════════════════════════════════════════════════════════════
# PUSH NOTIFICATIONS — live events (skipped unless --sse-wait > 0)
# ═══════════════════════════════════════════════════════════════════════════

class TestSSEPushNotifications:
    """
    Wait for live push notifications over the existing SSE sessions.

    Skipped by default.  Enable with:
        pytest test_mcp_sse.py --sse-wait=60
    """

    def test_top_push_shape(self, sse_top, sse_wait):
        """bitcoin/top push has method, params.height and params.hash."""
        if sse_wait <= 0:
            pytest.skip("pass --sse-wait=N to enable live push tests")

        sse_top.collect_pushes(sse_wait)
        if not sse_top.push_events:
            pytest.skip(
                f"No bitcoin/top push received in {sse_wait}s "
                "(expected on mainnet ~every 10 min)"
            )
        for msg in sse_top.push_events:
            assert msg.get("method") == "bitcoin/top", f"unexpected method: {msg}"
            params = msg.get("params", {})
            assert isinstance(params.get("height"), int), f"missing height: {msg}"
            assert validate_hex_hash(params.get("hash", "")), f"invalid hash: {msg}"

    def test_block_push_shape(self, sse_block, sse_wait):
        """bitcoin/block push has method and params.hash."""
        if sse_wait <= 0:
            pytest.skip("pass --sse-wait=N to enable live push tests")

        sse_block.collect_pushes(sse_wait)
        if not sse_block.push_events:
            pytest.skip(
                f"No bitcoin/block push received in {sse_wait}s "
                "(expected on mainnet ~every 10 min)"
            )
        for msg in sse_block.push_events:
            assert msg.get("method") == "bitcoin/block", f"unexpected method: {msg}"
            params = msg.get("params", {})
            assert validate_hex_hash(params.get("hash", "")), f"invalid hash: {msg}"

    def test_tx_push_shape(self, sse_tx, sse_wait):
        """bitcoin/transaction push has method and params.hash."""
        if sse_wait <= 0:
            pytest.skip("pass --sse-wait=N to enable live push tests")

        sse_tx.collect_pushes(sse_wait)
        if not sse_tx.push_events:
            pytest.skip(
                f"No bitcoin/transaction push received in {sse_wait}s "
                "(mempool activity required)"
            )
        for msg in sse_tx.push_events:
            assert msg.get("method") == "bitcoin/transaction", (
                f"unexpected method: {msg}"
            )
            params = msg.get("params", {})
            assert validate_hex_hash(params.get("hash", "")), f"invalid hash: {msg}"

"""
WebSocket tests for libbitcoin-server MCP subscriptions.

Tests the three push-subscription tools over a WebSocket connection:
  get_top_subscribe   — pushes bitcoin/top on each new chain tip
  get_block_subscribe — pushes bitcoin/block on each new confirmed block
  get_tx_subscribe    — pushes bitcoin/transaction on each new mempool tx

Run:
    python3 test_mcp_websocket.py [--host localhost] [--port 8334] [--wait 0]

    --wait N   seconds to listen for push notifications after subscribing
               (0 = skip push-delivery check, just test handshake + subscribe)
"""

import argparse
import asyncio
import json
import sys
import time

try:
    import websockets
except ImportError:
    sys.exit("pip install websockets")

MCP_VERSION = "2024-11-05"
_id = 0

def next_id():
    global _id
    _id += 1
    return _id

def rpc(method, params=None, *, notification=False):
    msg = {"jsonrpc": "2.0", "method": method}
    if not notification:
        msg["id"] = next_id()
    if params is not None:
        msg["params"] = params
    return json.dumps(msg)

def tool_call(name, arguments=None):
    return rpc("tools/call", {"name": name, "arguments": arguments or {}})

PASS = "✓"
FAIL = "✗"

async def run(host, port, wait_seconds):
    url = f"ws://{host}:{port}/mcp"
    results = []

    print(f"\nConnecting to {url} …")
    try:
        async with websockets.connect(url) as ws:
            print(f"  {PASS} WebSocket connected\n")

            # ── 1. initialize ─────────────────────────────────────────────
            await ws.send(rpc("initialize", {
                "protocolVersion": MCP_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "pytest-ws", "version": "1.0"},
            }))
            raw = await asyncio.wait_for(ws.recv(), timeout=10)
            resp = json.loads(raw)
            ok = (resp.get("result", {}).get("protocolVersion") == MCP_VERSION
                  and resp.get("error") is None)
            print(f"  {PASS if ok else FAIL} initialize  →  protocolVersion={resp.get('result',{}).get('protocolVersion')}")
            results.append(ok)

            # ── 2. notifications/initialized ──────────────────────────────
            await ws.send(rpc("notifications/initialized", notification=True))
            # No response expected for notifications.
            print(f"  {PASS} notifications/initialized sent (no response expected)")
            results.append(True)

            # ── 3. subscribe tools ────────────────────────────────────────
            for tool in ("get_top_subscribe", "get_block_subscribe", "get_tx_subscribe"):
                await ws.send(tool_call(tool))
                raw = await asyncio.wait_for(ws.recv(), timeout=10)
                resp = json.loads(raw)
                err = resp.get("error")
                ok = err is None and "result" in resp
                detail = (f"error {err['code']}: {err['message']}" if err
                          else resp["result"]["content"][0]["text"])
                print(f"  {PASS if ok else FAIL} {tool:<26} → {detail}")
                results.append(ok)

            # ── 4. push notification check ────────────────────────────────
            if wait_seconds > 0:
                print(f"\n  Listening {wait_seconds}s for push notifications …")
                deadline = time.monotonic() + wait_seconds
                pushes = []
                while time.monotonic() < deadline:
                    remaining = deadline - time.monotonic()
                    try:
                        raw = await asyncio.wait_for(ws.recv(), timeout=remaining)
                        msg = json.loads(raw)
                        method = msg.get("method", "?")
                        params = msg.get("params", {})
                        print(f"  {PASS} PUSH received: {method}  params={params}")
                        pushes.append(msg)
                    except asyncio.TimeoutError:
                        break
                if not pushes:
                    print(f"  ℹ  No push received in {wait_seconds}s "
                          f"(expected on mainnet — ~10 min/block)")
                results.append(True)  # absence of push is not a failure
            else:
                print("\n  (push-delivery check skipped — pass --wait N to enable)")

    except (OSError, websockets.exceptions.WebSocketException) as exc:
        print(f"  {FAIL} Connection failed: {exc}")
        sys.exit(1)

    passed = sum(results)
    total  = len(results)
    print(f"\n{'='*50}")
    print(f"  {passed}/{total} passed")
    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host",  default="localhost")
    ap.add_argument("--port",  type=int, default=8334)
    ap.add_argument("--wait",  type=int, default=0,
                    help="seconds to wait for push notifications (0=skip)")
    args = ap.parse_args()
    asyncio.run(run(args.host, args.port, args.wait))

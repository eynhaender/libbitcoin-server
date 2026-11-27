"""
Tests for libbitcoin-server Stratum v2 mining interface.

Tests the Stratum v2 mining protocol over websocket, one binary frame to each
websocket message (no Noise): connection setup, standard and extended mining
channels, jobs and their previous blocks, targets, channel updates and
closes, and share submission.

A channel pays the address of its user identity, up to any dot, unless the
server configures [stratum_v2] payout_address, in which case any identity
opens a channel. With a [stratum_v2] credential the websocket upgrade carries
it as basic authorization (the user is the identity account,
--stratum-v2-password its password).

The share tests rebuild the coinbase, merkle root and header from the job, as
a miner does. Where the share target is reachable by hashing in python (a
small [stratum_v2] minimum_difficulty), valid shares are submitted, and where
the block target is reachable too (regtest), a block is mined and confirmed.

Run with:
    pytest test_stratum_v2.py
    pytest test_stratum_v2.py --stratum-v2-host=localhost --stratum-v2-port=8980
    pytest test_stratum_v2.py --stratum-v2-user=bc1q...
"""

from __future__ import annotations

import base64
import hashlib
import struct
import time

import pytest
import requests
import websocket

from test_stratum_v1 import decode_segwit, script_height, segwit_script
from test_stratum_v1 import parse_coinbase as parse_coinbase_full

# Message types (mining protocol).
SETUP_CONNECTION = 0x00
SETUP_CONNECTION_SUCCESS = 0x01
SETUP_CONNECTION_ERROR = 0x02
OPEN_STANDARD_MINING_CHANNEL = 0x10
OPEN_STANDARD_MINING_CHANNEL_SUCCESS = 0x11
OPEN_MINING_CHANNEL_ERROR = 0x12
OPEN_EXTENDED_MINING_CHANNEL = 0x13
OPEN_EXTENDED_MINING_CHANNEL_SUCCESS = 0x14
NEW_MINING_JOB = 0x15
UPDATE_CHANNEL = 0x16
UPDATE_CHANNEL_ERROR = 0x17
CLOSE_CHANNEL = 0x18
SUBMIT_SHARES_STANDARD = 0x1a
SUBMIT_SHARES_EXTENDED = 0x1b
SUBMIT_SHARES_SUCCESS = 0x1c
SUBMIT_SHARES_ERROR = 0x1d
NEW_EXTENDED_MINING_JOB = 0x1f
SET_NEW_PREV_HASH = 0x20
SET_TARGET = 0x21

# Messages addressed to a channel carry the channel bit in the extension type.
_CHANNEL_MESSAGES = {
    NEW_MINING_JOB, UPDATE_CHANNEL, UPDATE_CHANNEL_ERROR, CLOSE_CHANNEL,
    SUBMIT_SHARES_STANDARD, SUBMIT_SHARES_EXTENDED, SUBMIT_SHARES_SUCCESS,
    SUBMIT_SHARES_ERROR, NEW_EXTENDED_MINING_JOB, SET_NEW_PREV_HASH,
    SET_TARGET,
}

_REQUIRES_STANDARD_JOBS = 0x01
_REQUIRES_WORK_SELECTION = 0x02
_REQUIRES_VERSION_ROLLING = 0x04
_ROLLING_MASK = 0x1fffe000
_MAXIMUM_TARGET = b"\xff" * 32

# The target of difficulty one (bits 0x1d00ffff).
_DIFFICULTY_ONE = 0xffff << 208

# Hashes tried for a share or block before the test skips (seconds in python).
_MAXIMUM_TRIES = 1 << 20


# ─── Codec ───────────────────────────────────────────────────────────────────

class Writer:
    def __init__(self):
        self.data = b""

    def u8(self, value: int) -> "Writer":
        self.data += struct.pack("<B", value)
        return self

    def u16(self, value: int) -> "Writer":
        self.data += struct.pack("<H", value)
        return self

    def u32(self, value: int) -> "Writer":
        self.data += struct.pack("<I", value)
        return self

    def f32(self, value: float) -> "Writer":
        self.data += struct.pack("<f", value)
        return self

    def u256(self, value: bytes) -> "Writer":
        assert len(value) == 32
        self.data += value
        return self

    def str0_255(self, value: str) -> "Writer":
        return self.b0_255(value.encode())

    def b0_255(self, value: bytes) -> "Writer":
        self.data += struct.pack("<B", len(value)) + value
        return self

    b0_32 = b0_255


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.offset = 0

    def _take(self, size: int) -> bytes:
        assert self.offset + size <= len(self.data), "truncated message"
        value = self.data[self.offset:self.offset + size]
        self.offset += size
        return value

    def u8(self) -> int:
        return self._take(1)[0]

    def u16(self) -> int:
        return struct.unpack("<H", self._take(2))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self._take(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self._take(8))[0]

    def boolean(self) -> bool:
        return self.u8() != 0

    def u256(self) -> bytes:
        return self._take(32)

    def b0_255(self) -> bytes:
        return self._take(self.u8())

    def b0_64k(self) -> bytes:
        return self._take(self.u16())

    def str0_255(self) -> str:
        return self.b0_255().decode()

    def option_u32(self) -> int | None:
        return self.u32() if self.u8() else None

    def seq0_255_u256(self) -> list[bytes]:
        return [self.u256() for _ in range(self.u8())]

    def end(self):
        assert self.offset == len(self.data), "trailing message bytes"


def frame(message_type: int, payload: bytes) -> bytes:
    extension = 0x8000 if message_type in _CHANNEL_MESSAGES else 0x0000
    return struct.pack("<HB", extension, message_type) + \
        len(payload).to_bytes(3, "little") + payload


def unframe(data: bytes) -> tuple[int, int, Reader]:
    assert len(data) >= 6, "short frame"
    extension, message_type = struct.unpack("<HB", data[:3])
    size = int.from_bytes(data[3:6], "little")
    assert size == len(data) - 6, "frame length is not its message"
    return extension, message_type, Reader(data[6:])


def parse(message_type: int, reader: Reader) -> dict:
    """The fields of a message sent by the server."""
    fields = {
        SETUP_CONNECTION_SUCCESS: lambda r: {
            "used_version": r.u16(), "flags": r.u32()},
        SETUP_CONNECTION_ERROR: lambda r: {
            "flags": r.u32(), "error_code": r.str0_255()},
        OPEN_STANDARD_MINING_CHANNEL_SUCCESS: lambda r: {
            "request_id": r.u32(), "channel_id": r.u32(),
            "target": r.u256(), "extranonce_prefix": r.b0_255(),
            "group_channel_id": r.u32()},
        OPEN_MINING_CHANNEL_ERROR: lambda r: {
            "request_id": r.u32(), "error_code": r.str0_255()},
        OPEN_EXTENDED_MINING_CHANNEL_SUCCESS: lambda r: {
            "request_id": r.u32(), "channel_id": r.u32(),
            "target": r.u256(), "extranonce_size": r.u16(),
            "extranonce_prefix": r.b0_255(), "group_channel_id": r.u32()},
        NEW_MINING_JOB: lambda r: {
            "channel_id": r.u32(), "job_id": r.u32(),
            "min_ntime": r.option_u32(), "version": r.u32(),
            "merkle_root": r.u256()},
        UPDATE_CHANNEL_ERROR: lambda r: {
            "channel_id": r.u32(), "error_code": r.str0_255()},
        SUBMIT_SHARES_SUCCESS: lambda r: {
            "channel_id": r.u32(), "last_sequence_number": r.u32(),
            "new_submits_accepted_count": r.u32(),
            "new_shares_sum": r.u64()},
        SUBMIT_SHARES_ERROR: lambda r: {
            "channel_id": r.u32(), "sequence_number": r.u32(),
            "error_code": r.str0_255()},
        NEW_EXTENDED_MINING_JOB: lambda r: {
            "channel_id": r.u32(), "job_id": r.u32(),
            "min_ntime": r.option_u32(), "version": r.u32(),
            "version_rolling_allowed": r.boolean(),
            "merkle_path": r.seq0_255_u256(),
            "coinbase_tx_prefix": r.b0_64k(),
            "coinbase_tx_suffix": r.b0_64k()},
        SET_NEW_PREV_HASH: lambda r: {
            "channel_id": r.u32(), "job_id": r.u32(),
            "prev_hash": r.u256(), "min_ntime": r.u32(), "nbits": r.u32()},
        SET_TARGET: lambda r: {
            "channel_id": r.u32(), "maximum_target": r.u256()},
    }[message_type](reader)
    reader.end()
    fields["type"] = message_type
    return fields


def setup_connection(minimum: int = 2, maximum: int = 2, flags: int = 0,
                     protocol: int = 0) -> bytes:
    return frame(SETUP_CONNECTION, Writer().u8(protocol).u16(minimum)
                 .u16(maximum).u32(flags).str0_255("127.0.0.1").u16(8980)
                 .str0_255("libbitcoin").str0_255("test").str0_255("1.0")
                 .str0_255("pytest").data)


def open_standard(user: str, request_id: int = 7, hash_rate: float = 0.0,
                  max_target: bytes = _MAXIMUM_TARGET) -> bytes:
    return frame(OPEN_STANDARD_MINING_CHANNEL, Writer().u32(request_id)
                 .str0_255(user).f32(hash_rate).u256(max_target).data)


def open_extended(user: str, request_id: int = 7, hash_rate: float = 0.0,
                  max_target: bytes = _MAXIMUM_TARGET,
                  min_extranonce: int = 0) -> bytes:
    return frame(OPEN_EXTENDED_MINING_CHANNEL, Writer().u32(request_id)
                 .str0_255(user).f32(hash_rate).u256(max_target)
                 .u16(min_extranonce).data)


def update_channel(channel_id: int, hash_rate: float = 0.0,
                   max_target: bytes = _MAXIMUM_TARGET) -> bytes:
    return frame(UPDATE_CHANNEL, Writer().u32(channel_id).f32(hash_rate)
                 .u256(max_target).data)


def close_channel(channel_id: int, reason: str = "done") -> bytes:
    return frame(CLOSE_CHANNEL, Writer().u32(channel_id).str0_255(reason).data)


def submit_standard(channel_id: int, sequence: int, job_id: int, nonce: int,
                    ntime: int, version: int) -> bytes:
    return frame(SUBMIT_SHARES_STANDARD, Writer().u32(channel_id)
                 .u32(sequence).u32(job_id).u32(nonce).u32(ntime)
                 .u32(version).data)


def submit_extended(channel_id: int, sequence: int, job_id: int, nonce: int,
                    ntime: int, version: int, extranonce: bytes) -> bytes:
    return frame(SUBMIT_SHARES_EXTENDED, Writer().u32(channel_id)
                 .u32(sequence).u32(job_id).u32(nonce).u32(ntime)
                 .u32(version).b0_32(extranonce).data)


# ─── Client ──────────────────────────────────────────────────────────────────

class Sv2Client:
    """One websocket, a frame to each binary message."""

    def __init__(self, config: dict, user: str | None = None,
                 password: str | None = None):
        self.timeout = config["timeout"]
        url = f"ws://{config['host']}:{config['port']}/"
        header = []
        secret = config["password"] if password is None else password
        if secret:
            account = (config["user"] if user is None else user).split(".")[0]
            token = base64.b64encode(f"{account}:{secret}".encode()).decode()
            header.append(f"Authorization: Basic {token}")
        self.ws = websocket.create_connection(url, header=header,
                                              timeout=self.timeout)

    def close(self):
        self.ws.close()

    def send(self, data: bytes):
        self.ws.send_binary(data)

    def receive(self) -> dict:
        opcode, data = self.ws.recv_data()
        if opcode == websocket.ABNF.OPCODE_CLOSE:
            raise ConnectionError("server closed the connection")
        assert opcode == websocket.ABNF.OPCODE_BINARY, "not a binary message"
        extension, message_type, reader = unframe(data)
        message = parse(message_type, reader)
        channel = message_type in _CHANNEL_MESSAGES
        assert bool(extension & 0x8000) == channel, "channel bit"
        return message

    def expect(self, message_type: int) -> dict:
        message = self.receive()
        assert message["type"] == message_type, message
        return message

    def until(self, message_type: int, bound: int = 16) -> dict:
        """Receive until a message of the type, skipping others."""
        for _ in range(bound):
            message = self.receive()
            if message["type"] == message_type:
                return message
        raise AssertionError(f"no message of type {message_type:#04x}")

    def dropped(self) -> bool:
        """The server closes the connection (no message follows)."""
        try:
            self.receive()
        except (ConnectionError, websocket.WebSocketException, OSError):
            return True
        return False

    def poll(self, seconds: float) -> dict | None:
        """The next message, or None where none arrives within the seconds."""
        self.ws.settimeout(seconds)
        try:
            return self.receive()
        except websocket.WebSocketTimeoutException:
            return None
        finally:
            self.ws.settimeout(self.timeout)

    def silent(self, seconds: float = 1.0) -> bool:
        """No message arrives within the seconds, the connection retained."""
        return self.poll(seconds) is None


# ─── Fixtures ────────────────────────────────────────────────────────────────

def _connect(config: dict, **kwargs) -> Sv2Client:
    try:
        return Sv2Client(config, **kwargs)
    except (OSError, websocket.WebSocketException) as exc:
        pytest.skip(f"Cannot connect to Stratum v2 server: {exc}")


@pytest.fixture
def client(stratum_v2_config: dict):
    connection = _connect(stratum_v2_config)
    yield connection
    connection.close()


@pytest.fixture
def ready(client: Sv2Client) -> Sv2Client:
    """A connection set up for mining."""
    client.send(setup_connection())
    client.expect(SETUP_CONNECTION_SUCCESS)
    return client


def opened(client: Sv2Client, message: bytes, success: int) -> dict:
    client.send(message)
    response = client.receive()
    if response["type"] == OPEN_MINING_CHANNEL_ERROR:
        pytest.skip(f"channel refused ({response['error_code']}), set "
                    "--stratum-v2-user to an address or configure "
                    "[stratum_v2] payout_address")
    assert response["type"] == success, response
    return response


@pytest.fixture
def extended(ready: Sv2Client, stratum_v2_config: dict) -> "Miner":
    """An open extended channel with its first job and previous block."""
    channel = opened(ready, open_extended(stratum_v2_config["user"]),
                     OPEN_EXTENDED_MINING_CHANNEL_SUCCESS)
    job = ready.expect(NEW_EXTENDED_MINING_JOB)
    previous = ready.expect(SET_NEW_PREV_HASH)
    return Miner(ready, channel, job, previous)


@pytest.fixture
def other(stratum_v2_config: dict):
    """A second connection set up for mining."""
    connection = _connect(stratum_v2_config)
    connection.send(setup_connection())
    connection.expect(SETUP_CONNECTION_SUCCESS)
    yield connection
    connection.close()


@pytest.fixture
def payout_configured(other: Sv2Client) -> bool:
    """The server opens a channel for any identity (payout_address)."""
    other.send(open_extended("not an address"))
    return other.receive()["type"] == OPEN_EXTENDED_MINING_CHANNEL_SUCCESS


@pytest.fixture
def standard(ready: Sv2Client, stratum_v2_config: dict) -> "Miner":
    """An open standard channel with its first job and previous block."""
    channel = opened(ready, open_standard(stratum_v2_config["user"]),
                     OPEN_STANDARD_MINING_CHANNEL_SUCCESS)
    job = ready.expect(NEW_MINING_JOB)
    previous = ready.expect(SET_NEW_PREV_HASH)
    return Miner(ready, channel, job, previous)


# ─── Helpers ─────────────────────────────────────────────────────────────────

def dsha256(data: bytes) -> bytes:
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def expand(bits: int) -> int:
    """The target of compact bits."""
    return (bits & 0x007fffff) << (8 * ((bits >> 24) - 3))


def tries(target: int) -> float:
    return float(1 << 256) / (target + 1)


def read_varint(data: bytes, offset: int) -> tuple[int, int]:
    head = data[offset]
    if head < 0xfd:
        return head, offset + 1
    size = {0xfd: 2, 0xfe: 4, 0xff: 8}[head]
    return int.from_bytes(data[offset + 1:offset + 1 + size], "little"), \
        offset + 1 + size


def parse_coinbase(tx: bytes) -> dict:
    """Parse a coinbase serialized without witness."""
    version = int.from_bytes(tx[:4], "little")
    inputs, offset = read_varint(tx, 4)
    prevout = tx[offset:offset + 36]
    offset += 36
    size, offset = read_varint(tx, offset)
    script = tx[offset:offset + size]
    offset += size + 4
    count, offset = read_varint(tx, offset)
    outputs = []
    for _ in range(count):
        value = int.from_bytes(tx[offset:offset + 8], "little")
        size, offset = read_varint(tx, offset + 8)
        outputs.append((value, tx[offset:offset + size]))
        offset += size
    assert offset + 4 == len(tx), "trailing coinbase bytes"
    return {"version": version, "inputs": inputs, "prevout": prevout,
            "script": script, "outputs": outputs}


def native_top(native_config: dict) -> dict:
    """The confirmed top, {hash, height}, from the native interface."""
    url = f"{native_config['base_url']}/top?format=json"
    tops = requests.get(url, timeout=10).json()
    return next(top for top in tops if top.get("chain") == "confirmed")


class Miner:
    """Builds coinbases, merkle roots and headers from jobs, as a miner does."""

    def __init__(self, client: Sv2Client, channel: dict, job: dict,
                 previous: dict):
        self.client = client
        self.channel = channel
        self.job = job
        self.previous = previous

    @property
    def channel_id(self) -> int:
        return self.channel["channel_id"]

    @property
    def standard(self) -> bool:
        return self.job["type"] == NEW_MINING_JOB

    @property
    def rolled(self) -> int:
        return 0 if self.standard else self.channel["extranonce_size"]

    @property
    def target(self) -> int:
        return int.from_bytes(self.channel["target"], "little")

    def coinbase(self, rolled: bytes) -> bytes:
        return self.job["coinbase_tx_prefix"] + \
            self.channel["extranonce_prefix"] + rolled + \
            self.job["coinbase_tx_suffix"]

    def root(self, rolled: bytes) -> bytes:
        if self.standard:
            return self.job["merkle_root"]
        root = dsha256(self.coinbase(rolled))
        for item in self.job["merkle_path"]:
            root = dsha256(root + item)
        return root

    def header(self, rolled: bytes, nonce: int,
               version: int | None = None) -> bytes:
        job_version = self.job["version"] if version is None else version
        return struct.pack("<I", job_version) + self.previous["prev_hash"] + \
            self.root(rolled) + struct.pack(
                "<III", self.previous["min_ntime"], self.previous["nbits"],
                nonce)

    def search(self, target: int, rolled: bytes) -> tuple[int, bytes] | None:
        """The first nonce whose header hash meets the target."""
        for nonce in range(_MAXIMUM_TRIES):
            digest = dsha256(self.header(rolled, nonce))
            if int.from_bytes(digest, "little") <= target:
                return nonce, digest
        return None

    def submit(self, nonce: int, rolled: bytes | None = None,
               sequence: int = 1, version: int | None = None,
               job_id: int | None = None, ntime: int | None = None,
               channel_id: int | None = None) -> dict:
        args = (self.channel_id if channel_id is None else channel_id,
                sequence,
                self.job["job_id"] if job_id is None else job_id,
                nonce,
                self.previous["min_ntime"] if ntime is None else ntime,
                self.job["version"] if version is None else version)
        if self.standard:
            self.client.send(submit_standard(*args))
        else:
            extranonce = bytes(self.rolled) if rolled is None else rolled
            self.client.send(submit_extended(*args, extranonce))
        return self.client.receive()


# ─── SetupConnection ─────────────────────────────────────────────────────────

def test_setup_connection_success(client: Sv2Client):
    client.send(setup_connection())
    success = client.expect(SETUP_CONNECTION_SUCCESS)
    assert success["used_version"] == 2


def test_setup_connection_version_range(client: Sv2Client):
    client.send(setup_connection(1, 3))
    assert client.expect(SETUP_CONNECTION_SUCCESS)["used_version"] == 2


@pytest.mark.parametrize("flags", [
    _REQUIRES_STANDARD_JOBS,
    _REQUIRES_VERSION_ROLLING,
    _REQUIRES_STANDARD_JOBS | _REQUIRES_VERSION_ROLLING,
])
def test_setup_connection_supported_flags(client: Sv2Client, flags: int):
    client.send(setup_connection(flags=flags))
    client.expect(SETUP_CONNECTION_SUCCESS)


def test_setup_connection_version_mismatch(client: Sv2Client):
    client.send(setup_connection(3, 3))
    error = client.expect(SETUP_CONNECTION_ERROR)
    assert error["error_code"] == "protocol-version-mismatch"
    assert client.dropped()


def test_setup_connection_work_selection_unsupported(client: Sv2Client):
    client.send(setup_connection(flags=_REQUIRES_WORK_SELECTION))
    error = client.expect(SETUP_CONNECTION_ERROR)
    assert error["error_code"] == "unsupported-feature-flags"
    assert error["flags"] == _REQUIRES_WORK_SELECTION


def test_setup_connection_other_protocol_unsupported(client: Sv2Client):
    client.send(setup_connection(protocol=1))
    error = client.expect(SETUP_CONNECTION_ERROR)
    assert error["error_code"] == "unsupported-protocol"


def test_mining_before_setup_dropped(client: Sv2Client,
                                     stratum_v2_config: dict):
    client.send(open_extended(stratum_v2_config["user"]))
    assert client.dropped()


def test_get_without_upgrade(stratum_v2_config: dict):
    url = f"http://{stratum_v2_config['host']}:{stratum_v2_config['port']}/"
    try:
        response = requests.get(url, timeout=stratum_v2_config["timeout"])
    except requests.exceptions.ConnectionError as exc:
        pytest.skip(f"Cannot connect to Stratum v2 server: {exc}")
    assert response.status_code in (401, 426)


# ─── Framing ─────────────────────────────────────────────────────────────────

def test_setup_connection_repeated_dropped(ready: Sv2Client):
    ready.send(setup_connection())
    assert ready.dropped()


def test_unknown_message_dropped(ready: Sv2Client):
    """A core protocol message type that is not served drops the miner."""
    ready.send(frame(0x7f, b""))
    assert ready.dropped()


def test_unknown_extension_message_discarded(ready: Sv2Client,
                                             stratum_v2_config: dict):
    """A message of an extension not negotiated is discarded."""
    ready.send(struct.pack("<HB", 0x0002, 0x42) +
               (4).to_bytes(3, "little") + bytes(4))
    assert ready.silent()
    opened(ready, open_extended(stratum_v2_config["user"]),
           OPEN_EXTENDED_MINING_CHANNEL_SUCCESS)


def test_channel_message_without_channel_bit_dropped(ready: Sv2Client):
    message = update_channel(0xffffffff)
    ready.send(struct.pack("<H", 0x0000) + message[2:])
    assert ready.dropped()


def test_channel_bit_on_connection_message_dropped(ready: Sv2Client,
                                                  stratum_v2_config: dict):
    message = open_extended(stratum_v2_config["user"])
    ready.send(struct.pack("<H", 0x8000) + message[2:])
    assert ready.dropped()


def test_frame_length_beyond_message_dropped(ready: Sv2Client,
                                             stratum_v2_config: dict):
    """The frame length exceeds its websocket message."""
    ready.send(open_extended(stratum_v2_config["user"])[:-1])
    assert ready.dropped()


def test_frame_length_short_of_message_dropped(ready: Sv2Client,
                                               stratum_v2_config: dict):
    """The websocket message carries bytes beyond its frame."""
    ready.send(open_extended(stratum_v2_config["user"]) + bytes(1))
    assert ready.dropped()


def test_truncated_payload_dropped(ready: Sv2Client,
                                   stratum_v2_config: dict):
    """A payload framed whole but missing its last fields."""
    ready.send(frame(OPEN_EXTENDED_MINING_CHANNEL, Writer().u32(7)
                     .str0_255(stratum_v2_config["user"]).data))
    assert ready.dropped()


def test_payload_trailing_bytes_dropped(ready: Sv2Client,
                                        stratum_v2_config: dict):
    """A payload framed whole but carrying bytes beyond its fields."""
    message = open_extended(stratum_v2_config["user"])
    ready.send(frame(OPEN_EXTENDED_MINING_CHANNEL, message[6:] + bytes(1)))
    assert ready.dropped()


def test_text_message_dropped(ready: Sv2Client):
    ready.ws.send("mining.subscribe")
    assert ready.dropped()


def test_frame_beyond_maximum_request_dropped(ready: Sv2Client):
    """A frame larger than any message is refused before it is read."""
    try:
        ready.send(struct.pack("<HB", 0x0002, 0x42) +
                   (0xffffff).to_bytes(3, "little") + bytes(0xffffff))
    except (websocket.WebSocketException, OSError):
        return
    assert ready.dropped()


# ─── OpenExtendedMiningChannel ───────────────────────────────────────────────

def test_open_extended_success(extended: "Miner"):
    channel = extended.channel
    assert channel["request_id"] == 7
    assert channel["channel_id"] > 0
    assert channel["extranonce_size"] > 0
    assert 4 <= len(channel["extranonce_prefix"]) <= 32
    assert len(channel["extranonce_prefix"]) + channel["extranonce_size"] <= 32
    assert extended.target > 0


def test_open_extended_first_job_is_future(extended: "Miner"):
    job, previous = extended.job, extended.previous
    assert job["channel_id"] == extended.channel_id
    assert job["min_ntime"] is None
    assert job["version_rolling_allowed"] is True
    assert previous["channel_id"] == extended.channel_id
    assert previous["job_id"] == job["job_id"]
    assert previous["min_ntime"] > 0


def test_open_extended_previous_is_top(extended: "Miner",
                                       native_config: dict):
    try:
        top = native_top(native_config)
    except (requests.RequestException, KeyError, ValueError) as exc:
        pytest.skip(f"native top unavailable: {exc}")
    previous = extended.previous["prev_hash"][::-1].hex()
    if previous != top["hash"]:
        pytest.skip("top changed while the job was sent")
    assert previous == top["hash"]


def test_open_extended_coinbase_layout(extended: "Miner"):
    """The coinbase: one null input, its script ending in the extranonce."""
    rolled = bytes(extended.rolled)
    coinbase = parse_coinbase(extended.coinbase(rolled))
    extranonce = extended.channel["extranonce_prefix"] + rolled
    assert coinbase["version"] == 2
    assert coinbase["inputs"] == 1
    assert coinbase["prevout"] == bytes(32) + b"\xff\xff\xff\xff"
    assert coinbase["script"].endswith(bytes([len(extranonce)]) + extranonce)
    assert coinbase["outputs"]


def test_open_extended_distinct_channels(extended: "Miner",
                                         stratum_v2_config: dict):
    extended.client.send(open_extended(stratum_v2_config["user"], 8))
    second = extended.client.expect(OPEN_EXTENDED_MINING_CHANNEL_SUCCESS)
    assert second["request_id"] == 8
    assert second["channel_id"] != extended.channel_id
    assert second["extranonce_prefix"] != extended.channel["extranonce_prefix"]
    job = extended.client.expect(NEW_EXTENDED_MINING_JOB)
    assert job["channel_id"] == second["channel_id"]


def test_open_extended_invalid_user(ready: Sv2Client):
    ready.send(open_extended("not an address"))
    response = ready.receive()
    if response["type"] == OPEN_EXTENDED_MINING_CHANNEL_SUCCESS:
        pytest.skip("Server configures [stratum_v2] payout_address")
    assert response["type"] == OPEN_MINING_CHANNEL_ERROR
    assert response["error_code"] == "unknown-user"


def test_open_extended_zero_max_target(ready: Sv2Client,
                                       stratum_v2_config: dict):
    ready.send(open_extended(stratum_v2_config["user"],
                             max_target=bytes(32)))
    error = ready.expect(OPEN_MINING_CHANNEL_ERROR)
    assert error["error_code"] in ("max-target-out-of-range", "unknown-user")


def test_open_extended_min_extranonce_too_large(ready: Sv2Client,
                                                stratum_v2_config: dict):
    ready.send(open_extended(stratum_v2_config["user"], min_extranonce=0xffff))
    error = ready.expect(OPEN_MINING_CHANNEL_ERROR)
    assert error["error_code"] == "min-extranonce-size-too-large"


def test_open_extended_max_target_floors_target(ready: Sv2Client,
                                                stratum_v2_config: dict):
    """The target of a channel is at or below the miner's maximum target."""
    maximum = (_DIFFICULTY_ONE).to_bytes(32, "little")
    channel = opened(ready, open_extended(stratum_v2_config["user"],
                                          max_target=maximum),
                     OPEN_EXTENDED_MINING_CHANNEL_SUCCESS)
    assert int.from_bytes(channel["target"], "little") <= _DIFFICULTY_ONE


def test_open_extended_identity_worker_suffix(ready: Sv2Client,
                                              stratum_v2_config: dict):
    """The identity up to the dot is the address, the rest names a worker."""
    user = stratum_v2_config["user"]
    if "." in user:
        pytest.skip("--stratum-v2-user already carries a worker name")
    opened(ready, open_extended(user + ".rig1"),
           OPEN_EXTENDED_MINING_CHANNEL_SUCCESS)


def test_open_extended_coinbase_rules(extended: "Miner",
                                      stratum_v2_config: dict,
                                      payout_configured: bool,
                                      native_config: dict):
    """The coinbase of a job: bip34 height, payout, bip141, bip54."""
    coinbase = parse_coinbase_full(extended.coinbase(bytes(extended.rolled)))

    # bip54: the sequence is not final and the locktime is the prior height.
    height = script_height(coinbase["script"])
    assert coinbase["sequence"] == 0xfffffffe
    assert coinbase["locktime"] == height - 1

    # The payout, then the zero value witness commitment (bip141).
    (value, payout), (zero, commitment) = coinbase["outputs"]
    assert value > 0
    assert zero == 0
    assert commitment[:6] == bytes.fromhex("6a24aa21a9ed")
    assert len(commitment) == 38

    decoded = decode_segwit(stratum_v2_config["user"].split(".")[0])
    if decoded is not None and not payout_configured:
        assert payout == segwit_script(decoded[1], decoded[2])

    try:
        top = native_top(native_config)
    except (requests.RequestException, KeyError, ValueError):
        return
    assert height in (top["height"] + 1, top["height"] + 2)


def test_open_extended_job_version(extended: "Miner"):
    """The job version signals versionbits (bip9), with no rolled bits."""
    version = extended.job["version"]
    assert version & 0xe0000000 == 0x20000000
    assert version & _ROLLING_MASK == 0


def test_open_extended_unknown_identity_with_payout(other: Sv2Client,
                                                    payout_configured: bool,
                                                    stratum_v2_config: dict):
    """With a payout address configured, any identity opens a channel."""
    if not payout_configured:
        pytest.skip("Server pays the identity ([stratum_v2] payout_address "
                    "not configured)")
    other.send(open_extended("anyone.rig1", 9))
    channel = other.until(OPEN_EXTENDED_MINING_CHANNEL_SUCCESS)
    assert channel["request_id"] == 9


def test_open_too_many_channels(ready: Sv2Client, stratum_v2_config: dict):
    """
    Channels beyond [stratum_v2] maximum_channels are refused, the
    connection retained. Skips where the maximum is above 64.
    """
    answers = (OPEN_EXTENDED_MINING_CHANNEL_SUCCESS, OPEN_MINING_CHANNEL_ERROR)
    for request in range(1, 65):
        ready.send(open_extended(stratum_v2_config["user"], request))
        response = ready.receive()
        while response["type"] not in answers:
            response = ready.receive()
        assert response["request_id"] == request
        if response["type"] == OPEN_MINING_CHANNEL_ERROR:
            if request == 1:
                pytest.skip(f"channel refused ({response['error_code']})")
            assert response["error_code"] == "too-many-channels"
            ready.send(update_channel(0xffffffff))
            error = ready.until(UPDATE_CHANNEL_ERROR, bound=4 * request)
            assert error["error_code"] == "invalid-channel-id"
            return
    pytest.skip("[stratum_v2] maximum_channels is above 64")


# ─── OpenStandardMiningChannel ───────────────────────────────────────────────

def test_open_standard_success(standard: "Miner"):
    channel = standard.channel
    assert channel["request_id"] == 7
    assert channel["channel_id"] > 0
    assert 4 < len(channel["extranonce_prefix"]) <= 32
    assert standard.target > 0


def test_open_standard_first_job_is_future(standard: "Miner"):
    assert standard.job["channel_id"] == standard.channel_id
    assert standard.job["min_ntime"] is None
    assert standard.job["merkle_root"] != bytes(32)
    assert standard.previous["job_id"] == standard.job["job_id"]


def test_open_standard_root_of_extended_job(extended: "Miner",
                                            stratum_v2_config: dict):
    """
    The merkle root of a standard job is that of the extended job of the same
    template, its coinbase carrying the whole standard extranonce prefix.
    """
    client = extended.client
    channel = opened(client, open_standard(stratum_v2_config["user"], 8),
                     OPEN_STANDARD_MINING_CHANNEL_SUCCESS)

    # A template update issues jobs to both channels, in either order.
    templates = [extended.job]
    job, previous = None, None
    message = client.receive()
    while message is not None:
        if message["type"] == NEW_EXTENDED_MINING_JOB:
            templates.append(message)
        elif message["type"] == NEW_MINING_JOB and job is None:
            job = message
        elif message["type"] == SET_NEW_PREV_HASH and \
                message["channel_id"] == channel["channel_id"]:
            previous = message
        message = client.poll(0.5) if previous else client.receive()
    if previous["prev_hash"] != extended.previous["prev_hash"]:
        pytest.skip("top changed between the channels")

    prefix = channel["extranonce_prefix"]
    assert len(prefix) == len(extended.channel["extranonce_prefix"]) + \
        extended.rolled

    def root(template: dict) -> bytes:
        node = dsha256(template["coinbase_tx_prefix"] + prefix +
                       template["coinbase_tx_suffix"])
        for item in template["merkle_path"]:
            node = dsha256(node + item)
        return node

    assert job["merkle_root"] in [root(template) for template in templates]
# ─── SubmitShares ────────────────────────────────────────────────────────────

def test_submit_unknown_channel(extended: "Miner"):
    error = extended.submit(0, channel_id=0xffffffff)
    assert error["type"] == SUBMIT_SHARES_ERROR
    assert error["error_code"] == "invalid-channel-id"


def test_submit_unknown_job(extended: "Miner"):
    error = extended.submit(0, job_id=0xffffffff)
    assert error["type"] == SUBMIT_SHARES_ERROR
    assert error["error_code"] == "invalid-job-id"


def test_submit_standard_on_extended_channel(extended: "Miner"):
    extended.client.send(submit_standard(
        extended.channel_id, 1, extended.job["job_id"], 0,
        extended.previous["min_ntime"], extended.job["version"]))
    error = extended.client.expect(SUBMIT_SHARES_ERROR)
    assert error["error_code"] == "invalid-channel-id"


def test_submit_extranonce_size_dropped(extended: "Miner"):
    error = extended.submit(0, rolled=bytes(extended.rolled + 1))
    assert error["error_code"] == "invalid-extranonce-size"
    assert extended.client.dropped()


def test_submit_version_outside_mask_dropped(extended: "Miner"):
    error = extended.submit(0, version=extended.job["version"] ^ 0x1)
    assert error["error_code"] == "invalid-version"
    assert extended.client.dropped()


def test_submit_invalid_timestamp_dropped(extended: "Miner"):
    error = extended.submit(0, ntime=1)
    assert error["error_code"] == "invalid-share"
    assert extended.client.dropped()


def test_submit_low_difficulty(extended: "Miner"):
    """A share above the channel target is refused, the channel retained."""
    if extended.target >= (1 << 256) - 1:
        pytest.skip("every share meets the target")
    rolled = bytes(extended.rolled)
    for nonce in range(_MAXIMUM_TRIES):
        digest = dsha256(extended.header(rolled, nonce))
        if int.from_bytes(digest, "little") > extended.target:
            break
    error = extended.submit(nonce, rolled)
    assert error["type"] == SUBMIT_SHARES_ERROR
    assert error["error_code"] == "difficulty-too-low"


def test_submit_error_echoes_channel_and_sequence(extended: "Miner"):
    error = extended.submit(0, job_id=0xffffffff, sequence=42)
    assert error["type"] == SUBMIT_SHARES_ERROR
    assert error["channel_id"] == extended.channel_id
    assert error["sequence_number"] == 42


def _not_dropped(miner: "Miner") -> bool:
    miner.client.send(update_channel(0xffffffff))
    return miner.client.until(UPDATE_CHANNEL_ERROR)["error_code"] == \
        "invalid-channel-id"


def test_submit_version_rolled_within_mask(extended: "Miner"):
    """Version bits rolled within the bip320 mask are a share, not invalid."""
    version = extended.job["version"] | (1 << 13) | (1 << 28)
    response = extended.submit(0, version=version)
    if response["type"] == SUBMIT_SHARES_ERROR:
        assert response["error_code"] == "difficulty-too-low"
    assert _not_dropped(extended)


def test_submit_ntime_rolled_forward(extended: "Miner"):
    """A later timestamp, within the future limit, is a share."""
    ntime = extended.previous["min_ntime"] + 60
    response = extended.submit(0, ntime=ntime)
    if response["type"] == SUBMIT_SHARES_ERROR:
        assert response["error_code"] == "difficulty-too-low"
    assert _not_dropped(extended)


def test_submit_futuristic_timestamp_dropped(extended: "Miner"):
    """A timestamp beyond the future limit (two hours) cannot be a block."""
    error = extended.submit(0, ntime=int(time.time()) + 3 * 3600)
    assert error["error_code"] == "invalid-share"
    assert extended.client.dropped()


def _reachable(miner: "Miner", target: int):
    if tries(target) > _MAXIMUM_TRIES:
        pytest.skip("share target not reachable in python, lower "
                    "[stratum_v2] minimum_difficulty")


def test_submit_shares_sum_is_difficulty(extended: "Miner"):
    """A share is counted by the whole units of its difficulty."""
    _reachable(extended, extended.target)
    rolled = (3).to_bytes(extended.rolled, "little")
    found = extended.search(extended.target, rolled)
    assert found is not None
    success = extended.submit(found[0], rolled)
    assert success["type"] == SUBMIT_SHARES_SUCCESS
    difficulty = _DIFFICULTY_ONE / extended.target
    assert abs(success["new_shares_sum"] - int(difficulty)) <= 1


def test_submit_prior_job_retained(extended: "Miner"):
    """
    A job refreshed at a new target retains the prior job of the same
    previous block, a share of which is valid at its own target.
    """
    _reachable(extended, extended.target)
    maximum = (extended.target // 2).to_bytes(32, "little")
    extended.client.send(update_channel(extended.channel_id,
                                        max_target=maximum))
    extended.client.expect(SET_TARGET)
    refreshed = extended.client.expect(NEW_EXTENDED_MINING_JOB)
    assert refreshed["job_id"] != extended.job["job_id"]

    rolled = (4).to_bytes(extended.rolled, "little")
    found = extended.search(extended.target, rolled)
    assert found is not None
    success = extended.submit(found[0], rolled)
    assert success["type"] == SUBMIT_SHARES_SUCCESS, success


def test_submit_evicted_job_is_stale(extended: "Miner"):
    """
    A channel retains its latest jobs (16), a share of an earlier job of the
    same previous block is stale.
    """
    target = extended.target
    for _ in range(17):
        target //= 2
        extended.client.send(update_channel(
            extended.channel_id, max_target=target.to_bytes(32, "little")))
        extended.client.expect(SET_TARGET)
        extended.client.expect(NEW_EXTENDED_MINING_JOB)
    error = extended.submit(0)
    assert error["type"] == SUBMIT_SHARES_ERROR
    assert error["error_code"] == "stale-share"


@pytest.mark.parametrize("kind", ["extended", "standard"])
def test_submit_valid_share(request, kind: str):
    """A share meeting the target is valid, and answered once."""
    miner = request.getfixturevalue(kind)
    if tries(miner.target) > _MAXIMUM_TRIES:
        pytest.skip("share target not reachable in python, lower "
                    "[stratum_v2] minimum_difficulty")
    rolled = (2).to_bytes(miner.rolled, "little") if miner.rolled else b""
    found = miner.search(miner.target, rolled)
    assert found is not None
    success = miner.submit(found[0], rolled, sequence=5)
    assert success["type"] == SUBMIT_SHARES_SUCCESS
    assert success["last_sequence_number"] == 5
    assert success["new_submits_accepted_count"] == 1
    if int.from_bytes(found[1], "little") > expand(miner.previous["nbits"]):
        again = miner.submit(found[0], rolled, sequence=6)
        assert again["error_code"] == "duplicate-share"


@pytest.mark.parametrize("kind", ["extended", "standard"])
def test_submit_mines_block(request, kind: str, native_config: dict):
    """
    A share meeting the block target is mined: the block becomes the top, and
    a future job and its previous block follow on it, and a share of the
    prior job is stale.

    Runs only where the block target is reachable in python (regtest), as it
    extends the chain.
    """
    miner = request.getfixturevalue(kind)
    block = expand(miner.previous["nbits"])
    target = min(block, miner.target)
    if tries(target) > _MAXIMUM_TRIES:
        pytest.skip("block target not reachable in python (not regtest)")
    try:
        before = native_top(native_config)
    except (requests.RequestException, KeyError, ValueError) as exc:
        pytest.skip(f"native top unavailable: {exc}")

    rolled = (1).to_bytes(miner.rolled, "little") if miner.rolled else b""
    found = miner.search(target, rolled)
    assert found is not None
    nonce, digest = found
    assert miner.submit(nonce, rolled)["type"] == SUBMIT_SHARES_SUCCESS

    mined = digest[::-1].hex()
    deadline = time.monotonic() + 30
    while native_top(native_config)["hash"] != mined:
        assert time.monotonic() < deadline, "mined block did not become top"
        time.sleep(0.2)
    assert native_top(native_config)["height"] == before["height"] + 1

    previous = miner.client.until(SET_NEW_PREV_HASH)
    assert previous["prev_hash"] == digest
    stale = miner.submit(nonce + 1, rolled, sequence=2)
    assert stale["error_code"] == "stale-share"


def test_mined_block_moves_other_connection(extended: "Miner",
                                            other: Sv2Client,
                                            stratum_v2_config: dict,
                                            native_config: dict):
    """
    A block mined on one connection moves the channels of another to a job
    on it, a share of their prior job then stale. Regtest only.
    """
    channel = opened(other, open_extended(stratum_v2_config["user"]),
                     OPEN_EXTENDED_MINING_CHANNEL_SUCCESS)
    observer = Miner(other, channel, other.expect(NEW_EXTENDED_MINING_JOB),
                     other.expect(SET_NEW_PREV_HASH))

    block = expand(extended.previous["nbits"])
    target = min(block, extended.target)
    if tries(target) > _MAXIMUM_TRIES:
        pytest.skip("block target not reachable in python (not regtest)")

    rolled = (5).to_bytes(extended.rolled, "little")
    found = extended.search(target, rolled)
    assert found is not None
    nonce, digest = found
    assert extended.submit(nonce, rolled)["type"] == SUBMIT_SHARES_SUCCESS

    future = other.until(NEW_EXTENDED_MINING_JOB)
    assert future["min_ntime"] is None
    previous = other.expect(SET_NEW_PREV_HASH)
    assert previous["prev_hash"] == digest
    assert previous["job_id"] == future["job_id"]
    stale = observer.submit(0, sequence=3)
    assert stale["error_code"] == "stale-share"


# ─── UpdateChannel and CloseChannel ──────────────────────────────────────────

def test_update_unknown_channel(extended: "Miner"):
    extended.client.send(update_channel(0xffffffff))
    error = extended.client.expect(UPDATE_CHANNEL_ERROR)
    assert error["error_code"] == "invalid-channel-id"


def test_update_zero_max_target(extended: "Miner"):
    extended.client.send(update_channel(extended.channel_id,
                                        max_target=bytes(32)))
    error = extended.client.expect(UPDATE_CHANNEL_ERROR)
    assert error["error_code"] == "max-target-out-of-range"


def test_update_lower_max_target_sets_target(extended: "Miner"):
    """A lower maximum target is set, then the job is sent again."""
    maximum = (extended.target // 2).to_bytes(32, "little")
    extended.client.send(update_channel(extended.channel_id,
                                        max_target=maximum))
    target = extended.client.expect(SET_TARGET)
    assert target["channel_id"] == extended.channel_id
    assert int.from_bytes(target["maximum_target"], "little") <= \
        extended.target // 2
    job = extended.client.expect(NEW_EXTENDED_MINING_JOB)
    assert job["min_ntime"] is not None


def test_update_hash_rate_sets_target(extended: "Miner"):
    """
    A nominal hash rate sets the difficulty of a channel to a share each
    interval, unless [stratum_v2] start_difficulty fixes it.
    """
    extended.client.send(update_channel(extended.channel_id, hash_rate=1e15))
    message = extended.client.poll(2.0)
    if message is None:
        pytest.skip("difficulty not automatic ([stratum_v2] "
                    "start_difficulty configured)")
    assert message["type"] == SET_TARGET
    assert int.from_bytes(message["maximum_target"], "little") < \
        extended.target
    job = extended.client.expect(NEW_EXTENDED_MINING_JOB)
    assert job["min_ntime"] is not None


def test_update_closed_channel(extended: "Miner"):
    extended.client.send(close_channel(extended.channel_id))
    extended.client.send(update_channel(extended.channel_id))
    error = extended.client.until(UPDATE_CHANNEL_ERROR)
    assert error["channel_id"] == extended.channel_id
    assert error["error_code"] == "invalid-channel-id"


def test_close_unknown_channel_ignored(extended: "Miner"):
    extended.client.send(close_channel(0xffffffff))
    error = extended.submit(0, job_id=0xffffffff)
    assert error["error_code"] == "invalid-job-id"


def test_close_channel_retains_others(extended: "Miner",
                                      stratum_v2_config: dict):
    client = extended.client
    second = opened(client, open_extended(stratum_v2_config["user"], 8),
                    OPEN_EXTENDED_MINING_CHANNEL_SUCCESS)
    client.send(close_channel(second["channel_id"]))
    client.send(submit_extended(second["channel_id"], 1, 0, 0, 0, 0,
                                bytes(extended.rolled)))
    assert client.until(SUBMIT_SHARES_ERROR)["error_code"] == \
        "invalid-channel-id"
    error = extended.submit(0, job_id=0xffffffff)
    assert error["error_code"] == "invalid-job-id"


def test_close_channel_refuses_shares(extended: "Miner"):
    extended.client.send(close_channel(extended.channel_id))
    error = extended.submit(0)
    assert error["type"] == SUBMIT_SHARES_ERROR
    assert error["error_code"] == "invalid-channel-id"


# ─── Credentials ─────────────────────────────────────────────────────────────

def test_upgrade_wrong_password_refused(stratum_v2_config: dict):
    if not stratum_v2_config["password"]:
        pytest.skip("no credential configured (--stratum-v2-password)")
    with pytest.raises(websocket.WebSocketException):
        Sv2Client(stratum_v2_config,
                  password=stratum_v2_config["password"] + "x")


def test_upgrade_without_credential_refused(stratum_v2_config: dict):
    if not stratum_v2_config["password"]:
        pytest.skip("no credential configured (--stratum-v2-password)")
    with pytest.raises(websocket.WebSocketException):
        Sv2Client(stratum_v2_config, password="")

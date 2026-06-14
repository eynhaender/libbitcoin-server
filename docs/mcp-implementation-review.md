# MCP-Server — Implementierungsübersicht

Branch: `feature/mcp-server`  
Basis-Commit: `89b97cdd`  
Letzte Aktualisierung: 2026-06-14

---

## Ziel

Einen [Model Context Protocol](https://spec.modelcontextprotocol.io/) Server in
libbitcoin-server integrieren, über den KI-Agenten (z.B. Claude) direkt
Blockchain-Daten abfragen können.  
MCP ist JSON-RPC 2.0 über Streamable HTTP (`POST /mcp`, Port 8334) sowie
optional über WebSocket (`ws://host:8334/mcp`).

---

## Schritt 1 — Interface-Definition

**Datei:** `include/bitcoin/server/interfaces/mcp.hpp`

`mcp_methods` deklariert 4 JSON-RPC-Methoden als `std::tuple` von
`rpc::method<"name", Args...>`:

| Methode | Parameter |
|---|---|
| `initialize` | `protocolVersion`, `capabilities?`, `clientInfo?` |
| `notifications/initialized` | — |
| `tools/list` | `cursor?` |
| `tools/call` | `name`, `arguments?` |

**Warum so:** libbitcoin-network's `rpc::dispatcher<Interface>` erwartet genau
dieses Muster — identisch zu `bitcoind_methods` in `interfaces/bitcoind.hpp`.
Die Methodennamen und Parametertypen entsprechen direkt der MCP-Spezifikation
(§3.1 Initialization, §3.6 Tools).

`notifications/initialized` hat keine Parameter: MCP-Notifications haben kein
`id`-Feld im Request und erwarten keinen Response-Body. Der Handler ruft deshalb
`send_ok(*reset_rpc_request())` auf, kein `send_result`.

---

## Schritt 2 — Protokollklasse

**Dateien:**
- `include/bitcoin/server/protocols/protocol_mcp.hpp`
- `src/protocols/mcp/protocol_mcp.cpp`

`protocol_mcp` erbt von `network::protocol_http` — auf derselben Ebene wie
`protocol_bitcoind_rpc`, nicht tiefer. Es gibt keine intermediate HTML-Schicht.

**Warum `protocol_http` direkt:** `protocol_http` bringt bereits alles mit:
HTTP-Request-Routing (OPTIONS/POST), Host- und Origin-Checks, CORS-Header,
`SEND()`-Makro. Wir überschreiben nur `handle_receive_options` und
`handle_receive_post`.

**Sender-Infrastruktur** (`send_rpc`, `send_error`, `send_result`,
`set_rpc_request`, `reset_rpc_request`) wurde 1:1 aus `protocol_bitcoind_rpc`
übernommen — die HTTP-Response-Logik ist identisch.

### `handle_receive_post` (HTTP-Zweig)

Nach Host/Origin-Check und JSON-Parse ruft `rpc_dispatcher_.notify(message)` den
registrierten Handler auf. Wenn die Methode unbekannt ist, liefert der Dispatcher
`error::unexpected_method`. Das wird mit `send_error` in eine JSON-RPC-Fehlerantwort
umgewandelt — die Session bleibt offen. Alle anderen Dispatcher-Fehler sind fatal
und schliessen die Session (`stop(code)`).

### `dispatch_websocket` (WebSocket-Zweig)

WebSocket-Frames kommen über `handle_receive_unknown` herein und werden an
`dispatch_websocket` weitergeleitet. Der Parse- und Dispatch-Pfad ist identisch
zum HTTP-Zweig, mit zwei Besonderheiten:

**`ws_response_sent_`-Flag:** MCP-Notifications (z. B. `notifications/initialized`)
senden keine Antwort. Für WebSocket muss nach jeder Nachricht `resume()` aufgerufen
werden, um den Read-Loop neu zu starten. `send_rpc` setzt `ws_response_sent_ = true`
wenn eine Antwort gesendet wurde. Wird kein Response gesendet, ruft
`dispatch_websocket` am Ende selbst `resume()`.

**`notify_json`:** Für WS-Push-Events (Subscriptions) sendet `notify_json` eine
unsolizitierte JSON-Nachricht ohne `id`. Dies spiegelt `protocol_html::notify_json`
1:1 wider.

### Chase-Event-Integration (`handle_chase`)

`start()` registriert `subscribe_chase(BIND(handle_chase, _1, _2, _3))`. Der
Handler reagiert auf:

| Chase-Event | Aktion |
|---|---|
| `chase::block` | Ruft `do_top` (wenn `top_subscribe_`) und `do_block` (wenn `block_subscribe_`) |
| `chase::reorganized` | Ruft `do_top` (wenn `top_subscribe_`) |
| `chase::transaction` | Ruft `do_transaction` (wenn `tx_subscribe_`) |

Für reine HTTP-Verbindungen ohne aktive SSE-Session werden Chase-Events
vorzeitig ignoriert (`if (!websocket() && !sse_) return true`).

Die drei `do_top` / `do_block` / `do_transaction`-Methoden lesen den Block- oder
TX-Hash aus dem Archiv und senden die Benachrichtigung — via `notify_json` (WS)
oder `SSE_WRITE` (HTTP/SSE).

### Tool-Implementierungen

#### Block-Tools

| Tool | Implementierung |
|---|---|
| `get_blockchain_info` | `get_top_confirmed()` → `to_confirmed()` → `{height, hash}` |
| `get_configuration` | Serialisiert `session().config()` als JSON |
| `get_block_header` | Arg `height` oder `hash` → `archive().get_header_key()` → Headerfelder |
| `get_block_header_context` | Wie `get_block_header` + Chainwork, Median-Time, Confirmations |
| `get_block_details` | Header + Transaktionsanzahl, Grösse, Gewicht |
| `get_block_txs` | Array aller TxIDs im Block via `get_block_txs()` |
| `get_block_tx` | Einzelne TX im Block per Index |
| `get_block_filter` | BIP157 Compact Block Filter |
| `get_block_filter_hash` | Filter-Hash |
| `get_block_filter_header` | Filter-Header |

#### Transaktions-Tools

| Tool | Implementierung |
|---|---|
| `get_transaction` | `decode_hash(hash)` → `archive().get_transaction()` → vollständige TX |
| `get_tx_header` | Nur Metadaten (TxID, Version, Locktime, Confirmations) |
| `get_tx_details` | Wie `get_tx_header` + Inputs/Outputs als strukturiertes JSON |
| `get_input` | TX-Hash + Input-Index → ein Input |
| `get_output` | TX-Hash + Output-Index → ein Output |
| `get_output_spender` | Output → spendende TX (falls vorhanden) |
| `get_output_spenders` | Alle Spender eines Outputs |

#### Adress-Tools

Die Adress-Tools nutzen `PARALLEL` (off-strand, nicht-blockierender Thread) für
datenbankintensive Suchen, mit `POST` für die Rückkehr auf den Strand:

| Tool | Implementierung |
|---|---|
| `get_address_history` | `PARALLEL(do_address_outpoints, false)` → alle Outpoints der Adresse |
| `get_address_confirmed` | `PARALLEL(do_address_outpoints, true)` → nur bestätigte, unausgegebene Outpoints |
| `get_address_balance` | `PARALLEL(do_address_balance)` — nur wenn `archive().address_enabled()` |

`stopping_` (atomares Flag) erlaubt dem Strand dem PARALLEL-Thread zu signalisieren,
die Suche abzubrechen wenn die Session beendet wird.

#### Subscription-Tools

| Tool | WS-Verhalten | HTTP-Verhalten |
|---|---|---|
| `get_top_subscribe` | Sofortiger Result + zukünftige `bitcoin/top`-Pushes via WS | SSE-Stream öffnen; erstes Event = Result; weitere = Pushes |
| `get_block_subscribe` | Sofortiger Result + zukünftige `bitcoin/block`-Pushes via WS | SSE analog |
| `get_tx_subscribe` | Sofortiger Result + zukünftige `bitcoin/transaction`-Pushes via WS | SSE analog |

### Wichtige Erkenntnis: `rpc::value_t`

Erster Compilierversuch verwendete Boost.JSON-API (`is_number()`,
`to_number<T>()`, `as_string()`). `rpc::value_t` ist jedoch ein eigener
Variant-Wrapper mit `inner_t = std::variant<null_t, boolean_t, number_t,
string_t, ...>` und `.value()` als Accessor.

Korrekte Zugriffsmuster:
```cpp
// Typ prüfen:
std::holds_alternative<rpc::number_t>(val.value())
// Wert extrahieren:
std::get<rpc::number_t>(val.value())   // → double
std::get<rpc::string_t>(val.value())   // → std::string&
```

Alle JSON-Zahlen werden beim Deserialisieren als `number_t` (`double`) gespeichert
(siehe `src/messages/rpc/model.cpp:154`). Für `height` daher:
```cpp
static_cast<int64_t>(std::get<rpc::number_t>(it->second.value()))
```
dann `possible_sign_cast<size_t>()` für den unsigned-Cast (Reviewer-Präferenz
aus PR #795).

---

## Schritt 3 — Integration in Includes und Session-Typ

**`interfaces/interfaces.hpp`:** `using mcp = publish<mcp_methods>;` — macht
`mcp::initialize`, `mcp::tools_call` etc. als Typen verfügbar.

**`protocols/protocols.hpp`:** Include von `protocol_mcp.hpp` hinzugefügt,
Kommentar-Diagramm aktualisiert.

**`sessions/sessions.hpp`:** `using session_mcp = session_server<protocol_mcp>;`
— dasselbe Template wie alle anderen HTTP-Sessions.

---

## Schritt 4 — Settings

**Datei:** `include/bitcoin/server/settings.hpp`

```cpp
/// mcp interface (http/s, stateless json-rpc-v2)
network::settings::http_server mcp{ "mcp" };
```

`http_server` kapselt: Bind-Adresse, TLS-Zertifikat, Host-Whitelist,
Origin-Whitelist, Connection-Limits. Das `"mcp"`-Prefix bestimmt die
Konfigurations-Sektion. Default-Port: 8334 (in `bs.cfg` aktivierbar).

---

## Schritt 5 — Startup-Kette

**Datei:** `src/server_node.cpp`

`start_mcp()` zwischen `start_bitcoind()` und `start_electrum()` eingehängt:

```
do_run → start_admin → start_native → start_bitcoind → start_mcp → start_electrum → …
```

`attach_mcp_session()` instanziiert `session_mcp` mit `config_.server.mcp`.
Das ist das etablierte Muster — jede Methode ruft am Ende die nächste auf.

---

## Schritt 6 — Konfiguration

**Dateien:** `src/parser.cpp`, `data/bs.cfg`

`[mcp]`-Sektion mit allen `http_server`-Optionen als `po::value<>` in
`parser.cpp`, dazugehöriger kommentierter Block in `bs.cfg`:

```ini
[mcp]
# bind = 0.0.0.0:8334
# safe = false
# ...
```

Ohne diesen Schritt wäre der Server compilierbar aber nicht per Konfigurationsdatei
aktivierbar.

---

## Schritt 7 — Build-System

**Datei:** `Makefile.am`

`src/protocols/mcp/protocol_mcp.cpp` und die beiden neuen Header explizit
eingetragen. Das CMake-Preset (`nix-gnu-debug-static`) findet neue Dateien per
`GLOB_RECURSE` automatisch. `Makefile.am` braucht für Autotools/`make dist`
explizite Einträge.

---

## Schritt 8 — SSE für HTTP-Subscription-Streaming

### Architektur

HTTP-Subscription-Tools können keine persistente Verbindung für Push-Nachrichten
nutzen wie WebSocket. Stattdessen öffnet der Server einen
`text/event-stream`-Response (SSE gemäss
[WHATWG EventSource](https://html.spec.whatwg.org/multipage/server-sent-events.html)).

Pro `protocol_mcp`-Instanz (= eine HTTP-Verbindung) gibt es genau einen SSE-Kanal:

```cpp
network::socket::sse_state::ptr sse_{};
std::string sse_initial_event_{};
bool sse_writing_{};
```

`sse_writing_` serialisiert Schreibzugriffe: `SSE_WRITE` darf nicht gerufen werden,
während ein vorheriges Write noch aussteht. Chase-Events die während eines Writes
eintreffen, werden verworfen (kein Buffer). Dies ist akzeptabel, weil Blöcke
im Mainnet ~alle 10 Minuten auftreten.

### Ablauf (am Beispiel `get_top_subscribe`)

```
Client POST /mcp (Accept: text/event-stream, tools/call get_top_subscribe)
  → tool_get_top_subscribe()
      top_subscribe_.store(true)
      reset_rpc_request() → sse_initial_event_ = "data: {JSON-RPC result}\n\n"
      sse_ = make_shared<sse_state>(HTTP-Version)
      SSE_START(sse_, handle_sse_start)   ← HTTP 200 + text/event-stream Header

  → handle_sse_start(ok)
      SSE_WRITE(sse_, sse_initial_event_) ← erstes Event: subscribed=true, height, hash

  → handle_sse_write(ok)
      sse_writing_ = false                ← bereit für nächstes Push-Event

  [Chase::block]
  → handle_chase() → POST(do_top, link)
  → do_top()
      sse_writing_ = true
      SSE_WRITE(sse_, "data: {bitcoin/top JSON}\n\n")
  → handle_sse_write(ok)
      sse_writing_ = false

  [Client trennt Verbindung]
  → stopping() → sse_.reset()
```

### SSE-Event-Format

```
data: {"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"..."}]}}\n\n

data: {"jsonrpc":"2.0","method":"bitcoin/top","params":{"height":953617,"hash":"000…"}}\n\n
```

Das initiale Event ist eine vollständige JSON-RPC-Response (mit `id` und `result`).
Folgeevents sind JSON-RPC-Notifications (kein `id`, nur `method` und `params`).

---

## Schritt 9 — Endpoint-Tests

### `endpoints/conftest.py`

Ergänzt um:
- `--mcp-host` / `--mcp-port` → `mcp_config`-Fixture
- `--sse-wait` → `sse_wait`-Fixture (Sekunden für Live-Push-Tests; Default 0 = skipped)

### `endpoints/test_mcp.py` — HTTP-Tests

26 Tests in 7 Klassen. **Ergebnis: 24 passed, 2 xfailed.**

| Klasse | Abdeckung |
|---|---|
| `TestSessionLifecycle` | `initialize` (vollständig + minimal), `notifications/initialized` → HTTP 200 kein Body |
| `TestToolsDiscovery` | Schema-Form, alle erwarteten Tools vorhanden, idempotent |
| `TestToolGetBlockchainInfo` | Height+Hash, positiv, leere Args |
| `TestToolGetBlockHeader` | Genesis, bekannte Height, bekannter Hash, out-of-range → Error, fehlende Args → Error |
| `TestToolGetTransaction` | Bekannte TxID, erste TX, unbekannte TxID → Error, ungültiger Hash → Error, fehlende Args → Error |
| `TestToolsNotImplemented` | Address-Tools als `xfail` markiert; strukturierter Error (kein Crash) erwartet |
| `TestErrorHandling` | Unbekanntes Tool, unbekannte Methode, ungültiges JSON, `jsonrpc=2.0` in jeder Antwort |

### `endpoints/test_mcp_websocket.py` — WebSocket-Tests

Eigenständiges Asyncio-Script (kein pytest). Testet:
1. WebSocket-Verbindungsaufbau
2. `initialize`-Handshake
3. `notifications/initialized`-Notification (kein Response erwartet)
4. `get_top_subscribe`, `get_block_subscribe`, `get_tx_subscribe` — sofortiger Result

**Ergebnis: 5/5 passed.** Optionaler `--wait N`-Parameter für Live-Push-Empfang.

### `endpoints/test_mcp_sse.py` — SSE-Tests

23 Tests in 4 Klassen. **Ergebnis: 20 passed, 3 skipped (Push-Tests ohne `--sse-wait`).**

**Architektonische Besonderheit:** Jeder SSE-Subscription-Test würde eine neue
TCP-Verbindung öffnen. Der Server hat ein Verbindungslimit (~8–10 simultane
Verbindungen). Lösung: drei `session`-scoped Fixtures (`sse_top`, `sse_block`,
`sse_tx`), die **eine Verbindung pro Tool** für die gesamte Test-Session halten.
Alle Tests teilen sich diese Fixtures — maximal 3 SSE-Verbindungen gleichzeitig.

| Klasse | Abdeckung |
|---|---|
| `TestSSEConnection` | HTTP 200, `Content-Type: text/event-stream` für alle 3 Tools |
| `TestSSEInitialEvent` | Event empfangen, JSON-RPC 2.0 Envelope, `subscribed=true`, valide Height/Hash |
| `TestSSEEventFormat` | SSE `data:`-Payload ist gültiges JSON |
| `TestSSEPushNotifications` | `bitcoin/top`/`block`/`transaction` Push-Shape (nur mit `--sse-wait=N`) |

### Keepalive-Mechanismus

SSE-Verbindungen ohne Aktivität werden nach dem `inactivity_`-Timer des Kanals
(Default 10 Minuten) getrennt. Um dies zu verhindern:

**`channel::writing()`:** Jedes `sse_write` ruft intern `writing()` auf, das den
Inaktivitäts-Timer zurücksetzt — analog zu `reading()` beim Empfang.

**5-Minuten-Keepalive:** `handle_sse_write` startet beim ersten erfolgreichen
Write einen `deadline`-Timer (5 Minuten). `handle_keepalive` sendet dann
periodisch einen SSE-Comment (`:\n\n`) — das einzige SSE-Format das kein
Event auslöst aber die TCP-Verbindung und den Timer am Leben hält.

```
handle_sse_write(ok, first time)
  → start_keepalive()
      deadline(5 min, handle_keepalive)
  → handle_keepalive()
      SSE_WRITE(":\n\n", handle_sse_keepalive)
      deadline(5 min, handle_keepalive)   ← nächster Zyklus
```

### Kritischer Bug: Beast `buffer_body::writer::toggle_`

**Problem:** `boost::beast::http::async_write` ist eine Loop bis
`serializer_is_done()`, die für einen offenen SSE-Stream nie `true` ist.
Nach dem Schreiben jedes Chunks ruft die Loop intern `wr_.get()` erneut auf.
`buffer_body::writer::get()` hat ein `toggle_`-Flag: beim zweiten Aufruf gibt es
`error::need_buffer` zurück und setzt `toggle_` zurück. `async_write` beendet den
Handler mit `ec = need_buffer`.

`handle_async` mapped diesen Code via `asio_to_error_code` (kennt nur
asio-Kategorien) → `error::unknown` (non-zero). `handle_sse_write` sieht
`stopped(error::unknown) = true` → `sse_.reset()`. Die Verbindung bricht nach
dem ersten Event still ab.

**Fix** (`libbitcoin-network/src/net/socket_sse.cpp`, `socket::sse_write`):

```cpp
VARIANT_DISPATCH_FUNCTION(
    boost::beast::http::async_write, get_tcp(),
    state->serializer,
    [self = shared_from_this(), h = std::move(handler)](
        const boost_code& ec, size_t bytes) mutable NOEXCEPT
    {
        // buffer_body::writer returns need_buffer after each SSE chunk
        // to signal "chunk written, ready for more data" — treat as success.
        const auto mapped =
            (ec == error::to_http_code(error::http_error_t::need_buffer))
            ? boost_code{} : ec;
        self->handle_async(mapped, bytes, h, "sse-write");
    });
```

`sse_close` bleibt auf `async_write` mit `std::bind` — mit `more=false` läuft
der Serializer bis `do_complete` durch ohne `need_buffer`.

### Verifizierte Testergebnisse (2026-06-14, Mainnet Block ~953665)

```
[  0.0s] INITIAL     height=953665 hash=0000000000000000...
[198.4s] bitcoin/top height=953666 hash=0000000000000000...
[300.0s] KEEPALIVE   (since last: 0.0s)
[590.5s] bitcoin/top height=953667 hash=0000000000000000...
[600.0s] KEEPALIVE   (since last: 0.0s)
```

- Keepalive exakt alle 300s ✓
- Block-Push-Events nach ~10 Minuten Verbindungszeit ✓
- Kein Reconnect über 10 Minuten ✓

---

## Sonderfälle

**Ungültiges JSON:** Beast's HTTP-Body-Reader setzt beim JSON-Parse-Fehler einen
Error-Code und schliesst die Connection, bevor `handle_receive_post` erreicht wird.
Eine 400-Antwort wäre wünschenswert, erfordert aber Änderungen in
`libbitcoin-network`. RFC 7230 §6.6 erlaubt Connection-Close ohne Response —
der Test akzeptiert beides.

**Unbekannte Methode:** Der Dispatcher gibt `network::error::unexpected_method`
zurück. Anders als in `protocol_bitcoind_rpc` (wo alle Methoden immer bekannt
sind) sendet `protocol_mcp` hier eine JSON-RPC-Fehlerantwort statt die Session
zu schliessen.

**Adress-Tools ohne Address-Indexierung:** `get_address_history` und
`get_address_confirmed` laufen via `PARALLEL` und können deshalb auch mit
aktiviertem Archiv-Flag genutzt werden. `get_address_balance` prüft zusätzlich
`archive().address_enabled()` und gibt `error::not_implemented` zurück wenn das
Feature nicht aktiv ist.

**SSE vs. WebSocket:** `handle_chase` prüft den Verbindungstyp:
- WebSocket → `notify_json` (Frame sofort senden)
- HTTP ohne aktive `sse_` → Chase-Events ignorieren (früher Return)
- HTTP mit `sse_` → `SSE_WRITE` wenn kein Write ausstehend, sonst verwerfen

---

## Noch ausstehend

- Address-Tools (`get_address_balance`, `get_address_history`, `get_address_confirmed`)
  vollständig testen sobald ein Node mit aktivem Address-Index verfügbar ist.
- `get_block_details`, `get_block_txs`, `get_tx_details` etc. in `test_mcp.py`
  abdecken (aktuell in `EXPECTED_TOOLS` gelistet aber nicht explizit getestet).
- SSE-Push-Tests in `test_mcp_sse.py` (`--sse-wait`) auf Mainnet verifiziert ✓ — kein
  separater Node mit Mining-Traffic erforderlich, reguläre Mainnet-Blöcke ausreichend.
- Strukturelle Refaktorierung: `protocol_mcp.cpp` (1943 Zeilen) nach Vorbild von
  `native/` in Domain-Dateien aufteilen (`protocol_mcp_block.cpp`,
  `protocol_mcp_tx.cpp`, `protocol_mcp_address.cpp`, etc.).

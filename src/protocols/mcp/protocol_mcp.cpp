/**
 * Copyright (c) 2011-2026 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/server/protocols/protocol_mcp.hpp>

#include <algorithm>
#include <limits>
#include <numeric>
#include <ranges>
#include <string>
#include <bitcoin/server/define.hpp>
#include <bitcoin/server/interfaces/interfaces.hpp>
#include <bitcoin/server/version.hpp>

namespace libbitcoin {
namespace server {

#define CLASS protocol_mcp
#define SUBSCRIBE_MCP(method, ...) \
    subscribe<CLASS>(&CLASS::method, __VA_ARGS__)

using namespace system;
using namespace network;
using namespace network::json;
using namespace std::placeholders;

BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)
BC_PUSH_WARNING(SMART_PTR_NOT_NEEDED)
BC_PUSH_WARNING(NO_VALUE_OR_CONST_REF_SHARED_PTR)

// Start.
// ----------------------------------------------------------------------------

void protocol_mcp::start() NOEXCEPT
{
    BC_ASSERT(stranded());

    if (started())
        return;

    subscribe_chase(BIND(handle_chase, _1, _2, _3));

    SUBSCRIBE_MCP(handle_initialize, _1, _2, _3, _4, _5);
    SUBSCRIBE_MCP(handle_notifications_initialized, _1, _2);
    SUBSCRIBE_MCP(handle_tools_list, _1, _2, _3);
    SUBSCRIBE_MCP(handle_tools_call, _1, _2, _3, _4, _5);
    network::protocol_http::start();
}

void protocol_mcp::stopping(const code& ec) NOEXCEPT
{
    BC_ASSERT(stranded());
    stopping_.store(true);
    top_subscribe_.store(false);
    block_subscribe_.store(false);
    tx_subscribe_.store(false);
    if (keepalive_)
        keepalive_->stop();
    sse_.reset();
    sse_writing_ = false;
    rpc_dispatcher_.stop(ec);
    unsubscribe_chase();
    network::protocol_http::stopping(ec);
}

// Dispatch (WebSocket).
// ----------------------------------------------------------------------------

void protocol_mcp::dispatch_websocket(const http::request& request) NOEXCEPT
{
    BC_ASSERT(stranded());

    if (!request.body().contains<http::string_value>())
    {
        stop(network::error::not_acceptable);
        return;
    }

    const auto& text = request.body().get<http::string_value>();

    ws_response_sent_ = false;

    BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)
    try
    {
        const auto json = boost::json::parse(text);
        const auto message = boost::json::value_to<rpc::request_t>(json);
        version_ = message.jsonrpc;
        id_      = message.id;
        if (const auto ec = rpc_dispatcher_.notify(message))
        {
            if (ec == network::error::unexpected_method)
                send_error(ec);
            else
                stop(ec);
        }
    }
    catch (...)
    {
        stop(network::error::bad_request);
    }
    BC_POP_WARNING()

    // For WS notifications (no response sent), restart the read loop.
    // SEND restarts it via handle_send; for no-response cases we must do it here.
    if (!ws_response_sent_ && !stopped())
        resume();
}

// Event handlers.
// ----------------------------------------------------------------------------

bool protocol_mcp::handle_chase(const code&, node::chase event_,
    node::event_value value) NOEXCEPT
{
    if (stopped())
        return false;

    // For HTTP connections, only process chase events when SSE is active.
    if (!websocket() && !sse_)
        return true;

    constexpr auto relaxed = std::memory_order_relaxed;
    switch (event_)
    {
        case node::chase::block:
        {
            BC_ASSERT(std::holds_alternative<node::header_t>(value));
            if (top_subscribe_.load(relaxed))
                POST(do_top, std::get<node::header_t>(value));
            if (block_subscribe_.load(relaxed))
                POST(do_block, std::get<node::header_t>(value));
            break;
        }
        case node::chase::reorganized:
        {
            BC_ASSERT(std::holds_alternative<node::header_t>(value));
            if (top_subscribe_.load(relaxed))
                POST(do_top, std::get<node::header_t>(value));
            break;
        }
        case node::chase::transaction:
        {
            BC_ASSERT(std::holds_alternative<node::transaction_t>(value));
            if (tx_subscribe_.load(relaxed))
                POST(do_transaction, std::get<node::transaction_t>(value));
            break;
        }
        default:
        {
            break;
        }
    }

    return true;
}

// Dispatch (HTTP).
// ----------------------------------------------------------------------------

void protocol_mcp::handle_receive_options(const code& ec,
    const network::http::method::options::cptr& options) NOEXCEPT
{
    BC_ASSERT(stranded());

    if (stopped(ec))
        return;

    if (!is_allowed_host(*options, options->version()))
    {
        send_bad_host(*options);
        return;
    }

    if (!is_allowed_origin(*options, options->version()))
    {
        send_forbidden(*options);
        return;
    }

    send_ok(*options);
}

void protocol_mcp::handle_receive_post(const code& ec,
    const network::http::method::post::cptr& post) NOEXCEPT
{
    BC_ASSERT(stranded());

    if (stopped(ec))
        return;

    if (!is_allowed_host(*post, post->version()))
    {
        send_bad_host(*post);
        return;
    }

    if (!is_allowed_origin(*post, post->version()))
    {
        send_forbidden(*post);
        return;
    }

    if (!post->body().contains<rpc::request>())
    {
        send_bad_request(*post);
        return;
    }

    const auto& message = post->body().get<rpc::request>().message;

    // v1 null id and v2 missing id implies notification (no response).
    set_rpc_request(message.jsonrpc, message.id, post);

    if (const auto code = rpc_dispatcher_.notify(message))
    {
        // Unknown method: send method_not_found error, keep session alive.
        // All other dispatcher errors are fatal (type mismatch etc.).
        if (code == network::error::unexpected_method)
            send_error(code);
        else
            stop(code);
    }
}

// Handlers — session lifecycle.
// ----------------------------------------------------------------------------

bool protocol_mcp::handle_initialize(const code& ec,
    rpc_interface::initialize,
    const std::string& /*protocol_version*/,
    const network::rpc::object_t& /*capabilities*/,
    const network::rpc::object_t& /*client_info*/) NOEXCEPT
{
    if (stopped(ec))
        return false;

    boost::json::value result
    {
        boost::json::object
        {
            { "protocolVersion", "2024-11-05" },
            { "capabilities",    boost::json::object{ { "tools", boost::json::object{} } } },
            { "serverInfo",      boost::json::object
                {
                    { "name",    "libbitcoin-server" },
                    { "version", LIBBITCOIN_SERVER_VERSION }
                }
            }
        }
    };

    send_result(std::move(result), 256);
    return true;
}

bool protocol_mcp::handle_notifications_initialized(const code& ec,
    rpc_interface::notifications_initialized) NOEXCEPT
{
    if (stopped(ec))
        return false;

    // Over WebSocket there is no HTTP request to acknowledge.
    if (websocket())
        return true;

    // MCP spec: notification-only requests get HTTP 202 with no body.
    const auto request = reset_rpc_request();
    send_ok(*request);
    return true;
}

// Handlers — tool discovery and invocation.
// ----------------------------------------------------------------------------

// Tools schema is static — compute once.
static const boost::json::value& tools_list_value() NOEXCEPT
{
    static const boost::json::value value
    {
        boost::json::object
        {
            { "tools", boost::json::array
                {
                    // -------------------------------------------------------
                    // Chain state
                    // -------------------------------------------------------
                    boost::json::object
                    {
                        { "name",        "get_blockchain_info" },
                        { "description", "Returns the current blockchain tip height and hash." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object{} },
                                { "required",   boost::json::array{} }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_configuration" },
                        { "description", "Returns node configuration: address/filter indexing, witness, pruning, turbo mode, subsidy fork state." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object{} },
                                { "required",   boost::json::array{} }
                            }
                        }
                    },

                    // -------------------------------------------------------
                    // Block
                    // -------------------------------------------------------
                    boost::json::object
                    {
                        { "name",        "get_block_header" },
                        { "description", "Returns the 80-byte block header for the block identified by hash or height." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash",   boost::json::object{ { "type", "string" }, { "description", "Block hash (hex, big-endian)" } } },
                                        { "height", boost::json::object{ { "type", "number" }, { "description", "Block height (zero-based)" } } }
                                    }
                                },
                                { "required", boost::json::array{} }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_block_header_context" },
                        { "description", "Returns chain context for a block: height, MTP, active BIP flags, validation/confirmation state, and block weight/size." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash",   boost::json::object{ { "type", "string" }, { "description", "Block hash (hex, big-endian)" } } },
                                        { "height", boost::json::object{ { "type", "number" }, { "description", "Block height (zero-based)" } } }
                                    }
                                },
                                { "required", boost::json::array{} }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_block_details" },
                        { "description", "Returns block statistics: tx count, size, weight, fees, subsidy, miner reward, coinbase claim." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash",   boost::json::object{ { "type", "string" }, { "description", "Block hash (hex, big-endian)" } } },
                                        { "height", boost::json::object{ { "type", "number" }, { "description", "Block height (zero-based)" } } }
                                    }
                                },
                                { "required", boost::json::array{} }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_block_txs" },
                        { "description", "Returns all transaction hashes in a block as a JSON array." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash",   boost::json::object{ { "type", "string" }, { "description", "Block hash (hex, big-endian)" } } },
                                        { "height", boost::json::object{ { "type", "number" }, { "description", "Block height (zero-based)" } } }
                                    }
                                },
                                { "required", boost::json::array{} }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_block_tx" },
                        { "description", "Returns the transaction at a given position within a block." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash",     boost::json::object{ { "type", "string" }, { "description", "Block hash (hex, big-endian)" } } },
                                        { "height",   boost::json::object{ { "type", "number" }, { "description", "Block height (zero-based)" } } },
                                        { "position", boost::json::object{ { "type", "number" }, { "description", "Zero-based transaction index within the block" } } }
                                    }
                                },
                                { "required", boost::json::array{ "position" } }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_block_filter" },
                        { "description", "Returns the BIP 157/158 (Neutrino) compact block filter body for a block. Requires filter indexing." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash",   boost::json::object{ { "type", "string" }, { "description", "Block hash (hex, big-endian)" } } },
                                        { "height", boost::json::object{ { "type", "number" }, { "description", "Block height (zero-based)" } } }
                                    }
                                },
                                { "required", boost::json::array{} }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_block_filter_hash" },
                        { "description", "Returns the BIP 157 filter hash for a block. Requires filter indexing." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash",   boost::json::object{ { "type", "string" }, { "description", "Block hash (hex, big-endian)" } } },
                                        { "height", boost::json::object{ { "type", "number" }, { "description", "Block height (zero-based)" } } }
                                    }
                                },
                                { "required", boost::json::array{} }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_block_filter_header" },
                        { "description", "Returns the BIP 157 filter header (cumulative hash chain) for a block. Requires filter indexing." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash",   boost::json::object{ { "type", "string" }, { "description", "Block hash (hex, big-endian)" } } },
                                        { "height", boost::json::object{ { "type", "number" }, { "description", "Block height (zero-based)" } } }
                                    }
                                },
                                { "required", boost::json::array{} }
                            }
                        }
                    },

                    // -------------------------------------------------------
                    // Transaction
                    // -------------------------------------------------------
                    boost::json::object
                    {
                        { "name",        "get_transaction" },
                        { "description", "Returns transaction data for the given txid." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash", boost::json::object
                                            {
                                                { "type",        "string" },
                                                { "description", "Transaction hash (hex, big-endian)" }
                                            }
                                        }
                                    }
                                },
                                { "required", boost::json::array{ "hash" } }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_tx_header" },
                        { "description", "Returns the block header of the confirmed block containing the given transaction. Fails if unconfirmed." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash", boost::json::object{ { "type", "string" }, { "description", "Transaction hash (hex, big-endian)" } } }
                                    }
                                },
                                { "required", boost::json::array{ "hash" } }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_tx_details" },
                        { "description", "Returns transaction metadata: fee, size, weight, coinbase flag, and confirmation context (height + position)." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash", boost::json::object{ { "type", "string" }, { "description", "Transaction hash (hex, big-endian)" } } }
                                    }
                                },
                                { "required", boost::json::array{ "hash" } }
                            }
                        }
                    },

                    // -------------------------------------------------------
                    // Inputs
                    // -------------------------------------------------------
                    boost::json::object
                    {
                        { "name",        "get_input" },
                        { "description", "Returns a single input at the given index within a transaction." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash",  boost::json::object{ { "type", "string" }, { "description", "Transaction hash (hex, big-endian)" } } },
                                        { "index", boost::json::object{ { "type", "number" }, { "description", "Zero-based input index" } } }
                                    }
                                },
                                { "required", boost::json::array{ "hash", "index" } }
                            }
                        }
                    },
                    // -------------------------------------------------------
                    // Outputs
                    // -------------------------------------------------------
                    boost::json::object
                    {
                        { "name",        "get_output" },
                        { "description", "Returns a single output at the given index within a transaction." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash",  boost::json::object{ { "type", "string" }, { "description", "Transaction hash (hex, big-endian)" } } },
                                        { "index", boost::json::object{ { "type", "number" }, { "description", "Zero-based output index" } } }
                                    }
                                },
                                { "required", boost::json::array{ "hash", "index" } }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_output_spender" },
                        { "description", "Returns the confirmed spending input (txhash + input index) for a UTXO, if spent." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash",  boost::json::object{ { "type", "string" }, { "description", "Transaction hash of the output (hex, big-endian)" } } },
                                        { "index", boost::json::object{ { "type", "number" }, { "description", "Zero-based output index" } } }
                                    }
                                },
                                { "required", boost::json::array{ "hash", "index" } }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_output_spenders" },
                        { "description", "Returns all spending inputs (confirmed and unconfirmed) for a transaction output." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "hash",  boost::json::object{ { "type", "string" }, { "description", "Transaction hash of the output (hex, big-endian)" } } },
                                        { "index", boost::json::object{ { "type", "number" }, { "description", "Zero-based output index" } } }
                                    }
                                },
                                { "required", boost::json::array{ "hash", "index" } }
                            }
                        }
                    },

                    // -------------------------------------------------------
                    // Address (requires address indexing)
                    // -------------------------------------------------------
                    boost::json::object
                    {
                        { "name",        "get_address_balance" },
                        { "description", "Returns the confirmed satoshi balance for a script hash (SHA256 of scriptPubKey, 64 hex chars). Requires address indexing." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "address", boost::json::object
                                            {
                                                { "type",        "string" },
                                                { "description", "Output script hash: SHA256(scriptPubKey), big-endian hex (64 chars)" }
                                            }
                                        }
                                    }
                                },
                                { "required", boost::json::array{ "address" } }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_address_history" },
                        { "description", "Returns all outpoints (confirmed + unconfirmed) for a script hash. Requires address indexing." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "address", boost::json::object
                                            {
                                                { "type",        "string" },
                                                { "description", "Output script hash: SHA256(scriptPubKey), big-endian hex (64 chars)" }
                                            }
                                        }
                                    }
                                },
                                { "required", boost::json::array{ "address" } }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_address_confirmed" },
                        { "description", "Returns confirmed unspent outpoints for a script hash. Requires address indexing." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "address", boost::json::object
                                            {
                                                { "type",        "string" },
                                                { "description", "Output script hash: SHA256(scriptPubKey), big-endian hex (64 chars)" }
                                            }
                                        }
                                    }
                                },
                                { "required", boost::json::array{ "address" } }
                            }
                        }
                    },

                    // -------------------------------------------------------
                    // Subscriptions (WebSocket only)
                    // -------------------------------------------------------
                    boost::json::object
                    {
                        { "name",        "get_top_subscribe" },
                        { "description", "Subscribe to chain tip updates over WebSocket. Returns the current tip immediately and pushes {\"jsonrpc\":\"2.0\",\"method\":\"bitcoin/top\",\"params\":{\"height\":N,\"hash\":\"...\"}} for every new confirmed block or reorganization. Requires a WebSocket connection." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object{} },
                                { "required",   boost::json::array{} }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_block_subscribe" },
                        { "description", "Subscribe to new confirmed block hashes over WebSocket. Returns the current top block hash immediately and pushes {\"jsonrpc\":\"2.0\",\"method\":\"bitcoin/block\",\"params\":{\"hash\":\"...\"}} for every new confirmed block. Requires a WebSocket connection." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object{} },
                                { "required",   boost::json::array{} }
                            }
                        }
                    },
                    boost::json::object
                    {
                        { "name",        "get_tx_subscribe" },
                        { "description", "Subscribe to new mempool transaction hashes over WebSocket. Pushes {\"jsonrpc\":\"2.0\",\"method\":\"bitcoin/transaction\",\"params\":{\"hash\":\"...\"}} for every new unconfirmed transaction. Requires a WebSocket connection." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object{} },
                                { "required",   boost::json::array{} }
                            }
                        }
                    }
                }
            }
        }
    };

    return value;
}

bool protocol_mcp::handle_tools_list(const code& ec,
    rpc_interface::tools_list, const std::string& /*cursor*/) NOEXCEPT
{
    if (stopped(ec))
        return false;

    send_result(boost::json::value{ tools_list_value() }, 8192);
    return true;
}

bool protocol_mcp::handle_tools_call(const code& ec,
    rpc_interface::tools_call, const std::string& name,
    const network::rpc::object_t& arguments,
    const network::rpc::object_t& /*meta*/) NOEXCEPT
{
    if (stopped(ec))
        return false;

    // Chain state.
    if (name == "get_blockchain_info")   return tool_get_blockchain_info();
    if (name == "get_configuration")     return tool_get_configuration();

    // Block.
    if (name == "get_block_header")      return tool_get_block_header(arguments);
    if (name == "get_block_header_context") return tool_get_block_header_context(arguments);
    if (name == "get_block_details")     return tool_get_block_details(arguments);
    if (name == "get_block_txs")         return tool_get_block_txs(arguments);
    if (name == "get_block_tx")          return tool_get_block_tx(arguments);
    if (name == "get_block_filter")      return tool_get_block_filter(arguments);
    if (name == "get_block_filter_hash") return tool_get_block_filter_hash(arguments);
    if (name == "get_block_filter_header") return tool_get_block_filter_header(arguments);

    // Transaction.
    if (name == "get_transaction")       return tool_get_transaction(arguments);
    if (name == "get_tx_header")         return tool_get_tx_header(arguments);
    if (name == "get_tx_details")        return tool_get_tx_details(arguments);

    // Inputs.
    if (name == "get_input")             return tool_get_input(arguments);

    // Outputs.
    if (name == "get_output")            return tool_get_output(arguments);
    if (name == "get_output_spender")    return tool_get_output_spender(arguments);
    if (name == "get_output_spenders")   return tool_get_output_spenders(arguments);

    // Address.
    if (name == "get_address_balance")   return tool_get_address_balance(arguments);
    if (name == "get_address_history")   return tool_get_address_history(arguments);
    if (name == "get_address_confirmed") return tool_get_address_confirmed(arguments);

    // Subscriptions (WebSocket only).
    if (name == "get_top_subscribe")     return tool_get_top_subscribe();
    if (name == "get_block_subscribe")   return tool_get_block_subscribe();
    if (name == "get_tx_subscribe")      return tool_get_tx_subscribe();

    send_error(error::not_found, name, name.size());
    return true;
}

// Argument parsing helpers.
// ----------------------------------------------------------------------------
// All static: no access to instance state needed.

static bool get_string_arg(const network::rpc::object_t& args,
    const std::string& key, std::string& out) NOEXCEPT
{
    const auto it = args.find(key);
    if (it == args.end()) return false;
    if (!std::holds_alternative<rpc::string_t>(it->second.value())) return false;
    out = std::get<rpc::string_t>(it->second.value());
    return true;
}

static bool get_number_arg(const network::rpc::object_t& args,
    const std::string& key, int64_t& out) NOEXCEPT
{
    const auto it = args.find(key);
    if (it == args.end()) return false;
    if (!std::holds_alternative<rpc::number_t>(it->second.value())) return false;
    out = static_cast<int64_t>(std::get<rpc::number_t>(it->second.value()));
    return true;
}

static bool get_hash_arg(const network::rpc::object_t& args,
    const std::string& key, hash_digest& out) NOEXCEPT
{
    std::string hex{};
    return get_string_arg(args, key, hex) && decode_hash(out, hex);
}

static database::header_link resolve_header(const auto& archive,
    const network::rpc::object_t& arguments) NOEXCEPT
{
    hash_digest hash{};
    if (get_hash_arg(arguments, "hash", hash))
        return archive.to_header(hash);

    int64_t height{};
    if (get_number_arg(arguments, "height", height) && height >= 0)
        return archive.to_confirmed(possible_sign_cast<size_t>(height));

    return {};
}

static bool get_txhash_arg(const network::rpc::object_t& arguments,
    hash_digest& hash) NOEXCEPT
{
    return get_hash_arg(arguments, "hash", hash);
}

static bool get_index_arg(const network::rpc::object_t& arguments,
    uint32_t& index) NOEXCEPT
{
    int64_t raw{};
    if (!get_number_arg(arguments, "index", raw) || raw < 0 ||
        raw > std::numeric_limits<uint32_t>::max()) return false;
    index = static_cast<uint32_t>(raw);
    return true;
}

// Tool implementations — chain state.
// ----------------------------------------------------------------------------

static boost::json::value make_tool_text(std::string&& text) NOEXCEPT
{
    return boost::json::object
    {
        { "content", boost::json::array
            {
                boost::json::object
                {
                    { "type", "text" },
                    { "text", std::move(text) }
                }
            }
        }
    };
}

bool protocol_mcp::tool_get_blockchain_info() NOEXCEPT
{
    const auto& query = archive();
    const auto height = query.get_top_confirmed();
    const auto link   = query.to_confirmed(height);
    const auto hash   = query.get_header_key(link);

    const boost::json::value info
    {
        boost::json::object
        {
            { "height", possible_sign_cast<int64_t>(height) },
            { "hash",   encode_hash(hash) }
        }
    };

    send_result(make_tool_text(boost::json::serialize(info)), 128);
    return true;
}

bool protocol_mcp::tool_get_configuration() NOEXCEPT
{
    BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)
    boost::json::object object
    {
        { "address", archive().address_enabled() },
        { "filter",  archive().filter_enabled() },
        { "turbo",   database_settings().turbo },
        { "witness", network_settings().witness_node() },
        { "retarget", system_settings().forks.retarget },
        { "difficult", system_settings().forks.difficult }
    };
    BC_POP_WARNING()

    send_result(make_tool_text(boost::json::serialize(object)), 128);
    return true;
}

// Tool implementations — block.
// ----------------------------------------------------------------------------

bool protocol_mcp::tool_get_block_header(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    const auto& query = archive();
    const auto link = resolve_header(query, arguments);

    if (const auto header = query.get_header(link))
    {
        size_t height{};
        query.get_height(height, link);

        auto model = value_from(header);
        model.as_object()["height"] = height;
        send_result(make_tool_text(boost::json::serialize(std::move(model))), 512);
        return true;
    }

    send_error(error::not_found);
    return true;
}

bool protocol_mcp::tool_get_block_header_context(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    database::context context{};
    const auto& query = archive();
    const auto link = resolve_header(query, arguments);

    if (!query.get_context(context, link))
    {
        send_error(error::not_found);
        return true;
    }

    boost::json::object object
    {
        { "hash",   encode_hash(query.get_header_key(link)) },
        { "height", static_cast<uint64_t>(context.height) },
        { "mtp",    static_cast<uint64_t>(context.mtp) }
    };

    if (query.is_associated(link))
    {
        size_t size{}, weight{};
        if (!query.get_block_sizes(size, weight, link))
        {
            send_error(database::error::integrity);
            return true;
        }

        const auto check   = system_settings().top_checkpoint().height();
        const auto bypass  = context.height < check || query.is_milestone(link);

        object["state"] = boost::json::object
        {
            { "size",           static_cast<uint64_t>(size) },
            { "weight",         static_cast<uint64_t>(weight) },
            { "count",          static_cast<uint64_t>(query.get_tx_count(link)) },
            { "validated",      static_cast<bool>(bypass || query.is_validated(link)) },
            { "confirmed",      static_cast<bool>(check || query.is_confirmed_block(link)) },
            { "confirmable",    static_cast<bool>(bypass || query.is_confirmable(link)) },
            { "unconfirmable",  static_cast<bool>(!bypass && query.is_unconfirmable(link)) }
        };
    }

    object["forks"] = boost::json::object
    {
        { "bip30",  static_cast<bool>(context.is_enabled(chain::flags::bip30_rule)) },
        { "bip34",  static_cast<bool>(context.is_enabled(chain::flags::bip34_rule)) },
        { "bip66",  static_cast<bool>(context.is_enabled(chain::flags::bip66_rule)) },
        { "bip65",  static_cast<bool>(context.is_enabled(chain::flags::bip65_rule)) },
        { "bip90",  static_cast<bool>(context.is_enabled(chain::flags::bip90_rule)) },
        { "bip68",  static_cast<bool>(context.is_enabled(chain::flags::bip68_rule)) },
        { "bip112", static_cast<bool>(context.is_enabled(chain::flags::bip112_rule)) },
        { "bip113", static_cast<bool>(context.is_enabled(chain::flags::bip113_rule)) },
        { "bip141", static_cast<bool>(context.is_enabled(chain::flags::bip141_rule)) },
        { "bip143", static_cast<bool>(context.is_enabled(chain::flags::bip143_rule)) },
        { "bip147", static_cast<bool>(context.is_enabled(chain::flags::bip147_rule)) },
        { "bip42",  static_cast<bool>(context.is_enabled(chain::flags::bip42_rule)) },
        { "bip341", static_cast<bool>(context.is_enabled(chain::flags::bip341_rule)) },
        { "bip342", static_cast<bool>(context.is_enabled(chain::flags::bip342_rule)) }
    };

    send_result(make_tool_text(boost::json::serialize(std::move(object))), 512);
    return true;
}

bool protocol_mcp::tool_get_block_details(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    const auto& query = archive();
    const auto link = resolve_header(query, arguments);

    if (!query.is_associated(link))
    {
        send_error(error::not_found);
        return true;
    }

    const auto count = query.get_tx_count(link);
    const auto key   = query.get_header_key(link);

    database::context context{};
    size_t nominal{}, maximal{};
    uint64_t value{}, spend{}, claim{};

    if (is_zero(count) || key == null_hash ||
        !query.get_tx_spend(claim, query.to_coinbase(link)) ||
        !query.get_block_sizes(nominal, maximal, link) ||
        !query.get_block_value(value, link) ||
        !query.get_block_spend(spend, link) ||
        !query.get_context(context, link) ||
        is_subtract_overflow(value, spend))
    {
        send_error(database::error::integrity);
        return true;
    }

    const auto fees    = floored_subtract(value, spend);
    const auto& sys    = system_settings();
    const auto bip42   = context.is_enabled(chain::flags::bip42_rule);
    const auto subsidy = chain::block::subsidy(context.height,
        sys.subsidy_interval_blocks, sys.initial_subsidy(), bip42);

    const boost::json::object object
    {
        { "hash",       encode_hash(key) },
        { "height",     static_cast<uint64_t>(context.height) },
        { "count",      static_cast<uint64_t>(count) },
        { "segregated", static_cast<bool>(maximal != nominal) },
        { "nominal",    static_cast<uint64_t>(nominal) },
        { "maximal",    static_cast<uint64_t>(maximal) },
        { "weight",     static_cast<uint64_t>(chain::weighted_size(nominal, maximal)) },
        { "virtual",    static_cast<uint64_t>(chain::virtual_size(nominal, maximal)) },
        { "value",      value },
        { "fees",       fees },
        { "subsidy",    subsidy },
        { "reward",     ceilinged_add(fees, subsidy) },
        { "claim",      claim }
    };

    send_result(make_tool_text(boost::json::serialize(object)), 512);
    return true;
}

bool protocol_mcp::tool_get_block_txs(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    const auto& query  = archive();
    const auto hashes  = query.get_tx_keys(resolve_header(query, arguments));
    const auto total   = hashes.size();

    if (is_zero(total))
    {
        send_error(error::not_found);
        return true;
    }

    // Optional pagination — defaults: offset=0, limit=25, max limit=100.
    int64_t raw_offset{}, raw_limit{ 25 };
    get_number_arg(arguments, "offset", raw_offset);
    get_number_arg(arguments, "limit",  raw_limit);
    if (raw_offset < 0) raw_offset = 0;
    if (raw_limit  < 1 || raw_limit > 100) raw_limit = 25;

    const auto offset = std::min(static_cast<size_t>(raw_offset), total);
    const auto count  = std::min(static_cast<size_t>(raw_limit),  total - offset);

    boost::json::array page(count);
    std::transform(hashes.begin() + offset, hashes.begin() + offset + count,
        page.begin(), [](const auto& h) { return encode_hash(h); });

    const boost::json::object out
    {
        { "total",  static_cast<uint64_t>(total)  },
        { "offset", static_cast<uint64_t>(offset) },
        { "limit",  static_cast<uint64_t>(count)  },
        { "hashes", std::move(page)                },
    };

    send_result(make_tool_text(boost::json::serialize(out)),
        two * count * hash_size);
    return true;
}

bool protocol_mcp::tool_get_block_tx(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    int64_t raw_pos{};
    if (!get_number_arg(arguments, "position", raw_pos) || raw_pos < 0)
    {
        send_error(error::invalid_argument);
        return true;
    }

    constexpr auto witness = true;
    const auto position = static_cast<uint32_t>(raw_pos);
    const auto& query   = archive();
    const auto link     = resolve_header(query, arguments);

    if (const auto tx = query.get_transaction(
            query.to_transaction(link, position), witness))
    {
        const auto size = tx->serialized_size(witness);
        send_result(make_tool_text(boost::json::serialize(value_from(tx))),
            two * size);
        return true;
    }

    send_error(error::not_found);
    return true;
}

bool protocol_mcp::tool_get_block_filter(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    const auto& query = archive();
    if (!query.filter_enabled())
    {
        send_error(error::not_implemented);
        return true;
    }

    data_chunk filter{};
    if (query.get_filter_body(filter, resolve_header(query, arguments)))
    {
        send_result(make_tool_text(boost::json::serialize(
            value_from(encode_base16(filter)))), two * filter.size());
        return true;
    }

    send_error(error::not_found);
    return true;
}

bool protocol_mcp::tool_get_block_filter_hash(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    const auto& query = archive();
    if (!query.filter_enabled())
    {
        send_error(error::not_implemented);
        return true;
    }

    hash_digest filter_hash{ hash_size };
    if (query.get_filter_hash(filter_hash, resolve_header(query, arguments)))
    {
        send_result(make_tool_text(boost::json::serialize(
            value_from(encode_hash(filter_hash)))), two * hash_size);
        return true;
    }

    send_error(error::not_found);
    return true;
}

bool protocol_mcp::tool_get_block_filter_header(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    const auto& query = archive();
    if (!query.filter_enabled())
    {
        send_error(error::not_implemented);
        return true;
    }

    hash_digest filter_head{ hash_size };
    if (query.get_filter_head(filter_head, resolve_header(query, arguments)))
    {
        send_result(make_tool_text(boost::json::serialize(
            value_from(encode_hash(filter_head)))), two * hash_size);
        return true;
    }

    send_error(error::not_found);
    return true;
}

// Tool implementations — transaction.
// ----------------------------------------------------------------------------

bool protocol_mcp::tool_get_transaction(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    hash_digest hash{};
    if (!get_txhash_arg(arguments, hash))
    {
        send_error(error::invalid_argument);
        return true;
    }

    constexpr auto witness = true;
    const auto& query = archive();
    const auto tx = query.get_transaction(query.to_tx(hash), witness);

    if (!tx)
    {
        send_error(error::not_found);
        return true;
    }

    const auto size = tx->serialized_size(witness);
    send_result(make_tool_text(boost::json::serialize(value_from(tx))),
        two * size);
    return true;
}

bool protocol_mcp::tool_get_tx_header(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    hash_digest hash{};
    if (!get_txhash_arg(arguments, hash))
    {
        send_error(error::invalid_argument);
        return true;
    }

    const auto& query = archive();
    const auto link   = query.find_confirmed_block(hash);
    if (link.is_terminal())
    {
        send_error(error::not_found);
        return true;
    }

    const auto header = query.get_header(link);
    if (!header)
    {
        send_error(database::error::integrity);
        return true;
    }

    size_t height{};
    query.get_height(height, link);

    auto model = value_from(header);
    model.as_object()["height"] = height;
    send_result(make_tool_text(boost::json::serialize(std::move(model))), 512);
    return true;
}

bool protocol_mcp::tool_get_tx_details(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    hash_digest hash{};
    if (!get_txhash_arg(arguments, hash))
    {
        send_error(error::invalid_argument);
        return true;
    }

    const auto& query = archive();
    const auto link   = query.to_tx(hash);
    if (link.is_terminal())
    {
        send_error(error::not_found);
        return true;
    }

    uint64_t value{}, spend{};
    size_t nominal{}, maximal{};
    const auto coinbase = query.is_coinbase(link);

    if (!query.get_tx_sizes(nominal, maximal, link) ||
        !query.get_tx_value(value, link) ||
        !query.get_tx_spend(spend, link) ||
        (!coinbase && is_subtract_overflow(value, spend)))
    {
        send_error(database::error::integrity);
        return true;
    }

    boost::json::object object
    {
        { "segregated", static_cast<bool>(maximal != nominal) },
        { "coinbase",   coinbase },
        { "nominal",    static_cast<uint64_t>(nominal) },
        { "maximal",    static_cast<uint64_t>(maximal) },
        { "weight",     static_cast<uint64_t>(chain::weighted_size(nominal, maximal)) },
        { "virtual",    static_cast<uint64_t>(chain::virtual_size(nominal, maximal)) },
        { "value",      value },
        { "spend",      spend },
        { "fee",        floored_subtract(value, spend) }
    };

    if (const auto block = query.find_strong(link); !block.is_terminal())
    {
        size_t position{};
        database::context context{};
        if (!query.get_context(context, block) ||
            !query.get_tx_position(position, link, block))
        {
            send_error(database::error::integrity);
            return true;
        }

        object["confirmed"] = boost::json::object
        {
            { "height",   static_cast<uint64_t>(context.height) },
            { "position", static_cast<uint64_t>(position) }
        };
    }

    send_result(make_tool_text(boost::json::serialize(std::move(object))), 256);
    return true;
}

// Tool implementations — inputs.
// ----------------------------------------------------------------------------

bool protocol_mcp::tool_get_input(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    hash_digest hash{};
    uint32_t index{};
    if (!get_txhash_arg(arguments, hash) || !get_index_arg(arguments, index))
    {
        send_error(error::invalid_argument);
        return true;
    }

    constexpr auto witness = true;
    const auto& query = archive();
    if (const auto input = query.get_input(query.to_tx(hash), index, witness))
    {
        const auto size = input->serialized_size(witness);
        send_result(make_tool_text(boost::json::serialize(value_from(input))),
            two * size);
        return true;
    }

    send_error(error::not_found);
    return true;
}

// Tool implementations — outputs.
// ----------------------------------------------------------------------------

bool protocol_mcp::tool_get_output(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    hash_digest hash{};
    uint32_t index{};
    if (!get_txhash_arg(arguments, hash) || !get_index_arg(arguments, index))
    {
        send_error(error::invalid_argument);
        return true;
    }

    const auto& query = archive();
    if (const auto output = query.get_output(query.to_tx(hash), index))
    {
        const auto size = output->serialized_size();
        send_result(make_tool_text(boost::json::serialize(value_from(output))),
            two * size);
        return true;
    }

    send_error(error::not_found);
    return true;
}

bool protocol_mcp::tool_get_output_spender(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    hash_digest hash{};
    uint32_t index{};
    if (!get_txhash_arg(arguments, hash) || !get_index_arg(arguments, index))
    {
        send_error(error::invalid_argument);
        return true;
    }

    const auto& query = archive();
    const chain::point spent{ hash, index };
    const auto spender = query.get_spender(query.find_confirmed_spender(spent));

    if (spender.is_null())
    {
        send_error(error::not_found);
        return true;
    }

    constexpr auto size = chain::point::serialized_size();
    send_result(make_tool_text(boost::json::serialize(value_from(spender))),
        two * size);
    return true;
}

bool protocol_mcp::tool_get_output_spenders(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    hash_digest hash{};
    uint32_t index{};
    if (!get_txhash_arg(arguments, hash) || !get_index_arg(arguments, index))
    {
        send_error(error::invalid_argument);
        return true;
    }

    const auto ins = archive().get_spenders({ hash, index });
    if (ins.empty())
    {
        send_error(error::not_found);
        return true;
    }

    const auto size = ins.size() * database::inpoint::serialized_size();
    send_result(make_tool_text(boost::json::serialize(value_from(ins))),
        two * size);
    return true;
}

// Tool implementations — address (async).
// ----------------------------------------------------------------------------

bool protocol_mcp::dispatch_address_outpoints(
    const network::rpc::object_t& arguments,
    bool confirmed_only) NOEXCEPT
{
    if (!archive().address_enabled())
    {
        send_error(error::not_implemented);
        return true;
    }

    hash_digest hash{};
    if (!get_hash_arg(arguments, "address", hash))
    {
        send_error(error::invalid_argument);
        return true;
    }

    auto ptr = std::make_shared<hash_digest>(std::move(hash));
    monitor(true);
    PARALLEL(do_address_outpoints, confirmed_only, ptr);
    return true;
}

void protocol_mcp::do_address_outpoints(bool confirmed_only,
    const system::hash_cptr& hash) NOEXCEPT
{
    BC_ASSERT(!stranded());

    database::outpoints set{};
    const auto& query = archive();
    const auto ec = confirmed_only
        ? query.get_confirmed_unspent_outpoints(stopping_, set, *hash, true)
        : query.get_address_outpoints(stopping_, set, *hash, true);

    POST(complete_address_outpoints, ec, std::move(set));
}

void protocol_mcp::complete_address_outpoints(const code& ec,
    database::outpoints set) NOEXCEPT
{
    BC_ASSERT(stranded());
    monitor(false);

    if (stopped())
        return;

    if (ec)
    {
        send_error(ec);
        return;
    }

    if (set.empty())
    {
        send_error(error::not_found);
        return;
    }

    const auto size = set.size() * chain::outpoint::serialized_size();
    send_result(make_tool_text(boost::json::serialize(value_from(set))),
        two * size);
}

bool protocol_mcp::tool_get_address_history(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    return dispatch_address_outpoints(arguments, false);
}

bool protocol_mcp::tool_get_address_confirmed(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    return dispatch_address_outpoints(arguments, true);
}

bool protocol_mcp::tool_get_address_balance(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    if (!archive().address_enabled())
    {
        send_error(error::not_implemented);
        return true;
    }

    hash_digest hash{};
    if (!get_hash_arg(arguments, "address", hash))
    {
        send_error(error::invalid_argument);
        return true;
    }

    auto ptr = std::make_shared<hash_digest>(std::move(hash));
    monitor(true);
    PARALLEL(do_address_balance, ptr);
    return true;
}

void protocol_mcp::do_address_balance(
    const system::hash_cptr& hash) NOEXCEPT
{
    BC_ASSERT(!stranded());

    uint64_t balance{};
    const auto ec = archive().get_confirmed_balance(stopping_, balance, *hash, true);
    POST(complete_address_balance, ec, balance);
}

void protocol_mcp::complete_address_balance(const code& ec,
    uint64_t balance) NOEXCEPT
{
    BC_ASSERT(stranded());
    monitor(false);

    if (stopped())
        return;

    if (ec)
    {
        send_error(ec);
        return;
    }

    const boost::json::object object{ { "satoshis", balance } };
    send_result(make_tool_text(boost::json::serialize(object)), 64);
}

// Tool implementations — subscriptions (WebSocket only).
// ----------------------------------------------------------------------------

bool protocol_mcp::tool_get_top_subscribe() NOEXCEPT
{
    const auto& query  = archive();
    const auto height  = query.get_top_confirmed();
    const auto link    = query.to_confirmed(height);
    const auto hash    = query.get_header_key(link);

    const boost::json::object current
    {
        { "height",     possible_sign_cast<int64_t>(height) },
        { "hash",       encode_hash(hash) },
        { "subscribed", true }
    };

    top_subscribe_.store(true, std::memory_order_relaxed);

    if (websocket())
    {
        send_result(make_tool_text(boost::json::serialize(current)), 128);
        return true;
    }

    // HTTP: open SSE stream; first event carries the tool result.
    const auto version = version_;
    const auto id      = id_;
    const auto request = reset_rpc_request();
    rpc::response_t model
    {
        .jsonrpc = version,
        .id      = id,
        .result  = make_tool_text(boost::json::serialize(current))
    };
    sse_initial_event_ = "data: " +
        boost::json::serialize(value_from(std::move(model))) + "\n\n";
    sse_ = std::make_shared<network::socket::sse_state>(request->version());
    SSE_START(sse_, handle_sse_start, _1, _2);
    return true;
}

bool protocol_mcp::tool_get_block_subscribe() NOEXCEPT
{
    const auto& query = archive();
    const auto link   = query.to_confirmed(query.get_top_confirmed());
    const auto hash   = query.get_header_key(link);

    const boost::json::object current
    {
        { "hash",       encode_hash(hash) },
        { "subscribed", true }
    };

    block_subscribe_.store(true, std::memory_order_relaxed);

    if (websocket())
    {
        send_result(make_tool_text(boost::json::serialize(current)), 128);
        return true;
    }

    // HTTP: open SSE stream; first event carries the tool result.
    const auto version = version_;
    const auto id      = id_;
    const auto request = reset_rpc_request();
    rpc::response_t model
    {
        .jsonrpc = version,
        .id      = id,
        .result  = make_tool_text(boost::json::serialize(current))
    };
    sse_initial_event_ = "data: " +
        boost::json::serialize(value_from(std::move(model))) + "\n\n";
    sse_ = std::make_shared<network::socket::sse_state>(request->version());
    SSE_START(sse_, handle_sse_start, _1, _2);
    return true;
}

bool protocol_mcp::tool_get_tx_subscribe() NOEXCEPT
{
    const boost::json::object current{ { "subscribed", true } };
    tx_subscribe_.store(true, std::memory_order_relaxed);

    if (websocket())
    {
        send_result(make_tool_text(boost::json::serialize(current)), 64);
        return true;
    }

    // HTTP: open SSE stream; first event carries the tool result.
    const auto version = version_;
    const auto id      = id_;
    const auto request = reset_rpc_request();
    rpc::response_t model
    {
        .jsonrpc = version,
        .id      = id,
        .result  = make_tool_text(boost::json::serialize(current))
    };
    sse_initial_event_ = "data: " +
        boost::json::serialize(value_from(std::move(model))) + "\n\n";
    sse_ = std::make_shared<network::socket::sse_state>(request->version());
    SSE_START(sse_, handle_sse_start, _1, _2);
    return true;
}

// WebSocket push notifiers (called via POST from handle_chase).
// ----------------------------------------------------------------------------

void protocol_mcp::do_top(node::header_t link) NOEXCEPT
{
    BC_ASSERT(stranded());

    if (stopped())
        return;

    const auto& query = archive();
    const auto height = query.get_height(link).value;
    const auto hash   = query.get_header_key(link);

    boost::json::value notification
    {
        boost::json::object
        {
            { "jsonrpc", "2.0" },
            { "method",  "bitcoin/top" },
            { "params",  boost::json::object
                {
                    { "height", possible_sign_cast<int64_t>(height) },
                    { "hash",   encode_hash(hash) }
                }
            }
        }
    };

    if (websocket())
    {
        notify_json(std::move(notification), 128);
        return;
    }

    if (sse_ && !sse_writing_)
    {
        sse_writing_ = true;
        auto event = "data: " + boost::json::serialize(notification) + "\n\n";
        SSE_WRITE(sse_, std::move(event), handle_sse_write, _1, _2);
    }
}

void protocol_mcp::do_block(node::header_t link) NOEXCEPT
{
    BC_ASSERT(stranded());

    if (stopped())
        return;

    const auto hash = archive().get_header_key(link);
    boost::json::value notification
    {
        boost::json::object
        {
            { "jsonrpc", "2.0" },
            { "method",  "bitcoin/block" },
            { "params",  boost::json::object{ { "hash", encode_hash(hash) } } }
        }
    };

    if (websocket())
    {
        notify_json(std::move(notification), 128);
        return;
    }

    if (sse_ && !sse_writing_)
    {
        sse_writing_ = true;
        auto event = "data: " + boost::json::serialize(notification) + "\n\n";
        SSE_WRITE(sse_, std::move(event), handle_sse_write, _1, _2);
    }
}

void protocol_mcp::do_transaction(node::transaction_t link) NOEXCEPT
{
    BC_ASSERT(stranded());

    if (stopped())
        return;

    const auto hash = archive().get_tx_key(link);
    boost::json::value notification
    {
        boost::json::object
        {
            { "jsonrpc", "2.0" },
            { "method",  "bitcoin/transaction" },
            { "params",  boost::json::object{ { "hash", encode_hash(hash) } } }
        }
    };

    if (websocket())
    {
        notify_json(std::move(notification), 128);
        return;
    }

    if (sse_ && !sse_writing_)
    {
        sse_writing_ = true;
        auto event = "data: " + boost::json::serialize(notification) + "\n\n";
        SSE_WRITE(sse_, std::move(event), handle_sse_write, _1, _2);
    }
}

// SSE completion handlers.
// ----------------------------------------------------------------------------

void protocol_mcp::handle_sse_start(const code& ec, size_t) NOEXCEPT
{
    BC_ASSERT(stranded());
    if (stopped(ec))
    {
        sse_.reset();
        return;
    }
    sse_writing_ = true;
    SSE_WRITE(sse_, std::move(sse_initial_event_), handle_sse_write, _1, _2);
}

void protocol_mcp::handle_sse_write(const code& ec, size_t) NOEXCEPT
{
    BC_ASSERT(stranded());
    sse_writing_ = false;
    if (stopped(ec))
    {
        sse_.reset();
        return;
    }
    // On success: stay open, next push driven by handle_chase.
    // Start keepalive timer on first write (after initial event is sent).
    if (!keepalive_)
        start_keepalive();
}

void protocol_mcp::start_keepalive() NOEXCEPT
{
    BC_ASSERT(stranded());
    using namespace std::chrono;
    keepalive_ = std::make_shared<network::deadline>(log, strand(), minutes(5));
    keepalive_->start(BIND(handle_keepalive, _1));
}

void protocol_mcp::handle_keepalive(const code& ec) NOEXCEPT
{
    BC_ASSERT(stranded());
    if (stopped(ec) || !sse_)
        return;

    // A real event is in-flight; that write already resets the inactivity
    // timer via channel::writing(). Just reschedule without sending a comment.
    if (sse_writing_)
    {
        keepalive_->start(BIND(handle_keepalive, _1));
        return;
    }

    sse_writing_ = true;
    SSE_WRITE(sse_, std::string(":\n\n"), handle_sse_keepalive, _1, _2);
}

void protocol_mcp::handle_sse_keepalive(const code& ec, size_t) NOEXCEPT
{
    BC_ASSERT(stranded());
    sse_writing_ = false;
    if (stopped(ec))
    {
        sse_.reset();
        return;
    }
    keepalive_->start(BIND(handle_keepalive, _1));
}

// Senders.
// ----------------------------------------------------------------------------

void protocol_mcp::send_error(const code& ec) NOEXCEPT
{
    send_error(ec, two * ec.message().size());
}

void protocol_mcp::send_error(const code& ec, size_t size_hint) NOEXCEPT
{
    send_error(ec, {}, size_hint);
}

void protocol_mcp::send_error(const code& ec, rpc::value_option&& error,
    size_t size_hint) NOEXCEPT
{
    BC_ASSERT(stranded());
    send_rpc(
    {
        .jsonrpc = version_,
        .id = id_,
        .error = rpc::result_t
        {
            .code    = ec.value(),
            .message = ec.message(),
            .data    = std::move(error)
        }
    }, size_hint);
}

void protocol_mcp::send_result(rpc::value_option&& result,
    size_t size_hint) NOEXCEPT
{
    BC_ASSERT(stranded());
    send_rpc(
    {
        .jsonrpc = version_,
        .id = id_,
        .result = std::move(result)
    }, size_hint);
}

// private
void protocol_mcp::notify_json(boost::json::value&& model,
    size_t /*size_hint*/) NOEXCEPT
{
    BC_ASSERT(stranded() && websocket());
    using namespace http;
    response response{ status::ok, 11u };
    response.body() = string_value{
        boost::json::serialize(std::move(model))
    };
    NOTIFY(std::move(response), handle_complete, _1, error::success);
}

// private
void protocol_mcp::send_rpc(rpc::response_t&& model,
    size_t size_hint) NOEXCEPT
{
    BC_ASSERT(stranded());

    if (websocket())
    {
        ws_response_sent_ = true;
        version_ = rpc::version::undefined;
        id_.reset();
        // Use SEND (not NOTIFY): handle_send restarts receive() after the WS
        // frame is written. NOTIFY is reserved for server-push where the read
        // loop is already running and no receive() restart is needed.
        //
        // Use string_value (pre-serialized) rather than json_value+size_hint:
        // body_write chunks by size_hint; each chunk becomes a separate WS
        // frame. The client's ws.recv() returns the first frame → truncated
        // JSON. string_body::writer returns the full string in one get() call
        // → body_write sends a single WS frame regardless of response size.
        using namespace http;
        response message{ status::ok, 11u };
        message.body() = string_value{
            boost::json::serialize(value_from(std::move(model)))
        };
        SEND(std::move(message), handle_complete, _1, error::success);
        return;
    }

    using namespace http;
    static const auto json = from_media_type(media_type::application_json);
    const auto request = reset_rpc_request();
    http::response message{ status::ok, request->version() };
    add_common_headers(message, *request);
    add_access_control_headers(message, *request);
    message.set(field::content_type, json);
    message.body() = rpc::response
    {
        { .size_hint = size_hint }, std::move(model),
    };
    message.prepare_payload();
    SEND(std::move(message), handle_complete, _1, error::success);
}

// private
void protocol_mcp::set_rpc_request(rpc::version version,
    const rpc::id_option& id, const http::request_cptr& request) NOEXCEPT
{
    BC_ASSERT(stranded());
    id_      = id;
    version_ = version;
    set_request(request);
}

// private
http::request_cptr protocol_mcp::reset_rpc_request() NOEXCEPT
{
    BC_ASSERT(stranded());
    id_.reset();
    version_ = rpc::version::undefined;
    return reset_request();
}

BC_POP_WARNING()
BC_POP_WARNING()
BC_POP_WARNING()

} // namespace server
} // namespace libbitcoin

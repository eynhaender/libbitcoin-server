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

    SUBSCRIBE_MCP(handle_initialize, _1, _2, _3, _4, _5);
    SUBSCRIBE_MCP(handle_notifications_initialized, _1, _2);
    SUBSCRIBE_MCP(handle_tools_list, _1, _2, _3);
    SUBSCRIBE_MCP(handle_tools_call, _1, _2, _3, _4);
    network::protocol_http::start();
}

void protocol_mcp::stopping(const code& ec) NOEXCEPT
{
    BC_ASSERT(stranded());
    rpc_dispatcher_.stop(ec);
    network::protocol_http::stopping(ec);
}

// Dispatch.
// ----------------------------------------------------------------------------

void protocol_mcp::handle_receive_options(const code& ec,
    const options::cptr& options) NOEXCEPT
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
    const post::cptr& post) NOEXCEPT
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
                        { "name",        "get_block" },
                        { "description", "Returns block data at the given height." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "height", boost::json::object
                                            {
                                                { "type",        "number" },
                                                { "description", "Block height (zero-based)" }
                                            }
                                        }
                                    }
                                },
                                { "required", boost::json::array{ "height" } }
                            }
                        }
                    },
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
                        { "name",        "get_address_balance" },
                        { "description", "Returns the confirmed satoshi balance for a Bitcoin address (requires address indexing)." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "address", boost::json::object
                                            {
                                                { "type",        "string" },
                                                { "description", "Bitcoin address (base58check)" }
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
                        { "description", "Returns confirmed transactions for a Bitcoin address (requires address indexing)." },
                        { "inputSchema", boost::json::object
                            {
                                { "type",       "object" },
                                { "properties", boost::json::object
                                    {
                                        { "address", boost::json::object
                                            {
                                                { "type",        "string" },
                                                { "description", "Bitcoin address (base58check)" }
                                            }
                                        }
                                    }
                                },
                                { "required", boost::json::array{ "address" } }
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

    send_result(boost::json::value{ tools_list_value() }, 2048);
    return true;
}

bool protocol_mcp::handle_tools_call(const code& ec,
    rpc_interface::tools_call, const std::string& name,
    const network::rpc::object_t& arguments) NOEXCEPT
{
    if (stopped(ec))
        return false;

    if (name == "get_blockchain_info")
        return tool_get_blockchain_info();
    if (name == "get_block")
        return tool_get_block(arguments);
    if (name == "get_transaction")
        return tool_get_transaction(arguments);
    if (name == "get_address_balance")
        return tool_get_address_balance(arguments);
    if (name == "get_address_history")
        return tool_get_address_history(arguments);

    send_error(error::not_found, name, name.size());
    return true;
}

// Tool implementations.
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

bool protocol_mcp::tool_get_block(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    const auto it = arguments.find("height");
    if (it == arguments.end() ||
        !std::holds_alternative<rpc::number_t>(it->second.value()))
    {
        send_error(error::invalid_argument);
        return true;
    }

    const auto height = possible_sign_cast<size_t>(static_cast<int64_t>(
        std::get<rpc::number_t>(it->second.value())));

    constexpr auto witness = true;
    const auto& query = archive();
    const auto block = query.get_block(query.to_confirmed(height), witness);

    if (!block)
    {
        send_error(error::not_found);
        return true;
    }

    const auto size = block->serialized_size(witness);
    send_result(make_tool_text(boost::json::serialize(value_from(block))),
        two * size);
    return true;
}

bool protocol_mcp::tool_get_transaction(
    const network::rpc::object_t& arguments) NOEXCEPT
{
    const auto it = arguments.find("hash");
    if (it == arguments.end() ||
        !std::holds_alternative<rpc::string_t>(it->second.value()))
    {
        send_error(error::invalid_argument);
        return true;
    }

    hash_digest hash{};
    if (!decode_hash(hash, std::get<rpc::string_t>(it->second.value())))
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

bool protocol_mcp::tool_get_address_balance(
    const network::rpc::object_t& /*arguments*/) NOEXCEPT
{
    send_error(error::not_implemented);
    return true;
}

bool protocol_mcp::tool_get_address_history(
    const network::rpc::object_t& /*arguments*/) NOEXCEPT
{
    send_error(error::not_implemented);
    return true;
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
void protocol_mcp::send_rpc(rpc::response_t&& model,
    size_t size_hint) NOEXCEPT
{
    BC_ASSERT(stranded());
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

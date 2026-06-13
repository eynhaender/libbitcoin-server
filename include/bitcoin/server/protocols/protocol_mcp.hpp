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
#ifndef LIBBITCOIN_SERVER_PROTOCOLS_PROTOCOL_MCP_HPP
#define LIBBITCOIN_SERVER_PROTOCOLS_PROTOCOL_MCP_HPP

#include <memory>
#include <bitcoin/server/channels/channels.hpp>
#include <bitcoin/server/define.hpp>
#include <bitcoin/server/interfaces/interfaces.hpp>
#include <bitcoin/server/protocols/protocol_http.hpp>

namespace libbitcoin {
namespace server {

class BCS_API protocol_mcp
  : public server::protocol_http,
    protected network::tracker<protocol_mcp>
{
public:
    // Replace base class channel_t (network::channel_http).
    using channel_t = channel_http;

    typedef std::shared_ptr<protocol_mcp> ptr;
    using rpc_interface = interface::mcp;
    using rpc_dispatcher = network::rpc::dispatcher<rpc_interface>;

    inline protocol_mcp(const auto& session,
        const network::channel::ptr& channel,
        const options_t& options) NOEXCEPT
      : server::protocol_http(session, channel, options),
        network::tracker<protocol_mcp>(session->log)
    {
    }

    void start() NOEXCEPT override;
    void stopping(const code& ec) NOEXCEPT override;

protected:
    using post = network::http::method::post;
    using options = network::http::method::options;

    /// Dispatch.
    void handle_receive_options(const code& ec,
        const network::http::method::options::cptr& options) NOEXCEPT override;
    void handle_receive_post(const code& ec,
        const post::cptr& post) NOEXCEPT override;

    /// Handlers — session lifecycle.
    bool handle_initialize(const code& ec,
        rpc_interface::initialize,
        const std::string& protocol_version,
        const network::rpc::object_t& capabilities,
        const network::rpc::object_t& client_info) NOEXCEPT;
    bool handle_notifications_initialized(const code& ec,
        rpc_interface::notifications_initialized) NOEXCEPT;

    /// Handlers — tool discovery and invocation.
    bool handle_tools_list(const code& ec,
        rpc_interface::tools_list,
        const std::string& cursor) NOEXCEPT;
    bool handle_tools_call(const code& ec,
        rpc_interface::tools_call,
        const std::string& name,
        const network::rpc::object_t& arguments) NOEXCEPT;

    /// Senders.
    void send_error(const code& ec) NOEXCEPT;
    void send_error(const code& ec, size_t size_hint) NOEXCEPT;
    void send_error(const code& ec, network::rpc::value_option&& error,
        size_t size_hint) NOEXCEPT;
    void send_result(network::rpc::value_option&& result,
        size_t size_hint) NOEXCEPT;

    /// Tool call dispatch (called from handle_tools_call).
    bool tool_get_blockchain_info() NOEXCEPT;
    bool tool_get_block(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_transaction(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_address_balance(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_address_history(const network::rpc::object_t& arguments) NOEXCEPT;

private:
    template <class Derived, typename Method, typename... Args>
    inline void subscribe(Method&& method, Args&&... args) NOEXCEPT
    {
        rpc_dispatcher_.subscribe(BIND_SHARED(method, args));
    }

    // Senders.
    void send_rpc(network::rpc::response_t&& model,
        size_t size_hint) NOEXCEPT;

    // Cache request for serialization (requires strand).
    void set_rpc_request(network::rpc::version version,
        const network::rpc::id_option& id,
        const network::http::request_cptr& request) NOEXCEPT;

    // Obtain cached request and clear cache (requires strand).
    network::http::request_cptr reset_rpc_request() NOEXCEPT;

    // These are protected by strand.
    rpc_dispatcher rpc_dispatcher_{};
    network::rpc::version version_{};
    network::rpc::id_option id_{};
};

} // namespace server
} // namespace libbitcoin

#endif

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

#include <atomic>
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
    /// Dispatch (HTTP).
    void handle_receive_options(const code& ec,
        const network::http::method::options::cptr& options) NOEXCEPT override;
    void handle_receive_post(const code& ec,
        const network::http::method::post::cptr& post) NOEXCEPT override;

    /// Dispatch (WebSocket).
    void dispatch_websocket(
        const network::http::request& request) NOEXCEPT override;

    /// Event handler for chain events — drives WS push subscriptions.
    bool handle_chase(const code& ec, node::chase event_,
        node::event_value value) NOEXCEPT;

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
        const network::rpc::object_t& arguments,
        const network::rpc::object_t& meta) NOEXCEPT;

    /// Senders.
    void send_error(const code& ec) NOEXCEPT;
    void send_error(const code& ec, size_t size_hint) NOEXCEPT;
    void send_error(const code& ec, network::rpc::value_option&& error,
        size_t size_hint) NOEXCEPT;
    void send_result(network::rpc::value_option&& result,
        size_t size_hint) NOEXCEPT;

    /// Tool call dispatch (called from handle_tools_call).
    bool tool_get_blockchain_info() NOEXCEPT;
    bool tool_get_configuration() NOEXCEPT;

    bool tool_get_block_header(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_block_header_context(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_block_details(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_block_txs(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_block_tx(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_block_filter(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_block_filter_hash(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_block_filter_header(const network::rpc::object_t& arguments) NOEXCEPT;

    bool tool_get_transaction(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_tx_header(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_tx_details(const network::rpc::object_t& arguments) NOEXCEPT;

    bool tool_get_input(const network::rpc::object_t& arguments) NOEXCEPT;

    bool tool_get_output(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_output_spender(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_output_spenders(const network::rpc::object_t& arguments) NOEXCEPT;

    bool tool_get_address_balance(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_address_history(const network::rpc::object_t& arguments) NOEXCEPT;
    bool tool_get_address_confirmed(const network::rpc::object_t& arguments) NOEXCEPT;

    /// Subscription tools (WebSocket only — error::not_implemented over HTTP).
    bool tool_get_top_subscribe() NOEXCEPT;
    bool tool_get_block_subscribe() NOEXCEPT;
    bool tool_get_tx_subscribe() NOEXCEPT;

    /// Push notifiers (called via POST from handle_chase).
    void do_top(node::header_t link) NOEXCEPT;
    void do_block(node::header_t link) NOEXCEPT;
    void do_transaction(node::transaction_t link) NOEXCEPT;

    /// SSE completion handlers for HTTP subscription streaming.
    void handle_sse_start(const code& ec, size_t bytes) NOEXCEPT;
    void handle_sse_write(const code& ec, size_t bytes) NOEXCEPT;

    /// SSE keepalive — sends a comment every 5 minutes to prevent idle timeout.
    void start_keepalive() NOEXCEPT;
    void handle_keepalive(const code& ec) NOEXCEPT;
    void handle_sse_keepalive(const code& ec, size_t bytes) NOEXCEPT;

private:
    /// Async address query helpers (run off-strand via PARALLEL, complete on-strand via POST).
    bool dispatch_address_outpoints(const network::rpc::object_t& arguments,
        bool confirmed_only) NOEXCEPT;
    void do_address_outpoints(bool confirmed_only,
        const system::hash_cptr& hash) NOEXCEPT;
    void complete_address_outpoints(const code& ec,
        database::outpoints set) NOEXCEPT;
    void do_address_balance(const system::hash_cptr& hash) NOEXCEPT;
    void complete_address_balance(const code& ec, uint64_t balance) NOEXCEPT;
    template <class Derived, typename Method, typename... Args>
    inline void subscribe(Method&& method, Args&&... args) NOEXCEPT
    {
        rpc_dispatcher_.subscribe(BIND_SHARED(method, args));
    }

    // WebSocket push — mirrors protocol_html::notify_json without changing options_t.
    void notify_json(boost::json::value&& model, size_t size_hint) NOEXCEPT;

    // Senders.
    void send_rpc(network::rpc::response_t&& model,
        size_t size_hint) NOEXCEPT;

    // True when send_rpc was called during the current dispatch_websocket call.
    // Cleared at the start of each WS frame dispatch; set in send_rpc WS branch.
    // Lets dispatch_websocket call resume() to restart the read loop for WS
    // notifications where no response is sent (e.g. notifications/initialized).
    bool ws_response_sent_{};

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

    // Thread safe — signals long-running address queries to abort.
    std::atomic_bool stopping_{};

    // Thread safe — active push subscriptions (WS or SSE, unknown = inactive).
    std::atomic_bool top_subscribe_{};
    std::atomic_bool block_subscribe_{};
    std::atomic_bool tx_subscribe_{};

    // SSE stream for HTTP subscription tools (protected by strand).
    network::socket::sse_state::ptr sse_{};
    std::string sse_initial_event_{};
    bool sse_writing_{};
    network::deadline::ptr keepalive_{};
};

} // namespace server
} // namespace libbitcoin

#endif

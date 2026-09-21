/**
 * Copyright (c) 2011-2026 libbitcoin developers
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
#include <map>
#include <string_view>
#include <bitcoin/server/channels/channels.hpp>
#include <bitcoin/server/define.hpp>
#include <bitcoin/server/interfaces/interfaces.hpp>
#include <bitcoin/server/protocols/protocol_rpc.hpp>

namespace libbitcoin {
namespace server {

/// Model Context Protocol (mcp) limited to address subscription. An address
/// is a native address resource (native:///v1/address/[hash]), which may be
/// read (native address) and subscribed. A subscription is notified (over
/// websocket) of each new confirmed payment, as it is by btcd notifyreceived.
/// A payment is an outpoint, in the json form of native address.
class BCS_API protocol_mcp
  : public protocol_rpc<interface::mcp>,
    protected network::tracker<protocol_mcp>
{
public:
    typedef std::shared_ptr<protocol_mcp> ptr;
    using rpc_interface = interface::mcp;
    using channel_t = channel_mcp;
    using options_t = channel_t::options_t;

    /// The mcp revision served.
    static constexpr std::string_view protocol_version{ "2025-06-18" };

    /// The resource identity of an address (scheme and native target).
    static constexpr std::string_view address_uri{ "native:///v1/address/" };

    inline protocol_mcp(const auto& session,
        const network::channel::ptr& channel,
        const options_t& options) NOEXCEPT
      : protocol_rpc<interface::mcp>(session, channel, options),
        options_(options),
        turbo_(session->database_settings().turbo),
        notification_strand_(channel->service().get_executor()),
        network::tracker<protocol_mcp>(session->log)
    {
    }

    void start() NOEXCEPT override;
    void stopping(const code& ec) NOEXCEPT override;

protected:
    /// Terminal responder (sole protocol) for unclaimed methods.
    void handle_unclaimed(
        const network::rpc::request_t& request) NOEXCEPT override;

    /// Event handlers.
    bool handle_chase(const code&, node::chase event_,
        node::event_value value) NOEXCEPT;

    /// Handlers (lifecycle).
    void handle_initialize(const code& ec, rpc_interface::initialize,
        const std::string& protocol_version,
        const interface::object_t& capabilities,
        const interface::object_t& client_info) NOEXCEPT;
    void handle_initialized(const code& ec,
        rpc_interface::notifications_initialized) NOEXCEPT;
    void handle_ping(const code& ec, rpc_interface::ping) NOEXCEPT;

    /// Handlers (resources).
    void handle_resources_list(const code& ec,
        rpc_interface::resources_list, const std::string& cursor) NOEXCEPT;
    void handle_resources_templates_list(const code& ec,
        rpc_interface::resources_templates_list,
        const std::string& cursor) NOEXCEPT;
    void handle_resources_read(const code& ec,
        rpc_interface::resources_read, const std::string& uri) NOEXCEPT;
    void handle_resources_subscribe(const code& ec,
        rpc_interface::resources_subscribe, const std::string& uri) NOEXCEPT;
    void handle_resources_unsubscribe(const code& ec,
        rpc_interface::resources_unsubscribe,
        const std::string& uri) NOEXCEPT;

protected:
    using array_t = network::rpc::array_t;
    using object_t = network::rpc::object_t;
    using array_ptr = std::shared_ptr<array_t>;
    using hash_digest = system::hash_digest;
    using histories = database::histories;
    using cursor_t = database::height_link;

    // Subscription to address.
    struct address_watch final
    {
        // Advances through the address history.
        cursor_t cursor{};

        // Payments confirmed at or below this height are not notified.
        size_t floor{};
    };

    /// Completion handlers (for long-running or other async queries).
    /// -----------------------------------------------------------------------

    void do_read(const std::string& uri, const hash_digest& hash) NOEXCEPT;
    void complete_read(const code& ec, const std::string& uri,
        const database::outpoints& set) NOEXCEPT;

    void do_subscribe(const hash_digest& hash) NOEXCEPT;
    void complete_subscribe(const code& ec) NOEXCEPT;
    void do_unsubscribe(const hash_digest& hash) NOEXCEPT;
    void complete_unsubscribe() NOEXCEPT;

    /// Notification event handlers.
    /// -----------------------------------------------------------------------

    void do_connected(node::header_t link) NOEXCEPT;
    void do_disconnected(node::header_t link) NOEXCEPT;
    void notify_updated(const hash_digest& hash,
        const array_ptr& outpoints) NOEXCEPT;

    /// Utilities.
    /// -----------------------------------------------------------------------

    /// Extract the address hash from an address resource identity.
    static bool to_hash(hash_digest& out, const std::string& uri) NOEXCEPT;

    /// Configuration options.
    inline const options_t& options() const NOEXCEPT
    {
        return options_;
    }

private:
    // Post to notification strand.
    template <class Derived, typename Method, typename... Args>
    inline auto notify(Method&& method, Args&&... args) NOEXCEPT
    {
        return boost::asio::post(notification_strand_,
            BIND_SAFE(BIND_SHARED(method, args)));
    }

    // These are thread safe.
    const options_t& options_;
    const bool turbo_;
    std::atomic_bool stopping_{};
    std::atomic_bool subscribed_{};

    // This is thread safe, uses network threadpool.
    network::asio::strand notification_strand_;

    // This is protected by notification strand.
    std::map<hash_digest, address_watch> watches_{};
};

} // namespace server
} // namespace libbitcoin

#endif

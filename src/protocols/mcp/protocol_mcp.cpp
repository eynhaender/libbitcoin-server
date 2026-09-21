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
#include <bitcoin/server/protocols/protocol_mcp.hpp>

#include <algorithm>
#include <atomic>
#include <bitcoin/server/define.hpp>
#include <bitcoin/server/interfaces/interfaces.hpp>
#include <bitcoin/server/protocols/protocol_rpc.hpp>

namespace libbitcoin {
namespace server {

#define CLASS protocol_mcp

using namespace system;
using namespace network::rpc;
using namespace std::placeholders;
constexpr auto relaxed = std::memory_order_relaxed;

BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)

// Start.
// ----------------------------------------------------------------------------
// modelcontextprotocol.io/specification/2025-06-18

void protocol_mcp::start() NOEXCEPT
{
    BC_ASSERT(stranded());
    if (started())
        return;

    // Chaser subscription is asynchronous, events may be missed.
    subscribe_chase(BIND(handle_chase, _1, _2, _3));

    // Lifecycle methods.
    SUBSCRIBE_RPC(handle_initialize, _1, _2, _3, _4, _5);
    SUBSCRIBE_RPC(handle_initialized, _1, _2);
    SUBSCRIBE_RPC(handle_ping, _1, _2);

    // Resource methods.
    SUBSCRIBE_RPC(handle_resources_list, _1, _2, _3);
    SUBSCRIBE_RPC(handle_resources_templates_list, _1, _2, _3);
    SUBSCRIBE_RPC(handle_resources_read, _1, _2, _3);
    SUBSCRIBE_RPC(handle_resources_subscribe, _1, _2, _3);
    SUBSCRIBE_RPC(handle_resources_unsubscribe, _1, _2, _3);

    protocol_rpc<interface::mcp>::start();
}

// Events unsubscription is asynchronous, race is ok.
void protocol_mcp::stopping(const code& ec) NOEXCEPT
{
    BC_ASSERT(stranded());
    stopping_.store(true);
    unsubscribe_chase();
    protocol_rpc<interface::mcp>::stopping(ec);
}

// No attached protocol subscribes the method (terminal responder).
void protocol_mcp::handle_unclaimed(const request_t&) NOEXCEPT
{
    BC_ASSERT(stranded());
    send_code(error::mcp::method_not_found);
}

// Handlers (event subscription).
// ----------------------------------------------------------------------------

bool protocol_mcp::handle_chase(const code&, node::chase event_,
    node::event_value value) NOEXCEPT
{
    // Do not pass ec to stopped as it is not a call status.
    if (stopped())
        return false;

    // Notifications require a full duplex transport, so subscriptions on an
    // http (post) connection are held but not computed.
    if (!websocket() && !downgraded())
        return true;

    if (!subscribed_.load(relaxed))
        return true;

    switch (event_)
    {
        case node::chase::organized:
        {
            BC_ASSERT(std::holds_alternative<node::header_t>(value));
            POST_NOTIFY(do_connected, std::get<node::header_t>(value));
            break;
        }
        case node::chase::reorganized:
        {
            // Value is regression branch_point.
            BC_ASSERT(std::holds_alternative<node::header_t>(value));
            POST_NOTIFY(do_disconnected, std::get<node::header_t>(value));
            break;
        }
        default:
        {
            break;
        }
    }

    return true;
}

// Handlers (lifecycle).
// ----------------------------------------------------------------------------

// The server offers its own revision, which the client accepts or abandons.
void protocol_mcp::handle_initialize(const code& ec,
    rpc_interface::initialize, const std::string&, const object_t&,
    const object_t&) NOEXCEPT
{
    BC_ASSERT(stranded());
    if (stopped(ec))
        return;

    send_result(object_t
    {
        { "protocolVersion", std::string{ protocol_version } },
        { "capabilities", object_t
            {
                { "resources", object_t{ { "subscribe", true } } }
            }
        },
        { "serverInfo", object_t
            {
                { "name", std::string{ "libbitcoin-server" } },
                { "version", std::string{ LIBBITCOIN_SERVER_VERSION } }
            }
        }
    });
}

void protocol_mcp::handle_initialized(const code& ec,
    rpc_interface::notifications_initialized) NOEXCEPT
{
    BC_ASSERT(stranded());
    if (stopped(ec))
        return;

    // A notification is not answered (json-rpc). An unsent response does not
    // restart the read cycle, so resume it (ws), or acknowledge the post.
    if (websocket() || downgraded())
        read_next();
    else
        send_ok();
}

void protocol_mcp::handle_ping(const code& ec,
    rpc_interface::ping) NOEXCEPT
{
    BC_ASSERT(stranded());
    if (stopped(ec))
        return;

    send_result(object_t{});
}

// Handlers (resources).
// ----------------------------------------------------------------------------

// There are no enumerable resources, only the address template.
void protocol_mcp::handle_resources_list(const code& ec,
    rpc_interface::resources_list, const std::string&) NOEXCEPT
{
    BC_ASSERT(stranded());
    if (stopped(ec))
        return;

    send_result(object_t{ { "resources", array_t{} } });
}

void protocol_mcp::handle_resources_templates_list(const code& ec,
    rpc_interface::resources_templates_list, const std::string&) NOEXCEPT
{
    BC_ASSERT(stranded());
    if (stopped(ec))
        return;

    send_result(object_t
    {
        { "resourceTemplates", array_t
            {
                object_t
                {
                    { "uriTemplate", std::string{ address_uri } + "{hash}" },
                    { "name", std::string{ "address" } },
                    { "description", std::string{ "Payments to an address "
                        "(script hash), as native /v1/address/[hash]." } },
                    { "mimeType", std::string{ "application/json" } }
                }
            }
        }
    });
}

// read
// ----------------------------------------------------------------------------
// Post to the network threadpool, the channel listener remains paused during
// this call, which guards against call backlogging (DoS) and requires the
// monitor to allow socket cancellation and server stop to interrupt query.

void protocol_mcp::handle_resources_read(const code& ec,
    rpc_interface::resources_read, const std::string& uri) NOEXCEPT
{
    BC_ASSERT(stranded());
    if (stopped(ec))
        return;

    hash_digest hash{};
    if (!to_hash(hash, uri))
    {
        send_code(error::mcp::invalid_params);
        return;
    }

    if (!archive().address_enabled())
    {
        send_code(error::mcp::method_not_found);
        return;
    }

    monitor(true);
    PARALLEL(do_read, uri, hash);
}

void protocol_mcp::do_read(const std::string& uri,
    const hash_digest& hash) NOEXCEPT
{
    BC_ASSERT(!stranded());

    database::outpoints set{};
    const auto& query = archive();
    const auto ec = query.get_address_outpoints(stopping_, set, hash, turbo_);
    POST(complete_read, ec, uri, std::move(set));
}

void protocol_mcp::complete_read(const code& ec, const std::string& uri,
    const database::outpoints& set) NOEXCEPT
{
    BC_ASSERT(stranded());

    // Stop monitoring socket.
    monitor(false);

    // Suppresses cancelation error response.
    if (stopped())
        return;

    if (ec)
    {
        send_code(error::mcp::translate(ec, error::mcp::internal_error));
        return;
    }

    send_result(object_t
    {
        { "contents", array_t
            {
                object_t
                {
                    { "uri", uri },
                    { "mimeType", std::string{ "application/json" } },
                    { "text", boost::json::serialize(
                        boost::json::value_from(set)) }
                }
            }
        }
    });
}

// subscribe
// ----------------------------------------------------------------------------
// Post to an independent strand on the network threadpool. This protects the
// subscriptions and ensures that the channel remains both cancellable and
// responsive.

void protocol_mcp::handle_resources_subscribe(const code& ec,
    rpc_interface::resources_subscribe, const std::string& uri) NOEXCEPT
{
    BC_ASSERT(stranded());
    if (stopped(ec))
        return;

    hash_digest hash{};
    if (!to_hash(hash, uri))
    {
        send_code(error::mcp::invalid_params);
        return;
    }

    if (!archive().address_enabled())
    {
        send_code(error::mcp::method_not_found);
        return;
    }

    monitor(true);
    POST_NOTIFY(do_subscribe, hash);
}

void protocol_mcp::do_subscribe(const hash_digest& hash) NOEXCEPT
{
    BC_ASSERT(notification_strand_.running_in_this_thread());

    code ec{ error::mcp::subscription_limit };
    if (watches_.contains(hash))
    {
        ec = error::mcp::success;
    }
    else if (watches_.size() < options().maximum_subscriptions)
    {
        // Prime the cursor to present, so only later payments are notified.
        address_watch watch{};
        histories discard{};
        const auto& query = archive();
        if (!(ec = query.get_history(stopping_, watch.cursor, discard, hash,
            options().maximum_history, turbo_)))
        {
            watch.floor = query.get_top_confirmed();
            watches_.emplace(hash, watch);
            subscribed_.store(true, relaxed);
        }
    }

    POST(complete_subscribe, ec);
}

void protocol_mcp::complete_subscribe(const code& ec) NOEXCEPT
{
    BC_ASSERT(stranded());

    monitor(false);
    if (stopped())
        return;

    if (ec)
    {
        send_code(error::mcp::translate(ec, error::mcp::internal_error));
        return;
    }

    send_result(object_t{});
}

// unsubscribe
// ----------------------------------------------------------------------------

void protocol_mcp::handle_resources_unsubscribe(const code& ec,
    rpc_interface::resources_unsubscribe, const std::string& uri) NOEXCEPT
{
    BC_ASSERT(stranded());
    if (stopped(ec))
        return;

    hash_digest hash{};
    if (!to_hash(hash, uri))
    {
        send_code(error::mcp::invalid_params);
        return;
    }

    POST_NOTIFY(do_unsubscribe, hash);
}

void protocol_mcp::do_unsubscribe(const hash_digest& hash) NOEXCEPT
{
    BC_ASSERT(notification_strand_.running_in_this_thread());

    watches_.erase(hash);
    if (watches_.empty())
        subscribed_.store(false, relaxed);

    POST(complete_unsubscribe);
}

void protocol_mcp::complete_unsubscribe() NOEXCEPT
{
    BC_ASSERT(stranded());
    if (stopped())
        return;

    send_result(object_t{});
}

// notify
// ----------------------------------------------------------------------------

// The address walk returns each tx that touches the address, spends included,
// so payments are the outputs that pay it. Cursors advance in the walk, so
// this stays on the notification strand.
void protocol_mcp::do_connected(node::header_t) NOEXCEPT
{
    BC_ASSERT(notification_strand_.running_in_this_thread());

    const auto& query = archive();
    for (auto& [key, watch]: watches_)
    {
        if (stopping_)
            return;

        const auto top = query.get_top_confirmed();

        histories delta{};
        if (const auto ec = query.get_history(stopping_, watch.cursor, delta,
            key, options().maximum_history, turbo_))
        {
            if (ec == database::error::query_canceled)
                return;

            LOGF("Mcp::do_connected, " << ec.message());
            continue;
        }

        auto outpoints = emplace_shared<array_t>();
        auto height = std::max(watch.floor, top);
        for (const auto& entry: delta)
        {
            // Confirmed only (no tx pool in v4), and not previously reported.
            if (!entry.confirmed() || entry.tx.height() <= watch.floor)
                continue;

            height = std::max(height, entry.tx.height());
            const auto tx = query.get_transaction(query.to_tx(entry.tx.hash()),
                true);

            if (!tx)
                continue;

            uint32_t index{};
            for (const auto& output: *tx->outputs_ptr())
            {
                if (output->script().hash() == key)
                    outpoints->emplace_back(object_t
                    {
                        { "point", object_t
                            {
                                { "hash", encode_hash(entry.tx.hash()) },
                                { "index", index }
                            }
                        },
                        { "value", output->value() }
                    });

                ++index;
            }
        }

        watch.floor = height;
        if (!outpoints->empty())
            POST(notify_updated, key, outpoints);
    }
}

// The chain has been reduced in height, the walks are invalid. Payments are
// notified again where confirmed above the regression branch point.
void protocol_mcp::do_disconnected(node::header_t link) NOEXCEPT
{
    BC_ASSERT(notification_strand_.running_in_this_thread());

    size_t height{};
    const auto found = archive().get_height(height,
        database::header_link{ link });

    for (auto& [key, watch]: watches_)
    {
        watch.cursor = {};
        if (found)
            watch.floor = std::min(watch.floor, height);
    }
}

void protocol_mcp::notify_updated(const hash_digest& hash,
    const array_ptr& outpoints) NOEXCEPT
{
    BC_ASSERT(stranded());
    if (stopped())
        return;

    send_notification("notifications/resources/updated", object_t
    {
        { "uri", std::string{ address_uri } + encode_hash(hash) },
        { "outpoints", std::move(*outpoints) }
    });
}

// utility
// ----------------------------------------------------------------------------

// static
bool protocol_mcp::to_hash(hash_digest& out, const std::string& uri) NOEXCEPT
{
    return uri.starts_with(address_uri) &&
        decode_hash(out, uri.substr(address_uri.size()));
}

BC_POP_WARNING()

} // namespace server
} // namespace libbitcoin

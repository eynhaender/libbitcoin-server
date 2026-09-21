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
#ifndef LIBBITCOIN_SERVER_INTERFACES_MCP_HPP
#define LIBBITCOIN_SERVER_INTERFACES_MCP_HPP

#include <bitcoin/server/define.hpp>
#include <bitcoin/server/interfaces/types.hpp>

namespace libbitcoin {
namespace server {
namespace interface {

/// Model Context Protocol (json-rpc-v2), limited to address subscription.
/// A resource is a native address target (/v1/address/[hash]) under the
/// native scheme (native:///v1/address/[hash]), where the hash is as native.
struct mcp_methods
{
    static constexpr std::tuple methods
    {
        /// Lifecycle.
        method<"initialize", string_t, object_t, object_t>{ "protocolVersion", "capabilities", "clientInfo" },
        method<"notifications/initialized">{},
        method<"ping">{},

        /// Resources (subscribe pushes notifications/resources/updated).
        method<"resources/list", optional<""_t>>{ "cursor" },
        method<"resources/templates/list", optional<""_t>>{ "cursor" },
        method<"resources/read", string_t>{ "uri" },
        method<"resources/subscribe", string_t>{ "uri" },
        method<"resources/unsubscribe", string_t>{ "uri" }
    };

    template <typename... Args>
    using subscriber = network::subscriber<Args...>;

    template <size_t Index>
    using at = method_at<methods, Index>;

    using initialize = at<0>;
    using notifications_initialized = at<1>;
    using ping = at<2>;

    using resources_list = at<3>;
    using resources_templates_list = at<4>;
    using resources_read = at<5>;
    using resources_subscribe = at<6>;
    using resources_unsubscribe = at<7>;
};

} // namespace interface
} // namespace server
} // namespace libbitcoin

#endif

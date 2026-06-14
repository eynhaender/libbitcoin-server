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
#ifndef LIBBITCOIN_SERVER_INTERFACES_MCP_HPP
#define LIBBITCOIN_SERVER_INTERFACES_MCP_HPP

#include <bitcoin/server/define.hpp>
#include <bitcoin/server/interfaces/types.hpp>

namespace libbitcoin {
namespace server {
namespace interface {

struct mcp_methods
{
    static constexpr std::tuple methods
    {
        /// Session lifecycle.
        method<"initialize", string_t, optional<empty::object>, optional<empty::object>>{ "protocolVersion", "capabilities", "clientInfo" },
        method<"notifications/initialized">{},

        /// Tool discovery and invocation.
        method<"tools/list", optional<""_t>>{ "cursor" },
        method<"tools/call", string_t, optional<empty::object>, optional<empty::object>>{ "name", "arguments", "_meta" }
    };

    template <typename... Args>
    using subscriber = network::unsubscriber<Args...>;

    template <size_t Index>
    using at = method_at<methods, Index>;

    // Derive this from above in c++26 using reflection.
    using initialize             = at<0>;
    using notifications_initialized = at<1>;
    using tools_list             = at<2>;
    using tools_call             = at<3>;
};

} // namespace interface
} // namespace server
} // namespace libbitcoin

#endif

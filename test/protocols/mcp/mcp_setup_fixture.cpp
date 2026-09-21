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
#include "../../test.hpp"
#include "../../mocks/blocks.hpp"
#include "mcp_setup_fixture.hpp"

BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)

mcp_setup_fixture::mcp_setup_fixture(const initializer& setup,
    bool address_index, const configurator& configure)
  : rpc_setup_fixture(setup,
        [configure](configuration& config) NOEXCEPT
        {
            auto& mcp = config.server.mcp;
            mcp.binds = { { MCP_ENDPOINT } };
            mcp.maximum_subscriptions = 2;
            mcp.maximum_history = 5;
            mcp.connections = 1;
            mcp.inactivity_minutes = 1;
            config.node.currency_window_minutes = 0;

            if (configure)
                configure(config);
        }, address_index, true)
{
    client_.connect(config_.server.mcp.binds.back().to_endpoint());
}

mcp_setup_fixture::~mcp_setup_fixture()
{
    client_.close();
}

BC_POP_WARNING()

boost::json::value mcp_setup_fixture::post(const std::string& request)
{
    return client_.post(request);
}

rpc_client::status mcp_setup_fixture::post_status(const std::string& request)
{
    return client_.post_status(request);
}

network::boost_code mcp_setup_fixture::ws_upgrade()
{
    return client_.upgrade();
}

boost::json::value mcp_setup_fixture::ws_get(const std::string& request)
{
    return client_.frame(request);
}

boost::json::value mcp_setup_fixture::ws_receive()
{
    return client_.read_frame();
}

void mcp_setup_fixture::ws_write(const std::string& request)
{
    client_.write_frame(request);
}

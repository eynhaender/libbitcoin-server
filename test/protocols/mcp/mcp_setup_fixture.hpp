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
#ifndef LIBBITCOIN_SERVER_TEST_PROTOCOLS_MCP_MCP_SETUP_FIXTURE
#define LIBBITCOIN_SERVER_TEST_PROTOCOLS_MCP_MCP_SETUP_FIXTURE

#include "../../test.hpp"
#include "../../mocks/blocks.hpp"
#include "../fixture/rpc_client.hpp"
#include "../fixture/rpc_setup_fixture.hpp"

#define MCP_ENDPOINT "127.0.0.1:65006"

struct mcp_setup_fixture
  : rpc_setup_fixture
{
    DELETE_COPY_MOVE(mcp_setup_fixture);

    using initializer = std::function<bool(test::query_t&)>;
    using configurator = std::function<void(configuration&)>;

    explicit mcp_setup_fixture(const initializer& setup,
        bool address_index=true, const configurator& configure={});
    ~mcp_setup_fixture();

    // json-rpc over http POST to "/" (the connection remains http).
    boost::json::value post(const std::string& request);
    rpc_client::status post_status(const std::string& request);

    // Upgrade the connection to websocket (no further http requests).
    network::boost_code ws_upgrade();

    // json-rpc over the upgraded websocket connection.
    boost::json::value ws_get(const std::string& request);

    // Read one unsolicited frame (notification) from the websocket.
    boost::json::value ws_receive();

    // Write a frame that is not answered (client notification).
    void ws_write(const std::string& request);

private:
    rpc_client client_{ io_ };
};

struct mcp_ten_block_setup_fixture
  : mcp_setup_fixture
{
    inline mcp_ten_block_setup_fixture()
      : mcp_setup_fixture([](test::query_t& query)
        {
            return test::setup_ten_block_store(query);
        })
    {
    }
};

struct mcp_disabled_setup_fixture
  : mcp_setup_fixture
{
    inline mcp_disabled_setup_fixture()
      : mcp_setup_fixture([](test::query_t& query)
        {
            return test::setup_ten_block_store(query);
        }, false)
    {
    }
};

#endif

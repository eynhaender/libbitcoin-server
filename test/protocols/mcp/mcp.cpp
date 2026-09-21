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
#include "mcp_setup_fixture.hpp"

using namespace system;

static const code method_not_found{ server::error::mcp::method_not_found };
static const code invalid_params{ server::error::mcp::invalid_params };
static const code subscription_limit{ server::error::mcp::subscription_limit };

static const std::string found_scripthash{ "bad83872c90886be19b98734fd16741611efcd9f5de699c14b712675eec682f5" };
static const std::string bogus_scripthash{ "9c2c84a6cf9809e08af19557e28d38257e6fee6981269760637a5f9dfb000b05" };
static const std::string found_uri{ "native:///v1/address/" + found_scripthash };
static const std::string bogus_uri{ "native:///v1/address/" + bogus_scripthash };

static const std::string initialize_request{ R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})" };

static std::string request(int id, const std::string& method,
    const std::string& uri)
{
    return (boost_format(R"({"jsonrpc":"2.0","id":%1%,"method":"%2%","params":{"uri":"%3%"}})")
        % id % method % uri).str();
}

static int64_t error_of(const boost::json::value& response)
{
    try { return response.at("error").as_object().at("code").as_int64(); }
    catch (const boost::system::system_error&) { return 0; }
}

// The outpoints of the block's transactions that pay the found address.
static boost::json::array payments(const chain::block& block)
{
    boost::json::array out{};
    hash_digest key{};
    decode_hash(key, found_scripthash);
    for (const auto& tx: *block.transactions_ptr())
    {
        uint32_t index{};
        for (const auto& output: *tx->outputs_ptr())
        {
            if (output->script().hash() == key)
                out.push_back(boost::json::object
                {
                    { "point", boost::json::object
                        {
                            { "hash", encode_hash(tx->hash(false)) },
                            { "index", index }
                        }
                    },
                    { "value", output->value() }
                });

            ++index;
        }
    }

    return out;
}

BOOST_FIXTURE_TEST_SUITE(mcp_tests, mcp_ten_block_setup_fixture)

// initialize

BOOST_AUTO_TEST_CASE(mcp__initialize__post__server_capabilities)
{
    const auto response = post(initialize_request);
    REQUIRE_NO_THROW_TRUE(response.at("id").is_int64());
    BOOST_REQUIRE_EQUAL(response.at("id").as_int64(), 1);
    REQUIRE_NO_THROW_TRUE(response.at("result").is_object());

    const auto& result = response.at("result").as_object();
    BOOST_REQUIRE_EQUAL(result.at("protocolVersion").as_string(), server::protocol_mcp::protocol_version);
    BOOST_REQUIRE(result.at("capabilities").at("resources").at("subscribe").as_bool());
    BOOST_REQUIRE_EQUAL(result.at("serverInfo").at("name").as_string(), "libbitcoin-server");
}

BOOST_AUTO_TEST_CASE(mcp__initialize__missing_arguments__dropped)
{
    const auto response = post(R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{}})");
    REQUIRE_NO_THROW_TRUE(response.at("dropped").as_bool());
}

BOOST_AUTO_TEST_CASE(mcp__ping__post__empty_result)
{
    const auto response = post(R"({"jsonrpc":"2.0","id":3,"method":"ping"})");
    BOOST_REQUIRE_EQUAL(response.at("id").as_int64(), 3);
    REQUIRE_NO_THROW_TRUE(response.at("result").is_object());
    BOOST_REQUIRE(response.at("result").as_object().empty());
}

BOOST_AUTO_TEST_CASE(mcp__initialized_notification__post__acknowledged)
{
    BOOST_REQUIRE(post_status(R"({"jsonrpc":"2.0","method":"notifications/initialized"})") == rpc_client::status::ok);
}

BOOST_AUTO_TEST_CASE(mcp__initialized_notification__websocket__read_cycle_resumed)
{
    BOOST_REQUIRE(!ws_upgrade());
    ws_write(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");

    // The notification is not answered, so this is the first response.
    const auto response = ws_get(R"({"jsonrpc":"2.0","id":4,"method":"ping"})");
    BOOST_REQUIRE_EQUAL(response.at("id").as_int64(), 4);
    BOOST_REQUIRE(response.as_object().contains("result"));
}

BOOST_AUTO_TEST_CASE(mcp__unknown_method__terminal__method_not_found)
{
    const auto response = post(R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})");
    BOOST_REQUIRE_EQUAL(error_of(response), method_not_found.value());
}

// resources/list, resources/templates/list

BOOST_AUTO_TEST_CASE(mcp__resources_list__post__empty)
{
    const auto response = post(R"({"jsonrpc":"2.0","id":6,"method":"resources/list"})");
    REQUIRE_NO_THROW_TRUE(response.at("result").at("resources").is_array());
    BOOST_REQUIRE(response.at("result").at("resources").as_array().empty());
}

BOOST_AUTO_TEST_CASE(mcp__resources_templates_list__post__address_template)
{
    const auto response = post(R"({"jsonrpc":"2.0","id":7,"method":"resources/templates/list"})");
    REQUIRE_NO_THROW_TRUE(response.at("result").at("resourceTemplates").is_array());

    const auto& templates = response.at("result").at("resourceTemplates").as_array();
    BOOST_REQUIRE_EQUAL(templates.size(), one);
    BOOST_REQUIRE_EQUAL(templates.at(0).at("uriTemplate").as_string(), "native:///v1/address/{hash}");
}

// resources/read

BOOST_AUTO_TEST_CASE(mcp__resources_read__invalid_uri__invalid_params)
{
    BOOST_REQUIRE_EQUAL(error_of(post(request(8, "resources/read", "/v1/address/" + found_scripthash))), invalid_params.value());
    BOOST_REQUIRE_EQUAL(error_of(post(request(9, "resources/read", "native:///v1/address/not_a_hash"))), invalid_params.value());
    BOOST_REQUIRE_EQUAL(error_of(post(request(10, "resources/read", "native:///v1/block/height/1"))), invalid_params.value());
}

BOOST_AUTO_TEST_CASE(mcp__resources_read__bogus_address__empty)
{
    const auto response = post(request(11, "resources/read", bogus_uri));
    const auto& content = response.at("result").at("contents").as_array().at(0);
    BOOST_REQUIRE_EQUAL(content.at("uri").as_string(), bogus_uri);
    BOOST_REQUIRE_EQUAL(content.at("mimeType").as_string(), "application/json");
    BOOST_REQUIRE_EQUAL(content.at("text").as_string(), "[]");
}

BOOST_AUTO_TEST_CASE(mcp__resources_read__confirmed_payment__outpoints)
{
    BOOST_REQUIRE(query_.set(test::mock_block10, database::context{ 0, 10, 0 }, false, false));
    BOOST_REQUIRE(query_.push_confirmed(query_.to_header(test::mock_block10.hash()), true));

    const auto response = post(request(12, "resources/read", found_uri));
    const auto& content = response.at("result").at("contents").as_array().at(0);
    const auto text = boost::json::parse(content.at("text").as_string());
    REQUIRE_NO_THROW_TRUE(text.is_array());
    BOOST_REQUIRE(!text.as_array().empty());
    BOOST_REQUIRE_EQUAL(text.as_array().front().at("point").at("hash").as_string(),
        encode_hash(test::mock_block10.transactions_ptr()->at(1)->hash(false)));
    BOOST_REQUIRE(text.as_array().front().as_object().contains("value"));
}

// resources/subscribe

BOOST_AUTO_TEST_CASE(mcp__resources_subscribe__invalid_uri__invalid_params)
{
    BOOST_REQUIRE_EQUAL(error_of(post(request(13, "resources/subscribe", "native:///v1/address/not_a_hash"))), invalid_params.value());
}

BOOST_AUTO_TEST_CASE(mcp__resources_subscribe__bogus_address__empty_result)
{
    const auto response = post(request(14, "resources/subscribe", bogus_uri));
    REQUIRE_NO_THROW_TRUE(response.at("result").is_object());
    BOOST_REQUIRE(response.at("result").as_object().empty());
}

BOOST_AUTO_TEST_CASE(mcp__resources_subscribe__repeat_call__idempotent)
{
    for (auto id = 15; id < 18; ++id)
    {
        const auto response = post(request(id, "resources/subscribe", found_uri));
        REQUIRE_NO_THROW_TRUE(response.at("result").is_object());
    }
}

BOOST_AUTO_TEST_CASE(mcp__resources_subscribe__over_maximum__subscription_limit)
{
    // The fixture allows two subscriptions.
    BOOST_REQUIRE(!error_of(post(request(18, "resources/subscribe", found_uri))));
    BOOST_REQUIRE(!error_of(post(request(19, "resources/subscribe", bogus_uri))));

    const auto third = "native:///v1/address/" + encode_hash(hash_digest{ 0x42 });
    BOOST_REQUIRE_EQUAL(error_of(post(request(20, "resources/subscribe", third))), subscription_limit.value());
}

// resources/unsubscribe

BOOST_AUTO_TEST_CASE(mcp__resources_unsubscribe__invalid_uri__invalid_params)
{
    BOOST_REQUIRE_EQUAL(error_of(post(request(21, "resources/unsubscribe", "native:///v1/address/not_a_hash"))), invalid_params.value());
}

BOOST_AUTO_TEST_CASE(mcp__resources_unsubscribe__unsubscribed_and_subscribed__empty_result)
{
    auto response = post(request(22, "resources/unsubscribe", found_uri));
    REQUIRE_NO_THROW_TRUE(response.at("result").is_object());

    BOOST_REQUIRE(!error_of(post(request(23, "resources/subscribe", found_uri))));
    response = post(request(24, "resources/unsubscribe", found_uri));
    REQUIRE_NO_THROW_TRUE(response.at("result").is_object());
    BOOST_REQUIRE(response.at("result").as_object().empty());
}

// notifications/resources/updated

BOOST_AUTO_TEST_CASE(mcp__resources_subscribe__progressive_notify__new_confirmed_payments)
{
    BOOST_REQUIRE(!ws_upgrade());

    // Blocks 10-12 are stored, and the address is subscribed before any pays.
    BOOST_REQUIRE(query_.set(test::mock_block10, database::context{ 0, 10, 0 }, false, false));
    BOOST_REQUIRE(query_.set(test::mock_block11, database::context{ 0, 11, 0 }, false, false));
    BOOST_REQUIRE(query_.set(test::mock_block12, database::context{ 0, 12, 0 }, false, false));

    auto response = ws_get(request(25, "resources/subscribe", found_uri));
    REQUIRE_NO_THROW_TRUE(response.at("result").is_object());

    // Confirming block 10 notifies its payment, and only its payment.
    BOOST_REQUIRE(query_.push_confirmed(query_.to_header(test::mock_block10.hash()), true));
    notify(node::chase::organized, node::header_t{ 0 });

    auto notification = ws_receive();
    REQUIRE_NO_THROW_TRUE(notification.at("method").is_string());
    BOOST_REQUIRE_EQUAL(notification.at("method").as_string(), "notifications/resources/updated");
    BOOST_REQUIRE(!notification.as_object().contains("id"));
    BOOST_REQUIRE_EQUAL(notification.at("params").at("uri").as_string(), found_uri);
    BOOST_REQUIRE(notification.at("params").at("outpoints") == payments(test::mock_block10));

    // Confirming block 11 notifies only block 11 (not repeating block 10).
    BOOST_REQUIRE(query_.push_confirmed(query_.to_header(test::mock_block11.hash()), true));
    notify(node::chase::organized, node::header_t{ 0 });

    notification = ws_receive();
    BOOST_REQUIRE_EQUAL(notification.at("method").as_string(), "notifications/resources/updated");
    BOOST_REQUIRE(notification.at("params").at("outpoints") == payments(test::mock_block11));

    // Unsubscribed, block 12 is not notified, the next response is the ping.
    response = ws_get(request(26, "resources/unsubscribe", found_uri));
    REQUIRE_NO_THROW_TRUE(response.at("result").is_object());

    BOOST_REQUIRE(query_.push_confirmed(query_.to_header(test::mock_block12.hash()), true));
    notify(node::chase::organized, node::header_t{ 0 });

    response = ws_get(R"({"jsonrpc":"2.0","id":27,"method":"ping"})");
    BOOST_REQUIRE_EQUAL(response.at("id").as_int64(), 27);
}

BOOST_AUTO_TEST_CASE(mcp__resources_subscribe__existing_history__not_notified)
{
    BOOST_REQUIRE(!ws_upgrade());

    BOOST_REQUIRE(query_.set(test::mock_block10, database::context{ 0, 10, 0 }, false, false));
    BOOST_REQUIRE(query_.set(test::mock_block11, database::context{ 0, 11, 0 }, false, false));
    BOOST_REQUIRE(query_.push_confirmed(query_.to_header(test::mock_block10.hash()), true));

    // Block 10 is confirmed before the subscription, so it is history.
    auto response = ws_get(request(28, "resources/subscribe", found_uri));
    REQUIRE_NO_THROW_TRUE(response.at("result").is_object());

    BOOST_REQUIRE(query_.push_confirmed(query_.to_header(test::mock_block11.hash()), true));
    notify(node::chase::organized, node::header_t{ 0 });

    const auto notification = ws_receive();
    BOOST_REQUIRE(notification.at("params").at("outpoints") == payments(test::mock_block11));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(mcp_disabled_tests, mcp_disabled_setup_fixture)

BOOST_AUTO_TEST_CASE(mcp__resources__no_address_index__method_not_found)
{
    BOOST_REQUIRE_EQUAL(error_of(post(request(30, "resources/read", found_uri))), method_not_found.value());
    BOOST_REQUIRE_EQUAL(error_of(post(request(31, "resources/subscribe", found_uri))), method_not_found.value());
}

BOOST_AUTO_TEST_SUITE_END()

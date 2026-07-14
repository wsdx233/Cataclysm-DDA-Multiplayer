#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif

#define ASIO_NO_DEPRECATED
#define ASIO_STANDALONE
#include <asio.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <locale>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "multiplayer_content_manifest.h"
#include "multiplayer_protocol.h"
#include "multiplayer_server.h"
#include "multiplayer_server_config.h"
#include "multiplayer_server_log.h"
#include "multiplayer_transport.h"

namespace
{

class grouped_integer_locale : public std::numpunct<char>
{
    protected:
        char do_thousands_sep() const override {
            return ',';
        }
        std::string do_grouping() const override {
            return "\3";
        }
};

std::uint16_t unused_loopback_port()
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(
        io, asio::ip::tcp::endpoint( asio::ip::address_v4::loopback(), 0 ) );
    return acceptor.local_endpoint().port();
}

void send_protocol_frame( asio::ip::tcp::socket &socket,
                          const multiplayer_protocol_envelope &envelope )
{
    multiplayer_transport_payload protocol_bytes;
    multiplayer_transport_payload frame;
    std::string error;
    REQUIRE( multiplayer_encode_protocol_envelope( envelope, protocol_bytes, error ) );
    REQUIRE( multiplayer_encode_transport_frame( protocol_bytes, frame, error ) );
    asio::write( socket, asio::buffer( frame ) );
}

bool pump_until_readable( multiplayer_dedicated_server &server, asio::ip::tcp::socket &socket,
                          std::string &error )
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    do {
        if( !server.poll_once( std::chrono::steady_clock::now(), error ) ) {
            return false;
        }
        asio::error_code available_error;
        if( socket.available( available_error ) > 0 ) {
            return true;
        }
        if( available_error ) {
            error = available_error.message();
            return false;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    } while( std::chrono::steady_clock::now() < deadline );
    error = "timed out waiting for server response";
    return false;
}

multiplayer_protocol_envelope read_protocol_frame( asio::ip::tcp::socket &socket )
{
    std::array<std::uint8_t, multiplayer_transport_frame_header_size> header = {};
    asio::error_code error;
    REQUIRE( asio::read( socket, asio::buffer( header ), error ) == header.size() );
    REQUIRE_FALSE( error );
    const std::uint32_t length =
        static_cast<std::uint32_t>( header[0] ) << 24U |
        static_cast<std::uint32_t>( header[1] ) << 16U |
        static_cast<std::uint32_t>( header[2] ) << 8U |
        static_cast<std::uint32_t>( header[3] );
    REQUIRE( length <= multiplayer_transport_maximum_frame_size );
    multiplayer_transport_payload bytes( length );
    REQUIRE( asio::read( socket, asio::buffer( bytes ), error ) == bytes.size() );
    REQUIRE_FALSE( error );
    multiplayer_protocol_envelope envelope;
    std::string decode_error;
    REQUIRE( multiplayer_decode_protocol_envelope( bytes, envelope, decode_error ) );
    return envelope;
}

} // namespace

TEST_CASE( "multiplayer_server_logs_are_single_line_json_and_escape_untrusted_text",
           "[multiplayer][dedicated_server]" )
{
    const std::string record = multiplayer_server_log_json(
                                   multiplayer_server_log_severity::warning, "bad\"event",
    { { "message", "line one\nline two\\end" } },
    std::chrono::system_clock::time_point {} );
    CHECK( record ==
           "{\"timestamp\":\"1970-01-01T00:00:00.000Z\",\"severity\":\"warning\","
           "\"category\":\"multiplayer_server\",\"event\":\"bad\\\"event\","
           "\"message\":\"line one\\nline two\\\\end\"}" );
    CHECK( record.find( '\n' ) == std::string::npos );
}

TEST_CASE( "multiplayer_content_manifest_hashes_ordered_file_names_sizes_and_bytes",
           "[multiplayer][dedicated_server]" )
{
    const std::string unique = "cdda-multiplayer-manifest-" + std::to_string(
                                   std::chrono::steady_clock::now().time_since_epoch().count() );
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / unique;
    REQUIRE( std::filesystem::create_directories( directory / "nested" ) );
    on_out_of_scope cleanup( [&directory]() {
        std::error_code ignored;
        std::filesystem::remove_all( directory, ignored );
    } );
    {
        std::ofstream output( directory / "a.json", std::ios::binary );
        output << "{}";
    }
    {
        std::ofstream output( directory / "nested" / "b.json", std::ios::binary );
        output << "hello\n";
    }

    std::string manifest;
    multiplayer_content_manifest_stats stats;
    std::string error;
    REQUIRE( multiplayer_build_content_manifest( { { "core", directory } }, manifest,
    stats, error ) );
    CHECK( manifest ==
           "sha256-fbd4d3cd0ce8a1fb84ee88e7e17c064d4fa8f763da86663e28c0bee442967b44" );
    CHECK( stats.file_count == 2 );
    CHECK( stats.byte_count == 8 );

    const std::locale original = std::locale();
    on_out_of_scope restore_locale( [&original]() {
        std::locale::global( original );
    } );
    std::locale::global( std::locale( std::locale::classic(), new grouped_integer_locale ) );
    std::string grouped;
    multiplayer_content_manifest_stats grouped_stats;
    REQUIRE( multiplayer_build_content_manifest( { { "core", directory } }, grouped,
    grouped_stats, error ) );
    CHECK( grouped == manifest );

    {
        std::ofstream output( directory / "a.json", std::ios::binary | std::ios::trunc );
        output << "[]";
    }
    std::string changed;
    REQUIRE( multiplayer_build_content_manifest( { { "core", directory } }, changed,
    grouped_stats, error ) );
    CHECK( changed != manifest );
}

TEST_CASE( "multiplayer_server_content_manifest_covers_core_and_selected_gameplay_data",
           "[multiplayer][dedicated_server]" )
{
    multiplayer_server_config config;
    config.world.mods = { "dda" };
    std::string manifest;
    multiplayer_content_manifest_stats stats;
    std::string error;
    REQUIRE( multiplayer_server_content_manifest( config, manifest, stats, error ) );
    CHECK( manifest.size() == 71 );
    CHECK( manifest.compare( 0, 7, "sha256-" ) == 0 );
    CHECK( stats.file_count > 3000 );
    CHECK( stats.byte_count > 40 * 1024 * 1024 );

    std::string repeated_manifest;
    multiplayer_content_manifest_stats repeated_stats;
    REQUIRE( multiplayer_server_content_manifest( config, repeated_manifest,
             repeated_stats, error ) );
    CHECK( repeated_manifest == manifest );
    CHECK( repeated_stats.file_count == stats.file_count );
    CHECK( repeated_stats.byte_count == stats.byte_count );
}

TEST_CASE( "multiplayer_dedicated_server_runs_transport_handshake_auth_and_ping",
           "[multiplayer][dedicated_server]" )
{
    const std::string unique = "cdda-multiplayer-server-" + std::to_string(
                                   std::chrono::steady_clock::now().time_since_epoch().count() );
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / unique;
    REQUIRE( std::filesystem::create_directory( directory ) );
    on_out_of_scope cleanup( [&directory]() {
        std::error_code ignored;
        std::filesystem::remove_all( directory, ignored );
    } );
    const std::filesystem::path config_path = directory / "server.json";
    std::string error;
    REQUIRE( initialize_multiplayer_server_files( config_path, error ) );
    multiplayer_server_config_result loaded = load_multiplayer_server_config( config_path );
    REQUIRE( loaded );
    multiplayer_server_config config = std::move( *loaded.config );
    const std::uint16_t port = unused_loopback_port();
    config.network.listen = "127.0.0.1:" + std::to_string( port );

    const std::string build_id = "dedicated-server-test";
    const std::string content_manifest =
        "sha256-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    multiplayer_server_player_identity identity;
    identity.player_id = "12345678-1234-4234-9234-123456789abc";
    identity.character_id = "42";
    multiplayer_dedicated_server server( config, config_path, build_id, content_manifest,
                                         identity );
    REQUIRE( server.start( error ) );
    REQUIRE( server.running() );
    REQUIRE( server.bound_port() == port );

    asio::io_context client_io;
    asio::ip::tcp::socket client( client_io );
    client.connect( asio::ip::tcp::endpoint( asio::ip::address_v4::loopback(), port ) );

    multiplayer_client_hello client_hello;
    client_hello.client_kind = multiplayer_protocol_client_kind::headless_test;
    client_hello.build_id = build_id;
    client_hello.content_manifest = content_manifest;
    client_hello.savegame_version = 39;
    client_hello.capabilities = { { "semantic-scene", 1, true } };
    client_hello.client_nonce[0] = 1;
    multiplayer_protocol_envelope request;
    request.message_type = multiplayer_protocol_message_type::client_hello;
    REQUIRE( multiplayer_build_client_hello_payload( client_hello, request.payload, error ) );
    send_protocol_frame( client, request );
    REQUIRE( pump_until_readable( server, client, error ) );
    multiplayer_protocol_envelope response = read_protocol_frame( client );
    multiplayer_server_hello server_hello;
    REQUIRE( multiplayer_parse_server_hello_payload( response, server_hello, error ) );
    REQUIRE( server_hello.accepted );

    std::string bearer_token;
    REQUIRE( load_multiplayer_server_bearer_token( config_path, config, bearer_token, error ) );
    multiplayer_authenticate_request authentication_request;
    authentication_request.display_name = "End To End Tester";
    authentication_request.bearer_token = bearer_token;
    request = {};
    request.message_type = multiplayer_protocol_message_type::authenticate;
    request.sequence = 1;
    REQUIRE( multiplayer_build_authenticate_payload( authentication_request,
             request.payload, error ) );
    send_protocol_frame( client, request );
    REQUIRE( pump_until_readable( server, client, error ) );
    response = read_protocol_frame( client );
    multiplayer_authentication_result authentication_result;
    REQUIRE( multiplayer_parse_authentication_result_payload( response,
             authentication_result, error ) );
    REQUIRE( authentication_result.accepted );
    CHECK( authentication_result.player_id == identity.player_id );
    CHECK( authentication_result.character_id == identity.character_id );
    REQUIRE( response.session != multiplayer_session_id {} );
    const multiplayer_session_id session = response.session;

    std::optional<multiplayer_server_lobby_event> server_event = server.poll_event();
    REQUIRE( server_event );
    CHECK( server_event->type == multiplayer_server_lobby_event_type::authenticated );
    CHECK( server_event->player_id == authentication_result.player_id );

    multiplayer_protocol_heartbeat ping;
    ping.nonce = 0x0102030405060708ULL;
    ping.monotonic_milliseconds = 42;
    request = {};
    request.message_type = multiplayer_protocol_message_type::ping;
    request.session = response.session;
    request.sequence = 2;
    REQUIRE( multiplayer_build_ping_payload( ping, request.payload, error ) );
    send_protocol_frame( client, request );
    REQUIRE( pump_until_readable( server, client, error ) );
    response = read_protocol_frame( client );
    multiplayer_protocol_heartbeat pong;
    REQUIRE( multiplayer_parse_pong_payload( response, pong, error ) );
    CHECK( pong.nonce == ping.nonce );
    CHECK( response.session == request.session );
    CHECK( response.sequence == request.sequence );

    multiplayer_scene_snapshot snapshot;
    snapshot.server_revision = 5;
    snapshot.turn = 17;
    snapshot.player.player_id = identity.player_id;
    snapshot.player.character_id = identity.character_id;
    snapshot.player.revision = snapshot.server_revision;
    multiplayer_protocol_envelope server_message;
    server_message.message_type = multiplayer_protocol_message_type::scene_snapshot;
    server_message.session = response.session;
    server_message.sequence = snapshot.server_revision;
    REQUIRE( multiplayer_build_scene_snapshot_payload( snapshot, server_message.payload, error ) );
    REQUIRE( server.send( server_event->connection, server_message, error ) );
    REQUIRE( pump_until_readable( server, client, error ) );
    response = read_protocol_frame( client );
    multiplayer_scene_snapshot parsed_snapshot;
    REQUIRE( multiplayer_parse_scene_snapshot_payload( response, parsed_snapshot, error ) );
    CHECK( parsed_snapshot.server_revision == snapshot.server_revision );
    CHECK( parsed_snapshot.player.player_id == identity.player_id );

    multiplayer_player_command command;
    command.client_sequence = 3;
    command.base_revision = snapshot.server_revision;
    command.kind = multiplayer_command_kind::wait;
    request = {};
    request.message_type = multiplayer_protocol_message_type::player_command;
    request.session = session;
    request.sequence = command.client_sequence;
    REQUIRE( multiplayer_build_player_command_payload( command, request.payload, error ) );
    send_protocol_frame( client, request );

    request = {};
    request.message_type = multiplayer_protocol_message_type::disconnect_notice;
    request.session = session;
    request.sequence = 4;
    REQUIRE( multiplayer_build_disconnect_notice_payload(
    { multiplayer_protocol_rejection::none, "ordered test leave" },
    request.payload, error ) );
    send_protocol_frame( client, request );

    std::vector<multiplayer_server_lobby_event> ordered_events;
    const auto event_deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    do {
        REQUIRE( server.poll_once( std::chrono::steady_clock::now(), error ) );
        while( std::optional<multiplayer_server_lobby_event> event = server.poll_event() ) {
            ordered_events.emplace_back( std::move( *event ) );
        }
        if( ordered_events.size() < 2 ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    } while( ordered_events.size() < 2 && std::chrono::steady_clock::now() < event_deadline );
    REQUIRE( ordered_events.size() == 2 );
    CHECK( ordered_events.front().type ==
           multiplayer_server_lobby_event_type::application_message );
    CHECK( ordered_events.front().message.message_type ==
           multiplayer_protocol_message_type::player_command );
    CHECK( ordered_events.back().type ==
           multiplayer_server_lobby_event_type::graceful_disconnect_requested );
    asio::error_code available_error;
    CHECK( client.available( available_error ) == 0 );
    CHECK_FALSE( available_error );

    multiplayer_command_result command_result;
    command_result.client_sequence = command.client_sequence;
    command_result.status = multiplayer_command_status::accepted;
    command_result.server_revision = snapshot.server_revision;
    server_message = {};
    server_message.message_type = multiplayer_protocol_message_type::command_result;
    server_message.session = session;
    server_message.sequence = command.client_sequence;
    REQUIRE( multiplayer_build_command_result_payload( command_result,
             server_message.payload, error ) );
    REQUIRE( server.send( ordered_events.front().connection, server_message, error ) );

    server_message = {};
    server_message.message_type = multiplayer_protocol_message_type::scene_snapshot;
    server_message.session = session;
    server_message.sequence = snapshot.server_revision;
    REQUIRE( multiplayer_build_scene_snapshot_payload( snapshot, server_message.payload, error ) );
    REQUIRE( server.send( ordered_events.front().connection, server_message, error ) );

    bool disconnect_completed = false;
    REQUIRE( server.complete_graceful_disconnect( ordered_events.back(), disconnect_completed,
             error ) );
    REQUIRE( disconnect_completed );

    REQUIRE( pump_until_readable( server, client, error ) );
    response = read_protocol_frame( client );
    multiplayer_command_result parsed_result;
    REQUIRE( multiplayer_parse_command_result_payload( response, parsed_result, error ) );
    CHECK( parsed_result.client_sequence == command.client_sequence );

    REQUIRE( pump_until_readable( server, client, error ) );
    response = read_protocol_frame( client );
    REQUIRE( multiplayer_parse_scene_snapshot_payload( response, parsed_snapshot, error ) );

    REQUIRE( pump_until_readable( server, client, error ) );
    response = read_protocol_frame( client );
    multiplayer_disconnect_notice acknowledgement;
    REQUIRE( multiplayer_parse_disconnect_notice_payload( response, acknowledgement, error ) );
    CHECK( response.session == session );
    CHECK( response.sequence == 4 );
    CHECK( acknowledgement.code == multiplayer_protocol_rejection::none );

    std::array<std::uint8_t, 1> end = {};
    asio::error_code close_error;
    client.read_some( asio::buffer( end ), close_error );
    CHECK( close_error == asio::error::eof );

    std::optional<multiplayer_server_lobby_event> disconnected_event;
    const auto disconnect_deadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds( 5 );
    do {
        REQUIRE( server.poll_once( std::chrono::steady_clock::now(), error ) );
        disconnected_event = server.poll_event();
        if( !disconnected_event ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    } while( !disconnected_event &&
             std::chrono::steady_clock::now() < disconnect_deadline );
    REQUIRE( disconnected_event );
    CHECK( disconnected_event->type == multiplayer_server_lobby_event_type::disconnected );
    CHECK( disconnected_event->connection == ordered_events.back().connection );

    disconnect_completed = true;
    REQUIRE( server.complete_graceful_disconnect( ordered_events.back(), disconnect_completed,
             error ) );
    CHECK_FALSE( disconnect_completed );

    server.stop();
    CHECK_FALSE( server.running() );
}

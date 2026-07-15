#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif

#define ASIO_NO_DEPRECATED
#define ASIO_STANDALONE
#include <asio.hpp>

#include <algorithm>
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

void negotiate_server_client( multiplayer_dedicated_server &server,
                              asio::ip::tcp::socket &socket,
                              const std::string &build_id,
                              const std::string &content_manifest,
                              std::string &error )
{
    multiplayer_client_hello hello;
    hello.client_kind = multiplayer_protocol_client_kind::headless_test;
    hello.build_id = build_id;
    hello.content_manifest = content_manifest;
    hello.savegame_version = 39;
    hello.capabilities = { { "semantic-scene", 1, true } };
    hello.client_nonce[0] = 1;
    multiplayer_protocol_envelope request;
    request.message_type = multiplayer_protocol_message_type::client_hello;
    REQUIRE( multiplayer_build_client_hello_payload( hello, request.payload, error ) );
    send_protocol_frame( socket, request );
    REQUIRE( pump_until_readable( server, socket, error ) );
    const multiplayer_protocol_envelope response = read_protocol_frame( socket );
    multiplayer_server_hello result;
    REQUIRE( multiplayer_parse_server_hello_payload( response, result, error ) );
    REQUIRE( result.accepted );
}

std::optional<multiplayer_server_lobby_event> wait_for_server_event(
    multiplayer_dedicated_server &server, std::string &error )
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    do {
        if( !server.poll_once( std::chrono::steady_clock::now(), error ) ) {
            return std::nullopt;
        }
        if( std::optional<multiplayer_server_lobby_event> event = server.poll_event() ) {
            return event;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    } while( std::chrono::steady_clock::now() < deadline );
    error = "timed out waiting for server event";
    return std::nullopt;
}

struct completed_server_admission {
    multiplayer_protocol_envelope response;
    multiplayer_server_lobby_event committed;
    std::string resume_token;
    std::uint64_t session_generation = 0;
};

completed_server_admission authenticate_server_client(
    multiplayer_dedicated_server &server, asio::ip::tcp::socket &socket,
    const std::string &bearer_token, const std::uint64_t generation,
    std::string &error )
{
    multiplayer_authenticate_request authentication;
    authentication.display_name = "Confirmation Tester";
    authentication.bearer_token = bearer_token;
    multiplayer_protocol_envelope request;
    request.message_type = multiplayer_protocol_message_type::authenticate;
    request.sequence = 1;
    REQUIRE( multiplayer_build_authenticate_payload( authentication, request.payload, error ) );
    send_protocol_frame( socket, request );

    const std::optional<multiplayer_server_lobby_event> pending =
        wait_for_server_event( server, error );
    REQUIRE( pending );
    REQUIRE( pending->type == multiplayer_server_lobby_event_type::authentication_pending );
    multiplayer_server_lobby_admission_decision decision;
    decision.accepted = true;
    decision.player_id = pending->player_id;
    decision.character_id = pending->character_id;
    decision.session_generation = generation;
    decision.rejection = multiplayer_protocol_rejection::none;
    decision.message = "authentication accepted";
    multiplayer_server_lobby_prepared_admission prepared;
    REQUIRE( server.prepare_admission( *pending, decision, prepared, error ) );
    bool published = false;
    REQUIRE( server.publish_prepared_admission( std::move( prepared ), published, error ) );
    REQUIRE( published );
    REQUIRE( pump_until_readable( server, socket, error ) );

    completed_server_admission completed;
    completed.response = read_protocol_frame( socket );
    multiplayer_authentication_result result;
    REQUIRE( multiplayer_parse_authentication_result_payload( completed.response, result, error ) );
    REQUIRE( result.accepted );
    completed.resume_token = result.resume_token;
    completed.session_generation = result.session_generation;
    const std::optional<multiplayer_server_lobby_event> committed = server.poll_event();
    REQUIRE( committed );
    REQUIRE( committed->type == multiplayer_server_lobby_event_type::authenticated );
    completed.committed = *committed;
    return completed;
}

completed_server_admission resume_server_client(
    multiplayer_dedicated_server &server, asio::ip::tcp::socket &socket,
    const multiplayer_resume_request &resume, const std::uint64_t generation,
    std::string &error )
{
    multiplayer_protocol_envelope request;
    request.message_type = multiplayer_protocol_message_type::resume_request;
    request.sequence = 1;
    REQUIRE( multiplayer_build_resume_request_payload( resume, request.payload, error ) );
    send_protocol_frame( socket, request );

    const std::optional<multiplayer_server_lobby_event> pending =
        wait_for_server_event( server, error );
    REQUIRE( pending );
    REQUIRE( pending->type == multiplayer_server_lobby_event_type::resume_pending );
    multiplayer_server_lobby_admission_decision decision;
    decision.accepted = true;
    decision.player_id = pending->player_id;
    decision.character_id = pending->character_id;
    decision.session_generation = generation;
    decision.rejection = multiplayer_protocol_rejection::none;
    decision.message = "resume accepted";
    multiplayer_server_lobby_prepared_admission prepared;
    REQUIRE( server.prepare_admission( *pending, decision, prepared, error ) );
    bool published = false;
    REQUIRE( server.publish_prepared_admission( std::move( prepared ), published, error ) );
    REQUIRE( published );
    REQUIRE( pump_until_readable( server, socket, error ) );

    completed_server_admission completed;
    completed.response = read_protocol_frame( socket );
    multiplayer_resume_result result;
    REQUIRE( multiplayer_parse_resume_result_payload( completed.response, result, error ) );
    REQUIRE( result.accepted );
    completed.session_generation = result.session_generation;
    const std::optional<multiplayer_server_lobby_event> committed = server.poll_event();
    REQUIRE( committed );
    REQUIRE( committed->type == multiplayer_server_lobby_event_type::resumed );
    completed.committed = *committed;
    return completed;
}

} // namespace

struct multiplayer_server_test_support {
    static multiplayer_graceful_disconnect_result execute_graceful_disconnect_action(
        multiplayer_dedicated_server &server,
        multiplayer_server_lobby_action action,
        const multiplayer_connection_id expected_connection,
        std::string &error ) {
        return server.execute_graceful_disconnect_action( std::move( action ),
                expected_connection, error );
    }
};

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

    std::optional<multiplayer_server_lobby_event> pending_admission;
    const auto admission_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::seconds( 5 );
    do {
        REQUIRE( server.poll_once( std::chrono::steady_clock::now(), error ) );
        pending_admission = server.poll_event();
        if( !pending_admission ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    } while( !pending_admission &&
             std::chrono::steady_clock::now() < admission_deadline );
    REQUIRE( pending_admission );
    REQUIRE( pending_admission->type ==
             multiplayer_server_lobby_event_type::authentication_pending );
    CHECK( pending_admission->player_id == identity.player_id );
    CHECK( pending_admission->character_id == identity.character_id );
    CHECK( pending_admission->admission_id != 0 );
    CHECK( pending_admission->expected_session_generation == 0 );
    REQUIRE( server.admission_is_pending( *pending_admission ) );

    asio::error_code pre_publish_error;
    CHECK( client.available( pre_publish_error ) == 0 );
    CHECK_FALSE( pre_publish_error );

    multiplayer_server_lobby_admission_decision admission_decision;
    admission_decision.accepted = true;
    admission_decision.player_id = pending_admission->player_id;
    admission_decision.character_id = pending_admission->character_id;
    admission_decision.session_generation = 1;
    admission_decision.rejection = multiplayer_protocol_rejection::none;
    admission_decision.message = "authentication accepted";
    multiplayer_server_lobby_prepared_admission prepared_admission;
    REQUIRE( server.prepare_admission( *pending_admission, admission_decision,
                                       prepared_admission, error ) );

    CHECK( client.available( pre_publish_error ) == 0 );
    CHECK_FALSE( pre_publish_error );

    bool admission_published = false;
    REQUIRE( server.publish_prepared_admission( prepared_admission, admission_published,
             error ) );
    REQUIRE( admission_published );
    bool duplicate_published = true;
    REQUIRE( server.publish_prepared_admission( std::move( prepared_admission ),
             duplicate_published, error ) );
    CHECK_FALSE( duplicate_published );

    REQUIRE( pump_until_readable( server, client, error ) );
    response = read_protocol_frame( client );
    multiplayer_authentication_result authentication_result;
    REQUIRE( multiplayer_parse_authentication_result_payload( response,
             authentication_result, error ) );
    REQUIRE( authentication_result.accepted );
    CHECK( authentication_result.player_id == identity.player_id );
    CHECK( authentication_result.character_id == identity.character_id );
    CHECK( authentication_result.session_generation == 1 );
    REQUIRE( response.session != multiplayer_session_id {} );
    const multiplayer_session_id session = response.session;

    std::optional<multiplayer_server_lobby_event> server_event = server.poll_event();
    REQUIRE( server_event );
    CHECK( server_event->type == multiplayer_server_lobby_event_type::authenticated );
    CHECK( server_event->player_id == authentication_result.player_id );
    CHECK( server_event->session_generation == authentication_result.session_generation );

    asio::error_code duplicate_response_error;
    CHECK( client.available( duplicate_response_error ) == 0 );
    CHECK_FALSE( duplicate_response_error );

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

    REQUIRE( server.complete_graceful_disconnect( ordered_events.back(), error ) ==
             multiplayer_graceful_disconnect_result::acknowledgement_queued );
    CHECK( error.empty() );
    CHECK( server.connection_is_closing( ordered_events.back().connection ) );

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
    CHECK_FALSE( server.connection_is_closing( disconnected_event->connection ) );

    CHECK( server.complete_graceful_disconnect( ordered_events.back(), error ) ==
           multiplayer_graceful_disconnect_result::stale_request );
    CHECK( error.empty() );

    server.stop();
    CHECK_FALSE( server.running() );
    CHECK( server.complete_graceful_disconnect( ordered_events.back(), error ) ==
           multiplayer_graceful_disconnect_result::server_or_transport_fatal );
    CHECK( error == "dedicated server is not running" );
}

TEST_CASE( "multiplayer_dedicated_server_treats_missing_graceful_response_as_no_ack_fallback",
           "[multiplayer][dedicated_server]" )
{
    const std::string unique = "cdda-multiplayer-graceful-fallback-" + std::to_string(
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

    const std::string build_id = "graceful-fallback-test";
    const std::string content_manifest =
        "sha256-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    multiplayer_server_player_identity identity;
    identity.player_id = "12345678-1234-4234-9234-123456789abc";
    identity.character_id = "42";
    multiplayer_dedicated_server server( config, config_path, build_id, content_manifest,
                                         identity );
    REQUIRE( server.start( error ) );

    std::string bearer_token;
    REQUIRE( load_multiplayer_server_bearer_token( config_path, config, bearer_token, error ) );
    asio::io_context client_io;
    asio::ip::tcp::socket client( client_io );
    client.connect( asio::ip::tcp::endpoint( asio::ip::address_v4::loopback(), port ) );
    negotiate_server_client( server, client, build_id, content_manifest, error );
    const completed_server_admission authentication = authenticate_server_client(
                server, client, bearer_token, 1, error );

    multiplayer_protocol_envelope request;
    request.message_type = multiplayer_protocol_message_type::disconnect_notice;
    request.session = authentication.response.session;
    request.sequence = 2;
    REQUIRE( multiplayer_build_disconnect_notice_payload(
    { multiplayer_protocol_rejection::none, "leave without response" },
    request.payload, error ) );
    send_protocol_frame( client, request );
    const std::optional<multiplayer_server_lobby_event> graceful_request =
        wait_for_server_event( server, error );
    REQUIRE( graceful_request );
    REQUIRE( graceful_request->type ==
             multiplayer_server_lobby_event_type::graceful_disconnect_requested );

    multiplayer_server_lobby_action fallback;
    fallback.type = multiplayer_server_lobby_action_type::disconnect;
    fallback.connection = graceful_request->connection;
    fallback.reason = "graceful disconnect response failed";
    CHECK( multiplayer_server_test_support::execute_graceful_disconnect_action(
               server, std::move( fallback ), graceful_request->connection, error ) ==
           multiplayer_graceful_disconnect_result::fallback_close_response_unavailable );
    CHECK( error.empty() );
    CHECK( server.running() );
    CHECK( server.connection_is_closing( graceful_request->connection ) );

    const std::optional<multiplayer_server_lobby_event> disconnected =
        wait_for_server_event( server, error );
    REQUIRE( disconnected );
    CHECK( disconnected->type == multiplayer_server_lobby_event_type::disconnected );
    CHECK( disconnected->connection == graceful_request->connection );
    CHECK_FALSE( server.connection_is_closing( disconnected->connection ) );
    CHECK( server.running() );

    server.stop();
}

TEST_CASE( "multiplayer_dedicated_server_tracks_public_and_admission_closing_decisions",
           "[multiplayer][dedicated_server]" )
{
    const std::string unique = "cdda-multiplayer-closing-decisions-" + std::to_string(
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

    const std::string build_id = "closing-decision-test";
    const std::string content_manifest =
        "sha256-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    multiplayer_server_player_identity identity;
    identity.player_id = "12345678-1234-4234-9234-123456789abc";
    identity.character_id = "42";
    multiplayer_dedicated_server server( config, config_path, build_id, content_manifest,
                                         identity );
    REQUIRE( server.start( error ) );

    std::string bearer_token;
    REQUIRE( load_multiplayer_server_bearer_token( config_path, config, bearer_token, error ) );
    asio::io_context client_io;
    asio::ip::tcp::socket client( client_io );
    client.connect( asio::ip::tcp::endpoint( asio::ip::address_v4::loopback(), port ) );
    negotiate_server_client( server, client, build_id, content_manifest, error );

    SECTION( "the public disconnect path marks before transport delivery" ) {
        const completed_server_admission authentication = authenticate_server_client(
                    server, client, bearer_token, 1, error );
        const multiplayer_connection_id connection = authentication.committed.connection;
        CHECK_FALSE( server.connection_is_closing( connection ) );

        server.disconnect( connection, "simulation owner rejected the connection" );
        CHECK( server.connection_is_closing( connection ) );

        const std::optional<multiplayer_server_lobby_event> disconnected =
            wait_for_server_event( server, error );
        REQUIRE( disconnected );
        CHECK( disconnected->type == multiplayer_server_lobby_event_type::disconnected );
        CHECK( disconnected->connection == connection );
        CHECK_FALSE( server.connection_is_closing( connection ) );
    }

    SECTION( "a rejected prepared admission is closing before response delivery" ) {
        multiplayer_authenticate_request authentication;
        authentication.display_name = "Rejected Tester";
        authentication.bearer_token = bearer_token;
        multiplayer_protocol_envelope request;
        request.message_type = multiplayer_protocol_message_type::authenticate;
        request.sequence = 1;
        REQUIRE( multiplayer_build_authenticate_payload( authentication, request.payload,
                 error ) );
        send_protocol_frame( client, request );

        const std::optional<multiplayer_server_lobby_event> pending =
            wait_for_server_event( server, error );
        REQUIRE( pending );
        REQUIRE( pending->type == multiplayer_server_lobby_event_type::authentication_pending );
        CHECK_FALSE( server.connection_is_closing( pending->connection ) );

        multiplayer_server_lobby_admission_decision decision;
        decision.rejection = multiplayer_protocol_rejection::permission_denied;
        decision.message = "test admission rejected";
        multiplayer_server_lobby_prepared_admission prepared;
        REQUIRE( server.prepare_admission( *pending, decision, prepared, error ) );
        bool published = false;
        REQUIRE( server.publish_prepared_admission( std::move( prepared ), published, error ) );
        REQUIRE( published );
        CHECK( server.connection_is_closing( pending->connection ) );

        REQUIRE( pump_until_readable( server, client, error ) );
        const multiplayer_protocol_envelope response = read_protocol_frame( client );
        multiplayer_authentication_result rejected;
        REQUIRE( multiplayer_parse_authentication_result_payload( response, rejected, error ) );
        CHECK_FALSE( rejected.accepted );
        CHECK( rejected.rejection == multiplayer_protocol_rejection::permission_denied );

        std::array<std::uint8_t, 1> end = {};
        asio::error_code close_error;
        client.read_some( asio::buffer( end ), close_error );
        CHECK( close_error == asio::error::eof );
        const auto terminal_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds( 5 );
        do {
            REQUIRE( server.poll_once( std::chrono::steady_clock::now(), error ) );
            if( server.connection_is_closing( pending->connection ) ) {
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            }
        } while( server.connection_is_closing( pending->connection ) &&
                 std::chrono::steady_clock::now() < terminal_deadline );
        CHECK_FALSE( server.connection_is_closing( pending->connection ) );
    }

    server.disconnect( 999999, "test stale connection" );
    CHECK( server.connection_is_closing( 999999 ) );
    server.stop();
    CHECK_FALSE( server.connection_is_closing( 999999 ) );
}

TEST_CASE( "multiplayer_dedicated_server_marks_a_closing_batch_before_delivering_its_command",
           "[multiplayer][dedicated_server]" )
{
    const std::string unique = "cdda-multiplayer-closing-batch-" + std::to_string(
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

    const std::string build_id = "closing-batch-test";
    const std::string content_manifest =
        "sha256-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    multiplayer_server_player_identity identity;
    identity.player_id = "12345678-1234-4234-9234-123456789abc";
    identity.character_id = "42";
    multiplayer_dedicated_server server( config, config_path, build_id, content_manifest,
                                         identity );
    REQUIRE( server.start( error ) );

    std::string bearer_token;
    REQUIRE( load_multiplayer_server_bearer_token( config_path, config, bearer_token, error ) );
    asio::io_context client_io;
    asio::ip::tcp::socket client( client_io );
    const asio::ip::tcp::endpoint endpoint( asio::ip::address_v4::loopback(), port );
    client.connect( endpoint );
    negotiate_server_client( server, client, build_id, content_manifest, error );
    const completed_server_admission authentication = authenticate_server_client(
                server, client, bearer_token, 1, error );

    multiplayer_protocol_envelope initial_ping;
    initial_ping.message_type = multiplayer_protocol_message_type::ping;
    initial_ping.session = authentication.response.session;
    initial_ping.sequence = 2;
    REQUIRE( multiplayer_build_ping_payload( { 2, 2 }, initial_ping.payload, error ) );
    send_protocol_frame( client, initial_ping );
    REQUIRE( pump_until_readable( server, client, error ) );
    multiplayer_protocol_heartbeat initial_pong;
    REQUIRE( multiplayer_parse_pong_payload( read_protocol_frame( client ), initial_pong,
             error ) );
    CHECK_FALSE( server.poll_event() );

    asio::error_code close_error;
    client.close( close_error );
    REQUIRE_FALSE( close_error );
    const std::optional<multiplayer_server_lobby_event> first_disconnect =
        wait_for_server_event( server, error );
    REQUIRE( first_disconnect );
    REQUIRE( first_disconnect->type == multiplayer_server_lobby_event_type::disconnected );

    multiplayer_resume_request resume;
    resume.resume_token = authentication.resume_token;
    resume.last_client_sequence = 2;
    resume.session_generation = authentication.session_generation;
    client = asio::ip::tcp::socket( client_io );
    client.connect( endpoint );
    negotiate_server_client( server, client, build_id, content_manifest, error );
    const completed_server_admission resumed = resume_server_client(
                server, client, resume, 2, error );
    const multiplayer_connection_id connection = resumed.committed.connection;

    multiplayer_player_command command;
    command.client_sequence = 3;
    command.base_revision = 0;
    command.kind = multiplayer_command_kind::wait;
    multiplayer_protocol_envelope command_envelope;
    command_envelope.message_type = multiplayer_protocol_message_type::player_command;
    command_envelope.session = resumed.response.session;
    command_envelope.sequence = command.client_sequence;
    REQUIRE( multiplayer_build_player_command_payload( command, command_envelope.payload, error ) );

    multiplayer_protocol_envelope malformed_ping;
    malformed_ping.message_type = multiplayer_protocol_message_type::ping;
    malformed_ping.session = resumed.response.session;
    malformed_ping.sequence = 4;
    REQUIRE( multiplayer_build_ping_payload( { 4, 4 }, malformed_ping.payload, error ) );

    multiplayer_transport_payload wire_batch;
    for( const multiplayer_protocol_envelope *envelope : {
             &command_envelope, &malformed_ping
         } ) {
        multiplayer_transport_payload protocol_bytes;
        multiplayer_transport_payload frame;
        REQUIRE( multiplayer_encode_protocol_envelope( *envelope, protocol_bytes, error ) );
        if( envelope == &malformed_ping ) {
            REQUIRE( protocol_bytes.size() > multiplayer_protocol_envelope_size + 4 );
            std::fill_n( protocol_bytes.begin() + multiplayer_protocol_envelope_size, 4, 0xff );
        }
        REQUIRE( multiplayer_encode_transport_frame( protocol_bytes, frame, error ) );
        wire_batch.insert( wire_batch.end(), frame.begin(), frame.end() );
    }
    asio::write( client, asio::buffer( wire_batch ) );

    // Keep the earlier command queued while the wrapper drains both ordered frames.  The later
    // malformed control must mark the connection before the simulation thread is allowed to
    // inspect that command event.
    const auto closing_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds( 5 );
    do {
        REQUIRE( server.poll_once( std::chrono::steady_clock::now(), error ) );
        if( !server.connection_is_closing( connection ) ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    } while( !server.connection_is_closing( connection ) &&
             std::chrono::steady_clock::now() < closing_deadline );
    REQUIRE( server.connection_is_closing( connection ) );

    const std::optional<multiplayer_server_lobby_event> queued_command = server.poll_event();
    REQUIRE( queued_command );
    REQUIRE( queued_command->type == multiplayer_server_lobby_event_type::application_message );
    CHECK( queued_command->message.message_type ==
           multiplayer_protocol_message_type::player_command );
    CHECK( queued_command->message.sequence == command.client_sequence );
    CHECK( queued_command->confirms_resume_generation );
    CHECK( server.connection_is_closing( connection ) );

    const std::optional<multiplayer_server_lobby_event> terminal =
        wait_for_server_event( server, error );
    REQUIRE( terminal );
    CHECK( terminal->type == multiplayer_server_lobby_event_type::disconnected );
    CHECK( terminal->connection == connection );
    CHECK_FALSE( server.connection_is_closing( connection ) );

    server.stop();
}

TEST_CASE( "multiplayer_dedicated_server_does_not_confirm_malformed_resumed_control",
           "[multiplayer][dedicated_server]" )
{
    const std::string unique = "cdda-multiplayer-confirmation-" + std::to_string(
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

    const std::string build_id = "confirmation-test";
    const std::string content_manifest =
        "sha256-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    multiplayer_server_player_identity identity;
    identity.player_id = "12345678-1234-4234-9234-123456789abc";
    identity.character_id = "42";
    multiplayer_dedicated_server server( config, config_path, build_id, content_manifest,
                                         identity );
    REQUIRE( server.start( error ) );

    std::string bearer_token;
    REQUIRE( load_multiplayer_server_bearer_token( config_path, config, bearer_token, error ) );
    asio::io_context client_io;
    asio::ip::tcp::socket client( client_io );
    const asio::ip::tcp::endpoint endpoint( asio::ip::address_v4::loopback(), port );
    client.connect( endpoint );
    negotiate_server_client( server, client, build_id, content_manifest, error );
    const completed_server_admission authentication = authenticate_server_client(
                server, client, bearer_token, 1, error );

    multiplayer_protocol_envelope ping;
    ping.message_type = multiplayer_protocol_message_type::ping;
    ping.session = authentication.response.session;
    ping.sequence = 2;
    REQUIRE( multiplayer_build_ping_payload( { 2, 2 }, ping.payload, error ) );
    send_protocol_frame( client, ping );
    REQUIRE( pump_until_readable( server, client, error ) );
    multiplayer_protocol_envelope pong = read_protocol_frame( client );
    multiplayer_protocol_heartbeat heartbeat;
    REQUIRE( multiplayer_parse_pong_payload( pong, heartbeat, error ) );
    CHECK_FALSE( server.poll_event() );

    asio::error_code close_error;
    client.close( close_error );
    REQUIRE_FALSE( close_error );
    std::optional<multiplayer_server_lobby_event> disconnected =
        wait_for_server_event( server, error );
    REQUIRE( disconnected );
    REQUIRE( disconnected->type == multiplayer_server_lobby_event_type::disconnected );

    multiplayer_resume_request resume;
    resume.resume_token = authentication.resume_token;
    resume.last_server_revision = 0;
    resume.last_client_sequence = 2;
    resume.session_generation = authentication.session_generation;
    client = asio::ip::tcp::socket( client_io );
    client.connect( endpoint );
    negotiate_server_client( server, client, build_id, content_manifest, error );
    const completed_server_admission resumed = resume_server_client(
                server, client, resume, 2, error );

    std::uint64_t confirmation_sequence = 4;
    SECTION( "a malformed ping is rejected by the dedicated wrapper" ) {
        multiplayer_protocol_envelope malformed;
        malformed.message_type = multiplayer_protocol_message_type::ping;
        malformed.session = resumed.response.session;
        malformed.sequence = 3;
        REQUIRE( multiplayer_build_ping_payload( { 3, 3 }, malformed.payload, error ) );
        multiplayer_transport_payload malformed_protocol;
        REQUIRE( multiplayer_encode_protocol_envelope( malformed, malformed_protocol, error ) );
        REQUIRE( malformed_protocol.size() > multiplayer_protocol_envelope_size + 4 );
        std::fill_n( malformed_protocol.begin() + multiplayer_protocol_envelope_size, 4, 0xff );
        multiplayer_transport_payload malformed_frame;
        REQUIRE( multiplayer_encode_transport_frame( malformed_protocol, malformed_frame, error ) );
        asio::write( client, asio::buffer( malformed_frame ) );
    }
    SECTION( "a malformed resync is rejected before lobby confirmation" ) {
        confirmation_sequence = 3;
        multiplayer_protocol_envelope malformed;
        malformed.message_type = multiplayer_protocol_message_type::resync_request;
        malformed.session = resumed.response.session;
        malformed.sequence = 3;
        REQUIRE( multiplayer_build_resync_request_payload( { 0, "x" }, malformed.payload,
                 error ) );
        const auto reason = std::find( malformed.payload.begin(), malformed.payload.end(), 'x' );
        REQUIRE( reason != malformed.payload.end() );
        *reason = 0x01;
        send_protocol_frame( client, malformed );
    }
    disconnected = wait_for_server_event( server, error );
    REQUIRE( disconnected );
    CHECK( disconnected->type == multiplayer_server_lobby_event_type::disconnected );
    CHECK_FALSE( server.poll_event() );

    client.close( close_error );
    client = asio::ip::tcp::socket( client_io );
    client.connect( endpoint );
    negotiate_server_client( server, client, build_id, content_manifest, error );
    const completed_server_admission replayed = resume_server_client(
                server, client, resume, 2, error );
    CHECK( replayed.session_generation == resumed.session_generation );

    ping = {};
    ping.message_type = multiplayer_protocol_message_type::ping;
    ping.session = replayed.response.session;
    ping.sequence = confirmation_sequence;
    REQUIRE( multiplayer_build_ping_payload(
    { confirmation_sequence, confirmation_sequence }, ping.payload, error ) );
    send_protocol_frame( client, ping );
    REQUIRE( pump_until_readable( server, client, error ) );
    pong = read_protocol_frame( client );
    REQUIRE( multiplayer_parse_pong_payload( pong, heartbeat, error ) );
    const std::optional<multiplayer_server_lobby_event> confirmation = server.poll_event();
    REQUIRE( confirmation );
    REQUIRE( confirmation->type == multiplayer_server_lobby_event_type::session_confirmed );
    CHECK( confirmation->confirms_resume_generation );
    REQUIRE( server.record_session_confirmed( *confirmation, error ) );

    server.stop();
}

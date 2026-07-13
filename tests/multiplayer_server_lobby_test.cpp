#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cata_catch.h"
#include "multiplayer_protocol.h"
#include "multiplayer_server_lobby.h"
#include "multiplayer_transport.h"

namespace
{

multiplayer_server_lobby_settings lobby_settings()
{
    multiplayer_server_lobby_settings settings;
    settings.server_build_id = "server-test-build";
    settings.world_id = "test-world";
    settings.content_manifest = "test-content";
    settings.savegame_version = 39;
    settings.bearer_token = std::string( 64, 'a' );
    settings.maximum_players = 2;
    return settings;
}

multiplayer_transport_event connected_event( const multiplayer_connection_id connection,
        const std::string &peer = "127.0.0.1" )
{
    return { multiplayer_transport_event_type::connected, connection, {}, {}, peer };
}

multiplayer_transport_event closed_event( const multiplayer_connection_id connection )
{
    return { multiplayer_transport_event_type::disconnected, connection, {}, "closed", {} };
}

multiplayer_transport_event frame_event( const multiplayer_connection_id connection,
        const multiplayer_protocol_envelope &envelope )
{
    multiplayer_transport_event event;
    event.type = multiplayer_transport_event_type::frame;
    event.connection = connection;
    std::string error;
    REQUIRE( multiplayer_encode_protocol_envelope( envelope, event.payload, error ) );
    return event;
}

multiplayer_protocol_envelope hello_envelope( const std::uint16_t major =
            multiplayer_protocol_current_major )
{
    multiplayer_client_hello hello;
    hello.protocol_major = major;
    hello.client_kind = multiplayer_protocol_client_kind::headless_test;
    hello.build_id = "server-test-build";
    hello.content_manifest = "test-content";
    hello.savegame_version = 39;
    hello.capabilities = { { "semantic-scene", 1, true } };
    hello.client_nonce[0] = 1;
    multiplayer_protocol_envelope envelope;
    envelope.protocol_major = major;
    envelope.message_type = multiplayer_protocol_message_type::client_hello;
    std::string error;
    REQUIRE( multiplayer_build_client_hello_payload( hello, envelope.payload, error ) );
    return envelope;
}

multiplayer_protocol_envelope authenticate_envelope( const std::string &token )
{
    multiplayer_authenticate_request request;
    request.display_name = "Lobby Tester";
    request.bearer_token = token;
    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::authenticate;
    envelope.sequence = 1;
    std::string error;
    REQUIRE( multiplayer_build_authenticate_payload( request, envelope.payload, error ) );
    return envelope;
}

multiplayer_protocol_envelope decode_action( const multiplayer_server_lobby_action &action )
{
    REQUIRE( action.type == multiplayer_server_lobby_action_type::send );
    multiplayer_protocol_envelope envelope;
    std::string error;
    REQUIRE( multiplayer_decode_protocol_envelope( action.payload, envelope, error ) );
    return envelope;
}

void complete_hello( multiplayer_server_lobby &lobby, const multiplayer_connection_id connection,
                     const multiplayer_server_lobby::clock::time_point now )
{
    REQUIRE( lobby.handle_transport_event( connected_event( connection ), now ).empty() );
    const std::vector<multiplayer_server_lobby_action> actions =
        lobby.handle_transport_event( frame_event( connection, hello_envelope() ), now );
    REQUIRE( actions.size() == 1 );
    const multiplayer_protocol_envelope response = decode_action( actions.front() );
    multiplayer_server_hello hello;
    std::string error;
    REQUIRE( multiplayer_parse_server_hello_payload( response, hello, error ) );
    REQUIRE( hello.accepted );
}

} // namespace

TEST_CASE( "multiplayer_server_lobby_authenticates_and_resumes_without_live_game_objects",
           "[multiplayer][server_lobby]" )
{
    multiplayer_server_lobby lobby( lobby_settings() );
    std::string error;
    REQUIRE( lobby.valid( error ) );
    const multiplayer_server_lobby::clock::time_point now{};
    complete_hello( lobby, 1, now );

    std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event(
                frame_event( 1, authenticate_envelope( std::string( 64, 'a' ) ) ), now );
    REQUIRE( actions.size() == 1 );
    multiplayer_protocol_envelope response = decode_action( actions.front() );
    multiplayer_authentication_result authentication;
    REQUIRE( multiplayer_parse_authentication_result_payload( response, authentication, error ) );
    REQUIRE( authentication.accepted );
    CHECK( authentication.session_generation == 1 );
    CHECK( response.session != multiplayer_session_id {} );
    const std::string resume_token = authentication.resume_token;

    std::optional<multiplayer_server_lobby_event> event = lobby.poll_event();
    REQUIRE( event );
    CHECK( event->type == multiplayer_server_lobby_event_type::authenticated );
    CHECK( event->player_id == authentication.player_id );
    CHECK( event->character_id == authentication.character_id );
    CHECK( lobby.authenticated_player_count() == 1 );

    multiplayer_protocol_heartbeat ping;
    ping.nonce = 5;
    multiplayer_protocol_envelope ping_envelope;
    ping_envelope.message_type = multiplayer_protocol_message_type::ping;
    ping_envelope.session = response.session;
    ping_envelope.sequence = 2;
    REQUIRE( multiplayer_build_ping_payload( ping, ping_envelope.payload, error ) );
    CHECK( lobby.handle_transport_event( frame_event( 1, ping_envelope ), now ).empty() );
    event = lobby.poll_event();
    REQUIRE( event );
    CHECK( event->type == multiplayer_server_lobby_event_type::application_message );
    CHECK( event->player_id == authentication.player_id );
    CHECK( event->character_id == authentication.character_id );
    CHECK( event->message.sequence == 2 );
    const std::vector<multiplayer_server_lobby_action> duplicate =
        lobby.handle_transport_event( frame_event( 1, ping_envelope ), now );
    REQUIRE( duplicate.size() == 1 );
    CHECK( duplicate.front().type == multiplayer_server_lobby_action_type::disconnect );
    CHECK( duplicate.front().reason.find( "sequence" ) != std::string::npos );

    lobby.handle_transport_event( closed_event( 1 ), now + std::chrono::seconds( 1 ) );
    event = lobby.poll_event();
    REQUIRE( event );
    CHECK( event->type == multiplayer_server_lobby_event_type::disconnected );
    CHECK( lobby.authenticated_player_count() == 0 );

    complete_hello( lobby, 2, now + std::chrono::seconds( 2 ) );
    multiplayer_resume_request resume;
    resume.resume_token = resume_token;
    resume.last_server_revision = 12;
    resume.last_client_sequence = 7;
    multiplayer_protocol_envelope resume_envelope;
    resume_envelope.message_type = multiplayer_protocol_message_type::resume_request;
    resume_envelope.sequence = 1;
    REQUIRE( multiplayer_build_resume_request_payload( resume, resume_envelope.payload, error ) );
    actions = lobby.handle_transport_event( frame_event( 2, resume_envelope ),
                                            now + std::chrono::seconds( 2 ) );
    REQUIRE( actions.size() == 1 );
    response = decode_action( actions.front() );
    multiplayer_resume_result resume_result;
    REQUIRE( multiplayer_parse_resume_result_payload( response, resume_result, error ) );
    CHECK( resume_result.accepted );
    CHECK( resume_result.session_generation == 2 );
    CHECK( resume_result.replay_from_sequence == 8 );
    CHECK( resume_result.player_id == authentication.player_id );
    CHECK( response.session != multiplayer_session_id {} );

    event = lobby.poll_event();
    REQUIRE( event );
    CHECK( event->type == multiplayer_server_lobby_event_type::resumed );
    CHECK( event->last_server_revision == 12 );
    CHECK( event->last_client_sequence == 7 );
}

TEST_CASE( "multiplayer_server_lobby_rejects_bad_auth_versions_and_timeouts",
           "[multiplayer][server_lobby]" )
{
    const multiplayer_server_lobby::clock::time_point now{};

    SECTION( "wrong bearer token" ) {
        multiplayer_server_lobby lobby( lobby_settings() );
        complete_hello( lobby, 1, now );
        const std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event(
                    frame_event( 1, authenticate_envelope( std::string( 64, 'b' ) ) ), now );
        REQUIRE( actions.size() == 2 );
        multiplayer_authentication_result result;
        std::string error;
        REQUIRE( multiplayer_parse_authentication_result_payload(
                     decode_action( actions.front() ), result, error ) );
        CHECK_FALSE( result.accepted );
        CHECK( result.rejection == multiplayer_protocol_rejection::authentication_failed );
        CHECK( actions.back().type == multiplayer_server_lobby_action_type::disconnect );
    }

    SECTION( "protocol major mismatch receives typed rejection" ) {
        multiplayer_server_lobby lobby( lobby_settings() );
        REQUIRE( lobby.handle_transport_event( connected_event( 1 ), now ).empty() );
        const std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event(
                    frame_event( 1, hello_envelope( multiplayer_protocol_current_major + 1 ) ), now );
        REQUIRE( actions.size() == 2 );
        multiplayer_server_hello result;
        std::string error;
        REQUIRE( multiplayer_parse_server_hello_payload( decode_action( actions.front() ),
                 result, error ) );
        CHECK_FALSE( result.accepted );
        CHECK( result.rejection == multiplayer_protocol_rejection::protocol_major_mismatch );
    }

    SECTION( "handshake timeout" ) {
        multiplayer_server_lobby_settings settings = lobby_settings();
        settings.handshake_timeout = std::chrono::milliseconds( 50 );
        multiplayer_server_lobby lobby( settings );
        REQUIRE( lobby.handle_transport_event( connected_event( 1 ), now ).empty() );
        CHECK( lobby.tick( now + std::chrono::milliseconds( 49 ) ).empty() );
        const std::vector<multiplayer_server_lobby_action> actions =
            lobby.tick( now + std::chrono::milliseconds( 50 ) );
        REQUIRE( actions.size() == 1 );
        CHECK( actions.front().type == multiplayer_server_lobby_action_type::disconnect );
        CHECK( actions.front().reason == "handshake timeout" );
        CHECK( lobby.tick( now + std::chrono::seconds( 1 ) ).empty() );
    }

    SECTION( "authenticated application message rate" ) {
        multiplayer_server_lobby_settings settings = lobby_settings();
        settings.maximum_application_messages_per_second = 1;
        multiplayer_server_lobby lobby( settings );
        complete_hello( lobby, 1, now );
        std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event(
                    frame_event( 1, authenticate_envelope( std::string( 64, 'a' ) ) ), now );
        REQUIRE( actions.size() == 1 );
        const multiplayer_protocol_envelope authentication_response = decode_action( actions.front() );
        REQUIRE( lobby.poll_event() );
        multiplayer_protocol_heartbeat ping;
        multiplayer_protocol_envelope ping_envelope;
        ping_envelope.message_type = multiplayer_protocol_message_type::ping;
        ping_envelope.session = authentication_response.session;
        ping_envelope.sequence = 2;
        std::string error;
        REQUIRE( multiplayer_build_ping_payload( ping, ping_envelope.payload, error ) );
        CHECK( lobby.handle_transport_event( frame_event( 1, ping_envelope ), now ).empty() );
        ping_envelope.sequence = 3;
        actions = lobby.handle_transport_event( frame_event( 1, ping_envelope ), now );
        REQUIRE( actions.size() == 1 );
        CHECK( actions.front().type == multiplayer_server_lobby_action_type::disconnect );
        CHECK( actions.front().reason.find( "rate" ) != std::string::npos );
    }

    SECTION( "disconnected identity reserves capacity only until resume expiry" ) {
        multiplayer_server_lobby_settings settings = lobby_settings();
        settings.maximum_players = 1;
        settings.resume_token_lifetime = std::chrono::seconds( 1 );
        multiplayer_server_lobby lobby( settings );
        complete_hello( lobby, 1, now );
        std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event(
                    frame_event( 1, authenticate_envelope( std::string( 64, 'a' ) ) ), now );
        REQUIRE( actions.size() == 1 );
        REQUIRE( lobby.poll_event() );
        lobby.handle_transport_event( closed_event( 1 ), now );
        REQUIRE( lobby.poll_event() );

        complete_hello( lobby, 2, now + std::chrono::milliseconds( 1 ) );
        actions = lobby.handle_transport_event(
                      frame_event( 2, authenticate_envelope( std::string( 64, 'a' ) ) ),
                      now + std::chrono::milliseconds( 1 ) );
        REQUIRE( actions.size() == 2 );
        multiplayer_authentication_result rejected;
        std::string error;
        REQUIRE( multiplayer_parse_authentication_result_payload(
                     decode_action( actions.front() ), rejected, error ) );
        CHECK( rejected.rejection == multiplayer_protocol_rejection::server_full );
        lobby.handle_transport_event( closed_event( 2 ), now + std::chrono::milliseconds( 1 ) );

        CHECK( lobby.tick( now + std::chrono::seconds( 1 ) ).empty() );
        complete_hello( lobby, 3, now + std::chrono::milliseconds( 1001 ) );
        actions = lobby.handle_transport_event(
                      frame_event( 3, authenticate_envelope( std::string( 64, 'a' ) ) ),
                      now + std::chrono::milliseconds( 1001 ) );
        REQUIRE( actions.size() == 1 );
        multiplayer_authentication_result accepted;
        REQUIRE( multiplayer_parse_authentication_result_payload(
                     decode_action( actions.front() ), accepted, error ) );
        CHECK( accepted.accepted );
    }

    SECTION( "per-address authentication rate" ) {
        multiplayer_server_lobby_settings settings = lobby_settings();
        settings.maximum_attempts_per_minute = 1;
        multiplayer_server_lobby lobby( settings );
        complete_hello( lobby, 1, now );
        lobby.handle_transport_event(
            frame_event( 1, authenticate_envelope( std::string( 64, 'b' ) ) ), now );
        lobby.handle_transport_event( closed_event( 1 ), now );
        complete_hello( lobby, 2, now );
        const std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event(
                    frame_event( 2, authenticate_envelope( std::string( 64, 'a' ) ) ), now );
        REQUIRE( actions.size() == 2 );
        multiplayer_authentication_result result;
        std::string error;
        REQUIRE( multiplayer_parse_authentication_result_payload(
                     decode_action( actions.front() ), result, error ) );
        CHECK( result.rejection == multiplayer_protocol_rejection::rate_limited );
    }
}

#include "catch/catch.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>

#include "multiplayer_client.h"
#include "multiplayer_protocol.h"
#include "multiplayer_transport.h"

namespace
{

constexpr char test_player_id[] = "00000000-0000-4000-8000-000000000001";
constexpr char test_character_id[] = "client-test-character";
constexpr char test_build_id[] = "multiplayer-client-test-build";
constexpr char test_manifest[] =
    "sha256-0000000000000000000000000000000000000000000000000000000000000000";
constexpr char test_resume_token[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

enum class scripted_server_behavior : std::uint8_t {
    normal,
    drop_first_hello,
    authentication_rejected,
    drop_first_command,
    pong_then_drop_first_command,
    accepted_without_semantic_scene,
    resume_without_generation_advance,
    resume_with_wrong_replay_boundary,
    resume_scene_ahead_of_cached_result,
    delay_scene_after_command_result,
    resume_session_expired,
    wrong_graceful_disconnect_sequence
};

multiplayer_scene_snapshot test_scene( const std::uint64_t revision )
{
    multiplayer_scene_snapshot scene;
    scene.server_revision = revision;
    scene.turn = static_cast<std::int64_t>( revision );
    scene.player.player_id = test_player_id;
    scene.player.character_id = test_character_id;
    scene.player.revision = revision;
    scene.player.position = { 10, 20, 0 };
    scene.player.moves = 100;
    multiplayer_visible_tile tile;
    tile.position = scene.player.position;
    tile.terrain_id = "t_grass";
    scene.tiles.emplace_back( std::move( tile ) );
    return scene;
}

class scripted_multiplayer_server
{
    public:
        explicit scripted_multiplayer_server(
            const scripted_server_behavior behavior = scripted_server_behavior::normal ) :
            behavior_( behavior ) {
            std::string error;
            REQUIRE( transport_.start( { "127.0.0.1", 0 }, error ) );
            first_session_.fill( 1 );
            resumed_session_.fill( 2 );
        }

        ~scripted_multiplayer_server() {
            transport_.stop();
        }

        std::uint16_t port() const {
            return transport_.bound_port();
        }

        void poll() {
            while( std::optional<multiplayer_transport_event> event = transport_.poll_event() ) {
                if( event->type == multiplayer_transport_event_type::connected ) {
                    connection_ = event->connection;
                } else if( event->type == multiplayer_transport_event_type::frame ) {
                    handle_frame( event->connection, event->payload );
                }
            }
        }

        bool command_seen() const {
            return command_seen_;
        }

        bool duplicate_sent() const {
            return duplicate_sent_;
        }

        bool resumed() const {
            return resumed_;
        }

        bool graceful_disconnect_seen() const {
            return graceful_disconnect_seen_;
        }

        std::uint64_t resume_last_client_sequence() const {
            return resume_last_client_sequence_;
        }

        void send_scene_for_test( const std::uint64_t revision, const bool wrong_session = false ) {
            multiplayer_session_id session = resumed_ ? resumed_session_ : first_session_;
            if( wrong_session ) {
                session.fill( 9 );
            }
            multiplayer_protocol_envelope response;
            response.message_type = multiplayer_protocol_message_type::scene_snapshot;
            response.session = session;
            response.sequence = revision;
            std::string error;
            REQUIRE( multiplayer_build_scene_snapshot_payload(
                         test_scene( revision ), response.payload, error ) );
            send( connection_, std::move( response ) );
        }

        void send_disconnect_notice_for_test() {
            multiplayer_protocol_envelope response;
            response.message_type = multiplayer_protocol_message_type::disconnect_notice;
            response.session = resumed_ ? resumed_session_ : first_session_;
            response.sequence = ++server_notice_sequence_;
            std::string error;
            REQUIRE( multiplayer_build_disconnect_notice_payload(
            { multiplayer_protocol_rejection::none, "server is shutting down" },
            response.payload, error ) );
            send( connection_, std::move( response ) );
        }

    private:
        void send( const multiplayer_connection_id connection,
                   multiplayer_protocol_envelope envelope ) {
            multiplayer_transport_payload encoded;
            std::string error;
            REQUIRE( multiplayer_encode_protocol_envelope( envelope, encoded, error ) );
            REQUIRE( transport_.send( connection, std::move( encoded ) ) ==
                     multiplayer_transport_send_result::queued );
        }

        void send_scene( const multiplayer_connection_id connection,
                         const multiplayer_session_id &session ) {
            multiplayer_protocol_envelope response;
            response.message_type = multiplayer_protocol_message_type::scene_snapshot;
            response.session = session;
            response.sequence = revision_;
            std::string error;
            REQUIRE( multiplayer_build_scene_snapshot_payload(
                         test_scene( revision_ ), response.payload, error ) );
            send( connection, std::move( response ) );
        }

        void handle_frame( const multiplayer_connection_id connection,
                           const multiplayer_transport_payload &bytes ) {
            multiplayer_protocol_envelope envelope;
            std::string error;
            REQUIRE( multiplayer_decode_protocol_envelope( bytes, envelope, error ) );
            switch( envelope.message_type ) {
                case multiplayer_protocol_message_type::client_hello:
                    handle_hello( connection, envelope );
                    break;
                case multiplayer_protocol_message_type::authenticate:
                    handle_authenticate( connection, envelope );
                    break;
                case multiplayer_protocol_message_type::resume_request:
                    handle_resume( connection, envelope );
                    break;
                case multiplayer_protocol_message_type::player_command:
                    handle_command( connection, envelope );
                    break;
                case multiplayer_protocol_message_type::ping:
                    handle_ping( connection, envelope );
                    break;
                case multiplayer_protocol_message_type::resync_request:
                    send_scene( connection, resumed_ ? resumed_session_ : first_session_ );
                    break;
                case multiplayer_protocol_message_type::disconnect_notice:
                    handle_disconnect_notice( connection, envelope );
                    break;
                default:
                    FAIL( "scripted server received an unexpected protocol message" );
            }
        }

        void handle_hello( const multiplayer_connection_id connection,
                           const multiplayer_protocol_envelope &envelope ) {
            if( behavior_ == scripted_server_behavior::drop_first_hello && !hello_dropped_ ) {
                hello_dropped_ = true;
                REQUIRE( transport_.disconnect( connection,
                                                "simulated pre-authentication disconnect" ) );
                return;
            }
            multiplayer_client_hello client_hello;
            std::string error;
            REQUIRE( multiplayer_parse_client_hello_payload( envelope, client_hello, error ) );
            REQUIRE( client_hello.build_id == test_build_id );
            REQUIRE( client_hello.content_manifest == test_manifest );

            multiplayer_server_hello hello;
            hello.accepted = true;
            hello.build_id = test_build_id;
            hello.world_id = "client-test-world";
            hello.content_manifest = test_manifest;
            if( behavior_ != scripted_server_behavior::accepted_without_semantic_scene ) {
                hello.capabilities = { { "semantic-scene", 1, true } };
            }
            hello.server_nonce[0] = 1;
            hello.savegame_version = 39;
            multiplayer_protocol_envelope response;
            response.message_type = multiplayer_protocol_message_type::server_hello;
            REQUIRE( multiplayer_build_server_hello_payload( hello, response.payload, error ) );
            send( connection, std::move( response ) );
        }

        void handle_authenticate( const multiplayer_connection_id connection,
                                  const multiplayer_protocol_envelope &envelope ) {
            multiplayer_authenticate_request request;
            std::string error;
            REQUIRE( multiplayer_parse_authenticate_payload( envelope, request, error ) );
            REQUIRE( request.bearer_token == std::string( 64, 'a' ) );
            multiplayer_authentication_result result;
            result.accepted = behavior_ != scripted_server_behavior::authentication_rejected;
            if( result.accepted ) {
                result.player_id = test_player_id;
                result.character_id = test_character_id;
                result.resume_token = test_resume_token;
                result.session_generation = 1;
            } else {
                result.rejection = multiplayer_protocol_rejection::authentication_failed;
                result.message = "scripted authentication rejection";
            }
            multiplayer_protocol_envelope response;
            response.message_type = multiplayer_protocol_message_type::authentication_result;
            response.session = result.accepted ? first_session_ : multiplayer_session_id{};
            response.sequence = 1;
            REQUIRE( multiplayer_build_authentication_result_payload( result, response.payload,
                     error ) );
            send( connection, std::move( response ) );
            send_scene( connection, first_session_ );
        }

        void handle_resume( const multiplayer_connection_id connection,
                            const multiplayer_protocol_envelope &envelope ) {
            multiplayer_resume_request request;
            std::string error;
            REQUIRE( multiplayer_parse_resume_request_payload( envelope, request, error ) );
            REQUIRE( request.resume_token == test_resume_token );
            resume_last_client_sequence_ = request.last_client_sequence;
            REQUIRE( request.last_client_sequence == 1 );
            multiplayer_resume_result result;
            result.accepted = behavior_ != scripted_server_behavior::resume_session_expired;
            if( result.accepted ) {
                result.player_id = test_player_id;
                result.character_id = test_character_id;
                result.session_generation =
                    behavior_ == scripted_server_behavior::resume_without_generation_advance ? 1 : 2;
                result.replay_from_sequence =
                    behavior_ == scripted_server_behavior::resume_with_wrong_replay_boundary ? 3 : 2;
                result.full_snapshot_required = true;
            } else {
                result.rejection = multiplayer_protocol_rejection::session_expired;
                result.message = "scripted resume session expired";
            }
            multiplayer_protocol_envelope response;
            response.message_type = multiplayer_protocol_message_type::resume_result;
            response.session = result.accepted ? resumed_session_ : multiplayer_session_id{};
            response.sequence = 1;
            REQUIRE( multiplayer_build_resume_result_payload( result, response.payload, error ) );
            send( connection, std::move( response ) );
            if( !result.accepted ) {
                return;
            }
            resumed_ = true;
            if( behavior_ == scripted_server_behavior::resume_scene_ahead_of_cached_result ) {
                revision_ = cached_command_revision_ + 1;
            }
            send_scene( connection, resumed_session_ );
        }

        void handle_command( const multiplayer_connection_id connection,
                             const multiplayer_protocol_envelope &envelope ) {
            multiplayer_player_command command;
            std::string error;
            REQUIRE( multiplayer_parse_player_command_payload( envelope, command, error ) );
            REQUIRE( command.client_sequence == 2 );
            REQUIRE( command.base_revision == 1 );
            if( !command_seen_ ) {
                command_seen_ = true;
                cached_command_payload_ = envelope.payload;
                revision_ = 2;
                cached_command_revision_ = revision_;
                if( behavior_ == scripted_server_behavior::drop_first_command ||
                    behavior_ == scripted_server_behavior::resume_without_generation_advance ||
                    behavior_ == scripted_server_behavior::resume_with_wrong_replay_boundary ||
                    behavior_ == scripted_server_behavior::resume_scene_ahead_of_cached_result ||
                    behavior_ == scripted_server_behavior::resume_session_expired ) {
                    REQUIRE( transport_.disconnect( connection,
                                                    "simulated lost command confirmation" ) );
                    return;
                }
                if( behavior_ == scripted_server_behavior::pong_then_drop_first_command ) {
                    return;
                }
            } else {
                REQUIRE( envelope.payload == cached_command_payload_ );
                duplicate_sent_ = true;
            }

            multiplayer_command_result result;
            result.client_sequence = command.client_sequence;
            result.status = duplicate_sent_ ? multiplayer_command_status::duplicate :
                            multiplayer_command_status::accepted;
            result.server_revision = duplicate_sent_ ? cached_command_revision_ : revision_;
            result.moves_spent = 100;
            multiplayer_protocol_envelope response;
            response.message_type = multiplayer_protocol_message_type::command_result;
            response.session = resumed_ ? resumed_session_ : first_session_;
            response.sequence = command.client_sequence;
            REQUIRE( multiplayer_build_command_result_payload( result, response.payload, error ) );
            send( connection, std::move( response ) );
            if( behavior_ != scripted_server_behavior::delay_scene_after_command_result ) {
                send_scene( connection, resumed_ ? resumed_session_ : first_session_ );
            }
        }

        void handle_ping( const multiplayer_connection_id connection,
                          const multiplayer_protocol_envelope &envelope ) {
            multiplayer_protocol_heartbeat ping;
            std::string error;
            REQUIRE( multiplayer_parse_ping_payload( envelope, ping, error ) );
            multiplayer_protocol_envelope response;
            response.message_type = multiplayer_protocol_message_type::pong;
            response.session = resumed_ ? resumed_session_ : first_session_;
            response.sequence = envelope.sequence;
            REQUIRE( multiplayer_build_pong_payload( ping, response.payload, error ) );
            send( connection, std::move( response ) );
            if( behavior_ == scripted_server_behavior::pong_then_drop_first_command &&
                command_seen_ && !resumed_ ) {
                REQUIRE( transport_.disconnect( connection,
                                                "simulated disconnect after later pong" ) );
            }
        }

        void handle_disconnect_notice( const multiplayer_connection_id connection,
                                       const multiplayer_protocol_envelope &envelope ) {
            multiplayer_disconnect_notice request;
            std::string error;
            REQUIRE( multiplayer_parse_disconnect_notice_payload( envelope, request, error ) );
            REQUIRE( request.code == multiplayer_protocol_rejection::none );
            graceful_disconnect_seen_ = true;
            multiplayer_protocol_envelope response;
            response.message_type = multiplayer_protocol_message_type::disconnect_notice;
            response.session = resumed_ ? resumed_session_ : first_session_;
            response.sequence = behavior_ ==
                                scripted_server_behavior::wrong_graceful_disconnect_sequence ?
                                envelope.sequence + 1 : envelope.sequence;
            REQUIRE( multiplayer_build_disconnect_notice_payload(
            { multiplayer_protocol_rejection::none, "scripted session released" },
            response.payload, error ) );
            send( connection, std::move( response ) );
            REQUIRE( transport_.disconnect( connection, "scripted graceful disconnect" ) );
        }

        multiplayer_server_transport transport_;
        multiplayer_connection_id connection_ = 0;
        multiplayer_session_id first_session_ = {};
        multiplayer_session_id resumed_session_ = {};
        multiplayer_transport_payload cached_command_payload_;
        std::uint64_t revision_ = 1;
        std::uint64_t cached_command_revision_ = 0;
        std::uint64_t resume_last_client_sequence_ = 0;
        std::uint64_t server_notice_sequence_ = 100;
        scripted_server_behavior behavior_ = scripted_server_behavior::normal;
        bool command_seen_ = false;
        bool duplicate_sent_ = false;
        bool resumed_ = false;
        bool hello_dropped_ = false;
        bool graceful_disconnect_seen_ = false;
};

multiplayer_client_settings client_settings( const std::uint16_t port )
{
    multiplayer_client_settings settings;
    settings.endpoint = { "127.0.0.1", port };
    settings.client_kind = multiplayer_protocol_client_kind::headless_test;
    settings.build_id = test_build_id;
    settings.content_manifest = test_manifest;
    settings.savegame_version = 39;
    settings.display_name = "Multiplayer Client Test";
    settings.bearer_token.assign( 64, 'a' );
    return settings;
}

bool pump_until( scripted_multiplayer_server &server, multiplayer_client &client,
                 const std::function<bool()> &predicate, std::string &error )
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( std::chrono::steady_clock::now() < deadline ) {
        server.poll();
        if( !client.poll_once( std::chrono::steady_clock::now(), error ) ) {
            return false;
        }
        server.poll();
        if( predicate() ) {
            return true;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    error = "timed out pumping scripted multiplayer client/server";
    return false;
}

bool pump_until_failure( scripted_multiplayer_server &server, multiplayer_client &client,
                         std::string &error )
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( std::chrono::steady_clock::now() < deadline ) {
        server.poll();
        if( !client.poll_once( std::chrono::steady_clock::now(), error ) ) {
            return true;
        }
        server.poll();
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    error = "client did not fail before the scripted test deadline";
    return false;
}

} // namespace

TEST_CASE( "multiplayer_client_authenticates_and_tracks_authoritative_scene",
           "[multiplayer][client]" )
{
    scripted_multiplayer_server server;
    multiplayer_client client( client_settings( server.port() ) );
    std::string error;
    REQUIRE( client.start( error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.ready();
    }, error ) );
    REQUIRE( client.latest_scene() );
    CHECK( client.latest_scene()->server_revision == 1 );
    CHECK( client.player_id() == test_player_id );
    CHECK( client.character_id() == test_character_id );

    // Locally rejected messages must not consume a protocol sequence and create a gap.
    std::uint64_t sequence = 0;
    CHECK_FALSE( client.send_command( multiplayer_command_kind::move,
                                      multiplayer_protocol_direction{ 2, 0, 0 }, sequence, error ) );
    CHECK_FALSE( client.request_resync( std::string( 2048, 'x' ), error ) );
    REQUIRE( client.send_command( multiplayer_command_kind::move,
                                  multiplayer_protocol_direction{ 1, 0, 0 }, sequence, error ) );
    CHECK( sequence == 2 );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.pending_command_count() == 0 && client.latest_scene() &&
               client.latest_scene()->server_revision == 2;
    }, error ) );
    CHECK( server.command_seen() );
    CHECK( client.last_confirmed_client_sequence() == 2 );

    REQUIRE( client.send_ping( 123, 456, error ) );
    bool saw_pong = false;
    REQUIRE( pump_until( server, client, [&client, &saw_pong]() {
        while( std::optional<multiplayer_client_event> event = client.poll_event() ) {
            saw_pong = saw_pong || event->type == multiplayer_client_event_type::pong;
        }
        return saw_pong;
    }, error ) );
    client.stop();
}

TEST_CASE( "multiplayer_client_retries_a_connection_that_failed_before_authentication",
           "[multiplayer][client]" )
{
    scripted_multiplayer_server server( scripted_server_behavior::drop_first_hello );
    multiplayer_client client( client_settings( server.port() ) );
    std::string error;
    REQUIRE( client.start( error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.stage() == multiplayer_client_stage::disconnected;
    }, error ) );

    REQUIRE( client.reconnect( error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.ready();
    }, error ) );
    CHECK( client.session_generation() == 1 );
    client.stop();
}

TEST_CASE( "multiplayer_client_waits_for_graceful_session_release",
           "[multiplayer][client]" )
{
    scripted_multiplayer_server server;
    multiplayer_client client( client_settings( server.port() ) );
    std::string error;
    REQUIRE( client.start( error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.ready();
    }, error ) );

    std::uint64_t command_sequence = 0;
    REQUIRE( client.send_command( multiplayer_command_kind::wait, std::nullopt,
                                  command_sequence, error ) );
    CHECK_FALSE( client.request_graceful_disconnect( error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.pending_command_count() == 0 && client.ready();
    }, error ) );
    REQUIRE( client.request_graceful_disconnect( error ) );
    CHECK( client.stage() == multiplayer_client_stage::disconnecting );
    bool acknowledged = false;
    REQUIRE( pump_until( server, client, [&client, &acknowledged]() {
        while( std::optional<multiplayer_client_event> event = client.poll_event() ) {
            acknowledged = acknowledged ||
                           event->type ==
                           multiplayer_client_event_type::gracefully_disconnected;
        }
        return acknowledged;
    }, error ) );
    CHECK( server.graceful_disconnect_seen() );
    CHECK( client.stage() == multiplayer_client_stage::disconnected );
    CHECK( client.player_id().empty() );
    CHECK( client.pending_command_count() == 0 );
    client.stop();
}

TEST_CASE( "multiplayer_client_rejects_a_mismatched_graceful_disconnect_acknowledgement",
           "[multiplayer][client]" )
{
    scripted_multiplayer_server server(
        scripted_server_behavior::wrong_graceful_disconnect_sequence );
    multiplayer_client client( client_settings( server.port() ) );
    std::string error;
    REQUIRE( client.start( error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.ready();
    }, error ) );
    REQUIRE( client.request_graceful_disconnect( error ) );
    REQUIRE( pump_until_failure( server, client, error ) );
    CHECK( error.find( "wrong sequence" ) != std::string::npos );
    CHECK( client.stage() == multiplayer_client_stage::failed );
    client.stop();
}

TEST_CASE( "multiplayer_client_resumes_and_replays_unconfirmed_command_once",
           "[multiplayer][client]" )
{
    scripted_multiplayer_server server( scripted_server_behavior::drop_first_command );
    multiplayer_client client( client_settings( server.port() ) );
    std::string error;
    REQUIRE( client.start( error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.ready();
    }, error ) );

    std::uint64_t sequence = 0;
    REQUIRE( client.send_command( multiplayer_command_kind::wait, std::nullopt, sequence, error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.stage() == multiplayer_client_stage::disconnected;
    }, error ) );
    CHECK( client.pending_command_count() == 1 );
    CHECK( client.last_confirmed_client_sequence() == 1 );

    REQUIRE( client.reconnect( error ) );
    REQUIRE( pump_until( server, client, [&client, &server]() {
        return client.ready() && client.pending_command_count() == 0 &&
               server.duplicate_sent();
    }, error ) );
    CHECK( server.resumed() );
    CHECK( client.session_generation() == 2 );
    CHECK( client.latest_scene()->server_revision == 2 );
    CHECK( client.last_confirmed_client_sequence() == 2 );
    client.stop();
}

TEST_CASE( "multiplayer_client_falls_back_to_fresh_auth_after_resume_expiry",
           "[multiplayer][client]" )
{
    scripted_multiplayer_server server( scripted_server_behavior::resume_session_expired );
    multiplayer_client client( client_settings( server.port() ) );
    std::string error;
    REQUIRE( client.start( error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.ready();
    }, error ) );

    std::uint64_t sequence = 0;
    REQUIRE( client.send_command( multiplayer_command_kind::wait, std::nullopt,
                                  sequence, error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.stage() == multiplayer_client_stage::disconnected;
    }, error ) );
    REQUIRE( client.reconnect( error ) );
    REQUIRE( pump_until_failure( server, client, error ) );
    CHECK( error.find( "session expired" ) != std::string::npos );

    REQUIRE( client.reconnect( error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.ready();
    }, error ) );
    CHECK( client.session_generation() == 1 );
    CHECK( client.pending_command_count() == 0 );
    client.stop();
}

TEST_CASE( "multiplayer_client_accepts_cached_result_older_than_resume_snapshot",
           "[multiplayer][client]" )
{
    scripted_multiplayer_server server(
        scripted_server_behavior::resume_scene_ahead_of_cached_result );
    multiplayer_client client( client_settings( server.port() ) );
    std::string error;
    REQUIRE( client.start( error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.ready();
    }, error ) );

    std::uint64_t sequence = 0;
    REQUIRE( client.send_command( multiplayer_command_kind::wait, std::nullopt,
                                  sequence, error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.stage() == multiplayer_client_stage::disconnected;
    }, error ) );
    REQUIRE( client.reconnect( error ) );
    REQUIRE( pump_until( server, client, [&client, &server]() {
        return client.ready() && client.pending_command_count() == 0 &&
               server.duplicate_sent();
    }, error ) );
    REQUIRE( client.latest_scene() );
    CHECK( client.latest_scene()->server_revision == 3 );
    CHECK( client.last_confirmed_client_sequence() == 2 );
    client.stop();
}

TEST_CASE( "multiplayer_client_waits_for_the_scene_revision_confirmed_by_a_result",
           "[multiplayer][client]" )
{
    scripted_multiplayer_server server(
        scripted_server_behavior::delay_scene_after_command_result );
    multiplayer_client client( client_settings( server.port() ) );
    std::string error;
    REQUIRE( client.start( error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.ready();
    }, error ) );

    std::uint64_t sequence = 0;
    REQUIRE( client.send_command( multiplayer_command_kind::wait, std::nullopt,
                                  sequence, error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.pending_command_count() == 0;
    }, error ) );
    REQUIRE( client.latest_scene() );
    CHECK( client.latest_scene()->server_revision == 1 );
    CHECK_FALSE( client.ready() );
    CHECK_FALSE( client.send_command( multiplayer_command_kind::wait, std::nullopt,
                                      sequence, error ) );

    server.send_scene_for_test( 2 );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.ready();
    }, error ) );
    CHECK( client.latest_scene()->server_revision == 2 );
    client.stop();
}

TEST_CASE( "multiplayer_client_heartbeat_survives_a_pending_scene_revision",
           "[multiplayer][client]" )
{
    scripted_multiplayer_server server(
        scripted_server_behavior::delay_scene_after_command_result );
    multiplayer_client client( client_settings( server.port() ) );
    std::string error;
    REQUIRE( client.start( error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.ready();
    }, error ) );

    std::uint64_t sequence = 0;
    REQUIRE( client.send_command( multiplayer_command_kind::wait, std::nullopt,
                                  sequence, error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.pending_command_count() == 0;
    }, error ) );
    CHECK( client.stage() == multiplayer_client_stage::ready );
    CHECK_FALSE( client.ready() );

    REQUIRE( client.send_ping( 1234, 5678, error ) );
    bool saw_pong = false;
    REQUIRE( pump_until( server, client, [&client, &saw_pong]() {
        while( std::optional<multiplayer_client_event> event = client.poll_event() ) {
            saw_pong = saw_pong || event->type == multiplayer_client_event_type::pong;
        }
        return saw_pong;
    }, error ) );
    CHECK_FALSE( client.ready() );

    REQUIRE( client.mark_transport_unresponsive(
                 "multiplayer server heartbeat timed out", error ) );
    CHECK( client.stage() == multiplayer_client_stage::disconnected );
    const std::optional<multiplayer_client_event> disconnected = client.poll_event();
    REQUIRE( disconnected );
    CHECK( disconnected->type == multiplayer_client_event_type::disconnected );
    CHECK( disconnected->message == "multiplayer server heartbeat timed out" );
    client.stop();
}

TEST_CASE( "multiplayer_client_resume_floor_does_not_skip_a_command_before_a_later_pong",
           "[multiplayer][client]" )
{
    scripted_multiplayer_server server(
        scripted_server_behavior::pong_then_drop_first_command );
    multiplayer_client client( client_settings( server.port() ) );
    std::string error;
    REQUIRE( client.start( error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.ready();
    }, error ) );

    std::uint64_t command_sequence = 0;
    REQUIRE( client.send_command( multiplayer_command_kind::wait, std::nullopt,
                                  command_sequence, error ) );
    REQUIRE( command_sequence == 2 );
    REQUIRE( client.send_ping( 777, 888, error ) );
    REQUIRE( pump_until( server, client, [&client]() {
        return client.stage() == multiplayer_client_stage::disconnected;
    }, error ) );
    CHECK( client.pending_command_count() == 1 );
    CHECK( client.last_confirmed_client_sequence() == 1 );

    REQUIRE( client.reconnect( error ) );
    REQUIRE( pump_until( server, client, [&client, &server]() {
        return client.ready() && client.pending_command_count() == 0 &&
               server.duplicate_sent();
    }, error ) );
    CHECK( server.resume_last_client_sequence() == 1 );
    CHECK( client.last_confirmed_client_sequence() == 3 );
    client.stop();
}

TEST_CASE( "multiplayer_client_rejects_incompatible_server_state_boundaries",
           "[multiplayer][client]" )
{
    SECTION( "typed authentication rejection without a session" ) {
        scripted_multiplayer_server server(
            scripted_server_behavior::authentication_rejected );
        multiplayer_client client( client_settings( server.port() ) );
        std::string error;
        REQUIRE( client.start( error ) );
        REQUIRE( pump_until_failure( server, client, error ) );
        CHECK( client.stage() == multiplayer_client_stage::failed );
        CHECK( error.find( "scripted authentication rejection" ) != std::string::npos );
    }

    SECTION( "accepted hello without required semantic scene capability" ) {
        scripted_multiplayer_server server(
            scripted_server_behavior::accepted_without_semantic_scene );
        multiplayer_client client( client_settings( server.port() ) );
        std::string error;
        REQUIRE( client.start( error ) );
        REQUIRE( pump_until_failure( server, client, error ) );
        CHECK( client.stage() == multiplayer_client_stage::failed );
        CHECK( error.find( "semantic-scene" ) != std::string::npos );
    }

    SECTION( "wrong application session" ) {
        scripted_multiplayer_server server;
        multiplayer_client client( client_settings( server.port() ) );
        std::string error;
        REQUIRE( client.start( error ) );
        REQUIRE( pump_until( server, client, [&client]() {
            return client.ready();
        }, error ) );
        server.send_scene_for_test( 1, true );
        REQUIRE( pump_until_failure( server, client, error ) );
        CHECK( error.find( "wrong session" ) != std::string::npos );
    }

    SECTION( "authoritative scene revision regression" ) {
        scripted_multiplayer_server server;
        multiplayer_client client( client_settings( server.port() ) );
        std::string error;
        REQUIRE( client.start( error ) );
        REQUIRE( pump_until( server, client, [&client]() {
            return client.ready();
        }, error ) );
        std::uint64_t sequence = 0;
        REQUIRE( client.send_command( multiplayer_command_kind::wait, std::nullopt,
                                      sequence, error ) );
        REQUIRE( pump_until( server, client, [&client]() {
            return client.latest_scene() && client.latest_scene()->server_revision == 2;
        }, error ) );
        server.send_scene_for_test( 1 );
        REQUIRE( pump_until_failure( server, client, error ) );
        CHECK( error.find( "revision" ) != std::string::npos );
        const std::string terminal_error = error;
        CHECK_FALSE( client.poll_once( std::chrono::steady_clock::now(), error ) );
        CHECK( error == terminal_error );
    }

    SECTION( "graceful disconnect notice" ) {
        scripted_multiplayer_server server;
        multiplayer_client client( client_settings( server.port() ) );
        std::string error;
        REQUIRE( client.start( error ) );
        REQUIRE( pump_until( server, client, [&client]() {
            return client.ready();
        }, error ) );
        server.send_disconnect_notice_for_test();
        REQUIRE( pump_until( server, client, [&client]() {
            return client.stage() == multiplayer_client_stage::disconnected;
        }, error ) );
    }
}

TEST_CASE( "multiplayer_client_rejects_invalid_resume_boundaries_and_event_overflow",
           "[multiplayer][client]" )
{
    for( const scripted_server_behavior behavior : {
             scripted_server_behavior::resume_without_generation_advance,
             scripted_server_behavior::resume_with_wrong_replay_boundary
         } ) {
        CAPTURE( behavior );
        scripted_multiplayer_server server( behavior );
        multiplayer_client client( client_settings( server.port() ) );
        std::string error;
        REQUIRE( client.start( error ) );
        REQUIRE( pump_until( server, client, [&client]() {
            return client.ready();
        }, error ) );
        std::uint64_t sequence = 0;
        REQUIRE( client.send_command( multiplayer_command_kind::wait, std::nullopt,
                                      sequence, error ) );
        REQUIRE( pump_until( server, client, [&client]() {
            return client.stage() == multiplayer_client_stage::disconnected;
        }, error ) );
        REQUIRE( client.reconnect( error ) );
        REQUIRE( pump_until_failure( server, client, error ) );
        CHECK( client.stage() == multiplayer_client_stage::failed );
    }

    SECTION( "bounded client event queue fails closed" ) {
        scripted_multiplayer_server server;
        multiplayer_client_settings settings = client_settings( server.port() );
        settings.maximum_events = 1;
        multiplayer_client client( std::move( settings ) );
        std::string error;
        REQUIRE( client.start( error ) );
        REQUIRE( pump_until_failure( server, client, error ) );
        CHECK( client.stage() == multiplayer_client_stage::failed );
        CHECK( error.find( "event queue" ) != std::string::npos );
        const std::optional<multiplayer_client_event> event = client.poll_event();
        REQUIRE( event );
        CHECK( event->type == multiplayer_client_event_type::error );
    }
}

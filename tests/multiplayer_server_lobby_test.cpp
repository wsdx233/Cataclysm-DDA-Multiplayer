#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cata_catch.h"
#include "multiplayer_protocol.h"
#include "multiplayer_server_lobby.h"
#include "multiplayer_session_directory.h"
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

multiplayer_transport_event closed_event(
    const multiplayer_connection_id connection,
    const std::string &detail = "closed",
    const multiplayer_transport_event_type type = multiplayer_transport_event_type::disconnected )
{
    return { type, connection, {}, detail, {} };
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

multiplayer_protocol_envelope resume_envelope( const multiplayer_resume_request &request )
{
    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::resume_request;
    envelope.sequence = 1;
    std::string error;
    REQUIRE( multiplayer_build_resume_request_payload( request, envelope.payload, error ) );
    return envelope;
}

multiplayer_protocol_envelope graceful_disconnect_envelope(
    const multiplayer_session_id &session, const std::uint64_t sequence )
{
    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::disconnect_notice;
    envelope.session = session;
    envelope.sequence = sequence;
    std::string error;
    REQUIRE( multiplayer_build_disconnect_notice_payload(
    { multiplayer_protocol_rejection::none, "client quit" },
    envelope.payload, error ) );
    return envelope;
}

multiplayer_protocol_envelope player_command_envelope(
    const multiplayer_session_id &session, const std::uint64_t sequence )
{
    multiplayer_player_command command;
    command.client_sequence = sequence;
    command.base_revision = 1;
    command.kind = multiplayer_command_kind::wait;
    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::player_command;
    envelope.session = session;
    envelope.sequence = sequence;
    std::string error;
    REQUIRE( multiplayer_build_player_command_payload( command, envelope.payload, error ) );
    return envelope;
}

multiplayer_protocol_envelope decode_action( const multiplayer_server_lobby_action &action )
{
    REQUIRE( ( action.type == multiplayer_server_lobby_action_type::send ||
               action.type == multiplayer_server_lobby_action_type::send_and_disconnect ) );
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

multiplayer_server_lobby_admission_decision accepted_decision(
    const multiplayer_server_lobby_event &pending, const std::uint64_t session_generation )
{
    multiplayer_server_lobby_admission_decision decision;
    decision.accepted = true;
    decision.player_id = pending.player_id;
    decision.character_id = pending.character_id;
    decision.session_generation = session_generation;
    decision.rejection = multiplayer_protocol_rejection::none;
    decision.message = "synthetic simulation admission";
    return decision;
}

struct completed_authentication {
    multiplayer_server_lobby_event pending;
    multiplayer_server_lobby_prepared_admission prepared;
    multiplayer_protocol_envelope response;
    multiplayer_authentication_result result;
    multiplayer_server_lobby_event committed;
};

completed_authentication complete_pending_authentication(
    multiplayer_server_lobby &lobby, const multiplayer_server_lobby_event &pending,
    const std::uint64_t session_generation )
{
    REQUIRE( pending.type == multiplayer_server_lobby_event_type::authentication_pending );
    REQUIRE( lobby.admission_is_pending( pending ) );
    completed_authentication completed;
    completed.pending = pending;
    std::string error;
    REQUIRE( lobby.prepare_admission( pending, accepted_decision( pending, session_generation ),
                                      completed.prepared, error ) );
    REQUIRE( completed.prepared.actions.size() == 1 );
    REQUIRE( lobby.publish_admission( completed.prepared ) );
    completed.response = decode_action( completed.prepared.actions.front() );
    REQUIRE( multiplayer_parse_authentication_result_payload(
                 completed.response, completed.result, error ) );
    REQUIRE( completed.result.accepted );
    CHECK( completed.result.player_id == pending.player_id );
    CHECK( completed.result.character_id == pending.character_id );
    CHECK( completed.result.session_generation == session_generation );
    CHECK( completed.response.session == pending.session );
    const std::optional<multiplayer_server_lobby_event> committed = lobby.poll_event();
    REQUIRE( committed );
    completed.committed = *committed;
    CHECK( completed.committed.type == multiplayer_server_lobby_event_type::authenticated );
    CHECK( completed.committed.player_id == pending.player_id );
    CHECK( completed.committed.character_id == pending.character_id );
    CHECK( completed.committed.session_generation == session_generation );
    return completed;
}

completed_authentication authenticate_successfully(
    multiplayer_server_lobby &lobby, const multiplayer_connection_id connection,
    const multiplayer_server_lobby::clock::time_point now,
    const std::uint64_t session_generation = 1 )
{
    const std::size_t authenticated_before = lobby.authenticated_player_count();
    const std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event(
                frame_event( connection, authenticate_envelope( std::string( 64, 'a' ) ) ), now );
    REQUIRE( actions.empty() );
    CHECK( lobby.authenticated_player_count() == authenticated_before );
    const std::optional<multiplayer_server_lobby_event> pending = lobby.poll_event();
    REQUIRE( pending );
    REQUIRE( pending->type == multiplayer_server_lobby_event_type::authentication_pending );
    completed_authentication completed = complete_pending_authentication(
            lobby, *pending, session_generation );
    CHECK( lobby.authenticated_player_count() == authenticated_before + 1 );
    return completed;
}

void confirm_fresh_authentication(
    multiplayer_server_lobby &lobby, const completed_authentication &authentication,
    const multiplayer_connection_id connection,
    const multiplayer_server_lobby::clock::time_point now )
{
    multiplayer_protocol_envelope ping;
    ping.message_type = multiplayer_protocol_message_type::ping;
    ping.session = authentication.response.session;
    ping.sequence = 2;
    std::string error;
    REQUIRE( multiplayer_build_ping_payload( { 2, 2 }, ping.payload, error ) );
    REQUIRE( lobby.handle_transport_event( frame_event( connection, ping ), now ).empty() );
    const std::optional<multiplayer_server_lobby_event> confirmation = lobby.poll_event();
    REQUIRE( confirmation );
    CHECK( confirmation->type == multiplayer_server_lobby_event_type::application_message );
    CHECK_FALSE( confirmation->confirms_resume_generation );
}

struct completed_resume {
    multiplayer_server_lobby_event pending;
    multiplayer_server_lobby_prepared_admission prepared;
    multiplayer_protocol_envelope response;
    multiplayer_resume_result result;
    multiplayer_server_lobby_event committed;
};

completed_resume complete_pending_resume(
    multiplayer_server_lobby &lobby, const multiplayer_server_lobby_event &pending,
    const std::uint64_t session_generation )
{
    REQUIRE( pending.type == multiplayer_server_lobby_event_type::resume_pending );
    REQUIRE( lobby.admission_is_pending( pending ) );
    completed_resume completed;
    completed.pending = pending;
    std::string error;
    REQUIRE( lobby.prepare_admission( pending, accepted_decision( pending, session_generation ),
                                      completed.prepared, error ) );
    REQUIRE( completed.prepared.actions.size() == 1 );
    REQUIRE( lobby.publish_admission( completed.prepared ) );
    completed.response = decode_action( completed.prepared.actions.front() );
    REQUIRE( multiplayer_parse_resume_result_payload( completed.response, completed.result, error ) );
    REQUIRE( completed.result.accepted );
    CHECK( completed.result.player_id == pending.player_id );
    CHECK( completed.result.character_id == pending.character_id );
    CHECK( completed.result.session_generation == session_generation );
    CHECK( completed.response.session == pending.session );
    const std::optional<multiplayer_server_lobby_event> committed = lobby.poll_event();
    REQUIRE( committed );
    completed.committed = *committed;
    CHECK( completed.committed.type == multiplayer_server_lobby_event_type::resumed );
    CHECK( completed.committed.player_id == pending.player_id );
    CHECK( completed.committed.character_id == pending.character_id );
    CHECK( completed.committed.session_generation == session_generation );
    return completed;
}

completed_resume resume_successfully(
    multiplayer_server_lobby &lobby, const multiplayer_connection_id connection,
    const multiplayer_resume_request &request,
    const multiplayer_server_lobby::clock::time_point now,
    const std::uint64_t session_generation )
{
    const std::size_t authenticated_before = lobby.authenticated_player_count();
    const std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event(
                frame_event( connection, resume_envelope( request ) ), now );
    REQUIRE( actions.empty() );
    CHECK( lobby.authenticated_player_count() == authenticated_before );
    const std::optional<multiplayer_server_lobby_event> pending = lobby.poll_event();
    REQUIRE( pending );
    REQUIRE( pending->type == multiplayer_server_lobby_event_type::resume_pending );
    CHECK( pending->expected_session_generation == request.session_generation );
    CHECK( pending->last_server_revision == request.last_server_revision );
    CHECK( pending->last_client_sequence == request.last_client_sequence );
    completed_resume completed = complete_pending_resume( lobby, *pending, session_generation );
    CHECK( lobby.authenticated_player_count() == authenticated_before + 1 );
    return completed;
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

    const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
    CHECK( authentication.response.session != multiplayer_session_id {} );
    const std::string resume_token = authentication.result.resume_token;
    CHECK( lobby.authenticated_player_count() == 1 );

    multiplayer_protocol_heartbeat ping;
    ping.nonce = 5;
    multiplayer_protocol_envelope ping_envelope;
    ping_envelope.message_type = multiplayer_protocol_message_type::ping;
    ping_envelope.session = authentication.response.session;
    ping_envelope.sequence = 2;
    REQUIRE( multiplayer_build_ping_payload( ping, ping_envelope.payload, error ) );
    CHECK( lobby.handle_transport_event( frame_event( 1, ping_envelope ), now ).empty() );
    std::optional<multiplayer_server_lobby_event> event = lobby.poll_event();
    REQUIRE( event );
    CHECK( event->type == multiplayer_server_lobby_event_type::application_message );
    CHECK( event->player_id == authentication.result.player_id );
    CHECK( event->character_id == authentication.result.character_id );
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
    resume.session_generation = authentication.result.session_generation;
    const completed_resume resumed = resume_successfully(
                                         lobby, 2, resume, now + std::chrono::seconds( 2 ), 2 );
    CHECK( resumed.result.replay_from_sequence == 8 );
    CHECK( resumed.result.player_id == authentication.result.player_id );
    CHECK( resumed.response.session != multiplayer_session_id {} );
    CHECK( resumed.committed.last_server_revision == 12 );
    CHECK( resumed.committed.last_client_sequence == 7 );

    const multiplayer_protocol_envelope future_command = player_command_envelope(
                resumed.response.session, 8 );
    CHECK( lobby.handle_transport_event( frame_event( 2, future_command ),
                                         now + std::chrono::seconds( 2 ) ).empty() );
    event = lobby.poll_event();
    REQUIRE( event );
    CHECK( event->type == multiplayer_server_lobby_event_type::application_message );
    CHECK( event->message.sequence == 8 );
}

TEST_CASE( "multiplayer_server_lobby_admission_waits_for_simulation_publish",
           "[multiplayer][server_lobby]" )
{
    const multiplayer_server_lobby::clock::time_point now{};

    SECTION( "pending authentication has no accepted state before publish" ) {
        multiplayer_server_lobby lobby( lobby_settings() );
        complete_hello( lobby, 1, now );

        const std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event(
                    frame_event( 1, authenticate_envelope( std::string( 64, 'a' ) ) ), now );
        CHECK( actions.empty() );
        CHECK( lobby.authenticated_player_count() == 0 );
        const std::optional<multiplayer_server_lobby_event> pending = lobby.poll_event();
        REQUIRE( pending );
        CHECK( pending->type == multiplayer_server_lobby_event_type::authentication_pending );
        CHECK( lobby.admission_is_pending( *pending ) );
        CHECK( lobby.authenticated_player_count() == 0 );

        multiplayer_server_lobby_admission_decision rejected;
        rejected.rejection = multiplayer_protocol_rejection::permission_denied;
        rejected.message = "simulation rejected admission";
        multiplayer_server_lobby_prepared_admission prepared;
        std::string error;
        REQUIRE( lobby.prepare_admission( *pending, rejected, prepared, error ) );
        REQUIRE( prepared.actions.size() == 1 );
        CHECK( prepared.actions.front().type ==
               multiplayer_server_lobby_action_type::send_and_disconnect );
        multiplayer_authentication_result result;
        REQUIRE( multiplayer_parse_authentication_result_payload(
                     decode_action( prepared.actions.front() ), result, error ) );
        CHECK_FALSE( result.accepted );
        CHECK( result.player_id.empty() );
        CHECK( result.character_id.empty() );
        CHECK( result.resume_token.empty() );
        CHECK( result.session_generation == 0 );
        CHECK( lobby.authenticated_player_count() == 0 );

        REQUIRE( lobby.publish_admission( prepared ) );
        CHECK( lobby.authenticated_player_count() == 0 );
        CHECK_FALSE( lobby.admission_is_pending( *pending ) );
        CHECK_FALSE( lobby.poll_event() );
        CHECK_FALSE( lobby.publish_admission( prepared ) );
    }

    SECTION( "published completion cannot be repeated or prepared from a stale request" ) {
        multiplayer_server_lobby lobby( lobby_settings() );
        complete_hello( lobby, 1, now );
        REQUIRE( lobby.handle_transport_event( frame_event(
                1, authenticate_envelope( std::string( 64, 'a' ) ) ), now ).empty() );
        const std::optional<multiplayer_server_lobby_event> pending = lobby.poll_event();
        REQUIRE( pending );
        const completed_authentication completed = complete_pending_authentication(
                    lobby, *pending, 1 );

        CHECK_FALSE( lobby.publish_admission( completed.prepared ) );
        multiplayer_server_lobby_prepared_admission stale;
        std::string error;
        CHECK_FALSE( lobby.prepare_admission(
                         completed.pending, accepted_decision( completed.pending, 1 ), stale, error ) );
        CHECK( error.find( "stale" ) != std::string::npos );
        multiplayer_server_lobby_event wrong_admission = completed.pending;
        ++wrong_admission.admission_id;
        CHECK_FALSE( lobby.admission_is_pending( wrong_admission ) );
    }

    SECTION( "peer close invalidates a prepared pending admission" ) {
        multiplayer_server_lobby lobby( lobby_settings() );
        complete_hello( lobby, 1, now );
        REQUIRE( lobby.handle_transport_event( frame_event(
                1, authenticate_envelope( std::string( 64, 'a' ) ) ), now ).empty() );
        const std::optional<multiplayer_server_lobby_event> pending = lobby.poll_event();
        REQUIRE( pending );
        multiplayer_server_lobby_prepared_admission prepared;
        std::string error;
        REQUIRE( lobby.prepare_admission(
                     *pending, accepted_decision( *pending, 1 ), prepared, error ) );

        CHECK( lobby.handle_transport_event( closed_event( 1 ), now ).empty() );
        CHECK_FALSE( lobby.admission_is_pending( *pending ) );
        CHECK_FALSE( lobby.publish_admission( prepared ) );
        CHECK( lobby.authenticated_player_count() == 0 );
        CHECK_FALSE( lobby.poll_event() );
    }

    SECTION( "handshake timeout invalidates a pending admission" ) {
        multiplayer_server_lobby_settings settings = lobby_settings();
        settings.handshake_timeout = std::chrono::milliseconds( 50 );
        multiplayer_server_lobby lobby( settings );
        complete_hello( lobby, 1, now );
        REQUIRE( lobby.handle_transport_event( frame_event(
                1, authenticate_envelope( std::string( 64, 'a' ) ) ), now ).empty() );
        const std::optional<multiplayer_server_lobby_event> pending = lobby.poll_event();
        REQUIRE( pending );
        multiplayer_server_lobby_prepared_admission prepared;
        std::string error;
        REQUIRE( lobby.prepare_admission(
                     *pending, accepted_decision( *pending, 1 ), prepared, error ) );

        const std::vector<multiplayer_server_lobby_action> timeout =
            lobby.tick( now + std::chrono::milliseconds( 50 ) );
        REQUIRE( timeout.size() == 1 );
        CHECK( timeout.front().type == multiplayer_server_lobby_action_type::disconnect );
        CHECK_FALSE( lobby.admission_is_pending( *pending ) );
        CHECK_FALSE( lobby.publish_admission( prepared ) );
        CHECK( lobby.authenticated_player_count() == 0 );
    }

    SECTION( "maximum one reserves capacity for the first pending fresh authentication" ) {
        multiplayer_server_lobby_settings settings = lobby_settings();
        settings.maximum_players = 1;
        multiplayer_server_lobby lobby( settings );
        complete_hello( lobby, 1, now );
        complete_hello( lobby, 2, now );

        CHECK( lobby.handle_transport_event( frame_event(
                1, authenticate_envelope( std::string( 64, 'a' ) ) ), now ).empty() );
        CHECK( lobby.authenticated_player_count() == 0 );
        const std::vector<multiplayer_server_lobby_action> second = lobby.handle_transport_event(
                    frame_event( 2, authenticate_envelope( std::string( 64, 'a' ) ) ), now );
        REQUIRE( second.size() == 1 );
        multiplayer_authentication_result rejected;
        std::string error;
        REQUIRE( multiplayer_parse_authentication_result_payload(
                     decode_action( second.front() ), rejected, error ) );
        CHECK_FALSE( rejected.accepted );
        CHECK( rejected.rejection == multiplayer_protocol_rejection::server_full );
        CHECK( lobby.authenticated_player_count() == 0 );

        const std::optional<multiplayer_server_lobby_event> pending = lobby.poll_event();
        REQUIRE( pending );
        const completed_authentication accepted = complete_pending_authentication(
                    lobby, *pending, 1 );
        CHECK( accepted.result.accepted );
        CHECK( lobby.authenticated_player_count() == 1 );
    }

    SECTION( "an unconfirmed fresh result does not strand an unknown resume token" ) {
        multiplayer_server_lobby_settings settings = lobby_settings();
        settings.maximum_players = 1;
        multiplayer_server_lobby lobby( settings );
        complete_hello( lobby, 1, now );
        const completed_authentication first = authenticate_successfully( lobby, 1, now );
        CHECK( first.result.accepted );

        lobby.handle_transport_event( closed_event( 1 ), now );
        const std::optional<multiplayer_server_lobby_event> disconnected = lobby.poll_event();
        REQUIRE( disconnected );
        CHECK( disconnected->type == multiplayer_server_lobby_event_type::disconnected );
        CHECK( lobby.authenticated_player_count() == 0 );

        complete_hello( lobby, 2, now );
        const completed_authentication replacement = authenticate_successfully(
                    lobby, 2, now, first.result.session_generation + 1 );
        CHECK( replacement.result.accepted );
    }
}

TEST_CASE( "multiplayer_server_lobby_resume_pending_and_committed_replay_are_single_owner",
           "[multiplayer][server_lobby]" )
{
    const multiplayer_server_lobby::clock::time_point now{};

    SECTION( "one token permits only one pending resume" ) {
        multiplayer_server_lobby lobby( lobby_settings() );
        complete_hello( lobby, 1, now );
        const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
        confirm_fresh_authentication( lobby, authentication, 1, now );
        lobby.handle_transport_event( closed_event( 1 ), now );
        REQUIRE( lobby.poll_event() );
        complete_hello( lobby, 2, now );
        complete_hello( lobby, 3, now );
        multiplayer_resume_request request;
        request.resume_token = authentication.result.resume_token;
        request.last_server_revision = 12;
        request.last_client_sequence = 7;
        request.session_generation = authentication.result.session_generation;

        CHECK( lobby.handle_transport_event(
                   frame_event( 2, resume_envelope( request ) ), now ).empty() );
        CHECK( lobby.authenticated_player_count() == 0 );
        const std::vector<multiplayer_server_lobby_action> competing =
            lobby.handle_transport_event( frame_event( 3, resume_envelope( request ) ), now );
        REQUIRE( competing.size() == 1 );
        multiplayer_resume_result rejected;
        std::string error;
        REQUIRE( multiplayer_parse_resume_result_payload(
                     decode_action( competing.front() ), rejected, error ) );
        CHECK_FALSE( rejected.accepted );
        CHECK( rejected.rejection == multiplayer_protocol_rejection::invalid_state );

        const std::optional<multiplayer_server_lobby_event> pending = lobby.poll_event();
        REQUIRE( pending );
        const completed_resume resumed = complete_pending_resume( lobby, *pending, 2 );
        CHECK( resumed.result.session_generation == 2 );
        CHECK( lobby.authenticated_player_count() == 1 );
    }

    SECTION( "a lost accepted response replays the same generation only for the same fingerprint" ) {
        multiplayer_server_lobby lobby( lobby_settings() );
        complete_hello( lobby, 1, now );
        const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
        confirm_fresh_authentication( lobby, authentication, 1, now );
        lobby.handle_transport_event( closed_event( 1 ), now );
        REQUIRE( lobby.poll_event() );

        multiplayer_resume_request request;
        request.resume_token = authentication.result.resume_token;
        request.last_server_revision = 12;
        request.last_client_sequence = 7;
        request.session_generation = authentication.result.session_generation;
        complete_hello( lobby, 2, now );
        const completed_resume lost_response = resume_successfully( lobby, 2, request, now, 2 );
        CHECK( lost_response.result.session_generation == 2 );
        lobby.handle_transport_event( closed_event( 2 ), now );
        REQUIRE( lobby.poll_event() );

        complete_hello( lobby, 3, now );
        const completed_resume replayed = resume_successfully( lobby, 3, request, now, 2 );
        CHECK( replayed.result.session_generation == lost_response.result.session_generation );
        CHECK( replayed.response.session != lost_response.response.session );
        lobby.handle_transport_event( closed_event( 3 ), now );
        REQUIRE( lobby.poll_event() );

        complete_hello( lobby, 4, now );
        multiplayer_resume_request conflict = request;
        ++conflict.last_server_revision;
        const std::vector<multiplayer_server_lobby_action> rejected_actions =
            lobby.handle_transport_event( frame_event( 4, resume_envelope( conflict ) ), now );
        REQUIRE( rejected_actions.size() == 1 );
        multiplayer_resume_result rejected;
        std::string error;
        REQUIRE( multiplayer_parse_resume_result_payload(
                     decode_action( rejected_actions.front() ), rejected, error ) );
        CHECK_FALSE( rejected.accepted );
        CHECK( rejected.rejection == multiplayer_protocol_rejection::session_expired );
        CHECK( rejected.message.find( "does not match" ) != std::string::npos );
        CHECK_FALSE( lobby.poll_event() );
    }

    SECTION( "an application frame consumes the old-generation resume replay" ) {
        multiplayer_server_lobby lobby( lobby_settings() );
        complete_hello( lobby, 1, now );
        const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
        confirm_fresh_authentication( lobby, authentication, 1, now );
        lobby.handle_transport_event( closed_event( 1 ), now );
        REQUIRE( lobby.poll_event() );

        multiplayer_resume_request request;
        request.resume_token = authentication.result.resume_token;
        request.last_server_revision = 12;
        request.last_client_sequence = 7;
        request.session_generation = authentication.result.session_generation;
        complete_hello( lobby, 2, now );
        const completed_resume resumed = resume_successfully( lobby, 2, request, now, 2 );

        multiplayer_protocol_envelope ping;
        ping.message_type = multiplayer_protocol_message_type::ping;
        ping.session = resumed.response.session;
        ping.sequence = 8;
        std::string error;
        REQUIRE( multiplayer_build_ping_payload( { 8, 8 }, ping.payload, error ) );
        CHECK( lobby.handle_transport_event( frame_event( 2, ping ), now ).empty() );
        const std::optional<multiplayer_server_lobby_event> confirmation = lobby.poll_event();
        REQUIRE( confirmation );
        CHECK( confirmation->type ==
               multiplayer_server_lobby_event_type::application_message );
        CHECK( confirmation->confirms_resume_generation );
        REQUIRE( lobby.record_session_confirmed( *confirmation ) );
        CHECK_FALSE( lobby.record_session_confirmed( *confirmation ) );

        lobby.handle_transport_event( closed_event( 2 ), now );
        REQUIRE( lobby.poll_event() );
        complete_hello( lobby, 3, now );
        const std::vector<multiplayer_server_lobby_action> rejected_actions =
            lobby.handle_transport_event( frame_event( 3, resume_envelope( request ) ), now );
        REQUIRE( rejected_actions.size() == 1 );
        multiplayer_resume_result rejected;
        REQUIRE( multiplayer_parse_resume_result_payload(
                     decode_action( rejected_actions.front() ), rejected, error ) );
        CHECK_FALSE( rejected.accepted );
        CHECK( rejected.rejection == multiplayer_protocol_rejection::session_expired );
        CHECK( rejected.message.find( "stale" ) != std::string::npos );
        CHECK_FALSE( lobby.poll_event() );
    }

    SECTION( "a downstream queue rejection retains the resume replay until acknowledgement" ) {
        multiplayer_server_lobby lobby( lobby_settings() );
        complete_hello( lobby, 1, now );
        const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
        confirm_fresh_authentication( lobby, authentication, 1, now );
        lobby.handle_transport_event( closed_event( 1 ), now );
        REQUIRE( lobby.poll_event() );

        multiplayer_resume_request request;
        request.resume_token = authentication.result.resume_token;
        request.last_server_revision = 12;
        request.last_client_sequence = 7;
        request.session_generation = authentication.result.session_generation;
        complete_hello( lobby, 2, now );
        const completed_resume resumed = resume_successfully( lobby, 2, request, now, 2 );

        multiplayer_protocol_envelope ping;
        ping.message_type = multiplayer_protocol_message_type::ping;
        ping.session = resumed.response.session;
        ping.sequence = 8;
        std::string error;
        REQUIRE( multiplayer_build_ping_payload( { 8, 8 }, ping.payload, error ) );
        CHECK( lobby.handle_transport_event( frame_event( 2, ping ), now ).empty() );
        const std::optional<multiplayer_server_lobby_event> unacknowledged = lobby.poll_event();
        REQUIRE( unacknowledged );
        CHECK( unacknowledged->confirms_resume_generation );

        // Simulate the dedicated server rejecting this event because its downstream simulation
        // queue is full: dequeue alone is not an acknowledgement.
        lobby.handle_transport_event( closed_event( 2 ), now );
        REQUIRE( lobby.poll_event() );
        complete_hello( lobby, 3, now );
        const completed_resume replayed = resume_successfully( lobby, 3, request, now, 2 );
        CHECK( replayed.result.session_generation == resumed.result.session_generation );
    }

    SECTION( "a terminal stale resume revokes capacity for a fresh authentication" ) {
        multiplayer_server_lobby_settings settings = lobby_settings();
        settings.maximum_players = 1;
        multiplayer_server_lobby lobby( settings );
        complete_hello( lobby, 1, now );
        const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
        confirm_fresh_authentication( lobby, authentication, 1, now );
        lobby.handle_transport_event( closed_event( 1 ), now );
        REQUIRE( lobby.poll_event() );

        complete_hello( lobby, 2, now );
        multiplayer_resume_request stale;
        stale.resume_token = authentication.result.resume_token;
        stale.last_server_revision = 12;
        stale.last_client_sequence = 7;
        stale.session_generation = authentication.result.session_generation + 1;
        const std::vector<multiplayer_server_lobby_action> rejected_actions =
            lobby.handle_transport_event( frame_event( 2, resume_envelope( stale ) ), now );
        REQUIRE( rejected_actions.size() == 1 );
        multiplayer_resume_result rejected;
        std::string error;
        REQUIRE( multiplayer_parse_resume_result_payload(
                     decode_action( rejected_actions.front() ), rejected, error ) );
        CHECK_FALSE( rejected.accepted );
        CHECK( rejected.rejection == multiplayer_protocol_rejection::session_expired );
        lobby.handle_transport_event( closed_event( 2 ), now );

        complete_hello( lobby, 3, now );
        const completed_authentication replacement = authenticate_successfully(
                    lobby, 3, now, authentication.result.session_generation + 1 );
        CHECK( replacement.result.accepted );
    }

    SECTION( "a simulation session-expired rejection also revokes resume capacity" ) {
        multiplayer_server_lobby_settings settings = lobby_settings();
        settings.maximum_players = 1;
        multiplayer_server_lobby lobby( settings );
        complete_hello( lobby, 1, now );
        const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
        confirm_fresh_authentication( lobby, authentication, 1, now );
        lobby.handle_transport_event( closed_event( 1 ), now );
        REQUIRE( lobby.poll_event() );

        complete_hello( lobby, 2, now );
        multiplayer_resume_request request;
        request.resume_token = authentication.result.resume_token;
        request.last_server_revision = 12;
        request.last_client_sequence = 7;
        request.session_generation = authentication.result.session_generation;
        REQUIRE( lobby.handle_transport_event(
                     frame_event( 2, resume_envelope( request ) ), now ).empty() );
        const std::optional<multiplayer_server_lobby_event> pending = lobby.poll_event();
        REQUIRE( pending );
        REQUIRE( pending->type == multiplayer_server_lobby_event_type::resume_pending );

        multiplayer_server_lobby_admission_decision expired;
        expired.rejection = multiplayer_protocol_rejection::session_expired;
        expired.message = "authoritative session no longer exists";
        multiplayer_server_lobby_prepared_admission prepared;
        std::string error;
        REQUIRE( lobby.prepare_admission( *pending, expired, prepared, error ) );
        REQUIRE( prepared.actions.size() == 1 );
        CHECK( prepared.actions.front().type ==
               multiplayer_server_lobby_action_type::send_and_disconnect );
        REQUIRE( lobby.publish_admission( prepared ) );
        lobby.handle_transport_event( closed_event( 2 ), now );

        complete_hello( lobby, 3, now );
        const completed_authentication replacement = authenticate_successfully(
                    lobby, 3, now, authentication.result.session_generation + 1 );
        CHECK( replacement.result.accepted );
    }

    SECTION( "an authoritative already-connected rejection preserves resume capacity" ) {
        multiplayer_server_lobby_settings settings = lobby_settings();
        settings.maximum_players = 1;
        multiplayer_server_lobby lobby( settings );
        complete_hello( lobby, 1, now );
        const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
        confirm_fresh_authentication( lobby, authentication, 1, now );
        lobby.handle_transport_event( closed_event( 1 ), now );
        REQUIRE( lobby.poll_event() );

        complete_hello( lobby, 2, now );
        multiplayer_resume_request request;
        request.resume_token = authentication.result.resume_token;
        request.last_server_revision = 12;
        request.last_client_sequence = 7;
        request.session_generation = authentication.result.session_generation;
        REQUIRE( lobby.handle_transport_event(
                     frame_event( 2, resume_envelope( request ) ), now ).empty() );
        const std::optional<multiplayer_server_lobby_event> pending = lobby.poll_event();
        REQUIRE( pending );
        REQUIRE( pending->type == multiplayer_server_lobby_event_type::resume_pending );

        multiplayer_server_lobby_admission_decision connected;
        connected.rejection = multiplayer_session_admission_rejection(
                                  multiplayer_session_admission_kind::resume,
                                  multiplayer_session_directory_status::already_connected );
        connected.message = multiplayer_session_directory_status_message(
                                multiplayer_session_directory_status::already_connected );
        CHECK( connected.rejection == multiplayer_protocol_rejection::invalid_state );
        multiplayer_server_lobby_prepared_admission prepared;
        std::string error;
        REQUIRE( lobby.prepare_admission( *pending, connected, prepared, error ) );
        REQUIRE( prepared.actions.size() == 1 );
        CHECK( prepared.actions.front().type ==
               multiplayer_server_lobby_action_type::send_and_disconnect );
        multiplayer_resume_result rejected;
        REQUIRE( multiplayer_parse_resume_result_payload(
                     decode_action( prepared.actions.front() ), rejected, error ) );
        CHECK_FALSE( rejected.accepted );
        CHECK( rejected.rejection == multiplayer_protocol_rejection::invalid_state );
        REQUIRE( lobby.publish_admission( prepared ) );
        lobby.handle_transport_event( closed_event( 2 ), now );

        complete_hello( lobby, 3, now );
        const completed_resume resumed = resume_successfully(
                                             lobby, 3, request, now,
                                             authentication.result.session_generation + 1 );
        CHECK( resumed.result.accepted );
    }
}

TEST_CASE( "multiplayer_server_lobby_graceful_disconnect_releases_resume_capacity",
           "[multiplayer][server_lobby]" )
{
    multiplayer_server_lobby_settings settings = lobby_settings();
    settings.maximum_players = 1;
    multiplayer_server_lobby lobby( settings );
    const multiplayer_server_lobby::clock::time_point now{};
    complete_hello( lobby, 1, now );
    const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
    const multiplayer_protocol_envelope &authentication_response = authentication.response;

    std::string error;
    std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event( frame_event(
                1, graceful_disconnect_envelope( authentication_response.session, 2 ) ), now );
    CHECK( actions.empty() );
    const std::optional<multiplayer_server_lobby_event> request = lobby.poll_event();
    REQUIRE( request );
    CHECK( request->type ==
           multiplayer_server_lobby_event_type::graceful_disconnect_requested );
    CHECK( request->message.sequence == 2 );
    CHECK( lobby.authenticated_player_count() == 1 );

    complete_hello( lobby, 2, now + std::chrono::milliseconds( 1 ) );
    actions = lobby.handle_transport_event(
                  frame_event( 2, authenticate_envelope( std::string( 64, 'a' ) ) ),
                  now + std::chrono::milliseconds( 1 ) );
    REQUIRE( actions.size() == 1 );
    multiplayer_authentication_result rejected;
    REQUIRE( multiplayer_parse_authentication_result_payload(
                 decode_action( actions.front() ), rejected, error ) );
    CHECK( rejected.rejection == multiplayer_protocol_rejection::server_full );
    lobby.handle_transport_event( closed_event( 2 ), now + std::chrono::milliseconds( 1 ) );

    actions = lobby.complete_graceful_disconnect( *request );
    REQUIRE( actions.size() == 1 );
    CHECK( actions.front().type ==
           multiplayer_server_lobby_action_type::send_and_disconnect );
    CHECK( actions.front().reason == multiplayer_graceful_release_transport_reason );
    const multiplayer_protocol_envelope acknowledgement_envelope = decode_action( actions.front() );
    multiplayer_disconnect_notice acknowledgement;
    REQUIRE( multiplayer_parse_disconnect_notice_payload(
                 acknowledgement_envelope, acknowledgement, error ) );
    CHECK( acknowledgement.code == multiplayer_protocol_rejection::none );
    CHECK( acknowledgement_envelope.session == authentication_response.session );
    CHECK( acknowledgement_envelope.sequence == 2 );
    CHECK( lobby.authenticated_player_count() == 1 );
    CHECK( lobby.complete_graceful_disconnect( *request ).empty() );

    multiplayer_protocol_envelope late_ping;
    late_ping.message_type = multiplayer_protocol_message_type::ping;
    late_ping.session = authentication_response.session;
    late_ping.sequence = 3;
    REQUIRE( multiplayer_build_ping_payload( { 3, 3 }, late_ping.payload, error ) );
    CHECK( lobby.handle_transport_event( frame_event( 1, late_ping ), now ).empty() );

    lobby.handle_transport_event( closed_event(
                                      1, multiplayer_graceful_release_transport_reason ), now );
    const std::optional<multiplayer_server_lobby_event> disconnected = lobby.poll_event();
    REQUIRE( disconnected );
    CHECK( disconnected->type == multiplayer_server_lobby_event_type::disconnected );
    CHECK( lobby.authenticated_player_count() == 0 );
    complete_hello( lobby, 3, now + std::chrono::milliseconds( 2 ) );
    const completed_authentication accepted = authenticate_successfully(
                lobby, 3, now + std::chrono::milliseconds( 2 ), 2 );
    CHECK( accepted.result.accepted );
}

TEST_CASE( "multiplayer_server_lobby_reserves_control_capacity_for_graceful_disconnect",
           "[multiplayer][server_lobby]" )
{
    multiplayer_server_lobby_settings settings = lobby_settings();
    settings.maximum_players = 1;
    settings.maximum_pending_events = 5;
    multiplayer_server_lobby lobby( settings );
    const multiplayer_server_lobby::clock::time_point now{};
    complete_hello( lobby, 1, now );
    const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
    const multiplayer_protocol_envelope &authentication_response = authentication.response;

    std::string error;
    for( std::uint64_t sequence = 2; sequence <= 4; ++sequence ) {
        multiplayer_protocol_envelope ping;
        ping.message_type = multiplayer_protocol_message_type::ping;
        ping.session = authentication_response.session;
        ping.sequence = sequence;
        REQUIRE( multiplayer_build_ping_payload( { sequence, sequence }, ping.payload, error ) );
        CHECK( lobby.handle_transport_event( frame_event( 1, ping ), now ).empty() );
    }

    std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event( frame_event(
                1, graceful_disconnect_envelope( authentication_response.session, 5 ) ), now );
    CHECK( actions.empty() );
    for( int index = 0; index < 3; ++index ) {
        const std::optional<multiplayer_server_lobby_event> application = lobby.poll_event();
        REQUIRE( application );
        CHECK( application->type == multiplayer_server_lobby_event_type::application_message );
    }
    const std::optional<multiplayer_server_lobby_event> disconnect_request = lobby.poll_event();
    REQUIRE( disconnect_request );
    CHECK( disconnect_request->type ==
           multiplayer_server_lobby_event_type::graceful_disconnect_requested );
    CHECK( lobby.authenticated_player_count() == 1 );
    actions = lobby.complete_graceful_disconnect( *disconnect_request );
    REQUIRE( actions.size() == 1 );
    CHECK( actions.front().type ==
           multiplayer_server_lobby_action_type::send_and_disconnect );
    CHECK( lobby.authenticated_player_count() == 1 );
}

TEST_CASE( "multiplayer_server_lobby_graceful_disconnect_drains_or_resumes_safely",
           "[multiplayer][server_lobby]" )
{
    multiplayer_server_lobby_settings settings = lobby_settings();
    settings.maximum_players = 1;
    multiplayer_server_lobby lobby( settings );
    const multiplayer_server_lobby::clock::time_point now{};
    complete_hello( lobby, 1, now );
    const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
    const multiplayer_protocol_envelope &authentication_response = authentication.response;
    std::string error;

    std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event( frame_event(
                1, graceful_disconnect_envelope( authentication_response.session, 2 ) ), now );
    CHECK( actions.empty() );
    const std::optional<multiplayer_server_lobby_event> request = lobby.poll_event();
    REQUIRE( request );
    REQUIRE( request->type ==
             multiplayer_server_lobby_event_type::graceful_disconnect_requested );

    SECTION( "peer close before completion keeps the resume record" ) {
        lobby.handle_transport_event( closed_event( 1 ), now + std::chrono::milliseconds( 1 ) );
        CHECK( lobby.complete_graceful_disconnect( *request ).empty() );
        const std::optional<multiplayer_server_lobby_event> disconnected = lobby.poll_event();
        REQUIRE( disconnected );
        CHECK( disconnected->type == multiplayer_server_lobby_event_type::disconnected );

        complete_hello( lobby, 2, now + std::chrono::milliseconds( 2 ) );
        multiplayer_resume_request resume;
        resume.resume_token = authentication.result.resume_token;
        resume.last_client_sequence = 2;
        resume.session_generation = authentication.result.session_generation;
        const completed_resume resumed = resume_successfully(
                                             lobby, 2, resume,
                                             now + std::chrono::milliseconds( 2 ), 2 );
        CHECK( resumed.result.accepted );
    }

    SECTION( "later frames are rejected while the connection drains" ) {
        multiplayer_protocol_envelope ping;
        ping.message_type = multiplayer_protocol_message_type::ping;
        ping.session = authentication_response.session;
        ping.sequence = 3;
        REQUIRE( multiplayer_build_ping_payload( { 3, 3 }, ping.payload, error ) );
        actions = lobby.handle_transport_event( frame_event( 1, ping ), now );
        REQUIRE( actions.size() == 1 );
        CHECK( actions.front().type == multiplayer_server_lobby_action_type::disconnect );
        CHECK( actions.front().reason.find( "after requesting" ) != std::string::npos );
        CHECK( lobby.complete_graceful_disconnect( *request ).empty() );
        CHECK( lobby.authenticated_player_count() == 1 );
    }

    SECTION( "transport failure after scheduling keeps the resume record" ) {
        actions = lobby.complete_graceful_disconnect( *request );
        REQUIRE( actions.size() == 1 );
        REQUIRE( actions.front().type ==
                 multiplayer_server_lobby_action_type::send_and_disconnect );
        lobby.handle_transport_event( closed_event(
                                          1, "outbound per-connection queue is full",
                                          multiplayer_transport_event_type::transport_error ),
                                      now + std::chrono::milliseconds( 1 ) );
        const std::optional<multiplayer_server_lobby_event> disconnected = lobby.poll_event();
        REQUIRE( disconnected );
        CHECK( disconnected->type == multiplayer_server_lobby_event_type::disconnected );

        complete_hello( lobby, 2, now + std::chrono::milliseconds( 2 ) );
        multiplayer_resume_request resume;
        resume.resume_token = authentication.result.resume_token;
        resume.last_client_sequence = 2;
        resume.session_generation = authentication.result.session_generation;
        const completed_resume resumed = resume_successfully(
                                             lobby, 2, resume,
                                             now + std::chrono::milliseconds( 2 ), 2 );
        CHECK( resumed.result.accepted );
    }
}

TEST_CASE( "multiplayer_server_lobby_resume_replay_window_preserves_sequence_types",
           "[multiplayer][server_lobby]" )
{
    const multiplayer_server_lobby::clock::time_point now{};

    SECTION( "an observed command may be replayed before simulation caches it" ) {
        multiplayer_server_lobby lobby( lobby_settings() );
        complete_hello( lobby, 1, now );
        const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
        multiplayer_protocol_envelope response = authentication.response;

        CHECK( lobby.handle_transport_event( frame_event(
                1, player_command_envelope( response.session, 2 ) ), now ).empty() );
        REQUIRE( lobby.poll_event() );
        lobby.handle_transport_event( closed_event( 1 ), now );
        REQUIRE( lobby.poll_event() );

        complete_hello( lobby, 2, now );
        multiplayer_resume_request resume;
        resume.resume_token = authentication.result.resume_token;
        resume.last_client_sequence = 1;
        resume.session_generation = authentication.result.session_generation;
        const completed_resume resumed = resume_successfully( lobby, 2, resume, now, 2 );
        response = resumed.response;

        CHECK( lobby.handle_transport_event( frame_event(
                2, player_command_envelope( response.session, 2 ) ), now ).empty() );
        const std::optional<multiplayer_server_lobby_event> replay = lobby.poll_event();
        REQUIRE( replay );
        CHECK( replay->message.message_type ==
               multiplayer_protocol_message_type::player_command );
        CHECK( replay->message.sequence == 2 );
    }

    SECTION( "a prior ping sequence cannot be reused as a player command" ) {
        multiplayer_server_lobby lobby( lobby_settings() );
        complete_hello( lobby, 1, now );
        const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
        multiplayer_protocol_envelope response = authentication.response;
        std::string error;
        multiplayer_protocol_envelope ping;
        ping.message_type = multiplayer_protocol_message_type::ping;
        ping.session = response.session;
        ping.sequence = 2;
        REQUIRE( multiplayer_build_ping_payload( { 2, 2 }, ping.payload, error ) );
        CHECK( lobby.handle_transport_event( frame_event( 1, ping ), now ).empty() );
        REQUIRE( lobby.poll_event() );
        lobby.handle_transport_event( closed_event( 1 ), now );
        REQUIRE( lobby.poll_event() );

        complete_hello( lobby, 2, now );
        multiplayer_resume_request resume;
        resume.resume_token = authentication.result.resume_token;
        resume.last_client_sequence = 1;
        resume.session_generation = authentication.result.session_generation;
        const completed_resume resumed = resume_successfully( lobby, 2, resume, now, 2 );
        response = resumed.response;

        const std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event(
                    frame_event(
                        2, player_command_envelope( response.session, 2 ) ), now );
        REQUIRE( actions.size() == 1 );
        CHECK( actions.front().type == multiplayer_server_lobby_action_type::disconnect );
        CHECK( actions.front().reason.find( "retained player command" ) != std::string::npos );
    }

    SECTION( "a command rejected before event admission may be replayed as new" ) {
        multiplayer_server_lobby_settings settings = lobby_settings();
        settings.maximum_players = 1;
        settings.maximum_pending_events = 5;
        multiplayer_server_lobby lobby( settings );
        complete_hello( lobby, 1, now );
        const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
        multiplayer_protocol_envelope response = authentication.response;
        std::string error;
        for( std::uint64_t sequence = 2; sequence <= 4; ++sequence ) {
            multiplayer_protocol_envelope ping;
            ping.message_type = multiplayer_protocol_message_type::ping;
            ping.session = response.session;
            ping.sequence = sequence;
            REQUIRE( multiplayer_build_ping_payload( { sequence, sequence }, ping.payload, error ) );
            CHECK( lobby.handle_transport_event( frame_event( 1, ping ), now ).empty() );
        }
        std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event( frame_event(
                    1, player_command_envelope( response.session, 5 ) ), now );
        REQUIRE( actions.size() == 1 );
        CHECK( actions.front().type == multiplayer_server_lobby_action_type::disconnect );
        CHECK( actions.front().reason.find( "event queue" ) != std::string::npos );
        for( int index = 0; index < 3; ++index ) {
            REQUIRE( lobby.poll_event() );
        }
        lobby.handle_transport_event( closed_event( 1 ), now );
        REQUIRE( lobby.poll_event() );

        complete_hello( lobby, 2, now );
        multiplayer_resume_request resume;
        resume.resume_token = authentication.result.resume_token;
        resume.last_client_sequence = 4;
        resume.session_generation = authentication.result.session_generation;
        const completed_resume resumed = resume_successfully( lobby, 2, resume, now, 2 );
        response = resumed.response;

        CHECK( lobby.handle_transport_event( frame_event(
                2, player_command_envelope( response.session, 5 ) ), now ).empty() );
        const std::optional<multiplayer_server_lobby_event> replay = lobby.poll_event();
        REQUIRE( replay );
        CHECK( replay->message.message_type ==
               multiplayer_protocol_message_type::player_command );
        CHECK( replay->message.sequence == 5 );
    }

    SECTION( "a floor older than the retained command window releases capacity" ) {
        multiplayer_server_lobby_settings settings = lobby_settings();
        settings.maximum_players = 1;
        settings.maximum_application_messages_per_second = 1000;
        multiplayer_server_lobby lobby( settings );
        complete_hello( lobby, 1, now );
        const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
        const multiplayer_protocol_envelope &authentication_response = authentication.response;
        std::string error;
        for( std::uint64_t sequence = 2;
             sequence <= multiplayer_server_command_replay_window + 2; ++sequence ) {
            CHECK( lobby.handle_transport_event( frame_event(
                    1, player_command_envelope( authentication_response.session, sequence ) ),
                                                 now ).empty() );
        }
        lobby.handle_transport_event( closed_event( 1 ), now );
        while( lobby.poll_event() ) {
            // Drain the retained command events and the terminal disconnect before a new admission.
        }

        complete_hello( lobby, 2, now );
        multiplayer_resume_request resume;
        resume.resume_token = authentication.result.resume_token;
        resume.last_client_sequence = 1;
        resume.session_generation = authentication.result.session_generation;
        std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event(
                    frame_event( 2, resume_envelope( resume ) ), now );
        REQUIRE( actions.size() == 1 );
        multiplayer_resume_result rejected;
        REQUIRE( multiplayer_parse_resume_result_payload(
                     decode_action( actions.front() ), rejected, error ) );
        CHECK_FALSE( rejected.accepted );
        CHECK( rejected.rejection == multiplayer_protocol_rejection::session_expired );
        lobby.handle_transport_event( closed_event( 2 ), now );

        complete_hello( lobby, 3, now );
        const completed_authentication accepted = authenticate_successfully( lobby, 3, now, 2 );
        CHECK( accepted.result.accepted );
    }
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
        REQUIRE( actions.size() == 1 );
        multiplayer_authentication_result result;
        std::string error;
        REQUIRE( multiplayer_parse_authentication_result_payload(
                     decode_action( actions.front() ), result, error ) );
        CHECK_FALSE( result.accepted );
        CHECK( result.rejection == multiplayer_protocol_rejection::authentication_failed );
        CHECK( actions.front().type ==
               multiplayer_server_lobby_action_type::send_and_disconnect );
    }

    SECTION( "protocol major mismatch receives typed rejection" ) {
        multiplayer_server_lobby lobby( lobby_settings() );
        REQUIRE( lobby.handle_transport_event( connected_event( 1 ), now ).empty() );
        const std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event(
                    frame_event( 1, hello_envelope( multiplayer_protocol_current_major + 1 ) ), now );
        REQUIRE( actions.size() == 1 );
        multiplayer_server_hello result;
        std::string error;
        REQUIRE( multiplayer_parse_server_hello_payload( decode_action( actions.front() ),
                 result, error ) );
        CHECK_FALSE( result.accepted );
        CHECK( result.rejection == multiplayer_protocol_rejection::protocol_major_mismatch );
        CHECK( actions.front().type ==
               multiplayer_server_lobby_action_type::send_and_disconnect );
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
        const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
        const multiplayer_protocol_envelope &authentication_response = authentication.response;
        multiplayer_protocol_heartbeat ping;
        multiplayer_protocol_envelope ping_envelope;
        ping_envelope.message_type = multiplayer_protocol_message_type::ping;
        ping_envelope.session = authentication_response.session;
        ping_envelope.sequence = 2;
        std::string error;
        REQUIRE( multiplayer_build_ping_payload( ping, ping_envelope.payload, error ) );
        CHECK( lobby.handle_transport_event( frame_event( 1, ping_envelope ), now ).empty() );
        ping_envelope.sequence = 3;
        const std::vector<multiplayer_server_lobby_action> actions =
            lobby.handle_transport_event( frame_event( 1, ping_envelope ), now );
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
        const completed_authentication authentication = authenticate_successfully( lobby, 1, now );
        confirm_fresh_authentication( lobby, authentication, 1, now );
        lobby.handle_transport_event( closed_event( 1 ), now );
        REQUIRE( lobby.poll_event() );

        complete_hello( lobby, 2, now + std::chrono::milliseconds( 1 ) );
        std::vector<multiplayer_server_lobby_action> actions = lobby.handle_transport_event(
                    frame_event( 2, authenticate_envelope( std::string( 64, 'a' ) ) ),
                    now + std::chrono::milliseconds( 1 ) );
        REQUIRE( actions.size() == 1 );
        multiplayer_authentication_result rejected;
        std::string error;
        REQUIRE( multiplayer_parse_authentication_result_payload(
                     decode_action( actions.front() ), rejected, error ) );
        CHECK( rejected.rejection == multiplayer_protocol_rejection::server_full );
        lobby.handle_transport_event( closed_event( 2 ), now + std::chrono::milliseconds( 1 ) );

        CHECK( lobby.tick( now + std::chrono::seconds( 1 ) ).empty() );
        complete_hello( lobby, 3, now + std::chrono::milliseconds( 1001 ) );
        const completed_authentication accepted = authenticate_successfully(
                    lobby, 3, now + std::chrono::milliseconds( 1001 ),
                    authentication.result.session_generation + 1 );
        CHECK( accepted.result.accepted );
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
        REQUIRE( actions.size() == 1 );
        multiplayer_authentication_result result;
        std::string error;
        REQUIRE( multiplayer_parse_authentication_result_payload(
                     decode_action( actions.front() ), result, error ) );
        CHECK( result.rejection == multiplayer_protocol_rejection::rate_limited );
    }
}

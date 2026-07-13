#include "multiplayer_server_lobby.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "multiplayer_crypto.h"

namespace
{

bool session_is_empty( const multiplayer_session_id &session )
{
    return std::all_of( session.begin(), session.end(), []( const std::uint8_t byte ) {
        return byte == 0;
    } );
}

bool client_application_message_type( const multiplayer_protocol_message_type type )
{
    return type == multiplayer_protocol_message_type::ping ||
           type == multiplayer_protocol_message_type::player_command ||
           type == multiplayer_protocol_message_type::resync_request;
}

multiplayer_server_lobby_action disconnect_action( const multiplayer_connection_id connection,
        std::string reason )
{
    multiplayer_server_lobby_action action;
    action.type = multiplayer_server_lobby_action_type::disconnect;
    action.connection = connection;
    action.reason = std::move( reason );
    return action;
}

} // namespace

multiplayer_server_lobby::multiplayer_server_lobby( multiplayer_server_lobby_settings settings ) :
    settings_( std::move( settings ) )
{
}

bool multiplayer_server_lobby::valid( std::string &error ) const
{
    if( settings_.maximum_players < 1 || settings_.maximum_players > 4 ||
        settings_.maximum_attempts_per_minute == 0 ||
        settings_.maximum_attempts_per_minute > 120 ||
        settings_.maximum_pending_events <= settings_.maximum_players * 2 ||
        settings_.maximum_application_messages_per_second == 0 ||
        settings_.maximum_application_messages_per_second > 1000 ||
        settings_.maximum_application_bytes_per_minute < multiplayer_protocol_envelope_size ||
        settings_.maximum_application_bytes_per_minute > 64 * 1024 * 1024 ||
        settings_.handshake_timeout <= std::chrono::milliseconds::zero() ||
        settings_.resume_token_lifetime < std::chrono::seconds( 60 ) ||
        settings_.savegame_version <= 0 ) {
        error = "multiplayer lobby resource settings are invalid";
        return false;
    }
    if( !settings_.bearer_token.empty() &&
        !multiplayer_is_valid_bearer_token( settings_.bearer_token ) ) {
        error = "multiplayer lobby bearer token has an invalid format";
        return false;
    }
    if( settings_.fixed_player_identity ) {
        multiplayer_authentication_result identity_probe;
        identity_probe.accepted = true;
        identity_probe.player_id = settings_.fixed_player_identity->first;
        identity_probe.character_id = settings_.fixed_player_identity->second;
        identity_probe.resume_token = std::string( 64, 'a' );
        identity_probe.session_generation = 1;
        multiplayer_transport_payload identity_payload;
        if( !multiplayer_build_authentication_result_payload( identity_probe, identity_payload,
                error ) ) {
            error = "multiplayer lobby fixed player identity is invalid: " + error;
            return false;
        }
    }
    multiplayer_server_hello probe;
    probe.accepted = true;
    probe.build_id = settings_.server_build_id;
    probe.world_id = settings_.world_id;
    probe.content_manifest = settings_.content_manifest;
    probe.capabilities = settings_.capabilities;
    probe.savegame_version = settings_.savegame_version;
    probe.server_nonce[0] = 1;
    multiplayer_transport_payload ignored;
    return multiplayer_build_server_hello_payload( probe, ignored, error );
}

std::vector<multiplayer_server_lobby_action> multiplayer_server_lobby::handle_transport_event(
    const multiplayer_transport_event &event, const clock::time_point now )
{
    if( event.type == multiplayer_transport_event_type::connected ) {
        connection_state state;
        state.peer_address = event.peer_address.empty() ? "unknown" : event.peer_address;
        state.deadline = now + settings_.handshake_timeout;
        const bool inserted = connections_.emplace( event.connection, std::move( state ) ).second;
        if( !inserted ) {
            return { disconnect_action( event.connection, "duplicate transport connection id" ) };
        }
        return {};
    }

    const auto found = connections_.find( event.connection );
    if( found == connections_.end() ) {
        return {};
    }
    if( event.type == multiplayer_transport_event_type::frame ) {
        return handle_frame( event.connection, found->second, event.payload, now );
    }
    if( event.type == multiplayer_transport_event_type::peer_half_closed ) {
        found->second.stage = connection_stage::closing;
        return { disconnect_action( event.connection, "peer half-closed the protocol stream" ) };
    }
    handle_closed_connection( event.connection, now );
    return {};
}

std::vector<multiplayer_server_lobby_action> multiplayer_server_lobby::handle_frame(
    const multiplayer_connection_id connection, connection_state &state,
    const multiplayer_transport_payload &payload, const clock::time_point now )
{
    if( state.stage == connection_stage::closing ) {
        return {};
    }
    multiplayer_protocol_envelope envelope;
    std::string error;
    if( !multiplayer_decode_protocol_envelope( payload, envelope, error ) ) {
        state.stage = connection_stage::closing;
        return { disconnect_action( connection, "malformed protocol envelope" ) };
    }
    switch( state.stage ) {
        case connection_stage::awaiting_hello:
            return handle_client_hello( connection, state, envelope, now );
        case connection_stage::awaiting_authentication:
            if( envelope.message_type == multiplayer_protocol_message_type::resume_request ) {
                return handle_resume( connection, state, envelope, now );
            }
            return handle_authenticate( connection, state, envelope, now );
        case connection_stage::authenticated:
            if( envelope.protocol_major != multiplayer_protocol_current_major ||
                envelope.protocol_minor != multiplayer_protocol_current_minor ||
                envelope.session != state.session ) {
                state.stage = connection_stage::closing;
                return { disconnect_action( connection, "authenticated protocol session mismatch" ) };
            }
            if( !client_application_message_type( envelope.message_type ) ) {
                state.stage = connection_stage::closing;
                return { disconnect_action( connection, "message type is not valid from a client" ) };
            }
            if( envelope.sequence <= state.last_inbound_sequence ) {
                state.stage = connection_stage::closing;
                return { disconnect_action( connection, "client message sequence is not increasing" ) };
            }
            if( !record_application_message( state, payload.size(), now ) ) {
                state.stage = connection_stage::closing;
                return { disconnect_action( connection, "client application message rate exceeded" ) };
            }
            if( !application_event_capacity_available() ) {
                state.stage = connection_stage::closing;
                return { disconnect_action( connection, "server application event queue is full" ) };
            }
            state.last_inbound_sequence = envelope.sequence;
            events_.push_back( { multiplayer_server_lobby_event_type::application_message,
                                 connection, state.session, state.player_id, state.character_id,
                                 state.display_name, state.session_generation, 0, 0,
                                 std::move( envelope ) } );
            return {};
        case connection_stage::closing:
            return {};
    }
    return {};
}

std::vector<multiplayer_server_lobby_action> multiplayer_server_lobby::handle_client_hello(
    const multiplayer_connection_id connection, connection_state &state,
    const multiplayer_protocol_envelope &envelope, const clock::time_point now )
{
    if( envelope.message_type != multiplayer_protocol_message_type::client_hello ||
        envelope.sequence != 0 || !session_is_empty( envelope.session ) ) {
        state.stage = connection_stage::closing;
        return { disconnect_action( connection, "expected an unauthenticated client hello" ) };
    }
    multiplayer_client_hello client;
    std::string error;
    if( !multiplayer_parse_client_hello_payload( envelope, client, error ) ) {
        state.stage = connection_stage::closing;
        return { disconnect_action( connection, "invalid client hello" ) };
    }
    multiplayer_server_hello server = multiplayer_negotiate_client_hello(
                                          client, settings_.capabilities, settings_.server_build_id,
                                          settings_.world_id, settings_.content_manifest,
                                          settings_.savegame_version );
    if( !multiplayer_fill_secure_random( server.server_nonce.data(), server.server_nonce.size(),
                                         error ) ) {
        state.stage = connection_stage::closing;
        return { disconnect_action( connection, "server random source failed" ) };
    }
    multiplayer_protocol_envelope response;
    response.message_type = multiplayer_protocol_message_type::server_hello;
    if( !multiplayer_build_server_hello_payload( server, response.payload, error ) ) {
        state.stage = connection_stage::closing;
        return { disconnect_action( connection, "server hello construction failed" ) };
    }
    multiplayer_server_lobby_action send;
    if( !build_send_action( connection, std::move( response ), send, error ) ) {
        state.stage = connection_stage::closing;
        return { disconnect_action( connection, "server hello encoding failed" ) };
    }
    if( !server.accepted ) {
        state.stage = connection_stage::closing;
        return { std::move( send ), disconnect_action( connection, "protocol negotiation rejected" ) };
    }
    state.stage = connection_stage::awaiting_authentication;
    state.deadline = now + settings_.handshake_timeout;
    return { std::move( send ) };
}

std::vector<multiplayer_server_lobby_action> multiplayer_server_lobby::handle_authenticate(
    const multiplayer_connection_id connection, connection_state &state,
    const multiplayer_protocol_envelope &envelope, const clock::time_point now )
{
    if( envelope.message_type != multiplayer_protocol_message_type::authenticate ||
        envelope.protocol_major != multiplayer_protocol_current_major ||
        envelope.protocol_minor != multiplayer_protocol_current_minor || envelope.sequence != 1 ||
        !session_is_empty( envelope.session ) ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::invalid_state,
                                      "expected an authentication request" );
    }
    if( !record_authentication_attempt( state.peer_address, now ) ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::rate_limited,
                                      "authentication attempt rate exceeded" );
    }
    multiplayer_authenticate_request request;
    std::string error;
    if( !multiplayer_parse_authenticate_payload( envelope, request, error ) ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::malformed_message,
                                      "authentication request is malformed" );
    }
    if( !request.player_id.empty() || !request.character_id.empty() ||
        !request.challenge_response.empty() ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::permission_denied,
                                      "new sessions cannot claim an existing identity" );
    }
    const bool token_matches = settings_.bearer_token.empty() ? request.bearer_token.empty() :
                               multiplayer_constant_time_token_equal( settings_.bearer_token,
                                       request.bearer_token );
    if( !token_matches ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::authentication_failed,
                                      "authentication failed" );
    }
    if( resume_records_.size() >= settings_.maximum_players ||
        ( settings_.fixed_player_identity && !resume_records_.empty() ) ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::server_full,
                                      "server player capacity is full" );
    }
    if( !application_event_capacity_available() ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::resource_limit,
                                      "server event queue is full" );
    }

    multiplayer_authentication_result result;
    result.accepted = true;
    result.session_generation = 1;
    if( settings_.fixed_player_identity ) {
        result.player_id = settings_.fixed_player_identity->first;
        result.character_id = settings_.fixed_player_identity->second;
    } else if( !multiplayer_generate_uuid_v4( result.player_id, error ) ||
               !multiplayer_generate_uuid_v4( result.character_id, error ) ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::internal_error,
                                      "server identity generation failed" );
    }
    if( !multiplayer_generate_bearer_token( result.resume_token, error ) ||
        !multiplayer_fill_secure_random( state.session.data(), state.session.size(), error ) ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::internal_error,
                                      "server identity generation failed" );
    }
    multiplayer_protocol_envelope response;
    response.message_type = multiplayer_protocol_message_type::authentication_result;
    response.session = state.session;
    response.sequence = 1;
    if( !multiplayer_build_authentication_result_payload( result, response.payload, error ) ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::internal_error,
                                      "authentication response construction failed" );
    }
    multiplayer_server_lobby_action send;
    if( !build_send_action( connection, std::move( response ), send, error ) ) {
        state.stage = connection_stage::closing;
        return { disconnect_action( connection, "authentication response encoding failed" ) };
    }

    state.stage = connection_stage::authenticated;
    state.resume_token = result.resume_token;
    state.player_id = result.player_id;
    state.character_id = result.character_id;
    state.display_name = request.display_name;
    state.session_generation = result.session_generation;
    state.last_inbound_sequence = 1;
    resume_record record;
    record.player_id = result.player_id;
    record.character_id = result.character_id;
    record.display_name = request.display_name;
    record.session = state.session;
    record.session_generation = result.session_generation;
    record.expires_at = now + settings_.resume_token_lifetime;
    record.active_connection = connection;
    resume_records_.emplace( result.resume_token, record );
    push_control_event( { multiplayer_server_lobby_event_type::authenticated, connection,
                          state.session, result.player_id, result.character_id,
                          request.display_name, result.session_generation, 0, 0, {} } );
    return { std::move( send ) };
}

std::vector<multiplayer_server_lobby_action> multiplayer_server_lobby::handle_resume(
    const multiplayer_connection_id connection, connection_state &state,
    const multiplayer_protocol_envelope &envelope, const clock::time_point now )
{
    if( envelope.protocol_major != multiplayer_protocol_current_major ||
        envelope.protocol_minor != multiplayer_protocol_current_minor || envelope.sequence != 1 ||
        !session_is_empty( envelope.session ) ) {
        return reject_resume( connection, state, multiplayer_protocol_rejection::invalid_state,
                              "expected an unauthenticated resume request" );
    }
    if( !record_authentication_attempt( state.peer_address, now ) ) {
        return reject_resume( connection, state, multiplayer_protocol_rejection::rate_limited,
                              "resume attempt rate exceeded" );
    }
    multiplayer_resume_request request;
    std::string error;
    if( !multiplayer_parse_resume_request_payload( envelope, request, error ) ) {
        return reject_resume( connection, state,
                              multiplayer_protocol_rejection::malformed_message,
                              "resume request is malformed" );
    }
    const auto record_entry = resume_records_.find( request.resume_token );
    if( record_entry == resume_records_.end() || record_entry->second.active_connection ||
        record_entry->second.expires_at <= now ) {
        return reject_resume( connection, state, multiplayer_protocol_rejection::session_expired,
                              "resume session is unavailable" );
    }
    resume_record &record = record_entry->second;
    if( !application_event_capacity_available() ) {
        return reject_resume( connection, state, multiplayer_protocol_rejection::resource_limit,
                              "server event queue is full" );
    }
    if( record.session_generation == std::numeric_limits<std::uint64_t>::max() ||
        !multiplayer_fill_secure_random( state.session.data(), state.session.size(), error ) ) {
        return reject_resume( connection, state, multiplayer_protocol_rejection::internal_error,
                              "resume session generation failed" );
    }
    ++record.session_generation;
    record.session = state.session;
    record.expires_at = now + settings_.resume_token_lifetime;
    record.active_connection = connection;
    state.stage = connection_stage::authenticated;
    state.resume_token = request.resume_token;
    state.player_id = record.player_id;
    state.character_id = record.character_id;
    state.display_name = record.display_name;
    state.session_generation = record.session_generation;
    state.last_inbound_sequence = request.last_client_sequence;

    multiplayer_resume_result result;
    result.accepted = true;
    result.player_id = record.player_id;
    result.character_id = record.character_id;
    result.session_generation = record.session_generation;
    result.replay_from_sequence = request.last_client_sequence +
                                  ( request.last_client_sequence !=
                                    std::numeric_limits<std::uint64_t>::max() ? 1 : 0 );
    result.full_snapshot_required = true;
    multiplayer_protocol_envelope response;
    response.message_type = multiplayer_protocol_message_type::resume_result;
    response.session = state.session;
    response.sequence = 1;
    if( !multiplayer_build_resume_result_payload( result, response.payload, error ) ) {
        state.stage = connection_stage::closing;
        record.active_connection.reset();
        return { disconnect_action( connection, "resume response construction failed" ) };
    }
    multiplayer_server_lobby_action send;
    if( !build_send_action( connection, std::move( response ), send, error ) ) {
        state.stage = connection_stage::closing;
        record.active_connection.reset();
        return { disconnect_action( connection, "resume response encoding failed" ) };
    }
    push_control_event( { multiplayer_server_lobby_event_type::resumed, connection,
                          state.session, record.player_id, record.character_id,
                          record.display_name, record.session_generation,
                          request.last_server_revision, request.last_client_sequence, {} } );
    return { std::move( send ) };
}

void multiplayer_server_lobby::handle_closed_connection(
    const multiplayer_connection_id connection, const clock::time_point now )
{
    const auto found = connections_.find( connection );
    if( found == connections_.end() ) {
        return;
    }
    const connection_state state = found->second;
    if( !state.resume_token.empty() ) {
        const auto record = resume_records_.find( state.resume_token );
        if( record != resume_records_.end() && record->second.active_connection == connection ) {
            record->second.active_connection.reset();
            record->second.expires_at = now + settings_.resume_token_lifetime;
            push_control_event( { multiplayer_server_lobby_event_type::disconnected, connection,
                                  state.session, record->second.player_id,
                                  record->second.character_id, record->second.display_name,
                                  record->second.session_generation, 0, 0, {} } );
        }
    }
    connections_.erase( found );
}

bool multiplayer_server_lobby::record_authentication_attempt( const std::string &peer_address,
        const clock::time_point now )
{
    std::deque<clock::time_point> &attempts = authentication_attempts_[peer_address];
    const clock::time_point cutoff = now - std::chrono::minutes( 1 );
    while( !attempts.empty() && attempts.front() <= cutoff ) {
        attempts.pop_front();
    }
    if( attempts.size() >= settings_.maximum_attempts_per_minute ) {
        return false;
    }
    attempts.push_back( now );
    return true;
}

bool multiplayer_server_lobby::record_application_message( connection_state &state,
        const std::size_t bytes, const clock::time_point now )
{
    const clock::time_point message_cutoff = now - std::chrono::seconds( 1 );
    while( !state.application_message_times.empty() &&
           state.application_message_times.front() <= message_cutoff ) {
        state.application_message_times.pop_front();
    }
    const clock::time_point byte_cutoff = now - std::chrono::minutes( 1 );
    while( !state.application_byte_times.empty() &&
           state.application_byte_times.front().first <= byte_cutoff ) {
        state.application_bytes_in_window -= state.application_byte_times.front().second;
        state.application_byte_times.pop_front();
    }
    if( state.application_message_times.size() >=
        settings_.maximum_application_messages_per_second ||
        bytes > settings_.maximum_application_bytes_per_minute -
        std::min( state.application_bytes_in_window,
                  settings_.maximum_application_bytes_per_minute ) ) {
        return false;
    }
    state.application_message_times.push_back( now );
    state.application_byte_times.emplace_back( now, bytes );
    state.application_bytes_in_window += bytes;
    return true;
}

bool multiplayer_server_lobby::build_send_action( const multiplayer_connection_id connection,
        multiplayer_protocol_envelope envelope, multiplayer_server_lobby_action &action,
        std::string &error ) const
{
    action = {};
    action.type = multiplayer_server_lobby_action_type::send;
    action.connection = connection;
    return multiplayer_encode_protocol_envelope( envelope, action.payload, error );
}

bool multiplayer_server_lobby::application_event_capacity_available() const
{
    return events_.size() < settings_.maximum_pending_events - settings_.maximum_players * 2;
}

void multiplayer_server_lobby::push_control_event( multiplayer_server_lobby_event event )
{
    if( events_.size() < settings_.maximum_pending_events ) {
        events_.emplace_back( std::move( event ) );
    }
}

std::vector<multiplayer_server_lobby_action> multiplayer_server_lobby::reject_authentication(
    const multiplayer_connection_id connection, connection_state &state,
    const multiplayer_protocol_rejection rejection, const std::string &message )
{
    multiplayer_authentication_result result;
    result.rejection = rejection;
    result.message = message;
    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::authentication_result;
    envelope.sequence = 1;
    std::string error;
    multiplayer_server_lobby_action send;
    state.stage = connection_stage::closing;
    if( multiplayer_build_authentication_result_payload( result, envelope.payload, error ) &&
        build_send_action( connection, std::move( envelope ), send, error ) ) {
        return { std::move( send ), disconnect_action( connection, "authentication rejected" ) };
    }
    return { disconnect_action( connection, "authentication rejected" ) };
}

std::vector<multiplayer_server_lobby_action> multiplayer_server_lobby::reject_resume(
    const multiplayer_connection_id connection, connection_state &state,
    const multiplayer_protocol_rejection rejection, const std::string &message )
{
    multiplayer_resume_result result;
    result.rejection = rejection;
    result.message = message;
    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::resume_result;
    envelope.sequence = 1;
    std::string error;
    multiplayer_server_lobby_action send;
    state.stage = connection_stage::closing;
    if( multiplayer_build_resume_result_payload( result, envelope.payload, error ) &&
        build_send_action( connection, std::move( envelope ), send, error ) ) {
        return { std::move( send ), disconnect_action( connection, "resume rejected" ) };
    }
    return { disconnect_action( connection, "resume rejected" ) };
}

std::vector<multiplayer_server_lobby_action> multiplayer_server_lobby::tick(
    const clock::time_point now )
{
    std::vector<multiplayer_server_lobby_action> actions;
    for( auto &entry : connections_ ) {
        connection_state &state = entry.second;
        if( ( state.stage == connection_stage::awaiting_hello ||
              state.stage == connection_stage::awaiting_authentication ) && state.deadline <= now ) {
            state.stage = connection_stage::closing;
            actions.emplace_back( disconnect_action( entry.first, "handshake timeout" ) );
        }
    }
    for( auto record = resume_records_.begin(); record != resume_records_.end(); ) {
        if( !record->second.active_connection && record->second.expires_at <= now ) {
            record = resume_records_.erase( record );
        } else {
            ++record;
        }
    }
    const clock::time_point cutoff = now - std::chrono::minutes( 1 );
    for( auto attempts = authentication_attempts_.begin();
         attempts != authentication_attempts_.end(); ) {
        while( !attempts->second.empty() && attempts->second.front() <= cutoff ) {
            attempts->second.pop_front();
        }
        if( attempts->second.empty() ) {
            attempts = authentication_attempts_.erase( attempts );
        } else {
            ++attempts;
        }
    }
    return actions;
}

std::optional<multiplayer_server_lobby_event> multiplayer_server_lobby::poll_event()
{
    if( events_.empty() ) {
        return std::nullopt;
    }
    multiplayer_server_lobby_event event = std::move( events_.front() );
    events_.pop_front();
    return event;
}

std::size_t multiplayer_server_lobby::connection_count() const
{
    return connections_.size();
}

std::size_t multiplayer_server_lobby::authenticated_player_count() const
{
    return static_cast<std::size_t>( std::count_if( resume_records_.begin(), resume_records_.end(),
    []( const auto & entry ) {
        return entry.second.active_connection.has_value();
    } ) );
}

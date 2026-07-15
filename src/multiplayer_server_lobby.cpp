#include "multiplayer_server_lobby.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "cata_assert.h"
#include "multiplayer_crypto.h"
#include "multiplayer_session_generation.h"

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
           type == multiplayer_protocol_message_type::disconnect_notice ||
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
    handle_closed_connection( event, now );
    return {};
}

std::vector<multiplayer_server_lobby_action> multiplayer_server_lobby::handle_frame(
    const multiplayer_connection_id connection, connection_state &state,
    const multiplayer_transport_payload &payload, const clock::time_point now )
{
    if( state.stage == connection_stage::draining ) {
        state.stage = connection_stage::closing;
        return { disconnect_action( connection,
                                    "client sent a message after requesting graceful disconnect" ) };
    }
    if( state.stage == connection_stage::releasing ) {
        return {};
    }
    if( state.stage == connection_stage::authentication_pending ||
        state.stage == connection_stage::resume_pending ) {
        state.stage = connection_stage::closing;
        return { disconnect_action( connection,
                                    "client sent a message while admission was pending" ) };
    }
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
        case connection_stage::authenticated: {
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
            const auto record_entry = resume_records_.find( state.resume_token );
            if( record_entry == resume_records_.end() ||
                record_entry->second.active_connection != connection ||
                record_entry->second.session != state.session ||
                record_entry->second.session_generation != state.session_generation ) {
                state.stage = connection_stage::closing;
                return { disconnect_action( connection, "authenticated resume record is unavailable" ) };
            }
            resume_record &record = record_entry->second;
            record.client_has_resume_token = true;
            if( state.resume_replay_high_water != 0 &&
                envelope.sequence <= state.resume_replay_high_water ) {
                const bool known_player_command =
                    envelope.message_type == multiplayer_protocol_message_type::player_command &&
                    std::find( record.recent_player_command_sequences.begin(),
                               record.recent_player_command_sequences.end(), envelope.sequence ) !=
                    record.recent_player_command_sequences.end();
                if( !known_player_command ) {
                    state.stage = connection_stage::closing;
                    return { disconnect_action(
                                 connection,
                                 "resumed sequence is not a retained player command" ) };
                }
            }
            if( envelope.message_type == multiplayer_protocol_message_type::player_command ) {
                multiplayer_player_command command;
                if( !multiplayer_parse_player_command_payload( envelope, command, error ) ) {
                    state.stage = connection_stage::closing;
                    return { disconnect_action( connection,
                                                "invalid semantic player command" ) };
                }
            }
            if( envelope.message_type != multiplayer_protocol_message_type::disconnect_notice &&
                !application_event_capacity_available() ) {
                state.stage = connection_stage::closing;
                return { disconnect_action( connection,
                                            "server application event queue is full" ) };
            }
            state.last_inbound_sequence = envelope.sequence;
            record.observed_application_high_water = std::max(
                        record.observed_application_high_water, envelope.sequence );
            if( envelope.message_type == multiplayer_protocol_message_type::player_command &&
                std::find( record.recent_player_command_sequences.begin(),
                           record.recent_player_command_sequences.end(), envelope.sequence ) ==
                record.recent_player_command_sequences.end() ) {
                record.recent_player_command_sequences.push_back( envelope.sequence );
                if( record.recent_player_command_sequences.size() >
                    multiplayer_server_command_replay_window ) {
                    record.minimum_command_replay_floor =
                        record.recent_player_command_sequences.front();
                    record.recent_player_command_sequences.pop_front();
                }
            }
            if( envelope.message_type == multiplayer_protocol_message_type::disconnect_notice ) {
                return handle_disconnect_notice( connection, state, envelope, record );
            }
            const bool confirms_resume_generation = record.last_resume &&
                                                    !record.pending_confirmation_connection;
            if( confirms_resume_generation ) {
                record.pending_confirmation_connection = connection;
            }
            multiplayer_server_lobby_event application;
            application.type = multiplayer_server_lobby_event_type::application_message;
            application.connection = connection;
            application.session = state.session;
            application.player_id = state.player_id;
            application.character_id = state.character_id;
            application.display_name = state.display_name;
            application.session_generation = state.session_generation;
            application.message = std::move( envelope );
            application.confirms_resume_generation = confirms_resume_generation;
            events_.emplace_back( std::move( application ) );
            return {};
        }
        case connection_stage::authentication_pending:
        case connection_stage::resume_pending:
        case connection_stage::draining:
        case connection_stage::releasing:
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
        send.type = multiplayer_server_lobby_action_type::send_and_disconnect;
        send.reason = "protocol negotiation rejected";
        return { std::move( send ) };
    }
    state.stage = connection_stage::awaiting_authentication;
    state.deadline = now + settings_.handshake_timeout;
    return { std::move( send ) };
}

std::vector<multiplayer_server_lobby_action>
multiplayer_server_lobby::handle_disconnect_notice(
    const multiplayer_connection_id connection, connection_state &state,
    const multiplayer_protocol_envelope &envelope,
    resume_record &record )
{
    multiplayer_disconnect_notice request;
    std::string error;
    if( !multiplayer_parse_disconnect_notice_payload( envelope, request, error ) ||
        request.code != multiplayer_protocol_rejection::none ) {
        state.stage = connection_stage::closing;
        return { disconnect_action( connection, "invalid graceful disconnect request" ) };
    }
    if( events_.size() >= settings_.maximum_pending_events ) {
        state.stage = connection_stage::closing;
        return { disconnect_action( connection, "server control event queue is full" ) };
    }
    const bool confirms_resume_generation = record.last_resume &&
                                            !record.pending_confirmation_connection;
    if( confirms_resume_generation ) {
        record.pending_confirmation_connection = connection;
    }
    state.stage = connection_stage::draining;
    multiplayer_server_lobby_event event;
    event.type = multiplayer_server_lobby_event_type::graceful_disconnect_requested;
    event.connection = connection;
    event.session = state.session;
    event.player_id = state.player_id;
    event.character_id = state.character_id;
    event.display_name = state.display_name;
    event.session_generation = state.session_generation;
    event.message = envelope;
    event.confirms_resume_generation = confirms_resume_generation;
    events_.emplace_back( std::move( event ) );
    return {};
}

std::vector<multiplayer_server_lobby_action>
multiplayer_server_lobby::complete_graceful_disconnect(
    const multiplayer_server_lobby_event &request )
{
    const auto found = connections_.find( request.connection );
    if( request.type != multiplayer_server_lobby_event_type::graceful_disconnect_requested ||
        found == connections_.end() ) {
        return {};
    }
    connection_state &state = found->second;
    if( state.stage != connection_stage::draining || request.session != state.session ||
        request.session_generation != state.session_generation ||
        request.message.message_type != multiplayer_protocol_message_type::disconnect_notice ||
        request.message.session != state.session ||
        request.message.sequence != state.last_inbound_sequence ) {
        return {};
    }
    const auto record = resume_records_.find( state.resume_token );
    if( record == resume_records_.end() || record->second.active_connection != request.connection ||
        record->second.session != state.session ||
        record->second.session_generation != state.session_generation ) {
        return {};
    }

    multiplayer_protocol_envelope response;
    response.message_type = multiplayer_protocol_message_type::disconnect_notice;
    response.session = state.session;
    response.sequence = request.message.sequence;
    std::string error;
    if( !multiplayer_build_disconnect_notice_payload(
{ multiplayer_protocol_rejection::none, "multiplayer session released" },
response.payload, error ) ) {
        state.stage = connection_stage::closing;
        return { disconnect_action( request.connection,
                                    "graceful disconnect response failed" ) };
    }
    multiplayer_server_lobby_action send;
    if( !build_send_action( request.connection, std::move( response ), send, error ) ) {
        state.stage = connection_stage::closing;
        return { disconnect_action( request.connection,
                                    "graceful disconnect response failed" ) };
    }

    send.type = multiplayer_server_lobby_action_type::send_and_disconnect;
    send.reason = multiplayer_graceful_release_transport_reason;
    state.stage = connection_stage::releasing;
    return { std::move( send ) };
}

bool multiplayer_server_lobby::admission_is_pending(
    const multiplayer_server_lobby_event &request ) const
{
    if( request.type != multiplayer_server_lobby_event_type::authentication_pending &&
        request.type != multiplayer_server_lobby_event_type::resume_pending ) {
        return false;
    }
    const auto found = connections_.find( request.connection );
    if( found == connections_.end() ) {
        return false;
    }
    const connection_state &state = found->second;
    const bool authentication =
        request.type == multiplayer_server_lobby_event_type::authentication_pending;
    if( state.stage != ( authentication ? connection_stage::authentication_pending :
                         connection_stage::resume_pending ) ||
        request.admission_id == 0 || request.admission_id != state.admission_id ||
        request.session != state.session || request.player_id != state.player_id ||
        request.character_id != state.character_id ||
        request.expected_session_generation != state.expected_session_generation ) {
        return false;
    }
    if( authentication ) {
        return state.resume_token.size() == 64 && state.expected_session_generation == 0;
    }
    const auto record = resume_records_.find( state.resume_token );
    return record != resume_records_.end() &&
           record->second.pending_connection == request.connection &&
           record->second.pending_admission_id == request.admission_id &&
           request.last_server_revision == state.pending_last_server_revision &&
           request.last_client_sequence == state.pending_last_client_sequence;
}

bool multiplayer_server_lobby::prepare_admission(
    const multiplayer_server_lobby_event &request,
    const multiplayer_server_lobby_admission_decision &decision,
    multiplayer_server_lobby_prepared_admission &prepared, std::string &error ) const
{
    prepared = {};
    if( !admission_is_pending( request ) ) {
        error = "multiplayer admission request is stale";
        return false;
    }
    if( decision.accepted ) {
        if( decision.rejection != multiplayer_protocol_rejection::none ||
            decision.player_id != request.player_id ||
            decision.character_id != request.character_id ||
            !multiplayer_is_valid_session_generation( decision.session_generation ) ||
            ( request.type == multiplayer_server_lobby_event_type::resume_pending &&
              !multiplayer_is_next_session_generation(
                  request.expected_session_generation, decision.session_generation ) ) ) {
            error = "multiplayer admission decision is inconsistent with the pending request";
            return false;
        }
    } else if( decision.rejection == multiplayer_protocol_rejection::none ||
               !decision.player_id.empty() || !decision.character_id.empty() ||
               decision.session_generation != 0 ) {
        error = "multiplayer admission rejection contains accepted-session fields";
        return false;
    }

    const connection_state &state = connections_.find( request.connection )->second;
    multiplayer_protocol_envelope response;
    response.sequence = 1;
    if( request.type == multiplayer_server_lobby_event_type::authentication_pending ) {
        multiplayer_authentication_result result;
        result.accepted = decision.accepted;
        result.player_id = decision.player_id;
        result.character_id = decision.character_id;
        result.resume_token = decision.accepted ? state.resume_token : std::string();
        result.session_generation = decision.session_generation;
        result.rejection = decision.accepted ? multiplayer_protocol_rejection::none :
                           decision.rejection;
        result.message = decision.message;
        response.message_type = multiplayer_protocol_message_type::authentication_result;
        response.session = decision.accepted ? state.session : multiplayer_session_id {};
        if( !multiplayer_build_authentication_result_payload( result, response.payload, error ) ) {
            return false;
        }
    } else {
        multiplayer_resume_result result;
        result.accepted = decision.accepted;
        result.player_id = decision.player_id;
        result.character_id = decision.character_id;
        result.session_generation = decision.session_generation;
        result.replay_from_sequence = decision.accepted ?
                                      state.pending_last_client_sequence +
                                      ( state.pending_last_client_sequence !=
                                        std::numeric_limits<std::uint64_t>::max() ? 1 : 0 ) : 0;
        result.full_snapshot_required = true;
        result.rejection = decision.accepted ? multiplayer_protocol_rejection::none :
                           decision.rejection;
        result.message = decision.message;
        response.message_type = multiplayer_protocol_message_type::resume_result;
        response.session = decision.accepted ? state.session : multiplayer_session_id {};
        if( !multiplayer_build_resume_result_payload( result, response.payload, error ) ) {
            return false;
        }
    }

    multiplayer_server_lobby_action send;
    if( !build_send_action( request.connection, std::move( response ), send, error ) ) {
        return false;
    }
    prepared.request = request;
    prepared.decision = decision;
    if( !decision.accepted ) {
        send.type = multiplayer_server_lobby_action_type::send_and_disconnect;
        send.reason = "multiplayer admission rejected";
    }
    prepared.actions.emplace_back( std::move( send ) );
    error.clear();
    return true;
}

bool multiplayer_server_lobby::publish_admission(
    const multiplayer_server_lobby_prepared_admission &prepared )
{
    if( prepared.actions.empty() || !admission_is_pending( prepared.request ) ) {
        return false;
    }
    connection_state &state = connections_.find( prepared.request.connection )->second;
    const multiplayer_server_lobby_admission_decision &decision = prepared.decision;
    if( !decision.accepted ) {
        if( state.stage == connection_stage::resume_pending ) {
            const auto record = resume_records_.find( state.resume_token );
            if( record != resume_records_.end() &&
                record->second.pending_connection == prepared.request.connection &&
                record->second.pending_admission_id == prepared.request.admission_id ) {
                if( decision.rejection == multiplayer_protocol_rejection::session_expired ) {
                    resume_records_.erase( record );
                } else {
                    record->second.pending_connection.reset();
                    record->second.pending_admission_id = 0;
                }
            }
        }
        state.stage = connection_stage::closing;
        return true;
    }

    state.session_generation = decision.session_generation;
    state.last_inbound_sequence = state.stage == connection_stage::authentication_pending ? 1 :
                                  state.pending_last_client_sequence;
    if( state.stage == connection_stage::authentication_pending ) {
        resume_record record;
        record.player_id = decision.player_id;
        record.character_id = decision.character_id;
        record.display_name = state.display_name;
        record.session = state.session;
        record.session_generation = decision.session_generation;
        record.expires_at = state.pending_resume_expires_at;
        record.active_connection = prepared.request.connection;
        const bool inserted = resume_records_.emplace( state.resume_token,
                              std::move( record ) ).second;
        cata_assert( inserted );
        if( !inserted ) {
            return false;
        }
        state.stage = connection_stage::authenticated;
        push_control_event( { multiplayer_server_lobby_event_type::authenticated,
                              prepared.request.connection, state.session, decision.player_id,
                              decision.character_id, state.display_name,
                              decision.session_generation, 0, 0, {} } );
    } else {
        const auto record_entry = resume_records_.find( state.resume_token );
        if( record_entry == resume_records_.end() ) {
            return false;
        }
        resume_record &record = record_entry->second;
        if( !state.pending_resume_replay ) {
            record.session_generation = decision.session_generation;
            record.last_resume = resume_fingerprint {
                state.expected_session_generation,
                state.pending_last_server_revision,
                state.pending_last_client_sequence
            };
        } else if( record.session_generation != decision.session_generation ) {
            return false;
        }
        record.pending_confirmation_connection.reset();
        record.session = state.session;
        record.expires_at = state.pending_resume_expires_at;
        record.active_connection = prepared.request.connection;
        record.pending_connection.reset();
        record.pending_admission_id = 0;
        record.observed_application_high_water = std::max(
                    record.observed_application_high_water,
                    state.pending_last_client_sequence );
        state.resume_replay_high_water = state.pending_resume_replay_high_water;
        state.stage = connection_stage::authenticated;
        push_control_event( { multiplayer_server_lobby_event_type::resumed,
                              prepared.request.connection, state.session, decision.player_id,
                              decision.character_id, state.display_name,
                              decision.session_generation,
                              state.pending_last_server_revision,
                              state.pending_last_client_sequence, {} } );
    }
    return true;
}

bool multiplayer_server_lobby::record_session_confirmed(
    const multiplayer_server_lobby_event &event )
{
    if( !event.confirms_resume_generation ||
        ( event.type != multiplayer_server_lobby_event_type::session_confirmed &&
          event.type != multiplayer_server_lobby_event_type::application_message &&
          event.type != multiplayer_server_lobby_event_type::graceful_disconnect_requested ) ) {
        return false;
    }
    const auto record = std::find_if( resume_records_.begin(), resume_records_.end(),
    [&event]( const auto & item ) {
        const resume_record &candidate = item.second;
        return candidate.pending_confirmation_connection == event.connection &&
               candidate.last_resume && candidate.session == event.session &&
               candidate.player_id == event.player_id &&
               candidate.character_id == event.character_id &&
               candidate.session_generation == event.session_generation;
    } );
    if( record == resume_records_.end() ) {
        return false;
    }
    record->second.last_resume.reset();
    record->second.pending_confirmation_connection.reset();
    return true;
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
    const std::size_t reserved_players = resume_records_.size() + pending_authentication_count();
    if( reserved_players >= settings_.maximum_players ||
        ( settings_.fixed_player_identity && reserved_players != 0 ) ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::server_full,
                                      "server player capacity is full" );
    }
    if( !application_event_capacity_available() ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::resource_limit,
                                      "server event queue is full" );
    }

    if( next_admission_id_ == std::numeric_limits<std::uint64_t>::max() ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::internal_error,
                                      "server admission identity is exhausted" );
    }
    std::string proposed_player_id;
    std::string proposed_character_id;
    if( settings_.fixed_player_identity ) {
        proposed_player_id = settings_.fixed_player_identity->first;
        proposed_character_id = settings_.fixed_player_identity->second;
    } else if( !multiplayer_generate_uuid_v4( proposed_player_id, error ) ||
               !multiplayer_generate_uuid_v4( proposed_character_id, error ) ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::internal_error,
                                      "server identity generation failed" );
    }
    std::string resume_token;
    for( int attempt = 0; attempt < 8 && resume_token.empty(); ++attempt ) {
        std::string candidate;
        if( !multiplayer_generate_bearer_token( candidate, error ) ) {
            break;
        }
        const bool connection_uses_candidate = std::any_of(
        connections_.begin(), connections_.end(), [&candidate]( const auto & item ) {
            return item.second.resume_token == candidate;
        } );
        if( resume_records_.find( candidate ) == resume_records_.end() &&
            !connection_uses_candidate ) {
            resume_token = std::move( candidate );
        }
    }
    if( resume_token.empty() ||
        !multiplayer_fill_secure_random( state.session.data(), state.session.size(), error ) ) {
        return reject_authentication( connection, state,
                                      multiplayer_protocol_rejection::internal_error,
                                      "server identity generation failed" );
    }
    state.stage = connection_stage::authentication_pending;
    state.admission_id = next_admission_id_++;
    state.resume_token = std::move( resume_token );
    state.player_id = std::move( proposed_player_id );
    state.character_id = std::move( proposed_character_id );
    state.display_name = request.display_name;
    state.session_generation = 0;
    state.expected_session_generation = 0;
    state.pending_last_server_revision = 0;
    state.pending_last_client_sequence = 0;
    state.pending_resume_replay_high_water = 0;
    state.pending_resume_replay = false;
    state.pending_resume_expires_at = now + settings_.resume_token_lifetime;
    state.last_inbound_sequence = 1;
    multiplayer_server_lobby_event pending;
    pending.type = multiplayer_server_lobby_event_type::authentication_pending;
    pending.connection = connection;
    pending.session = state.session;
    pending.player_id = state.player_id;
    pending.character_id = state.character_id;
    pending.display_name = state.display_name;
    pending.admission_id = state.admission_id;
    push_control_event( std::move( pending ) );
    return {};
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
    if( record_entry == resume_records_.end() ) {
        return reject_resume( connection, state, multiplayer_protocol_rejection::session_expired,
                              "resume session is unavailable" );
    }
    resume_record &record = record_entry->second;
    record.client_has_resume_token = true;
    if( record.active_connection || record.pending_connection ) {
        return reject_resume( connection, state, multiplayer_protocol_rejection::invalid_state,
                              "resume session already has an active or pending connection" );
    }
    if( record.expires_at <= now ) {
        resume_records_.erase( record_entry );
        return reject_resume( connection, state, multiplayer_protocol_rejection::session_expired,
                              "resume session has expired" );
    }
    if( request.last_client_sequence < record.minimum_command_replay_floor ) {
        resume_records_.erase( record_entry );
        return reject_resume( connection, state, multiplayer_protocol_rejection::session_expired,
                              "resume replay window has expired" );
    }
    const bool normal_resume = request.session_generation == record.session_generation;
    const bool replay_generation_matches =
        request.session_generation < multiplayer_session_generation_exclusive_limit - 1 &&
        request.session_generation + 1 == record.session_generation;
    const bool replay_fingerprint_matches = record.last_resume &&
                                            record.last_resume->expected_session_generation == request.session_generation &&
                                            record.last_resume->last_server_revision == request.last_server_revision &&
                                            record.last_resume->last_client_sequence == request.last_client_sequence;
    const bool replay_committed_resume = replay_generation_matches && replay_fingerprint_matches;
    if( !normal_resume && !replay_committed_resume ) {
        const bool conflicting_replay = replay_generation_matches && record.last_resume;
        resume_records_.erase( record_entry );
        return reject_resume( connection, state, multiplayer_protocol_rejection::session_expired,
                              conflicting_replay ?
                              "resume retry does not match the committed admission" :
                              "resume session generation is stale" );
    }
    if( !application_event_capacity_available() ) {
        return reject_resume( connection, state, multiplayer_protocol_rejection::resource_limit,
                              "server event queue is full" );
    }
    if( next_admission_id_ == std::numeric_limits<std::uint64_t>::max() ||
        ( normal_resume &&
          !multiplayer_is_next_session_generation( record.session_generation,
                  record.session_generation + 1 ) ) ||
        !multiplayer_fill_secure_random( state.session.data(), state.session.size(), error ) ) {
        return reject_resume( connection, state, multiplayer_protocol_rejection::internal_error,
                              "resume session generation failed" );
    }
    state.stage = connection_stage::resume_pending;
    state.admission_id = next_admission_id_++;
    state.resume_token = request.resume_token;
    state.player_id = record.player_id;
    state.character_id = record.character_id;
    state.display_name = record.display_name;
    state.session_generation = 0;
    state.expected_session_generation = request.session_generation;
    state.pending_last_server_revision = request.last_server_revision;
    state.pending_last_client_sequence = request.last_client_sequence;
    state.pending_resume_replay_high_water = record.observed_application_high_water;
    state.pending_resume_replay = replay_committed_resume;
    state.pending_resume_expires_at = now + settings_.resume_token_lifetime;
    state.last_inbound_sequence = request.last_client_sequence;
    record.pending_connection = connection;
    record.pending_admission_id = state.admission_id;

    multiplayer_server_lobby_event pending;
    pending.type = multiplayer_server_lobby_event_type::resume_pending;
    pending.connection = connection;
    pending.session = state.session;
    pending.player_id = state.player_id;
    pending.character_id = state.character_id;
    pending.display_name = state.display_name;
    pending.last_server_revision = request.last_server_revision;
    pending.last_client_sequence = request.last_client_sequence;
    pending.admission_id = state.admission_id;
    pending.expected_session_generation = request.session_generation;
    push_control_event( std::move( pending ) );
    return {};
}

void multiplayer_server_lobby::handle_closed_connection(
    const multiplayer_transport_event &event, const clock::time_point now )
{
    const auto found = connections_.find( event.connection );
    if( found == connections_.end() ) {
        return;
    }
    const connection_state state = found->second;
    if( !state.resume_token.empty() ) {
        const auto record = resume_records_.find( state.resume_token );
        if( record != resume_records_.end() &&
            record->second.pending_connection == event.connection ) {
            record->second.pending_connection.reset();
            record->second.pending_admission_id = 0;
        } else if( record != resume_records_.end() &&
                   record->second.active_connection == event.connection ) {
            const bool graceful_release_completed =
                state.stage == connection_stage::releasing &&
                event.type == multiplayer_transport_event_type::disconnected &&
                event.detail == multiplayer_graceful_release_transport_reason;
            const std::string player_id = record->second.player_id;
            const std::string character_id = record->second.character_id;
            const std::string display_name = record->second.display_name;
            const std::uint64_t session_generation = record->second.session_generation;
            if( graceful_release_completed ||
                !record->second.client_has_resume_token ) {
                resume_records_.erase( record );
            } else {
                record->second.active_connection.reset();
                record->second.expires_at = now + settings_.resume_token_lifetime;
            }
            push_control_event( { multiplayer_server_lobby_event_type::disconnected,
                                  event.connection, state.session, player_id,
                                  character_id, display_name, session_generation,
                                  0, 0, {} } );
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

std::size_t multiplayer_server_lobby::pending_authentication_count() const
{
    return static_cast<std::size_t>( std::count_if( connections_.begin(), connections_.end(),
    []( const auto & item ) {
        return item.second.stage == connection_stage::authentication_pending;
    } ) );
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
        send.type = multiplayer_server_lobby_action_type::send_and_disconnect;
        send.reason = "authentication rejected";
        return { std::move( send ) };
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
        send.type = multiplayer_server_lobby_action_type::send_and_disconnect;
        send.reason = "resume rejected";
        return { std::move( send ) };
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
              state.stage == connection_stage::awaiting_authentication ||
              state.stage == connection_stage::authentication_pending ||
              state.stage == connection_stage::resume_pending ) && state.deadline <= now ) {
            state.stage = connection_stage::closing;
            actions.emplace_back( disconnect_action( entry.first, "handshake timeout" ) );
        }
    }
    for( auto record = resume_records_.begin(); record != resume_records_.end(); ) {
        if( !record->second.active_connection && !record->second.pending_connection &&
            record->second.expires_at <= now ) {
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

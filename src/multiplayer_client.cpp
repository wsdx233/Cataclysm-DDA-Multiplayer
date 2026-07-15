#include "multiplayer_client.h"

#include <algorithm>
#include <limits>
#include <utility>

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

bool is_handshake_stage( const multiplayer_client_stage stage )
{
    return stage == multiplayer_client_stage::connecting ||
           stage == multiplayer_client_stage::awaiting_server_hello ||
           stage == multiplayer_client_stage::awaiting_authentication ||
           stage == multiplayer_client_stage::awaiting_initial_scene;
}

} // namespace

multiplayer_client::multiplayer_client( multiplayer_client_settings settings ) :
    settings_( std::move( settings ) )
{
}

multiplayer_client::~multiplayer_client()
{
    stop();
}

bool multiplayer_client::start( std::string &error )
{
    if( stage_ != multiplayer_client_stage::stopped ) {
        error = "multiplayer client has already been started";
        return false;
    }
    reconnecting_ = false;
    clear_authenticated_session();
    events_.clear();
    terminal_error_.clear();
    return begin_connection( false, error );
}

bool multiplayer_client::reconnect( std::string &error )
{
    if( stage_ != multiplayer_client_stage::disconnected &&
        stage_ != multiplayer_client_stage::failed ) {
        error = "multiplayer client can reconnect only after a disconnect";
        return false;
    }
    const bool resume = !resume_token_.empty() && !player_id_.empty() && !character_id_.empty();
    if( transport_ ) {
        transport_->stop();
        transport_.reset();
    }
    reconnecting_ = resume;
    session_ = {};
    return begin_connection( resume, error );
}

bool multiplayer_client::begin_connection( const bool resume, std::string &error )
{
    if( settings_.endpoint.host.empty() || settings_.endpoint.port == 0 ||
        settings_.handshake_timeout <= std::chrono::milliseconds::zero() ||
        settings_.maximum_events == 0 || settings_.maximum_pending_commands == 0 ) {
        error = "multiplayer client settings are invalid";
        return false;
    }

    multiplayer_client_hello hello;
    hello.client_kind = settings_.client_kind;
    hello.build_id = settings_.build_id;
    hello.content_manifest = settings_.content_manifest;
    hello.server_state_schema = multiplayer_server_state_schema_version;
    hello.savegame_version = settings_.savegame_version;
    hello.capabilities = { { "semantic-scene", 1, true } };
    if( !multiplayer_fill_secure_random( hello.client_nonce.data(), hello.client_nonce.size(),
                                         error ) ) {
        return false;
    }
    if( !multiplayer_build_client_hello_payload( hello, hello_payload_, error ) ) {
        return false;
    }

    transport_ = std::make_unique<multiplayer_client_transport>();
    if( !transport_->start( settings_.endpoint, error ) ) {
        transport_.reset();
        return false;
    }
    reconnecting_ = resume;
    terminal_error_.clear();
    stage_ = multiplayer_client_stage::connecting;
    handshake_deadline_ = clock::now() + settings_.handshake_timeout;
    error.clear();
    return true;
}

bool multiplayer_client::poll_once( const clock::time_point now, std::string &error )
{
    if( stage_ == multiplayer_client_stage::failed ) {
        error = terminal_error_;
        return false;
    }
    if( stage_ == multiplayer_client_stage::disconnected ) {
        error.clear();
        return true;
    }
    if( !transport_ ) {
        error = "multiplayer client transport is not running";
        return false;
    }
    while( std::optional<multiplayer_transport_event> event = transport_->poll_event() ) {
        if( !handle_transport_event( std::move( *event ), now, error ) ) {
            return false;
        }
    }
    if( is_handshake_stage( stage_ ) && now >= handshake_deadline_ ) {
        return fail( "multiplayer connection handshake timed out", error );
    }
    error.clear();
    return true;
}

bool multiplayer_client::handle_transport_event( multiplayer_transport_event event,
        const clock::time_point now, std::string &error )
{
    switch( event.type ) {
        case multiplayer_transport_event_type::connected: {
            if( stage_ != multiplayer_client_stage::connecting ) {
                return fail( "transport connected outside the client connection stage", error );
            }
            multiplayer_protocol_envelope envelope;
            envelope.message_type = multiplayer_protocol_message_type::client_hello;
            envelope.payload = hello_payload_;
            if( !send_envelope( envelope, error ) ) {
                return fail( error, error );
            }
            stage_ = multiplayer_client_stage::awaiting_server_hello;
            handshake_deadline_ = now + settings_.handshake_timeout;
            return true;
        }
        case multiplayer_transport_event_type::frame: {
            multiplayer_protocol_envelope envelope;
            if( !multiplayer_decode_protocol_envelope( event.payload, envelope, error ) ) {
                return fail( "server sent an invalid protocol envelope: " + error, error );
            }
            return handle_envelope( std::move( envelope ), now, error );
        }
        case multiplayer_transport_event_type::disconnected:
        case multiplayer_transport_event_type::peer_half_closed:
        case multiplayer_transport_event_type::transport_error:
        case multiplayer_transport_event_type::protocol_error:
            stage_ = multiplayer_client_stage::disconnected;
            pending_ping_nonces_.clear();
            if( !push_event( { multiplayer_client_event_type::disconnected, {}, {}, {},
                               event.detail.empty() ? "server connection closed" : event.detail } ) ) {
                return fail( "multiplayer client event queue is full", error );
            }
            error.clear();
            return true;
    }
    return fail( "client transport returned an unknown event", error );
}

bool multiplayer_client::handle_envelope( multiplayer_protocol_envelope envelope,
        const clock::time_point now, std::string &error )
{
    if( envelope.protocol_major != multiplayer_protocol_current_major ||
        envelope.protocol_minor != multiplayer_protocol_current_minor ) {
        return fail( "server protocol envelope version is incompatible", error );
    }
    if( stage_ == multiplayer_client_stage::awaiting_server_hello ) {
        return handle_server_hello( envelope, now, error );
    }
    if( stage_ == multiplayer_client_stage::awaiting_authentication ) {
        return reconnecting_ ? handle_resume_result( envelope, now, error ) :
               handle_authentication_result( envelope, now, error );
    }
    if( stage_ != multiplayer_client_stage::awaiting_initial_scene &&
        stage_ != multiplayer_client_stage::ready &&
        stage_ != multiplayer_client_stage::disconnecting ) {
        return fail( "server sent an application message before authentication", error );
    }
    if( envelope.session != session_ || session_is_empty( envelope.session ) ) {
        return fail( "server application message has the wrong session id", error );
    }

    switch( envelope.message_type ) {
        case multiplayer_protocol_message_type::scene_snapshot:
            return handle_scene( envelope, error );
        case multiplayer_protocol_message_type::command_result: {
            multiplayer_command_result result;
            if( !multiplayer_parse_command_result_payload( envelope, result, error ) ) {
                return fail( "server command result is invalid: " + error, error );
            }
            const auto pending = pending_commands_.find( result.client_sequence );
            if( pending == pending_commands_.end() ) {
                return fail( "server returned a command result for an unknown sequence", error );
            }
            if( result.server_revision < pending->second.base_revision ) {
                return fail( "server command result predates the command base revision", error );
            }
            minimum_required_scene_revision_ = std::max( minimum_required_scene_revision_,
                                               result.server_revision );
            pending_commands_.erase( pending );
            update_resume_sequence_floor();
            multiplayer_client_event event;
            event.type = multiplayer_client_event_type::command_result;
            event.command_result = std::move( result );
            if( !push_event( std::move( event ) ) ) {
                return fail( "multiplayer client event queue is full", error );
            }
            error.clear();
            return true;
        }
        case multiplayer_protocol_message_type::pong: {
            multiplayer_protocol_heartbeat pong;
            if( !multiplayer_parse_pong_payload( envelope, pong, error ) ) {
                return fail( "server pong is invalid: " + error, error );
            }
            const auto pending = pending_ping_nonces_.find( envelope.sequence );
            if( pending == pending_ping_nonces_.end() || pending->second != pong.nonce ) {
                return fail( "server pong does not match an outstanding ping", error );
            }
            pending_ping_nonces_.erase( pending );
            multiplayer_client_event event;
            event.type = multiplayer_client_event_type::pong;
            event.heartbeat = pong;
            if( !push_event( std::move( event ) ) ) {
                return fail( "multiplayer client event queue is full", error );
            }
            error.clear();
            return true;
        }
        case multiplayer_protocol_message_type::disconnect_notice: {
            multiplayer_disconnect_notice notice;
            if( !multiplayer_parse_disconnect_notice_payload( envelope, notice, error ) ) {
                return fail( "server disconnect notice is invalid: " + error, error );
            }
            if( stage_ == multiplayer_client_stage::disconnecting &&
                envelope.sequence != graceful_disconnect_sequence_ ) {
                return fail( "server graceful disconnect acknowledgement has the wrong sequence",
                             error );
            }
            const bool graceful_ack = stage_ == multiplayer_client_stage::disconnecting &&
                                      graceful_disconnect_sequence_ != 0 &&
                                      notice.code == multiplayer_protocol_rejection::none;
            if( graceful_ack ) {
                clear_authenticated_session();
            }
            stage_ = multiplayer_client_stage::disconnected;
            pending_ping_nonces_.clear();
            if( transport_ ) {
                transport_->stop();
            }
            multiplayer_client_event event;
            event.type = graceful_ack ?
                         multiplayer_client_event_type::gracefully_disconnected :
                         multiplayer_client_event_type::disconnected;
            event.message = notice.message.empty() ?
                            "server requested a graceful disconnect" : notice.message;
            event.rejection = notice.code;
            if( !push_event( std::move( event ) ) ) {
                return fail( "multiplayer client event queue is full", error );
            }
            error.clear();
            return true;
        }
        default:
            return fail( "server sent an unexpected application message", error );
    }
}

bool multiplayer_client::handle_server_hello( const multiplayer_protocol_envelope &envelope,
        const clock::time_point now, std::string &error )
{
    if( !session_is_empty( envelope.session ) || envelope.sequence != 0 ) {
        return fail( "server hello has an invalid session or sequence", error );
    }
    multiplayer_server_hello hello;
    if( !multiplayer_parse_server_hello_payload( envelope, hello, error ) ) {
        return fail( "server hello is invalid: " + error, error );
    }
    if( !hello.accepted ) {
        return fail( "server rejected compatibility handshake: " + hello.message, error );
    }
    if( hello.build_id != settings_.build_id ||
        hello.content_manifest != settings_.content_manifest ||
        hello.server_state_schema != multiplayer_server_state_schema_version ||
        hello.savegame_version != settings_.savegame_version ) {
        return fail( "server accepted a handshake with mismatched compatibility axes", error );
    }
    const auto semantic_scene = std::find_if( hello.capabilities.begin(),
    hello.capabilities.end(), []( const multiplayer_protocol_capability & capability ) {
        return capability.id == "semantic-scene" && capability.version >= 1;
    } );
    if( semantic_scene == hello.capabilities.end() ) {
        return fail( "server accepted a handshake without semantic-scene capability", error );
    }
    return send_authentication( reconnecting_, now, error );
}

bool multiplayer_client::send_authentication( const bool resume, const clock::time_point now,
        std::string &error )
{
    multiplayer_protocol_envelope envelope;
    envelope.sequence = 1;
    if( resume ) {
        multiplayer_resume_request request;
        request.resume_token = resume_token_;
        request.last_server_revision = latest_scene_ ? latest_scene_->server_revision : 0;
        request.last_client_sequence = confirmed_sequence_;
        request.session_generation = session_generation_;
        expected_replay_from_sequence_ = confirmed_sequence_ ==
                                         std::numeric_limits<std::uint64_t>::max() ?
                                         confirmed_sequence_ : confirmed_sequence_ + 1;
        envelope.message_type = multiplayer_protocol_message_type::resume_request;
        if( !multiplayer_build_resume_request_payload( request, envelope.payload, error ) ) {
            return fail( error, error );
        }
    } else {
        multiplayer_authenticate_request request;
        request.display_name = settings_.display_name;
        request.bearer_token = settings_.bearer_token;
        envelope.message_type = multiplayer_protocol_message_type::authenticate;
        if( !multiplayer_build_authenticate_payload( request, envelope.payload, error ) ) {
            return fail( error, error );
        }
    }
    if( !send_envelope( envelope, error ) ) {
        return fail( error, error );
    }
    stage_ = multiplayer_client_stage::awaiting_authentication;
    handshake_deadline_ = now + settings_.handshake_timeout;
    error.clear();
    return true;
}

bool multiplayer_client::handle_authentication_result(
    const multiplayer_protocol_envelope &envelope, const clock::time_point now,
    std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::authentication_result ||
        envelope.sequence != 1 ) {
        return fail( "server authentication response has an invalid envelope", error );
    }
    multiplayer_authentication_result result;
    if( !multiplayer_parse_authentication_result_payload( envelope, result, error ) ) {
        return fail( "server authentication response is invalid: " + error, error );
    }
    if( !result.accepted ) {
        return fail( "server rejected authentication: " + result.message, error );
    }
    if( session_is_empty( envelope.session ) ) {
        return fail( "accepted server authentication response has an empty session", error );
    }
    session_ = envelope.session;
    player_id_ = result.player_id;
    character_id_ = result.character_id;
    resume_token_ = result.resume_token;
    session_generation_ = result.session_generation;
    outbound_sequence_ = 1;
    confirmed_sequence_ = 1;
    expected_replay_from_sequence_ = 0;
    stage_ = multiplayer_client_stage::awaiting_initial_scene;
    handshake_deadline_ = now + settings_.handshake_timeout;
    if( !push_event( { multiplayer_client_event_type::authenticated, {}, {}, {},
                       result.message } ) ) {
        return fail( "multiplayer client event queue is full", error );
    }
    error.clear();
    return true;
}

bool multiplayer_client::handle_resume_result( const multiplayer_protocol_envelope &envelope,
        const clock::time_point now, std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::resume_result ||
        envelope.sequence != 1 ) {
        return fail( "server resume response has an invalid envelope", error );
    }
    multiplayer_resume_result result;
    if( !multiplayer_parse_resume_result_payload( envelope, result, error ) ) {
        return fail( "server resume response is invalid: " + error, error );
    }
    if( !result.accepted ) {
        if( result.rejection == multiplayer_protocol_rejection::session_expired ) {
            clear_authenticated_session();
        }
        return fail( "server rejected session resume: " + result.message, error );
    }
    if( session_is_empty( envelope.session ) ) {
        return fail( "accepted server resume response has an empty session", error );
    }
    if( result.player_id != player_id_ || result.character_id != character_id_ ||
        !multiplayer_is_next_session_generation( session_generation_,
                result.session_generation ) ) {
        return fail( "server resume response changed identity or did not advance generation exactly once",
                     error );
    }
    if( !result.full_snapshot_required ||
        result.replay_from_sequence != expected_replay_from_sequence_ ) {
        return fail( "server resume response has an incompatible replay boundary", error );
    }
    session_ = envelope.session;
    session_generation_ = result.session_generation;
    stage_ = multiplayer_client_stage::awaiting_initial_scene;
    handshake_deadline_ = now + settings_.handshake_timeout;
    if( !push_event( { multiplayer_client_event_type::resumed, {}, {}, {}, result.message } ) ) {
        return fail( "multiplayer client event queue is full", error );
    }
    error.clear();
    return true;
}

bool multiplayer_client::handle_scene( const multiplayer_protocol_envelope &envelope,
                                       std::string &error )
{
    multiplayer_scene_snapshot scene;
    if( !multiplayer_parse_scene_snapshot_payload( envelope, scene, error ) ) {
        return fail( "server scene snapshot is invalid: " + error, error );
    }
    if( envelope.sequence != scene.server_revision ||
        scene.player.player_id != player_id_ || scene.player.character_id != character_id_ ||
        scene.server_revision < minimum_required_scene_revision_ ||
        ( latest_scene_ && scene.server_revision < latest_scene_->server_revision ) ) {
        return fail( "server scene snapshot has an invalid revision or player identity", error );
    }
    const bool initial = stage_ == multiplayer_client_stage::awaiting_initial_scene;
    const bool disconnecting = stage_ == multiplayer_client_stage::disconnecting;
    latest_scene_ = std::move( scene );
    if( !disconnecting ) {
        stage_ = multiplayer_client_stage::ready;
    }
    multiplayer_client_event event;
    event.type = multiplayer_client_event_type::scene;
    if( !push_event( std::move( event ) ) ) {
        return fail( "multiplayer client event queue is full", error );
    }
    if( initial && reconnecting_ && !replay_pending_commands( error ) ) {
        return fail( error, error );
    }
    error.clear();
    return true;
}

bool multiplayer_client::send_envelope( const multiplayer_protocol_envelope &envelope,
                                        std::string &error )
{
    multiplayer_transport_payload encoded;
    if( !multiplayer_encode_protocol_envelope( envelope, encoded, error ) ) {
        return false;
    }
    const multiplayer_transport_send_result result = transport_->send( std::move( encoded ) );
    if( result != multiplayer_transport_send_result::queued ) {
        error = result == multiplayer_transport_send_result::queue_full ?
                "multiplayer client outbound queue is full" :
                "multiplayer client transport is not available";
        return false;
    }
    error.clear();
    return true;
}

bool multiplayer_client::send_command( const multiplayer_command_kind kind,
                                       const std::optional<multiplayer_protocol_direction> direction,
                                       std::uint64_t &sequence, std::string &error )
{
    if( !ready() ) {
        error = "multiplayer client is not ready to send commands";
        return false;
    }
    if( pending_commands_.size() >= settings_.maximum_pending_commands ) {
        error = "too many multiplayer commands are awaiting confirmation";
        return false;
    }
    if( outbound_sequence_ == std::numeric_limits<std::uint64_t>::max() ) {
        error = "multiplayer client sequence is exhausted";
        return false;
    }
    const std::uint64_t previous_sequence = outbound_sequence_;
    multiplayer_player_command command;
    command.client_sequence = ++outbound_sequence_;
    command.base_revision = latest_scene_->server_revision;
    command.kind = kind;
    command.direction = direction;

    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::player_command;
    envelope.session = session_;
    envelope.sequence = command.client_sequence;
    if( !multiplayer_build_player_command_payload( command, envelope.payload, error ) ) {
        outbound_sequence_ = previous_sequence;
        update_resume_sequence_floor();
        return false;
    }
    pending_command pending;
    pending.payload = envelope.payload;
    pending.base_revision = command.base_revision;
    pending_commands_.emplace( command.client_sequence, std::move( pending ) );
    update_resume_sequence_floor();
    if( !send_envelope( envelope, error ) ) {
        pending_commands_.erase( command.client_sequence );
        outbound_sequence_ = previous_sequence;
        update_resume_sequence_floor();
        return false;
    }
    sequence = command.client_sequence;
    error.clear();
    return true;
}

bool multiplayer_client::send_ping( const std::uint64_t nonce,
                                    const std::uint64_t monotonic_milliseconds,
                                    std::string &error )
{
    if( stage_ != multiplayer_client_stage::awaiting_initial_scene &&
        stage_ != multiplayer_client_stage::ready ) {
        error = "multiplayer client is not ready to send ping";
        return false;
    }
    if( outbound_sequence_ == std::numeric_limits<std::uint64_t>::max() ) {
        error = "multiplayer client sequence is exhausted";
        return false;
    }
    if( pending_ping_nonces_.size() >= settings_.maximum_pending_commands ) {
        error = "too many multiplayer pings are awaiting confirmation";
        return false;
    }
    const std::uint64_t previous_sequence = outbound_sequence_;
    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::ping;
    envelope.session = session_;
    envelope.sequence = ++outbound_sequence_;
    if( !multiplayer_build_ping_payload( { nonce, monotonic_milliseconds }, envelope.payload,
                                         error ) ) {
        outbound_sequence_ = previous_sequence;
        update_resume_sequence_floor();
        return false;
    }
    pending_ping_nonces_.emplace( envelope.sequence, nonce );
    if( !send_envelope( envelope, error ) ) {
        pending_ping_nonces_.erase( envelope.sequence );
        outbound_sequence_ = previous_sequence;
        update_resume_sequence_floor();
        return false;
    }
    update_resume_sequence_floor();
    return true;
}

bool multiplayer_client::request_resync( std::string reason, std::string &error )
{
    if( stage_ != multiplayer_client_stage::ready || !latest_scene_ ) {
        error = "multiplayer client is not ready to request resynchronization";
        return false;
    }
    if( outbound_sequence_ == std::numeric_limits<std::uint64_t>::max() ) {
        error = "multiplayer client sequence is exhausted";
        return false;
    }
    const std::uint64_t previous_sequence = outbound_sequence_;
    multiplayer_resync_request request;
    request.client_revision = latest_scene_->server_revision;
    request.reason = std::move( reason );
    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::resync_request;
    envelope.session = session_;
    envelope.sequence = ++outbound_sequence_;
    if( !multiplayer_build_resync_request_payload( request, envelope.payload, error ) ) {
        outbound_sequence_ = previous_sequence;
        update_resume_sequence_floor();
        return false;
    }
    if( !send_envelope( envelope, error ) ) {
        outbound_sequence_ = previous_sequence;
        update_resume_sequence_floor();
        return false;
    }
    update_resume_sequence_floor();
    return true;
}

bool multiplayer_client::request_graceful_disconnect( std::string &error )
{
    if( ( stage_ != multiplayer_client_stage::awaiting_initial_scene &&
          stage_ != multiplayer_client_stage::ready ) || session_is_empty( session_ ) ) {
        error = "multiplayer client is not ready for a graceful disconnect";
        return false;
    }
    if( !pending_commands_.empty() ) {
        error = "multiplayer client cannot disconnect while a command is unconfirmed";
        return false;
    }
    if( outbound_sequence_ == std::numeric_limits<std::uint64_t>::max() ) {
        error = "multiplayer client sequence is exhausted";
        return false;
    }
    const std::uint64_t previous_sequence = outbound_sequence_;
    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::disconnect_notice;
    envelope.session = session_;
    envelope.sequence = ++outbound_sequence_;
    if( !multiplayer_build_disconnect_notice_payload(
{ multiplayer_protocol_rejection::none, "client requested graceful disconnect" },
envelope.payload, error ) ) {
        outbound_sequence_ = previous_sequence;
        update_resume_sequence_floor();
        return false;
    }
    if( !send_envelope( envelope, error ) ) {
        outbound_sequence_ = previous_sequence;
        update_resume_sequence_floor();
        return false;
    }
    update_resume_sequence_floor();
    graceful_disconnect_sequence_ = envelope.sequence;
    stage_ = multiplayer_client_stage::disconnecting;
    error.clear();
    return true;
}

bool multiplayer_client::mark_transport_unresponsive( std::string reason, std::string &error )
{
    if( stage_ == multiplayer_client_stage::stopped ||
        stage_ == multiplayer_client_stage::disconnected ) {
        error = "multiplayer client connection is not active";
        return false;
    }
    if( reason.empty() ) {
        reason = "multiplayer server heartbeat timed out";
    }
    if( transport_ ) {
        transport_->stop();
    }
    stage_ = multiplayer_client_stage::disconnected;
    pending_ping_nonces_.clear();
    if( !push_event( { multiplayer_client_event_type::disconnected, {}, {}, {}, reason } ) ) {
        return fail( "multiplayer client event queue is full", error );
    }
    error.clear();
    return true;
}

bool multiplayer_client::replay_pending_commands( std::string &error )
{
    for( const auto &entry : pending_commands_ ) {
        if( entry.first < expected_replay_from_sequence_ ) {
            error = "pending command falls below the server replay boundary";
            return false;
        }
        multiplayer_protocol_envelope envelope;
        envelope.message_type = multiplayer_protocol_message_type::player_command;
        envelope.session = session_;
        envelope.sequence = entry.first;
        envelope.payload = entry.second.payload;
        if( !send_envelope( envelope, error ) ) {
            return false;
        }
    }
    expected_replay_from_sequence_ = 0;
    error.clear();
    return true;
}

bool multiplayer_client::fail( std::string message, std::string &error )
{
    stage_ = multiplayer_client_stage::failed;
    pending_ping_nonces_.clear();
    if( transport_ ) {
        transport_->stop();
    }
    terminal_error_ = std::move( message );
    error = terminal_error_;
    if( events_.size() >= settings_.maximum_events ) {
        events_.pop_front();
    }
    events_.push_back( { multiplayer_client_event_type::error, {}, {}, {}, error } );
    return false;
}

bool multiplayer_client::push_event( multiplayer_client_event event )
{
    if( events_.size() >= settings_.maximum_events ) {
        return false;
    }
    events_.emplace_back( std::move( event ) );
    return true;
}

void multiplayer_client::clear_authenticated_session()
{
    session_ = {};
    player_id_.clear();
    character_id_.clear();
    resume_token_.clear();
    session_generation_ = 0;
    outbound_sequence_ = 0;
    confirmed_sequence_ = 0;
    expected_replay_from_sequence_ = 0;
    minimum_required_scene_revision_ = 0;
    graceful_disconnect_sequence_ = 0;
    pending_commands_.clear();
    pending_ping_nonces_.clear();
    latest_scene_.reset();
}

void multiplayer_client::update_resume_sequence_floor()
{
    confirmed_sequence_ = pending_commands_.empty() ? outbound_sequence_ :
                          pending_commands_.begin()->first - 1;
}

void multiplayer_client::stop()
{
    if( transport_ ) {
        transport_->stop();
        transport_.reset();
    }
    stage_ = multiplayer_client_stage::stopped;
}

multiplayer_client_stage multiplayer_client::stage() const
{
    return stage_;
}

bool multiplayer_client::ready() const
{
    return stage_ == multiplayer_client_stage::ready && latest_scene_ &&
           latest_scene_->server_revision >= minimum_required_scene_revision_;
}

std::optional<multiplayer_client_event> multiplayer_client::poll_event()
{
    if( events_.empty() ) {
        return std::nullopt;
    }
    multiplayer_client_event event = std::move( events_.front() );
    events_.pop_front();
    return event;
}

const std::optional<multiplayer_scene_snapshot> &multiplayer_client::latest_scene() const
{
    return latest_scene_;
}

const std::string &multiplayer_client::player_id() const
{
    return player_id_;
}

const std::string &multiplayer_client::character_id() const
{
    return character_id_;
}

std::uint64_t multiplayer_client::session_generation() const
{
    return session_generation_;
}

std::uint64_t multiplayer_client::last_confirmed_client_sequence() const
{
    return confirmed_sequence_;
}

std::size_t multiplayer_client::pending_command_count() const
{
    return pending_commands_.size();
}

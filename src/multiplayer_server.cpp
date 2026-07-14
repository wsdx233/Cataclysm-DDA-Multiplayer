#include "multiplayer_server.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "game.h"
#include "mod_manager.h"
#include "multiplayer_content_manifest.h"
#include "multiplayer_protocol.h"
#include "path_info.h"
#include "type_id.h"

namespace
{

constexpr std::size_t maximum_server_events = 1024;

multiplayer_server_transport_settings server_transport_settings(
    const multiplayer_server_config &config )
{
    multiplayer_server_transport_settings settings;
    settings.maximum_connections = static_cast<std::size_t>( config.players.maximum ) + 4;
    return settings;
}

} // namespace

bool multiplayer_server_content_manifest( const multiplayer_server_config &config,
        std::string &manifest, multiplayer_content_manifest_stats &stats,
        std::string &error )
{
    std::vector<multiplayer_content_root> roots;
    roots.reserve( config.world.mods.size() + 1 );
    // Core definitions are loaded independently of the selected mod order and can change
    // authoritative rules (weather, damage indicators, migrations, and external options).
    roots.push_back( { "core-engine", PATH_INFO::jsondir().get_unrelative_path() } );
    for( const std::string &configured_mod : config.world.mods ) {
        const mod_id id( configured_mod );
        if( !id.is_valid() ) {
            error = "configured multiplayer mod is unavailable while building content manifest: " +
                    configured_mod;
            return false;
        }
        const MOD_INFORMATION &information = id.obj();
        roots.push_back( { "mod-" + configured_mod,
                           information.path.get_unrelative_path() } );
    }
    return multiplayer_build_content_manifest( roots, manifest, stats, error );
}

multiplayer_dedicated_server::multiplayer_dedicated_server(
    multiplayer_server_config config, std::filesystem::path config_path,
    std::string build_id, std::string content_manifest_override,
    std::optional<multiplayer_server_player_identity> fixed_player_identity ) :
    config_( std::move( config ) ),
    config_path_( std::move( config_path ) ),
    build_id_( std::move( build_id ) ),
    content_manifest_override_( std::move( content_manifest_override ) ),
    fixed_player_identity_( std::move( fixed_player_identity ) ),
    transport_( server_transport_settings( config_ ) )
{
}

multiplayer_dedicated_server::~multiplayer_dedicated_server()
{
    stop();
}

bool multiplayer_dedicated_server::start( std::string &error )
{
    if( running_ ) {
        error = "dedicated server is already running";
        return false;
    }
    if( const std::optional<std::string> validation_error =
            validate_multiplayer_server_config( config_ ) ) {
        error = *validation_error;
        return false;
    }
    multiplayer_server_listen_endpoint parsed_endpoint;
    if( const std::optional<std::string> endpoint_error =
            parse_multiplayer_server_listen_endpoint( config_.network.listen, parsed_endpoint ) ) {
        error = *endpoint_error;
        return false;
    }
    std::string bearer_token;
    if( !load_multiplayer_server_bearer_token( config_path_, config_, bearer_token, error ) ) {
        return false;
    }

    std::string content_manifest = content_manifest_override_;
    if( content_manifest.empty() ) {
        multiplayer_content_manifest_stats stats;
        if( !multiplayer_server_content_manifest( config_, content_manifest, stats, error ) ) {
            return false;
        }
    }

    multiplayer_server_lobby_settings lobby_settings;
    lobby_settings.server_build_id = build_id_;
    lobby_settings.world_id = config_.world.name;
    lobby_settings.content_manifest = std::move( content_manifest );
    lobby_settings.savegame_version = savegame_version;
    lobby_settings.bearer_token = std::move( bearer_token );
    if( fixed_player_identity_ ) {
        lobby_settings.fixed_player_identity = std::make_pair(
                fixed_player_identity_->player_id, fixed_player_identity_->character_id );
    }
    lobby_settings.maximum_players = static_cast<std::size_t>( config_.players.maximum );
    lobby_settings.maximum_attempts_per_minute =
        config_.authentication.maximum_attempts_per_minute;
    lobby_settings.handshake_timeout =
        std::chrono::milliseconds( config_.network.handshake_timeout_ms );
    lobby_settings.resume_token_lifetime =
        std::chrono::seconds( config_.authentication.resume_token_seconds );
    lobby_ = std::make_unique<multiplayer_server_lobby>( std::move( lobby_settings ) );
    if( !lobby_->valid( error ) ) {
        lobby_.reset();
        return false;
    }

    multiplayer_transport_endpoint endpoint;
    endpoint.host = std::move( parsed_endpoint.host );
    endpoint.port = parsed_endpoint.port;
    if( !transport_.start( endpoint, error ) ) {
        lobby_.reset();
        return false;
    }
    running_ = true;
    error.clear();
    return true;
}

bool multiplayer_dedicated_server::poll_once( const clock::time_point now, std::string &error )
{
    if( !running_ || !lobby_ ) {
        error = "dedicated server is not running";
        return false;
    }
    while( std::optional<multiplayer_transport_event> transport_event = transport_.poll_event() ) {
        if( transport_event->type == multiplayer_transport_event_type::transport_error &&
            transport_event->connection == 0 ) {
            error = transport_event->detail;
            stop();
            return false;
        }
        if( !execute_actions( lobby_->handle_transport_event( *transport_event, now ), error ) ) {
            return false;
        }
    }
    if( !execute_actions( lobby_->tick( now ), error ) ) {
        return false;
    }
    while( std::optional<multiplayer_server_lobby_event> lobby_event = lobby_->poll_event() ) {
        if( !process_lobby_event( std::move( *lobby_event ), now, error ) ) {
            return false;
        }
    }
    error.clear();
    return true;
}

bool multiplayer_dedicated_server::execute_actions(
    std::vector<multiplayer_server_lobby_action> actions, std::string &error )
{
    for( multiplayer_server_lobby_action &action : actions ) {
        if( action.type == multiplayer_server_lobby_action_type::disconnect ) {
            transport_.disconnect( action.connection, std::move( action.reason ) );
            continue;
        }
        if( action.type == multiplayer_server_lobby_action_type::send_and_disconnect ) {
            transport_.send_and_disconnect( action.connection, std::move( action.payload ),
                                            std::move( action.reason ) );
            continue;
        }
        const multiplayer_transport_send_result result = transport_.send(
                    action.connection, std::move( action.payload ) );
        if( result != multiplayer_transport_send_result::queued ) {
            transport_.disconnect( action.connection, "server outbound queue rejected a message" );
        }
    }
    if( !transport_.running() ) {
        error = transport_.failure_detail();
        if( error.empty() ) {
            error = "dedicated server transport stopped unexpectedly";
        }
        running_ = false;
        return false;
    }
    return true;
}

bool multiplayer_dedicated_server::process_lobby_event(
    multiplayer_server_lobby_event event, const clock::time_point now, std::string &error )
{
    if( event.type == multiplayer_server_lobby_event_type::application_message &&
        event.message.message_type == multiplayer_protocol_message_type::ping ) {
        multiplayer_protocol_heartbeat ping;
        if( !multiplayer_parse_ping_payload( event.message, ping, error ) ) {
            transport_.disconnect( event.connection, "invalid ping payload" );
            return true;
        }
        multiplayer_protocol_heartbeat pong;
        pong.nonce = ping.nonce;
        pong.monotonic_milliseconds = static_cast<std::uint64_t>(
                                          std::chrono::duration_cast<std::chrono::milliseconds>(
                                              now.time_since_epoch() ).count() );
        multiplayer_protocol_envelope response;
        response.message_type = multiplayer_protocol_message_type::pong;
        response.session = event.message.session;
        response.sequence = event.message.sequence;
        if( !multiplayer_build_pong_payload( pong, response.payload, error ) ) {
            return false;
        }
        multiplayer_transport_payload encoded;
        if( !multiplayer_encode_protocol_envelope( response, encoded, error ) ) {
            return false;
        }
        if( transport_.send( event.connection, std::move( encoded ) ) !=
            multiplayer_transport_send_result::queued ) {
            transport_.disconnect( event.connection, "server outbound queue rejected pong" );
        }
        return true;
    }
    if( events_.size() >= maximum_server_events ) {
        transport_.disconnect( event.connection, "server simulation event queue is full" );
        return true;
    }
    events_.emplace_back( std::move( event ) );
    return true;
}

void multiplayer_dedicated_server::stop()
{
    if( running_ ) {
        running_ = false;
        transport_.stop();
    }
    lobby_.reset();
}

bool multiplayer_dedicated_server::running() const
{
    return running_;
}

std::uint16_t multiplayer_dedicated_server::bound_port() const
{
    return transport_.bound_port();
}

bool multiplayer_dedicated_server::send( const multiplayer_connection_id connection,
        const multiplayer_protocol_envelope &envelope, std::string &error )
{
    if( !running_ ) {
        error = "dedicated server is not running";
        return false;
    }
    multiplayer_transport_payload encoded;
    if( !multiplayer_encode_protocol_envelope( envelope, encoded, error ) ) {
        return false;
    }
    if( transport_.send( connection, std::move( encoded ) ) !=
        multiplayer_transport_send_result::queued ) {
        error = "dedicated server outbound queue rejected a protocol message";
        return false;
    }
    error.clear();
    return true;
}

bool multiplayer_dedicated_server::complete_graceful_disconnect(
    const multiplayer_server_lobby_event &request, bool &completed, std::string &error )
{
    completed = false;
    if( !running_ || !lobby_ ) {
        error = "dedicated server is not running";
        return false;
    }
    std::vector<multiplayer_server_lobby_action> actions =
        lobby_->complete_graceful_disconnect( request );
    completed = actions.size() == 1 &&
                actions.front().type ==
                multiplayer_server_lobby_action_type::send_and_disconnect;
    if( !execute_actions( std::move( actions ), error ) ) {
        completed = false;
        return false;
    }
    error.clear();
    return true;
}

void multiplayer_dedicated_server::disconnect( const multiplayer_connection_id connection,
        std::string reason )
{
    transport_.disconnect( connection, std::move( reason ) );
}

std::optional<multiplayer_server_lobby_event> multiplayer_dedicated_server::poll_event()
{
    if( events_.empty() ) {
        return std::nullopt;
    }
    multiplayer_server_lobby_event event = std::move( events_.front() );
    events_.pop_front();
    return event;
}

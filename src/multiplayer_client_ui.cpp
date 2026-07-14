#include "multiplayer_client_ui.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "game.h"
#include "input_context.h"
#include "mapdata.h"
#include "mtype.h"
#include "output.h"
#include "trap.h"
#include "translations.h"

namespace
{

catacurses::window remote_scene_window;
const multiplayer_scene_snapshot *remote_scene = nullptr;

point scene_screen_position( const multiplayer_protocol_position &position,
                             const multiplayer_scene_snapshot &scene,
                             const int width, const int height )
{
    return point( width / 2 + position.x - scene.player.position.x,
                  height / 2 + position.y - scene.player.position.y );
}

bool in_window( const point &position, const int width, const int height )
{
    return position.x >= 0 && position.y >= 0 && position.x < width && position.y < height;
}

void draw_remote_scene_curses( const catacurses::window &window,
                               const multiplayer_scene_snapshot &scene )
{
    const int width = getmaxx( window );
    const int height = getmaxy( window );
    werase( window );
    for( const multiplayer_visible_tile &tile : scene.tiles ) {
        if( tile.position.z != scene.player.position.z ) {
            continue;
        }
        const point screen = scene_screen_position( tile.position, scene, width, height );
        if( !in_window( screen, width, height ) ) {
            continue;
        }
        uint32_t symbol = '?';
        nc_color color = c_dark_gray;
        const ter_str_id terrain( tile.terrain_id );
        if( terrain.is_valid() ) {
            symbol = terrain->symbol();
            color = terrain->color();
        }
        if( !tile.furniture_id.empty() ) {
            const furn_str_id furniture( tile.furniture_id );
            if( furniture.is_valid() ) {
                symbol = furniture->symbol();
                color = furniture->color();
            }
        }
        if( !tile.visible_trap_id.empty() ) {
            const trap_str_id visible_trap( tile.visible_trap_id );
            if( visible_trap.is_valid() ) {
                symbol = visible_trap->sym;
                color = visible_trap->color;
            }
        }
        mvwputch( window, screen, color, symbol );
    }
    for( const multiplayer_visible_entity &entity : scene.entities ) {
        if( entity.position.z != scene.player.position.z ) {
            continue;
        }
        const point screen = scene_screen_position( entity.position, scene, width, height );
        if( !in_window( screen, width, height ) ) {
            continue;
        }
        if( entity.kind == multiplayer_visible_entity_kind::player ) {
            mvwputch( window, screen, c_white, '@' );
        } else if( entity.kind == multiplayer_visible_entity_kind::monster ) {
            const mtype_id type( entity.appearance_id );
            if( type.is_valid() ) {
                mvwputch( window, screen, type->color, UTF8_getch( type->sym ) );
            } else {
                mvwputch( window, screen, c_light_red, 'M' );
            }
        }
    }
    wnoutrefresh( window );
}

std::string client_stage_text( const multiplayer_client_stage stage )
{
    switch( stage ) {
        case multiplayer_client_stage::stopped:
            return _( "stopped" );
        case multiplayer_client_stage::connecting:
            return _( "connecting" );
        case multiplayer_client_stage::awaiting_server_hello:
            return _( "negotiating" );
        case multiplayer_client_stage::awaiting_authentication:
            return _( "authenticating" );
        case multiplayer_client_stage::awaiting_initial_scene:
            return _( "synchronizing" );
        case multiplayer_client_stage::ready:
            return _( "connected" );
        case multiplayer_client_stage::disconnecting:
            return _( "disconnecting" );
        case multiplayer_client_stage::disconnected:
            return _( "disconnected" );
        case multiplayer_client_stage::failed:
            return _( "failed" );
    }
    return _( "unknown" );
}

} // namespace

bool multiplayer_client_is_remote_scene_window( const catacurses::window &window )
{
    return remote_scene_window && window == remote_scene_window;
}

const multiplayer_scene_snapshot *multiplayer_client_scene_for_render()
{
    return remote_scene;
}

int run_multiplayer_client_ui( multiplayer_client_settings settings, std::string &error )
{
    constexpr std::chrono::seconds heartbeat_interval( 30 );
    // Pong generation currently shares the simulation-thread poll boundary.  Leave ample
    // room for a long world phase or save while still detecting mobile half-open sockets.
    constexpr std::chrono::seconds heartbeat_timeout( 120 );
    multiplayer_client client( std::move( settings ) );
    if( !client.start( error ) ) {
        return 1;
    }

    int status_height = 0;
    int scene_height = 0;
    int window_width = 0;
    catacurses::window status_window;
    const catacurses::window previous_terrain_window = g->w_terrain;
    const auto resize_windows = [&]() {
        const int desired_status_height = std::min( 2, std::max( 1, TERMY ) );
        const int desired_scene_height = std::max( 1, TERMY - desired_status_height );
        const int desired_width = std::max( 1, TERMX );
        if( remote_scene_window && status_window &&
            desired_status_height == status_height &&
            desired_scene_height == scene_height && desired_width == window_width ) {
            return;
        }
        status_height = desired_status_height;
        scene_height = desired_scene_height;
        window_width = desired_width;
        remote_scene_window = catacurses::newwin( scene_height, window_width, point::zero );
        status_window = catacurses::newwin( status_height, window_width,
                                            point( 0, scene_height ) );
        g->w_terrain = remote_scene_window;
    };
    resize_windows();

    input_context input( "DEFAULTMODE" );
    input.set_iso( true );
    input.register_directions();
    input.register_action( "pause", to_translation( "Wait one authoritative turn" ) );
    input.register_action( "QUIT", to_translation( "Disconnect and return" ) );
    input.register_action( "CONFIRM", to_translation( "Reconnect after disconnect" ) );
    input.register_action( "HELP_KEYBINDINGS" );

    std::string status_message = _( "Connecting to multiplayer server…" );
    multiplayer_client::clock::time_point next_heartbeat =
        multiplayer_client::clock::now() + heartbeat_interval;
    std::optional<multiplayer_client::clock::time_point> heartbeat_deadline;
    std::uint64_t heartbeat_nonce = 1;
    bool quit = false;
    bool graceful_disconnect_pending = false;
    bool graceful_disconnect_requested = false;
    int result = 0;
    while( !quit ) {
        resize_windows();
        const multiplayer_client::clock::time_point now = multiplayer_client::clock::now();
        std::string poll_error;
        if( !client.poll_once( now, poll_error ) ) {
            status_message = poll_error;
            result = 1;
        }
        while( std::optional<multiplayer_client_event> event = client.poll_event() ) {
            switch( event->type ) {
                case multiplayer_client_event_type::authenticated:
                    status_message = _( "Authenticated; waiting for scene." );
                    heartbeat_deadline.reset();
                    next_heartbeat = now + heartbeat_interval;
                    break;
                case multiplayer_client_event_type::resumed:
                    status_message = _( "Session resumed; replaying unconfirmed command." );
                    heartbeat_deadline.reset();
                    next_heartbeat = now + heartbeat_interval;
                    break;
                case multiplayer_client_event_type::scene:
                    status_message = _( "Authoritative scene synchronized." );
                    break;
                case multiplayer_client_event_type::command_result:
                    if( event->command_result ) {
                        status_message = event->command_result->message.empty() ?
                                         _( "Command confirmed by server." ) :
                                         event->command_result->message;
                    }
                    break;
                case multiplayer_client_event_type::pong:
                    status_message = _( "Server connection is responsive." );
                    heartbeat_deadline.reset();
                    next_heartbeat = now + heartbeat_interval;
                    break;
                case multiplayer_client_event_type::gracefully_disconnected:
                    status_message = event->message;
                    quit = true;
                    break;
                case multiplayer_client_event_type::disconnected:
                    if( graceful_disconnect_requested ) {
                        graceful_disconnect_requested = false;
                        graceful_disconnect_pending = true;
                    }
                    status_message = event->message;
                    heartbeat_deadline.reset();
                    break;
                case multiplayer_client_event_type::error:
                    status_message = event->message;
                    heartbeat_deadline.reset();
                    break;
            }
        }

        const bool authenticated_session =
            client.stage() == multiplayer_client_stage::awaiting_initial_scene ||
            client.stage() == multiplayer_client_stage::ready;
        if( graceful_disconnect_pending && authenticated_session &&
            client.pending_command_count() == 0 ) {
            if( client.request_graceful_disconnect( error ) ) {
                graceful_disconnect_pending = false;
                graceful_disconnect_requested = true;
                status_message = _( "Waiting for the server to release this session…" );
            } else {
                status_message = error;
                result = 1;
            }
        }

        if( authenticated_session && heartbeat_deadline && now >= *heartbeat_deadline ) {
            if( !client.mark_transport_unresponsive(
                    "multiplayer server heartbeat timed out", error ) ) {
                status_message = error;
                result = 1;
            }
            heartbeat_deadline.reset();
        } else if( authenticated_session && !heartbeat_deadline && now >= next_heartbeat ) {
            const std::uint64_t milliseconds = static_cast<std::uint64_t>(
                                                   std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           now.time_since_epoch() ).count() );
            if( client.send_ping( heartbeat_nonce++, milliseconds, error ) ) {
                heartbeat_deadline = now + heartbeat_timeout;
            } else {
                status_message = error;
                next_heartbeat = now + heartbeat_interval;
            }
        }

        remote_scene = client.latest_scene() ? &*client.latest_scene() : nullptr;
        if( remote_scene != nullptr ) {
            draw_remote_scene_curses( remote_scene_window, *remote_scene );
        } else {
            werase( remote_scene_window );
            mvwprintz( remote_scene_window, point::zero, c_light_gray,
                       _( "Waiting for the server's visibility-filtered scene…" ) );
            wnoutrefresh( remote_scene_window );
        }
        werase( status_window );
        mvwprintz( status_window, point::zero, c_light_green, "%s: %s",
                   _( "Multiplayer" ), client_stage_text( client.stage() ) );
        if( status_height > 1 ) {
            mvwprintz( status_window, point( 0, 1 ), c_light_gray, "%s  %s",
                       status_message,
                       client.stage() == multiplayer_client_stage::disconnected ?
                       _( "Press confirm to reconnect, or quit." ) :
                       _( "Move: local direction keys/touch. Wait: pause. Quit: quit." ) );
        }
        wnoutrefresh( status_window );
        catacurses::doupdate();

        const std::string action = input.handle_input( 50 );
        if( action == "QUIT" ) {
            if( graceful_disconnect_requested || !authenticated_session ) {
                quit = true;
            } else if( client.pending_command_count() != 0 ) {
                graceful_disconnect_pending = true;
                status_message = _( "Waiting for the pending command before disconnecting…" );
            } else if( client.request_graceful_disconnect( error ) ) {
                graceful_disconnect_requested = true;
                status_message = _( "Waiting for the server to release this session…" );
            } else {
                status_message = error;
                result = 1;
            }
            continue;
        }
        if( action == "HELP_KEYBINDINGS" ) {
            input.display_menu();
            continue;
        }
        if( action == "CONFIRM" &&
            ( client.stage() == multiplayer_client_stage::disconnected ||
              client.stage() == multiplayer_client_stage::failed ) ) {
            if( !client.reconnect( error ) ) {
                status_message = error;
                result = 1;
            } else {
                status_message = _( "Reconnecting to multiplayer server…" );
                result = 0;
            }
            continue;
        }
        if( !client.ready() || client.pending_command_count() != 0 ) {
            continue;
        }

        std::optional<multiplayer_protocol_direction> direction;
        if( const std::optional<tripoint_rel_ms> delta = input.get_direction_rel_ms( action ) ) {
            direction = multiplayer_protocol_direction{
                static_cast<std::int8_t>( delta->x() ),
                static_cast<std::int8_t>( delta->y() ), 0
            };
        }
        multiplayer_command_kind kind = multiplayer_command_kind::none;
        if( direction ) {
            kind = multiplayer_command_kind::move;
        } else if( action == "pause" ) {
            kind = multiplayer_command_kind::wait;
        }
        if( kind != multiplayer_command_kind::none ) {
            std::uint64_t sequence = 0;
            if( !client.send_command( kind, direction, sequence, error ) ) {
                status_message = error;
                result = 1;
            } else {
                status_message = string_format(
                                     _( "Command %llu sent; awaiting server result." ),
                                     static_cast<unsigned long long>( sequence ) );
            }
        }
    }

    remote_scene = nullptr;
    remote_scene_window = {};
    g->w_terrain = previous_terrain_window;
    client.stop();
    if( result == 0 ) {
        error.clear();
    } else if( error.empty() ) {
        error = status_message;
    }
    return result;
}

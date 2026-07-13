#include "multiplayer_command_executor.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "avatar.h"
#include "avatar_action.h"
#include "creature_tracker.h"
#include "game.h"
#include "map.h"
#include "npc.h"

bool multiplayer_execute_wait( game &simulation, avatar &player,
                               const bool repeat_safe_mode_warnings )
{
    if( !simulation.is_simulation_thread() || &simulation.active_avatar() != &player ||
        !simulation.check_safe_mode_allowed( repeat_safe_mode_warnings ) ) {
        return false;
    }
    player.pause();
    return true;
}

bool multiplayer_execute_move( game &simulation, avatar &player, map &here,
                               const tripoint_rel_ms &direction,
                               const multiplayer_command_execution_context context )
{
    if( !simulation.is_simulation_thread() || &simulation.active_avatar() != &player ||
        direction.z() != 0 || std::abs( direction.x() ) > 1 ||
        std::abs( direction.y() ) > 1 || direction == tripoint_rel_ms::zero ) {
        return false;
    }
    if( context == multiplayer_command_execution_context::remote_headless ) {
        if( !simulation.check_safe_mode_allowed( true ) ) {
            return false;
        }
        if( player.maybe_get_value( "remote_controlling" ) ||
            player.controlling_vehicle || simulation.remoteveh() != nullptr ) {
            return false;
        }
        const tripoint_bub_ms destination = player.pos_bub() + direction;
        if( npc *other = get_creature_tracker().creature_at<npc>( destination );
            other != nullptr && !other->is_enemy() ) {
            // Friendly NPC movement opens an interactive menu in the legacy path.
            // Phase 2 remote commands must reject it rather than block on server UI.
            return false;
        }
    }
    return avatar_action::move(
               player, here, direction,
               context == multiplayer_command_execution_context::local_interactive );
}

multiplayer_command_execution multiplayer_execute_basic_command(
    game &simulation, avatar &player, const multiplayer_player_command &command )
{
    multiplayer_command_execution result;
    if( !simulation.is_simulation_thread() || &simulation.active_avatar() != &player ) {
        result.rejection = multiplayer_protocol_rejection::invalid_state;
        result.message = "player command is outside the active simulation context";
        return result;
    }

    const int moves_before = player.get_moves();
    bool handled = false;
    switch( command.kind ) {
        case multiplayer_command_kind::wait:
            if( command.direction ) {
                result.message = "wait command must not include a direction";
                return result;
            }
            handled = multiplayer_execute_wait( simulation, player, true );
            if( !handled ) {
                result.rejection = multiplayer_protocol_rejection::permission_denied;
                result.message = "safe mode rejected the wait command";
                return result;
            }
            break;
        case multiplayer_command_kind::move:
            if( !command.direction ) {
                result.message = "move command requires a direction";
                return result;
            }
            handled = multiplayer_execute_move(
                          simulation, player, get_map(),
                          tripoint_rel_ms( command.direction->dx, command.direction->dy,
                                           command.direction->dz ),
                          multiplayer_command_execution_context::remote_headless );
            break;
        case multiplayer_command_kind::none:
            result.message = "command kind is not executable";
            return result;
    }

    result.moves_spent = std::max( 0, moves_before - player.get_moves() );
    result.action_taken = handled || result.moves_spent > 0;
    if( !result.action_taken ) {
        result.rejection = multiplayer_protocol_rejection::invalid_command;
        result.message = "command did not produce a valid game action";
        return result;
    }
    result.status = multiplayer_command_status::accepted;
    result.rejection = multiplayer_protocol_rejection::none;
    result.message.clear();
    return result;
}

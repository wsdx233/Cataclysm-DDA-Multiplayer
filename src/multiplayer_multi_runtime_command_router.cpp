#include "multiplayer_multi_runtime_command_router.h"

#include <cstdlib>
#include <string>

#include "avatar.h"
#include "creature_tracker.h"
#include "field.h"
#include "game.h"
#include "map.h"
#include "mapdata.h"
#include "trap.h"
#include "type_id.h"

namespace
{

static const json_character_flag json_flag_cannot_move( "CANNOT_MOVE" );
static const json_character_flag json_flag_nyctophobia( "NYCTOPHOBIA" );
static const json_character_flag json_flag_phase_movement( "PHASE_MOVEMENT" );

static const trait_id trait_shell2( "SHELL2" );
static const trait_id trait_shell3( "SHELL3" );

bool is_valid_adjacent_direction( const tripoint_rel_ms &direction )
{
    return direction.z() == 0 && std::abs( direction.x() ) <= 1 &&
           std::abs( direction.y() ) <= 1 && direction != tripoint_rel_ms::zero;
}

bool is_verified_plain_multi_runtime_move( game &simulation, avatar &player,
        map &here, const tripoint_rel_ms &direction )
{
    if( !is_valid_adjacent_direction( direction ) ) {
        return false;
    }

    const tripoint_bub_ms source = player.pos_bub( here );
    const tripoint_bub_ms destination = source + direction;
    if( !here.inbounds( destination ) || !here.passable_through( destination ) ||
        !here.valid_move( source, destination ) ) {
        return false;
    }

    if( player.in_vehicle || player.controlling_vehicle || player.is_underwater() ||
        player.is_mounted() || player.mounted_creature != nullptr ||
        player.is_hauling() || player.get_grab_type() != object_type::NONE ||
        player.grab_point != tripoint_rel_ms::zero ||
        player.maybe_get_value( "remote_controlling" ) ||
        player.maybe_get_value( "remote_controlling_vehicle" ) ||
        !player.activity.is_null() || player.is_auto_moving() ||
        player.has_destination() || !player.get_effects().empty() ||
        !player.enough_working_legs() ||
        player.has_active_mutation( trait_shell2 ) ||
        player.has_active_mutation( trait_shell3 ) ||
        player.has_flag( json_flag_cannot_move ) ||
        player.has_flag( json_flag_nyctophobia ) ||
        player.has_flag( json_flag_phase_movement ) ) {
        return false;
    }

    if( here.veh_at( source ) || here.veh_at( destination ) ||
        get_creature_tracker().creature_at<Creature>( destination, true ) != nullptr ||
        here.field_at( destination ).field_count() != 0 ||
        !here.tr_at( destination ).is_null() || here.has_furn( destination ) ||
        here.has_items( destination ) || here.is_open_air( destination ) ||
        here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, source ) ||
        here.has_flag( ter_furn_flag::TFLAG_DEEP_WATER, source ) ||
        here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, destination ) ||
        here.has_flag( ter_furn_flag::TFLAG_DEEP_WATER, destination ) ||
        here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, destination ) ||
        here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, destination ) ||
        here.has_flag( ter_furn_flag::TFLAG_ROUGH, destination ) ||
        here.has_flag( ter_furn_flag::TFLAG_SHARP, destination ) ||
        here.has_flag( ter_furn_flag::TFLAG_UNSTABLE, destination ) ||
        here.has_flag( ter_furn_flag::TFLAG_SMALL_PASSAGE, destination ) ||
        here.has_flag( ter_furn_flag::TFLAG_MINEABLE, destination ) ||
        here.has_flag( ter_furn_flag::TFLAG_DOOR, destination ) ||
        here.open_door( player, destination, !here.is_outside( source ), true ) ) {
        return false;
    }

    return simulation.is_simulation_thread() &&
           &simulation.active_avatar() == &player;
}

multiplayer_command_execution rejected_unsupported_move()
{
    multiplayer_command_execution execution;
    execution.rejection = multiplayer_protocol_rejection::invalid_state;
    execution.message =
        "multi-runtime move is outside the verified adjacent empty-ground subset";
    return execution;
}

std::optional<multiplayer_turn_action_disposition> disposition_for_execution(
    const multiplayer_command_execution &execution, const avatar &player )
{
    if( execution.status == multiplayer_command_status::accepted &&
        execution.action_taken ) {
        return player.get_moves() > 0 ?
               multiplayer_turn_action_disposition::accepted_remains_eligible :
               multiplayer_turn_action_disposition::accepted_finished;
    }
    if( execution.status == multiplayer_command_status::rejected &&
        !execution.action_taken ) {
        return multiplayer_turn_action_disposition::rejected;
    }
    if( execution.status == multiplayer_command_status::duplicate &&
        !execution.action_taken ) {
        return multiplayer_turn_action_disposition::duplicate;
    }
    return std::nullopt;
}

} // namespace

multiplayer_multi_runtime_routed_command_result
multiplayer_route_multi_runtime_basic_command(
    multiplayer_multi_runtime_barrier_owner &owner,
    const multiplayer_turn_participant_key &expected_participant,
    const multiplayer_player_command &command )
{
    multiplayer_multi_runtime_routed_command_result result;
    result.phase_status = owner.execute_current_player(
                              expected_participant,
                              [&]( multiplayer_player_runtime &, avatar & player )
    -> std::optional<multiplayer_turn_action_disposition> {
        if( command.kind == multiplayer_command_kind::move && command.direction )
        {
            const tripoint_rel_ms direction( command.direction->dx,
                                             command.direction->dy,
                                             command.direction->dz );
            if( is_valid_adjacent_direction( direction ) &&
                !is_verified_plain_multi_runtime_move( owner.simulation_, player,
                        get_map(), direction ) ) {
                result.execution = rejected_unsupported_move();
            }
        }
        if( !result.execution )
        {
            result.execution = multiplayer_execute_basic_command(
                                   owner.simulation_, player, command );
        }
        result.disposition = disposition_for_execution( *result.execution, player );
        return result.disposition;
    } );
    return result;
}

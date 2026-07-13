#include "avatar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "calendar.h"
#include "enums.h"
#include "game.h"
#include "map.h"
#include "map_helpers.h"
#include "mapdata.h"
#include "multiplayer_command_executor.h"
#include "options_helpers.h"
#include "player_helpers.h"

TEST_CASE( "multiplayer_basic_command_executor_runs_wait_and_move_on_simulation_thread",
           "[multiplayer][command_executor]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );
    const safe_mode_type original_safe_mode = g->get_safe_mode();
    on_out_of_scope restore_safe_mode( [original_safe_mode]() {
        g->set_safe_mode( original_safe_mode );
    } );
    g->set_safe_mode( SAFE_MODE_OFF );

    avatar &player = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms start( 60, 60, 0 );
    here.ter_set( start, ter_id( "t_floor" ) );
    here.ter_set( start + tripoint_rel_ms::east, ter_id( "t_floor" ) );
    player.setpos( here, start );
    player.set_moves( 200 );

    multiplayer_player_command command;
    command.client_sequence = 2;
    command.base_revision = 1;
    command.kind = multiplayer_command_kind::move;
    command.direction = multiplayer_protocol_direction{ 1, 0, 0 };
    multiplayer_command_execution execution = multiplayer_execute_basic_command(
                *g, player, command );
    REQUIRE( execution.status == multiplayer_command_status::accepted );
    CHECK( execution.rejection == multiplayer_protocol_rejection::none );
    CHECK( execution.action_taken );
    CHECK( execution.moves_spent > 0 );
    CHECK( player.pos_bub() == start + tripoint_rel_ms::east );

    player.set_moves( 100 );
    command.client_sequence = 3;
    command.kind = multiplayer_command_kind::wait;
    command.direction.reset();
    execution = multiplayer_execute_basic_command( *g, player, command );
    REQUIRE( execution.status == multiplayer_command_status::accepted );
    CHECK( execution.moves_spent > 0 );
    CHECK( player.get_moves() <= 0 );
}

TEST_CASE( "game_remote_turn_consumes_semantic_actions_without_local_input_ui",
           "[multiplayer][command_executor]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );
    restore_on_out_of_scope restore_turn( calendar::turn );
    restore_on_out_of_scope restore_new_game( g->new_game );
    restore_on_out_of_scope restore_quit( g->uquit );
    override_option disable_autosave( "AUTOSAVE", "false" );
    const safe_mode_type original_safe_mode = g->get_safe_mode();
    on_out_of_scope restore_safe_mode( [original_safe_mode]() {
        g->set_safe_mode( original_safe_mode );
    } );
    g->set_safe_mode( SAFE_MODE_OFF );
    g->uquit = QUIT_NO;
    avatar &player = get_avatar();
    player.set_moves( 100 );
    int calls = 0;

    const bool stopped = g->do_turn_remote( [&]() -> std::optional<bool> {
        ++calls;
        multiplayer_player_command command;
        command.client_sequence = 2;
        command.base_revision = 1;
        command.kind = multiplayer_command_kind::wait;
        const multiplayer_command_execution execution =
        multiplayer_execute_basic_command( *g, player, command );
        REQUIRE( execution.status == multiplayer_command_status::accepted );
        return execution.action_taken;
    } );
    CHECK_FALSE( stopped );
    CHECK( calls == 1 );
    CHECK( player.get_moves() > 0 );
}

TEST_CASE( "multiplayer_remote_move_rejects_interactive_deep_water_confirmation",
           "[multiplayer][command_executor]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );
    const safe_mode_type original_safe_mode = g->get_safe_mode();
    on_out_of_scope restore_safe_mode( [original_safe_mode]() {
        g->set_safe_mode( original_safe_mode );
    } );
    g->set_safe_mode( SAFE_MODE_OFF );
    avatar &player = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms start( 60, 60, 0 );
    here.ter_set( start, ter_id( "t_floor" ) );
    here.ter_set( start + tripoint_rel_ms::east, ter_id( "t_water_dp" ) );
    player.setpos( here, start );
    player.set_moves( 100 );

    multiplayer_player_command command;
    command.client_sequence = 2;
    command.base_revision = 1;
    command.kind = multiplayer_command_kind::move;
    command.direction = multiplayer_protocol_direction{ 1, 0, 0 };
    const multiplayer_command_execution execution = multiplayer_execute_basic_command(
                *g, player, command );
    CHECK( execution.status == multiplayer_command_status::rejected );
    CHECK_FALSE( execution.action_taken );
    CHECK( execution.moves_spent == 0 );
    CHECK( player.pos_bub() == start );
}

TEST_CASE( "multiplayer_remote_move_rejects_vehicle_control_without_entering_local_ui",
           "[multiplayer][command_executor]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );
    avatar &player = get_avatar();
    player.set_moves( 100 );
    player.set_value( "remote_controlling", tripoint_abs_ms::zero );

    multiplayer_player_command command;
    command.client_sequence = 2;
    command.base_revision = 1;
    command.kind = multiplayer_command_kind::move;
    command.direction = multiplayer_protocol_direction{ 1, 0, 0 };
    const multiplayer_command_execution execution = multiplayer_execute_basic_command(
                *g, player, command );
    CHECK( execution.status == multiplayer_command_status::rejected );
    CHECK( execution.rejection == multiplayer_protocol_rejection::invalid_command );
    CHECK_FALSE( execution.action_taken );
    CHECK( execution.moves_spent == 0 );
}

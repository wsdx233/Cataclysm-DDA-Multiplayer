#include "cata_catch.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "avatar.h"
#include "cata_scope_helpers.h"
#include "creature_tracker.h"
#include "enums.h"
#include "game.h"
#include "map.h"
#include "map_helpers.h"
#include "mapdata.h"
#include "memory_fast.h"
#include "multiplayer_multi_runtime_barrier_owner.h"
#include "multiplayer_multi_runtime_command_router.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_player_runtime.h"
#include "options_helpers.h"
#include "player_helpers.h"

namespace
{

multiplayer_turn_participant_key participant_key(
    const multiplayer_player_runtime &runtime )
{
    return { runtime.player_id(), runtime.session_generation() };
}

class registered_secondary_player
{
    public:
        registered_secondary_player( map &here, const tripoint_bub_ms &position ) :
            owner_( make_shared_fast<avatar>() ) {
            avatar &player = *owner_;
            player.create( character_type::NOW );
            clear_character( player );
            player.setID( g->assign_npc_id(), true );
            player.setpos( here, position );
            player.set_moves( 100 );
            registered_ = g->register_multiplayer_player( owner_ );
            if( registered_ ) {
                runtime_ = g->multiplayer_players().find_runtime( player );
            }
        }

        ~registered_secondary_player() {
            if( !registered_ ) {
                return;
            }
            if( runtime_ != nullptr &&
                runtime_->status() == multiplayer_player_status::active ) {
                g->disconnect_multiplayer_player( runtime_->player_id() );
            }
            g->unregister_multiplayer_player( *owner_ );
        }

        registered_secondary_player( const registered_secondary_player & ) = delete;
        registered_secondary_player &operator=( const registered_secondary_player & ) = delete;

        bool valid() const {
            return registered_ && runtime_ != nullptr;
        }

        avatar &player() const {
            return *owner_;
        }

        const shared_ptr_fast<multiplayer_player_runtime> &runtime() const {
            return runtime_;
        }

    private:
        shared_ptr_fast<avatar> owner_;
        shared_ptr_fast<multiplayer_player_runtime> runtime_;
        bool registered_ = false;
};

std::unique_ptr<multiplayer_multi_runtime_barrier_owner> make_owner()
{
    std::string error;
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner =
        multiplayer_multi_runtime_barrier_owner::create( *g, 2, error );
    INFO( error );
    REQUIRE( owner );
    CHECK( error.empty() );
    return owner;
}

multiplayer_player_command move_command( const std::uint64_t sequence,
        const tripoint_rel_ms &direction )
{
    multiplayer_player_command command;
    command.client_sequence = sequence;
    command.base_revision = 1;
    command.kind = multiplayer_command_kind::move;
    command.direction = multiplayer_protocol_direction{
        static_cast<std::int8_t>( direction.x() ),
        static_cast<std::int8_t>( direction.y() ),
        static_cast<std::int8_t>( direction.z() )
    };
    return command;
}

multiplayer_player_command wait_command( const std::uint64_t sequence )
{
    multiplayer_player_command command;
    command.client_sequence = sequence;
    command.base_revision = 1;
    command.kind = multiplayer_command_kind::wait;
    return command;
}

void check_root_context( avatar &root,
                         const multiplayer_player_runtime &root_runtime )
{
    CHECK( &get_avatar() == &root );
    CHECK( &g->active_avatar() == &root );
    CHECK( &g->active_player_runtime() == &root_runtime );
}

void check_accepted_executor_result(
    const multiplayer_multi_runtime_routed_command_result &result,
    const multiplayer_turn_action_disposition expected_disposition )
{
    REQUIRE( result.phase_status == multiplayer_turn_phase_adapter_status::completed );
    REQUIRE( result.execution );
    REQUIRE( result.disposition );
    CHECK( result.resolution ==
           multiplayer_multi_runtime_command_resolution::executor_result );
    CHECK_FALSE( result.blocking_player );
    CHECK( result.execution->status == multiplayer_command_status::accepted );
    CHECK( result.execution->rejection == multiplayer_protocol_rejection::none );
    CHECK( result.execution->action_taken );
    CHECK( *result.disposition == expected_disposition );
}

void check_blocked_by_player_result(
    const multiplayer_multi_runtime_routed_command_result &result,
    const multiplayer_turn_participant_key &expected_blocker )
{
    REQUIRE( result.phase_status == multiplayer_turn_phase_adapter_status::completed );
    REQUIRE( result.execution );
    REQUIRE( result.disposition );
    REQUIRE( result.blocking_player );
    CHECK( result.resolution ==
           multiplayer_multi_runtime_command_resolution::blocked_by_player );
    CHECK( *result.blocking_player == expected_blocker );
    CHECK( result.execution->status == multiplayer_command_status::rejected );
    CHECK( result.execution->rejection == multiplayer_protocol_rejection::invalid_state );
    CHECK_FALSE( result.execution->action_taken );
    CHECK( result.execution->moves_spent == 0 );
    CHECK( *result.disposition == multiplayer_turn_action_disposition::rejected );
}

void complete_world_and_player_end(
    multiplayer_multi_runtime_barrier_owner &owner,
    const std::vector<multiplayer_turn_participant_key> &roster,
    const std::uint64_t expected_shared_turn, int &world_count )
{
    REQUIRE( owner.stage() == multiplayer_multi_runtime_barrier_stage::world_ready );
    CHECK_FALSE( owner.current_slot() );
    REQUIRE( owner.execute_world( [&]( const multiplayer_world_ticket & ticket ) {
        ++world_count;
        CHECK( ticket.shared_turn == expected_shared_turn );
        CHECK( owner.stage() ==
               multiplayer_multi_runtime_barrier_stage::world_processing );
        return true;
    } ) == multiplayer_turn_phase_adapter_status::completed );
    CHECK( owner.stage() ==
           multiplayer_multi_runtime_barrier_stage::player_end_pending );
    CHECK( owner.participant_count() == roster.size() );
    CHECK( owner.has_open_boundary() );
    CHECK( owner.gameplay_side_effects_may_have_occurred() );
    CHECK( owner.begin_turn( roster ) ==
           multiplayer_multi_runtime_barrier_status::not_ready );
    const int world_count_before_duplicate = world_count;
    CHECK( owner.execute_world( [&]( const multiplayer_world_ticket & ) {
        ++world_count;
        return true;
    } ) == multiplayer_turn_phase_adapter_status::invalid_scheduler_state );
    CHECK( world_count == world_count_before_duplicate );
    const std::optional<multiplayer_multi_runtime_world_completion> completion =
        owner.pending_world_completion();
    REQUIRE( completion );
    CHECK( owner.record_external_player_end_completed( *completion ) ==
           multiplayer_multi_runtime_barrier_status::applied );
    CHECK( owner.record_external_player_end_completed( *completion ) ==
           multiplayer_multi_runtime_barrier_status::duplicate );
    CHECK_FALSE( owner.pending_world_completion() );
    CHECK( owner.stage() == multiplayer_multi_runtime_barrier_stage::idle );
    CHECK( owner.participant_count() == 0 );
    CHECK_FALSE( owner.has_open_boundary() );
    CHECK_FALSE( owner.gameplay_side_effects_may_have_occurred() );
    CHECK_FALSE( owner.is_faulted() );
}

} // namespace

TEST_CASE( "multiplayer_multi_runtime_command_router_round_robins_real_moves_and_waits",
           "[multiplayer][multi_runtime_command_router][multi_runtime_barrier_owner]"
           "[command_executor][scheduler][player_bridge]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );
    override_option disable_autosave( "AUTOSAVE", "false" );

    avatar &alpha = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms alpha_start( 60, 60, 0 );
    const tripoint_bub_ms beta_start( 60, 64, 0 );
    here.ter_set( alpha_start, ter_id( "t_floor" ) );
    here.ter_set( alpha_start + tripoint_rel_ms::east, ter_id( "t_floor" ) );
    here.ter_set( beta_start, ter_id( "t_floor" ) );
    here.ter_set( beta_start + tripoint_rel_ms::east, ter_id( "t_floor" ) );
    alpha.setpos( here, alpha_start );
    alpha.set_moves( 1000 );
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    const safe_mode_type original_alpha_safe_mode = alpha_runtime->safe_mode();
    on_out_of_scope restore_alpha_safe_mode( [alpha_runtime, original_alpha_safe_mode]() {
        alpha_runtime->set_safe_mode( original_alpha_safe_mode );
    } );
    alpha_runtime->set_safe_mode( SAFE_MODE_OFF );

    registered_secondary_player secondary( here, beta_start );
    REQUIRE( secondary.valid() );
    avatar &beta = secondary.player();
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime = secondary.runtime();
    beta_runtime->set_safe_mode( SAFE_MODE_OFF );
    beta.set_moves( 1000 );

    const multiplayer_turn_participant_key alpha_key = participant_key( *alpha_runtime );
    const multiplayer_turn_participant_key beta_key = participant_key( *beta_runtime );
    const std::vector<multiplayer_turn_participant_key> roster = { beta_key, alpha_key };
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    const int actions_before = g->get_moves_since_last_save();

    REQUIRE( owner->begin_turn( roster ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    REQUIRE( owner->current_slot() );
    const multiplayer_turn_participant_key first = owner->current_slot()->participant;
    const multiplayer_turn_participant_key second =
        first.player_id == alpha_key.player_id ? beta_key : alpha_key;
    const std::vector<multiplayer_turn_participant_key> expected_order = {
        first, second, first, second
    };
    std::vector<multiplayer_turn_participant_key> observed_order;

    for( std::size_t command_index = 0; command_index < expected_order.size();
         ++command_index ) {
        const std::optional<multiplayer_turn_slot> slot = owner->current_slot();
        REQUIRE( slot );
        CHECK( slot->participant == expected_order[command_index] );
        CHECK( slot->ordering.round == command_index / 2 );
        observed_order.push_back( slot->participant );

        const shared_ptr_fast<multiplayer_player_runtime> target_runtime =
            g->multiplayer_players().find_by_player_id( slot->participant.player_id );
        REQUIRE( target_runtime );
        avatar &target = target_runtime->player();
        avatar &other = &target == &alpha ? beta : alpha;
        const shared_ptr_fast<multiplayer_player_runtime> other_runtime =
            &target == &alpha ? beta_runtime : alpha_runtime;
        REQUIRE( g->multiplayer_players().find_runtime( target ) == target_runtime );
        REQUIRE( g->multiplayer_players().find_runtime( other ) == other_runtime );

        alpha.nv_cached = true;
        beta.nv_cached = true;
        const bool moving = command_index < 2;
        const bool beta_move = moving && &target == &beta;
        if( beta_move ) {
            alpha.grab( object_type::FURNITURE, tripoint_rel_ms::east );
            REQUIRE( alpha.get_grab_type() == object_type::FURNITURE );
            REQUIRE( alpha.grab_point == tripoint_rel_ms::east );
        }

        const tripoint_abs_ms target_position_before = target.pos_abs();
        const tripoint_abs_ms other_position_before = other.pos_abs();
        const int target_moves_before = target.get_moves();
        const int other_moves_before = other.get_moves();
        const int actions_before_command = g->get_moves_since_last_save();
        REQUIRE( get_creature_tracker().creature_at<avatar>( target_position_before ) ==
                 &target );
        REQUIRE( get_creature_tracker().creature_at<avatar>( other_position_before ) ==
                 &other );

        const multiplayer_player_command command = moving ?
                move_command( command_index + 2, tripoint_rel_ms::east ) :
                wait_command( command_index + 2 );
        const auto routed = multiplayer_route_multi_runtime_basic_command(
                                *owner, slot->participant, command );

        REQUIRE( routed.phase_status == multiplayer_turn_phase_adapter_status::completed );
        REQUIRE( routed.execution );
        REQUIRE( routed.disposition );
        CHECK( routed.execution->status == multiplayer_command_status::accepted );
        CHECK( routed.execution->rejection == multiplayer_protocol_rejection::none );
        CHECK( routed.execution->action_taken );
        CHECK( routed.execution->moves_spent ==
               std::max( 0, target_moves_before - target.get_moves() ) );
        CHECK( *routed.disposition ==
               ( moving ?
                 multiplayer_turn_action_disposition::accepted_remains_eligible :
                 multiplayer_turn_action_disposition::accepted_finished ) );
        CHECK( g->get_moves_since_last_save() == actions_before_command + 1 );
        CHECK_FALSE( target.nv_cached );
        CHECK( other.nv_cached );
        CHECK( other.pos_abs() == other_position_before );
        CHECK( other.get_moves() == other_moves_before );

        if( moving ) {
            CHECK( target.pos_abs() == target_position_before + tripoint_rel_ms::east );
            CHECK( target.get_moves() > 0 );
            CHECK( get_creature_tracker().creature_at<avatar>( target_position_before ) ==
                   nullptr );
            CHECK( get_creature_tracker().creature_at<avatar>( target.pos_abs() ) ==
                   &target );
        } else {
            CHECK( target.pos_abs() == target_position_before );
            CHECK( target.get_moves() <= 0 );
            CHECK( get_creature_tracker().creature_at<avatar>( target_position_before ) ==
                   &target );
        }
        CHECK( get_creature_tracker().creature_at<avatar>( other_position_before ) ==
               &other );
        CHECK( g->multiplayer_players().find_runtime( target ) == target_runtime );
        CHECK( g->multiplayer_players().find_runtime( other ) == other_runtime );

        if( beta_move ) {
            CHECK( alpha.get_grab_type() == object_type::FURNITURE );
            CHECK( alpha.grab_point == tripoint_rel_ms::east );
            alpha.grab( object_type::NONE );
        }
        check_root_context( alpha, *alpha_runtime );
    }

    CHECK( observed_order == expected_order );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::world_ready );
    CHECK_FALSE( owner->current_slot() );
    CHECK( g->get_moves_since_last_save() == actions_before + 4 );
    int world_count = 0;
    complete_world_and_player_end( *owner, roster, 1, world_count );
    CHECK( world_count == 1 );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::idle );
    CHECK_FALSE( owner->has_open_boundary() );
    CHECK_FALSE( owner->is_faulted() );
    check_root_context( alpha, *alpha_runtime );
}

TEST_CASE( "multiplayer_multi_runtime_command_router_blocks_adjacent_active_players_symmetrically",
           "[multiplayer][multi_runtime_command_router][multi_runtime_barrier_owner]"
           "[command_executor][scheduler][player_bridge][human_collision]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );
    override_option disable_autosave( "AUTOSAVE", "false" );

    avatar &alpha = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms alpha_start( 60, 60, 0 );
    const tripoint_bub_ms beta_start = alpha_start + tripoint_rel_ms::east;
    here.ter_set( alpha_start, ter_id( "t_floor" ) );
    here.ter_set( beta_start, ter_id( "t_floor" ) );
    alpha.setpos( here, alpha_start );
    alpha.set_moves( 1000 );
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    const safe_mode_type original_alpha_safe_mode = alpha_runtime->safe_mode();
    on_out_of_scope restore_alpha_safe_mode( [alpha_runtime, original_alpha_safe_mode]() {
        alpha_runtime->set_safe_mode( original_alpha_safe_mode );
    } );
    alpha_runtime->set_safe_mode( SAFE_MODE_OFF );

    registered_secondary_player secondary( here, beta_start );
    REQUIRE( secondary.valid() );
    avatar &beta = secondary.player();
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime = secondary.runtime();
    beta_runtime->set_safe_mode( SAFE_MODE_OFF );
    beta.set_moves( 1000 );

    const multiplayer_turn_participant_key alpha_key = participant_key( *alpha_runtime );
    const multiplayer_turn_participant_key beta_key = participant_key( *beta_runtime );
    const std::vector<multiplayer_turn_participant_key> roster = { beta_key, alpha_key };
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    const int actions_before = g->get_moves_since_last_save();
    std::uint64_t next_sequence = 20;

    REQUIRE( owner->begin_turn( roster ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    for( int actor_index = 0; actor_index < 2; ++actor_index ) {
        const std::optional<multiplayer_turn_slot> blocked_slot = owner->current_slot();
        REQUIRE( blocked_slot );
        CHECK( blocked_slot->ordering.round == 0 );
        CHECK( blocked_slot->state ==
               multiplayer_turn_participant_state::awaiting_command );

        const shared_ptr_fast<multiplayer_player_runtime> target_runtime =
            g->multiplayer_players().find_by_player_id(
                blocked_slot->participant.player_id );
        REQUIRE( target_runtime );
        avatar &target = target_runtime->player();
        avatar &blocker = &target == &alpha ? beta : alpha;
        const shared_ptr_fast<multiplayer_player_runtime> blocker_runtime =
            &target == &alpha ? beta_runtime : alpha_runtime;
        const multiplayer_turn_participant_key blocker_key =
            participant_key( *blocker_runtime );
        const tripoint_rel_ms toward_blocker = &target == &alpha ?
                                               tripoint_rel_ms::east :
                                               tripoint_rel_ms::west;
        REQUIRE( target_runtime->status() == multiplayer_player_status::active );
        REQUIRE( blocker_runtime->status() == multiplayer_player_status::active );
        REQUIRE( g->multiplayer_players().find_runtime( target ) == target_runtime );
        REQUIRE( g->multiplayer_players().find_runtime( blocker ) == blocker_runtime );

        alpha.nv_cached = true;
        beta.nv_cached = true;
        const tripoint_abs_ms target_position = target.pos_abs();
        const tripoint_abs_ms blocker_position = blocker.pos_abs();
        const int target_moves = target.get_moves();
        const int blocker_moves = blocker.get_moves();
        const int actions_before_collision = g->get_moves_since_last_save();
        const bool side_effects_before_collision =
            owner->gameplay_side_effects_may_have_occurred();
        REQUIRE( get_creature_tracker().creature_at<avatar>( target_position ) ==
                 &target );
        REQUIRE( get_creature_tracker().creature_at<avatar>( blocker_position ) ==
                 &blocker );
        REQUIRE( g->multiplayer_players().find_other_at( target_position, target ) ==
                 nullptr );
        REQUIRE( g->multiplayer_players().find_other_at( blocker_position, target ) ==
                 &blocker );
        REQUIRE( g->multiplayer_players().find_other_at( blocker_position, blocker ) ==
                 nullptr );

        for( int retry = 0; retry < 2; ++retry ) {
            const auto blocked = multiplayer_route_multi_runtime_basic_command(
                                     *owner, blocked_slot->participant,
                                     move_command( next_sequence++, toward_blocker ) );
            check_blocked_by_player_result( blocked, blocker_key );
            CHECK( owner->current_slot() == blocked_slot );
            CHECK( target.pos_abs() == target_position );
            CHECK( blocker.pos_abs() == blocker_position );
            CHECK( target.get_moves() == target_moves );
            CHECK( blocker.get_moves() == blocker_moves );
            CHECK( alpha.nv_cached );
            CHECK( beta.nv_cached );
            CHECK( g->get_moves_since_last_save() == actions_before_collision );
            CHECK( get_creature_tracker().creature_at<avatar>( target_position ) ==
                   &target );
            CHECK( get_creature_tracker().creature_at<avatar>( blocker_position ) ==
                   &blocker );
            CHECK( g->multiplayer_players().find_runtime( target ) == target_runtime );
            CHECK( g->multiplayer_players().find_runtime( blocker ) == blocker_runtime );
            CHECK( owner->gameplay_side_effects_may_have_occurred() ==
                   side_effects_before_collision );
            CHECK_FALSE( owner->is_faulted() );
            check_root_context( alpha, *alpha_runtime );
        }

        const int moves_before_wait = target.get_moves();
        const int actions_before_wait = g->get_moves_since_last_save();
        const auto waited = multiplayer_route_multi_runtime_basic_command(
                                *owner, blocked_slot->participant,
                                wait_command( next_sequence++ ) );
        check_accepted_executor_result(
            waited, multiplayer_turn_action_disposition::accepted_finished );
        REQUIRE( waited.execution );
        CHECK( waited.execution->moves_spent ==
               std::max( 0, moves_before_wait - target.get_moves() ) );
        CHECK( target.get_moves() <= 0 );
        CHECK( target.pos_abs() == target_position );
        CHECK( blocker.pos_abs() == blocker_position );
        CHECK_FALSE( target.nv_cached );
        CHECK( blocker.nv_cached );
        CHECK( g->get_moves_since_last_save() == actions_before_wait + 1 );
        CHECK( get_creature_tracker().creature_at<avatar>( target_position ) ==
               &target );
        CHECK( get_creature_tracker().creature_at<avatar>( blocker_position ) ==
               &blocker );
        CHECK_FALSE( owner->is_faulted() );
        check_root_context( alpha, *alpha_runtime );
    }

    CHECK( alpha.pos_bub( here ) == alpha_start );
    CHECK( beta.pos_bub( here ) == beta_start );
    CHECK( alpha.get_moves() <= 0 );
    CHECK( beta.get_moves() <= 0 );
    CHECK( g->get_moves_since_last_save() == actions_before + 2 );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::world_ready );
    int world_count = 0;
    complete_world_and_player_end( *owner, roster, 1, world_count );
    CHECK( world_count == 1 );
    check_root_context( alpha, *alpha_runtime );
}

TEST_CASE( "multiplayer_multi_runtime_command_router_rotates_contested_tile_winner",
           "[multiplayer][multi_runtime_command_router][multi_runtime_barrier_owner]"
           "[command_executor][scheduler][player_bridge][human_collision]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );
    override_option disable_autosave( "AUTOSAVE", "false" );

    avatar &alpha = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms center( 60, 60, 0 );
    const tripoint_bub_ms alpha_start = center + tripoint_rel_ms::west;
    const tripoint_bub_ms beta_start = center + tripoint_rel_ms::east;
    here.ter_set( alpha_start, ter_id( "t_floor" ) );
    here.ter_set( center, ter_id( "t_floor" ) );
    here.ter_set( beta_start, ter_id( "t_floor" ) );
    alpha.setpos( here, alpha_start );
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    const safe_mode_type original_alpha_safe_mode = alpha_runtime->safe_mode();
    on_out_of_scope restore_alpha_safe_mode( [alpha_runtime, original_alpha_safe_mode]() {
        alpha_runtime->set_safe_mode( original_alpha_safe_mode );
    } );
    alpha_runtime->set_safe_mode( SAFE_MODE_OFF );

    registered_secondary_player secondary( here, beta_start );
    REQUIRE( secondary.valid() );
    avatar &beta = secondary.player();
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime = secondary.runtime();
    beta_runtime->set_safe_mode( SAFE_MODE_OFF );

    const multiplayer_turn_participant_key alpha_key = participant_key( *alpha_runtime );
    const multiplayer_turn_participant_key beta_key = participant_key( *beta_runtime );
    const std::vector<multiplayer_turn_participant_key> roster = { beta_key, alpha_key };
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    const tripoint_abs_ms center_abs = here.get_abs( center );
    const int actions_before = g->get_moves_since_last_save();
    std::uint64_t next_sequence = 40;
    int world_count = 0;
    std::vector<multiplayer_turn_participant_key> winners;

    for( std::uint64_t shared_turn = 1; shared_turn <= 2; ++shared_turn ) {
        REQUIRE( owner->stage() == multiplayer_multi_runtime_barrier_stage::idle );
        alpha.setpos( here, alpha_start, false );
        beta.setpos( here, beta_start, false );
        alpha.set_moves( 1000 );
        beta.set_moves( 1000 );
        alpha.nv_cached = true;
        beta.nv_cached = true;
        REQUIRE( get_creature_tracker().creature_at<avatar>( alpha.pos_abs() ) ==
                 &alpha );
        REQUIRE( get_creature_tracker().creature_at<avatar>( beta.pos_abs() ) ==
                 &beta );
        REQUIRE( get_creature_tracker().creature_at<avatar>( center_abs ) == nullptr );
        REQUIRE( g->multiplayer_players().find_runtime( alpha ) == alpha_runtime );
        REQUIRE( g->multiplayer_players().find_runtime( beta ) == beta_runtime );
        check_root_context( alpha, *alpha_runtime );

        REQUIRE( owner->begin_turn( roster ) ==
                 multiplayer_multi_runtime_barrier_status::applied );
        const std::optional<multiplayer_turn_slot> winning_slot = owner->current_slot();
        REQUIRE( winning_slot );
        CHECK( winning_slot->ordering.shared_turn == shared_turn );
        CHECK( winning_slot->ordering.round == 0 );
        if( !winners.empty() ) {
            CHECK( winning_slot->participant != winners.front() );
        }
        winners.push_back( winning_slot->participant );

        const shared_ptr_fast<multiplayer_player_runtime> winner_runtime =
            g->multiplayer_players().find_by_player_id(
                winning_slot->participant.player_id );
        REQUIRE( winner_runtime );
        avatar &winner = winner_runtime->player();
        avatar &loser = &winner == &alpha ? beta : alpha;
        const shared_ptr_fast<multiplayer_player_runtime> loser_runtime =
            &winner == &alpha ? beta_runtime : alpha_runtime;
        const multiplayer_turn_participant_key loser_key =
            participant_key( *loser_runtime );
        const tripoint_rel_ms winner_direction = &winner == &alpha ?
                tripoint_rel_ms::east :
                tripoint_rel_ms::west;
        const tripoint_rel_ms loser_direction = &loser == &alpha ?
                                                tripoint_rel_ms::east :
                                                tripoint_rel_ms::west;
        const tripoint_abs_ms winner_start = winner.pos_abs();
        const tripoint_abs_ms loser_start = loser.pos_abs();
        const int winner_moves_before = winner.get_moves();
        const int loser_moves_before = loser.get_moves();
        const int actions_before_winning_move = g->get_moves_since_last_save();

        const auto won = multiplayer_route_multi_runtime_basic_command(
                             *owner, winning_slot->participant,
                             move_command( next_sequence++, winner_direction ) );
        check_accepted_executor_result(
            won, multiplayer_turn_action_disposition::accepted_remains_eligible );
        REQUIRE( won.execution );
        CHECK( won.execution->moves_spent ==
               std::max( 0, winner_moves_before - winner.get_moves() ) );
        CHECK( won.execution->moves_spent > 0 );
        CHECK( winner.pos_abs() == center_abs );
        CHECK( winner.get_moves() > 0 );
        CHECK( loser.pos_abs() == loser_start );
        CHECK( loser.get_moves() == loser_moves_before );
        CHECK_FALSE( winner.nv_cached );
        CHECK( loser.nv_cached );
        CHECK( g->get_moves_since_last_save() == actions_before_winning_move + 1 );
        CHECK( get_creature_tracker().creature_at<avatar>( winner_start ) == nullptr );
        CHECK( get_creature_tracker().creature_at<avatar>( center_abs ) == &winner );
        CHECK( get_creature_tracker().creature_at<avatar>( loser_start ) == &loser );
        CHECK( g->multiplayer_players().find_other_at( center_abs, winner ) == nullptr );
        CHECK( g->multiplayer_players().find_other_at( center_abs, loser ) == &winner );
        CHECK( g->multiplayer_players().find_runtime( winner ) == winner_runtime );
        CHECK( g->multiplayer_players().find_runtime( loser ) == loser_runtime );
        CHECK( owner->gameplay_side_effects_may_have_occurred() );
        CHECK_FALSE( owner->is_faulted() );
        check_root_context( alpha, *alpha_runtime );

        const std::optional<multiplayer_turn_slot> losing_slot = owner->current_slot();
        REQUIRE( losing_slot );
        CHECK( losing_slot->participant == loser_key );
        CHECK( losing_slot->ordering.shared_turn == shared_turn );
        CHECK( losing_slot->ordering.round == 0 );
        const int winner_moves_after_move = winner.get_moves();
        const int loser_moves_before_collision = loser.get_moves();
        const int actions_before_collision = g->get_moves_since_last_save();
        const bool winner_cache_before_collision = winner.nv_cached;
        const bool loser_cache_before_collision = loser.nv_cached;

        for( int retry = 0; retry < 2; ++retry ) {
            const auto blocked = multiplayer_route_multi_runtime_basic_command(
                                     *owner, losing_slot->participant,
                                     move_command( next_sequence++, loser_direction ) );
            check_blocked_by_player_result( blocked, winning_slot->participant );
            CHECK( owner->current_slot() == losing_slot );
            CHECK( winner.pos_abs() == center_abs );
            CHECK( loser.pos_abs() == loser_start );
            CHECK( winner.get_moves() == winner_moves_after_move );
            CHECK( loser.get_moves() == loser_moves_before_collision );
            CHECK( winner.nv_cached == winner_cache_before_collision );
            CHECK( loser.nv_cached == loser_cache_before_collision );
            CHECK( g->get_moves_since_last_save() == actions_before_collision );
            CHECK( get_creature_tracker().creature_at<avatar>( center_abs ) == &winner );
            CHECK( get_creature_tracker().creature_at<avatar>( loser_start ) == &loser );
            CHECK( g->multiplayer_players().find_runtime( winner ) == winner_runtime );
            CHECK( g->multiplayer_players().find_runtime( loser ) == loser_runtime );
            CHECK( owner->gameplay_side_effects_may_have_occurred() );
            CHECK_FALSE( owner->is_faulted() );
            check_root_context( alpha, *alpha_runtime );
        }

        const int actions_before_loser_wait = g->get_moves_since_last_save();
        const auto loser_waited = multiplayer_route_multi_runtime_basic_command(
                                      *owner, losing_slot->participant,
                                      wait_command( next_sequence++ ) );
        check_accepted_executor_result(
            loser_waited, multiplayer_turn_action_disposition::accepted_finished );
        REQUIRE( loser_waited.execution );
        CHECK( loser_waited.execution->moves_spent ==
               std::max( 0, loser_moves_before_collision - loser.get_moves() ) );
        CHECK( loser.get_moves() <= 0 );
        CHECK( winner.pos_abs() == center_abs );
        CHECK( loser.pos_abs() == loser_start );
        CHECK( g->get_moves_since_last_save() == actions_before_loser_wait + 1 );
        CHECK( get_creature_tracker().creature_at<avatar>( center_abs ) == &winner );
        CHECK( get_creature_tracker().creature_at<avatar>( loser_start ) == &loser );
        CHECK_FALSE( owner->is_faulted() );
        check_root_context( alpha, *alpha_runtime );

        const std::optional<multiplayer_turn_slot> winner_again = owner->current_slot();
        REQUIRE( winner_again );
        CHECK( winner_again->participant == winning_slot->participant );
        CHECK( winner_again->ordering.shared_turn == shared_turn );
        CHECK( winner_again->ordering.round == 1 );
        const int actions_before_winner_wait = g->get_moves_since_last_save();
        const int winner_moves_before_wait = winner.get_moves();
        const auto winner_waited = multiplayer_route_multi_runtime_basic_command(
                                       *owner, winner_again->participant,
                                       wait_command( next_sequence++ ) );
        check_accepted_executor_result(
            winner_waited, multiplayer_turn_action_disposition::accepted_finished );
        REQUIRE( winner_waited.execution );
        CHECK( winner_waited.execution->moves_spent ==
               std::max( 0, winner_moves_before_wait - winner.get_moves() ) );
        CHECK( winner.get_moves() <= 0 );
        CHECK( winner.pos_abs() == center_abs );
        CHECK( loser.pos_abs() == loser_start );
        CHECK( g->get_moves_since_last_save() == actions_before_winner_wait + 1 );
        CHECK( get_creature_tracker().creature_at<avatar>( center_abs ) == &winner );
        CHECK( get_creature_tracker().creature_at<avatar>( loser_start ) == &loser );
        CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::world_ready );
        CHECK_FALSE( owner->is_faulted() );
        check_root_context( alpha, *alpha_runtime );

        complete_world_and_player_end( *owner, roster, shared_turn, world_count );
        CHECK( world_count == static_cast<int>( shared_turn ) );
        CHECK( g->get_moves_since_last_save() ==
               actions_before + static_cast<int>( shared_turn ) * 3 );
        check_root_context( alpha, *alpha_runtime );
    }

    REQUIRE( winners.size() == 2 );
    CHECK( winners[0] != winners[1] );
    CHECK( ( ( winners[0] == alpha_key && winners[1] == beta_key ) ||
             ( winners[0] == beta_key && winners[1] == alpha_key ) ) );
    CHECK( world_count == 2 );
    CHECK( g->get_moves_since_last_save() == actions_before + 6 );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::idle );
    CHECK_FALSE( owner->has_open_boundary() );
    CHECK_FALSE( owner->is_faulted() );
    check_root_context( alpha, *alpha_runtime );
}

TEST_CASE( "multiplayer_multi_runtime_command_router_rejects_unowned_or_unsupported_moves",
           "[multiplayer][multi_runtime_command_router][multi_runtime_barrier_owner]"
           "[command_executor][scheduler][player_bridge]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );
    override_option disable_autosave( "AUTOSAVE", "false" );

    avatar &alpha = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms alpha_start( 60, 60, 0 );
    const tripoint_bub_ms beta_start( 61, 60, 0 );
    here.ter_set( alpha_start + tripoint_rel_ms::west, ter_id( "t_floor" ) );
    here.ter_set( alpha_start, ter_id( "t_floor" ) );
    here.ter_set( beta_start, ter_id( "t_floor" ) );
    here.ter_set( beta_start + tripoint_rel_ms::east, ter_id( "t_floor" ) );
    alpha.setpos( here, alpha_start );
    alpha.set_moves( 1000 );
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    const safe_mode_type original_alpha_safe_mode = alpha_runtime->safe_mode();
    on_out_of_scope restore_alpha_safe_mode( [alpha_runtime, original_alpha_safe_mode]() {
        alpha_runtime->set_safe_mode( original_alpha_safe_mode );
    } );
    alpha_runtime->set_safe_mode( SAFE_MODE_OFF );

    registered_secondary_player secondary( here, beta_start );
    REQUIRE( secondary.valid() );
    avatar &beta = secondary.player();
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime = secondary.runtime();
    beta_runtime->set_safe_mode( SAFE_MODE_OFF );
    beta.set_moves( 1000 );

    const multiplayer_turn_participant_key alpha_key = participant_key( *alpha_runtime );
    const multiplayer_turn_participant_key beta_key = participant_key( *beta_runtime );
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    REQUIRE( owner->begin_turn( { alpha_key, beta_key } ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    const std::optional<multiplayer_turn_slot> initial_slot = owner->current_slot();
    REQUIRE( initial_slot );
    const multiplayer_turn_participant_key other_key =
        initial_slot->participant.player_id == alpha_key.player_id ? beta_key : alpha_key;
    const shared_ptr_fast<multiplayer_player_runtime> target_runtime =
        g->multiplayer_players().find_by_player_id( initial_slot->participant.player_id );
    REQUIRE( target_runtime );
    avatar &target = target_runtime->player();
    avatar &other = &target == &alpha ? beta : alpha;
    const tripoint_rel_ms away_from_other = &target == &alpha ?
                                            tripoint_rel_ms::west : tripoint_rel_ms::east;
    const tripoint_rel_ms toward_other = &target == &alpha ?
                                         tripoint_rel_ms::east : tripoint_rel_ms::west;
    const tripoint_abs_ms target_position = target.pos_abs();
    const tripoint_abs_ms other_position = other.pos_abs();
    const int target_moves = target.get_moves();
    const int other_moves = other.get_moves();
    const int actions_before = g->get_moves_since_last_save();

    alpha.nv_cached = true;
    beta.nv_cached = true;
    const auto noncurrent = multiplayer_route_multi_runtime_basic_command(
                                *owner, other_key, move_command( 2, away_from_other ) );
    CHECK( noncurrent.phase_status ==
           multiplayer_turn_phase_adapter_status::invalid_scheduler_state );
    CHECK_FALSE( noncurrent.execution );
    CHECK_FALSE( noncurrent.disposition );
    CHECK( noncurrent.resolution ==
           multiplayer_multi_runtime_command_resolution::none );
    CHECK_FALSE( noncurrent.blocking_player );
    CHECK( owner->current_slot() == initial_slot );

    multiplayer_player_command invalid_move;
    invalid_move.client_sequence = 3;
    invalid_move.base_revision = 1;
    invalid_move.kind = multiplayer_command_kind::move;
    const auto invalid = multiplayer_route_multi_runtime_basic_command(
                             *owner, initial_slot->participant, invalid_move );
    REQUIRE( invalid.phase_status == multiplayer_turn_phase_adapter_status::completed );
    REQUIRE( invalid.execution );
    REQUIRE( invalid.disposition );
    CHECK( invalid.resolution ==
           multiplayer_multi_runtime_command_resolution::executor_result );
    CHECK_FALSE( invalid.blocking_player );
    CHECK( invalid.execution->status == multiplayer_command_status::rejected );
    CHECK_FALSE( invalid.execution->action_taken );
    CHECK( invalid.execution->moves_spent == 0 );
    CHECK( *invalid.disposition == multiplayer_turn_action_disposition::rejected );
    CHECK( owner->current_slot() == initial_slot );

    target.grab( object_type::FURNITURE, tripoint_rel_ms::east );
    REQUIRE( target.get_grab_type() == object_type::FURNITURE );
    const auto unsupported = multiplayer_route_multi_runtime_basic_command(
                                 *owner, initial_slot->participant,
                                 move_command( 4, away_from_other ) );
    REQUIRE( unsupported.phase_status == multiplayer_turn_phase_adapter_status::completed );
    REQUIRE( unsupported.execution );
    REQUIRE( unsupported.disposition );
    CHECK( unsupported.resolution ==
           multiplayer_multi_runtime_command_resolution::unsupported_move );
    CHECK_FALSE( unsupported.blocking_player );
    CHECK( unsupported.execution->status == multiplayer_command_status::rejected );
    CHECK_FALSE( unsupported.execution->action_taken );
    CHECK( unsupported.execution->moves_spent == 0 );
    CHECK( *unsupported.disposition == multiplayer_turn_action_disposition::rejected );
    CHECK( target.get_grab_type() == object_type::FURNITURE );
    CHECK( owner->current_slot() == initial_slot );
    target.grab( object_type::NONE );

    const auto occupied = multiplayer_route_multi_runtime_basic_command(
                              *owner, initial_slot->participant,
                              move_command( 5, toward_other ) );
    check_blocked_by_player_result( occupied, other_key );
    CHECK( owner->current_slot() == initial_slot );

    CHECK( target.pos_abs() == target_position );
    CHECK( other.pos_abs() == other_position );
    CHECK( target.get_moves() == target_moves );
    CHECK( other.get_moves() == other_moves );
    CHECK( get_creature_tracker().creature_at<avatar>( target_position ) == &target );
    CHECK( get_creature_tracker().creature_at<avatar>( other_position ) == &other );
    CHECK( g->get_moves_since_last_save() == actions_before );
    CHECK( alpha.nv_cached );
    CHECK( beta.nv_cached );
    CHECK_FALSE( owner->is_faulted() );
    CHECK_FALSE( owner->gameplay_side_effects_may_have_occurred() );
    check_root_context( alpha, *alpha_runtime );
}

#include "cata_catch.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "avatar.h"
#include "cata_scope_helpers.h"
#include "enums.h"
#include "game.h"
#include "map.h"
#include "map_helpers.h"
#include "memory_fast.h"
#include "multiplayer_command_executor.h"
#include "multiplayer_player_context.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_player_runtime.h"
#include "multiplayer_turn_scheduler.h"
#include "player_helpers.h"

namespace
{

static_assert( !std::is_copy_constructible_v<multiplayer_turn_scheduler> );
static_assert( !std::is_copy_assignable_v<multiplayer_turn_scheduler> );
static_assert( !std::is_move_constructible_v<multiplayer_turn_scheduler> );
static_assert( !std::is_move_assignable_v<multiplayer_turn_scheduler> );

multiplayer_turn_participant_key participant( const int suffix,
        const std::uint64_t generation = 1 )
{
    std::string id = "00000000-0000-4000-8000-000000000000";
    id.back() = static_cast<char>( '0' + suffix );
    return { multiplayer_player_id::from_string( std::move( id ) ), generation };
}

std::string player_id_string( const multiplayer_turn_participant_key &participant )
{
    return participant.player_id.str();
}

} // namespace

TEST_CASE( "multiplayer_turn_scheduler_snapshots_and_validates_roster",
           "[multiplayer][scheduler]" )
{
    multiplayer_turn_scheduler scheduler;
    const multiplayer_turn_participant_key alpha = participant( 1 );
    const multiplayer_turn_participant_key beta = participant( 2 );
    const multiplayer_turn_participant_key gamma = participant( 3 );

    CHECK_FALSE( scheduler.begin_turn( 1, {} ) );
    CHECK_FALSE( scheduler.begin_turn( 1, { multiplayer_turn_participant_key{} } ) );
    CHECK_FALSE( scheduler.begin_turn( 1, { { alpha.player_id, 0 } } ) );
    CHECK_FALSE( scheduler.begin_turn( 1, { alpha, { alpha.player_id, 2 } } ) );
    CHECK_FALSE( scheduler.begin_turn( 1, { alpha, beta, gamma, participant( 4 ), participant( 5 ) } ) );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::idle );

    std::vector<multiplayer_turn_participant_key> roster = { beta, alpha };
    REQUIRE( scheduler.begin_turn( 10, roster ) );
    roster.emplace_back( gamma );
    CHECK( scheduler.participant_count() == 2 );
    CHECK_FALSE( scheduler.has_participant( gamma.player_id ) );
    CHECK_FALSE( scheduler.begin_turn( 11, roster ) );

    while( const std::optional<multiplayer_turn_slot> slot = scheduler.current_slot() ) {
        REQUIRE( scheduler.record_action_result(
                     slot->participant,
                     multiplayer_turn_action_disposition::accepted_finished ) );
    }
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_ready );
    CHECK_FALSE( scheduler.record_world_completed( { 10 } ) );
    const std::optional<multiplayer_world_ticket> ticket = scheduler.claim_world();
    REQUIRE( ticket );
    CHECK( ticket->shared_turn == 10 );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_processing );
    CHECK_FALSE( scheduler.claim_world() );
    CHECK_FALSE( scheduler.record_world_completed( { 9 } ) );
    REQUIRE( scheduler.record_world_completed( *ticket ) );
    CHECK_FALSE( scheduler.record_world_completed( *ticket ) );
    CHECK_FALSE( scheduler.begin_turn( 10, roster ) );
    REQUIRE( scheduler.begin_turn( 11, roster ) );
    CHECK( scheduler.participant_count() == 3 );
    CHECK( scheduler.has_participant( gamma.player_id ) );
}

TEST_CASE( "multiplayer_turn_scheduler_round_robin_records_typed_results",
           "[multiplayer][scheduler]" )
{
    multiplayer_turn_scheduler scheduler;
    const multiplayer_turn_participant_key alpha = participant( 1 );
    const multiplayer_turn_participant_key beta = participant( 2 );
    const multiplayer_turn_participant_key gamma = participant( 3 );
    REQUIRE( scheduler.begin_turn( 1, { gamma, alpha, beta } ) );

    const std::optional<multiplayer_turn_slot> first = scheduler.current_slot();
    REQUIRE( first );
    CHECK( first->participant == alpha );
    const multiplayer_turn_ordering_key expected_first_order = { 1, 0, 0, alpha.player_id };
    CHECK( first->ordering == expected_first_order );
    CHECK( scheduler.current_slot() == first );
    CHECK_FALSE( scheduler.record_action_result(
                     beta, multiplayer_turn_action_disposition::accepted_remains_eligible ) );
    CHECK( scheduler.current_slot() == first );
    REQUIRE( scheduler.record_action_result(
                 alpha, multiplayer_turn_action_disposition::rejected ) );
    CHECK( scheduler.current_slot() == first );
    REQUIRE( scheduler.record_action_result(
                 alpha, multiplayer_turn_action_disposition::duplicate ) );
    CHECK( scheduler.current_slot() == first );

    REQUIRE( scheduler.record_action_result(
                 alpha, multiplayer_turn_action_disposition::accepted_remains_eligible ) );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->participant == beta );
    CHECK( scheduler.current_slot()->ordering.round == 0 );
    REQUIRE( scheduler.record_action_result(
                 beta, multiplayer_turn_action_disposition::accepted_remains_eligible ) );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->participant == gamma );
    REQUIRE( scheduler.record_action_result(
                 gamma, multiplayer_turn_action_disposition::accepted_remains_eligible ) );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->participant == alpha );
    CHECK( scheduler.current_slot()->ordering.round == 1 );

    REQUIRE( scheduler.record_action_result(
                 alpha, multiplayer_turn_action_disposition::accepted_finished ) );
    REQUIRE( scheduler.record_action_result(
                 beta, multiplayer_turn_action_disposition::accepted_finished ) );
    REQUIRE( scheduler.record_action_result(
                 gamma, multiplayer_turn_action_disposition::accepted_finished ) );
    REQUIRE( scheduler.claim_world() );
    REQUIRE( scheduler.record_world_completed( { 1 } ) );

    REQUIRE( scheduler.begin_turn( 2, { gamma, alpha, beta } ) );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->participant == beta );
}

TEST_CASE( "multiplayer_turn_scheduler_disconnect_resume_and_timeout_are_barrier_local",
           "[multiplayer][scheduler]" )
{
    multiplayer_turn_scheduler scheduler;
    const multiplayer_turn_participant_key alpha = participant( 1 );
    const multiplayer_turn_participant_key beta = participant( 2 );
    REQUIRE( scheduler.begin_turn( 1, { alpha, beta } ) );

    REQUIRE( scheduler.mark_barrier_disconnected( beta ) );
    CHECK_FALSE( scheduler.apply_disconnect_timeout(
                     beta, multiplayer_disconnect_timeout_policy::automatic_wait ) );
    REQUIRE( scheduler.mark_barrier_disconnected( alpha ) );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->state ==
           multiplayer_turn_participant_state::disconnected_grace );
    CHECK_FALSE( scheduler.resume_barrier_participant( { alpha.player_id, 2 }, 3 ) );
    CHECK_FALSE( scheduler.resume_barrier_participant( alpha, 1 ) );
    CHECK_FALSE( scheduler.resume_barrier_participant( alpha, 3 ) );
    REQUIRE( scheduler.resume_barrier_participant( alpha, 2 ) );

    const multiplayer_turn_participant_key resumed_alpha = { alpha.player_id, 2 };
    CHECK( scheduler.has_participant( alpha.player_id ) );
    REQUIRE( scheduler.participant_key( alpha.player_id ) );
    CHECK( scheduler.participant_key( alpha.player_id )->session_generation == 2 );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->participant == resumed_alpha );
    CHECK_FALSE( scheduler.mark_barrier_disconnected( alpha ) );
    REQUIRE( scheduler.record_action_result(
                 resumed_alpha, multiplayer_turn_action_disposition::accepted_finished ) );

    REQUIRE( scheduler.apply_disconnect_timeout(
                 beta, multiplayer_disconnect_timeout_policy::keep_waiting ) );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->participant == beta );
    REQUIRE( scheduler.apply_disconnect_timeout(
                 beta, multiplayer_disconnect_timeout_policy::automatic_wait ) );
    CHECK( scheduler.current_slot()->state ==
           multiplayer_turn_participant_state::automatic_wait_pending );
    CHECK_FALSE( scheduler.record_action_result(
                     beta, multiplayer_turn_action_disposition::accepted_finished ) );
    CHECK_FALSE( scheduler.resume_barrier_participant( beta, 2 ) );
    REQUIRE( scheduler.record_automatic_wait_executed( beta ) );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_ready );

    multiplayer_turn_scheduler removal_scheduler;
    REQUIRE( removal_scheduler.begin_turn( 1, { alpha } ) );
    REQUIRE( removal_scheduler.mark_barrier_disconnected( alpha ) );
    REQUIRE( removal_scheduler.apply_disconnect_timeout(
                 alpha, multiplayer_disconnect_timeout_policy::remove_from_barrier ) );
    CHECK( removal_scheduler.current_slot()->state ==
           multiplayer_turn_participant_state::automatic_wait_pending );
    REQUIRE( removal_scheduler.record_automatic_wait_executed( alpha ) );
    CHECK( removal_scheduler.participant_count() == 1 );
    CHECK( removal_scheduler.has_participant( alpha.player_id ) );
    CHECK( removal_scheduler.participant_state( alpha.player_id ) ==
           multiplayer_turn_participant_state::removed );
    const std::optional<multiplayer_world_ticket> removal_ticket =
        removal_scheduler.claim_world();
    REQUIRE( removal_ticket );
    REQUIRE( removal_scheduler.record_world_completed( *removal_ticket ) );
    CHECK( removal_scheduler.participant_count() == 0 );
    CHECK_FALSE( removal_scheduler.has_participant( alpha.player_id ) );
}

TEST_CASE( "multiplayer_turn_scheduler_rotates_by_stable_player_successor_across_roster_churn",
           "[multiplayer][scheduler]" )
{
    multiplayer_turn_scheduler scheduler;
    const multiplayer_turn_participant_key alpha = participant( 1 );
    const multiplayer_turn_participant_key beta = participant( 2 );
    const multiplayer_turn_participant_key gamma = participant( 3 );
    const multiplayer_turn_participant_key delta = participant( 4 );

    const auto finish_turn = [&scheduler]() {
        while( const std::optional<multiplayer_turn_slot> slot = scheduler.current_slot() ) {
            REQUIRE( scheduler.record_action_result(
                         slot->participant,
                         multiplayer_turn_action_disposition::accepted_finished ) );
        }
        const std::optional<multiplayer_world_ticket> ticket = scheduler.claim_world();
        REQUIRE( ticket );
        REQUIRE( scheduler.record_world_completed( *ticket ) );
    };

    REQUIRE( scheduler.begin_turn( 1, { gamma, alpha, beta } ) );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->participant == alpha );
    finish_turn();

    REQUIRE( scheduler.begin_turn( 2, { gamma, alpha, beta } ) );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->participant == beta );
    finish_turn();

    REQUIRE( scheduler.begin_turn( 3, { gamma, beta } ) );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->participant == gamma );
    finish_turn();

    REQUIRE( scheduler.begin_turn( 4, { delta, gamma, alpha, beta } ) );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->participant == delta );
}

TEST_CASE( "multiplayer_turn_scheduler_four_players_have_no_starvation_and_one_world",
           "[multiplayer][scheduler]" )
{
    multiplayer_turn_scheduler scheduler;
    const std::vector<multiplayer_turn_participant_key> roster = {
        participant( 4 ), participant( 2 ), participant( 1 ), participant( 3 )
    };
    std::vector<multiplayer_turn_participant_key> sorted_roster = roster;
    std::sort( sorted_roster.begin(), sorted_roster.end(),
               []( const multiplayer_turn_participant_key & lhs,
    const multiplayer_turn_participant_key & rhs ) {
        return lhs.player_id.str() < rhs.player_id.str();
    } );
    std::map<std::string, int> first_counts;
    int world_count = 0;

    for( std::uint64_t turn = 0; turn < 8; ++turn ) {
        REQUIRE( scheduler.begin_turn( turn + 1, roster ) );
        REQUIRE( scheduler.current_slot() );
        ++first_counts[player_id_string( scheduler.current_slot()->participant )];
        CHECK( scheduler.current_slot()->participant ==
               sorted_roster[static_cast<std::size_t>( turn % sorted_roster.size() )] );

        std::map<std::string, int> actions;
        std::set<std::pair<std::uint64_t, std::string>> round_visits;
        while( const std::optional<multiplayer_turn_slot> slot = scheduler.current_slot() ) {
            const std::string id = player_id_string( slot->participant );
            CHECK( round_visits.emplace( slot->ordering.round, id ).second );
            const int action_count = ++actions[id];
            REQUIRE( scheduler.record_action_result(
                         slot->participant,
                         action_count < 3 ?
                         multiplayer_turn_action_disposition::accepted_remains_eligible :
                         multiplayer_turn_action_disposition::accepted_finished ) );
        }
        for( const multiplayer_turn_participant_key &entry : roster ) {
            CHECK( actions[player_id_string( entry )] == 3 );
        }
        const std::optional<multiplayer_world_ticket> ticket = scheduler.claim_world();
        REQUIRE( ticket );
        ++world_count;
        CHECK_FALSE( scheduler.claim_world() );
        REQUIRE( scheduler.record_world_completed( *ticket ) );
    }

    CHECK( world_count == 8 );
    for( const multiplayer_turn_participant_key &entry : roster ) {
        CHECK( first_counts[player_id_string( entry )] == 2 );
    }
}

TEST_CASE( "multiplayer_turn_scheduler_wait_only_registry_adapter_executes_before_policy_record",
           "[multiplayer][scheduler][player_bridge]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &alpha = get_avatar();
    map &here = get_map();
    alpha.setpos( here, tripoint_bub_ms( 60, 60, 0 ) );
    alpha.set_moves( 100 );
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    const safe_mode_type original_alpha_safe_mode = alpha_runtime->safe_mode();
    on_out_of_scope restore_alpha_safe_mode( [alpha_runtime, original_alpha_safe_mode]() {
        alpha_runtime->set_safe_mode( original_alpha_safe_mode );
    } );

    const shared_ptr_fast<avatar> beta_owner = make_shared_fast<avatar>();
    avatar &beta = *beta_owner;
    beta.create( character_type::NOW );
    clear_character( beta );
    beta.setID( g->assign_npc_id(), true );
    beta.setpos( here, tripoint_bub_ms( 62, 60, 0 ) );
    beta.set_moves( 100 );
    REQUIRE( g->register_multiplayer_player( beta_owner ) );
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime =
        g->multiplayer_players().find_runtime( beta );
    REQUIRE( beta_runtime );
    on_out_of_scope unregister_beta( [&beta, beta_runtime]() {
        if( beta_runtime->status() == multiplayer_player_status::active ) {
            g->disconnect_multiplayer_player( beta_runtime->player_id() );
        }
        g->unregister_multiplayer_player( beta );
    } );

    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 1, {
        { beta_runtime->player_id(), beta_runtime->session_generation() },
        { alpha_runtime->player_id(), alpha_runtime->session_generation() }
    } ) );

    std::set<std::string> waited_players;
    while( const std::optional<multiplayer_turn_slot> slot = scheduler.current_slot() ) {
        const shared_ptr_fast<multiplayer_player_runtime> runtime =
            g->multiplayer_players().find_by_player_id( slot->participant.player_id );
        REQUIRE( runtime );
        const bool timed_out = runtime == beta_runtime;
        if( timed_out ) {
            REQUIRE( scheduler.mark_barrier_disconnected( slot->participant ) );
            REQUIRE( scheduler.apply_disconnect_timeout(
                         slot->participant,
                         multiplayer_disconnect_timeout_policy::automatic_wait ) );
            REQUIRE( scheduler.current_slot() );
            CHECK( scheduler.current_slot()->state ==
                   multiplayer_turn_participant_state::automatic_wait_pending );
        }
        const safe_mode_type safe_mode_before_wait = runtime->safe_mode();
        {
            multiplayer_active_player_guard guard( *g, runtime->player_owner() );
            on_out_of_scope restore_safe_mode( [runtime, safe_mode_before_wait]() {
                runtime->set_safe_mode( safe_mode_before_wait );
            } );
            runtime->set_safe_mode( SAFE_MODE_OFF );
            CHECK( &get_avatar() == &runtime->player() );
            REQUIRE( multiplayer_execute_wait( *g, runtime->player(), true ) );
            CHECK( runtime->player().get_moves() <= 0 );
            waited_players.emplace( runtime->player_id().str() );
        }
        CHECK( &get_avatar() == &alpha );
        CHECK( runtime->safe_mode() == safe_mode_before_wait );
        if( timed_out ) {
            REQUIRE( scheduler.record_automatic_wait_executed( slot->participant ) );
        } else {
            REQUIRE( scheduler.record_action_result(
                         slot->participant,
                         multiplayer_turn_action_disposition::accepted_finished ) );
        }
    }

    CHECK( waited_players.size() == 2 );
    CHECK( alpha.get_moves() <= 0 );
    CHECK( beta.get_moves() <= 0 );
    const std::optional<multiplayer_world_ticket> ticket = scheduler.claim_world();
    REQUIRE( ticket );
    CHECK_FALSE( scheduler.claim_world() );
    REQUIRE( scheduler.record_world_completed( *ticket ) );
}

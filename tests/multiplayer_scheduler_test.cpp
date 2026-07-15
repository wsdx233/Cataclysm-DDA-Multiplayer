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

#include "multiplayer_session_generation.h"
#include "multiplayer_turn_scheduler.h"

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
    CHECK_FALSE( scheduler.begin_turn( 1, { {
            alpha.player_id,
            multiplayer_session_generation_exclusive_limit
        }
    } ) );
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

TEST_CASE( "multiplayer_turn_scheduler_session_generation_stays_below_int64_max",
           "[multiplayer][scheduler]" )
{
    const std::uint64_t maximum_generation =
        multiplayer_session_generation_exclusive_limit - 1;

    multiplayer_turn_scheduler final_valid_resume;
    const multiplayer_turn_participant_key penultimate =
        participant( 1, maximum_generation - 1 );
    REQUIRE( final_valid_resume.begin_turn( 1, { penultimate } ) );
    REQUIRE( final_valid_resume.mark_barrier_disconnected( penultimate ) );
    REQUIRE( final_valid_resume.resume_barrier_participant( penultimate,
             maximum_generation ) );
    REQUIRE( final_valid_resume.participant_key( penultimate.player_id ) );
    CHECK( final_valid_resume.participant_key(
               penultimate.player_id )->session_generation == maximum_generation );

    multiplayer_turn_scheduler exhausted_resume;
    const multiplayer_turn_participant_key exhausted = participant( 2, maximum_generation );
    REQUIRE( exhausted_resume.begin_turn( 1, { exhausted } ) );
    REQUIRE( exhausted_resume.mark_barrier_disconnected( exhausted ) );
    CHECK_FALSE( exhausted_resume.resume_barrier_participant(
                     exhausted, multiplayer_session_generation_exclusive_limit ) );
    REQUIRE( exhausted_resume.participant_key( exhausted.player_id ) );
    CHECK( exhausted_resume.participant_key(
               exhausted.player_id )->session_generation == maximum_generation );
    CHECK( exhausted_resume.participant_state( exhausted.player_id ) ==
           multiplayer_turn_participant_state::disconnected_grace );
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

TEST_CASE( "multiplayer_turn_scheduler_rebinds_only_an_exact_disconnected_replay",
           "[multiplayer][scheduler]" )
{
    multiplayer_turn_scheduler scheduler;
    const multiplayer_turn_participant_key alpha = participant( 1 );
    const multiplayer_turn_participant_key beta = participant( 2 );
    const multiplayer_turn_participant_key missing = participant( 3 );
    REQUIRE( scheduler.begin_turn( 1, { alpha, beta } ) );

    CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( alpha ) );
    REQUIRE( scheduler.mark_barrier_disconnected( alpha ) );
    CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( missing ) );
    CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( { alpha.player_id, 2 } ) );
    REQUIRE( scheduler.rebind_replayed_barrier_participant( alpha ) );
    REQUIRE( scheduler.participant_key( alpha.player_id ) );
    CHECK( *scheduler.participant_key( alpha.player_id ) == alpha );
    CHECK( scheduler.participant_state( alpha.player_id ) ==
           multiplayer_turn_participant_state::awaiting_command );
    CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( alpha ) );

    REQUIRE( scheduler.mark_barrier_disconnected( alpha ) );
    REQUIRE( scheduler.apply_disconnect_timeout(
                 alpha, multiplayer_disconnect_timeout_policy::automatic_wait ) );
    CHECK( scheduler.participant_state( alpha.player_id ) ==
           multiplayer_turn_participant_state::automatic_wait_pending );
    CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( alpha ) );
    REQUIRE( scheduler.record_automatic_wait_executed( alpha ) );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::player_actions );
    CHECK( scheduler.participant_state( alpha.player_id ) ==
           multiplayer_turn_participant_state::finished );
    CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( alpha ) );

    REQUIRE( scheduler.record_action_result(
                 beta, multiplayer_turn_action_disposition::accepted_finished ) );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_ready );
    CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( alpha ) );
    const std::optional<multiplayer_world_ticket> ticket = scheduler.claim_world();
    REQUIRE( ticket );
    CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( alpha ) );
    REQUIRE( scheduler.record_world_completed( *ticket ) );
    CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( alpha ) );
}

TEST_CASE( "multiplayer_turn_scheduler_repairs_disconnected_generation_without_resuming",
           "[multiplayer][scheduler]" )
{
    multiplayer_turn_scheduler scheduler;
    const multiplayer_turn_participant_key alpha = participant( 1 );
    const multiplayer_turn_participant_key missing = participant( 2 );
    const multiplayer_turn_participant_key repaired = { alpha.player_id, 2 };

    CHECK_FALSE( scheduler.repair_disconnected_barrier_generation( alpha, 2 ) );
    REQUIRE( scheduler.begin_turn( 1, { alpha } ) );
    CHECK_FALSE( scheduler.repair_disconnected_barrier_generation( alpha, 2 ) );
    REQUIRE( scheduler.mark_barrier_disconnected( alpha ) );

    CHECK_FALSE( scheduler.repair_disconnected_barrier_generation( missing, 2 ) );
    CHECK_FALSE( scheduler.repair_disconnected_barrier_generation(
    { alpha.player_id, 2 }, 3 ) );
    CHECK_FALSE( scheduler.repair_disconnected_barrier_generation( alpha, 1 ) );
    CHECK_FALSE( scheduler.repair_disconnected_barrier_generation( alpha, 3 ) );
    REQUIRE( scheduler.repair_disconnected_barrier_generation( alpha, 2 ) );

    REQUIRE( scheduler.participant_key( alpha.player_id ) );
    CHECK( *scheduler.participant_key( alpha.player_id ) == repaired );
    CHECK( scheduler.participant_state( alpha.player_id ) ==
           multiplayer_turn_participant_state::disconnected_grace );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->participant == repaired );
    CHECK( scheduler.current_slot()->state ==
           multiplayer_turn_participant_state::disconnected_grace );

    CHECK_FALSE( scheduler.apply_disconnect_timeout(
                     alpha, multiplayer_disconnect_timeout_policy::automatic_wait ) );
    REQUIRE( scheduler.apply_disconnect_timeout(
                 repaired, multiplayer_disconnect_timeout_policy::automatic_wait ) );
    CHECK_FALSE( scheduler.repair_disconnected_barrier_generation( repaired, 3 ) );
    REQUIRE( scheduler.record_automatic_wait_executed( repaired ) );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_ready );
    CHECK_FALSE( scheduler.repair_disconnected_barrier_generation( repaired, 3 ) );

    const std::optional<multiplayer_world_ticket> ticket = scheduler.claim_world();
    REQUIRE( ticket );
    CHECK_FALSE( scheduler.repair_disconnected_barrier_generation( repaired, 3 ) );
    REQUIRE( scheduler.record_world_completed( *ticket ) );
    CHECK_FALSE( scheduler.repair_disconnected_barrier_generation( repaired, 3 ) );
}

TEST_CASE( "multiplayer_turn_scheduler_resume_and_timeout_races_have_one_winner",
           "[multiplayer][scheduler]" )
{
    multiplayer_turn_scheduler scheduler;
    const multiplayer_turn_participant_key alpha = participant( 1 );
    REQUIRE( scheduler.begin_turn( 1, { alpha } ) );
    REQUIRE( scheduler.mark_barrier_disconnected( alpha ) );

    SECTION( "next-generation resume wins before timeout" ) {
        REQUIRE( scheduler.resume_barrier_participant( alpha, 2 ) );
        const multiplayer_turn_participant_key resumed = { alpha.player_id, 2 };
        CHECK_FALSE( scheduler.apply_disconnect_timeout(
                         alpha, multiplayer_disconnect_timeout_policy::automatic_wait ) );
        CHECK_FALSE( scheduler.apply_disconnect_timeout(
                         resumed, multiplayer_disconnect_timeout_policy::automatic_wait ) );
        CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( alpha ) );
        CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( resumed ) );
        REQUIRE( scheduler.participant_key( alpha.player_id ) );
        CHECK( *scheduler.participant_key( alpha.player_id ) == resumed );
        CHECK( scheduler.participant_state( alpha.player_id ) ==
               multiplayer_turn_participant_state::awaiting_command );
    }

    SECTION( "same-generation replay rebind wins before timeout" ) {
        REQUIRE( scheduler.rebind_replayed_barrier_participant( alpha ) );
        CHECK_FALSE( scheduler.apply_disconnect_timeout(
                         alpha, multiplayer_disconnect_timeout_policy::automatic_wait ) );
        CHECK_FALSE( scheduler.resume_barrier_participant( alpha, 2 ) );
        CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( alpha ) );
        REQUIRE( scheduler.participant_key( alpha.player_id ) );
        CHECK( *scheduler.participant_key( alpha.player_id ) == alpha );
        CHECK( scheduler.participant_state( alpha.player_id ) ==
               multiplayer_turn_participant_state::awaiting_command );
    }

    SECTION( "timeout wins before either resume path" ) {
        REQUIRE( scheduler.apply_disconnect_timeout(
                     alpha, multiplayer_disconnect_timeout_policy::automatic_wait ) );
        CHECK_FALSE( scheduler.resume_barrier_participant( alpha, 2 ) );
        CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( alpha ) );
        CHECK( scheduler.participant_state( alpha.player_id ) ==
               multiplayer_turn_participant_state::automatic_wait_pending );
        REQUIRE( scheduler.record_automatic_wait_executed( alpha ) );
        CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_ready );
        CHECK_FALSE( scheduler.resume_barrier_participant( alpha, 2 ) );
        CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( alpha ) );
    }
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

TEST_CASE( "multiplayer_turn_scheduler_execution_fault_permanently_blocks_transitions",
           "[multiplayer][scheduler][phase_adapter]" )
{
    multiplayer_turn_scheduler scheduler;
    const multiplayer_turn_participant_key alpha = participant( 1 );
    const multiplayer_turn_participant_key beta = participant( 2 );
    REQUIRE( scheduler.begin_turn( 1, { alpha, beta } ) );
    REQUIRE( scheduler.current_slot() );
    const multiplayer_turn_participant_key current = scheduler.current_slot()->participant;

    scheduler.latch_execution_fault();

    CHECK( scheduler.is_faulted() );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::player_actions );
    CHECK_FALSE( scheduler.current_slot() );
    CHECK( scheduler.participant_state( current.player_id ) ==
           multiplayer_turn_participant_state::awaiting_command );
    CHECK_FALSE( scheduler.record_action_result(
                     current,
                     multiplayer_turn_action_disposition::accepted_finished ) );
    CHECK_FALSE( scheduler.mark_barrier_disconnected( current ) );
    CHECK_FALSE( scheduler.resume_barrier_participant( current, 2 ) );
    CHECK_FALSE( scheduler.rebind_replayed_barrier_participant( current ) );
    CHECK_FALSE( scheduler.repair_disconnected_barrier_generation( current, 2 ) );
    CHECK_FALSE( scheduler.apply_disconnect_timeout(
                     current, multiplayer_disconnect_timeout_policy::automatic_wait ) );
    CHECK_FALSE( scheduler.record_automatic_wait_executed( current ) );
    CHECK_FALSE( scheduler.claim_world() );
    CHECK_FALSE( scheduler.record_world_completed( { 1 } ) );
    CHECK_FALSE( scheduler.begin_turn( 2, { alpha, beta } ) );
}

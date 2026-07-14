#include "cata_catch.h"

#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "avatar.h"
#include "cata_scope_helpers.h"
#include "calendar.h"
#include "enums.h"
#include "game.h"
#include "game_constants.h"
#include "map.h"
#include "map_helpers.h"
#include "memory_fast.h"
#include "multiplayer_command_executor.h"
#include "multiplayer_player_context.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_player_runtime.h"
#include "multiplayer_turn_phase_adapter.h"
#include "multiplayer_turn_scheduler.h"
#include "player_helpers.h"
#include "type_id.h"

namespace
{

static_assert( !std::is_copy_constructible_v<multiplayer_turn_phase_adapter> );
static_assert( !std::is_copy_assignable_v<multiplayer_turn_phase_adapter> );
static_assert( !std::is_move_constructible_v<multiplayer_turn_phase_adapter> );
static_assert( !std::is_move_assignable_v<multiplayer_turn_phase_adapter> );

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

} // namespace

TEST_CASE( "multiplayer_turn_phase_adapter_scopes_player_callbacks_and_safe_mode",
           "[multiplayer][phase_adapter][player_bridge]" )
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
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    const safe_mode_type original_alpha_safe_mode = alpha_runtime->safe_mode();
    on_out_of_scope restore_alpha_safe_mode( [alpha_runtime, original_alpha_safe_mode]() {
        alpha_runtime->set_safe_mode( original_alpha_safe_mode );
    } );
    alpha_runtime->set_safe_mode( SAFE_MODE_OFF );
    alpha.set_moves( 100 );
    alpha.add_effect( efftype_id( "laserlocked" ), 1_turns );

    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime = secondary.runtime();
    beta_runtime->set_safe_mode( SAFE_MODE_OFF );
    secondary.player().set_moves( 100 );
    const multiplayer_turn_participant_key beta_key = participant_key( *beta_runtime );

    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 1, { beta_key } ) );
    multiplayer_turn_phase_adapter adapter( *g, scheduler );
    const int actions_before = g->get_moves_since_last_save();
    int callback_count = 0;
    const multiplayer_turn_phase_adapter_status status = adapter.execute_current_player(
                beta_key,
    [&]( multiplayer_player_runtime & runtime, avatar & player ) {
        ++callback_count;
        CHECK( &runtime == beta_runtime.get() );
        CHECK( &player == &secondary.player() );
        CHECK( &get_avatar() == &secondary.player() );
        CHECK( &g->active_player_runtime() == beta_runtime.get() );
        CHECK( g->get_safe_mode() == SAFE_MODE_OFF );
        const bool waited = multiplayer_execute_wait(
                                *g, player,
                                multiplayer_wait_execution_mode::player_requested );
        CHECK( waited );
        if( !waited ) {
            return std::optional<multiplayer_turn_action_disposition>();
        }
        runtime.set_safe_mode( SAFE_MODE_ON );
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } );

    CHECK( status == multiplayer_turn_phase_adapter_status::completed );
    CHECK( callback_count == 1 );
    CHECK_FALSE( adapter.is_faulted() );
    CHECK( &get_avatar() == &alpha );
    CHECK( &g->active_player_runtime() == alpha_runtime.get() );
    CHECK( alpha_runtime->safe_mode() == SAFE_MODE_OFF );
    CHECK( beta_runtime->safe_mode() == SAFE_MODE_ON );
    CHECK( alpha.get_moves() == 100 );
    CHECK( secondary.player().get_moves() <= 0 );
    CHECK( g->get_moves_since_last_save() == actions_before + 1 );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_ready );

    int world_count = 0;
    {
        multiplayer_active_player_guard non_root_context( *g, beta_runtime->player_owner() );
        REQUIRE( non_root_context.is_engaged() );
        CHECK( adapter.execute_claimed_world(
        [&]( const multiplayer_world_ticket & ) {
            ++world_count;
            return true;
        } ) == multiplayer_turn_phase_adapter_status::activation_failed );
        CHECK( world_count == 0 );
        CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_ready );
    }
    REQUIRE( adapter.execute_claimed_world(
    [&]( const multiplayer_world_ticket & ) {
        ++world_count;
        return true;
    } ) == multiplayer_turn_phase_adapter_status::completed );
    CHECK( world_count == 1 );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::idle );
}

TEST_CASE( "multiplayer_turn_phase_adapter_player_callback_failure_restores_and_latches",
           "[multiplayer][phase_adapter][player_bridge]" )
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
    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    const multiplayer_turn_participant_key beta_key = participant_key( *secondary.runtime() );

    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 1, { beta_key } ) );
    multiplayer_turn_phase_adapter adapter( *g, scheduler );
    const int actions_before = g->get_moves_since_last_save();
    int callback_count = 0;
    bool callback_throws = false;
    SECTION( "callback returns no disposition" ) {
        callback_throws = false;
    }
    SECTION( "callback throws" ) {
        callback_throws = true;
    }
    const multiplayer_turn_phase_adapter_status status = adapter.execute_current_player(
                beta_key,
                [&]( multiplayer_player_runtime & runtime,
    avatar & player ) -> std::optional<multiplayer_turn_action_disposition> {
        ++callback_count;
        CHECK( &runtime == secondary.runtime().get() );
        CHECK( &player == &secondary.player() );
        CHECK( &get_avatar() == &secondary.player() );
        if( callback_throws )
        {
            throw std::runtime_error( "player callback failed" );
        }
        return std::nullopt;
    } );

    CHECK( status == multiplayer_turn_phase_adapter_status::player_callback_failed );
    CHECK( adapter.is_faulted() );
    CHECK( scheduler.is_faulted() );
    CHECK( callback_count == 1 );
    CHECK( &get_avatar() == &alpha );
    CHECK( g->get_moves_since_last_save() == actions_before );
    CHECK_FALSE( scheduler.current_slot() );
    CHECK( scheduler.participant_state( beta_key.player_id ) ==
           multiplayer_turn_participant_state::awaiting_command );

    CHECK( adapter.execute_current_player(
               beta_key,
    [&]( multiplayer_player_runtime &, avatar & ) {
        ++callback_count;
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } ) == multiplayer_turn_phase_adapter_status::faulted );
    CHECK( callback_count == 1 );

    multiplayer_turn_phase_adapter replacement_adapter( *g, scheduler );
    CHECK( replacement_adapter.is_faulted() );
    CHECK( replacement_adapter.execute_current_player(
               beta_key,
    [&]( multiplayer_player_runtime &, avatar & ) {
        ++callback_count;
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } ) == multiplayer_turn_phase_adapter_status::faulted );
    CHECK( callback_count == 1 );
}

TEST_CASE( "multiplayer_turn_phase_adapter_record_failure_after_action_is_fail_stop",
           "[multiplayer][phase_adapter][player_bridge]" )
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
    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    const multiplayer_turn_participant_key beta_key = participant_key( *secondary.runtime() );

    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 1, { beta_key } ) );
    multiplayer_turn_phase_adapter adapter( *g, scheduler );
    const int actions_before = g->get_moves_since_last_save();
    int callback_count = 0;

    const multiplayer_turn_phase_adapter_status status = adapter.execute_current_player(
                beta_key,
    [&]( multiplayer_player_runtime &, avatar & player ) {
        ++callback_count;
        const bool waited = multiplayer_execute_wait(
                                *g, player,
                                multiplayer_wait_execution_mode::authoritative_forced );
        CHECK( waited );
        if( !waited ) {
            return std::optional<multiplayer_turn_action_disposition>();
        }

        // Deliberately violate the trusted-callback contract after the real
        // action to inject a post-side-effect scheduler record failure.
        const bool advanced = scheduler.record_action_result(
                                  beta_key,
                                  multiplayer_turn_action_disposition::accepted_finished );
        CHECK( advanced );
        if( !advanced ) {
            return std::optional<multiplayer_turn_action_disposition>();
        }
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } );

    CHECK( status == multiplayer_turn_phase_adapter_status::scheduler_record_failed );
    CHECK( callback_count == 1 );
    CHECK( secondary.player().get_moves() <= 0 );
    CHECK( g->get_moves_since_last_save() == actions_before + 1 );
    CHECK( &get_avatar() == &alpha );
    CHECK( adapter.is_faulted() );
    CHECK( scheduler.is_faulted() );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_ready );
    CHECK_FALSE( scheduler.claim_world() );

    multiplayer_turn_phase_adapter replacement_adapter( *g, scheduler );
    CHECK( replacement_adapter.is_faulted() );
    CHECK( replacement_adapter.execute_current_player(
               beta_key,
    [&]( multiplayer_player_runtime &, avatar & ) {
        ++callback_count;
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } ) == multiplayer_turn_phase_adapter_status::faulted );
    CHECK( callback_count == 1 );
}

TEST_CASE( "multiplayer_turn_phase_adapter_authoritative_wait_executes_real_pause_before_record",
           "[multiplayer][phase_adapter][player_bridge]" )
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
    alpha.set_moves( 73 );
    alpha.recoil = 17;
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    const safe_mode_type original_alpha_safe_mode = alpha_runtime->safe_mode();
    on_out_of_scope restore_alpha_safe_mode( [alpha_runtime, original_alpha_safe_mode]() {
        alpha_runtime->set_safe_mode( original_alpha_safe_mode );
    } );
    alpha_runtime->set_safe_mode( SAFE_MODE_OFF );

    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime = secondary.runtime();
    beta_runtime->set_safe_mode( SAFE_MODE_STOP );
    secondary.player().set_moves( 100 );
    secondary.player().recoil = 0;
    const multiplayer_turn_participant_key beta_key = participant_key( *beta_runtime );

    multiplayer_disconnect_timeout_policy timeout_policy =
        multiplayer_disconnect_timeout_policy::automatic_wait;
    multiplayer_turn_participant_state expected_final_state =
        multiplayer_turn_participant_state::finished;
    SECTION( "automatic wait keeps participant in the roster" ) {
        timeout_policy = multiplayer_disconnect_timeout_policy::automatic_wait;
        expected_final_state = multiplayer_turn_participant_state::finished;
    }
    SECTION( "remove policy still executes wait before removal" ) {
        timeout_policy = multiplayer_disconnect_timeout_policy::remove_from_barrier;
        expected_final_state = multiplayer_turn_participant_state::removed;
    }

    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 1, { beta_key } ) );
    REQUIRE( scheduler.mark_barrier_disconnected( beta_key ) );
    REQUIRE( scheduler.apply_disconnect_timeout(
                 beta_key, timeout_policy ) );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->state ==
           multiplayer_turn_participant_state::automatic_wait_pending );

    multiplayer_turn_phase_adapter adapter( *g, scheduler );
    const int actions_before = g->get_moves_since_last_save();
    const multiplayer_turn_phase_adapter_status status =
        adapter.execute_authoritative_wait( beta_key );

    CHECK( status == multiplayer_turn_phase_adapter_status::completed );
    CHECK_FALSE( adapter.is_faulted() );
    CHECK( secondary.player().get_moves() <= 0 );
    CHECK( secondary.player().recoil == MAX_RECOIL );
    CHECK( beta_runtime->safe_mode() == SAFE_MODE_STOP );
    CHECK( alpha.get_moves() == 73 );
    CHECK( alpha.recoil == 17 );
    CHECK( g->get_moves_since_last_save() == actions_before + 1 );
    CHECK( &get_avatar() == &alpha );
    CHECK( &g->active_player_runtime() == alpha_runtime.get() );
    CHECK( scheduler.participant_state( beta_key.player_id ) == expected_final_state );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_ready );
}

TEST_CASE( "multiplayer_turn_phase_adapter_stale_or_inactive_runtime_never_records_wait",
           "[multiplayer][phase_adapter][player_bridge]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &alpha = get_avatar();
    alpha.set_moves( 100 );
    map &here = get_map();
    alpha.setpos( here, tripoint_bub_ms( 60, 60, 0 ) );
    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime = secondary.runtime();
    const multiplayer_turn_participant_key beta_key = participant_key( *beta_runtime );
    secondary.player().set_moves( 100 );
    secondary.player().recoil = 0;

    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 1, { beta_key } ) );
    REQUIRE( scheduler.mark_barrier_disconnected( beta_key ) );
    REQUIRE( scheduler.apply_disconnect_timeout(
                 beta_key, multiplayer_disconnect_timeout_policy::automatic_wait ) );
    multiplayer_turn_phase_adapter adapter( *g, scheduler );

    multiplayer_turn_phase_adapter_status expected_status =
        multiplayer_turn_phase_adapter_status::inactive_runtime;
    SECTION( "inactive runtime" ) {
        REQUIRE( g->disconnect_multiplayer_player( beta_runtime->player_id() ) );
        REQUIRE( beta_runtime->status() == multiplayer_player_status::offline );
        expected_status = multiplayer_turn_phase_adapter_status::inactive_runtime;
    }
    SECTION( "stale session generation" ) {
        REQUIRE( g->disconnect_multiplayer_player( beta_runtime->player_id() ) );
        REQUIRE( g->begin_multiplayer_player_session( beta_runtime->player_id() ) );
        REQUIRE( beta_runtime->status() == multiplayer_player_status::active );
        REQUIRE( beta_runtime->session_generation() == beta_key.session_generation + 1 );
        expected_status = multiplayer_turn_phase_adapter_status::stale_session_generation;
    }

    CHECK( adapter.execute_authoritative_wait( beta_key ) == expected_status );
    CHECK_FALSE( adapter.is_faulted() );
    CHECK( secondary.player().get_moves() == 100 );
    CHECK( secondary.player().recoil == 0 );
    CHECK( &get_avatar() == &alpha );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->participant == beta_key );
    CHECK( scheduler.current_slot()->state ==
           multiplayer_turn_participant_state::automatic_wait_pending );
}

TEST_CASE( "multiplayer_turn_phase_adapter_missing_runtime_never_records_wait",
           "[multiplayer][phase_adapter][player_bridge]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    const multiplayer_turn_participant_key missing_key = {
        multiplayer_player_id::random(), 1
    };
    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 1, { missing_key } ) );
    REQUIRE( scheduler.mark_barrier_disconnected( missing_key ) );
    REQUIRE( scheduler.apply_disconnect_timeout(
                 missing_key, multiplayer_disconnect_timeout_policy::automatic_wait ) );
    multiplayer_turn_phase_adapter adapter( *g, scheduler );

    CHECK( adapter.execute_authoritative_wait( missing_key ) ==
           multiplayer_turn_phase_adapter_status::runtime_not_found );
    CHECK_FALSE( adapter.is_faulted() );
    REQUIRE( scheduler.current_slot() );
    CHECK( scheduler.current_slot()->participant == missing_key );
    CHECK( scheduler.current_slot()->state ==
           multiplayer_turn_participant_state::automatic_wait_pending );
}

TEST_CASE( "multiplayer_turn_phase_adapter_claims_and_records_world_exactly_once",
           "[multiplayer][phase_adapter]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &alpha = get_avatar();
    alpha.set_moves( 100 );
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    const multiplayer_turn_participant_key alpha_key = participant_key( *alpha_runtime );

    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 7, { alpha_key } ) );
    multiplayer_turn_phase_adapter adapter( *g, scheduler );
    REQUIRE( adapter.execute_current_player(
                 alpha_key,
    []( multiplayer_player_runtime &, avatar & player ) {
        if( !multiplayer_execute_wait(
                *g, player, multiplayer_wait_execution_mode::authoritative_forced ) ) {
            return std::optional<multiplayer_turn_action_disposition>();
        }
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } ) == multiplayer_turn_phase_adapter_status::completed );
    REQUIRE( scheduler.stage() == multiplayer_turn_scheduler_stage::world_ready );

    int world_count = 0;
    const multiplayer_turn_phase_adapter_status status = adapter.execute_claimed_world(
    [&]( const multiplayer_world_ticket & ticket ) {
        ++world_count;
        CHECK( ticket.shared_turn == 7 );
        CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_processing );
        CHECK( &get_avatar() == &alpha );
        return true;
    } );

    CHECK( status == multiplayer_turn_phase_adapter_status::completed );
    CHECK_FALSE( adapter.is_faulted() );
    CHECK( world_count == 1 );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::idle );
    CHECK( adapter.execute_claimed_world(
    [&]( const multiplayer_world_ticket & ) {
        ++world_count;
        return true;
    } ) == multiplayer_turn_phase_adapter_status::world_already_attempted );
    CHECK( world_count == 1 );
}

TEST_CASE( "multiplayer_turn_phase_adapter_world_failure_is_fail_stop",
           "[multiplayer][phase_adapter]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &alpha = get_avatar();
    alpha.set_moves( 100 );
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    const multiplayer_turn_participant_key alpha_key = participant_key( *alpha_runtime );

    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 9, { alpha_key } ) );
    multiplayer_turn_phase_adapter adapter( *g, scheduler );
    REQUIRE( adapter.execute_current_player(
                 alpha_key,
    []( multiplayer_player_runtime &, avatar & player ) {
        if( !multiplayer_execute_wait(
                *g, player, multiplayer_wait_execution_mode::authoritative_forced ) ) {
            return std::optional<multiplayer_turn_action_disposition>();
        }
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } ) == multiplayer_turn_phase_adapter_status::completed );

    int world_count = 0;
    bool callback_throws = false;
    SECTION( "callback reports failure" ) {
        callback_throws = false;
    }
    SECTION( "callback throws" ) {
        callback_throws = true;
    }
    CHECK( adapter.execute_claimed_world(
    [&]( const multiplayer_world_ticket & ticket ) {
        ++world_count;
        CHECK( ticket.shared_turn == 9 );
        CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_processing );
        if( callback_throws ) {
            throw std::runtime_error( "world callback failed" );
        }
        return false;
    } ) == multiplayer_turn_phase_adapter_status::world_callback_failed );
    CHECK( adapter.is_faulted() );
    CHECK( scheduler.is_faulted() );
    CHECK( world_count == 1 );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_processing );
    CHECK_FALSE( scheduler.begin_turn( 10, { alpha_key } ) );

    CHECK( adapter.execute_claimed_world(
    [&]( const multiplayer_world_ticket & ) {
        ++world_count;
        return true;
    } ) == multiplayer_turn_phase_adapter_status::faulted );
    CHECK( world_count == 1 );

    multiplayer_turn_phase_adapter replacement_adapter( *g, scheduler );
    CHECK( replacement_adapter.is_faulted() );
    CHECK( replacement_adapter.execute_claimed_world(
    [&]( const multiplayer_world_ticket & ) {
        ++world_count;
        return true;
    } ) == multiplayer_turn_phase_adapter_status::faulted );
    CHECK( world_count == 1 );
}

TEST_CASE( "multiplayer_turn_phase_adapter_runs_two_runtime_wait_only_barrier",
           "[multiplayer][phase_adapter][player_bridge]" )
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
    alpha_runtime->set_safe_mode( SAFE_MODE_STOP );

    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime = secondary.runtime();
    beta_runtime->set_safe_mode( SAFE_MODE_STOP );
    secondary.player().set_moves( 100 );

    const multiplayer_turn_participant_key alpha_key = participant_key( *alpha_runtime );
    const multiplayer_turn_participant_key beta_key = participant_key( *beta_runtime );
    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 1, { beta_key, alpha_key } ) );
    multiplayer_turn_phase_adapter adapter( *g, scheduler );

    std::set<std::string> waited_players;
    while( const std::optional<multiplayer_turn_slot> slot = scheduler.current_slot() ) {
        if( slot->participant.player_id == beta_runtime->player_id() ) {
            REQUIRE( scheduler.mark_barrier_disconnected( slot->participant ) );
            REQUIRE( scheduler.apply_disconnect_timeout(
                         slot->participant,
                         multiplayer_disconnect_timeout_policy::automatic_wait ) );
            REQUIRE( adapter.execute_authoritative_wait( slot->participant ) ==
                     multiplayer_turn_phase_adapter_status::completed );
            waited_players.emplace( beta_runtime->player_id().str() );
        } else {
            REQUIRE( adapter.execute_current_player(
                         slot->participant,
            [&]( multiplayer_player_runtime & runtime, avatar & player ) {
                CHECK( &runtime == alpha_runtime.get() );
                CHECK( &player == &alpha );
                CHECK( &get_avatar() == &alpha );
                const bool waited = multiplayer_execute_wait(
                                        *g, player,
                                        multiplayer_wait_execution_mode::authoritative_forced );
                CHECK( waited );
                if( !waited ) {
                    return std::optional<multiplayer_turn_action_disposition>();
                }
                waited_players.emplace( runtime.player_id().str() );
                return std::optional<multiplayer_turn_action_disposition>(
                           multiplayer_turn_action_disposition::accepted_finished );
            } ) == multiplayer_turn_phase_adapter_status::completed );
        }
        CHECK( &get_avatar() == &alpha );
    }

    CHECK( waited_players.size() == 2 );
    CHECK( alpha.get_moves() <= 0 );
    CHECK( secondary.player().get_moves() <= 0 );
    REQUIRE( scheduler.stage() == multiplayer_turn_scheduler_stage::world_ready );

    int world_count = 0;
    REQUIRE( adapter.execute_claimed_world(
    [&]( const multiplayer_world_ticket & ticket ) {
        ++world_count;
        CHECK( ticket.shared_turn == 1 );
        CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::world_processing );
        CHECK( &get_avatar() == &alpha );
        return true;
    } ) == multiplayer_turn_phase_adapter_status::completed );
    CHECK( world_count == 1 );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::idle );
}

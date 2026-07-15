#include "cata_catch.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "avatar.h"
#include "cata_scope_helpers.h"
#include "game.h"
#include "map.h"
#include "map_helpers.h"
#include "memory_fast.h"
#include "multiplayer_command_executor.h"
#include "multiplayer_multi_runtime_barrier_owner.h"
#include "multiplayer_player_context.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_player_runtime.h"
#include "options_helpers.h"
#include "player_helpers.h"

namespace
{

static_assert( !std::is_copy_constructible_v<multiplayer_multi_runtime_barrier_owner> );
static_assert( !std::is_copy_assignable_v<multiplayer_multi_runtime_barrier_owner> );
static_assert( !std::is_move_constructible_v<multiplayer_multi_runtime_barrier_owner> );
static_assert( !std::is_move_assignable_v<multiplayer_multi_runtime_barrier_owner> );

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

class registered_same_key_replacement
{
    public:
        registered_same_key_replacement( map &here, const tripoint_bub_ms &position,
                                         const multiplayer_turn_participant_key &key ) :
            registry_( const_cast<multiplayer_player_registry &>( g->multiplayer_players() ) ),
            owner_( make_shared_fast<avatar>() ) {
            owner_->create( character_type::NOW );
            clear_character( *owner_ );
            owner_->setID( g->assign_npc_id(), true );
            owner_->setpos( here, position );
            runtime_ = make_shared_fast<multiplayer_player_runtime>(
                           owner_, multiplayer_player_runtime::achievement_callback(),
                           multiplayer_player_runtime::achievement_callback(), key.player_id,
                           key.session_generation - 1 );
            registered_ = registry_.register_player( runtime_ );
            active_ = registered_ && registry_.begin_session( key.player_id );
        }

        ~registered_same_key_replacement() {
            if( active_ ) {
                registry_.disconnect( runtime_->player_id() );
            }
            if( registered_ ) {
                registry_.unregister_player( *owner_ );
            }
        }

        registered_same_key_replacement( const registered_same_key_replacement & ) = delete;
        registered_same_key_replacement &operator=(
            const registered_same_key_replacement & ) = delete;

        bool valid() const {
            return registered_ && active_ && runtime_ != nullptr;
        }

        avatar &player() const {
            return *owner_;
        }

        const shared_ptr_fast<multiplayer_player_runtime> &runtime() const {
            return runtime_;
        }

    private:
        multiplayer_player_registry &registry_;
        shared_ptr_fast<avatar> owner_;
        shared_ptr_fast<multiplayer_player_runtime> runtime_;
        bool registered_ = false;
        bool active_ = false;
};

std::unique_ptr<multiplayer_multi_runtime_barrier_owner> make_owner(
    const std::size_t maximum_participants = 2 )
{
    std::string error;
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner =
        multiplayer_multi_runtime_barrier_owner::create(
            *g, maximum_participants, error );
    INFO( error );
    REQUIRE( owner );
    CHECK( error.empty() );
    return owner;
}

multiplayer_turn_phase_adapter_status execute_forced_wait(
    multiplayer_multi_runtime_barrier_owner &owner,
    const multiplayer_turn_participant_key &participant,
    std::set<std::string> *waited_players = nullptr )
{
    const shared_ptr_fast<multiplayer_player_runtime> expected_runtime =
        g->multiplayer_players().find_by_player_id( participant.player_id );
    REQUIRE( expected_runtime );
    return owner.execute_current_player(
               participant,
    [&]( multiplayer_player_runtime & runtime, avatar & player ) {
        CHECK( &runtime == expected_runtime.get() );
        CHECK( runtime.player_id() == participant.player_id );
        CHECK( runtime.session_generation() == participant.session_generation );
        CHECK( &player == &runtime.player() );
        CHECK( &get_avatar() == &player );
        const bool waited = multiplayer_execute_wait(
                                *g, player,
                                multiplayer_wait_execution_mode::authoritative_forced );
        CHECK( waited );
        if( !waited ) {
            return std::optional<multiplayer_turn_action_disposition>();
        }
        if( waited_players != nullptr ) {
            waited_players->emplace( runtime.player_id().str() );
        }
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } );
}

void complete_reported_player_end(
    multiplayer_multi_runtime_barrier_owner &owner )
{
    const std::optional<multiplayer_multi_runtime_world_completion> completion =
        owner.pending_world_completion();
    REQUIRE( completion );
    CHECK( owner.record_external_player_end_completed( *completion ) ==
           multiplayer_multi_runtime_barrier_status::applied );
    CHECK( owner.record_external_player_end_completed( *completion ) ==
           multiplayer_multi_runtime_barrier_status::duplicate );
}

multiplayer_multi_runtime_world_completion complete_zero_action_barrier_to_world(
    multiplayer_multi_runtime_barrier_owner &owner,
    const std::vector<multiplayer_turn_participant_key> &roster )
{
    REQUIRE( owner.begin_turn( roster ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    while( const std::optional<multiplayer_turn_slot> slot = owner.current_slot() ) {
        REQUIRE( owner.complete_player_phase( slot->participant ) ==
                 multiplayer_multi_runtime_barrier_status::applied );
    }
    REQUIRE( owner.execute_world( []( const multiplayer_world_ticket & ) {
        return true;
    } ) == multiplayer_turn_phase_adapter_status::completed );
    const std::optional<multiplayer_multi_runtime_world_completion> completion =
        owner.pending_world_completion();
    REQUIRE( completion );
    return *completion;
}

} // namespace

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_validates_capacity_and_full_roster",
           "[multiplayer][multi_runtime_barrier_owner][player_bridge]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    std::string error;
    CHECK_FALSE( multiplayer_multi_runtime_barrier_owner::create( *g, 1, error ) );
    CHECK( error == "multi-runtime barrier owner capacity must be between 2 and 4" );
    CHECK_FALSE( multiplayer_multi_runtime_barrier_owner::create( *g, 5, error ) );
    CHECK( error == "multi-runtime barrier owner capacity must be between 2 and 4" );

    avatar &alpha = get_avatar();
    map &here = get_map();
    alpha.setpos( here, tripoint_bub_ms( 60, 60, 0 ) );
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime = secondary.runtime();
    const multiplayer_turn_participant_key alpha_key = participant_key( *alpha_runtime );
    const multiplayer_turn_participant_key beta_key = participant_key( *beta_runtime );
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();

    CHECK( owner->maximum_participants() == 2 );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::idle );
    CHECK_FALSE( owner->has_open_boundary() );
    CHECK( owner->begin_turn( {} ) ==
           multiplayer_multi_runtime_barrier_status::invalid_roster );
    CHECK( owner->begin_turn( { alpha_key, alpha_key } ) ==
           multiplayer_multi_runtime_barrier_status::invalid_roster );
    CHECK( owner->begin_turn( { alpha_key, beta_key,
        { multiplayer_player_id::random(), 1 } } ) ==
    multiplayer_multi_runtime_barrier_status::invalid_roster );

    multiplayer_turn_participant_key stale_beta = beta_key;
    ++stale_beta.session_generation;
    CHECK( owner->begin_turn( { alpha_key, stale_beta } ) ==
           multiplayer_multi_runtime_barrier_status::stale_session_generation );
    CHECK( owner->begin_turn( { alpha_key,
        { multiplayer_player_id::random(), 1 } } ) ==
    multiplayer_multi_runtime_barrier_status::runtime_not_found );
    CHECK_FALSE( owner->is_faulted() );
    CHECK_FALSE( owner->active_shared_turn() );

    REQUIRE( g->disconnect_multiplayer_player( beta_runtime->player_id() ) );
    CHECK( owner->begin_turn( { alpha_key, beta_key } ) ==
           multiplayer_multi_runtime_barrier_status::inactive_runtime );
    REQUIRE( g->begin_multiplayer_player_session( beta_runtime->player_id() ) );
    const multiplayer_turn_participant_key reactivated_beta_key =
        participant_key( *beta_runtime );

    REQUIRE( owner->begin_turn( { alpha_key, reactivated_beta_key } ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    REQUIRE( owner->active_shared_turn() );
    CHECK( *owner->active_shared_turn() == 1 );
    CHECK( owner->participant_count() == 2 );
}

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_allows_single_online_roster_with_multi_capacity",
           "[multiplayer][multi_runtime_barrier_owner][roster]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &alpha = get_avatar();
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    const multiplayer_turn_participant_key alpha_key = participant_key( *alpha_runtime );
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner( 4 );

    REQUIRE( owner->begin_turn( { alpha_key } ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    CHECK( owner->participant_count() == 1 );
    REQUIRE( owner->complete_player_phase( alpha_key ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    REQUIRE( owner->stage() == multiplayer_multi_runtime_barrier_stage::world_ready );
    REQUIRE( owner->execute_world( []( const multiplayer_world_ticket & ticket ) {
        return ticket.shared_turn == 1;
    } ) == multiplayer_turn_phase_adapter_status::completed );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::player_end_pending );
    complete_reported_player_end( *owner );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::idle );
    CHECK_FALSE( owner->has_open_boundary() );
}

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_rejects_noncurrent_commands_without_side_effects",
           "[multiplayer][multi_runtime_barrier_owner][routing]" )
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
    alpha.setpos( here, tripoint_bub_ms( 60, 60, 0 ) );
    alpha.set_moves( 100 );
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    secondary.player().set_moves( 100 );
    const multiplayer_turn_participant_key alpha_key = participant_key( *alpha_runtime );
    const multiplayer_turn_participant_key beta_key = participant_key( *secondary.runtime() );
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    REQUIRE( owner->begin_turn( { alpha_key, beta_key } ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    const std::optional<multiplayer_turn_slot> initial = owner->current_slot();
    REQUIRE( initial );
    const multiplayer_turn_participant_key other =
        initial->participant.player_id == alpha_key.player_id ? beta_key : alpha_key;
    int callback_count = 0;

    CHECK( owner->execute_current_player(
    other, [&]( multiplayer_player_runtime &, avatar & ) {
        ++callback_count;
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } ) == multiplayer_turn_phase_adapter_status::invalid_scheduler_state );
    CHECK( callback_count == 0 );
    CHECK( owner->current_slot() == initial );
    CHECK_FALSE( owner->is_faulted() );

    multiplayer_turn_participant_key stale_current = initial->participant;
    ++stale_current.session_generation;
    CHECK( owner->execute_current_player(
    stale_current, [&]( multiplayer_player_runtime &, avatar & ) {
        ++callback_count;
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } ) == multiplayer_turn_phase_adapter_status::stale_session_generation );
    CHECK( callback_count == 0 );
    CHECK( owner->current_slot() == initial );
    CHECK_FALSE( owner->is_faulted() );

    CHECK( owner->execute_current_player(
               initial->participant,
    [&]( multiplayer_player_runtime &, avatar & ) {
        ++callback_count;
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::rejected );
    } ) == multiplayer_turn_phase_adapter_status::completed );
    CHECK( callback_count == 1 );
    CHECK( owner->current_slot() == initial );
    CHECK_FALSE( owner->gameplay_side_effects_may_have_occurred() );
}

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_runs_two_waiters_and_rotates_first_player",
           "[multiplayer][multi_runtime_barrier_owner][phase_adapter][scheduler]" )
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
    const std::vector<multiplayer_turn_participant_key> roster = { beta_key, alpha_key };
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    const int actions_before = g->get_moves_since_last_save();
    int world_count = 0;

    REQUIRE( owner->begin_turn( roster ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    REQUIRE( owner->current_slot() );
    const multiplayer_player_id first_turn_first =
        owner->current_slot()->participant.player_id;
    std::set<std::string> first_turn_waiters;
    while( const std::optional<multiplayer_turn_slot> slot = owner->current_slot() ) {
        const int actions_before_wait = g->get_moves_since_last_save();
        if( slot->participant.player_id == beta_runtime->player_id() ) {
            REQUIRE( owner->mark_barrier_disconnected( slot->participant ) ==
                     multiplayer_multi_runtime_barrier_status::applied );
            REQUIRE( owner->apply_disconnect_timeout(
                         slot->participant,
                         multiplayer_disconnect_timeout_policy::automatic_wait ) ==
                     multiplayer_multi_runtime_barrier_status::applied );
            REQUIRE( owner->execute_authoritative_wait( slot->participant ) ==
                     multiplayer_turn_phase_adapter_status::completed );
            first_turn_waiters.emplace( beta_runtime->player_id().str() );
        } else {
            REQUIRE( execute_forced_wait( *owner, slot->participant,
                                          &first_turn_waiters ) ==
                     multiplayer_turn_phase_adapter_status::completed );
        }
        CHECK( g->get_moves_since_last_save() == actions_before_wait + 1 );
        CHECK( &get_avatar() == &alpha );
        CHECK( &g->active_player_runtime() == alpha_runtime.get() );
    }
    CHECK( first_turn_waiters.size() == 2 );
    CHECK( alpha.get_moves() <= 0 );
    CHECK( secondary.player().get_moves() <= 0 );
    CHECK( beta_runtime->status() == multiplayer_player_status::active );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::world_ready );
    REQUIRE( owner->execute_world( [&]( const multiplayer_world_ticket & ticket ) {
        ++world_count;
        CHECK( ticket.shared_turn == 1 );
        CHECK( owner->stage() ==
               multiplayer_multi_runtime_barrier_stage::world_processing );
        CHECK( &get_avatar() == &alpha );
        return true;
    } ) == multiplayer_turn_phase_adapter_status::completed );
    CHECK( world_count == 1 );
    CHECK( owner->stage() ==
           multiplayer_multi_runtime_barrier_stage::player_end_pending );
    CHECK( owner->participant_count() == 2 );
    CHECK( owner->has_open_boundary() );
    CHECK( owner->begin_turn( roster ) ==
           multiplayer_multi_runtime_barrier_status::not_ready );
    CHECK( owner->execute_world( [&]( const multiplayer_world_ticket & ) {
        ++world_count;
        return true;
    } ) == multiplayer_turn_phase_adapter_status::invalid_scheduler_state );
    CHECK( world_count == 1 );
    complete_reported_player_end( *owner );
    CHECK( owner->participant_count() == 0 );
    CHECK( g->get_moves_since_last_save() == actions_before + 2 );

    alpha.set_moves( 100 );
    secondary.player().set_moves( 100 );
    REQUIRE( owner->begin_turn( roster ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    REQUIRE( owner->current_slot() );
    const multiplayer_player_id expected_second_turn_first =
        first_turn_first == alpha_runtime->player_id() ? beta_runtime->player_id() :
        alpha_runtime->player_id();
    CHECK( owner->current_slot()->participant.player_id == expected_second_turn_first );
    std::set<std::string> second_turn_waiters;
    while( const std::optional<multiplayer_turn_slot> slot = owner->current_slot() ) {
        const int actions_before_wait = g->get_moves_since_last_save();
        REQUIRE( execute_forced_wait( *owner, slot->participant,
                                      &second_turn_waiters ) ==
                 multiplayer_turn_phase_adapter_status::completed );
        CHECK( g->get_moves_since_last_save() == actions_before_wait + 1 );
        CHECK( &get_avatar() == &alpha );
        CHECK( &g->active_player_runtime() == alpha_runtime.get() );
    }
    CHECK( second_turn_waiters.size() == 2 );
    REQUIRE( owner->execute_world( [&]( const multiplayer_world_ticket & ticket ) {
        ++world_count;
        CHECK( ticket.shared_turn == 2 );
        return true;
    } ) == multiplayer_turn_phase_adapter_status::completed );
    CHECK( world_count == 2 );
    complete_reported_player_end( *owner );
    CHECK( g->get_moves_since_last_save() == actions_before + 4 );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::idle );
    CHECK_FALSE( owner->has_open_boundary() );
    CHECK_FALSE( owner->is_faulted() );
}

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_fail_stops_an_exact_current_runtime_loss",
           "[multiplayer][multi_runtime_barrier_owner][runtime][fault]" )
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
    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime = secondary.runtime();
    secondary.player().set_moves( 100 );
    const std::vector<multiplayer_turn_participant_key> roster = {
        participant_key( *alpha_runtime ), participant_key( *beta_runtime )
    };
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    REQUIRE( owner->begin_turn( roster ) ==
             multiplayer_multi_runtime_barrier_status::applied );

    while( owner->current_slot() &&
           owner->current_slot()->participant.player_id != beta_runtime->player_id() ) {
        REQUIRE( execute_forced_wait( *owner, owner->current_slot()->participant ) ==
                 multiplayer_turn_phase_adapter_status::completed );
    }
    REQUIRE( owner->current_slot() );
    const multiplayer_turn_participant_key beta_current =
        owner->current_slot()->participant;
    REQUIRE( beta_current.player_id == beta_runtime->player_id() );
    REQUIRE( g->disconnect_multiplayer_player( beta_runtime->player_id() ) );

    int callback_count = 0;
    CHECK( owner->execute_current_player(
    beta_current, [&]( multiplayer_player_runtime &, avatar & ) {
        ++callback_count;
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } ) == multiplayer_turn_phase_adapter_status::inactive_runtime );
    CHECK( callback_count == 0 );
    CHECK( owner->is_faulted() );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::faulted );
    CHECK( owner->fault_stage() ==
           multiplayer_multi_runtime_barrier_fault_stage::player_action );
    CHECK( owner->begin_turn( roster ) ==
           multiplayer_multi_runtime_barrier_status::faulted );
}

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_rejects_same_key_runtime_replacement",
           "[multiplayer][multi_runtime_barrier_owner][runtime][ownership][fault]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &alpha = get_avatar();
    map &here = get_map();
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    const shared_ptr_fast<multiplayer_player_runtime> original_runtime =
        secondary.runtime();
    const multiplayer_turn_participant_key original_key =
        participant_key( *original_runtime );
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    REQUIRE( owner->begin_turn( { original_key } ) ==
             multiplayer_multi_runtime_barrier_status::applied );

    SECTION( "player callback never enters a replacement owner" ) {
        REQUIRE( g->disconnect_multiplayer_player( original_key.player_id ) );
        REQUIRE( g->unregister_multiplayer_player( secondary.player() ) );
        registered_same_key_replacement replacement(
            here, tripoint_bub_ms( 64, 60, 0 ), original_key );
        REQUIRE( replacement.valid() );
        REQUIRE( replacement.runtime()->player_id() == original_key.player_id );
        REQUIRE( replacement.runtime()->session_generation() ==
                 original_key.session_generation );
        REQUIRE( replacement.runtime().get() != original_runtime.get() );
        int callback_count = 0;

        CHECK( owner->execute_current_player(
        original_key, [&]( multiplayer_player_runtime &, avatar & ) {
            ++callback_count;
            return std::optional<multiplayer_turn_action_disposition>(
                       multiplayer_turn_action_disposition::accepted_finished );
        } ) == multiplayer_turn_phase_adapter_status::activation_failed );
        CHECK( callback_count == 0 );
        CHECK( owner->is_faulted() );
        CHECK( owner->fault_stage() ==
               multiplayer_multi_runtime_barrier_fault_stage::player_action );
        CHECK_FALSE( owner->gameplay_side_effects_may_have_occurred() );
    }

    SECTION( "zero-action terminal transition never accepts a replacement owner" ) {
        REQUIRE( g->disconnect_multiplayer_player( original_key.player_id ) );
        REQUIRE( g->unregister_multiplayer_player( secondary.player() ) );
        registered_same_key_replacement replacement(
            here, tripoint_bub_ms( 64, 60, 0 ), original_key );
        REQUIRE( replacement.valid() );

        CHECK( owner->complete_player_phase( original_key ) ==
               multiplayer_multi_runtime_barrier_status::runtime_identity_mismatch );
        CHECK( owner->is_faulted() );
        CHECK( owner->participant_count() == 1 );
        CHECK( owner->has_open_boundary() );
        CHECK( owner->fault_stage() ==
               multiplayer_multi_runtime_barrier_fault_stage::player_action );
    }

    SECTION( "world callback never runs after a terminal participant is replaced" ) {
        REQUIRE( owner->complete_player_phase( original_key ) ==
                 multiplayer_multi_runtime_barrier_status::applied );
        REQUIRE( owner->stage() == multiplayer_multi_runtime_barrier_stage::world_ready );
        REQUIRE( g->disconnect_multiplayer_player( original_key.player_id ) );
        REQUIRE( g->unregister_multiplayer_player( secondary.player() ) );
        registered_same_key_replacement replacement(
            here, tripoint_bub_ms( 64, 60, 0 ), original_key );
        REQUIRE( replacement.valid() );
        int world_count = 0;

        CHECK( owner->execute_world( [&]( const multiplayer_world_ticket & ) {
            ++world_count;
            return true;
        } ) == multiplayer_turn_phase_adapter_status::activation_failed );
        CHECK( world_count == 0 );
        CHECK( owner->is_faulted() );
        CHECK( owner->fault_stage() ==
               multiplayer_multi_runtime_barrier_fault_stage::world );
        CHECK_FALSE( owner->gameplay_side_effects_may_have_occurred() );
        CHECK( owner->has_open_boundary() );
    }
}

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_fail_stops_an_exact_command_from_non_root_context",
           "[multiplayer][multi_runtime_barrier_owner][player_bridge][fault]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &alpha = get_avatar();
    map &here = get_map();
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime = secondary.runtime();
    const std::vector<multiplayer_turn_participant_key> roster = {
        participant_key( *alpha_runtime ), participant_key( *beta_runtime )
    };
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    REQUIRE( owner->begin_turn( roster ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    REQUIRE( owner->current_slot() );
    const multiplayer_turn_participant_key current = owner->current_slot()->participant;
    int callback_count = 0;

    {
        multiplayer_active_player_guard non_root_context( *g, beta_runtime->player_owner() );
        REQUIRE( non_root_context.is_engaged() );
        CHECK( owner->execute_current_player(
        current, [&]( multiplayer_player_runtime &, avatar & ) {
            ++callback_count;
            return std::optional<multiplayer_turn_action_disposition>(
                       multiplayer_turn_action_disposition::accepted_finished );
        } ) == multiplayer_turn_phase_adapter_status::activation_failed );
        CHECK( owner->is_faulted() );
    }

    CHECK( callback_count == 0 );
    CHECK( owner->is_faulted() );
    CHECK( owner->fault_stage() ==
           multiplayer_multi_runtime_barrier_fault_stage::player_action );
    CHECK_FALSE( owner->gameplay_side_effects_may_have_occurred() );
    CHECK( &get_avatar() == &alpha );
    CHECK( &g->active_player_runtime() == alpha_runtime.get() );
}

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_terminalizes_a_zero_action_phase",
           "[multiplayer][multi_runtime_barrier_owner][player_phase]" )
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
    alpha.setpos( here, tripoint_bub_ms( 60, 60, 0 ) );
    alpha.set_moves( 100 );
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    secondary.player().set_moves( 100 );
    const std::vector<multiplayer_turn_participant_key> roster = {
        participant_key( *alpha_runtime ), participant_key( *secondary.runtime() )
    };
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    const int actions_before = g->get_moves_since_last_save();

    REQUIRE( owner->begin_turn( roster ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    REQUIRE( owner->current_slot() );
    const multiplayer_turn_participant_key no_action =
        owner->current_slot()->participant;
    const shared_ptr_fast<multiplayer_player_runtime> no_action_runtime =
        g->multiplayer_players().find_by_player_id( no_action.player_id );
    REQUIRE( no_action_runtime );
    no_action_runtime->player().set_moves( 0 );
    CHECK( no_action_runtime->player().get_moves() == 0 );
    REQUIRE( owner->complete_player_phase( no_action ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    CHECK( g->get_moves_since_last_save() == actions_before );
    REQUIRE( owner->current_slot() );
    REQUIRE( execute_forced_wait( *owner, owner->current_slot()->participant ) ==
             multiplayer_turn_phase_adapter_status::completed );
    CHECK( g->get_moves_since_last_save() == actions_before + 1 );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::world_ready );
    int world_count = 0;
    REQUIRE( owner->execute_world( [&]( const multiplayer_world_ticket & ) {
        ++world_count;
        return true;
    } ) == multiplayer_turn_phase_adapter_status::completed );
    CHECK( world_count == 1 );
    complete_reported_player_end( *owner );
}

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_fail_stops_after_a_player_side_effect",
           "[multiplayer][multi_runtime_barrier_owner][fault]" )
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
    alpha.setpos( here, tripoint_bub_ms( 60, 60, 0 ) );
    alpha.set_moves( 100 );
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    secondary.player().set_moves( 100 );
    const std::vector<multiplayer_turn_participant_key> roster = {
        participant_key( *alpha_runtime ), participant_key( *secondary.runtime() )
    };
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    REQUIRE( owner->begin_turn( roster ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    REQUIRE( owner->current_slot() );
    const multiplayer_turn_participant_key current =
        owner->current_slot()->participant;
    avatar *acted_player = nullptr;
    int callback_count = 0;

    CHECK( owner->execute_current_player(
               current,
    [&]( multiplayer_player_runtime &, avatar & player ) {
        ++callback_count;
        acted_player = &player;
        CHECK( multiplayer_execute_wait(
                   *g, player,
                   multiplayer_wait_execution_mode::authoritative_forced ) );
        return std::optional<multiplayer_turn_action_disposition>();
    } ) == multiplayer_turn_phase_adapter_status::player_callback_failed );
    REQUIRE( acted_player != nullptr );
    CHECK( acted_player->get_moves() <= 0 );
    CHECK( callback_count == 1 );
    CHECK( &get_avatar() == &alpha );
    CHECK( owner->is_faulted() );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::faulted );
    CHECK( owner->fault_stage() ==
           multiplayer_multi_runtime_barrier_fault_stage::player_action );
    CHECK( owner->gameplay_side_effects_may_have_occurred() );

    CHECK( owner->execute_current_player(
    current, [&]( multiplayer_player_runtime &, avatar & ) {
        ++callback_count;
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } ) == multiplayer_turn_phase_adapter_status::faulted );
    CHECK( callback_count == 1 );
    CHECK( owner->execute_world( []( const multiplayer_world_ticket & ) {
        return true;
    } ) == multiplayer_turn_phase_adapter_status::faulted );
    CHECK( owner->begin_turn( roster ) ==
           multiplayer_multi_runtime_barrier_status::faulted );
}

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_fail_stops_after_a_world_failure",
           "[multiplayer][multi_runtime_barrier_owner][fault][world]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &alpha = get_avatar();
    map &here = get_map();
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    const std::vector<multiplayer_turn_participant_key> roster = {
        participant_key( *alpha_runtime ), participant_key( *secondary.runtime() )
    };
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    REQUIRE( owner->begin_turn( roster ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    while( const std::optional<multiplayer_turn_slot> slot = owner->current_slot() ) {
        REQUIRE( owner->complete_player_phase( slot->participant ) ==
                 multiplayer_multi_runtime_barrier_status::applied );
    }

    int world_count = 0;
    CHECK( owner->execute_world( [&]( const multiplayer_world_ticket & ) {
        ++world_count;
        return false;
    } ) == multiplayer_turn_phase_adapter_status::world_callback_failed );
    CHECK( world_count == 1 );
    CHECK( owner->is_faulted() );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::faulted );
    CHECK( owner->fault_stage() ==
           multiplayer_multi_runtime_barrier_fault_stage::world );
    CHECK( owner->gameplay_side_effects_may_have_occurred() );
    CHECK( owner->execute_world( [&]( const multiplayer_world_ticket & ) {
        ++world_count;
        return true;
    } ) == multiplayer_turn_phase_adapter_status::faulted );
    CHECK( world_count == 1 );
    CHECK( owner->begin_turn( roster ) ==
           multiplayer_multi_runtime_barrier_status::faulted );
}

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_rejects_forged_stale_and_cross_owner_receipts",
           "[multiplayer][multi_runtime_barrier_owner][receipt]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &alpha = get_avatar();
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    const std::vector<multiplayer_turn_participant_key> roster = {
        participant_key( *alpha_runtime )
    };

    SECTION( "default receipt cannot close an externally completed boundary" ) {
        std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
        complete_zero_action_barrier_to_world( *owner, roster );
        CHECK( owner->record_external_player_end_completed(
                   multiplayer_multi_runtime_world_completion() ) ==
               multiplayer_multi_runtime_barrier_status::faulted );
        CHECK( owner->fault_stage() ==
               multiplayer_multi_runtime_barrier_fault_stage::external_player_end );
        CHECK( owner->gameplay_side_effects_may_have_occurred() );
    }

    SECTION( "previous exact receipt cannot close a new pending boundary" ) {
        std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
        const multiplayer_multi_runtime_world_completion first =
            complete_zero_action_barrier_to_world( *owner, roster );
        REQUIRE( owner->record_external_player_end_completed( first ) ==
                 multiplayer_multi_runtime_barrier_status::applied );
        complete_zero_action_barrier_to_world( *owner, roster );
        CHECK( owner->record_external_player_end_completed( first ) ==
               multiplayer_multi_runtime_barrier_status::faulted );
        CHECK( owner->fault_stage() ==
               multiplayer_multi_runtime_barrier_fault_stage::external_player_end );
    }

    SECTION( "previous exact receipt is not a duplicate during a new active turn" ) {
        std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
        const multiplayer_multi_runtime_world_completion first =
            complete_zero_action_barrier_to_world( *owner, roster );
        REQUIRE( owner->record_external_player_end_completed( first ) ==
                 multiplayer_multi_runtime_barrier_status::applied );
        REQUIRE( owner->begin_turn( roster ) ==
                 multiplayer_multi_runtime_barrier_status::applied );
        const std::optional<multiplayer_turn_slot> current = owner->current_slot();
        REQUIRE( current );
        const multiplayer_multi_runtime_barrier_stage stage_before = owner->stage();

        CHECK( owner->record_external_player_end_completed( first ) ==
               multiplayer_multi_runtime_barrier_status::invalid_scheduler_state );
        CHECK( owner->stage() == stage_before );
        CHECK( owner->current_slot() == current );
        CHECK_FALSE( owner->is_faulted() );
    }

    SECTION( "receipt is owner-bound" ) {
        std::unique_ptr<multiplayer_multi_runtime_barrier_owner> first_owner = make_owner();
        std::unique_ptr<multiplayer_multi_runtime_barrier_owner> second_owner = make_owner();
        const multiplayer_multi_runtime_world_completion first_completion =
            complete_zero_action_barrier_to_world( *first_owner, roster );
        complete_zero_action_barrier_to_world( *second_owner, roster );
        CHECK( second_owner->record_external_player_end_completed( first_completion ) ==
               multiplayer_multi_runtime_barrier_status::faulted );
        CHECK( second_owner->fault_stage() ==
               multiplayer_multi_runtime_barrier_fault_stage::external_player_end );
    }
}

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_latches_an_outer_record_failure",
           "[multiplayer][multi_runtime_barrier_owner][fault][player_end]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &alpha = get_avatar();
    map &here = get_map();
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    registered_secondary_player secondary( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( secondary.valid() );
    const std::vector<multiplayer_turn_participant_key> roster = {
        participant_key( *alpha_runtime ), participant_key( *secondary.runtime() )
    };
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    const multiplayer_multi_runtime_world_completion completion =
        complete_zero_action_barrier_to_world( *owner, roster );
    REQUIRE( owner->stage() ==
             multiplayer_multi_runtime_barrier_stage::player_end_pending );
    REQUIRE( owner->latch_external_fault( true ) ==
             multiplayer_multi_runtime_barrier_status::faulted );
    CHECK( owner->is_faulted() );
    CHECK( owner->fault_stage() ==
           multiplayer_multi_runtime_barrier_fault_stage::external_player_end );
    CHECK( owner->gameplay_side_effects_may_have_occurred() );
    CHECK( owner->begin_turn( roster ) ==
           multiplayer_multi_runtime_barrier_status::faulted );
    CHECK( owner->record_external_player_end_completed( completion ) ==
           multiplayer_multi_runtime_barrier_status::faulted );
}

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_rejects_external_fault_from_worker_thread",
           "[multiplayer][multi_runtime_barrier_owner][thread][fault]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &alpha = get_avatar();
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    const std::vector<multiplayer_turn_participant_key> roster = {
        participant_key( *alpha_runtime )
    };
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    REQUIRE( owner->begin_turn( roster ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    const std::optional<multiplayer_turn_slot> current = owner->current_slot();
    REQUIRE( current );
    multiplayer_multi_runtime_barrier_status worker_status =
        multiplayer_multi_runtime_barrier_status::faulted;

    std::thread worker( [&]() {
        worker_status = owner->latch_external_fault( true );
    } );
    worker.join();

    CHECK( worker_status == multiplayer_multi_runtime_barrier_status::wrong_thread );
    CHECK_FALSE( owner->is_faulted() );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::player_actions );
    CHECK( owner->current_slot() == current );
    CHECK_FALSE( owner->gameplay_side_effects_may_have_occurred() );
}

TEST_CASE( "multiplayer_multi_runtime_barrier_owner_pins_roster_runtime_owners_until_boundary_closes",
           "[multiplayer][multi_runtime_barrier_owner][runtime][lifetime]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &alpha = get_avatar();
    map &here = get_map();
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    shared_ptr_fast<avatar> beta_owner = make_shared_fast<avatar>();
    beta_owner->create( character_type::NOW );
    clear_character( *beta_owner );
    beta_owner->setID( g->assign_npc_id(), true );
    beta_owner->setpos( here, tripoint_bub_ms( 62, 60, 0 ) );
    REQUIRE( g->register_multiplayer_player( beta_owner ) );
    shared_ptr_fast<multiplayer_player_runtime> beta_runtime =
        g->multiplayer_players().find_runtime( *beta_owner );
    REQUIRE( beta_runtime );
    weak_ptr_fast<avatar> beta_owner_weak = beta_owner;
    weak_ptr_fast<multiplayer_player_runtime> beta_runtime_weak = beta_runtime;
    const multiplayer_player_id beta_id = beta_runtime->player_id();
    const std::vector<multiplayer_turn_participant_key> roster = {
        participant_key( *alpha_runtime ), participant_key( *beta_runtime )
    };
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner = make_owner();
    REQUIRE( owner->begin_turn( roster ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    while( const std::optional<multiplayer_turn_slot> slot = owner->current_slot() ) {
        REQUIRE( owner->complete_player_phase( slot->participant ) ==
                 multiplayer_multi_runtime_barrier_status::applied );
    }
    REQUIRE( owner->execute_world( []( const multiplayer_world_ticket & ) {
        return true;
    } ) == multiplayer_turn_phase_adapter_status::completed );
    const std::optional<multiplayer_multi_runtime_world_completion> completion =
        owner->pending_world_completion();
    REQUIRE( completion );
    REQUIRE( owner->stage() ==
             multiplayer_multi_runtime_barrier_stage::player_end_pending );

    REQUIRE( g->disconnect_multiplayer_player( beta_id ) );
    REQUIRE( g->unregister_multiplayer_player( *beta_owner ) );
    beta_runtime.reset();
    beta_owner.reset();
    CHECK_FALSE( beta_runtime_weak.expired() );
    CHECK_FALSE( beta_owner_weak.expired() );

    REQUIRE( owner->record_external_player_end_completed( *completion ) ==
             multiplayer_multi_runtime_barrier_status::applied );
    CHECK( owner->stage() == multiplayer_multi_runtime_barrier_stage::idle );
    CHECK( beta_runtime_weak.expired() );
    CHECK( beta_owner_weak.expired() );
}

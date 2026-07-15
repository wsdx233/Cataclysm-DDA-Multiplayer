#include "cata_catch.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "avatar.h"
#include "cata_scope_helpers.h"
#include "calendar.h"
#include "game.h"
#include "map_helpers.h"
#include "memory_fast.h"
#include "multiplayer_command_executor.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_player_runtime.h"
#include "multiplayer_session_generation.h"
#include "multiplayer_single_root_owner.h"
#include "options_helpers.h"
#include "player_helpers.h"

namespace
{

multiplayer_session_id session_id( const std::uint8_t value )
{
    multiplayer_session_id result = {};
    result.fill( value );
    return result;
}

multiplayer_session_admission_request authentication_request(
    const multiplayer_player_runtime &runtime,
    const std::uint64_t admission_id = 1,
    const multiplayer_connection_id connection = 101,
    const std::uint8_t session_value = 1 )
{
    multiplayer_session_admission_request request;
    request.kind = multiplayer_session_admission_kind::authentication;
    request.admission_id = admission_id;
    request.connection = connection;
    request.session = session_id( session_value );
    request.player_id = runtime.player_id().str();
    request.character_id = std::to_string( runtime.player().getID().get_value() );
    return request;
}

multiplayer_session_admission_request resume_request(
    const multiplayer_player_runtime &runtime,
    const std::uint64_t expected_generation,
    const std::uint64_t admission_id = 2,
    const multiplayer_connection_id connection = 102,
    const std::uint8_t session_value = 2 )
{
    multiplayer_session_admission_request request;
    request.kind = multiplayer_session_admission_kind::resume;
    request.admission_id = admission_id;
    request.connection = connection;
    request.session = session_id( session_value );
    request.player_id = runtime.player_id().str();
    request.character_id = std::to_string( runtime.player().getID().get_value() );
    request.expected_session_generation = expected_generation;
    request.last_server_revision = 17;
    request.last_client_sequence = 23;
    return request;
}

std::unique_ptr<multiplayer_single_root_owner> make_owner(
    const std::chrono::steady_clock::duration disconnect_grace =
        std::chrono::seconds( 5 ) )
{
    std::string error;
    std::unique_ptr<multiplayer_single_root_owner> owner =
        multiplayer_single_root_owner::create( *g, 1, disconnect_grace, error );
    INFO( error );
    REQUIRE( owner );
    CHECK( error.empty() );
    return owner;
}

multiplayer_session_binding publish_initial_binding(
    multiplayer_single_root_owner &owner,
    const multiplayer_player_runtime &runtime )
{
    const multiplayer_single_root_admission_plan plan = owner.plan_admission(
                authentication_request( runtime ) );
    REQUIRE( plan );
    bool publish_called = false;
    const multiplayer_single_root_owner_result admitted = owner.execute_admission(
                plan, [&]( const std::uint64_t admission_id,
    const multiplayer_session_binding & binding ) {
        publish_called = true;
        CHECK( owner.admission_committed() );
        return multiplayer_single_root_admission_publish_receipt {
            multiplayer_single_root_admission_publish_outcome::published,
            admission_id, binding
        };
    } );
    REQUIRE( admitted.status == multiplayer_single_root_owner_status::applied );
    CHECK( publish_called );
    CHECK_FALSE( owner.admission_committed() );

    const std::optional<multiplayer_session_binding> binding =
        owner.session_for_player( runtime.player_id().str() );
    REQUIRE( binding );
    REQUIRE( owner.matches_connected( *binding ) );
    return *binding;
}

multiplayer_owned_turn_result run_owned_wait_turn(
    multiplayer_single_root_owner &owner, avatar &player,
    const shared_ptr_fast<multiplayer_player_runtime> &runtime,
    bool &saw_world_processing )
{
    REQUIRE( owner.begin_turn().status == multiplayer_single_root_owner_status::applied );
    REQUIRE( owner.can_execute_command() );

    multiplayer_owned_remote_turn_hooks hooks;
    hooks.execute_player_action = [&]() {
        return owner.execute_current_player(
        [&]( multiplayer_player_runtime & callback_runtime, avatar & callback_player ) {
            CHECK( &callback_runtime == runtime.get() );
            CHECK( &callback_player == &player );
            if( !multiplayer_execute_wait(
                    *g, callback_player,
                    multiplayer_wait_execution_mode::authoritative_forced ) ) {
                return std::optional<multiplayer_turn_action_disposition>();
            }
            return std::optional<multiplayer_turn_action_disposition>(
                       multiplayer_turn_action_disposition::accepted_finished );
        } ) == multiplayer_turn_phase_adapter_status::completed;
    };
    hooks.complete_player_phase = [&]() {
        const multiplayer_single_root_owner_result completed =
            owner.complete_player_phase();
        return completed.status == multiplayer_single_root_owner_status::applied ||
               completed.status == multiplayer_single_root_owner_status::duplicate;
    };
    hooks.execute_world = [&]( multiplayer_owned_world_thunk & world_thunk ) {
        return owner.execute_world(
        [&]( const multiplayer_world_ticket & ) {
            saw_world_processing =
                owner.snapshot().barrier ==
                multiplayer_root_barrier_state::world_processing;
            CHECK( owner.save_disposition() ==
                   multiplayer_single_root_save_disposition::open_turn );
            return world_thunk();
        } ) == multiplayer_turn_phase_adapter_status::completed;
    };
    return owner.execute_owned_turn( hooks );
}

multiplayer_owned_turn_result run_owned_turn_after_external_terminal(
    multiplayer_single_root_owner &owner, int &action_calls )
{
    multiplayer_owned_remote_turn_hooks hooks;
    hooks.execute_player_action = [&]() {
        ++action_calls;
        return false;
    };
    hooks.complete_player_phase = [&]() {
        const multiplayer_single_root_owner_result completed =
            owner.complete_player_phase();
        return completed.status == multiplayer_single_root_owner_status::applied ||
               completed.status == multiplayer_single_root_owner_status::duplicate;
    };
    hooks.execute_world = [&]( multiplayer_owned_world_thunk & world_thunk ) {
        return owner.execute_world(
        [&]( const multiplayer_world_ticket & ) {
            return world_thunk();
        } ) == multiplayer_turn_phase_adapter_status::completed;
    };
    return owner.execute_owned_turn( hooks );
}

} // namespace

TEST_CASE( "multiplayer_single_root_owner_requires_players_max_one",
           "[multiplayer][single_root_owner]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    std::size_t maximum_players = 0;
    SECTION( "zero players" ) {
        maximum_players = 0;
    }
    SECTION( "more than one player" ) {
        maximum_players = 2;
    }

    std::string error;
    CHECK_FALSE( multiplayer_single_root_owner::create(
                     *g, maximum_players, std::chrono::seconds( 5 ), error ) );
    CHECK( error == "single-root owner requires players.max = 1" );
}

TEST_CASE( "multiplayer_single_root_owner_publishes_initial_authentication_atomically",
           "[multiplayer][single_root_owner][session_directory]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &player = get_avatar();
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        g->multiplayer_players().find_runtime( player );
    REQUIRE( runtime );
    std::unique_ptr<multiplayer_single_root_owner> owner = make_owner();
    CHECK_FALSE( owner->can_begin_turn() );
    CHECK( owner->begin_turn().status ==
           multiplayer_single_root_owner_status::not_ready );

    const multiplayer_session_binding binding =
        publish_initial_binding( *owner, *runtime );

    CHECK( binding.player_id == runtime->player_id().str() );
    CHECK( binding.character_id ==
           std::to_string( player.getID().get_value() ) );
    CHECK( binding.session_generation == runtime->session_generation() );
    CHECK( owner->snapshot().connection ==
           multiplayer_root_connection_state::connected );
    CHECK( owner->can_begin_turn() );
    CHECK( owner->save_disposition() ==
           multiplayer_single_root_save_disposition::allowed );
}

TEST_CASE( "multiplayer_single_root_owner_keeps_the_player_phase_open_after_an_eligible_action",
           "[multiplayer][single_root_owner][owned_turn][accepted_remains]" )
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

    avatar &player = get_avatar();
    player.set_moves( 100 );
    g->new_game = true;
    g->uquit = QUIT_NO;
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        g->multiplayer_players().find_runtime( player );
    REQUIRE( runtime );
    std::unique_ptr<multiplayer_single_root_owner> owner = make_owner();
    publish_initial_binding( *owner, *runtime );
    REQUIRE( owner->begin_turn().status ==
             multiplayer_single_root_owner_status::applied );

    int callback_count = 0;
    REQUIRE( owner->execute_current_player(
    [&]( multiplayer_player_runtime & callback_runtime, avatar & callback_player ) {
        ++callback_count;
        CHECK( &callback_runtime == runtime.get() );
        CHECK( &callback_player == &player );
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_remains_eligible );
    } ) == multiplayer_turn_phase_adapter_status::completed );
    CHECK( callback_count == 1 );
    CHECK( owner->can_execute_command() );
    REQUIRE( owner->current_slot() );
    CHECK( owner->current_slot()->state ==
           multiplayer_turn_participant_state::awaiting_command );

    REQUIRE( owner->execute_current_player(
    [&]( multiplayer_player_runtime &, avatar & callback_player ) {
        ++callback_count;
        if( !multiplayer_execute_wait(
                *g, callback_player,
                multiplayer_wait_execution_mode::authoritative_forced ) ) {
            return std::optional<multiplayer_turn_action_disposition>();
        }
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } ) == multiplayer_turn_phase_adapter_status::completed );
    CHECK( callback_count == 2 );
    CHECK_FALSE( owner->can_execute_command() );

    int unexpected_action_hooks = 0;
    const multiplayer_owned_turn_result turn_result =
        run_owned_turn_after_external_terminal( *owner, unexpected_action_hooks );
    REQUIRE( turn_result.status == multiplayer_owned_turn_status::completed );
    CHECK( unexpected_action_hooks == 0 );
    REQUIRE( owner->complete_turn_after_player_end( turn_result ).status ==
             multiplayer_single_root_owner_status::applied );
    CHECK( owner->can_begin_turn() );
}

TEST_CASE( "multiplayer_single_root_owner_completes_lifecycle_only_after_player_end",
           "[multiplayer][single_root_owner][owned_turn]" )
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

    avatar &player = get_avatar();
    player.set_moves( 100 );
    g->uquit = QUIT_NO;
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        g->multiplayer_players().find_runtime( player );
    REQUIRE( runtime );
    std::unique_ptr<multiplayer_single_root_owner> owner = make_owner();
    publish_initial_binding( *owner, *runtime );

    bool saw_world_processing = false;
    const multiplayer_owned_turn_result turn_result =
        run_owned_wait_turn( *owner, player, runtime, saw_world_processing );

    REQUIRE( turn_result.status == multiplayer_owned_turn_status::completed );
    REQUIRE( turn_result.progress == multiplayer_owned_turn_progress::turn_completed );
    REQUIRE( turn_result.player_end_completed() );
    CHECK( saw_world_processing );
    CHECK( owner->snapshot().barrier ==
           multiplayer_root_barrier_state::world_processing );
    CHECK( owner->save_disposition() ==
           multiplayer_single_root_save_disposition::open_turn );

    SECTION( "the exact completed result closes the lifecycle boundary" ) {
        const multiplayer_single_root_owner_result completed =
            owner->complete_turn_after_player_end( turn_result );
        CHECK( completed.status == multiplayer_single_root_owner_status::applied );
        CHECK( owner->snapshot().barrier == multiplayer_root_barrier_state::none );
        CHECK( owner->can_begin_turn() );
        CHECK( owner->save_disposition() ==
               multiplayer_single_root_save_disposition::allowed );
    }

    SECTION( "public fields cannot forge player-end completion" ) {
        multiplayer_owned_turn_result forged;
        forged.status = multiplayer_owned_turn_status::completed;
        forged.progress = multiplayer_owned_turn_progress::turn_completed;
        forged.abort_stage = multiplayer_owned_turn_abort_stage::none;
        const multiplayer_single_root_owner_result rejected =
            owner->complete_turn_after_player_end( forged );
        CHECK( rejected.status == multiplayer_single_root_owner_status::faulted );
        CHECK( owner->is_faulted() );
        CHECK( owner->save_disposition() ==
               multiplayer_single_root_save_disposition::canonical_state_uncertain );
    }

    SECTION( "an unfinished result fail-stops without a save" ) {
        multiplayer_owned_turn_result unfinished;
        unfinished.status = multiplayer_owned_turn_status::aborted;
        unfinished.progress = multiplayer_owned_turn_progress::world_completed;
        unfinished.abort_stage = multiplayer_owned_turn_abort_stage::player_end;
        const multiplayer_single_root_owner_result rejected =
            owner->complete_turn_after_player_end( unfinished );
        CHECK( rejected.status == multiplayer_single_root_owner_status::faulted );
        CHECK( owner->is_faulted() );
        CHECK( owner->save_disposition() ==
               multiplayer_single_root_save_disposition::canonical_state_uncertain );
    }

    SECTION( "a second owned invocation in the same open turn fail-stops" ) {
        int unexpected_hook_calls = 0;
        multiplayer_owned_remote_turn_hooks repeated_hooks;
        repeated_hooks.execute_player_action = [&]() {
            ++unexpected_hook_calls;
            return true;
        };
        repeated_hooks.complete_player_phase = [&]() {
            ++unexpected_hook_calls;
            return true;
        };
        repeated_hooks.execute_world = [&]( multiplayer_owned_world_thunk & world_thunk ) {
            ++unexpected_hook_calls;
            return world_thunk();
        };
        const multiplayer_owned_turn_result repeated =
            owner->execute_owned_turn( repeated_hooks );
        CHECK( repeated.status == multiplayer_owned_turn_status::aborted );
        CHECK( repeated.progress == multiplayer_owned_turn_progress::not_started );
        CHECK( unexpected_hook_calls == 0 );
        CHECK( owner->is_faulted() );
        CHECK( owner->save_disposition() ==
               multiplayer_single_root_save_disposition::canonical_state_uncertain );
    }

    SECTION( "a completed result from the previous turn cannot close the next turn" ) {
        REQUIRE( owner->complete_turn_after_player_end( turn_result ).status ==
                 multiplayer_single_root_owner_status::applied );
        player.set_moves( 100 );
        // The unit-test game has no installed gamemode; keep the second owned
        // invocation on the same new-game-safe branch as the first.
        g->new_game = true;
        bool saw_second_world_processing = false;
        const multiplayer_owned_turn_result second_turn_result =
            run_owned_wait_turn( *owner, player, runtime,
                                 saw_second_world_processing );
        REQUIRE( second_turn_result.status ==
                 multiplayer_owned_turn_status::completed );
        REQUIRE( second_turn_result.player_end_completed() );
        REQUIRE( saw_second_world_processing );

        const multiplayer_single_root_owner_result replayed =
            owner->complete_turn_after_player_end( turn_result );
        CHECK( replayed.status == multiplayer_single_root_owner_status::faulted );
        CHECK( owner->is_faulted() );
        CHECK( owner->save_disposition() ==
               multiplayer_single_root_save_disposition::canonical_state_uncertain );
    }
}

TEST_CASE( "multiplayer_single_root_owner_fail_stops_a_mismatched_admission_receipt",
           "[multiplayer][single_root_owner][session_directory][fault]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &player = get_avatar();
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        g->multiplayer_players().find_runtime( player );
    REQUIRE( runtime );
    std::unique_ptr<multiplayer_single_root_owner> owner = make_owner();
    const multiplayer_single_root_admission_plan plan = owner->plan_admission(
                authentication_request( *runtime ) );
    REQUIRE( plan );

    const multiplayer_single_root_owner_result rejected = owner->execute_admission(
                plan, []( const std::uint64_t admission_id,
    const multiplayer_session_binding & binding ) {
        return multiplayer_single_root_admission_publish_receipt {
            multiplayer_single_root_admission_publish_outcome::published,
            admission_id + 1, binding
        };
    } );
    CHECK( rejected.status == multiplayer_single_root_owner_status::faulted );
    CHECK( rejected.next_effect ==
           multiplayer_root_lifecycle_effect::fatal_shutdown );
    CHECK( owner->is_faulted() );
    CHECK( owner->save_disposition() ==
           multiplayer_single_root_save_disposition::allowed );
}

TEST_CASE( "multiplayer_single_root_owner_graceful_boundary_departure_waits_for_exact_close",
           "[multiplayer][single_root_owner][root_lifecycle]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &player = get_avatar();
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        g->multiplayer_players().find_runtime( player );
    REQUIRE( runtime );
    std::unique_ptr<multiplayer_single_root_owner> owner = make_owner();
    const multiplayer_session_binding binding =
        publish_initial_binding( *owner, *runtime );
    constexpr std::uint64_t request_sequence = 77;
    const auto now = multiplayer_single_root_owner::clock::time_point{};

    const multiplayer_single_root_owner_result departed =
        owner->record_graceful_departure( binding, request_sequence, now );
    REQUIRE( departed.status == multiplayer_single_root_owner_status::applied );
    CHECK( departed.next_effect ==
           multiplayer_root_lifecycle_effect::complete_graceful_release );
    CHECK( owner->is_dormant() );
    CHECK( runtime->status() == multiplayer_player_status::offline );
    CHECK( owner->save_disposition() ==
           multiplayer_single_root_save_disposition::allowed );

    const std::optional<multiplayer_single_root_graceful_completion> completion =
        owner->pending_graceful_completion();
    REQUIRE( completion );
    CHECK( completion->binding == binding );
    CHECK( completion->request_sequence == request_sequence );
    REQUIRE( owner->record_graceful_completion_queued( *completion ).status ==
             multiplayer_single_root_owner_status::applied );
    CHECK_FALSE( owner->pending_graceful_completion() );
    CHECK( owner->snapshot().graceful_completion_queued );

    const multiplayer_single_root_owner_result closed =
        owner->record_transport_disconnected( binding, now );
    REQUIRE( closed.status == multiplayer_single_root_owner_status::applied );
    CHECK( owner->snapshot().connection ==
           multiplayer_root_connection_state::disconnected );
    CHECK( owner->save_disposition() ==
           multiplayer_single_root_save_disposition::allowed );
    CHECK( owner->record_transport_disconnected( binding, now ).status ==
           multiplayer_single_root_owner_status::duplicate );

    multiplayer_session_binding stale = binding;
    ++stale.connection;
    CHECK( owner->record_transport_disconnected( stale, now ).status ==
           multiplayer_single_root_owner_status::stale );

    const multiplayer_single_root_admission_plan reactivation =
        owner->plan_admission( resume_request(
                                   *runtime, binding.session_generation, 2, 102, 2 ) );
    REQUIRE( reactivation );
    REQUIRE( owner->execute_admission(
                 reactivation, []( const std::uint64_t admission_id,
    const multiplayer_session_binding & receipt_binding ) {
        return multiplayer_single_root_admission_publish_receipt {
            multiplayer_single_root_admission_publish_outcome::published,
            admission_id, receipt_binding
        };
    } ).status == multiplayer_single_root_owner_status::applied );
    CHECK( runtime->status() == multiplayer_player_status::active );
}

TEST_CASE( "multiplayer_single_root_owner_resumes_disconnect_grace_at_exact_next_generation",
           "[multiplayer][single_root_owner][resume]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &player = get_avatar();
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        g->multiplayer_players().find_runtime( player );
    REQUIRE( runtime );
    std::unique_ptr<multiplayer_single_root_owner> owner = make_owner();
    const multiplayer_session_binding generation_one =
        publish_initial_binding( *owner, *runtime );
    REQUIRE( owner->begin_turn().status ==
             multiplayer_single_root_owner_status::applied );
    const auto now = multiplayer_single_root_owner::clock::time_point{};

    const multiplayer_single_root_owner_result disconnected =
        owner->record_transport_disconnected( generation_one, now );
    REQUIRE( disconnected.status ==
             multiplayer_single_root_owner_status::applied );
    CHECK( disconnected.next_effect ==
           multiplayer_root_lifecycle_effect::wait_for_disconnect_deadline );
    REQUIRE( owner->snapshot().barrier ==
             multiplayer_root_barrier_state::disconnected_grace );

    const multiplayer_single_root_admission_plan resume = owner->plan_admission(
                resume_request( *runtime, generation_one.session_generation ) );
    REQUIRE( resume );
    CHECK( resume.directory_plan().advances_generation );
    CHECK_FALSE( resume.directory_plan().replays_committed_generation );
    CHECK( multiplayer_is_next_session_generation(
               generation_one.session_generation,
               resume.directory_plan().committed_session_generation ) );
    CHECK( resume.lifecycle_result().next_effect ==
           multiplayer_root_lifecycle_effect::resume_barrier_plus_one );
    REQUIRE( owner->execute_admission(
                 resume, []( const std::uint64_t admission_id,
    const multiplayer_session_binding & receipt_binding ) {
        return multiplayer_single_root_admission_publish_receipt {
            multiplayer_single_root_admission_publish_outcome::published,
            admission_id, receipt_binding
        };
    } ).status == multiplayer_single_root_owner_status::applied );

    const std::optional<multiplayer_session_binding> generation_two =
        owner->session_for_player( runtime->player_id().str() );
    REQUIRE( generation_two );
    CHECK( multiplayer_is_next_session_generation(
               generation_one.session_generation,
               generation_two->session_generation ) );
    CHECK( runtime->session_generation() == generation_two->session_generation );
    CHECK( owner->snapshot().session_generation ==
           generation_two->session_generation );
    REQUIRE( owner->snapshot().participant );
    CHECK( owner->snapshot().participant->session_generation ==
           generation_two->session_generation );
    REQUIRE( owner->current_slot() );
    CHECK( owner->current_slot()->participant.session_generation ==
           generation_two->session_generation );
    CHECK( owner->can_execute_command() );
}

TEST_CASE( "multiplayer_single_root_owner_repairs_unpublished_resume_then_replays_exact_generation",
           "[multiplayer][single_root_owner][resume][owned_turn]" )
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

    avatar &player = get_avatar();
    player.set_moves( 100 );
    g->new_game = true;
    g->uquit = QUIT_NO;
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        g->multiplayer_players().find_runtime( player );
    REQUIRE( runtime );
    std::unique_ptr<multiplayer_single_root_owner> owner = make_owner();
    const multiplayer_session_binding original_binding =
        publish_initial_binding( *owner, *runtime );
    const std::uint64_t original_generation =
        original_binding.session_generation;
    REQUIRE( owner->begin_turn().status ==
             multiplayer_single_root_owner_status::applied );
    const auto now = multiplayer_single_root_owner::clock::time_point{};
    REQUIRE( owner->record_transport_disconnected(
                 original_binding, now ).status ==
             multiplayer_single_root_owner_status::applied );

    const multiplayer_session_admission_request first_request =
        resume_request( *runtime, original_generation, 2, 102, 2 );
    const multiplayer_single_root_admission_plan first_resume =
        owner->plan_admission( first_request );
    REQUIRE( first_resume );
    REQUIRE( first_resume.directory_plan().advances_generation );
    const std::uint64_t repaired_generation =
        first_resume.directory_plan().committed_session_generation;
    REQUIRE( multiplayer_is_next_session_generation(
                 original_generation, repaired_generation ) );
    const multiplayer_single_root_owner_result unpublished =
        owner->execute_admission(
            first_resume, []( const std::uint64_t admission_id,
    const multiplayer_session_binding & receipt_binding ) {
        return multiplayer_single_root_admission_publish_receipt {
            multiplayer_single_root_admission_publish_outcome::not_published,
            admission_id, receipt_binding
        };
    } );
    REQUIRE( unpublished.status == multiplayer_single_root_owner_status::applied );
    CHECK( unpublished.next_effect ==
           multiplayer_root_lifecycle_effect::wait_for_disconnect_deadline );
    CHECK_FALSE( owner->session_for_player( runtime->player_id().str() ) );
    CHECK( runtime->session_generation() == repaired_generation );
    REQUIRE( owner->current_slot() );
    CHECK( owner->current_slot()->participant.session_generation ==
           repaired_generation );
    CHECK( owner->current_slot()->state ==
           multiplayer_turn_participant_state::disconnected_grace );

    multiplayer_session_admission_request replay_request = first_request;
    replay_request.admission_id = 3;
    replay_request.connection = 103;
    replay_request.session = session_id( 3 );
    const multiplayer_single_root_admission_plan replay =
        owner->plan_admission( replay_request );
    REQUIRE( replay );
    CHECK_FALSE( replay.directory_plan().advances_generation );
    REQUIRE( replay.directory_plan().replays_committed_generation );
    REQUIRE( replay.directory_plan().committed_session_generation ==
             repaired_generation );
    REQUIRE( owner->execute_admission(
                 replay, []( const std::uint64_t admission_id,
    const multiplayer_session_binding & receipt_binding ) {
        return multiplayer_single_root_admission_publish_receipt {
            multiplayer_single_root_admission_publish_outcome::published,
            admission_id, receipt_binding
        };
    } ).status == multiplayer_single_root_owner_status::applied );
    CHECK( runtime->session_generation() == repaired_generation );
    REQUIRE( owner->current_slot() );
    CHECK( owner->current_slot()->state ==
           multiplayer_turn_participant_state::awaiting_command );
    REQUIRE( owner->can_execute_command() );

    int executed_player_actions = 0;
    const multiplayer_turn_phase_adapter_status executed =
        owner->execute_current_player(
    [&]( multiplayer_player_runtime & callback_runtime, avatar & callback_player ) {
        ++executed_player_actions;
        CHECK( &callback_runtime == runtime.get() );
        CHECK( &callback_player == &player );
        if( !multiplayer_execute_wait(
                *g, callback_player,
                multiplayer_wait_execution_mode::authoritative_forced ) ) {
            return std::optional<multiplayer_turn_action_disposition>();
        }
        return std::optional<multiplayer_turn_action_disposition>(
                   multiplayer_turn_action_disposition::accepted_finished );
    } );
    REQUIRE( executed == multiplayer_turn_phase_adapter_status::completed );
    CHECK( executed_player_actions == 1 );
    CHECK( player.get_moves() <= 0 );

    int unexpected_action_hooks = 0;
    const multiplayer_owned_turn_result turn_result =
        run_owned_turn_after_external_terminal( *owner, unexpected_action_hooks );
    REQUIRE( turn_result.status == multiplayer_owned_turn_status::completed );
    CHECK( unexpected_action_hooks == 0 );
    REQUIRE( owner->complete_turn_after_player_end( turn_result ).status ==
             multiplayer_single_root_owner_status::applied );
    CHECK( runtime->status() == multiplayer_player_status::active );
    CHECK( owner->can_begin_turn() );
    CHECK( owner->save_disposition() ==
           multiplayer_single_root_save_disposition::allowed );
}

TEST_CASE( "multiplayer_single_root_owner_times_out_to_forced_wait_and_dormant_world_completion",
           "[multiplayer][single_root_owner][disconnect][owned_turn]" )
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

    avatar &player = get_avatar();
    player.set_moves( 100 );
    g->new_game = true;
    g->uquit = QUIT_NO;
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        g->multiplayer_players().find_runtime( player );
    REQUIRE( runtime );
    std::unique_ptr<multiplayer_single_root_owner> owner =
        make_owner( std::chrono::seconds( 5 ) );
    const multiplayer_session_binding binding =
        publish_initial_binding( *owner, *runtime );
    REQUIRE( owner->begin_turn().status ==
             multiplayer_single_root_owner_status::applied );
    const auto now = multiplayer_single_root_owner::clock::time_point{};

    const multiplayer_single_root_owner_result disconnected =
        owner->record_transport_disconnected( binding, now );
    REQUIRE( disconnected.status ==
             multiplayer_single_root_owner_status::applied );
    REQUIRE( disconnected.next_effect ==
             multiplayer_root_lifecycle_effect::wait_for_disconnect_deadline );
    CHECK( owner->save_disposition() ==
           multiplayer_single_root_save_disposition::open_turn );

    const multiplayer_single_root_owner_result waited =
        owner->progress_departure( now + std::chrono::seconds( 5 ) );
    REQUIRE( waited.status == multiplayer_single_root_owner_status::applied );
    REQUIRE( waited.next_effect ==
             multiplayer_root_lifecycle_effect::claim_world );
    CHECK( player.get_moves() <= 0 );
    CHECK( owner->snapshot().barrier ==
           multiplayer_root_barrier_state::terminal );

    int action_calls = 0;
    const multiplayer_owned_turn_result turn_result =
        run_owned_turn_after_external_terminal( *owner, action_calls );
    REQUIRE( turn_result.status == multiplayer_owned_turn_status::completed );
    REQUIRE( turn_result.player_end_completed() );
    CHECK( action_calls == 0 );
    CHECK( owner->snapshot().barrier ==
           multiplayer_root_barrier_state::world_processing );
    CHECK( owner->save_disposition() ==
           multiplayer_single_root_save_disposition::open_turn );

    const multiplayer_single_root_owner_result completed =
        owner->complete_turn_after_player_end( turn_result );
    REQUIRE( completed.status == multiplayer_single_root_owner_status::applied );
    CHECK( owner->is_dormant() );
    CHECK( runtime->status() == multiplayer_player_status::offline );
    CHECK( owner->snapshot().barrier == multiplayer_root_barrier_state::none );
    CHECK_FALSE( owner->can_begin_turn() );
    CHECK( owner->begin_turn().status ==
           multiplayer_single_root_owner_status::not_ready );
    CHECK( owner->save_disposition() ==
           multiplayer_single_root_save_disposition::allowed );

    const multiplayer_single_root_admission_plan reactivation =
        owner->plan_admission( resume_request(
                                   *runtime, binding.session_generation, 2, 102, 2 ) );
    REQUIRE( reactivation );
    REQUIRE( owner->execute_admission(
                 reactivation, []( const std::uint64_t admission_id,
    const multiplayer_session_binding & receipt_binding ) {
        return multiplayer_single_root_admission_publish_receipt {
            multiplayer_single_root_admission_publish_outcome::published,
            admission_id, receipt_binding
        };
    } ).status == multiplayer_single_root_owner_status::applied );
    CHECK( runtime->status() == multiplayer_player_status::active );
}

TEST_CASE( "multiplayer_single_root_owner_fail_stops_an_early_owned_turn_abort",
           "[multiplayer][single_root_owner][owned_turn][fault]" )
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

    avatar &player = get_avatar();
    player.set_moves( 100 );
    g->uquit = QUIT_NO;
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        g->multiplayer_players().find_runtime( player );
    REQUIRE( runtime );
    std::unique_ptr<multiplayer_single_root_owner> owner = make_owner();
    publish_initial_binding( *owner, *runtime );
    REQUIRE( owner->begin_turn().status ==
             multiplayer_single_root_owner_status::applied );

    multiplayer_owned_remote_turn_hooks hooks;
    hooks.execute_player_action = []() {
        return false;
    };
    hooks.complete_player_phase = []() {
        return true;
    };
    hooks.execute_world = []( multiplayer_owned_world_thunk & world_thunk ) {
        return world_thunk();
    };
    const multiplayer_owned_turn_result aborted =
        owner->execute_owned_turn( hooks );
    REQUIRE( aborted.status == multiplayer_owned_turn_status::aborted );
    REQUIRE( aborted.progress == multiplayer_owned_turn_progress::turn_started );
    REQUIRE( aborted.abort_stage ==
             multiplayer_owned_turn_abort_stage::player_action );

    const multiplayer_single_root_owner_result rejected =
        owner->complete_turn_after_player_end( aborted );
    CHECK( rejected.status == multiplayer_single_root_owner_status::faulted );
    CHECK( owner->is_faulted() );
    CHECK( owner->save_disposition() ==
           multiplayer_single_root_save_disposition::canonical_state_uncertain );
}

TEST_CASE( "multiplayer_single_root_owner_fault_save_disposition_tracks_safe_boundary",
           "[multiplayer][single_root_owner][fault]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );

    avatar &player = get_avatar();
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        g->multiplayer_players().find_runtime( player );
    REQUIRE( runtime );
    std::unique_ptr<multiplayer_single_root_owner> owner = make_owner();
    publish_initial_binding( *owner, *runtime );

    SECTION( "a pre-turn fault proven before gameplay may use the normal save" ) {
        CHECK( owner->latch_fault( false ).status ==
               multiplayer_single_root_owner_status::faulted );
        CHECK( owner->save_disposition() ==
               multiplayer_single_root_save_disposition::allowed );
    }

    SECTION( "an open-turn fault cannot use the normal save" ) {
        REQUIRE( owner->begin_turn().status ==
                 multiplayer_single_root_owner_status::applied );
        CHECK( owner->latch_fault( false ).status ==
               multiplayer_single_root_owner_status::faulted );
        CHECK( owner->save_disposition() ==
               multiplayer_single_root_save_disposition::open_turn );
    }

    SECTION( "canonical uncertainty always refuses a new save" ) {
        CHECK( owner->latch_fault( true ).status ==
               multiplayer_single_root_owner_status::faulted );
        CHECK( owner->save_disposition() ==
               multiplayer_single_root_save_disposition::canonical_state_uncertain );
    }
}

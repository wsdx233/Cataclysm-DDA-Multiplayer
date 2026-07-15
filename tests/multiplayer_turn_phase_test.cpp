#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "calendar.h"
#include "enums.h"
#include "game.h"
#include "map_helpers.h"
#include "multiplayer_command_executor.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_player_runtime.h"
#include "multiplayer_turn_phase.h"
#include "multiplayer_turn_phase_adapter.h"
#include "multiplayer_turn_scheduler.h"
#include "options_helpers.h"
#include "player_helpers.h"

namespace
{

static_assert( !std::is_copy_constructible_v<multiplayer_owned_world_thunk> );
static_assert( !std::is_move_constructible_v<multiplayer_owned_world_thunk> );

multiplayer_turn_participant_key participant_key(
    const multiplayer_player_runtime &runtime )
{
    return { runtime.player_id(), runtime.session_generation() };
}

class recording_turn_phase_observer : public multiplayer_turn_phase_observer
{
    public:
        explicit recording_turn_phase_observer( std::vector<std::string> *events = nullptr ) :
            events( events ) {}

        void on_phase_finished( const multiplayer_turn_phase phase,
                                const std::chrono::nanoseconds elapsed ) noexcept override {
            if( count < phases.size() ) {
                phases[count] = phase;
                durations[count] = elapsed;
            }
            if( events != nullptr ) {
                events->emplace_back( multiplayer_turn_phase_name( phase ) );
            }
            ++count;
        }

        std::array<multiplayer_turn_phase, 5> phases = {};
        std::array<std::chrono::nanoseconds, 5> durations = {};
        std::size_t count = 0;

    private:
        std::vector<std::string> *events;
};

} // namespace

TEST_CASE( "game_do_turn_reports_multiplayer_phase_order_and_timings",
           "[multiplayer][turn_phase]" )
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
    player.set_moves( 0 );
    g->uquit = QUIT_NO;

    recording_turn_phase_observer observer;
    {
        scoped_multiplayer_turn_phase_observer observe( observer );
        CHECK_FALSE( g->do_turn() );
    }

    const std::array<multiplayer_turn_phase, 5> expected = {
        multiplayer_turn_phase::turn_begin,
        multiplayer_turn_phase::player_begin,
        multiplayer_turn_phase::player_input,
        multiplayer_turn_phase::world,
        multiplayer_turn_phase::player_end
    };
    REQUIRE( observer.count == expected.size() );
    CHECK( observer.phases == expected );
    for( std::size_t index = 0; index < observer.count; ++index ) {
        CAPTURE( multiplayer_turn_phase_name( observer.phases[index] ) );
        CHECK( observer.durations[index].count() >= 0 );
    }
}

TEST_CASE( "game_remote_turn_preserves_single_player_phase_order_and_replenishment",
           "[multiplayer][turn_phase][phase_adapter]" )
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
    const safe_mode_type original_safe_mode = g->get_safe_mode();
    on_out_of_scope restore_safe_mode( [original_safe_mode]() {
        g->set_safe_mode( original_safe_mode );
    } );
    g->set_safe_mode( SAFE_MODE_OFF );
    g->uquit = QUIT_NO;

    g->new_game = true;
    player.set_moves( 100 );
    const time_point turn_before = calendar::turn;

    int action_calls = 0;
    std::vector<std::string> events;
    recording_turn_phase_observer observer( &events );
    const bool stopped = [&]() {
        scoped_multiplayer_turn_phase_observer observe( observer );
        return g->do_turn_remote( [&]() -> std::optional<bool> {
            events.emplace_back( "action" );
            ++action_calls;
            multiplayer_player_command command;
            command.client_sequence = 1;
            command.base_revision = 1;
            command.kind = multiplayer_command_kind::wait;
            const multiplayer_command_execution execution =
            multiplayer_execute_basic_command( *g, player, command );
            REQUIRE( execution.status == multiplayer_command_status::accepted );
            CHECK( execution.action_taken );
            CHECK( player.get_moves() <= 0 );
            return execution.action_taken;
        } );
    }
    ();

    CHECK_FALSE( stopped );
    CHECK( action_calls == 1 );
    CHECK_FALSE( g->new_game );
    CHECK( calendar::turn == turn_before );
    CHECK( player.get_moves() > 0 );
    const std::vector<std::string> expected = {
        "turn_begin",
        "player_begin",
        "action",
        "player_input",
        "world",
        "player_end"
    };
    CHECK( events == expected );
}

TEST_CASE( "game_owned_remote_turn_wraps_real_world_and_avoids_double_action_bookkeeping",
           "[multiplayer][turn_phase][owned_turn]" )
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
    const multiplayer_turn_participant_key key = participant_key( *runtime );

    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 17, { key } ) );
    multiplayer_turn_phase_adapter adapter( *g, scheduler );
    const int actions_before = g->get_moves_since_last_save();
    int action_calls = 0;
    int player_phase_completion_calls = 0;
    int world_claims = 0;
    std::vector<std::string> events;
    recording_turn_phase_observer observer( &events );

    multiplayer_owned_remote_turn_hooks hooks;
    hooks.execute_player_action = [&]() {
        events.emplace_back( "action" );
        ++action_calls;
        return adapter.execute_current_player(
                   key,
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
        events.emplace_back( "player_phase_complete" );
        ++player_phase_completion_calls;
        return scheduler.stage() == multiplayer_turn_scheduler_stage::world_ready;
    };
    hooks.execute_world = [&]( multiplayer_owned_world_thunk & world_thunk ) {
        events.emplace_back( "world_hook" );
        return adapter.execute_claimed_world(
        [&]( const multiplayer_world_ticket & ticket ) {
            events.emplace_back( "legacy_world" );
            ++world_claims;
            CHECK( ticket.shared_turn == 17 );
            return world_thunk();
        } ) == multiplayer_turn_phase_adapter_status::completed;
    };

    multiplayer_owned_turn_result result;
    {
        scoped_multiplayer_turn_phase_observer observe( observer );
        result = g->do_turn_remote_owned( hooks );
    }

    CHECK( result.status == multiplayer_owned_turn_status::completed );
    CHECK( result.progress == multiplayer_owned_turn_progress::turn_completed );
    CHECK( result.abort_stage == multiplayer_owned_turn_abort_stage::none );
    CHECK( result.preserves_normal_save_boundary() );
    CHECK( result.gameplay_side_effects_may_have_occurred() );
    CHECK( action_calls == 1 );
    CHECK( player_phase_completion_calls == 1 );
    CHECK( world_claims == 1 );
    CHECK( g->get_moves_since_last_save() == actions_before + 1 );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::idle );
    CHECK( player.get_moves() > 0 );
    const std::vector<std::string> expected = {
        "turn_begin",
        "player_begin",
        "action",
        "player_phase_complete",
        "player_input",
        "world_hook",
        "legacy_world",
        "world",
        "player_end"
    };
    CHECK( events == expected );
}

TEST_CASE( "game_owned_remote_turn_terminalizes_player_phase_when_action_is_skipped",
           "[multiplayer][turn_phase][owned_turn]" )
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
    g->uquit = QUIT_NO;
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        g->multiplayer_players().find_runtime( player );
    REQUIRE( runtime );
    const multiplayer_turn_participant_key key = participant_key( *runtime );

    SECTION( "zero moves" ) {
        player.set_moves( 0 );
    }
    SECTION( "sleep" ) {
        player.set_moves( 100 );
        player.add_effect( efftype_id( "sleep" ), 1_hours );
    }
    SECTION( "activity consumes the player phase" ) {
        player.set_moves( 100 );
        player.assign_activity( wait_activity_actor( 1_turns ) );
    }

    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 18, { key } ) );
    multiplayer_turn_phase_adapter adapter( *g, scheduler );
    int action_calls = 0;
    int player_phase_completion_calls = 0;
    int world_claims = 0;

    multiplayer_owned_remote_turn_hooks hooks;
    hooks.execute_player_action = [&]() {
        ++action_calls;
        return false;
    };
    hooks.complete_player_phase = [&]() {
        ++player_phase_completion_calls;
        return scheduler.record_player_phase_completed( key );
    };
    hooks.execute_world = [&]( multiplayer_owned_world_thunk & world_thunk ) {
        return adapter.execute_claimed_world(
        [&]( const multiplayer_world_ticket & ticket ) {
            ++world_claims;
            CHECK( ticket.shared_turn == 18 );
            return world_thunk();
        } ) == multiplayer_turn_phase_adapter_status::completed;
    };

    const multiplayer_owned_turn_result result = g->do_turn_remote_owned( hooks );

    CHECK( result.status == multiplayer_owned_turn_status::completed );
    CHECK( result.progress == multiplayer_owned_turn_progress::turn_completed );
    CHECK( action_calls == 0 );
    CHECK( player_phase_completion_calls == 1 );
    CHECK( world_claims == 1 );
    CHECK( scheduler.stage() == multiplayer_turn_scheduler_stage::idle );
    CHECK( player.get_moves() > 0 );
}

TEST_CASE( "game_owned_remote_turn_reports_typed_abort_progress_and_stops_later_phases",
           "[multiplayer][turn_phase][owned_turn]" )
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
    g->uquit = QUIT_NO;
    int action_calls = 0;
    int player_phase_completion_calls = 0;
    int world_hook_calls = 0;
    int world_thunk_calls = 0;
    multiplayer_owned_turn_progress expected_progress =
        multiplayer_owned_turn_progress::turn_started;
    multiplayer_owned_turn_abort_stage expected_abort_stage =
        multiplayer_owned_turn_abort_stage::player_action;

    multiplayer_owned_remote_turn_hooks hooks;
    hooks.execute_player_action = [&]() {
        ++action_calls;
        return false;
    };
    hooks.complete_player_phase = [&]() {
        ++player_phase_completion_calls;
        return false;
    };
    hooks.execute_world = [&]( multiplayer_owned_world_thunk & world_thunk ) {
        ++world_hook_calls;
        ++world_thunk_calls;
        return world_thunk();
    };

    SECTION( "action callback failure" ) {
        player.set_moves( 100 );
        expected_progress = multiplayer_owned_turn_progress::turn_started;
        expected_abort_stage = multiplayer_owned_turn_abort_stage::player_action;
    }
    SECTION( "player phase completion failure without an action" ) {
        player.set_moves( 0 );
        expected_progress = multiplayer_owned_turn_progress::turn_started;
        expected_abort_stage =
            multiplayer_owned_turn_abort_stage::player_phase_completion;
    }
    SECTION( "world hook omits the thunk" ) {
        player.set_moves( 0 );
        hooks.complete_player_phase = [&]() {
            ++player_phase_completion_calls;
            return true;
        };
        hooks.execute_world = [&]( multiplayer_owned_world_thunk & ) {
            ++world_hook_calls;
            return true;
        };
        expected_progress = multiplayer_owned_turn_progress::player_phase_completed;
        expected_abort_stage = multiplayer_owned_turn_abort_stage::world;
    }
    SECTION( "world hook reports failure after the thunk" ) {
        player.set_moves( 0 );
        hooks.complete_player_phase = [&]() {
            ++player_phase_completion_calls;
            return true;
        };
        hooks.execute_world = [&]( multiplayer_owned_world_thunk & world_thunk ) {
            ++world_hook_calls;
            ++world_thunk_calls;
            CHECK( world_thunk() );
            return false;
        };
        expected_progress = multiplayer_owned_turn_progress::world_completed;
        expected_abort_stage = multiplayer_owned_turn_abort_stage::world;
    }
    SECTION( "world thunk rejects a second invocation" ) {
        player.set_moves( 0 );
        hooks.complete_player_phase = [&]() {
            ++player_phase_completion_calls;
            return true;
        };
        hooks.execute_world = [&]( multiplayer_owned_world_thunk & world_thunk ) {
            ++world_hook_calls;
            ++world_thunk_calls;
            CHECK( world_thunk() );
            ++world_thunk_calls;
            CHECK_FALSE( world_thunk() );
            return true;
        };
        expected_progress = multiplayer_owned_turn_progress::world_completed;
        expected_abort_stage = multiplayer_owned_turn_abort_stage::world;
    }

    const multiplayer_owned_turn_result result = g->do_turn_remote_owned( hooks );

    CHECK( result.status == multiplayer_owned_turn_status::aborted );
    CHECK( result.progress == expected_progress );
    CHECK( result.abort_stage == expected_abort_stage );
    CHECK_FALSE( result.preserves_normal_save_boundary() );
    CHECK( result.gameplay_side_effects_may_have_occurred() );
    if( expected_abort_stage == multiplayer_owned_turn_abort_stage::player_action ) {
        CHECK( action_calls == 1 );
        CHECK( player_phase_completion_calls == 0 );
        CHECK( world_hook_calls == 0 );
    } else if( expected_abort_stage ==
               multiplayer_owned_turn_abort_stage::player_phase_completion ) {
        CHECK( action_calls == 0 );
        CHECK( player_phase_completion_calls == 1 );
        CHECK( world_hook_calls == 0 );
    } else {
        CHECK( action_calls == 0 );
        CHECK( player_phase_completion_calls == 1 );
        CHECK( world_hook_calls == 1 );
    }
    if( expected_progress == multiplayer_owned_turn_progress::player_phase_completed ) {
        CHECK( world_thunk_calls == 0 );
    }
}

TEST_CASE( "game_owned_remote_turn_rejects_invalid_hooks_before_opening_a_turn",
           "[multiplayer][turn_phase][owned_turn]" )
{
    multiplayer_owned_remote_turn_hooks hooks;
    hooks.execute_player_action = []() {
        return true;
    };

    const multiplayer_owned_turn_result result = g->do_turn_remote_owned( hooks );

    CHECK( result.status == multiplayer_owned_turn_status::aborted );
    CHECK( result.progress == multiplayer_owned_turn_progress::not_started );
    CHECK( result.abort_stage == multiplayer_owned_turn_abort_stage::invalid_hooks );
    CHECK( result.preserves_normal_save_boundary() );
    CHECK_FALSE( result.gameplay_side_effects_may_have_occurred() );
}

TEST_CASE( "game_owned_remote_turn_reports_game_over_without_running_hooks",
           "[multiplayer][turn_phase][owned_turn]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );
    restore_on_out_of_scope restore_quit( g->uquit );
    g->uquit = QUIT_NOSAVED;
    int hook_calls = 0;

    multiplayer_owned_remote_turn_hooks hooks;
    hooks.execute_player_action = [&]() {
        ++hook_calls;
        return true;
    };
    hooks.complete_player_phase = [&]() {
        ++hook_calls;
        return true;
    };
    hooks.execute_world = [&]( multiplayer_owned_world_thunk & world_thunk ) {
        ++hook_calls;
        return world_thunk();
    };

    const multiplayer_owned_turn_result result = g->do_turn_remote_owned( hooks );

    CHECK( result.status == multiplayer_owned_turn_status::game_over );
    CHECK( result.progress == multiplayer_owned_turn_progress::not_started );
    CHECK( result.abort_stage == multiplayer_owned_turn_abort_stage::none );
    CHECK( result.preserves_normal_save_boundary() );
    CHECK_FALSE( result.gameplay_side_effects_may_have_occurred() );
    CHECK( hook_calls == 0 );
}

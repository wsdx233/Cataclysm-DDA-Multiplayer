#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "avatar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "calendar.h"
#include "enums.h"
#include "game.h"
#include "map_helpers.h"
#include "multiplayer_command_executor.h"
#include "multiplayer_turn_phase.h"
#include "options_helpers.h"
#include "player_helpers.h"

namespace
{

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

#include <array>
#include <chrono>
#include <cstddef>

#include "avatar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "calendar.h"
#include "enums.h"
#include "game.h"
#include "map_helpers.h"
#include "multiplayer_turn_phase.h"
#include "options_helpers.h"
#include "player_helpers.h"

namespace
{

class recording_turn_phase_observer : public multiplayer_turn_phase_observer
{
    public:
        void on_phase_finished( const multiplayer_turn_phase phase,
                                const std::chrono::nanoseconds elapsed ) noexcept override {
            if( count < phases.size() ) {
                phases[count] = phase;
                durations[count] = elapsed;
            }
            ++count;
        }

        std::array<multiplayer_turn_phase, 5> phases = {};
        std::array<std::chrono::nanoseconds, 5> durations = {};
        std::size_t count = 0;
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

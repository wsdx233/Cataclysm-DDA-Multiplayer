#include "multiplayer_turn_phase.h"

#include "cata_assert.h"

namespace
{

thread_local multiplayer_turn_phase_observer *current_observer = nullptr;

} // namespace

const char *multiplayer_turn_phase_name( const multiplayer_turn_phase phase )
{
    switch( phase ) {
        case multiplayer_turn_phase::turn_begin:
            return "turn_begin";
        case multiplayer_turn_phase::player_begin:
            return "player_begin";
        case multiplayer_turn_phase::player_input:
            return "player_input";
        case multiplayer_turn_phase::world:
            return "world";
        case multiplayer_turn_phase::player_end:
            return "player_end";
    }
    cata_assert( false );
    return "unknown";
}

scoped_multiplayer_turn_phase_observer::scoped_multiplayer_turn_phase_observer(
    multiplayer_turn_phase_observer &observer ) noexcept :
    previous( current_observer )
{
    current_observer = &observer;
}

scoped_multiplayer_turn_phase_observer::~scoped_multiplayer_turn_phase_observer()
{
    current_observer = previous;
}

multiplayer_turn_phase_trace::multiplayer_turn_phase_trace(
    const multiplayer_turn_phase initial_phase ) noexcept :
    observer( current_observer ),
    current_phase( initial_phase ),
    phase_started( observer != nullptr ? clock::now() : clock::time_point() )
{
}

multiplayer_turn_phase_trace::~multiplayer_turn_phase_trace()
{
    if( observer != nullptr ) {
        finish_current( clock::now() );
    }
}

void multiplayer_turn_phase_trace::enter( const multiplayer_turn_phase next_phase ) noexcept
{
    if( observer == nullptr ) {
        current_phase = next_phase;
        return;
    }
    const clock::time_point now = clock::now();
    finish_current( now );
    current_phase = next_phase;
    phase_started = now;
}

void multiplayer_turn_phase_trace::finish_current( const clock::time_point now ) noexcept
{
    observer->on_phase_finished(
        current_phase, std::chrono::duration_cast<std::chrono::nanoseconds>( now - phase_started ) );
}

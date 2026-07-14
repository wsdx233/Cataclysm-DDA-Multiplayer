#include "multiplayer_player_context.h"

#include "avatar.h"
#include "cata_assert.h"
#include "game.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_player_runtime.h"

multiplayer_active_player_guard::multiplayer_active_player_guard(
    game &owner, const shared_ptr_fast<avatar> &next ) :
    owner( owner )
{
    cata_assert( owner.is_simulation_thread() );
    if( !owner.is_simulation_thread() ) {
        return;
    }
    previous = owner.active_player_runtime_shared_ptr;
    cata_assert( previous != nullptr );
    cata_assert( next != nullptr );
    const shared_ptr_fast<multiplayer_player_runtime> next_runtime =
        owner.multiplayer_players().runtime_from_owner( next );
    cata_assert( next_runtime != nullptr );
    cata_assert( next_runtime != nullptr &&
                 next_runtime->status() == multiplayer_player_status::active );
    if( next_runtime == nullptr || next_runtime->status() != multiplayer_player_status::active ) {
        return;
    }
    owner.multiplayer_player_registry_ptr->enter_active_context();
    owner.set_active_player( next_runtime );
    engaged = true;
}

multiplayer_active_player_guard::~multiplayer_active_player_guard()
{
    if( engaged ) {
        owner.set_active_player( previous );
        owner.multiplayer_player_registry_ptr->leave_active_context();
    }
}

bool multiplayer_active_player_guard::is_engaged() const noexcept
{
    return engaged;
}

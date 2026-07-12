#pragma once
#ifndef CATA_SRC_MULTIPLAYER_PLAYER_CONTEXT_H
#define CATA_SRC_MULTIPLAYER_PLAYER_CONTEXT_H

#include "memory_fast.h"

class avatar;
class game;
class multiplayer_player_runtime;

/**
 * Temporarily selects a stable avatar as the current simulation player.
 *
 * This is a Phase 0 feasibility boundary.  It redirects the legacy global
 * player accessors and player-scoped runtime supplied by the human-player
 * registry.  It must only be used on the simulation thread.
 */
class multiplayer_active_player_guard
{
    public:
        multiplayer_active_player_guard( game &owner, const shared_ptr_fast<avatar> &next );
        ~multiplayer_active_player_guard();

        multiplayer_active_player_guard( const multiplayer_active_player_guard & ) = delete;
        multiplayer_active_player_guard &operator=(
            const multiplayer_active_player_guard & ) = delete;
        multiplayer_active_player_guard( multiplayer_active_player_guard && ) = delete;
        multiplayer_active_player_guard &operator=( multiplayer_active_player_guard && ) = delete;

    private:
        game &owner;
        shared_ptr_fast<multiplayer_player_runtime> previous;
        bool engaged = false;
};

#endif // CATA_SRC_MULTIPLAYER_PLAYER_CONTEXT_H

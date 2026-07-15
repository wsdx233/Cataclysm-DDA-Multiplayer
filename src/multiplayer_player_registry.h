#pragma once
#ifndef CATA_SRC_MULTIPLAYER_PLAYER_REGISTRY_H
#define CATA_SRC_MULTIPLAYER_PLAYER_REGISTRY_H

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "character_id.h"
#include "coordinates.h"
#include "memory_fast.h"

class avatar;
class multiplayer_active_player_guard;
class multiplayer_player_id;
class multiplayer_player_runtime;
class multiplayer_session_directory;

/**
 * Owns the human player runtimes represented in the shared reality bubble.
 *
 * Runtime and avatar objects keep stable addresses because this registry
 * stores shared owners rather than values.  Phase 0 refreshes the tiny ID and
 * position indexes when a live avatar changes, so ordinary Character movement
 * cannot leave stale entries.
 */
class multiplayer_player_registry
{
    public:
        multiplayer_player_registry();
        ~multiplayer_player_registry();

        bool register_player( const shared_ptr_fast<multiplayer_player_runtime> &runtime );
        bool unregister_player( const avatar &player );
        bool begin_session( const multiplayer_player_id &id );
        bool disconnect( const multiplayer_player_id &id );
        bool mark_dead( const multiplayer_player_id &id );
        void update_position( const avatar &player, const tripoint_abs_ms &old_position,
                              const tripoint_abs_ms &new_position );

        shared_ptr_fast<avatar> find_by_id( const character_id &id ) const;
        shared_ptr_fast<multiplayer_player_runtime> find_by_player_id(
            const multiplayer_player_id &id ) const;
        shared_ptr_fast<multiplayer_player_runtime> find_runtime( const avatar &player ) const;
        shared_ptr_fast<multiplayer_player_runtime> runtime_from_owner(
            const shared_ptr_fast<avatar> &player ) const;
        avatar *find_at( const tripoint_abs_ms &position ) const;
        /** Finds a living active/offline avatar at a world position. */
        avatar *find_living_world_avatar_at( const tripoint_abs_ms &position ) const;
        /** Finds another physical registry avatar, regardless of runtime status. */
        avatar *find_other_at( const tripoint_abs_ms &position,
                               const avatar &excluded ) const;
        /** Simulation-thread-only count of living active/offline world avatars. */
        std::size_t living_world_avatar_count() const;
        shared_ptr_fast<avatar> shared_from( const avatar &player ) const;
        bool contains( const avatar &player ) const;
        bool owns( const shared_ptr_fast<avatar> &player ) const;
        bool owns( const shared_ptr_fast<multiplayer_player_runtime> &runtime ) const;

        const std::vector<shared_ptr_fast<multiplayer_player_runtime>> &all() const;
        std::size_t size() const;

    private:
        void enter_active_context();
        void leave_active_context();
        bool active_context_is_clear() const noexcept;
        void rebuild_indexes() const;

        std::vector<shared_ptr_fast<multiplayer_player_runtime>> players;
        mutable std::unordered_map<int, avatar *> players_by_character_id;
        mutable std::unordered_map<tripoint_abs_ms, std::vector<avatar *>> players_by_position;
        mutable std::vector<const avatar *> indexed_players;
        mutable std::vector<character_id> indexed_character_ids;
        mutable std::vector<tripoint_abs_ms> indexed_positions;
        std::size_t active_context_depth = 0;

        friend class multiplayer_active_player_guard;
        friend class multiplayer_session_directory;
};

#endif // CATA_SRC_MULTIPLAYER_PLAYER_REGISTRY_H

#include "multiplayer_player_registry.h"

#include <algorithm>
#include <utility>

#include "avatar.h"
#include "cata_assert.h"
#include "multiplayer_player_runtime.h"

multiplayer_player_registry::multiplayer_player_registry() = default;

multiplayer_player_registry::~multiplayer_player_registry() = default;

bool multiplayer_player_registry::register_player(
    const shared_ptr_fast<multiplayer_player_runtime> &runtime )
{
    cata_assert( active_context_depth == 0 );
    cata_assert( runtime != nullptr );
    if( active_context_depth != 0 || runtime == nullptr ||
        runtime->player_owner() == nullptr || contains( runtime->player() ) ||
        runtime->status() != multiplayer_player_status::importing ||
        !runtime->player_id().is_valid() || find_by_player_id( runtime->player_id() ) != nullptr ) {
        return false;
    }

    const character_id id = runtime->player().getID();
    if( id.is_valid() && find_by_id( id ) != nullptr ) {
        return false;
    }

    players.emplace_back( runtime );
    return true;
}

bool multiplayer_player_registry::unregister_player( const avatar &player )
{
    cata_assert( active_context_depth == 0 );
    if( active_context_depth != 0 ) {
        return false;
    }
    const auto found = std::find_if( players.begin(), players.end(), [&player](
    const shared_ptr_fast<multiplayer_player_runtime> &candidate ) {
        return &candidate->player() == &player;
    } );
    if( found == players.end() ) {
        return false;
    }
    if( ( *found )->status() == multiplayer_player_status::active ) {
        return false;
    }

    players.erase( found );
    return true;
}

bool multiplayer_player_registry::begin_session( const multiplayer_player_id &id )
{
    cata_assert( active_context_depth == 0 );
    if( active_context_depth != 0 ) {
        return false;
    }
    const shared_ptr_fast<multiplayer_player_runtime> runtime = find_by_player_id( id );
    return runtime != nullptr && runtime->begin_session();
}

bool multiplayer_player_registry::disconnect( const multiplayer_player_id &id )
{
    cata_assert( active_context_depth == 0 );
    if( active_context_depth != 0 ) {
        return false;
    }
    const shared_ptr_fast<multiplayer_player_runtime> runtime = find_by_player_id( id );
    return runtime != nullptr && runtime->disconnect();
}

bool multiplayer_player_registry::mark_dead( const multiplayer_player_id &id )
{
    cata_assert( active_context_depth == 0 );
    if( active_context_depth != 0 ) {
        return false;
    }
    const shared_ptr_fast<multiplayer_player_runtime> runtime = find_by_player_id( id );
    return runtime != nullptr && runtime->mark_dead();
}

shared_ptr_fast<avatar> multiplayer_player_registry::find_by_id( const character_id &id ) const
{
    if( !id.is_valid() ) {
        return nullptr;
    }

    rebuild_indexes();
    const auto found = players_by_character_id.find( id.get_value() );
    return found == players_by_character_id.end() ? nullptr : shared_from( *found->second );
}

shared_ptr_fast<multiplayer_player_runtime> multiplayer_player_registry::find_by_player_id(
    const multiplayer_player_id &id ) const
{
    if( !id.is_valid() ) {
        return nullptr;
    }
    const auto found = std::find_if( players.begin(), players.end(), [&id](
    const shared_ptr_fast<multiplayer_player_runtime> &candidate ) {
        return candidate->player_id() == id;
    } );
    return found == players.end() ? nullptr : *found;
}

shared_ptr_fast<multiplayer_player_runtime> multiplayer_player_registry::find_runtime(
    const avatar &player ) const
{
    const auto found = std::find_if( players.begin(), players.end(), [&player](
    const shared_ptr_fast<multiplayer_player_runtime> &candidate ) {
        return &candidate->player() == &player;
    } );
    return found == players.end() ? nullptr : *found;
}

shared_ptr_fast<multiplayer_player_runtime> multiplayer_player_registry::runtime_from_owner(
    const shared_ptr_fast<avatar> &player ) const
{
    if( player == nullptr ) {
        return nullptr;
    }
    const shared_ptr_fast<multiplayer_player_runtime> runtime = find_runtime( *player );
    if( runtime == nullptr ) {
        return nullptr;
    }
    const shared_ptr_fast<avatar> &registered = runtime->player_owner();
    return !registered.owner_before( player ) && !player.owner_before( registered ) ?
           runtime : nullptr;
}

avatar *multiplayer_player_registry::find_at( const tripoint_abs_ms &position ) const
{
    rebuild_indexes();
    const auto found = players_by_position.find( position );
    return found == players_by_position.end() ||
           found->second.empty() ? nullptr : found->second.front();
}

shared_ptr_fast<avatar> multiplayer_player_registry::shared_from( const avatar &player ) const
{
    const shared_ptr_fast<multiplayer_player_runtime> runtime = find_runtime( player );
    return runtime == nullptr ? nullptr : runtime->player_owner();
}

bool multiplayer_player_registry::contains( const avatar &player ) const
{
    return shared_from( player ) != nullptr;
}

bool multiplayer_player_registry::owns( const shared_ptr_fast<avatar> &player ) const
{
    return runtime_from_owner( player ) != nullptr;
}

bool multiplayer_player_registry::owns(
    const shared_ptr_fast<multiplayer_player_runtime> &runtime ) const
{
    if( runtime == nullptr ) {
        return false;
    }
    const shared_ptr_fast<multiplayer_player_runtime> registered =
        find_by_player_id( runtime->player_id() );
    return registered != nullptr && !registered.owner_before( runtime ) &&
           !runtime.owner_before( registered );
}

const std::vector<shared_ptr_fast<multiplayer_player_runtime>> &
        multiplayer_player_registry::all() const
{
    return players;
}

std::size_t multiplayer_player_registry::size() const
{
    return players.size();
}

void multiplayer_player_registry::enter_active_context()
{
    ++active_context_depth;
}

void multiplayer_player_registry::leave_active_context()
{
    cata_assert( active_context_depth > 0 );
    if( active_context_depth > 0 ) {
        --active_context_depth;
    }
}

void multiplayer_player_registry::rebuild_indexes() const
{
    if( indexed_players.size() == players.size() &&
        indexed_character_ids.size() == players.size() &&
        indexed_positions.size() == players.size() ) {
        bool indexes_are_current = true;
        for( std::size_t i = 0; i < players.size(); ++i ) {
            const avatar &player = players[i]->player();
            if( indexed_players[i] != &player ||
                indexed_character_ids[i] != player.getID() ||
                indexed_positions[i] != player.pos_abs() ) {
                indexes_are_current = false;
                break;
            }
        }
        if( indexes_are_current ) {
            return;
        }
    }

    players_by_character_id.clear();
    players_by_position.clear();
    indexed_players.clear();
    indexed_character_ids.clear();
    indexed_positions.clear();
    indexed_players.reserve( players.size() );
    indexed_character_ids.reserve( players.size() );
    indexed_positions.reserve( players.size() );

    for( const shared_ptr_fast<multiplayer_player_runtime> &runtime : players ) {
        cata_assert( runtime != nullptr );
        avatar &player = runtime->player();
        const character_id id = player.getID();
        indexed_players.emplace_back( &player );
        indexed_character_ids.emplace_back( id );
        indexed_positions.emplace_back( player.pos_abs() );
        if( id.is_valid() ) {
            const auto inserted = players_by_character_id.emplace( id.get_value(), &player );
            cata_assert( inserted.second || inserted.first->second == &player );
        }
        players_by_position[player.pos_abs()].emplace_back( &player );
    }
}

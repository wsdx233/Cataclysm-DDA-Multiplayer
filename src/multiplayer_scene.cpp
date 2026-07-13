#include "multiplayer_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>

#include "avatar.h"
#include "calendar.h"
#include "creature.h"
#include "game.h"
#include "map.h"
#include "mapdata.h"
#include "monster.h"
#include "mtype.h"
#include "trap.h"

namespace
{

multiplayer_protocol_position protocol_position( const tripoint_abs_ms &position )
{
    const tripoint raw = position.raw();
    return { raw.x, raw.y, raw.z };
}

multiplayer_visible_attitude protocol_attitude( const Creature::Attitude attitude )
{
    switch( attitude ) {
        case Creature::Attitude::FRIENDLY:
            return multiplayer_visible_attitude::friendly;
        case Creature::Attitude::NEUTRAL:
            return multiplayer_visible_attitude::neutral;
        case Creature::Attitude::HOSTILE:
            return multiplayer_visible_attitude::hostile;
        case Creature::Attitude::ANY:
            return multiplayer_visible_attitude::unknown;
    }
    return multiplayer_visible_attitude::unknown;
}

std::uint8_t protocol_health_percent( const Creature &creature )
{
    const int maximum = creature.get_hp_max();
    if( maximum <= 0 ) {
        return 0;
    }
    return static_cast<std::uint8_t>( std::clamp(
                                          creature.get_hp() * 100 / maximum, 0, 100 ) );
}

class scene_entity_identity_registry
{
    public:
        std::uint64_t id_for( const shared_ptr_fast<monster> &creature ) {
            for( auto iter = ids.begin(); iter != ids.end(); ) {
                if( iter->first.use_count() == 1 ) {
                    iter = ids.erase( iter );
                } else {
                    ++iter;
                }
            }
            const auto found = ids.find( creature );
            if( found != ids.end() ) {
                return found->second;
            }
            const std::uint64_t id = next_id++;
            ids.emplace( creature, id );
            return id;
        }

    private:
        std::map<shared_ptr_fast<monster>, std::uint64_t> ids;
        std::uint64_t next_id = 1;
};

scene_entity_identity_registry scene_entity_ids;

std::uint8_t protocol_light_level( const float ambient_light )
{
    if( !std::isfinite( ambient_light ) || ambient_light <= 0.0f ) {
        return 0;
    }
    return static_cast<std::uint8_t>( std::clamp(
                                          std::lround( ambient_light ), 0L,
                                          static_cast<long>( std::numeric_limits<std::uint8_t>::max() ) ) );
}

} // namespace

bool multiplayer_build_visible_scene( game &simulation, const std::string &player_id,
                                      const std::string &character_id,
                                      const std::uint64_t server_revision, const int radius,
                                      multiplayer_scene_snapshot &snapshot,
                                      std::string &error )
{
    if( !simulation.is_simulation_thread() || server_revision == 0 || radius < 1 || radius > 60 ) {
        error = "visible scene must be built on the simulation thread with valid bounds";
        return false;
    }

    avatar &player = simulation.active_avatar();
    map &here = get_map();
    multiplayer_scene_snapshot result;
    result.server_revision = server_revision;
    result.turn = to_turns<std::int64_t>( calendar::turn - calendar::turn_zero );
    result.player.player_id = player_id;
    result.player.character_id = character_id;
    result.player.revision = server_revision;
    result.player.position = protocol_position( player.pos_abs() );
    result.player.moves = player.get_moves();
    result.player.pain = player.get_pain();
    result.player.stamina = player.get_stamina();
    if( player.activity ) {
        result.player.activity_id = player.activity.id().str();
    }

    multiplayer_visible_entity player_entity;
    player_entity.kind = multiplayer_visible_entity_kind::player;
    player_entity.stable_id = "player-" + character_id;
    player_entity.revision = server_revision;
    player_entity.position = result.player.position;
    player_entity.appearance_id = "avatar";
    player_entity.display_name = player.get_name();
    player_entity.attitude = multiplayer_visible_attitude::friendly;
    player_entity.health_percent = protocol_health_percent( player );
    result.entities.emplace_back( std::move( player_entity ) );

    for( monster &creature : simulation.all_monsters() ) {
        if( creature.is_dead() || !player.sees( here, creature ) ||
            rl_dist( player.pos_abs(), creature.pos_abs() ) > radius ) {
            continue;
        }
        multiplayer_visible_entity entity;
        entity.kind = multiplayer_visible_entity_kind::monster;
        const shared_ptr_fast<monster> creature_owner = simulation.shared_from( creature );
        if( !creature_owner ) {
            error = "visible monster is not owned by the authoritative creature tracker";
            return false;
        }
        entity.stable_id = "monster-" + std::to_string( scene_entity_ids.id_for( creature_owner ) );
        entity.revision = server_revision;
        entity.position = protocol_position( creature.pos_abs() );
        entity.appearance_id = creature.type->id.str();
        entity.display_name = creature.get_name();
        entity.attitude = protocol_attitude( creature.attitude_to( player ) );
        entity.health_percent = protocol_health_percent( creature );
        result.entities.emplace_back( std::move( entity ) );
    }

    const tripoint_bub_ms center = player.pos_bub();
    for( const tripoint_bub_ms &position : here.points_in_radius( center, radius ) ) {
        if( position.z() != center.z() || rl_dist( center, position ) > radius ||
            !player.sees( here, position, true ) ) {
            continue;
        }
        multiplayer_visible_tile tile;
        tile.position = protocol_position( here.get_abs( position ) );
        tile.terrain_id = here.ter( position ).id().str();
        const std::string furniture = here.furn( position ).id().str();
        if( furniture != "f_null" ) {
            tile.furniture_id = furniture;
        }
        if( here.can_see_trap_at( position, player ) ) {
            const std::string trap = here.tr_at( position ).id.str();
            if( trap != "tr_null" ) {
                tile.visible_trap_id = trap;
            }
        }
        tile.light_level = protocol_light_level( here.ambient_light_at( position ) );
        result.tiles.emplace_back( std::move( tile ) );
    }

    snapshot = std::move( result );
    error.clear();
    return true;
}

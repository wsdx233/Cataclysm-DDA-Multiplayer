#include <algorithm>
#include <string>

#include "avatar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "game.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "mapdata.h"
#include "monster.h"
#include "multiplayer_scene.h"
#include "player_helpers.h"

TEST_CASE( "multiplayer_visible_scene_contains_only_visible_terrain_player_and_monsters",
           "[multiplayer][scene]" )
{
    clear_avatar();
    clear_map();
    on_out_of_scope cleanup( []() {
        clear_avatar();
        clear_map();
    } );
    build_test_map( ter_id( "t_floor" ) );
    set_time_to_day();

    avatar &player = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms start( 60, 60, 0 );
    player.setpos( here, start );
    monster &zombie = spawn_test_monster( "mon_zombie", start + tripoint_rel_ms( 0, 2, 0 ) );
    here.ter_set( start + tripoint_rel_ms( 1, 0, 0 ), ter_id( "t_wall" ) );
    spawn_test_monster( "mon_zombie", start + tripoint_rel_ms( 2, 0, 0 ) );
    player.recalc_sight_limits();
    here.invalidate_visibility_cache();
    here.build_map_cache( 0, true );
    REQUIRE( player.sees( here, zombie ) );

    multiplayer_scene_snapshot snapshot;
    std::string error;
    REQUIRE( multiplayer_build_visible_scene(
                 *g, "12345678-1234-4234-9234-123456789abc", "42", 7, 5,
                 snapshot, error ) );
    CHECK( snapshot.server_revision == 7 );
    CHECK( snapshot.player.character_id == "42" );
    CHECK( snapshot.player.position.x == player.pos_abs().x() );
    CHECK_FALSE( snapshot.tiles.empty() );
    CHECK( std::all_of( snapshot.tiles.begin(), snapshot.tiles.end(),
    [&player]( const multiplayer_visible_tile & tile ) {
        const tripoint_abs_ms absolute( tile.position.x, tile.position.y, tile.position.z );
        return rl_dist( absolute, player.pos_abs() ) <= 5;
    } ) );

    const auto visible_player = std::find_if( snapshot.entities.begin(), snapshot.entities.end(),
    []( const multiplayer_visible_entity & entity ) {
        return entity.kind == multiplayer_visible_entity_kind::player;
    } );
    REQUIRE( visible_player != snapshot.entities.end() );
    CHECK( visible_player->stable_id == "player-42" );
    CHECK( visible_player->appearance_id == "avatar" );

    const auto visible_monster = std::find_if( snapshot.entities.begin(), snapshot.entities.end(),
    []( const multiplayer_visible_entity & entity ) {
        return entity.kind == multiplayer_visible_entity_kind::monster;
    } );
    REQUIRE( visible_monster != snapshot.entities.end() );
    CHECK( visible_monster->appearance_id == "mon_zombie" );
    CHECK( visible_monster->stable_id.find( "monster-" ) == 0 );
    CHECK( visible_monster->health_percent > 0 );
    CHECK( std::count_if( snapshot.entities.begin(), snapshot.entities.end(),
    []( const multiplayer_visible_entity & entity ) {
        return entity.kind == multiplayer_visible_entity_kind::monster;
    } ) == 1 );

    const std::string first_monster_id = visible_monster->stable_id;
    spawn_test_monster( "mon_zombie", start + tripoint_rel_ms( 0, 3, 0 ) );
    here.invalidate_visibility_cache();
    here.build_map_cache( 0, true );
    multiplayer_scene_snapshot later_snapshot;
    REQUIRE( multiplayer_build_visible_scene(
                 *g, "12345678-1234-4234-9234-123456789abc", "42", 8, 5,
                 later_snapshot, error ) );
    CHECK( std::any_of( later_snapshot.entities.begin(), later_snapshot.entities.end(),
    [&first_monster_id]( const multiplayer_visible_entity & entity ) {
        return entity.stable_id == first_monster_id;
    } ) );
    CHECK( std::count_if( later_snapshot.entities.begin(), later_snapshot.entities.end(),
    []( const multiplayer_visible_entity & entity ) {
        return entity.kind == multiplayer_visible_entity_kind::monster;
    } ) == 2 );
}

TEST_CASE( "multiplayer_visible_scene_is_reduced_to_the_mobile_payload_budget",
           "[multiplayer][scene]" )
{
    multiplayer_scene_snapshot snapshot;
    snapshot.server_revision = 1;
    snapshot.player.player_id = "12345678-1234-4234-9234-123456789abc";
    snapshot.player.character_id = "budget-character";
    snapshot.player.revision = snapshot.server_revision;
    snapshot.player.position = { 0, 0, 0 };
    const std::string terrain_id = "t_" + std::string( 125, 'a' );
    for( int y = -30; y <= 30; ++y ) {
        for( int x = -30; x <= 30; ++x ) {
            snapshot.tiles.push_back( { { x, y, 0 }, terrain_id, "", "", 0 } );
        }
    }
    snapshot.entities.push_back( { multiplayer_visible_entity_kind::player,
                                   "player-budget-character", snapshot.server_revision,
                                   snapshot.player.position, "avatar", "Budget Player",
                                   multiplayer_visible_attitude::friendly, 100 } );

    multiplayer_transport_payload payload;
    std::string error;
    REQUIRE( multiplayer_fit_scene_snapshot_to_payload_budget( snapshot, payload, error ) );
    CHECK( payload.size() <= multiplayer_scene_snapshot_maximum_payload_size );
    CHECK( snapshot.tiles.size() < 61 * 61 );
    CHECK( std::all_of( snapshot.tiles.begin(), snapshot.tiles.end(),
    []( const multiplayer_visible_tile & tile ) {
        return std::max( std::abs( tile.position.x ), std::abs( tile.position.y ) ) < 30;
    } ) );
    REQUIRE( snapshot.entities.size() == 1 );
    CHECK( snapshot.entities.front().kind == multiplayer_visible_entity_kind::player );

    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::scene_snapshot;
    envelope.sequence = snapshot.server_revision;
    envelope.payload = std::move( payload );
    multiplayer_scene_snapshot parsed;
    REQUIRE( multiplayer_parse_scene_snapshot_payload( envelope, parsed, error ) );
    CHECK( parsed.tiles.size() == snapshot.tiles.size() );
    REQUIRE( parsed.entities.size() == 1 );
}

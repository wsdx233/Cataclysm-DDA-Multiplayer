#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "character_id.h"
#include "effect.h"
#include "game.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "memory_fast.h"
#include "monattack.h"
#include "monster.h"
#include "multiplayer_player_context.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_player_runtime.h"
#include "options_helpers.h"
#include "player_helpers.h"
#include "point.h"
#include "rng.h"
#include "type_id.h"
#include "weather_type.h"

static const efftype_id effect_monster_locked_on( "monster_locked_on" );
static const efftype_id effect_no_sight( "no_sight" );
static const ter_str_id ter_t_grass( "t_grass" );
static const ter_str_id ter_t_wall( "t_wall" );
static const trait_id trait_DEBUG_CLOAK( "DEBUG_CLOAK" );

namespace
{

class scoped_secondary_player
{
    public:
        explicit scoped_secondary_player( const tripoint_bub_ms &position ) :
            owner_( make_shared_fast<avatar>() ) {
            owner_->create( character_type::NOW );
            clear_character( *owner_ );
            owner_->setID( g->assign_npc_id(), true );
            owner_->name = "monster-target-secondary";
            owner_->setpos( get_map(), position );
            REQUIRE( g->register_multiplayer_player( owner_ ) );
            runtime_ = g->multiplayer_players().find_runtime( *owner_ );
            REQUIRE( runtime_ );
        }

        ~scoped_secondary_player() {
            if( runtime_ == nullptr ) {
                return;
            }
            if( runtime_->status() == multiplayer_player_status::active ) {
                g->disconnect_multiplayer_player( runtime_->player_id() );
            }
            g->unregister_multiplayer_player( *owner_ );
        }

        avatar &player() const {
            return *owner_;
        }

        const shared_ptr_fast<avatar> &owner() const {
            return owner_;
        }

        const shared_ptr_fast<multiplayer_player_runtime> &runtime() const {
            return runtime_;
        }

    private:
        shared_ptr_fast<avatar> owner_;
        shared_ptr_fast<multiplayer_player_runtime> runtime_;
};

struct multiplayer_monster_test_context {
    multiplayer_monster_test_context() :
        restore_turn( calendar::turn ), weather_clear( WEATHER_CLEAR ),
        root( get_avatar() ), original_root_id( root.getID() ) {
        clear_creatures();
        clear_map();
        clear_avatar();
        set_time_to_day();
        root.setID( g->assign_npc_id(), true );
        root.name = "monster-target-root";
        root_runtime = g->multiplayer_players().find_runtime( root );
        REQUIRE( root_runtime );
    }

    ~multiplayer_monster_test_context() {
        clear_creatures();
        clear_avatar();
        root.setID( original_root_id, true );
    }

    void refresh_visibility() const {
        map &here = get_map();
        g->reset_light_level();
        here.invalidate_visibility_cache();
        here.update_visibility_cache( root.posz() );
        here.invalidate_map_cache( root.posz() );
        here.build_map_cache( root.posz() );
        here.invalidate_visibility_cache();
        here.update_visibility_cache( root.posz() );
        here.invalidate_map_cache( root.posz() );
        here.build_map_cache( root.posz() );
        for( const shared_ptr_fast<multiplayer_player_runtime> &runtime :
             g->multiplayer_players().all() ) {
            REQUIRE( runtime );
            runtime->player().recalc_sight_limits();
        }
    }

    void check_root_context() const {
        CHECK( &get_avatar() == &root );
        CHECK( &g->active_avatar() == &root );
        CHECK( &g->active_player_runtime() == root_runtime.get() );
    }

    restore_on_out_of_scope<time_point> restore_turn;
    scoped_weather_override weather_clear;
    avatar &root;
    character_id original_root_id;
    shared_ptr_fast<multiplayer_player_runtime> root_runtime;
};

} // namespace

TEST_CASE( "multiplayer_importing_player_does_not_participate_in_world_rules",
           "[multiplayer][monster_target]" )
{
    shared_ptr_fast<avatar> player = make_shared_fast<avatar>();
    player->create( character_type::NOW );
    clear_character( *player );
    player->setID( g->assign_npc_id(), true );
    player->setpos( get_map(), tripoint_bub_ms( 60, 60, 0 ) );
    const multiplayer_player_runtime::achievement_callback no_achievement_callback;
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        make_shared_fast<multiplayer_player_runtime>( player, no_achievement_callback,
                no_achievement_callback, false );
    multiplayer_player_registry registry;

    REQUIRE( runtime->status() == multiplayer_player_status::importing );
    REQUIRE_FALSE( runtime->is_living_world_avatar() );
    REQUIRE( registry.register_player( runtime ) );
    CHECK( registry.living_world_avatar_count() == 0 );
    CHECK( registry.find_living_world_avatar_at( player->pos_abs() ) == nullptr );
}

TEST_CASE( "multiplayer_monster_planner_uses_all_humans_not_active_context",
           "[multiplayer][monster_target]" )
{
    multiplayer_monster_test_context context;
    map &here = get_map();
    const tripoint_bub_ms monster_position( 60, 60, 0 );

    SECTION( "secondary is the nearer target" ) {
        context.root.setpos( here, tripoint_bub_ms( 55, 60, 0 ) );
        scoped_secondary_player secondary( tripoint_bub_ms( 61, 60, 0 ) );
        monster &zombie = spawn_test_monster( "mon_test_multiplayer_targeter", monster_position,
                                              false );
        context.refresh_visibility();

        REQUIRE( zombie.sees( here, context.root ) );
        REQUIRE( zombie.sees( here, secondary.player() ) );
        zombie.plan();

        CHECK( zombie.get_dest() == secondary.player().pos_abs() );
        CHECK( zombie.attack_target() == &secondary.player() );
        context.check_root_context();
    }

    SECTION( "active-player context does not replace the target population" ) {
        context.root.setpos( here, tripoint_bub_ms( 61, 60, 0 ) );
        scoped_secondary_player secondary( tripoint_bub_ms( 65, 60, 0 ) );
        monster &zombie = spawn_test_monster( "mon_test_multiplayer_targeter", monster_position,
                                              false );
        context.refresh_visibility();

        {
            multiplayer_active_player_guard guard( *g, secondary.owner() );
            REQUIRE( &get_avatar() == &secondary.player() );
            zombie.plan();
            CHECK( zombie.get_dest() == context.root.pos_abs() );
            CHECK( zombie.attack_target() == &context.root );
        }

        context.check_root_context();
    }

    SECTION( "avatar visibility belongs to the target rather than the active root" ) {
        context.root.setpos( here, tripoint_bub_ms( 55, 60, 0 ) );
        context.root.set_mutation( trait_DEBUG_CLOAK );
        here.ter_set( tripoint_bub_ms( 57, 60, 0 ), ter_t_wall );
        scoped_secondary_player secondary( tripoint_bub_ms( 60, 64, 0 ) );
        monster &zombie = spawn_test_monster( "mon_test_multiplayer_targeter", monster_position,
                                              false );
        context.refresh_visibility();

        REQUIRE_FALSE( zombie.sees( here, context.root ) );
        REQUIRE_FALSE( here.sees( context.root.pos_bub(), monster_position, 20 ) );
        REQUIRE( here.sees( secondary.player().pos_bub(), monster_position, 20 ) );
        REQUIRE( zombie.sees( here, secondary.player() ) );
        zombie.plan();

        CHECK( zombie.get_dest() == secondary.player().pos_abs() );
        CHECK( zombie.attack_target() == &secondary.player() );
        context.check_root_context();
    }

    SECTION( "an avatar's own blindness does not hide them from a monster" ) {
        context.root.setpos( here, tripoint_bub_ms( 55, 60, 0 ) );
        context.root.set_mutation( trait_DEBUG_CLOAK );
        scoped_secondary_player secondary( tripoint_bub_ms( 60, 64, 0 ) );
        secondary.player().add_effect( effect_no_sight, 1_minutes );
        monster &zombie = spawn_test_monster( "mon_test_multiplayer_targeter", monster_position,
                                              false );
        context.refresh_visibility();

        REQUIRE( secondary.player().has_effect( effect_no_sight ) );
        REQUIRE( zombie.sees( here, secondary.player() ) );
        zombie.plan();
        CHECK( zombie.get_dest() == secondary.player().pos_abs() );
        CHECK( zombie.attack_target() == &secondary.player() );
    }
}

TEST_CASE( "multiplayer_monster_planner_filters_lifecycle_death_and_visibility",
           "[multiplayer][monster_target]" )
{
    multiplayer_monster_test_context context;
    map &here = get_map();
    const tripoint_bub_ms monster_position( 60, 60, 0 );
    context.root.setpos( here, tripoint_bub_ms( 65, 60, 0 ) );
    scoped_secondary_player secondary( tripoint_bub_ms( 61, 60, 0 ) );
    monster &zombie = spawn_test_monster( "mon_test_multiplayer_targeter", monster_position,
                                          false );

    SECTION( "offline living players remain in world rules" ) {
        REQUIRE( g->disconnect_multiplayer_player( secondary.runtime()->player_id() ) );
        REQUIRE( secondary.runtime()->status() == multiplayer_player_status::offline );
        REQUIRE( secondary.runtime()->is_living_world_avatar() );
        context.refresh_visibility();

        zombie.plan();
        CHECK( zombie.get_dest() == secondary.player().pos_abs() );
        CHECK( zombie.attack_target() == &secondary.player() );
        zombie.set_moves( 1000 );
        CHECK( zombie.attack_at( secondary.player().pos_bub() ) );
    }

    SECTION( "dead runtime is excluded even before physical cleanup" ) {
        REQUIRE( g->mark_multiplayer_player_dead( secondary.runtime()->player_id() ) );
        REQUIRE( secondary.runtime()->status() == multiplayer_player_status::dead );
        REQUIRE_FALSE( secondary.runtime()->is_living_world_avatar() );
        context.refresh_visibility();

        zombie.plan();
        CHECK( zombie.get_dest() == context.root.pos_abs() );
        zombie.set_dest( secondary.player().pos_abs() );
        CHECK( zombie.attack_target() == nullptr );
        CHECK_FALSE( zombie.attack_at( secondary.player().pos_bub() ) );
    }

    SECTION( "avatar death state is excluded before runtime transition" ) {
        secondary.player().set_all_parts_hp_cur( 0 );
        REQUIRE( secondary.runtime()->status() == multiplayer_player_status::active );
        REQUIRE( secondary.player().is_dead_state() );
        REQUIRE_FALSE( secondary.runtime()->is_living_world_avatar() );
        context.refresh_visibility();

        zombie.plan();
        CHECK( zombie.get_dest() == context.root.pos_abs() );
        zombie.set_dest( secondary.player().pos_abs() );
        CHECK( zombie.attack_target() == nullptr );
        CHECK_FALSE( zombie.attack_at( secondary.player().pos_bub() ) );
    }

    SECTION( "hidden secondary does not replace a visible root" ) {
        secondary.player().set_mutation( trait_DEBUG_CLOAK );
        context.refresh_visibility();
        REQUIRE_FALSE( zombie.sees( here, secondary.player() ) );
        REQUIRE( zombie.sees( here, context.root ) );

        zombie.plan();
        CHECK( zombie.get_dest() == context.root.pos_abs() );
        zombie.set_dest( secondary.player().pos_abs() );
        CHECK( zombie.attack_target() == nullptr );
    }

    context.check_root_context();
}

TEST_CASE( "multiplayer_monster_planner_equal_distance_has_no_root_bias",
           "[multiplayer][monster_target]" )
{
    multiplayer_monster_test_context context;
    map &here = get_map();
    // NOLINTNEXTLINE(cata-determinism)
    restore_on_out_of_scope restore_rng( rng_get_engine() );

    const tripoint_bub_ms monster_position( 60, 60, 0 );
    context.root.setpos( here, tripoint_bub_ms( 58, 60, 0 ) );
    scoped_secondary_player secondary( tripoint_bub_ms( 62, 60, 0 ) );
    monster &zombie = spawn_test_monster( "mon_test_multiplayer_targeter", monster_position,
                                          false );
    context.refresh_visibility();
    REQUIRE( zombie.sees( here, context.root ) );
    REQUIRE( zombie.sees( here, secondary.player() ) );

    int root_targets = 0;
    int secondary_targets = 0;
    rng_set_engine_seed( 424242U );
    for( int attempt = 0; attempt < 64; ++attempt ) {
        zombie.unset_dest();
        zombie.plan();
        if( zombie.get_dest() == context.root.pos_abs() ) {
            ++root_targets;
        } else if( zombie.get_dest() == secondary.player().pos_abs() ) {
            ++secondary_targets;
        } else {
            FAIL( "monster selected a non-human destination in an equal two-player setup" );
        }
    }

    CHECK( root_targets > 0 );
    CHECK( secondary_targets > 0 );
    context.check_root_context();
}

TEST_CASE( "multiplayer_monster_lock_on_uses_eligible_visible_humans",
           "[multiplayer][monster_target]" )
{
    multiplayer_monster_test_context context;
    map &here = get_map();
    const tripoint_bub_ms monster_position( 60, 60, 0 );
    context.root.setpos( here, tripoint_bub_ms( 55, 60, 0 ) );
    context.root.set_mutation( trait_DEBUG_CLOAK );
    scoped_secondary_player secondary( tripoint_bub_ms( 64, 60, 0 ) );
    monster &hound = spawn_test_monster( "mon_tindalos", monster_position );
    context.refresh_visibility();
    REQUIRE_FALSE( hound.sees( here, context.root ) );
    REQUIRE( hound.sees( here, secondary.player() ) );
    hound.plan();
    REQUIRE( hound.has_effect( effect_monster_locked_on ) );
    REQUIRE( hound.is_locked_on_to( secondary.player() ) );
    REQUIRE_FALSE( hound.is_locked_on_to( context.root ) );
    const std::optional<character_id> source_id =
        hound.get_effect( effect_monster_locked_on ).get_source().get_character_id();
    REQUIRE( source_id );
    CHECK( *source_id == secondary.player().getID() );

    SECTION( "one avatar lock does not reveal an unrelated hidden avatar" ) {
        context.root.setpos( here, tripoint_bub_ms( 61, 60, 0 ) );
        context.refresh_visibility();
        REQUIRE_FALSE( hound.sees( here, context.root ) );
        REQUIRE( hound.sees( here, secondary.player() ) );
        hound.plan();
        CHECK( hound.get_dest() == secondary.player().pos_abs() );
        CHECK( hound.is_locked_on_to( secondary.player() ) );
        CHECK_FALSE( hound.is_locked_on_to( context.root ) );
    }

    SECTION( "lost direct sight does not refresh a lock but reacquisition does" ) {
        const std::vector<tripoint_bub_ms> blocking_wall = {
            tripoint_bub_ms( 58, 62, 0 ), tripoint_bub_ms( 59, 62, 0 ),
            tripoint_bub_ms( 60, 62, 0 ), tripoint_bub_ms( 61, 62, 0 ),
            tripoint_bub_ms( 62, 62, 0 )
        };
        for( const tripoint_bub_ms &wall : blocking_wall ) {
            here.ter_set( wall, ter_t_wall );
        }
        secondary.player().setpos( here, tripoint_bub_ms( 60, 65, 0 ) );
        hound.get_effect( effect_monster_locked_on ).set_duration( 1_turns );
        context.refresh_visibility();
        REQUIRE_FALSE( hound.sees( here, context.root ) );
        REQUIRE( hound.sees( here, secondary.player() ) );
        REQUIRE_FALSE( here.sees( secondary.player().pos_bub(), hound.pos_bub( here ), 20 ) );
        REQUIRE_FALSE( hound.sees_avatar_position( here, secondary.player() ) );
        hound.plan();
        CHECK( hound.get_effect_dur( effect_monster_locked_on ) == 1_turns );

        for( const tripoint_bub_ms &wall : blocking_wall ) {
            here.ter_set( wall, ter_t_grass );
        }
        secondary.player().setpos( here, tripoint_bub_ms( 60, 66, 0 ) );
        context.refresh_visibility();
        REQUIRE( hound.sees_avatar_position( here, secondary.player() ) );
        hound.plan();
        CHECK( hound.get_effect_dur( effect_monster_locked_on ) == 2_minutes );
    }

    SECTION( "seeing another avatar does not transfer an existing lock" ) {
        context.root.unset_mutation( trait_DEBUG_CLOAK );
        context.root.setpos( here, tripoint_bub_ms( 61, 60, 0 ) );
        hound.get_effect( effect_monster_locked_on ).set_duration( 1_turns );
        context.refresh_visibility();
        REQUIRE( hound.sees( here, context.root ) );
        hound.plan();
        CHECK( hound.get_dest() == context.root.pos_abs() );
        CHECK( hound.get_effect_dur( effect_monster_locked_on ) == 1_turns );
        CHECK( hound.is_locked_on_to( secondary.player() ) );
        CHECK_FALSE( hound.is_locked_on_to( context.root ) );
    }

    SECTION( "tindalos teleport requires the current target's exact lock" ) {
        context.root.unset_mutation( trait_DEBUG_CLOAK );
        context.root.setpos( here, tripoint_bub_ms( 50, 60, 0 ) );
        const tripoint_bub_ms corner( 52, 62, 0 );
        here.ter_set( corner + tripoint::north, ter_t_wall );
        here.ter_set( corner + tripoint::east, ter_t_wall );
        here.ter_set( corner + tripoint::north_east, ter_t_wall );
        context.refresh_visibility();
        REQUIRE( here.is_cornerfloor( corner ) );
        REQUIRE( g->is_empty( corner ) );
        REQUIRE( here.sees( corner, context.root.pos_bub(), 10 ) );
        hound.set_dest( context.root.pos_abs() );
        REQUIRE( hound.attack_target() == &context.root );
        const tripoint_bub_ms old_position = hound.pos_bub( here );

        CHECK( mattack::tindalos_teleport( &hound ) );
        CHECK( hound.pos_bub( here ) == old_position );
        CHECK_FALSE( hound.is_locked_on_to( context.root ) );
    }

    context.check_root_context();
}

TEST_CASE( "multiplayer_monster_source_less_lock_is_single_avatar_only",
           "[multiplayer][monster_target]" )
{
    multiplayer_monster_test_context context;
    map &here = get_map();
    context.root.setpos( here, tripoint_bub_ms( 55, 60, 0 ) );
    monster &hound = spawn_test_monster( "mon_tindalos", tripoint_bub_ms( 60, 60, 0 ) );
    hound.add_effect( effect_monster_locked_on, 1_turns );

    REQUIRE( hound.is_locked_on_to( context.root ) );
    scoped_secondary_player secondary( tripoint_bub_ms( 64, 60, 0 ) );
    CHECK_FALSE( hound.is_locked_on_to( context.root ) );
    CHECK_FALSE( hound.is_locked_on_to( secondary.player() ) );
}

TEST_CASE( "non_living_registry_entries_preserve_single_avatar_monster_rules",
           "[multiplayer][monster_target]" )
{
    multiplayer_monster_test_context context;
    map &here = get_map();
    context.root.setpos( here, tripoint_bub_ms( 55, 60, 0 ) );
    scoped_secondary_player secondary( tripoint_bub_ms( 64, 60, 0 ) );
    REQUIRE( g->mark_multiplayer_player_dead( secondary.runtime()->player_id() ) );
    REQUIRE( secondary.runtime()->status() == multiplayer_player_status::dead );
    REQUIRE( g->multiplayer_players().size() == 2 );
    REQUIRE( g->multiplayer_players().living_world_avatar_count() == 1 );

    monster &hound = spawn_test_monster( "mon_tindalos", tripoint_bub_ms( 60, 60, 0 ) );
    context.refresh_visibility();
    REQUIRE( here.sees( context.root.pos_bub(), hound.pos_bub( here ), 20 ) );
    level_cache &cache = const_cast<level_cache &>( here.get_cache_ref( hound.posz() ) );
    float &observer_seen = cache.seen_cache[hound.posx( here )][hound.posy( here )];
    restore_on_out_of_scope<float> restore_observer_seen( observer_seen );
    observer_seen = 0.0f;
    CHECK_FALSE( hound.sees_avatar_position( here, context.root ) );

    hound.add_effect( effect_monster_locked_on, 1_turns );
    CHECK( hound.is_locked_on_to( context.root ) );
    CHECK_FALSE( hound.is_locked_on_to( secondary.player() ) );
    context.check_root_context();
}

TEST_CASE( "multiplayer_monster_basic_attack_hits_selected_secondary",
           "[multiplayer][monster_target]" )
{
    multiplayer_monster_test_context context;
    map &here = get_map();
    const tripoint_bub_ms monster_position( 60, 60, 0 );
    context.root.setpos( here, tripoint_bub_ms( 55, 60, 0 ) );
    scoped_secondary_player secondary( tripoint_bub_ms( 61, 60, 0 ) );
    monster &zombie = spawn_test_monster( "mon_test_multiplayer_targeter", monster_position,
                                          false );
    context.refresh_visibility();

    zombie.plan();
    REQUIRE( zombie.get_dest() == secondary.player().pos_abs() );
    REQUIRE( zombie.attack_target() == &secondary.player() );

    const int root_hp_before = context.root.get_hp();
    const int secondary_hp_before = secondary.player().get_hp();
    secondary.player().set_dodges_left( 0 );
    zombie.set_moves( 1000 );
    const int moves_before = zombie.get_moves();
    REQUIRE( zombie.attack_at( secondary.player().pos_bub() ) );
    CHECK( zombie.get_moves() < moves_before );
    CHECK( secondary.player().get_hp() < secondary_hp_before );
    CHECK( context.root.get_hp() == root_hp_before );

    secondary.player().set_all_parts_hp_to_max();
    const int move_secondary_hp_before = secondary.player().get_hp();
    secondary.player().set_dodges_left( 0 );
    zombie.set_moves( 1000 );
    zombie.set_dest( secondary.player().pos_abs() );
    zombie.move();
    CHECK( secondary.player().get_hp() < move_secondary_hp_before );
    CHECK( context.root.get_hp() == root_hp_before );
    CHECK_FALSE( secondary.player().is_dead_state() );
    context.check_root_context();
}

TEST_CASE( "single_player_monster_planner_keeps_root_target_and_attack",
           "[multiplayer][monster_target]" )
{
    multiplayer_monster_test_context context;
    map &here = get_map();
    const tripoint_bub_ms monster_position( 60, 60, 0 );
    context.root.setpos( here, tripoint_bub_ms( 61, 60, 0 ) );
    monster &zombie = spawn_test_monster( "mon_test_multiplayer_targeter", monster_position,
                                          false );
    context.refresh_visibility();

    zombie.plan();
    CHECK( zombie.get_dest() == context.root.pos_abs() );
    CHECK( zombie.attack_target() == &context.root );
    zombie.set_moves( 1000 );
    CHECK( zombie.attack_at( context.root.pos_bub() ) );
    context.check_root_context();
}

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "achievement.h"
#include "activity_actor_definitions.h"
#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "character_id.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "enums.h"
#include "event_bus.h"
#include "game.h"
#include "item.h"
#include "item_location.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "map_helpers.h"
#include "memory_fast.h"
#include "messages.h"
#include "multiplayer_player_context.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_player_runtime.h"
#include "npc.h"
#include "player_helpers.h"
#include "pocket_type.h"
#include "skill.h"
#include "stats_tracker.h"
#include "type_id.h"

static const itype_id itype_2x4( "2x4" );
static const itype_id itype_backpack( "backpack" );
static const itype_id itype_jeans( "jeans" );
static const itype_id itype_tshirt( "tshirt" );
static const skill_id skill_survival( "survival" );

static std::string serialize_avatar( const avatar &who )
{
    std::ostringstream out;
    JsonOut json( out );
    who.serialize( json );
    return out.str();
}

static std::string serialize_item_location( const item_location &location )
{
    std::ostringstream out;
    JsonOut json( out );
    location.serialize( json );
    return out.str();
}

static item_location deserialize_item_location( const std::string &serialized )
{
    item_location location;
    JsonValue value = json_loader::from_string( serialized );
    JsonObject object = value;
    location.deserialize( object );
    return location;
}

static bool same_owner( const shared_ptr_fast<avatar> &lhs,
                        const shared_ptr_fast<avatar> &rhs )
{
    return lhs && rhs && !lhs.owner_before( rhs ) && !rhs.owner_before( lhs );
}

static bool item_identity_matches( const item_location &location, const avatar &owner,
                                   const int64_t uid )
{
    return location && location.carrier() == &owner && location->uid().get_value() == uid;
}

static bool is_uuid_v4( const std::string &value )
{
    if( value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
        value[23] != '-' || value[14] != '4' ||
        std::string( "89ab" ).find( value[19] ) == std::string::npos ) {
        return false;
    }
    static const std::string hex = "0123456789abcdef";
    for( std::size_t i = 0; i < value.size(); ++i ) {
        if( i == 8 || i == 13 || i == 18 || i == 23 ) {
            continue;
        }
        if( hex.find( value[i] ) == std::string::npos ) {
            return false;
        }
    }
    return true;
}

static void select_avatar_and_return( game &owner, const shared_ptr_fast<avatar> &next )
{
    multiplayer_active_player_guard guard( owner, next );
}

[[noreturn]] static void select_avatar_and_throw( game &owner,
        const shared_ptr_fast<avatar> &next )
{
    multiplayer_active_player_guard guard( owner, next );
    throw std::runtime_error( "player context exception probe" );
}

static void swap_avatar_values( avatar &lhs, avatar &rhs )
{
    avatar temporary( std::move( lhs ) );
    lhs = std::move( rhs );
    rhs = std::move( temporary );
}

static avatar *find_avatar( avatar &first, avatar &second, const character_id id )
{
    if( first.getID() == id ) {
        return &first;
    }
    if( second.getID() == id ) {
        return &second;
    }
    return nullptr;
}

TEST_CASE( "stable_avatar_context_switch_preserves_player_and_item_identity",
           "[.multiplayer_player_slot][multiplayer][player_bridge][stable_context]" )
{
    avatar &alpha = get_avatar();
    const character_id original_alpha_id = alpha.getID();
    clear_avatar();
    clear_map();

    shared_ptr_fast<avatar> beta_owner = make_shared_fast<avatar>();
    avatar &beta = *beta_owner;
    beta.create( character_type::NOW );
    clear_character( beta );

    map &here = get_map();
    const character_id alpha_id = g->assign_npc_id();
    const character_id beta_id = g->assign_npc_id();
    alpha.setID( alpha_id, true );
    alpha.name = "stable-alpha";
    alpha.setpos( here, tripoint_bub_ms( 60, 60, 0 ) );
    alpha.set_value( "multiplayer_context_probe", "alpha" );
    alpha.assign_activity( wait_activity_actor( time_duration::from_turns( 321 ) ) );

    beta.setID( beta_id, true );
    beta.name = "stable-beta";
    beta.setpos( here, tripoint_bub_ms( 61, 60, 0 ) );
    beta.set_value( "multiplayer_context_probe", "beta" );

    item alpha_pack( itype_backpack );
    alpha_pack.put_in( item( itype_tshirt ), pocket_type::CONTAINER );
    item_location alpha_worn( alpha, & **alpha.wear_item( alpha_pack ) );
    item_location alpha_nested( alpha_worn, &alpha_worn->only_item() );
    item_location alpha_inventory = alpha.i_add( item( itype_jeans ) );
    alpha.set_wielded_item( item( itype_2x4 ) );
    item_location alpha_wielded = alpha.get_wielded_item();

    item beta_pack( itype_backpack );
    beta_pack.put_in( item( itype_jeans ), pocket_type::CONTAINER );
    item_location beta_worn( beta, & **beta.wear_item( beta_pack ) );
    item_location beta_nested( beta_worn, &beta_worn->only_item() );
    item_location beta_inventory = beta.i_add( item( itype_tshirt ) );
    beta.set_wielded_item( item( itype_2x4 ) );
    item_location beta_wielded = beta.get_wielded_item();
    beta.assign_activity( wield_activity_actor( beta_inventory, 1 ) );

    const avatar *const alpha_address = &alpha;
    const avatar *const beta_address = &beta;
    const std::string alpha_before = serialize_avatar( alpha );
    const std::string beta_before = serialize_avatar( beta );
    const std::string alpha_location_json = serialize_item_location( alpha_inventory );
    const std::string beta_location_json = serialize_item_location( beta_inventory );
    const int64_t alpha_worn_uid = alpha_worn->uid().get_value();
    const int64_t alpha_nested_uid = alpha_nested->uid().get_value();
    const int64_t alpha_inventory_uid = alpha_inventory->uid().get_value();
    const int64_t alpha_wielded_uid = alpha_wielded->uid().get_value();
    const int64_t beta_worn_uid = beta_worn->uid().get_value();
    const int64_t beta_nested_uid = beta_nested->uid().get_value();
    const int64_t beta_inventory_uid = beta_inventory->uid().get_value();
    const int64_t beta_wielded_uid = beta_wielded->uid().get_value();
    const shared_ptr_fast<avatar> alpha_owner = g->shared_from<avatar>( alpha );
    REQUIRE( alpha_owner );
    CHECK( same_owner( alpha_owner, g->shared_from<avatar>( alpha ) ) );
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime =
        g->multiplayer_players().find_runtime( alpha );
    REQUIRE( alpha_runtime );
    const safe_mode_type original_alpha_safe_mode = g->get_safe_mode();
    const int original_alpha_most_seen = g->get_most_seen();
    const time_duration original_alpha_turns_since_last_monster =
        g->get_turns_since_last_monster();
    const bool original_alpha_safe_mode_warning = g->is_safe_mode_warning_logged();
    on_out_of_scope restore_alpha_runtime( [ = ]() {
        g->set_safe_mode( original_alpha_safe_mode );
        g->set_most_seen( original_alpha_most_seen );
        g->set_turns_since_last_monster( original_alpha_turns_since_last_monster );
        g->set_safe_mode_warning_logged( original_alpha_safe_mode_warning );
        Messages::clear_messages();
    } );

    const std::size_t initial_player_count = g->multiplayer_players().size();
    REQUIRE( g->register_multiplayer_player( beta_owner ) );
    on_out_of_scope unregister_beta( [&beta]() {
        const shared_ptr_fast<multiplayer_player_runtime> runtime =
            g->multiplayer_players().find_runtime( beta );
        if( runtime != nullptr && runtime->status() == multiplayer_player_status::active ) {
            g->disconnect_multiplayer_player( runtime->player_id() );
        }
        g->unregister_multiplayer_player( beta );
    } );
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime =
        g->multiplayer_players().find_runtime( beta );
    REQUIRE( beta_runtime );
    CHECK( g->multiplayer_players().size() == initial_player_count + 1 );
    CHECK( &g->active_player_runtime() == alpha_runtime.get() );
    CHECK( g->multiplayer_players().owns( alpha_runtime ) );
    CHECK( g->multiplayer_players().owns( beta_runtime ) );
    CHECK( is_uuid_v4( alpha_runtime->player_id().str() ) );
    CHECK( is_uuid_v4( beta_runtime->player_id().str() ) );
    CHECK( alpha_runtime->player_id() != beta_runtime->player_id() );
    CHECK( g->multiplayer_players().find_by_player_id( alpha_runtime->player_id() ) ==
           alpha_runtime );
    CHECK( g->multiplayer_players().find_by_player_id( beta_runtime->player_id() ) ==
           beta_runtime );
    CHECK( alpha_runtime->status() == multiplayer_player_status::active );
    CHECK( beta_runtime->status() == multiplayer_player_status::active );
    CHECK( alpha_runtime->session_generation() == 1 );
    CHECK( beta_runtime->session_generation() == 1 );
    CHECK_FALSE( g->unregister_multiplayer_player( beta ) );
    CHECK( g->disconnect_multiplayer_player( beta_runtime->player_id() ) );
    CHECK( beta_runtime->status() == multiplayer_player_status::offline );
    CHECK_FALSE( g->disconnect_multiplayer_player( beta_runtime->player_id() ) );
    CHECK( g->begin_multiplayer_player_session( beta_runtime->player_id() ) );
    CHECK( beta_runtime->status() == multiplayer_player_status::active );
    CHECK( beta_runtime->session_generation() == 2 );
    CHECK_FALSE( g->begin_multiplayer_player_session( beta_runtime->player_id() ) );
    CHECK_FALSE( g->disconnect_multiplayer_player( alpha_runtime->player_id() ) );
    CHECK_FALSE( g->mark_multiplayer_player_dead( alpha_runtime->player_id() ) );
    CHECK_FALSE( g->register_multiplayer_player( alpha_owner ) );
    CHECK_FALSE( g->register_multiplayer_player( beta_owner ) );
    const shared_ptr_fast<avatar> invalid_id_owner = make_shared_fast<avatar>();
    CHECK_FALSE( g->register_multiplayer_player( invalid_id_owner ) );
    const shared_ptr_fast<avatar> duplicate_id_owner = make_shared_fast<avatar>();
    duplicate_id_owner->setID( beta_id, true );
    CHECK_FALSE( g->register_multiplayer_player( duplicate_id_owner ) );
    const shared_ptr_fast<avatar> counterfeit_beta_owner( &beta, []( avatar * ) {} );
    const shared_ptr_fast<multiplayer_player_runtime> counterfeit_beta_runtime(
    beta_runtime.get(), []( multiplayer_player_runtime * ) {} );
    CHECK_FALSE( g->multiplayer_players().owns( counterfeit_beta_owner ) );
    CHECK_FALSE( g->multiplayer_players().owns( counterfeit_beta_runtime ) );
    CHECK( g->multiplayer_players().owns( beta_owner ) );
    CHECK( g->multiplayer_players().find_by_id( alpha_id ).get() == &alpha );
    CHECK( g->multiplayer_players().find_by_id( beta_id ).get() == &beta );
    CHECK( g->critter_by_id<Character>( beta_id ) == &beta );
    CHECK( get_creature_tracker().creature_at<avatar>( beta.pos_abs() ) == &beta );
    CHECK( same_owner( beta_owner, g->shared_from<avatar>( beta ) ) );

    bool found_alpha_in_all_creatures = false;
    bool found_beta_in_all_creatures = false;
    std::size_t avatar_count = 0;
    for( Creature &candidate : g->all_creatures() ) {
        if( candidate.is_avatar() ) {
            ++avatar_count;
            found_alpha_in_all_creatures = found_alpha_in_all_creatures || &candidate == &alpha;
            found_beta_in_all_creatures = found_beta_in_all_creatures || &candidate == &beta;
        }
    }
    CHECK( found_alpha_in_all_creatures );
    CHECK( found_beta_in_all_creatures );
    CHECK( avatar_count == initial_player_count + 1 );
    CHECK( g->num_creatures() == initial_player_count + 1 );

    bool beta_was_enumerated_as_npc = false;
    for( npc &candidate : g->all_npcs() ) {
        beta_was_enumerated_as_npc = beta_was_enumerated_as_npc ||
                                     static_cast<Creature *>( &candidate ) == &beta;
    }
    CHECK_FALSE( beta.is_npc() );
    CHECK_FALSE( beta_was_enumerated_as_npc );

    const tripoint_abs_ms beta_original_position = beta.pos_abs();
    const tripoint_bub_ms beta_moved_position( 62, 60, 0 );
    beta.setpos( here, beta_moved_position, false );
    CHECK( get_creature_tracker().creature_at<avatar>( beta_original_position ) == nullptr );
    CHECK( get_creature_tracker().creature_at<avatar>( beta_moved_position ) == &beta );
    beta.setpos( beta_original_position, false );
    CHECK( get_creature_tracker().creature_at<avatar>( beta_original_position ) == &beta );

    g->set_safe_mode( SAFE_MODE_ON );
    g->set_most_seen( 7 );
    g->set_turns_since_last_monster( 11_turns );
    g->set_safe_mode_warning_logged( true );
    Messages::clear_messages();
    add_msg( "alpha runtime message" );
    REQUIRE( Messages::size() == 1 );

    const int alpha_damage_events_before =
        alpha_runtime->stats().get_events( event_type::character_takes_damage ).count();
    const int beta_damage_events_before =
        beta_runtime->stats().get_events( event_type::character_takes_damage ).count();
    {
        multiplayer_active_player_guard guard( *g, beta_owner );
        CHECK( &g->active_player_runtime() == beta_runtime.get() );
        CHECK( &g->stats() == &beta_runtime->stats() );
        CHECK( &g->achievements() == &beta_runtime->achievements() );
        CHECK( beta_runtime->messages_are_active() );
        CHECK( Messages::size() == 0 );
        add_msg( "beta runtime message" );
        CHECK( Messages::size() == 1 );
        g->set_safe_mode( SAFE_MODE_OFF );
        g->set_most_seen( 9 );
        g->set_turns_since_last_monster( 13_turns );
        g->set_safe_mode_warning_logged( true );
        get_event_bus().send<event_type::character_takes_damage>(
            beta_id, 3, bodypart_str_id::NULL_ID(), 1 );
        CHECK( beta_runtime->stats().get_events(
                   event_type::character_takes_damage ).count() == beta_damage_events_before + 1 );
    }
    CHECK( &g->active_player_runtime() == alpha_runtime.get() );
    CHECK( &g->stats() == &alpha_runtime->stats() );
    CHECK( &g->achievements() == &alpha_runtime->achievements() );
    CHECK( alpha_runtime->messages_are_active() );
    CHECK( Messages::size() == 1 );
    CHECK( Messages::recent_messages( 1 ).back().second == "alpha runtime message" );
    CHECK( alpha_runtime->stats().get_events(
               event_type::character_takes_damage ).count() == alpha_damage_events_before );
    CHECK( g->get_safe_mode() == SAFE_MODE_ON );
    CHECK( g->get_most_seen() == 7 );
    CHECK( g->get_turns_since_last_monster() == 11_turns );
    CHECK( g->is_safe_mode_warning_logged() );
    {
        multiplayer_active_player_guard guard( *g, beta_owner );
        CHECK( Messages::size() == 1 );
        CHECK( Messages::recent_messages( 1 ).back().second == "beta runtime message" );
        CHECK( g->get_safe_mode() == SAFE_MODE_OFF );
        CHECK( g->get_most_seen() == 9 );
        CHECK( g->get_turns_since_last_monster() == 13_turns );
        CHECK( g->is_safe_mode_warning_logged() );
    }

    bool context_invariants_hold = true;
    std::string first_context_failure;
    const auto record_context_invariant = [&]( const bool holds, const char *label,
    const int iteration, const avatar & selected ) {
        context_invariants_hold = context_invariants_hold && holds;
        if( !holds && first_context_failure.empty() ) {
            first_context_failure = std::string( label ) + " at iteration " +
                                    std::to_string( iteration ) + " for " + selected.name;
        }
    };
    for( int i = 0; i < 10000; ++i ) {
        const shared_ptr_fast<avatar> &selected_owner = i % 2 == 0 ? beta_owner : alpha_owner;
        avatar &selected = *selected_owner;
        {
            multiplayer_active_player_guard guard( *g, selected_owner );
            const shared_ptr_fast<avatar> routed_owner = g->shared_from<avatar>( selected );
            record_context_invariant( &get_avatar() == &selected, "get_avatar", i, selected );
            const multiplayer_player_runtime &selected_runtime =
                &selected == &alpha ? *alpha_runtime : *beta_runtime;
            record_context_invariant( &g->active_player_runtime() == &selected_runtime,
                                      "active runtime", i, selected );
            record_context_invariant( selected_runtime.messages_are_active(),
                                      "message sink", i, selected );
            record_context_invariant( &g->stats() == &selected_runtime.stats(),
                                      "stats tracker", i, selected );
            record_context_invariant( &g->achievements() == &selected_runtime.achievements(),
                                      "achievements tracker", i, selected );
            record_context_invariant( &get_player_character() ==
                                      static_cast<Character *>( &selected ),
                                      "get_player_character", i, selected );
            record_context_invariant(
                g->critter_by_id<Character>( selected.getID() ) == &selected,
                "critter_by_id", i, selected );
            record_context_invariant(
                get_creature_tracker().creature_at<avatar>( selected.pos_abs() ) == &selected,
                "creature_at", i, selected );
            record_context_invariant( routed_owner.get() == &selected &&
                                      same_owner( routed_owner, selected_owner ),
                                      "shared_from", i, selected );
            if( &selected == &alpha ) {
                record_context_invariant( g->get_safe_mode() == SAFE_MODE_ON &&
                                          g->get_most_seen() == 7 &&
                                          g->get_turns_since_last_monster() == 11_turns,
                                          "alpha safe mode", i, selected );
                record_context_invariant( item_identity_matches( alpha_worn, alpha,
                                          alpha_worn_uid ), "alpha worn item", i, selected );
                record_context_invariant( item_identity_matches( alpha_nested, alpha,
                                          alpha_nested_uid ), "alpha nested item", i, selected );
                record_context_invariant(
                    item_identity_matches( alpha_inventory, alpha, alpha_inventory_uid ),
                    "alpha inventory item", i, selected );
                record_context_invariant( item_identity_matches( alpha_wielded, alpha,
                                          alpha_wielded_uid ), "alpha wielded item", i, selected );
            } else {
                record_context_invariant( g->get_safe_mode() == SAFE_MODE_OFF &&
                                          g->get_most_seen() == 9 &&
                                          g->get_turns_since_last_monster() == 13_turns,
                                          "beta safe mode", i, selected );
                record_context_invariant( item_identity_matches( beta_worn, beta,
                                          beta_worn_uid ), "beta worn item", i, selected );
                record_context_invariant( item_identity_matches( beta_nested, beta,
                                          beta_nested_uid ), "beta nested item", i, selected );
                record_context_invariant(
                    item_identity_matches( beta_inventory, beta, beta_inventory_uid ),
                    "beta inventory item", i, selected );
                record_context_invariant( item_identity_matches( beta_wielded, beta,
                                          beta_wielded_uid ), "beta wielded item", i, selected );
            }
        }
        record_context_invariant( &get_avatar() == &alpha, "guard restoration", i, selected );
    }
    INFO( first_context_failure );
    CHECK( context_invariants_hold );

    select_avatar_and_return( *g, beta_owner );
    CHECK( &get_avatar() == &alpha );
    CHECK_THROWS_AS( select_avatar_and_throw( *g, beta_owner ), std::runtime_error );
    CHECK( &get_avatar() == &alpha );

    {
        multiplayer_active_player_guard beta_guard( *g, beta_owner );
        CHECK( &get_avatar() == &beta );
        CHECK( same_owner( beta_owner, g->shared_from<avatar>( beta ) ) );
        CHECK( same_owner( alpha_owner, g->shared_from<avatar>( alpha ) ) );
        CHECK( g->critter_by_id<Character>( alpha_id ) == &alpha );
        CHECK( get_creature_tracker().creature_at<avatar>( alpha.pos_abs() ) == &alpha );
        {
            multiplayer_active_player_guard alpha_guard( *g, alpha_owner );
            CHECK( &get_avatar() == &alpha );
        }
        CHECK( &get_avatar() == &beta );
    }
    CHECK( &get_avatar() == &alpha );

    weak_ptr_fast<avatar> temporary_weak_owner;
    avatar *temporary_player = nullptr;
    character_id temporary_player_id;
    tripoint_abs_ms temporary_player_position;
    {
        shared_ptr_fast<avatar> temporary_owner = make_shared_fast<avatar>();
        temporary_player_id = g->assign_npc_id();
        temporary_owner->setID( temporary_player_id, true );
        temporary_owner->setpos( here, tripoint_bub_ms( 63, 60, 0 ), false );
        temporary_player_position = temporary_owner->pos_abs();
        temporary_player = temporary_owner.get();
        REQUIRE( g->register_multiplayer_player( temporary_owner ) );
        on_out_of_scope unregister_temporary( [&temporary_player]() {
            if( temporary_player != nullptr ) {
                const shared_ptr_fast<multiplayer_player_runtime> runtime =
                    g->multiplayer_players().find_runtime( *temporary_player );
                if( runtime != nullptr &&
                    runtime->status() == multiplayer_player_status::active ) {
                    g->disconnect_multiplayer_player( runtime->player_id() );
                }
                g->unregister_multiplayer_player( *temporary_player );
            }
        } );
        temporary_weak_owner = temporary_owner;
        {
            multiplayer_active_player_guard guard( *g, temporary_owner );
            temporary_owner.reset();
            const shared_ptr_fast<avatar> locked_owner = temporary_weak_owner.lock();
            REQUIRE( locked_owner );
            CHECK( &get_avatar() == locked_owner.get() );
            CHECK( same_owner( locked_owner, g->shared_from<avatar>( *locked_owner ) ) );
        }
        CHECK_FALSE( temporary_weak_owner.expired() );
        const shared_ptr_fast<multiplayer_player_runtime> temporary_runtime =
            g->multiplayer_players().find_runtime( *temporary_player );
        REQUIRE( temporary_runtime );
        CHECK_FALSE( g->unregister_multiplayer_player( *temporary_player ) );
        REQUIRE( g->disconnect_multiplayer_player( temporary_runtime->player_id() ) );
        REQUIRE( g->unregister_multiplayer_player( *temporary_player ) );
        temporary_player = nullptr;
        unregister_temporary.cancel();
    }
    CHECK( temporary_weak_owner.expired() );
    CHECK( g->multiplayer_players().find_by_id( temporary_player_id ) == nullptr );
    CHECK( get_creature_tracker().creature_at<avatar>( temporary_player_position ) == nullptr );
    CHECK( &get_avatar() == &alpha );

    item_location loaded_alpha = deserialize_item_location( alpha_location_json );
    item_location loaded_beta;
    {
        multiplayer_active_player_guard guard( *g, beta_owner );
        loaded_beta = deserialize_item_location( beta_location_json );
        REQUIRE( loaded_beta );
        CHECK( loaded_beta.carrier() == &beta );
    }

    CHECK( &alpha == alpha_address );
    CHECK( &beta == beta_address );
    CHECK( serialize_avatar( alpha ) == alpha_before );
    CHECK( serialize_avatar( beta ) == beta_before );

    REQUIRE( alpha_worn );
    REQUIRE( alpha_nested );
    REQUIRE( alpha_inventory );
    REQUIRE( alpha_wielded );
    REQUIRE( beta_worn );
    REQUIRE( beta_nested );
    REQUIRE( beta_inventory );
    REQUIRE( beta_wielded );
    REQUIRE( loaded_alpha );
    REQUIRE( loaded_beta );
    CHECK( alpha_worn.carrier() == &alpha );
    CHECK( alpha_nested.carrier() == &alpha );
    CHECK( alpha_inventory.carrier() == &alpha );
    CHECK( alpha_wielded.carrier() == &alpha );
    CHECK( beta_worn.carrier() == &beta );
    CHECK( beta_nested.carrier() == &beta );
    CHECK( beta_inventory.carrier() == &beta );
    CHECK( beta_wielded.carrier() == &beta );
    CHECK( loaded_alpha.carrier() == &alpha );
    CHECK( loaded_beta.carrier() == &beta );
    CHECK( g->mark_multiplayer_player_dead( beta_runtime->player_id() ) );
    CHECK( beta_runtime->status() == multiplayer_player_status::dead );
    CHECK_FALSE( g->begin_multiplayer_player_session( beta_runtime->player_id() ) );
    CHECK_FALSE( g->disconnect_multiplayer_player( beta_runtime->player_id() ) );
    CHECK_FALSE( g->mark_multiplayer_player_dead( beta_runtime->player_id() ) );

    clear_avatar();
    alpha.setID( original_alpha_id, true );
}

TEST_CASE( "full_avatar_slots_survive_repeated_move_swap",
           "[.multiplayer_player_slot][multiplayer][player_bridge]" )
{
    avatar &active_slot = get_avatar();
    const character_id original_active_id = active_slot.getID();
    clear_avatar();

    avatar parked_slot;
    parked_slot.create( character_type::NOW );
    clear_character( parked_slot );

    map &here = get_map();
    const character_id alpha_id = g->assign_npc_id();
    const character_id beta_id = g->assign_npc_id();

    active_slot.setID( alpha_id, true );
    active_slot.name = "slot-alpha";
    active_slot.setpos( here, tripoint_bub_ms( 60, 60, 0 ) );
    active_slot.set_str_base( 9 );
    active_slot.set_skill_level( skill_survival, 3 );
    active_slot.set_moves( 137 );
    active_slot.set_value( "multiplayer_slot_probe", "alpha" );
    active_slot.assign_activity( wait_activity_actor( time_duration::from_turns( 321 ) ) );

    parked_slot.setID( beta_id, true );
    parked_slot.name = "slot-beta";
    parked_slot.setpos( here, tripoint_bub_ms( 61, 60, 0 ) );
    parked_slot.set_str_base( 11 );
    parked_slot.set_skill_level( skill_survival, 5 );
    parked_slot.set_moves( 271 );
    parked_slot.set_value( "multiplayer_slot_probe", "beta" );
    parked_slot.assign_activity( wait_activity_actor( time_duration::from_turns( 654 ) ) );

    item alpha_pack( itype_backpack );
    alpha_pack.put_in( item( itype_tshirt ), pocket_type::CONTAINER );
    item_location alpha_worn( active_slot, & **active_slot.wear_item( alpha_pack ) );
    item_location alpha_nested( alpha_worn, &alpha_worn->only_item() );
    item_location alpha_inventory = active_slot.i_add( item( itype_jeans ) );
    active_slot.set_wielded_item( item( itype_2x4 ) );

    item beta_pack( itype_backpack );
    beta_pack.put_in( item( itype_jeans ), pocket_type::CONTAINER );
    item_location beta_worn( parked_slot, & **parked_slot.wear_item( beta_pack ) );
    item_location beta_nested( beta_worn, &beta_worn->only_item() );
    item_location beta_inventory = parked_slot.i_add( item( itype_tshirt ) );
    parked_slot.set_wielded_item( item( itype_2x4 ) );

    const std::string alpha_before = serialize_avatar( active_slot );
    const std::string beta_before = serialize_avatar( parked_slot );
    const int64_t alpha_inventory_uid = alpha_inventory->uid().get_value();
    const int64_t alpha_nested_uid = alpha_nested->uid().get_value();
    const int64_t alpha_wielded_uid = active_slot.get_wielded_item()->uid().get_value();
    const int64_t beta_inventory_uid = beta_inventory->uid().get_value();
    const int64_t beta_nested_uid = beta_nested->uid().get_value();
    const int64_t beta_wielded_uid = parked_slot.get_wielded_item()->uid().get_value();

    for( int i = 0; i < 10000; ++i ) {
        swap_avatar_values( active_slot, parked_slot );
    }

    avatar *alpha_after = find_avatar( active_slot, parked_slot, alpha_id );
    avatar *beta_after = find_avatar( active_slot, parked_slot, beta_id );
    REQUIRE( alpha_after != nullptr );
    REQUIRE( beta_after != nullptr );
    CHECK( serialize_avatar( *alpha_after ) == alpha_before );
    CHECK( serialize_avatar( *beta_after ) == beta_before );

    REQUIRE( alpha_inventory );
    REQUIRE( alpha_nested );
    REQUIRE( beta_inventory );
    REQUIRE( beta_nested );
    REQUIRE( alpha_after->get_wielded_item() );
    REQUIRE( beta_after->get_wielded_item() );
    CHECK( alpha_inventory->uid().get_value() == alpha_inventory_uid );
    CHECK( alpha_nested->uid().get_value() == alpha_nested_uid );
    CHECK( alpha_after->get_wielded_item()->uid().get_value() == alpha_wielded_uid );
    CHECK( beta_inventory->uid().get_value() == beta_inventory_uid );
    CHECK( beta_nested->uid().get_value() == beta_nested_uid );
    CHECK( beta_after->get_wielded_item()->uid().get_value() == beta_wielded_uid );

    clear_avatar();
    active_slot.setID( original_active_id, true );
}

TEST_CASE( "full_avatar_slot_swap_preserves_persistent_wielded_item_location",
           "[.multiplayer_player_slot][multiplayer][player_bridge][!mayfail]" )
{
    avatar &active_slot = get_avatar();
    const character_id original_active_id = active_slot.getID();
    clear_avatar();

    avatar parked_slot;
    parked_slot.create( character_type::NOW );
    clear_character( parked_slot );

    active_slot.setID( g->assign_npc_id(), true );
    parked_slot.setID( g->assign_npc_id(), true );
    active_slot.set_wielded_item( item( itype_2x4 ) );
    item_location wielded = active_slot.get_wielded_item();
    REQUIRE( wielded );
    const int64_t wielded_uid = wielded->uid().get_value();

    for( int i = 0; i < 10000; ++i ) {
        swap_avatar_values( active_slot, parked_slot );
    }

    CHECK( wielded );
    if( wielded ) {
        CHECK( wielded->uid().get_value() == wielded_uid );
    }

    clear_avatar();
    active_slot.setID( original_active_id, true );
}

TEST_CASE( "full_avatar_slot_swap_rebinds_persistent_item_location_owner",
           "[.multiplayer_player_slot][multiplayer][player_bridge][!mayfail]" )
{
    avatar &active_slot = get_avatar();
    const character_id original_active_id = active_slot.getID();
    clear_avatar();

    avatar parked_slot;
    parked_slot.create( character_type::NOW );
    clear_character( parked_slot );

    const character_id alpha_id = g->assign_npc_id();
    const character_id beta_id = g->assign_npc_id();
    active_slot.setID( alpha_id, true );
    parked_slot.setID( beta_id, true );

    REQUIRE( active_slot.wear_item( item( itype_backpack ) ) );
    item_location alpha_item = active_slot.i_add( item( itype_jeans ) );
    REQUIRE( alpha_item );
    swap_avatar_values( active_slot, parked_slot );

    avatar *alpha_after = find_avatar( active_slot, parked_slot, alpha_id );
    CHECK( alpha_after != nullptr );
    CHECK( alpha_item );
    if( alpha_after != nullptr && alpha_item ) {
        CHECK( alpha_item.carrier() == alpha_after );
        CHECK( alpha_item.carrier() != nullptr );
        if( alpha_item.carrier() != nullptr ) {
            CHECK( alpha_item.carrier()->getID() == alpha_id );
        }
    }

    // Restore the global active slot even when the expected bridge invariant fails.
    swap_avatar_values( active_slot, parked_slot );
    clear_avatar();
    active_slot.setID( original_active_id, true );
}

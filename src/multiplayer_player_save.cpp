#include "multiplayer_player_save.h"

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "achievement.h"
#include "avatar.h"
#include "calendar.h"
#include "game.h"
#include "json.h"
#include "json_loader.h"
#include "messages.h"
#include "multiplayer_session_generation.h"
#include "stats_tracker.h"

namespace
{

constexpr int player_snapshot_schema_version = 1;

} // namespace

std::string serialize_multiplayer_player_runtime( const multiplayer_player_runtime &runtime )
{
    if( g != nullptr && !g->is_simulation_thread() ) {
        throw std::logic_error( "player runtime snapshots require the simulation thread" );
    }
    std::ostringstream output;
    JsonOut json( output );
    json.start_object();
    json.member( "server_player_schema", player_snapshot_schema_version );
    json.member( "player_id", runtime.player_id().str() );
    json.member( "session_generation", runtime.session_generation() );
    json.member( "avatar", runtime.player() );
    json.member( "run_mode", static_cast<int>( runtime.safe_mode() ) );
    json.member( "mostseen", runtime.most_seen() );
    json.member( "turnssincelastmon", runtime.turns_since_last_monster() );
    json.member( "safe_mode_warning_logged", runtime.safe_mode_warning_logged() );
    json.member( "stats_tracker", runtime.stats() );
    json.member( "achievements_tracker", runtime.achievements() );
    Messages::serialize( runtime.message_log(), json );
    json.end_object();
    return output.str();
}

multiplayer_player_load_result deserialize_multiplayer_player_runtime(
    const std::string &serialized,
    const multiplayer_player_runtime::achievement_callback &achievement_attained,
    const multiplayer_player_runtime::achievement_callback &achievement_failed )
{
    if( g != nullptr && !g->is_simulation_thread() ) {
        return { nullptr, "player runtime snapshots require the simulation thread" };
    }
    try {
        JsonObject data = json_loader::from_string( serialized );
        data.allow_omitted_members();
        if( data.get_int( "server_player_schema" ) != player_snapshot_schema_version ) {
            return { nullptr, "unsupported server player snapshot schema" };
        }

        const multiplayer_player_id player_id =
            multiplayer_player_id::from_string( data.get_string( "player_id" ) );
        if( !player_id.is_valid() ) {
            return { nullptr, "invalid player_id" };
        }

        std::uint64_t session_generation = 0;
        if( !data.read( "session_generation", session_generation ) ) {
            return { nullptr, "missing session_generation" };
        }
        if( session_generation != 0 &&
            !multiplayer_is_valid_session_generation( session_generation ) ) {
            return { nullptr, "session generation exhausted" };
        }

        const int safe_mode = data.get_int( "run_mode" );
        if( safe_mode < SAFE_MODE_OFF || safe_mode > SAFE_MODE_STOP ) {
            return { nullptr, "invalid safe mode" };
        }
        const int most_seen = data.get_int( "mostseen" );
        if( most_seen < 0 ) {
            return { nullptr, "invalid mostseen" };
        }
        time_duration turns_since_last_monster;
        if( !data.read( "turnssincelastmon", turns_since_last_monster ) ||
            turns_since_last_monster < 0_turns ) {
            return { nullptr, "invalid turnssincelastmon" };
        }
        const bool warning_logged = data.get_bool( "safe_mode_warning_logged" );
        if( !data.has_object( "avatar" ) || !data.has_object( "stats_tracker" ) ||
            !data.has_object( "achievements_tracker" ) ||
            !data.has_object( "player_messages" ) ) {
            return { nullptr, "player snapshot is missing a required object" };
        }

        shared_ptr_fast<avatar> player = make_shared_fast<avatar>();
        data.read( "avatar", *player );
        if( !player->getID().is_valid() ) {
            return { nullptr, "loaded avatar has no valid character_id" };
        }

        shared_ptr_fast<multiplayer_player_runtime> runtime =
            make_shared_fast<multiplayer_player_runtime>( player, achievement_attained,
                    achievement_failed, player_id, session_generation );

        runtime->set_safe_mode( static_cast<safe_mode_type>( safe_mode ) );
        runtime->set_most_seen( most_seen );
        runtime->set_turns_since_last_monster( turns_since_last_monster );
        runtime->set_safe_mode_warning_logged( warning_logged );
        runtime->stats().deserialize_multiplayer_snapshot(
            data.get_object( "stats_tracker" ) );
        data.read( "achievements_tracker", runtime->achievements() );
        Messages::deserialize( runtime->message_log(), data );
        return { std::move( runtime ), std::string() };
    } catch( const JsonError &error ) {
        return { nullptr, error.what() };
    } catch( const std::exception &error ) {
        return { nullptr, error.what() };
    }
}

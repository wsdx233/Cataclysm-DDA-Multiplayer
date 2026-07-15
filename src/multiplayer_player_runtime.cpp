#include "multiplayer_player_runtime.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <random>
#include <utility>

#include "achievement.h"
#include "avatar.h"
#include "calendar.h"
#include "cata_assert.h"
#include "game.h"
#include "messages.h"
#include "multiplayer_session_generation.h"
#include "stats_tracker.h"

namespace
{

std::string random_uuid_v4()
{
    thread_local std::mt19937_64 generator = []() {
        std::random_device entropy;
        std::array<std::random_device::result_type, 8> seed_data = {};
        for( std::random_device::result_type &value : seed_data ) {
            value = entropy();
        }
        std::seed_seq seed( seed_data.begin(), seed_data.end() );
        return std::mt19937_64( seed );
    }
    ();
    std::uniform_int_distribution<unsigned int> byte_distribution( 0, 255 );

    std::array<unsigned char, 16> bytes = {};
    for( unsigned char &value : bytes ) {
        value = static_cast<unsigned char>( byte_distribution( generator ) );
    }
    bytes[6] = static_cast<unsigned char>( ( bytes[6] & 0x0fU ) | 0x40U );
    bytes[8] = static_cast<unsigned char>( ( bytes[8] & 0x3fU ) | 0x80U );

    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve( 36 );
    for( std::size_t i = 0; i < bytes.size(); ++i ) {
        if( i == 4 || i == 6 || i == 8 || i == 10 ) {
            result.push_back( '-' );
        }
        result.push_back( hex[bytes[i] >> 4U] );
        result.push_back( hex[bytes[i] & 0x0fU] );
    }
    return result;
}

bool is_uuid_v4( const std::string &value )
{
    if( value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
        value[23] != '-' || value[14] != '4' ||
        std::string( "89ab" ).find( value[19] ) == std::string::npos ) {
        return false;
    }
    static constexpr char hex[] = "0123456789abcdef";
    for( std::size_t i = 0; i < value.size(); ++i ) {
        if( i == 8 || i == 13 || i == 18 || i == 23 ) {
            continue;
        }
        if( std::find( std::begin( hex ), std::end( hex ) - 1, value[i] ) == std::end( hex ) - 1 ) {
            return false;
        }
    }
    return true;
}

} // namespace

multiplayer_player_id::multiplayer_player_id( std::string value ) :
    value( std::move( value ) )
{
}

multiplayer_player_id multiplayer_player_id::random()
{
    return multiplayer_player_id( random_uuid_v4() );
}

multiplayer_player_id multiplayer_player_id::from_string( std::string value )
{
    return is_uuid_v4( value ) ? multiplayer_player_id( std::move( value ) ) :
           multiplayer_player_id();
}

bool multiplayer_player_id::is_valid() const
{
    return !value.empty();
}

const std::string &multiplayer_player_id::str() const
{
    return value;
}

class multiplayer_player_runtime::impl
{
    public:
        impl( const shared_ptr_fast<avatar> &player,
              const achievement_callback &achievement_attained,
              const achievement_callback &achievement_failed,
              const bool adopt_current_messages,
              const multiplayer_player_id &id,
              const std::uint64_t session_generation ) :
            id( id ),
            session_generation( session_generation ),
            player_owner( player ),
            messages( adopt_current_messages ? Messages::current_message_log() :
                      Messages::make_message_log() ),
            achievements( stats, achievement_attained, achievement_failed, true ) {
        }

        multiplayer_player_id id;
        std::uint64_t session_generation;
        multiplayer_player_status status = multiplayer_player_status::importing;
        shared_ptr_fast<avatar> player_owner;
        shared_ptr_fast<Messages::message_log> messages;
        stats_tracker stats;
        achievements_tracker achievements;
        safe_mode_type safe_mode = SAFE_MODE_ON;
        int most_seen = 0;
        time_duration turns_since_last_monster = 0_turns;
        bool safe_mode_warning_logged = false;
        time_point remote_vehicle_cache_time = calendar::before_time_starts;
        vehicle *remote_vehicle_cache = nullptr;
};

multiplayer_player_runtime::multiplayer_player_runtime(
    const shared_ptr_fast<avatar> &player,
    const achievement_callback &achievement_attained,
    const achievement_callback &achievement_failed,
    const bool adopt_current_messages ) :
    impl_( std::make_unique<impl>( player, achievement_attained, achievement_failed,
                                   adopt_current_messages, multiplayer_player_id::random(), 0 ) )
{
    cata_assert( player != nullptr );
    cata_assert( impl_->messages != nullptr );
}

multiplayer_player_runtime::multiplayer_player_runtime(
    const shared_ptr_fast<avatar> &player,
    const achievement_callback &achievement_attained,
    const achievement_callback &achievement_failed,
    const multiplayer_player_id &player_id,
    const std::uint64_t session_generation ) :
    impl_( std::make_unique<impl>( player, achievement_attained, achievement_failed,
                                   false, player_id, session_generation ) )
{
    cata_assert( player != nullptr );
    cata_assert( impl_->messages != nullptr );
}

multiplayer_player_runtime::~multiplayer_player_runtime() = default;

const multiplayer_player_id &multiplayer_player_runtime::player_id() const
{
    return impl_->id;
}

std::uint64_t multiplayer_player_runtime::session_generation() const
{
    return impl_->session_generation;
}

multiplayer_player_status multiplayer_player_runtime::status() const
{
    return impl_->status;
}

bool multiplayer_player_runtime::begin_session()
{
    if( impl_->status != multiplayer_player_status::importing &&
        impl_->status != multiplayer_player_status::offline ) {
        return false;
    }
    if( impl_->session_generation >= multiplayer_session_generation_exclusive_limit - 1 ) {
        return false;
    }
    return transition_session_generation( impl_->session_generation,
                                          impl_->session_generation + 1 );
}

bool multiplayer_player_runtime::transition_session_generation(
    const std::uint64_t expected_old, const std::uint64_t next_generation )
{
    if( impl_->status != multiplayer_player_status::importing &&
        impl_->status != multiplayer_player_status::offline &&
        impl_->status != multiplayer_player_status::active ) {
        return false;
    }
    if( impl_->session_generation != expected_old ||
        !multiplayer_is_next_session_generation( expected_old, next_generation ) ) {
        return false;
    }
    impl_->session_generation = next_generation;
    impl_->status = multiplayer_player_status::active;
    return true;
}

bool multiplayer_player_runtime::disconnect()
{
    if( impl_->status != multiplayer_player_status::active ) {
        return false;
    }
    impl_->status = multiplayer_player_status::offline;
    return true;
}

bool multiplayer_player_runtime::mark_dead()
{
    if( impl_->status != multiplayer_player_status::active &&
        impl_->status != multiplayer_player_status::offline ) {
        return false;
    }
    impl_->status = multiplayer_player_status::dead;
    return true;
}

avatar &multiplayer_player_runtime::player()
{
    cata_assert( impl_->player_owner != nullptr );
    return *impl_->player_owner;
}

const avatar &multiplayer_player_runtime::player() const
{
    cata_assert( impl_->player_owner != nullptr );
    return *impl_->player_owner;
}

const shared_ptr_fast<avatar> &multiplayer_player_runtime::player_owner() const
{
    return impl_->player_owner;
}

stats_tracker &multiplayer_player_runtime::stats()
{
    return impl_->stats;
}

const stats_tracker &multiplayer_player_runtime::stats() const
{
    return impl_->stats;
}

achievements_tracker &multiplayer_player_runtime::achievements()
{
    return impl_->achievements;
}

const achievements_tracker &multiplayer_player_runtime::achievements() const
{
    return impl_->achievements;
}

void multiplayer_player_runtime::activate_messages() const
{
    Messages::set_current_message_log( impl_->messages );
}

bool multiplayer_player_runtime::messages_are_active() const
{
    return Messages::current_message_log() == impl_->messages;
}

const shared_ptr_fast<Messages::message_log> &multiplayer_player_runtime::message_log() const
{
    return impl_->messages;
}

safe_mode_type multiplayer_player_runtime::safe_mode() const
{
    return impl_->safe_mode;
}

void multiplayer_player_runtime::set_safe_mode( const safe_mode_type value )
{
    impl_->safe_mode = value;
}

int multiplayer_player_runtime::most_seen() const
{
    return impl_->most_seen;
}

void multiplayer_player_runtime::set_most_seen( const int value )
{
    impl_->most_seen = value;
}

time_duration multiplayer_player_runtime::turns_since_last_monster() const
{
    return impl_->turns_since_last_monster;
}

void multiplayer_player_runtime::set_turns_since_last_monster( const time_duration &value )
{
    impl_->turns_since_last_monster = value;
}

bool multiplayer_player_runtime::safe_mode_warning_logged() const
{
    return impl_->safe_mode_warning_logged;
}

void multiplayer_player_runtime::set_safe_mode_warning_logged( const bool value )
{
    impl_->safe_mode_warning_logged = value;
}

bool multiplayer_player_runtime::remote_vehicle_cache_is_current( const time_point &now ) const
{
    return impl_->remote_vehicle_cache_time == now;
}

vehicle *multiplayer_player_runtime::remote_vehicle_cache() const
{
    return impl_->remote_vehicle_cache;
}

void multiplayer_player_runtime::set_remote_vehicle_cache( const time_point &now, vehicle *value )
{
    impl_->remote_vehicle_cache_time = now;
    impl_->remote_vehicle_cache = value;
}

void multiplayer_player_runtime::invalidate_remote_vehicle_cache()
{
    impl_->remote_vehicle_cache_time = calendar::before_time_starts;
    impl_->remote_vehicle_cache = nullptr;
}

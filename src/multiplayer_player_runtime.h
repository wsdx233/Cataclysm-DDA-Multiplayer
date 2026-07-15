#pragma once
#ifndef CATA_SRC_MULTIPLAYER_PLAYER_RUNTIME_H
#define CATA_SRC_MULTIPLAYER_PLAYER_RUNTIME_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "memory_fast.h"

class achievement;
class achievements_tracker;
class avatar;
class stats_tracker;
class time_duration;
class time_point;
class vehicle;
class multiplayer_session_directory;
enum safe_mode_type : int;
namespace Messages
{
class message_log;
} // namespace Messages

/** Stable protocol identity for one human player. */
class multiplayer_player_id
{
    public:
        multiplayer_player_id() = default;

        static multiplayer_player_id random();
        static multiplayer_player_id from_string( std::string value );

        bool is_valid() const;
        const std::string &str() const;

        friend bool operator==( const multiplayer_player_id &lhs,
                                const multiplayer_player_id &rhs ) {
            return lhs.value == rhs.value;
        }
        friend bool operator!=( const multiplayer_player_id &lhs,
                                const multiplayer_player_id &rhs ) {
            return !( lhs == rhs );
        }

    private:
        explicit multiplayer_player_id( std::string value );

        std::string value;
};

enum class multiplayer_player_status : int {
    importing = 0,
    active,
    offline,
    dead
};

/**
 * Address-stable owner for one human avatar and its player-scoped runtime.
 *
 * The legacy local avatar can be adopted through a non-owning shared alias;
 * additional avatars must use a real shared owner supplied by the registry.
 */
class multiplayer_player_runtime
{
    public:
        using achievement_callback = std::function<void( const achievement *, bool )>;

        multiplayer_player_runtime( const shared_ptr_fast<avatar> &player,
                                    const achievement_callback &achievement_attained,
                                    const achievement_callback &achievement_failed,
                                    bool adopt_current_messages );
        multiplayer_player_runtime( const shared_ptr_fast<avatar> &player,
                                    const achievement_callback &achievement_attained,
                                    const achievement_callback &achievement_failed,
                                    const multiplayer_player_id &player_id,
                                    std::uint64_t session_generation );
        ~multiplayer_player_runtime();

        multiplayer_player_runtime( const multiplayer_player_runtime & ) = delete;
        multiplayer_player_runtime &operator=( const multiplayer_player_runtime & ) = delete;
        multiplayer_player_runtime( multiplayer_player_runtime && ) = delete;
        multiplayer_player_runtime &operator=( multiplayer_player_runtime && ) = delete;

        const multiplayer_player_id &player_id() const;
        std::uint64_t session_generation() const;
        multiplayer_player_status status() const;

        avatar &player();
        const avatar &player() const;
        const shared_ptr_fast<avatar> &player_owner() const;

        stats_tracker &stats();
        const stats_tracker &stats() const;
        achievements_tracker &achievements();
        const achievements_tracker &achievements() const;

        void activate_messages() const;
        bool messages_are_active() const;
        const shared_ptr_fast<Messages::message_log> &message_log() const;

        safe_mode_type safe_mode() const;
        void set_safe_mode( safe_mode_type value );
        int most_seen() const;
        void set_most_seen( int value );
        time_duration turns_since_last_monster() const;
        void set_turns_since_last_monster( const time_duration &value );
        bool safe_mode_warning_logged() const;
        void set_safe_mode_warning_logged( bool value );

        bool remote_vehicle_cache_is_current( const time_point &now ) const;
        vehicle *remote_vehicle_cache() const;
        void set_remote_vehicle_cache( const time_point &now, vehicle *value );
        void invalidate_remote_vehicle_cache();

    private:
        bool begin_session();
        bool transition_session_generation( std::uint64_t expected_old,
                                            std::uint64_t next_generation );
        bool transition_offline_at_generation( std::uint64_t expected_generation );
        bool reactivate_session_generation( std::uint64_t expected_generation );
        bool disconnect();
        bool mark_dead();

        class impl;
        std::unique_ptr<impl> impl_;

        friend class multiplayer_player_registry;
        friend class multiplayer_session_directory;
};

#endif // CATA_SRC_MULTIPLAYER_PLAYER_RUNTIME_H

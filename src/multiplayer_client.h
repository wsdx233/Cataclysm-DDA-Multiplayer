#pragma once
#ifndef CATA_SRC_MULTIPLAYER_CLIENT_H
#define CATA_SRC_MULTIPLAYER_CLIENT_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "multiplayer_protocol.h"
#include "multiplayer_transport.h"

enum class multiplayer_client_stage : std::uint8_t {
    stopped,
    connecting,
    awaiting_server_hello,
    awaiting_authentication,
    awaiting_initial_scene,
    ready,
    disconnecting,
    disconnected,
    failed
};

enum class multiplayer_client_event_type : std::uint8_t {
    authenticated,
    resumed,
    scene,
    command_result,
    pong,
    gracefully_disconnected,
    disconnected,
    error
};

struct multiplayer_client_event {
    multiplayer_client_event_type type = multiplayer_client_event_type::error;
    std::optional<multiplayer_scene_snapshot> scene;
    std::optional<multiplayer_command_result> command_result;
    std::optional<multiplayer_protocol_heartbeat> heartbeat;
    std::string message;
    multiplayer_protocol_rejection rejection = multiplayer_protocol_rejection::none;
};

struct multiplayer_client_settings {
    multiplayer_transport_endpoint endpoint;
    multiplayer_protocol_client_kind client_kind =
        multiplayer_protocol_client_kind::graphical_desktop;
    std::string build_id;
    std::string content_manifest;
    std::int32_t savegame_version = 0;
    std::string display_name = "CDDA Multiplayer Client";
    std::string bearer_token;
    std::chrono::milliseconds handshake_timeout = std::chrono::seconds( 10 );
    std::size_t maximum_events = 128;
    std::size_t maximum_pending_commands = 16;
};

class multiplayer_client
{
    public:
        using clock = std::chrono::steady_clock;

        explicit multiplayer_client( multiplayer_client_settings settings );
        ~multiplayer_client();

        multiplayer_client( const multiplayer_client & ) = delete;
        multiplayer_client &operator=( const multiplayer_client & ) = delete;

        bool start( std::string &error );
        bool reconnect( std::string &error );
        bool poll_once( clock::time_point now, std::string &error );
        void stop();

        multiplayer_client_stage stage() const;
        bool ready() const;
        std::optional<multiplayer_client_event> poll_event();
        const std::optional<multiplayer_scene_snapshot> &latest_scene() const;

        bool send_command( multiplayer_command_kind kind,
                           std::optional<multiplayer_protocol_direction> direction,
                           std::uint64_t &sequence, std::string &error );
        bool send_ping( std::uint64_t nonce, std::uint64_t monotonic_milliseconds,
                        std::string &error );
        bool request_resync( std::string reason, std::string &error );
        bool request_graceful_disconnect( std::string &error );
        bool mark_transport_unresponsive( std::string reason, std::string &error );

        const std::string &player_id() const;
        const std::string &character_id() const;
        std::uint64_t session_generation() const;
        std::uint64_t last_confirmed_client_sequence() const;
        std::size_t pending_command_count() const;

    private:
        struct pending_command {
            multiplayer_transport_payload payload;
            std::uint64_t base_revision = 0;
        };

        bool begin_connection( bool resume, std::string &error );
        bool handle_transport_event( multiplayer_transport_event event,
                                     clock::time_point now, std::string &error );
        bool handle_envelope( multiplayer_protocol_envelope envelope,
                              clock::time_point now, std::string &error );
        bool handle_server_hello( const multiplayer_protocol_envelope &envelope,
                                  clock::time_point now, std::string &error );
        bool handle_authentication_result( const multiplayer_protocol_envelope &envelope,
                                           clock::time_point now, std::string &error );
        bool handle_resume_result( const multiplayer_protocol_envelope &envelope,
                                   clock::time_point now, std::string &error );
        bool handle_scene( const multiplayer_protocol_envelope &envelope,
                           std::string &error );
        bool send_envelope( const multiplayer_protocol_envelope &envelope,
                            std::string &error );
        bool send_authentication( bool resume, clock::time_point now, std::string &error );
        bool replay_pending_commands( std::string &error );
        bool fail( std::string message, std::string &error );
        bool push_event( multiplayer_client_event event );
        void clear_authenticated_session();
        void update_resume_sequence_floor();

        multiplayer_client_settings settings_;
        std::unique_ptr<multiplayer_client_transport> transport_;
        multiplayer_transport_payload hello_payload_;
        multiplayer_client_stage stage_ = multiplayer_client_stage::stopped;
        clock::time_point handshake_deadline_ = {};
        bool reconnecting_ = false;
        multiplayer_session_id session_ = {};
        std::string player_id_;
        std::string character_id_;
        std::string resume_token_;
        std::uint64_t session_generation_ = 0;
        std::uint64_t outbound_sequence_ = 0;
        // Highest sequence that can safely be skipped when resuming.  It never
        // advances past the oldest command that still lacks a CommandResult.
        std::uint64_t confirmed_sequence_ = 0;
        std::uint64_t expected_replay_from_sequence_ = 0;
        std::uint64_t minimum_required_scene_revision_ = 0;
        std::uint64_t graceful_disconnect_sequence_ = 0;
        std::map<std::uint64_t, pending_command> pending_commands_;
        std::map<std::uint64_t, std::uint64_t> pending_ping_nonces_;
        std::optional<multiplayer_scene_snapshot> latest_scene_;
        std::deque<multiplayer_client_event> events_;
        std::string terminal_error_;
};

#endif // CATA_SRC_MULTIPLAYER_CLIENT_H

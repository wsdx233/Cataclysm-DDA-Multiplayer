#pragma once
#ifndef CATA_SRC_MULTIPLAYER_SERVER_LOBBY_H
#define CATA_SRC_MULTIPLAYER_SERVER_LOBBY_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "multiplayer_protocol.h"
#include "multiplayer_transport.h"

inline constexpr std::size_t multiplayer_server_command_replay_window = 256;
inline constexpr char multiplayer_graceful_release_transport_reason[] =
    "multiplayer graceful session release completed";

struct multiplayer_server_lobby_settings {
    std::string server_build_id;
    std::string world_id;
    std::string content_manifest;
    std::int32_t savegame_version = 0;
    std::string bearer_token;
    std::optional<std::pair<std::string, std::string>> fixed_player_identity;
    std::vector<multiplayer_protocol_capability> capabilities = {
        { "semantic-scene", 1, true }
    };
    std::size_t maximum_players = 4;
    std::size_t maximum_attempts_per_minute = 10;
    std::size_t maximum_pending_events = 1024;
    std::size_t maximum_application_messages_per_second = 64;
    std::size_t maximum_application_bytes_per_minute = 4 * 1024 * 1024;
    std::chrono::milliseconds handshake_timeout = std::chrono::seconds( 10 );
    std::chrono::seconds resume_token_lifetime = std::chrono::hours( 1 );
};

enum class multiplayer_server_lobby_action_type : std::uint8_t {
    send,
    send_and_disconnect,
    disconnect
};

struct multiplayer_server_lobby_action {
    multiplayer_server_lobby_action_type type = multiplayer_server_lobby_action_type::disconnect;
    multiplayer_connection_id connection = 0;
    multiplayer_transport_payload payload;
    std::string reason;
};

enum class multiplayer_server_lobby_event_type : std::uint8_t {
    authenticated,
    resumed,
    disconnected,
    graceful_disconnect_requested,
    application_message
};

struct multiplayer_server_lobby_event {
    multiplayer_server_lobby_event_type type =
        multiplayer_server_lobby_event_type::application_message;
    multiplayer_connection_id connection = 0;
    multiplayer_session_id session = {};
    std::string player_id;
    std::string character_id;
    std::string display_name;
    std::uint64_t session_generation = 0;
    std::uint64_t last_server_revision = 0;
    std::uint64_t last_client_sequence = 0;
    multiplayer_protocol_envelope message;
};

class multiplayer_server_lobby
{
    public:
        using clock = std::chrono::steady_clock;

        explicit multiplayer_server_lobby( multiplayer_server_lobby_settings settings );

        bool valid( std::string &error ) const;
        std::vector<multiplayer_server_lobby_action> handle_transport_event(
            const multiplayer_transport_event &event, clock::time_point now );
        std::vector<multiplayer_server_lobby_action> complete_graceful_disconnect(
            const multiplayer_server_lobby_event &request );
        std::vector<multiplayer_server_lobby_action> tick( clock::time_point now );
        std::optional<multiplayer_server_lobby_event> poll_event();

        std::size_t connection_count() const;
        std::size_t authenticated_player_count() const;

    private:
        enum class connection_stage : std::uint8_t {
            awaiting_hello,
            awaiting_authentication,
            authenticated,
            draining,
            releasing,
            closing
        };

        struct connection_state {
            connection_stage stage = connection_stage::awaiting_hello;
            std::string peer_address;
            clock::time_point deadline;
            multiplayer_session_id session = {};
            std::string resume_token;
            std::string player_id;
            std::string character_id;
            std::string display_name;
            std::uint64_t session_generation = 0;
            std::uint64_t last_inbound_sequence = 1;
            std::uint64_t resume_replay_high_water = 0;
            std::deque<clock::time_point> application_message_times;
            std::deque<std::pair<clock::time_point, std::size_t>> application_byte_times;
            std::size_t application_bytes_in_window = 0;
        };

        struct resume_record {
            std::string player_id;
            std::string character_id;
            std::string display_name;
            multiplayer_session_id session = {};
            std::uint64_t session_generation = 0;
            std::uint64_t observed_application_high_water = 1;
            std::uint64_t minimum_command_replay_floor = 1;
            std::deque<std::uint64_t> recent_player_command_sequences;
            clock::time_point expires_at;
            std::optional<multiplayer_connection_id> active_connection;
        };

        std::vector<multiplayer_server_lobby_action> handle_frame(
            multiplayer_connection_id connection, connection_state &state,
            const multiplayer_transport_payload &payload, clock::time_point now );
        std::vector<multiplayer_server_lobby_action> handle_client_hello(
            multiplayer_connection_id connection, connection_state &state,
            const multiplayer_protocol_envelope &envelope, clock::time_point now );
        std::vector<multiplayer_server_lobby_action> handle_authenticate(
            multiplayer_connection_id connection, connection_state &state,
            const multiplayer_protocol_envelope &envelope, clock::time_point now );
        std::vector<multiplayer_server_lobby_action> handle_resume(
            multiplayer_connection_id connection, connection_state &state,
            const multiplayer_protocol_envelope &envelope, clock::time_point now );
        std::vector<multiplayer_server_lobby_action> handle_disconnect_notice(
            multiplayer_connection_id connection, connection_state &state,
            const multiplayer_protocol_envelope &envelope );
        void handle_closed_connection( const multiplayer_transport_event &event,
                                       clock::time_point now );
        bool record_authentication_attempt( const std::string &peer_address,
                                            clock::time_point now );
        bool record_application_message( connection_state &state, std::size_t bytes,
                                         clock::time_point now );
        bool build_send_action( multiplayer_connection_id connection,
                                multiplayer_protocol_envelope envelope,
                                multiplayer_server_lobby_action &action,
                                std::string &error ) const;
        bool application_event_capacity_available() const;
        void push_control_event( multiplayer_server_lobby_event event );
        std::vector<multiplayer_server_lobby_action> reject_authentication(
            multiplayer_connection_id connection, connection_state &state,
            multiplayer_protocol_rejection rejection, const std::string &message );
        std::vector<multiplayer_server_lobby_action> reject_resume(
            multiplayer_connection_id connection, connection_state &state,
            multiplayer_protocol_rejection rejection, const std::string &message );

        multiplayer_server_lobby_settings settings_;
        std::map<multiplayer_connection_id, connection_state> connections_;
        std::map<std::string, resume_record, std::less<>> resume_records_;
        std::map<std::string, std::deque<clock::time_point>, std::less<>> authentication_attempts_;
        std::deque<multiplayer_server_lobby_event> events_;
};

#endif // CATA_SRC_MULTIPLAYER_SERVER_LOBBY_H

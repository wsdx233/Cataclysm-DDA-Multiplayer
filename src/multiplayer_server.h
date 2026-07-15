#pragma once
#ifndef CATA_SRC_MULTIPLAYER_SERVER_H
#define CATA_SRC_MULTIPLAYER_SERVER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "multiplayer_content_manifest.h"
#include "multiplayer_server_config.h"
#include "multiplayer_server_lobby.h"
#include "multiplayer_transport.h"

bool multiplayer_server_content_manifest( const multiplayer_server_config &config,
        std::string &manifest, multiplayer_content_manifest_stats &stats,
        std::string &error );

struct multiplayer_server_player_identity {
    std::string player_id;
    std::string character_id;
};

enum class multiplayer_graceful_disconnect_result : std::uint8_t {
    stale_request,
    acknowledgement_queued,
    fallback_close_queue_full,
    fallback_close_frame_too_large,
    fallback_close_response_unavailable,
    server_or_transport_fatal
};

class multiplayer_dedicated_server
{
    public:
        using clock = std::chrono::steady_clock;

        multiplayer_dedicated_server( multiplayer_server_config config,
                                      std::filesystem::path config_path,
                                      std::string build_id,
                                      std::string content_manifest_override = {},
                                      std::optional<multiplayer_server_player_identity>
                                      fixed_player_identity = std::nullopt );
        ~multiplayer_dedicated_server();

        multiplayer_dedicated_server( const multiplayer_dedicated_server & ) = delete;
        multiplayer_dedicated_server &operator=( const multiplayer_dedicated_server & ) = delete;

        bool start( std::string &error );
        bool poll_once( clock::time_point now, std::string &error );
        void stop();
        bool running() const;
        std::uint16_t bound_port() const;
        std::optional<multiplayer_server_lobby_event> poll_event();
        bool send( multiplayer_connection_id connection,
                   const multiplayer_protocol_envelope &envelope, std::string &error );
        // acknowledgement_queued only confirms that the ordered ACK-and-close command entered
        // the transport queue.  The caller must still wait for the exact terminal disconnected
        // lobby event before considering the connection released.  Every fallback_close result
        // means that no ACK was queued and only the terminal close should be recorded.
        multiplayer_graceful_disconnect_result complete_graceful_disconnect(
            const multiplayer_server_lobby_event &request, std::string &error );
        bool admission_is_pending( const multiplayer_server_lobby_event &request ) const;
        bool prepare_admission( const multiplayer_server_lobby_event &request,
                                const multiplayer_server_lobby_admission_decision &decision,
                                multiplayer_server_lobby_prepared_admission &prepared,
                                std::string &error ) const;
        bool publish_prepared_admission( multiplayer_server_lobby_prepared_admission prepared,
                                         bool &published, std::string &error );
        bool record_session_confirmed( const multiplayer_server_lobby_event &event,
                                       std::string &error );
        void disconnect( multiplayer_connection_id connection, std::string reason );
        bool connection_is_closing( multiplayer_connection_id connection ) const;

    private:
        friend struct multiplayer_server_test_support;

        bool execute_actions( std::vector<multiplayer_server_lobby_action> actions,
                              std::string &error );
        multiplayer_graceful_disconnect_result execute_graceful_disconnect_action(
            multiplayer_server_lobby_action action,
            multiplayer_connection_id expected_connection,
            std::string &error );
        bool process_lobby_event( multiplayer_server_lobby_event event,
                                  clock::time_point now, std::string &error );

        multiplayer_server_config config_;
        std::filesystem::path config_path_;
        std::string build_id_;
        std::string content_manifest_override_;
        std::optional<multiplayer_server_player_identity> fixed_player_identity_;
        multiplayer_server_transport transport_;
        std::unique_ptr<multiplayer_server_lobby> lobby_;
        std::deque<multiplayer_server_lobby_event> events_;
        std::set<multiplayer_connection_id> closing_connections_;
        bool running_ = false;
};

#endif // CATA_SRC_MULTIPLAYER_SERVER_H

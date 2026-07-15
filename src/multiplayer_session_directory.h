#pragma once
#ifndef CATA_SRC_MULTIPLAYER_SESSION_DIRECTORY_H
#define CATA_SRC_MULTIPLAYER_SESSION_DIRECTORY_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "multiplayer_protocol.h"
#include "multiplayer_transport.h"

class multiplayer_player_registry;

enum class multiplayer_session_admission_kind : std::uint8_t {
    authentication,
    resume
};

enum class multiplayer_session_directory_status : std::uint8_t {
    success,
    not_simulation_thread,
    invalid_request,
    capacity_full,
    player_not_found,
    identity_mismatch,
    runtime_unavailable,
    already_connected,
    stale_session_generation,
    generation_exhausted,
    conflicting_resume_replay,
    stale_connection,
    duplicate,
    invalid_lifecycle_state
};

const char *multiplayer_session_directory_status_message(
    multiplayer_session_directory_status status );

multiplayer_protocol_rejection multiplayer_session_admission_rejection(
    multiplayer_session_admission_kind kind,
    multiplayer_session_directory_status status );

struct multiplayer_session_admission_request {
    multiplayer_session_admission_kind kind =
        multiplayer_session_admission_kind::authentication;
    std::uint64_t admission_id = 0;
    multiplayer_connection_id connection = 0;
    multiplayer_session_id session = {};
    std::string player_id;
    std::string character_id;
    std::uint64_t expected_session_generation = 0;
    std::uint64_t last_server_revision = 0;
    std::uint64_t last_client_sequence = 0;
};

struct multiplayer_session_admission_plan {
    multiplayer_session_directory_status status =
        multiplayer_session_directory_status::invalid_request;
    multiplayer_session_admission_request request;
    std::uint64_t committed_session_generation = 0;
    bool advances_generation = false;
    bool replays_committed_generation = false;
    std::uint64_t directory_version = 0;
    std::string message;

    explicit operator bool() const {
        return status == multiplayer_session_directory_status::success;
    }
};

struct multiplayer_session_binding {
    multiplayer_connection_id connection = 0;
    multiplayer_session_id session = {};
    std::string player_id;
    std::string character_id;
    std::uint64_t session_generation = 0;

    explicit operator bool() const {
        return connection != 0;
    }

    friend bool operator==( const multiplayer_session_binding &lhs,
                            const multiplayer_session_binding &rhs ) {
        return lhs.connection == rhs.connection && lhs.session == rhs.session &&
               lhs.player_id == rhs.player_id && lhs.character_id == rhs.character_id &&
               lhs.session_generation == rhs.session_generation;
    }

    friend bool operator!=( const multiplayer_session_binding &lhs,
                            const multiplayer_session_binding &rhs ) {
        return !( lhs == rhs );
    }
};

struct multiplayer_session_runtime_key {
    std::string player_id;
    std::string character_id;
    std::uint64_t session_generation = 0;
};

/**
 * Simulation-thread owner for authoritative player/session admission state.
 *
 * The network lobby owns wire validation and token/replay mirrors.  This directory
 * resolves the address-stable registry runtime, plans an admission without mutation,
 * then commits the exact generation and connection tuple before an accepted response
 * may be published.
 */
class multiplayer_session_directory
{
    public:
        explicit multiplayer_session_directory( const multiplayer_player_registry &registry,
                                                std::size_t maximum_players );
        ~multiplayer_session_directory();

        multiplayer_session_directory( const multiplayer_session_directory & ) = delete;
        multiplayer_session_directory &operator=( const multiplayer_session_directory & ) = delete;
        multiplayer_session_directory( multiplayer_session_directory && ) = delete;
        multiplayer_session_directory &operator=( multiplayer_session_directory && ) = delete;

        bool valid( std::string &error ) const;
        multiplayer_session_admission_plan plan_admission(
            const multiplayer_session_admission_request &request ) const;
        bool commit_admission( const multiplayer_session_admission_plan &plan );
        multiplayer_session_directory_status record_session_confirmed(
            const multiplayer_session_binding &binding );
        multiplayer_session_directory_status record_disconnected(
            const multiplayer_session_binding &binding );
        multiplayer_session_directory_status record_graceful_release_pending(
            const multiplayer_session_binding &binding );
        multiplayer_session_directory_status record_runtime_offline(
            const multiplayer_session_runtime_key &key );
        multiplayer_session_directory_status record_admission_unpublished(
            const multiplayer_session_binding &binding );

        std::optional<multiplayer_session_binding> session_for_player(
            const std::string &player_id ) const;
        bool matches_connected( const multiplayer_session_binding &binding ) const;
        std::size_t session_count() const;
        std::size_t connected_count() const;

    private:
        class impl;
        std::unique_ptr<impl> impl_;
};

#endif // CATA_SRC_MULTIPLAYER_SESSION_DIRECTORY_H

#pragma once
#ifndef CATA_SRC_MULTIPLAYER_SINGLE_ROOT_OWNER_H
#define CATA_SRC_MULTIPLAYER_SINGLE_ROOT_OWNER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "multiplayer_selected_root_lifecycle.h"
#include "multiplayer_session_directory.h"
#include "multiplayer_turn_phase_adapter.h"
#include "multiplayer_turn_scheduler.h"

class game;
struct multiplayer_owned_remote_turn_hooks;
struct multiplayer_owned_turn_result;

enum class multiplayer_single_root_owner_status : std::uint8_t {
    applied,
    duplicate,
    rejected,
    deferred,
    not_ready,
    stale,
    invalid,
    faulted
};

enum class multiplayer_single_root_save_disposition : std::uint8_t {
    allowed,
    open_turn,
    canonical_state_uncertain,
    faulted
};

enum class multiplayer_single_root_admission_publish_outcome : std::uint8_t {
    published,
    not_published,
    fatal_after_external_effect
};

struct multiplayer_single_root_admission_publish_receipt {
    multiplayer_single_root_admission_publish_outcome outcome =
        multiplayer_single_root_admission_publish_outcome::fatal_after_external_effect;
    std::uint64_t admission_id = 0;
    multiplayer_session_binding binding;
};

struct multiplayer_single_root_owner_result {
    multiplayer_single_root_owner_status status =
        multiplayer_single_root_owner_status::invalid;
    multiplayer_root_lifecycle_effect next_effect =
        multiplayer_root_lifecycle_effect::none;

    explicit operator bool() const {
        return status == multiplayer_single_root_owner_status::applied ||
               status == multiplayer_single_root_owner_status::duplicate;
    }
};

struct multiplayer_single_root_graceful_completion {
    multiplayer_session_binding binding;
    std::uint64_t request_sequence = 0;
};

class multiplayer_single_root_owner;

/**
 * Immutable directory/lifecycle admission recipe owned by one root owner.
 *
 * Planning does not mutate canonical state.  Once commit_admission() succeeds,
 * the owner locks this exact recipe until complete_admission() records either
 * the published or unpublished transport outcome.
 */
class multiplayer_single_root_admission_plan
{
    public:
        explicit operator bool() const {
            return status_ == multiplayer_single_root_owner_status::applied &&
                   root_plan_.has_value();
        }

        multiplayer_single_root_owner_status status() const {
            return status_;
        }
        const multiplayer_session_admission_plan &directory_plan() const {
            return directory_plan_;
        }
        const multiplayer_root_lifecycle_result &lifecycle_result() const {
            return lifecycle_result_;
        }

    private:
        friend class multiplayer_single_root_owner;

        const multiplayer_single_root_owner *owner_ = nullptr;
        multiplayer_single_root_owner_status status_ =
            multiplayer_single_root_owner_status::invalid;
        multiplayer_session_admission_plan directory_plan_;
        std::optional<multiplayer_root_admission_plan> root_plan_;
        multiplayer_root_lifecycle_result lifecycle_result_;
};

/**
 * Production orchestration boundary for the selected single-player-compatible
 * root.  It owns the authoritative directory, turn scheduler, lifecycle and
 * one per-turn phase adapter.  Socket I/O and protocol encoding remain outside;
 * their exact publish/close outcomes are recorded through narrow methods.
 */
class multiplayer_single_root_owner
{
    public:
        using clock = std::chrono::steady_clock;
        using player_callback = multiplayer_turn_phase_adapter::player_callback;
        using world_callback = multiplayer_turn_phase_adapter::world_callback;
        using admission_publish_callback =
            std::function<multiplayer_single_root_admission_publish_receipt(
                std::uint64_t, const multiplayer_session_binding & )>;

        static std::unique_ptr<multiplayer_single_root_owner> create(
            game &simulation, std::size_t maximum_players,
            clock::duration disconnect_grace, std::string &error );
        ~multiplayer_single_root_owner();

        multiplayer_single_root_owner( const multiplayer_single_root_owner & ) = delete;
        multiplayer_single_root_owner &operator=(
            const multiplayer_single_root_owner & ) = delete;
        multiplayer_single_root_owner( multiplayer_single_root_owner && ) = delete;
        multiplayer_single_root_owner &operator=( multiplayer_single_root_owner && ) = delete;

        bool valid( std::string &error ) const;

        multiplayer_single_root_admission_plan plan_admission(
            const multiplayer_session_admission_request &request ) const;
        multiplayer_single_root_owner_result execute_admission(
            const multiplayer_single_root_admission_plan &plan,
            const admission_publish_callback &publish );
        bool admission_committed() const;

        multiplayer_session_directory_status record_session_confirmed(
            const multiplayer_session_binding &binding );
        std::optional<multiplayer_session_binding> session_for_player(
            const std::string &player_id ) const;
        bool matches_connected( const multiplayer_session_binding &binding ) const;

        multiplayer_single_root_owner_result begin_turn();
        multiplayer_owned_turn_result execute_owned_turn(
            multiplayer_owned_remote_turn_hooks hooks );
        multiplayer_turn_phase_adapter_status execute_current_player(
            const player_callback &callback );
        multiplayer_single_root_owner_result complete_player_phase();
        multiplayer_turn_phase_adapter_status execute_world(
            const world_callback &callback );
        multiplayer_single_root_owner_result complete_turn_after_player_end(
            const multiplayer_owned_turn_result &turn_result );

        multiplayer_single_root_owner_result record_graceful_departure(
            const multiplayer_session_binding &binding,
            std::uint64_t request_sequence, clock::time_point now );
        multiplayer_single_root_owner_result record_transport_disconnected(
            const multiplayer_session_binding &binding, clock::time_point now );
        multiplayer_single_root_owner_result progress_departure( clock::time_point now );

        std::optional<multiplayer_single_root_graceful_completion>
        pending_graceful_completion() const;
        multiplayer_single_root_owner_result record_graceful_completion_queued(
            const multiplayer_single_root_graceful_completion &completion );

        multiplayer_single_root_owner_result latch_fault(
            bool canonical_state_uncertain );

        multiplayer_selected_root_lifecycle_snapshot snapshot() const;
        std::optional<multiplayer_turn_slot> current_slot() const;
        bool can_begin_turn() const;
        bool can_execute_command() const;
        bool is_dormant() const;
        bool is_faulted() const;
        multiplayer_single_root_save_disposition save_disposition() const;

    private:
        multiplayer_single_root_owner( game &simulation, std::size_t maximum_players,
                                       clock::duration disconnect_grace );

        struct pending_admission {
            multiplayer_single_root_admission_plan plan;
        };

        multiplayer_single_root_owner_result result(
            multiplayer_single_root_owner_status status,
            multiplayer_root_lifecycle_effect effect =
                multiplayer_root_lifecycle_effect::none ) const;
        bool same_admission( const multiplayer_single_root_admission_plan &lhs,
                             const multiplayer_single_root_admission_plan &rhs ) const;
        multiplayer_single_root_owner_result commit_admission(
            const multiplayer_single_root_admission_plan &plan );
        multiplayer_single_root_owner_result complete_admission(
            const multiplayer_single_root_admission_plan &plan, bool published );
        bool lifecycle_applied_or_duplicate(
            const multiplayer_root_lifecycle_result &value ) const;
        bool apply_admission_scheduler_effect(
            const multiplayer_root_admission_plan &plan,
            multiplayer_root_lifecycle_effect effect );
        multiplayer_single_root_owner_result apply_departure_effect(
            multiplayer_root_lifecycle_effect effect );
        multiplayer_single_root_owner_result transition_runtime_offline();
        multiplayer_single_root_owner_result fault_after_external_change(
            bool canonical_state_uncertain = false );
        void note_adapter_fault();

        game &simulation_;
        multiplayer_session_directory directory_;
        multiplayer_turn_scheduler scheduler_;
        multiplayer_selected_root_lifecycle lifecycle_;
        clock::duration disconnect_grace_;
        std::unique_ptr<multiplayer_turn_phase_adapter> adapter_;
        std::optional<pending_admission> pending_admission_;
        std::optional<multiplayer_session_binding> turn_binding_;
        std::optional<multiplayer_turn_participant_key> turn_participant_;
        std::optional<multiplayer_world_ticket> claimed_world_ticket_;
        std::uint64_t next_shared_turn_ = 1;
        std::uint64_t next_turn_invocation_ = 1;
        std::uint64_t current_turn_invocation_ = 0;
        bool turn_invocation_started_ = false;
        bool canonical_state_uncertain_ = false;
        bool fault_save_allowed_ = false;
        bool fault_open_turn_ = false;
        bool valid_ = false;
        std::string invalid_reason_;
};

#endif // CATA_SRC_MULTIPLAYER_SINGLE_ROOT_OWNER_H

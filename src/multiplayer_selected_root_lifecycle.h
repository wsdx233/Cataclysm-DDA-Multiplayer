#pragma once
#ifndef CATA_SRC_MULTIPLAYER_SELECTED_ROOT_LIFECYCLE_H
#define CATA_SRC_MULTIPLAYER_SELECTED_ROOT_LIFECYCLE_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "multiplayer_player_runtime.h"
#include "multiplayer_session_directory.h"
#include "multiplayer_turn_scheduler.h"

enum class multiplayer_root_connection_state : std::uint8_t {
    unbound,
    connected,
    graceful_release_pending,
    disconnected
};

enum class multiplayer_root_barrier_state : std::uint8_t {
    none,
    awaiting_command,
    disconnected_grace,
    forced_wait_pending,
    terminal,
    world_processing,
    world_completed
};

enum class multiplayer_root_server_state : std::uint8_t {
    running,
    dormant,
    faulted
};

enum class multiplayer_root_departure_kind : std::uint8_t {
    none,
    transport_loss,
    graceful_release
};

enum class multiplayer_root_lifecycle_status : std::uint8_t {
    applied,
    duplicate,
    deadline_not_reached,
    not_current_slot,
    resume_deferred_until_boundary,
    stale_binding,
    stale_generation,
    invalid_transition,
    external_state_mismatch,
    faulted
};

enum class multiplayer_root_lifecycle_effect : std::uint8_t {
    none,
    mark_barrier_disconnected,
    wait_for_disconnect_deadline,
    request_automatic_wait,
    execute_authoritative_wait,
    claim_world,
    execute_claimed_world,
    transition_runtime_offline,
    complete_graceful_release,
    resume_barrier_plus_one,
    rebind_replayed_barrier_generation,
    repair_disconnected_barrier_generation,
    begin_next_turn,
    fatal_shutdown
};

struct multiplayer_root_lifecycle_result {
    multiplayer_root_lifecycle_status status =
        multiplayer_root_lifecycle_status::invalid_transition;
    multiplayer_root_lifecycle_effect next_effect =
        multiplayer_root_lifecycle_effect::none;

    explicit operator bool() const {
        return status == multiplayer_root_lifecycle_status::applied;
    }
};

class multiplayer_selected_root_lifecycle;

struct multiplayer_root_departure_ticket {
        // Issued/renewed by the lifecycle.  After an unpublished +1 repair the
        // binding remains the departed transport tuple while participant carries
        // the canonical barrier generation.  Call runtime_key() for offline work.
        std::uint64_t lifecycle_version = 0;
        std::uint64_t departure_epoch = 0;
        multiplayer_session_binding binding;
        multiplayer_turn_participant_key participant;
        std::chrono::steady_clock::time_point deadline;

        friend bool operator==( const multiplayer_root_departure_ticket &lhs,
                                const multiplayer_root_departure_ticket &rhs ) {
            return lhs.lifecycle_version == rhs.lifecycle_version &&
                   lhs.departure_epoch == rhs.departure_epoch &&
                   lhs.binding == rhs.binding && lhs.participant == rhs.participant &&
                   lhs.deadline == rhs.deadline && lhs.owner_ == rhs.owner_;
        }

        friend bool operator!=( const multiplayer_root_departure_ticket &lhs,
                                const multiplayer_root_departure_ticket &rhs ) {
            return !( lhs == rhs );
        }

    private:
        friend class multiplayer_selected_root_lifecycle;

        const multiplayer_selected_root_lifecycle *owner_ = nullptr;
};

enum class multiplayer_root_admission_origin : std::uint8_t {
    unbound_active,
    disconnected_grace,
    dormant
};

enum class multiplayer_root_barrier_resume_mode : std::uint8_t {
    no_open_barrier,
    advance_one_generation,
    replay_same_generation
};

/**
 * Immutable owner/version-bound admission recipe.
 *
 * After directory commit, a published path applies published_effect() before
 * record_admission_published().  An enqueue-failure path asks the directory to
 * apply its commit-owned cleanup and applies unpublished_effect() before
 * record_admission_unpublished().  Any failed external transition is fail-stop.
 */
class multiplayer_root_admission_plan
{
    public:
        multiplayer_root_admission_plan( const multiplayer_root_admission_plan & ) = default;
        multiplayer_root_admission_plan &operator=(
            const multiplayer_root_admission_plan & ) = default;
        multiplayer_root_admission_plan( multiplayer_root_admission_plan && ) = default;
        multiplayer_root_admission_plan &operator=(
            multiplayer_root_admission_plan && ) = default;

        explicit operator bool() const {
            return static_cast<bool>( result_ );
        }

        const multiplayer_root_lifecycle_result &result() const {
            return result_;
        }
        /** Scheduler transition that must succeed before published completion is recorded. */
        multiplayer_root_lifecycle_effect published_effect() const {
            return result_.next_effect;
        }
        multiplayer_root_admission_origin origin() const {
            return origin_;
        }
        multiplayer_root_barrier_resume_mode barrier_mode() const {
            return barrier_mode_;
        }
        const multiplayer_turn_participant_key &old_participant() const {
            return old_participant_;
        }
        const multiplayer_turn_participant_key &committed_participant() const {
            return committed_participant_;
        }
        const multiplayer_session_binding &binding() const {
            return binding_;
        }
        std::uint64_t admission_id() const {
            return admission_id_;
        }
        /** Scheduler transition that must succeed before unpublished completion is recorded. */
        multiplayer_root_lifecycle_effect unpublished_effect() const {
            return unpublished_effect_;
        }

    private:
        friend class multiplayer_selected_root_lifecycle;

        multiplayer_root_admission_plan() = default;

        const multiplayer_selected_root_lifecycle *owner_ = nullptr;
        multiplayer_root_lifecycle_result result_;
        std::uint64_t lifecycle_version_ = 0;
        multiplayer_root_admission_origin origin_ =
            multiplayer_root_admission_origin::unbound_active;
        multiplayer_root_barrier_resume_mode barrier_mode_ =
            multiplayer_root_barrier_resume_mode::no_open_barrier;
        multiplayer_turn_participant_key old_participant_;
        multiplayer_turn_participant_key committed_participant_;
        multiplayer_session_binding binding_;
        std::uint64_t admission_id_ = 0;
        multiplayer_session_admission_kind admission_kind_ =
            multiplayer_session_admission_kind::authentication;
        std::uint64_t expected_generation_ = 0;
        std::uint64_t directory_version_ = 0;
        bool advances_generation_ = false;
        bool replays_committed_generation_ = false;
        multiplayer_root_lifecycle_effect unpublished_effect_ =
            multiplayer_root_lifecycle_effect::none;
};

struct multiplayer_selected_root_lifecycle_snapshot {
    multiplayer_player_id player_id;
    std::string character_id;
    std::uint64_t session_generation = 0;
    std::optional<multiplayer_session_binding> binding;
    std::optional<multiplayer_session_binding> departed_binding;
    std::optional<multiplayer_turn_participant_key> participant;
    std::optional<multiplayer_world_ticket> world_ticket;
    multiplayer_root_connection_state connection =
        multiplayer_root_connection_state::unbound;
    multiplayer_root_barrier_state barrier = multiplayer_root_barrier_state::none;
    multiplayer_player_status verified_runtime_status = multiplayer_player_status::importing;
    multiplayer_root_server_state server = multiplayer_root_server_state::faulted;
    multiplayer_root_departure_kind departure_kind =
        multiplayer_root_departure_kind::none;
    std::uint64_t lifecycle_version = 0;
    std::uint64_t departure_epoch = 0;
    std::uint64_t shared_turn = 0;
    std::optional<std::chrono::steady_clock::time_point> disconnect_deadline;
    std::optional<std::uint64_t> graceful_request_sequence;
    bool forced_wait_requested = false;
    bool graceful_completion_queued = false;
};

/**
 * Pure cross-component contract for the selected single-player-compatible root.
 *
 * This object owns no socket, runtime, scheduler, callback, or game object.  A
 * caller records a transition only after the authoritative external component
 * has completed the corresponding exact operation.  The object then exposes the
 * next required orchestration effect and rejects stale or duplicate work.  If
 * an external operation succeeded but its record call is not applied/duplicate,
 * the owner must immediately call latch_fault(); probe/rejection calls that did
 * not mutate external state need not fault the lifecycle.
 */
class multiplayer_selected_root_lifecycle
{
    public:
        using clock = std::chrono::steady_clock;

        multiplayer_selected_root_lifecycle( const multiplayer_player_id &player_id,
                                             std::string character_id,
                                             std::uint64_t session_generation,
                                             multiplayer_player_status runtime_status );

        multiplayer_selected_root_lifecycle(
            const multiplayer_selected_root_lifecycle & ) = delete;
        multiplayer_selected_root_lifecycle &operator=(
            const multiplayer_selected_root_lifecycle & ) = delete;
        multiplayer_selected_root_lifecycle(
            multiplayer_selected_root_lifecycle && ) = delete;
        multiplayer_selected_root_lifecycle &operator=(
            multiplayer_selected_root_lifecycle && ) = delete;

        multiplayer_root_lifecycle_result begin_turn(
            const multiplayer_session_binding &binding,
            const multiplayer_turn_participant_key &participant,
            std::uint64_t shared_turn );
        multiplayer_root_lifecycle_result record_connected_barrier_terminal(
            const multiplayer_session_binding &binding,
            const multiplayer_turn_participant_key &participant,
            std::uint64_t shared_turn );
        multiplayer_root_lifecycle_result record_connected_world_claimed(
            const multiplayer_session_binding &binding,
            const multiplayer_turn_participant_key &participant,
            const multiplayer_world_ticket &world_ticket );
        multiplayer_root_lifecycle_result record_connected_world_completed(
            const multiplayer_session_binding &binding,
            const multiplayer_turn_participant_key &participant,
            const multiplayer_world_ticket &world_ticket );

        multiplayer_root_lifecycle_result record_departure_accepted(
            const multiplayer_session_binding &binding,
            multiplayer_root_departure_kind kind,
            clock::time_point now,
            clock::duration disconnect_grace,
            std::optional<std::uint64_t> graceful_request_sequence = std::nullopt );
        multiplayer_root_lifecycle_result record_barrier_disconnected(
            const multiplayer_root_departure_ticket &ticket );
        multiplayer_root_lifecycle_result evaluate_disconnect_timeout(
            const multiplayer_root_departure_ticket &ticket,
            clock::time_point now,
            const std::optional<multiplayer_turn_participant_key> &current_slot );
        multiplayer_root_lifecycle_result record_forced_wait_pending(
            const multiplayer_root_departure_ticket &ticket );
        multiplayer_root_lifecycle_result record_forced_wait_terminal(
            const multiplayer_root_departure_ticket &ticket );
        multiplayer_root_lifecycle_result record_world_claimed(
            const multiplayer_root_departure_ticket &ticket,
            const multiplayer_world_ticket &world_ticket );
        multiplayer_root_lifecycle_result record_world_completed(
            const multiplayer_root_departure_ticket &ticket,
            const multiplayer_world_ticket &world_ticket );
        multiplayer_root_lifecycle_result record_runtime_offline(
            const multiplayer_root_departure_ticket &ticket );
        multiplayer_root_lifecycle_result record_graceful_completion_queued(
            const multiplayer_session_binding &binding,
            std::uint64_t request_sequence );
        /** Records ordered close or an earlier peer/write failure; early close cancels the ACK. */
        multiplayer_root_lifecycle_result record_graceful_transport_closed(
            const multiplayer_session_binding &binding );

        multiplayer_root_admission_plan plan_admission(
            const multiplayer_session_admission_plan &directory_plan ) const;
        multiplayer_root_lifecycle_result record_admission_published(
            const multiplayer_root_admission_plan &plan );
        multiplayer_root_lifecycle_result record_admission_unpublished(
            const multiplayer_root_admission_plan &plan );

        multiplayer_root_lifecycle_result latch_fault();

        multiplayer_selected_root_lifecycle_snapshot snapshot() const;
        multiplayer_session_runtime_key runtime_key() const;
        std::optional<multiplayer_root_departure_ticket> departure_ticket() const;

        bool can_begin_turn() const;
        bool can_execute_command() const;
        bool can_create_guard() const;
        bool can_save_or_shutdown() const;

    private:
        struct admission_outcome {
            std::uint64_t admission_id = 0;
            multiplayer_session_binding binding;
            std::uint64_t committed_generation = 0;
            bool published = false;
        };

        multiplayer_root_lifecycle_result result(
            multiplayer_root_lifecycle_status status,
            multiplayer_root_lifecycle_effect effect =
                multiplayer_root_lifecycle_effect::none ) const;
        multiplayer_root_lifecycle_result record_departure(
            const multiplayer_session_binding &binding,
            multiplayer_root_departure_kind kind,
            clock::time_point now,
            clock::duration disconnect_grace,
            std::optional<std::uint64_t> graceful_request_sequence );
        bool ticket_matches_current(
            const multiplayer_root_departure_ticket &ticket ) const;
        bool ticket_matches_departure(
            const multiplayer_root_departure_ticket &ticket ) const;
        bool directory_plan_is_well_formed(
            const multiplayer_session_admission_plan &plan ) const;
        bool plan_matches_current_origin(
            const multiplayer_root_admission_plan &plan ) const;
        bool admission_outcome_matches(
            const multiplayer_root_admission_plan &plan, bool published ) const;
        void clear_departure_for_connected_session();
        void refresh_departure_ticket();
        void advance_version();

        multiplayer_player_id player_id_;
        std::string character_id_;
        std::uint64_t session_generation_ = 0;
        std::optional<multiplayer_session_binding> binding_;
        std::optional<multiplayer_session_binding> departed_binding_;
        std::optional<multiplayer_turn_participant_key> participant_;
        std::optional<multiplayer_root_departure_ticket> exact_departure_ticket_;
        std::optional<multiplayer_world_ticket> world_ticket_;
        std::optional<multiplayer_world_ticket> last_completed_world_ticket_;
        multiplayer_root_connection_state connection_ =
            multiplayer_root_connection_state::unbound;
        multiplayer_root_barrier_state barrier_ = multiplayer_root_barrier_state::none;
        multiplayer_player_status verified_runtime_status_ = multiplayer_player_status::importing;
        multiplayer_root_server_state server_ = multiplayer_root_server_state::faulted;
        multiplayer_root_departure_kind departure_kind_ =
            multiplayer_root_departure_kind::none;
        std::uint64_t lifecycle_version_ = 1;
        std::uint64_t departure_epoch_ = 0;
        std::uint64_t departure_lifecycle_version_ = 0;
        std::uint64_t shared_turn_ = 0;
        std::uint64_t last_started_shared_turn_ = 0;
        std::optional<clock::time_point> disconnect_deadline_;
        std::optional<std::uint64_t> graceful_request_sequence_;
        bool forced_wait_requested_ = false;
        bool graceful_completion_queued_ = false;
        std::optional<admission_outcome> last_admission_outcome_;
};

#endif // CATA_SRC_MULTIPLAYER_SELECTED_ROOT_LIFECYCLE_H

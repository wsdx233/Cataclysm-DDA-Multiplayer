#pragma once
#ifndef CATA_SRC_MULTIPLAYER_TURN_SCHEDULER_H
#define CATA_SRC_MULTIPLAYER_TURN_SCHEDULER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "multiplayer_player_runtime.h"

struct multiplayer_turn_participant_key {
    multiplayer_player_id player_id;
    std::uint64_t session_generation = 0;

    friend bool operator==( const multiplayer_turn_participant_key &lhs,
                            const multiplayer_turn_participant_key &rhs ) {
        return lhs.player_id == rhs.player_id &&
               lhs.session_generation == rhs.session_generation;
    }

    friend bool operator!=( const multiplayer_turn_participant_key &lhs,
                            const multiplayer_turn_participant_key &rhs ) {
        return !( lhs == rhs );
    }
};

struct multiplayer_turn_ordering_key {
    std::uint64_t shared_turn = 0;
    std::uint64_t round = 0;
    std::uint32_t slot = 0;
    multiplayer_player_id player_id;

    friend bool operator==( const multiplayer_turn_ordering_key &lhs,
                            const multiplayer_turn_ordering_key &rhs ) {
        return lhs.shared_turn == rhs.shared_turn && lhs.round == rhs.round &&
               lhs.slot == rhs.slot && lhs.player_id == rhs.player_id;
    }
};

struct multiplayer_world_ticket {
    std::uint64_t shared_turn = 0;

    friend bool operator==( const multiplayer_world_ticket &lhs,
                            const multiplayer_world_ticket &rhs ) {
        return lhs.shared_turn == rhs.shared_turn;
    }
};

enum class multiplayer_turn_scheduler_stage : std::uint8_t {
    idle,
    player_actions,
    world_ready,
    world_processing
};

enum class multiplayer_turn_participant_state : std::uint8_t {
    awaiting_command,
    disconnected_grace,
    automatic_wait_pending,
    finished,
    removed
};

enum class multiplayer_turn_action_disposition : std::uint8_t {
    accepted_remains_eligible,
    accepted_finished,
    rejected,
    duplicate
};

enum class multiplayer_disconnect_timeout_policy : std::uint8_t {
    keep_waiting,
    automatic_wait,
    remove_from_barrier
};

struct multiplayer_turn_slot {
    multiplayer_turn_participant_key participant;
    multiplayer_turn_ordering_key ordering;
    multiplayer_turn_participant_state state =
        multiplayer_turn_participant_state::awaiting_command;

    friend bool operator==( const multiplayer_turn_slot &lhs,
                            const multiplayer_turn_slot &rhs ) {
        return lhs.participant == rhs.participant && lhs.ordering == rhs.ordering &&
               lhs.state == rhs.state;
    }
};

/**
 * Pure shared-turn barrier policy.
 *
 * The scheduler owns no live simulation objects, sockets, or command payloads.
 * A participant's stable roster identity is player_id; session generation is
 * mutable metadata validated at every barrier transition.
 */
class multiplayer_turn_scheduler
{
    public:
        static constexpr std::size_t maximum_participants = 4;

        multiplayer_turn_scheduler() = default;
        multiplayer_turn_scheduler( const multiplayer_turn_scheduler & ) = delete;
        multiplayer_turn_scheduler &operator=( const multiplayer_turn_scheduler & ) = delete;
        multiplayer_turn_scheduler( multiplayer_turn_scheduler && ) = delete;
        multiplayer_turn_scheduler &operator=( multiplayer_turn_scheduler && ) = delete;

        bool begin_turn( std::uint64_t shared_turn,
                         const std::vector<multiplayer_turn_participant_key> &roster );

        multiplayer_turn_scheduler_stage stage() const;
        /** Permanently blocks transitions after an uncertain simulation side effect. */
        bool is_faulted() const noexcept;
        void latch_execution_fault() noexcept;
        /** Includes finished and removed records until this turn is completed. */
        std::size_t participant_count() const;
        /** Removed participants remain discoverable in the immutable turn snapshot. */
        bool has_participant( const multiplayer_player_id &player_id ) const;
        std::optional<multiplayer_turn_participant_key> participant_key(
            const multiplayer_player_id &player_id ) const;
        std::optional<multiplayer_turn_participant_state> participant_state(
            const multiplayer_player_id &player_id ) const;
        std::optional<multiplayer_turn_slot> current_slot() const;

        /** Records a typed result; rejected and duplicate results never advance. */
        bool record_action_result(
            const multiplayer_turn_participant_key &participant,
            multiplayer_turn_action_disposition disposition );

        /**
         * Terminalizes the exact current awaiting slot after its player phase
         * completed without executing a semantic command.  Stale generations,
         * non-current slots, non-awaiting states, duplicates, and faults reject.
         */
        bool record_player_phase_completed(
            const multiplayer_turn_participant_key &participant );

        /** Barrier-local state; authoritative session ownership remains external. */
        bool mark_barrier_disconnected(
            const multiplayer_turn_participant_key &participant );
        /** Requires the exact old generation and advances it by exactly one. */
        bool resume_barrier_participant(
            const multiplayer_turn_participant_key &expected_participant,
            std::uint64_t resumed_session_generation );
        /** Rebinds an exact same-generation replay without advancing generation. */
        bool rebind_replayed_barrier_participant(
            const multiplayer_turn_participant_key &exact_current );
        /** Repairs a committed +1 generation while preserving disconnected grace. */
        bool repair_disconnected_barrier_generation(
            const multiplayer_turn_participant_key &expected_participant,
            std::uint64_t committed_session_generation );
        /** Timeout policy only applies to the current disconnected slot. */
        bool apply_disconnect_timeout(
            const multiplayer_turn_participant_key &participant,
            multiplayer_disconnect_timeout_policy policy );
        /**
         * Records that the caller already executed the authoritative auto-wait.
         * This pure policy object cannot verify the external simulation side effect.
         */
        bool record_automatic_wait_executed(
            const multiplayer_turn_participant_key &participant );

        /** Claims permission to execute the completed barrier's world phase once. */
        std::optional<multiplayer_world_ticket> claim_world();
        /**
         * Records caller-reported completion of the exact claimed world turn.
         * World execution and its failure policy remain the orchestrator's responsibility.
         */
        bool record_world_completed( const multiplayer_world_ticket &ticket );

    private:
        struct participant_record {
            multiplayer_turn_participant_key key;
            multiplayer_turn_participant_state state =
                multiplayer_turn_participant_state::awaiting_command;
            std::optional<multiplayer_disconnect_timeout_policy> pending_timeout_policy;
        };

        std::optional<std::size_t> find_participant(
            const multiplayer_player_id &player_id ) const;
        std::optional<std::size_t> find_exact_participant(
            const multiplayer_turn_participant_key &participant ) const;
        bool is_terminal( const participant_record &participant ) const;
        void advance_from_current();
        void enter_world_ready();

        multiplayer_turn_scheduler_stage stage_ = multiplayer_turn_scheduler_stage::idle;
        std::vector<participant_record> participants_;
        std::optional<multiplayer_player_id> previous_first_player_;
        std::optional<std::uint64_t> last_shared_turn_;
        std::uint64_t shared_turn_ = 0;
        std::uint64_t round_ = 0;
        std::size_t cursor_ = 0;
        bool faulted_ = false;
};

#endif // CATA_SRC_MULTIPLAYER_TURN_SCHEDULER_H

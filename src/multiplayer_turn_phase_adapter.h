#pragma once
#ifndef CATA_SRC_MULTIPLAYER_TURN_PHASE_ADAPTER_H
#define CATA_SRC_MULTIPLAYER_TURN_PHASE_ADAPTER_H

#include <cstdint>
#include <functional>
#include <optional>

#include "memory_fast.h"
#include "multiplayer_turn_scheduler.h"

class avatar;
class game;
class multiplayer_player_runtime;

enum class multiplayer_turn_phase_adapter_status : std::uint8_t {
    completed,
    faulted,
    wrong_thread,
    invalid_scheduler_state,
    runtime_not_found,
    stale_session_generation,
    inactive_runtime,
    activation_failed,
    player_callback_failed,
    automatic_wait_failed,
    world_callback_failed,
    context_restore_failed,
    scheduler_record_failed,
    world_already_attempted
};

/**
 * Simulation-thread bridge between the pure turn scheduler and live game rules.
 *
 * One adapter instance is scoped to one begun shared-turn barrier and must be
 * discarded after world completion or a latched fault.  Callback failure after
 * entering a player context or claiming the world latches this adapter as
 * faulted, preventing retries of partially executed simulation work.  Player
 * callbacks are trusted orchestration code and may return an accepted
 * disposition only after a shared rule executor has performed the action.
 */
class multiplayer_turn_phase_adapter
{
    public:
        using player_callback = std::function<std::optional<multiplayer_turn_action_disposition>(
                                    multiplayer_player_runtime &, avatar & )>;
        using world_callback = std::function<bool( const multiplayer_world_ticket & )>;

        multiplayer_turn_phase_adapter( game &simulation,
                                        multiplayer_turn_scheduler &scheduler );
        multiplayer_turn_phase_adapter( const multiplayer_turn_phase_adapter & ) = delete;
        multiplayer_turn_phase_adapter &operator=(
            const multiplayer_turn_phase_adapter & ) = delete;
        multiplayer_turn_phase_adapter( multiplayer_turn_phase_adapter && ) = delete;
        multiplayer_turn_phase_adapter &operator=( multiplayer_turn_phase_adapter && ) = delete;

        multiplayer_turn_phase_adapter_status execute_current_player(
            const multiplayer_turn_participant_key &participant,
            const player_callback &callback );
        multiplayer_turn_phase_adapter_status execute_authoritative_wait(
            const multiplayer_turn_participant_key &participant );
        multiplayer_turn_phase_adapter_status execute_claimed_world(
            const world_callback &callback );

        bool is_faulted() const noexcept;

    private:
        struct active_context_snapshot {
            multiplayer_player_runtime *runtime = nullptr;
            avatar *player = nullptr;
        };

        multiplayer_turn_phase_adapter_status validate_current_participant(
            const multiplayer_turn_participant_key &participant,
            multiplayer_turn_participant_state expected_state,
            shared_ptr_fast<multiplayer_player_runtime> &runtime ) const;
        active_context_snapshot capture_active_context() const;
        bool active_context_matches( const active_context_snapshot &expected ) const;
        multiplayer_turn_phase_adapter_status latch_fault(
            multiplayer_turn_phase_adapter_status status );

        game &simulation_;
        multiplayer_turn_scheduler &scheduler_;
        active_context_snapshot root_context_;
        bool faulted_ = false;
        bool world_attempted_ = false;
};

#endif // CATA_SRC_MULTIPLAYER_TURN_PHASE_ADAPTER_H

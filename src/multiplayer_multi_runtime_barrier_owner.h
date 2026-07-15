#pragma once
#ifndef CATA_SRC_MULTIPLAYER_MULTI_RUNTIME_BARRIER_OWNER_H
#define CATA_SRC_MULTIPLAYER_MULTI_RUNTIME_BARRIER_OWNER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "memory_fast.h"
#include "multiplayer_turn_phase_adapter.h"
#include "multiplayer_turn_scheduler.h"

class game;
class multiplayer_player_runtime;

enum class multiplayer_multi_runtime_barrier_status : std::uint8_t {
    applied,
    duplicate,
    not_ready,
    wrong_thread,
    invalid_roster,
    runtime_not_found,
    stale_session_generation,
    inactive_runtime,
    runtime_identity_mismatch,
    invalid_scheduler_state,
    faulted
};

enum class multiplayer_multi_runtime_barrier_fault_stage : std::uint8_t {
    none,
    pre_gameplay,
    player_action,
    automatic_wait,
    world,
    external_player_end
};

enum class multiplayer_multi_runtime_barrier_stage : std::uint8_t {
    idle,
    player_actions,
    world_ready,
    world_processing,
    player_end_pending,
    faulted
};

class multiplayer_multi_runtime_barrier_owner;

/**
 * Opaque witness that this owner's exact scheduler world callback completed.
 *
 * This process-local witness is valid only while its owner remains alive.  It
 * must not be persisted or queued across owner replacement; an asynchronous
 * outer owner must first replace the raw owner identity with a durable tag.
 */
class multiplayer_multi_runtime_world_completion
{
    public:
        multiplayer_multi_runtime_world_completion() = default;

    private:
        friend class multiplayer_multi_runtime_barrier_owner;

        multiplayer_multi_runtime_world_completion(
            const multiplayer_multi_runtime_barrier_owner *owner,
            std::uint64_t shared_turn, std::uint64_t epoch ) :
            owner_( owner ), shared_turn_( shared_turn ), epoch_( epoch ) {}

        const multiplayer_multi_runtime_barrier_owner *owner_ = nullptr;
        std::uint64_t shared_turn_ = 0;
        std::uint64_t epoch_ = 0;
};

/**
 * Inner simulation-thread owner for one multi-runtime shared barrier.
 *
 * This class deliberately does not own transport, session admission, runtime
 * lifecycle, player-begin/player-end, canonical save safety, or the legacy
 * world implementation.  A future production multi-runtime session owner must
 * compose those outer effects around this barrier contract.  It validates the
 * complete immutable roster before any player callback can run, owns the
 * persistent fair scheduler and one adapter per barrier, and fail-stops after
 * an uncertain rule/world side effect.  World completion enters an explicit
 * player_end_pending state; an outer phase owner must report exact player-end
 * completion before this owner can start the next barrier.  That report is a
 * sequencing contract, not proof that canonical save state is safe.
 */
class multiplayer_multi_runtime_barrier_owner
{
    public:
        using player_callback = multiplayer_turn_phase_adapter::player_callback;
        using world_callback = multiplayer_turn_phase_adapter::world_callback;

        static std::unique_ptr<multiplayer_multi_runtime_barrier_owner> create(
            game &simulation, std::size_t maximum_participants,
            std::string &error );
        ~multiplayer_multi_runtime_barrier_owner();

        multiplayer_multi_runtime_barrier_owner(
            const multiplayer_multi_runtime_barrier_owner & ) = delete;
        multiplayer_multi_runtime_barrier_owner &operator=(
            const multiplayer_multi_runtime_barrier_owner & ) = delete;
        multiplayer_multi_runtime_barrier_owner(
            multiplayer_multi_runtime_barrier_owner && ) = delete;
        multiplayer_multi_runtime_barrier_owner &operator=(
            multiplayer_multi_runtime_barrier_owner && ) = delete;

        bool valid( std::string &error ) const;

        /** Requires one to capacity exact, registry-owned active runtime keys. */
        multiplayer_multi_runtime_barrier_status begin_turn(
            const std::vector<multiplayer_turn_participant_key> &roster );

        multiplayer_turn_phase_adapter_status execute_current_player(
            const multiplayer_turn_participant_key &expected_participant,
            const player_callback &callback );
        multiplayer_multi_runtime_barrier_status complete_player_phase(
            const multiplayer_turn_participant_key &expected_participant );

        multiplayer_multi_runtime_barrier_status mark_barrier_disconnected(
            const multiplayer_turn_participant_key &participant );
        multiplayer_multi_runtime_barrier_status apply_disconnect_timeout(
            const multiplayer_turn_participant_key &participant,
            multiplayer_disconnect_timeout_policy policy );
        multiplayer_turn_phase_adapter_status execute_authoritative_wait(
            const multiplayer_turn_participant_key &expected_participant );

        /** Completes scheduler world ownership, not canonical player-end/save. */
        multiplayer_turn_phase_adapter_status execute_world(
            const world_callback &callback );
        std::optional<multiplayer_multi_runtime_world_completion>
        pending_world_completion() const;
        /** Caller-reported outer player-end completion; not canonical save proof. */
        multiplayer_multi_runtime_barrier_status record_external_player_end_completed(
            const multiplayer_multi_runtime_world_completion &completion );
        /** Allows a trusted outer owner to fail-stop after an external effect diverges. */
        multiplayer_multi_runtime_barrier_status latch_external_fault(
            bool gameplay_side_effects_may_have_occurred = true ) noexcept;

        multiplayer_multi_runtime_barrier_stage stage() const;
        std::size_t maximum_participants() const;
        std::size_t participant_count() const;
        std::optional<multiplayer_turn_slot> current_slot() const;
        std::optional<std::uint64_t> active_shared_turn() const;
        bool has_open_boundary() const noexcept;
        bool is_faulted() const noexcept;
        multiplayer_multi_runtime_barrier_fault_stage fault_stage() const noexcept;
        bool gameplay_side_effects_may_have_occurred() const noexcept;

    private:
        multiplayer_multi_runtime_barrier_owner( game &simulation,
                std::size_t maximum_participants );

        multiplayer_multi_runtime_barrier_status validate_roster(
            const std::vector<multiplayer_turn_participant_key> &roster,
            std::vector<shared_ptr_fast<multiplayer_player_runtime>> &resolved ) const;
        multiplayer_turn_phase_adapter_status validate_pinned_runtime(
            const multiplayer_turn_participant_key &participant ) const;
        multiplayer_turn_phase_adapter_status validate_pinned_roster() const;
        multiplayer_multi_runtime_barrier_status latch_fault(
            multiplayer_multi_runtime_barrier_fault_stage stage,
            bool gameplay_side_effects_may_have_occurred ) noexcept;
        void note_adapter_status(
            multiplayer_turn_phase_adapter_status status,
            multiplayer_multi_runtime_barrier_fault_stage stage,
            bool gameplay_side_effects_may_have_occurred ) noexcept;

        game &simulation_;
        std::size_t maximum_participants_ = 0;
        multiplayer_turn_scheduler scheduler_;
        std::unique_ptr<multiplayer_turn_phase_adapter> adapter_;
        std::vector<multiplayer_turn_participant_key> active_roster_;
        std::vector<shared_ptr_fast<multiplayer_player_runtime>> active_runtimes_;
        std::optional<std::uint64_t> active_shared_turn_;
        std::optional<multiplayer_multi_runtime_world_completion> pending_completion_;
        std::optional<multiplayer_multi_runtime_world_completion> last_completion_;
        std::uint64_t next_shared_turn_ = 1;
        std::uint64_t next_completion_epoch_ = 1;
        bool faulted_ = false;
        multiplayer_multi_runtime_barrier_fault_stage fault_stage_ =
            multiplayer_multi_runtime_barrier_fault_stage::none;
        bool gameplay_side_effects_may_have_occurred_ = false;
        bool valid_ = false;
        std::string invalid_reason_;
};

#endif // CATA_SRC_MULTIPLAYER_MULTI_RUNTIME_BARRIER_OWNER_H

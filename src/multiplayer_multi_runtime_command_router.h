#pragma once
#ifndef CATA_SRC_MULTIPLAYER_MULTI_RUNTIME_COMMAND_ROUTER_H
#define CATA_SRC_MULTIPLAYER_MULTI_RUNTIME_COMMAND_ROUTER_H

#include <optional>

#include "multiplayer_command_executor.h"
#include "multiplayer_multi_runtime_barrier_owner.h"

enum class multiplayer_multi_runtime_command_resolution {
    none,
    executor_result,
    unsupported_move,
    blocked_by_player
};

struct multiplayer_multi_runtime_routed_command_result {
    multiplayer_turn_phase_adapter_status phase_status =
        multiplayer_turn_phase_adapter_status::invalid_scheduler_state;
    std::optional<multiplayer_command_execution> execution;
    std::optional<multiplayer_turn_action_disposition> disposition;
    multiplayer_multi_runtime_command_resolution resolution =
        multiplayer_multi_runtime_command_resolution::none;
    /** Internal authority detail; an outer wire owner must visibility-filter it. */
    std::optional<multiplayer_turn_participant_key> blocking_player;
};

/**
 * Executes one verified semantic command for the exact current barrier slot.
 *
 * This inner router deliberately does not own revisions, deduplication,
 * transport, or admission.  Multi-runtime movement currently supports the
 * side-effect-audited adjacent empty-ground subset and authoritative player
 * collision rejection; unsupported legacy branches reject without advancing
 * the scheduler.
 */
multiplayer_multi_runtime_routed_command_result
multiplayer_route_multi_runtime_basic_command(
    multiplayer_multi_runtime_barrier_owner &owner,
    const multiplayer_turn_participant_key &expected_participant,
    const multiplayer_player_command &command );

#endif // CATA_SRC_MULTIPLAYER_MULTI_RUNTIME_COMMAND_ROUTER_H

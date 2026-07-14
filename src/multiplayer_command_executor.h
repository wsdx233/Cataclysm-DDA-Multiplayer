#pragma once
#ifndef CATA_SRC_MULTIPLAYER_COMMAND_EXECUTOR_H
#define CATA_SRC_MULTIPLAYER_COMMAND_EXECUTOR_H

#include <string>

#include "coordinates.h"
#include "multiplayer_protocol.h"

class avatar;
class game;
class map;

enum class multiplayer_command_execution_context {
    local_interactive,
    remote_headless
};

enum class multiplayer_wait_execution_mode {
    player_requested,
    authoritative_forced
};

struct multiplayer_command_execution {
    multiplayer_command_status status = multiplayer_command_status::rejected;
    multiplayer_protocol_rejection rejection = multiplayer_protocol_rejection::invalid_command;
    int moves_spent = 0;
    bool action_taken = false;
    std::string message;
};

/** Shared wait rule used by local input and semantic remote commands. */
bool multiplayer_execute_wait( game &simulation, avatar &player,
                               multiplayer_wait_execution_mode mode,
                               bool repeat_safe_mode_warnings = true );

/** Shared adjacent movement rule used by local input and semantic remote commands. */
bool multiplayer_execute_move( game &simulation, avatar &player, map &here,
                               const tripoint_rel_ms &direction,
                               multiplayer_command_execution_context context );

/** Validates and executes the Phase 2 wait/move semantic command subset. */
multiplayer_command_execution multiplayer_execute_basic_command(
    game &simulation, avatar &player, const multiplayer_player_command &command );

#endif // CATA_SRC_MULTIPLAYER_COMMAND_EXECUTOR_H

#pragma once
#ifndef CATA_SRC_MULTIPLAYER_RUNTIME_MODE_H
#define CATA_SRC_MULTIPLAYER_RUNTIME_MODE_H

#include <cstdint>

enum class multiplayer_runtime_mode : std::uint8_t {
    local_client,
    network_client,
    dedicated_server,
    test
};

const char *multiplayer_runtime_mode_name( multiplayer_runtime_mode mode ) noexcept;
bool multiplayer_runtime_mode_uses_local_ui( multiplayer_runtime_mode mode ) noexcept;
void set_multiplayer_runtime_mode( multiplayer_runtime_mode mode ) noexcept;
multiplayer_runtime_mode get_multiplayer_runtime_mode() noexcept;

#endif // CATA_SRC_MULTIPLAYER_RUNTIME_MODE_H

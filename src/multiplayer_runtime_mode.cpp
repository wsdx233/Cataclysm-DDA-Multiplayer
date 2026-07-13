#include "multiplayer_runtime_mode.h"

namespace
{
multiplayer_runtime_mode current_runtime_mode = multiplayer_runtime_mode::local_client;
} // namespace

#include "cata_assert.h"

const char *multiplayer_runtime_mode_name( const multiplayer_runtime_mode mode ) noexcept
{
    switch( mode ) {
        case multiplayer_runtime_mode::local_client:
            return "local_client";
        case multiplayer_runtime_mode::network_client:
            return "network_client";
        case multiplayer_runtime_mode::dedicated_server:
            return "dedicated_server";
        case multiplayer_runtime_mode::test:
            return "test";
    }
    cata_assert( false );
    return "unknown";
}

bool multiplayer_runtime_mode_uses_local_ui( const multiplayer_runtime_mode mode ) noexcept
{
    return mode == multiplayer_runtime_mode::local_client ||
           mode == multiplayer_runtime_mode::network_client;
}

void set_multiplayer_runtime_mode( const multiplayer_runtime_mode mode ) noexcept
{
    current_runtime_mode = mode;
}

multiplayer_runtime_mode get_multiplayer_runtime_mode() noexcept
{
    return current_runtime_mode;
}

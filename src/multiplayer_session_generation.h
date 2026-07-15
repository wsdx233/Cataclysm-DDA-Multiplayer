#pragma once
#ifndef CATA_SRC_MULTIPLAYER_SESSION_GENERATION_H
#define CATA_SRC_MULTIPLAYER_SESSION_GENERATION_H

#include <cstdint>
#include <limits>

inline constexpr std::uint64_t multiplayer_session_generation_exclusive_limit =
    static_cast<std::uint64_t>( std::numeric_limits<std::int64_t>::max() );

constexpr bool multiplayer_is_valid_session_generation( const std::uint64_t generation )
{
    return generation > 0 && generation < multiplayer_session_generation_exclusive_limit;
}

constexpr bool multiplayer_is_next_session_generation( const std::uint64_t expected_old,
        const std::uint64_t next )
{
    return expected_old < multiplayer_session_generation_exclusive_limit - 1 &&
           next == expected_old + 1 && multiplayer_is_valid_session_generation( next );
}

#endif // CATA_SRC_MULTIPLAYER_SESSION_GENERATION_H

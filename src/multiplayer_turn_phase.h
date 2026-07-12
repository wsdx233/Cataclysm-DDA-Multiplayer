#pragma once
#ifndef CATA_SRC_MULTIPLAYER_TURN_PHASE_H
#define CATA_SRC_MULTIPLAYER_TURN_PHASE_H

#include <chrono>
#include <cstdint>

enum class multiplayer_turn_phase : std::uint8_t {
    turn_begin,
    player_begin,
    player_input,
    world,
    player_end
};

const char *multiplayer_turn_phase_name( multiplayer_turn_phase phase );

class multiplayer_turn_phase_observer
{
    public:
        virtual ~multiplayer_turn_phase_observer() = default;
        virtual void on_phase_finished( multiplayer_turn_phase phase,
                                        std::chrono::nanoseconds elapsed ) noexcept = 0;
};

class scoped_multiplayer_turn_phase_observer
{
    public:
        explicit scoped_multiplayer_turn_phase_observer(
            multiplayer_turn_phase_observer &observer ) noexcept;
        ~scoped_multiplayer_turn_phase_observer();

        scoped_multiplayer_turn_phase_observer(
            const scoped_multiplayer_turn_phase_observer & ) = delete;
        scoped_multiplayer_turn_phase_observer &operator=(
            const scoped_multiplayer_turn_phase_observer & ) = delete;
        scoped_multiplayer_turn_phase_observer( scoped_multiplayer_turn_phase_observer && ) = delete;
        scoped_multiplayer_turn_phase_observer &operator=(
            scoped_multiplayer_turn_phase_observer && ) = delete;

    private:
        multiplayer_turn_phase_observer *previous;
};

class multiplayer_turn_phase_trace
{
    public:
        explicit multiplayer_turn_phase_trace( multiplayer_turn_phase initial_phase ) noexcept;
        ~multiplayer_turn_phase_trace();

        multiplayer_turn_phase_trace( const multiplayer_turn_phase_trace & ) = delete;
        multiplayer_turn_phase_trace &operator=( const multiplayer_turn_phase_trace & ) = delete;
        multiplayer_turn_phase_trace( multiplayer_turn_phase_trace && ) = delete;
        multiplayer_turn_phase_trace &operator=( multiplayer_turn_phase_trace && ) = delete;

        void enter( multiplayer_turn_phase next_phase ) noexcept;

    private:
        using clock = std::chrono::steady_clock;

        void finish_current( clock::time_point now ) noexcept;

        multiplayer_turn_phase_observer *observer;
        multiplayer_turn_phase current_phase;
        clock::time_point phase_started;
};

#endif // CATA_SRC_MULTIPLAYER_TURN_PHASE_H

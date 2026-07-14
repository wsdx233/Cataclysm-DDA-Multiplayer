#include "multiplayer_turn_phase_adapter.h"

#include <exception>

#include "avatar.h"
#include "game.h"
#include "multiplayer_command_executor.h"
#include "multiplayer_player_context.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_player_runtime.h"

multiplayer_turn_phase_adapter::multiplayer_turn_phase_adapter(
    game &simulation, multiplayer_turn_scheduler &scheduler ) :
    simulation_( simulation ),
    scheduler_( scheduler ),
    root_context_( simulation.is_simulation_thread() ? capture_active_context() :
                   active_context_snapshot{} )
{
}

multiplayer_turn_phase_adapter_status multiplayer_turn_phase_adapter::execute_current_player(
    const multiplayer_turn_participant_key &participant,
    const player_callback &callback )
{
    if( faulted_ ) {
        return multiplayer_turn_phase_adapter_status::faulted;
    }
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_turn_phase_adapter_status::wrong_thread;
    }
    if( scheduler_.is_faulted() ) {
        return multiplayer_turn_phase_adapter_status::faulted;
    }

    shared_ptr_fast<multiplayer_player_runtime> runtime;
    const multiplayer_turn_phase_adapter_status validation = validate_current_participant(
                participant, multiplayer_turn_participant_state::awaiting_command, runtime );
    if( validation != multiplayer_turn_phase_adapter_status::completed ) {
        return validation;
    }
    if( !callback ) {
        return latch_fault( multiplayer_turn_phase_adapter_status::player_callback_failed );
    }

    std::optional<multiplayer_turn_action_disposition> disposition;
    bool activation_succeeded = false;
    bool callback_succeeded = false;
    {
        multiplayer_active_player_guard guard( simulation_, runtime->player_owner() );
        activation_succeeded = guard.is_engaged() &&
                               &simulation_.active_player_runtime() == runtime.get() &&
                               &simulation_.active_avatar() == &runtime->player();
        if( activation_succeeded ) {
            try {
                disposition = callback( *runtime, runtime->player() );
                callback_succeeded = disposition.has_value();
                if( callback_succeeded &&
                    ( *disposition ==
                      multiplayer_turn_action_disposition::accepted_remains_eligible ||
                      *disposition == multiplayer_turn_action_disposition::accepted_finished ) ) {
                    simulation_.record_turn_player_action( runtime->player() );
                }
            } catch( ... ) {
                callback_succeeded = false;
            }
        }
    }

    if( !active_context_matches( root_context_ ) ) {
        return latch_fault( multiplayer_turn_phase_adapter_status::context_restore_failed );
    }
    if( !activation_succeeded ) {
        return multiplayer_turn_phase_adapter_status::activation_failed;
    }
    if( !callback_succeeded ) {
        return latch_fault( multiplayer_turn_phase_adapter_status::player_callback_failed );
    }
    if( !scheduler_.record_action_result( participant, *disposition ) ) {
        return latch_fault( multiplayer_turn_phase_adapter_status::scheduler_record_failed );
    }
    return multiplayer_turn_phase_adapter_status::completed;
}

multiplayer_turn_phase_adapter_status
multiplayer_turn_phase_adapter::execute_authoritative_wait(
    const multiplayer_turn_participant_key &participant )
{
    if( faulted_ ) {
        return multiplayer_turn_phase_adapter_status::faulted;
    }
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_turn_phase_adapter_status::wrong_thread;
    }
    if( scheduler_.is_faulted() ) {
        return multiplayer_turn_phase_adapter_status::faulted;
    }

    shared_ptr_fast<multiplayer_player_runtime> runtime;
    const multiplayer_turn_phase_adapter_status validation = validate_current_participant(
                participant, multiplayer_turn_participant_state::automatic_wait_pending, runtime );
    if( validation != multiplayer_turn_phase_adapter_status::completed ) {
        return validation;
    }

    bool activation_succeeded = false;
    bool wait_succeeded = false;
    {
        multiplayer_active_player_guard guard( simulation_, runtime->player_owner() );
        activation_succeeded = guard.is_engaged() &&
                               &simulation_.active_player_runtime() == runtime.get() &&
                               &simulation_.active_avatar() == &runtime->player();
        if( activation_succeeded ) {
            try {
                wait_succeeded = multiplayer_execute_wait(
                                     simulation_, runtime->player(),
                                     multiplayer_wait_execution_mode::authoritative_forced ) &&
                                 runtime->player().get_moves() <= 0;
                if( wait_succeeded ) {
                    simulation_.record_turn_player_action( runtime->player() );
                }
            } catch( ... ) {
                wait_succeeded = false;
            }
        }
    }

    if( !active_context_matches( root_context_ ) ) {
        return latch_fault( multiplayer_turn_phase_adapter_status::context_restore_failed );
    }
    if( !activation_succeeded ) {
        return multiplayer_turn_phase_adapter_status::activation_failed;
    }
    if( !wait_succeeded ) {
        return latch_fault( multiplayer_turn_phase_adapter_status::automatic_wait_failed );
    }
    if( !scheduler_.record_automatic_wait_executed( participant ) ) {
        return latch_fault( multiplayer_turn_phase_adapter_status::scheduler_record_failed );
    }
    return multiplayer_turn_phase_adapter_status::completed;
}

multiplayer_turn_phase_adapter_status multiplayer_turn_phase_adapter::execute_claimed_world(
    const world_callback &callback )
{
    if( faulted_ ) {
        return multiplayer_turn_phase_adapter_status::faulted;
    }
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_turn_phase_adapter_status::wrong_thread;
    }
    if( scheduler_.is_faulted() ) {
        return multiplayer_turn_phase_adapter_status::faulted;
    }
    if( !active_context_matches( root_context_ ) ) {
        return multiplayer_turn_phase_adapter_status::activation_failed;
    }
    if( world_attempted_ ) {
        return multiplayer_turn_phase_adapter_status::world_already_attempted;
    }
    if( scheduler_.stage() != multiplayer_turn_scheduler_stage::world_ready ) {
        return multiplayer_turn_phase_adapter_status::invalid_scheduler_state;
    }

    const std::optional<multiplayer_world_ticket> ticket = scheduler_.claim_world();
    if( !ticket ) {
        return multiplayer_turn_phase_adapter_status::invalid_scheduler_state;
    }
    world_attempted_ = true;

    bool callback_succeeded = false;
    if( callback ) {
        try {
            callback_succeeded = callback( *ticket );
        } catch( ... ) {
            callback_succeeded = false;
        }
    }

    if( !active_context_matches( root_context_ ) ) {
        return latch_fault( multiplayer_turn_phase_adapter_status::context_restore_failed );
    }
    if( !callback_succeeded ) {
        return latch_fault( multiplayer_turn_phase_adapter_status::world_callback_failed );
    }
    if( !scheduler_.record_world_completed( *ticket ) ) {
        return latch_fault( multiplayer_turn_phase_adapter_status::scheduler_record_failed );
    }
    return multiplayer_turn_phase_adapter_status::completed;
}

bool multiplayer_turn_phase_adapter::is_faulted() const noexcept
{
    return faulted_ || scheduler_.is_faulted();
}

multiplayer_turn_phase_adapter_status
multiplayer_turn_phase_adapter::validate_current_participant(
    const multiplayer_turn_participant_key &participant,
    const multiplayer_turn_participant_state expected_state,
    shared_ptr_fast<multiplayer_player_runtime> &runtime ) const
{
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_turn_phase_adapter_status::wrong_thread;
    }
    if( scheduler_.is_faulted() ) {
        return multiplayer_turn_phase_adapter_status::faulted;
    }
    if( !active_context_matches( root_context_ ) ) {
        return multiplayer_turn_phase_adapter_status::activation_failed;
    }

    const std::optional<multiplayer_turn_slot> slot = scheduler_.current_slot();
    if( !slot || slot->state != expected_state ||
        slot->participant.player_id != participant.player_id ) {
        return multiplayer_turn_phase_adapter_status::invalid_scheduler_state;
    }
    if( slot->participant.session_generation != participant.session_generation ) {
        return multiplayer_turn_phase_adapter_status::stale_session_generation;
    }

    runtime = simulation_.multiplayer_players().find_by_player_id( participant.player_id );
    if( runtime == nullptr ) {
        return multiplayer_turn_phase_adapter_status::runtime_not_found;
    }
    if( runtime->session_generation() != participant.session_generation ) {
        return multiplayer_turn_phase_adapter_status::stale_session_generation;
    }
    if( runtime->status() != multiplayer_player_status::active ) {
        return multiplayer_turn_phase_adapter_status::inactive_runtime;
    }
    if( !simulation_.multiplayer_players().owns( runtime ) ||
        !simulation_.multiplayer_players().owns( runtime->player_owner() ) ) {
        return multiplayer_turn_phase_adapter_status::activation_failed;
    }
    return multiplayer_turn_phase_adapter_status::completed;
}

multiplayer_turn_phase_adapter::active_context_snapshot
multiplayer_turn_phase_adapter::capture_active_context() const
{
    return { &simulation_.active_player_runtime(), &simulation_.active_avatar() };
}

bool multiplayer_turn_phase_adapter::active_context_matches(
    const active_context_snapshot &expected ) const
{
    return &simulation_.active_player_runtime() == expected.runtime &&
           &simulation_.active_avatar() == expected.player;
}

multiplayer_turn_phase_adapter_status multiplayer_turn_phase_adapter::latch_fault(
    const multiplayer_turn_phase_adapter_status status )
{
    faulted_ = true;
    scheduler_.latch_execution_fault();
    return status;
}

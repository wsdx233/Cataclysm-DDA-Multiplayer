#include "multiplayer_multi_runtime_barrier_owner.h"

#include <limits>
#include <unordered_set>
#include <utility>

#include "game.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_player_runtime.h"
#include "multiplayer_session_generation.h"

namespace
{

template<typename T>
bool same_shared_owner_and_pointer( const shared_ptr_fast<T> &lhs,
                                    const shared_ptr_fast<T> &rhs )
{
    return lhs.get() == rhs.get() && !lhs.owner_before( rhs ) && !rhs.owner_before( lhs );
}

multiplayer_multi_runtime_barrier_status barrier_status_from_adapter_status(
    const multiplayer_turn_phase_adapter_status status )
{
    switch( status ) {
        case multiplayer_turn_phase_adapter_status::completed:
            return multiplayer_multi_runtime_barrier_status::applied;
        case multiplayer_turn_phase_adapter_status::wrong_thread:
            return multiplayer_multi_runtime_barrier_status::wrong_thread;
        case multiplayer_turn_phase_adapter_status::runtime_not_found:
            return multiplayer_multi_runtime_barrier_status::runtime_not_found;
        case multiplayer_turn_phase_adapter_status::stale_session_generation:
            return multiplayer_multi_runtime_barrier_status::stale_session_generation;
        case multiplayer_turn_phase_adapter_status::inactive_runtime:
            return multiplayer_multi_runtime_barrier_status::inactive_runtime;
        case multiplayer_turn_phase_adapter_status::activation_failed:
            return multiplayer_multi_runtime_barrier_status::runtime_identity_mismatch;
        case multiplayer_turn_phase_adapter_status::faulted:
            return multiplayer_multi_runtime_barrier_status::faulted;
        default:
            return multiplayer_multi_runtime_barrier_status::invalid_scheduler_state;
    }
}

} // namespace

std::unique_ptr<multiplayer_multi_runtime_barrier_owner>
multiplayer_multi_runtime_barrier_owner::create(
    game &simulation, const std::size_t maximum_participants, std::string &error )
{
    if( !simulation.is_simulation_thread() ) {
        error = "multi-runtime barrier owner must be constructed on the simulation thread";
        return nullptr;
    }
    if( maximum_participants < 2 ||
        maximum_participants > multiplayer_turn_scheduler::maximum_participants ) {
        error = "multi-runtime barrier owner capacity must be between 2 and 4";
        return nullptr;
    }
    std::unique_ptr<multiplayer_multi_runtime_barrier_owner> owner(
        new multiplayer_multi_runtime_barrier_owner( simulation, maximum_participants ) );
    if( !owner->valid( error ) ) {
        return nullptr;
    }
    error.clear();
    return owner;
}

multiplayer_multi_runtime_barrier_owner::multiplayer_multi_runtime_barrier_owner(
    game &simulation, const std::size_t maximum_participants ) :
    simulation_( simulation ), maximum_participants_( maximum_participants ), valid_( true )
{
}

multiplayer_multi_runtime_barrier_owner::~multiplayer_multi_runtime_barrier_owner() = default;

bool multiplayer_multi_runtime_barrier_owner::valid( std::string &error ) const
{
    if( !valid_ ) {
        error = invalid_reason_.empty() ? "multi-runtime barrier owner is invalid" :
                invalid_reason_;
        return false;
    }
    error.clear();
    return true;
}

multiplayer_multi_runtime_barrier_status
multiplayer_multi_runtime_barrier_owner::begin_turn(
    const std::vector<multiplayer_turn_participant_key> &roster )
{
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_multi_runtime_barrier_status::wrong_thread;
    }
    if( is_faulted() ) {
        return multiplayer_multi_runtime_barrier_status::faulted;
    }
    if( adapter_ || active_shared_turn_ || pending_completion_ ||
        scheduler_.stage() != multiplayer_turn_scheduler_stage::idle ) {
        return multiplayer_multi_runtime_barrier_status::not_ready;
    }

    std::vector<shared_ptr_fast<multiplayer_player_runtime>> resolved;
    const multiplayer_multi_runtime_barrier_status validation = validate_roster(
                roster, resolved );
    if( validation != multiplayer_multi_runtime_barrier_status::applied ) {
        return validation;
    }
    if( next_shared_turn_ == std::numeric_limits<std::uint64_t>::max() ) {
        return latch_fault( multiplayer_multi_runtime_barrier_fault_stage::pre_gameplay,
                            false );
    }

    const std::uint64_t shared_turn = next_shared_turn_;
    if( !scheduler_.begin_turn( shared_turn, roster ) ) {
        return latch_fault( multiplayer_multi_runtime_barrier_fault_stage::pre_gameplay,
                            false );
    }
    ++next_shared_turn_;
    active_roster_ = roster;
    active_runtimes_ = std::move( resolved );
    active_shared_turn_ = shared_turn;
    gameplay_side_effects_may_have_occurred_ = false;
    adapter_ = std::make_unique<multiplayer_turn_phase_adapter>( simulation_, scheduler_ );
    return multiplayer_multi_runtime_barrier_status::applied;
}

multiplayer_turn_phase_adapter_status
multiplayer_multi_runtime_barrier_owner::execute_current_player(
    const multiplayer_turn_participant_key &expected_participant,
    const player_callback &callback )
{
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_turn_phase_adapter_status::wrong_thread;
    }
    if( is_faulted() ) {
        return multiplayer_turn_phase_adapter_status::faulted;
    }
    if( !adapter_ ) {
        return multiplayer_turn_phase_adapter_status::invalid_scheduler_state;
    }
    const std::optional<multiplayer_turn_slot> before = scheduler_.current_slot();
    const bool exact_current_before = before &&
                                      before->participant == expected_participant;
    if( exact_current_before ) {
        const multiplayer_turn_phase_adapter_status pinned =
            validate_pinned_runtime( expected_participant );
        if( pinned != multiplayer_turn_phase_adapter_status::completed ) {
            latch_fault( multiplayer_multi_runtime_barrier_fault_stage::player_action,
                         gameplay_side_effects_may_have_occurred_ );
            return pinned;
        }
    }
    bool callback_entered = false;
    bool callback_may_have_side_effects = false;
    const multiplayer_turn_phase_adapter_status status = adapter_->execute_current_player(
                expected_participant,
    [&]( multiplayer_player_runtime & runtime, avatar & player ) {
        callback_entered = true;
        try {
            const std::optional<multiplayer_turn_action_disposition> disposition =
                callback ? callback( runtime, player ) :
                std::optional<multiplayer_turn_action_disposition>();
            callback_may_have_side_effects = !disposition ||
                                             *disposition ==
                                             multiplayer_turn_action_disposition::accepted_remains_eligible ||
                                             *disposition ==
                                             multiplayer_turn_action_disposition::accepted_finished;
            return disposition;
        } catch( ... ) {
            callback_may_have_side_effects = true;
            throw;
        }
    } );
    if( status == multiplayer_turn_phase_adapter_status::completed &&
        callback_may_have_side_effects ) {
        gameplay_side_effects_may_have_occurred_ = true;
    }
    note_adapter_status( status,
                         multiplayer_multi_runtime_barrier_fault_stage::player_action,
                         callback_entered && callback_may_have_side_effects );
    if( exact_current_before &&
        ( status == multiplayer_turn_phase_adapter_status::runtime_not_found ||
          status == multiplayer_turn_phase_adapter_status::stale_session_generation ||
          status == multiplayer_turn_phase_adapter_status::inactive_runtime ||
          status == multiplayer_turn_phase_adapter_status::activation_failed ) ) {
        latch_fault( multiplayer_multi_runtime_barrier_fault_stage::player_action,
                     gameplay_side_effects_may_have_occurred_ );
    }
    return status;
}

multiplayer_multi_runtime_barrier_status
multiplayer_multi_runtime_barrier_owner::complete_player_phase(
    const multiplayer_turn_participant_key &expected_participant )
{
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_multi_runtime_barrier_status::wrong_thread;
    }
    if( is_faulted() ) {
        return multiplayer_multi_runtime_barrier_status::faulted;
    }
    if( !adapter_ ) {
        return multiplayer_multi_runtime_barrier_status::not_ready;
    }
    const std::optional<multiplayer_turn_slot> slot = scheduler_.current_slot();
    if( !slot || slot->participant != expected_participant ||
        slot->state != multiplayer_turn_participant_state::awaiting_command ) {
        return multiplayer_multi_runtime_barrier_status::invalid_scheduler_state;
    }
    const multiplayer_turn_phase_adapter_status pinned =
        validate_pinned_runtime( expected_participant );
    if( pinned != multiplayer_turn_phase_adapter_status::completed ) {
        latch_fault( multiplayer_multi_runtime_barrier_fault_stage::player_action,
                     gameplay_side_effects_may_have_occurred_ );
        return barrier_status_from_adapter_status( pinned );
    }
    if( !scheduler_.record_player_phase_completed( expected_participant ) ) {
        return latch_fault( multiplayer_multi_runtime_barrier_fault_stage::player_action,
                            gameplay_side_effects_may_have_occurred_ );
    }
    return multiplayer_multi_runtime_barrier_status::applied;
}

multiplayer_multi_runtime_barrier_status
multiplayer_multi_runtime_barrier_owner::mark_barrier_disconnected(
    const multiplayer_turn_participant_key &participant )
{
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_multi_runtime_barrier_status::wrong_thread;
    }
    if( is_faulted() ) {
        return multiplayer_multi_runtime_barrier_status::faulted;
    }
    if( !adapter_ ) {
        return multiplayer_multi_runtime_barrier_status::not_ready;
    }
    return scheduler_.mark_barrier_disconnected( participant ) ?
           multiplayer_multi_runtime_barrier_status::applied :
           multiplayer_multi_runtime_barrier_status::invalid_scheduler_state;
}

multiplayer_multi_runtime_barrier_status
multiplayer_multi_runtime_barrier_owner::apply_disconnect_timeout(
    const multiplayer_turn_participant_key &participant,
    const multiplayer_disconnect_timeout_policy policy )
{
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_multi_runtime_barrier_status::wrong_thread;
    }
    if( is_faulted() ) {
        return multiplayer_multi_runtime_barrier_status::faulted;
    }
    if( !adapter_ ) {
        return multiplayer_multi_runtime_barrier_status::not_ready;
    }
    return scheduler_.apply_disconnect_timeout( participant, policy ) ?
           multiplayer_multi_runtime_barrier_status::applied :
           multiplayer_multi_runtime_barrier_status::invalid_scheduler_state;
}

multiplayer_turn_phase_adapter_status
multiplayer_multi_runtime_barrier_owner::execute_authoritative_wait(
    const multiplayer_turn_participant_key &expected_participant )
{
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_turn_phase_adapter_status::wrong_thread;
    }
    if( is_faulted() ) {
        return multiplayer_turn_phase_adapter_status::faulted;
    }
    if( !adapter_ ) {
        return multiplayer_turn_phase_adapter_status::invalid_scheduler_state;
    }
    const std::optional<multiplayer_turn_slot> before = scheduler_.current_slot();
    const bool exact_current_before = before &&
                                      before->participant == expected_participant;
    if( exact_current_before ) {
        const multiplayer_turn_phase_adapter_status pinned =
            validate_pinned_runtime( expected_participant );
        if( pinned != multiplayer_turn_phase_adapter_status::completed ) {
            latch_fault( multiplayer_multi_runtime_barrier_fault_stage::automatic_wait,
                         gameplay_side_effects_may_have_occurred_ );
            return pinned;
        }
    }
    const multiplayer_turn_phase_adapter_status status =
        adapter_->execute_authoritative_wait( expected_participant );
    if( status == multiplayer_turn_phase_adapter_status::completed ) {
        gameplay_side_effects_may_have_occurred_ = true;
    }
    note_adapter_status( status,
                         multiplayer_multi_runtime_barrier_fault_stage::automatic_wait,
                         adapter_->is_faulted() );
    if( exact_current_before &&
        ( status == multiplayer_turn_phase_adapter_status::runtime_not_found ||
          status == multiplayer_turn_phase_adapter_status::stale_session_generation ||
          status == multiplayer_turn_phase_adapter_status::inactive_runtime ||
          status == multiplayer_turn_phase_adapter_status::activation_failed ) ) {
        latch_fault( multiplayer_multi_runtime_barrier_fault_stage::automatic_wait,
                     gameplay_side_effects_may_have_occurred_ );
    }
    return status;
}

multiplayer_turn_phase_adapter_status
multiplayer_multi_runtime_barrier_owner::execute_world( const world_callback &callback )
{
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_turn_phase_adapter_status::wrong_thread;
    }
    if( is_faulted() ) {
        return multiplayer_turn_phase_adapter_status::faulted;
    }
    if( !adapter_ || !active_shared_turn_ || pending_completion_ ) {
        return multiplayer_turn_phase_adapter_status::invalid_scheduler_state;
    }
    const multiplayer_turn_phase_adapter_status pinned = validate_pinned_roster();
    if( pinned != multiplayer_turn_phase_adapter_status::completed ) {
        latch_fault( multiplayer_multi_runtime_barrier_fault_stage::world,
                     gameplay_side_effects_may_have_occurred_ );
        return pinned;
    }

    bool world_entered = false;
    const multiplayer_turn_phase_adapter_status status =
        adapter_->execute_claimed_world(
    [&]( const multiplayer_world_ticket & ticket ) {
        world_entered = true;
        gameplay_side_effects_may_have_occurred_ = true;
        return callback && callback( ticket );
    } );
    note_adapter_status( status,
                         multiplayer_multi_runtime_barrier_fault_stage::world,
                         world_entered );
    if( status == multiplayer_turn_phase_adapter_status::activation_failed ) {
        latch_fault( multiplayer_multi_runtime_barrier_fault_stage::world,
                     gameplay_side_effects_may_have_occurred_ );
    }
    if( status == multiplayer_turn_phase_adapter_status::completed ) {
        if( scheduler_.stage() != multiplayer_turn_scheduler_stage::idle ) {
            latch_fault( multiplayer_multi_runtime_barrier_fault_stage::world, true );
            return multiplayer_turn_phase_adapter_status::scheduler_record_failed;
        }
        if( next_completion_epoch_ == std::numeric_limits<std::uint64_t>::max() ) {
            latch_fault( multiplayer_multi_runtime_barrier_fault_stage::world, true );
            return multiplayer_turn_phase_adapter_status::faulted;
        }
        pending_completion_ = multiplayer_multi_runtime_world_completion(
                                  this, *active_shared_turn_, next_completion_epoch_++ );
        adapter_.reset();
    }
    return status;
}

std::optional<multiplayer_multi_runtime_world_completion>
multiplayer_multi_runtime_barrier_owner::pending_world_completion() const
{
    return pending_completion_;
}

multiplayer_multi_runtime_barrier_status
multiplayer_multi_runtime_barrier_owner::record_external_player_end_completed(
    const multiplayer_multi_runtime_world_completion &completion )
{
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_multi_runtime_barrier_status::wrong_thread;
    }
    if( is_faulted() ) {
        return multiplayer_multi_runtime_barrier_status::faulted;
    }
    if( !pending_completion_ && !active_shared_turn_ && !adapter_ &&
        scheduler_.stage() == multiplayer_turn_scheduler_stage::idle && last_completion_ &&
        completion.owner_ == this && completion.shared_turn_ == last_completion_->shared_turn_ &&
        completion.epoch_ == last_completion_->epoch_ ) {
        return multiplayer_multi_runtime_barrier_status::duplicate;
    }
    if( !pending_completion_ || !active_shared_turn_ || adapter_ ||
        scheduler_.stage() != multiplayer_turn_scheduler_stage::idle ) {
        return multiplayer_multi_runtime_barrier_status::invalid_scheduler_state;
    }
    if( completion.owner_ != this || completion.shared_turn_ != *active_shared_turn_ ||
        completion.epoch_ != pending_completion_->epoch_ ) {
        return latch_fault(
                   multiplayer_multi_runtime_barrier_fault_stage::external_player_end, true );
    }

    last_completion_ = completion;
    pending_completion_.reset();
    active_shared_turn_.reset();
    active_roster_.clear();
    active_runtimes_.clear();
    gameplay_side_effects_may_have_occurred_ = false;
    return multiplayer_multi_runtime_barrier_status::applied;
}

multiplayer_multi_runtime_barrier_status
multiplayer_multi_runtime_barrier_owner::latch_external_fault(
    const bool gameplay_side_effects_may_have_occurred ) noexcept
{
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_multi_runtime_barrier_status::wrong_thread;
    }
    return latch_fault(
               multiplayer_multi_runtime_barrier_fault_stage::external_player_end,
               gameplay_side_effects_may_have_occurred );
}

multiplayer_multi_runtime_barrier_stage
multiplayer_multi_runtime_barrier_owner::stage() const
{
    if( is_faulted() ) {
        return multiplayer_multi_runtime_barrier_stage::faulted;
    }
    if( pending_completion_ ) {
        return multiplayer_multi_runtime_barrier_stage::player_end_pending;
    }
    switch( scheduler_.stage() ) {
        case multiplayer_turn_scheduler_stage::idle:
            return multiplayer_multi_runtime_barrier_stage::idle;
        case multiplayer_turn_scheduler_stage::player_actions:
            return multiplayer_multi_runtime_barrier_stage::player_actions;
        case multiplayer_turn_scheduler_stage::world_ready:
            return multiplayer_multi_runtime_barrier_stage::world_ready;
        case multiplayer_turn_scheduler_stage::world_processing:
            return multiplayer_multi_runtime_barrier_stage::world_processing;
    }
    return multiplayer_multi_runtime_barrier_stage::faulted;
}

std::size_t multiplayer_multi_runtime_barrier_owner::maximum_participants() const
{
    return maximum_participants_;
}

std::size_t multiplayer_multi_runtime_barrier_owner::participant_count() const
{
    return active_roster_.size();
}

std::optional<multiplayer_turn_slot>
multiplayer_multi_runtime_barrier_owner::current_slot() const
{
    return scheduler_.current_slot();
}

std::optional<std::uint64_t>
multiplayer_multi_runtime_barrier_owner::active_shared_turn() const
{
    return active_shared_turn_;
}

bool multiplayer_multi_runtime_barrier_owner::is_faulted() const noexcept
{
    return faulted_ || scheduler_.is_faulted() || ( adapter_ && adapter_->is_faulted() );
}

bool multiplayer_multi_runtime_barrier_owner::has_open_boundary() const noexcept
{
    return adapter_ || active_shared_turn_ || pending_completion_;
}

multiplayer_multi_runtime_barrier_fault_stage
multiplayer_multi_runtime_barrier_owner::fault_stage() const noexcept
{
    return fault_stage_;
}

bool multiplayer_multi_runtime_barrier_owner::gameplay_side_effects_may_have_occurred() const
noexcept
{
    return gameplay_side_effects_may_have_occurred_;
}

multiplayer_multi_runtime_barrier_status
multiplayer_multi_runtime_barrier_owner::validate_roster(
    const std::vector<multiplayer_turn_participant_key> &roster,
    std::vector<shared_ptr_fast<multiplayer_player_runtime>> &resolved ) const
{
    if( roster.empty() || roster.size() > maximum_participants_ ) {
        return multiplayer_multi_runtime_barrier_status::invalid_roster;
    }

    std::unordered_set<std::string> player_ids;
    player_ids.reserve( roster.size() );
    resolved.clear();
    resolved.reserve( roster.size() );
    for( const multiplayer_turn_participant_key &participant : roster ) {
        if( !participant.player_id.is_valid() ||
            !multiplayer_is_valid_session_generation( participant.session_generation ) ||
            !player_ids.emplace( participant.player_id.str() ).second ) {
            return multiplayer_multi_runtime_barrier_status::invalid_roster;
        }

        const shared_ptr_fast<multiplayer_player_runtime> runtime =
            simulation_.multiplayer_players().find_by_player_id( participant.player_id );
        if( runtime == nullptr || !simulation_.multiplayer_players().owns( runtime ) ||
            !simulation_.multiplayer_players().owns( runtime->player_owner() ) ) {
            return multiplayer_multi_runtime_barrier_status::runtime_not_found;
        }
        if( runtime->session_generation() != participant.session_generation ) {
            return multiplayer_multi_runtime_barrier_status::stale_session_generation;
        }
        if( runtime->status() != multiplayer_player_status::active ) {
            return multiplayer_multi_runtime_barrier_status::inactive_runtime;
        }
        resolved.emplace_back( runtime );
    }
    return multiplayer_multi_runtime_barrier_status::applied;
}

multiplayer_turn_phase_adapter_status
multiplayer_multi_runtime_barrier_owner::validate_pinned_runtime(
    const multiplayer_turn_participant_key &participant ) const
{
    if( !simulation_.is_simulation_thread() ) {
        return multiplayer_turn_phase_adapter_status::wrong_thread;
    }
    if( active_roster_.size() != active_runtimes_.size() ) {
        return multiplayer_turn_phase_adapter_status::activation_failed;
    }

    for( std::size_t i = 0; i < active_roster_.size(); ++i ) {
        const multiplayer_turn_participant_key &pinned_key = active_roster_[i];
        if( pinned_key.player_id != participant.player_id ) {
            continue;
        }
        if( pinned_key.session_generation != participant.session_generation ) {
            return multiplayer_turn_phase_adapter_status::stale_session_generation;
        }

        const shared_ptr_fast<multiplayer_player_runtime> &pinned_runtime =
            active_runtimes_[i];
        if( pinned_runtime == nullptr ) {
            return multiplayer_turn_phase_adapter_status::runtime_not_found;
        }
        if( pinned_runtime->player_id() != participant.player_id ) {
            return multiplayer_turn_phase_adapter_status::activation_failed;
        }
        if( pinned_runtime->session_generation() != participant.session_generation ) {
            return multiplayer_turn_phase_adapter_status::stale_session_generation;
        }

        const shared_ptr_fast<multiplayer_player_runtime> registered_runtime =
            simulation_.multiplayer_players().find_by_player_id( participant.player_id );
        if( registered_runtime == nullptr ) {
            return multiplayer_turn_phase_adapter_status::runtime_not_found;
        }
        if( !same_shared_owner_and_pointer( pinned_runtime, registered_runtime ) ||
            !same_shared_owner_and_pointer( pinned_runtime->player_owner(),
                                            registered_runtime->player_owner() ) ) {
            return multiplayer_turn_phase_adapter_status::activation_failed;
        }
        if( registered_runtime->session_generation() != participant.session_generation ) {
            return multiplayer_turn_phase_adapter_status::stale_session_generation;
        }
        if( registered_runtime->status() != multiplayer_player_status::active ) {
            return multiplayer_turn_phase_adapter_status::inactive_runtime;
        }
        if( !simulation_.multiplayer_players().owns( registered_runtime ) ||
            !simulation_.multiplayer_players().owns( registered_runtime->player_owner() ) ) {
            return multiplayer_turn_phase_adapter_status::activation_failed;
        }
        return multiplayer_turn_phase_adapter_status::completed;
    }
    return multiplayer_turn_phase_adapter_status::invalid_scheduler_state;
}

multiplayer_turn_phase_adapter_status
multiplayer_multi_runtime_barrier_owner::validate_pinned_roster() const
{
    if( active_roster_.empty() || active_roster_.size() != active_runtimes_.size() ) {
        return multiplayer_turn_phase_adapter_status::activation_failed;
    }
    for( const multiplayer_turn_participant_key &participant : active_roster_ ) {
        const multiplayer_turn_phase_adapter_status status =
            validate_pinned_runtime( participant );
        if( status != multiplayer_turn_phase_adapter_status::completed ) {
            return status;
        }
    }
    return multiplayer_turn_phase_adapter_status::completed;
}

multiplayer_multi_runtime_barrier_status
multiplayer_multi_runtime_barrier_owner::latch_fault(
    const multiplayer_multi_runtime_barrier_fault_stage stage,
    const bool gameplay_side_effects_may_have_occurred ) noexcept
{
    faulted_ = true;
    if( fault_stage_ == multiplayer_multi_runtime_barrier_fault_stage::none ) {
        fault_stage_ = stage;
    }
    gameplay_side_effects_may_have_occurred_ =
        gameplay_side_effects_may_have_occurred_ || gameplay_side_effects_may_have_occurred;
    scheduler_.latch_execution_fault();
    return multiplayer_multi_runtime_barrier_status::faulted;
}

void multiplayer_multi_runtime_barrier_owner::note_adapter_status(
    const multiplayer_turn_phase_adapter_status status,
    const multiplayer_multi_runtime_barrier_fault_stage stage,
    const bool gameplay_side_effects_may_have_occurred ) noexcept
{
    if( status == multiplayer_turn_phase_adapter_status::faulted ||
        ( adapter_ && adapter_->is_faulted() ) ) {
        latch_fault( stage, gameplay_side_effects_may_have_occurred );
    }
}

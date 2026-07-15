#include "multiplayer_single_root_owner.h"

#include <limits>
#include <utility>

#include "avatar.h"
#include "game.h"
#include "multiplayer_player_runtime.h"

namespace
{

std::string active_character_id( game &simulation )
{
    return std::to_string( simulation.active_avatar().getID().get_value() );
}

bool same_binding( const multiplayer_session_binding &lhs,
                   const multiplayer_session_binding &rhs )
{
    return lhs == rhs;
}

} // namespace

std::unique_ptr<multiplayer_single_root_owner> multiplayer_single_root_owner::create(
    game &simulation, const std::size_t maximum_players,
    const clock::duration disconnect_grace, std::string &error )
{
    if( !simulation.is_simulation_thread() ) {
        error = "single-root owner must be constructed on the simulation thread";
        return nullptr;
    }
    if( maximum_players != 1 ) {
        error = "single-root owner requires players.max = 1";
        return nullptr;
    }
    if( disconnect_grace < clock::duration::zero() ) {
        error = "single-root disconnect grace cannot be negative";
        return nullptr;
    }
    std::unique_ptr<multiplayer_single_root_owner> owner(
        new multiplayer_single_root_owner( simulation, maximum_players, disconnect_grace ) );
    if( !owner->valid( error ) ) {
        return nullptr;
    }
    error.clear();
    return owner;
}

multiplayer_single_root_owner::multiplayer_single_root_owner(
    game &simulation, const std::size_t maximum_players,
    const clock::duration disconnect_grace ) :
    simulation_( simulation ),
    directory_( simulation.multiplayer_players(), maximum_players ),
    lifecycle_( simulation.active_player_runtime().player_id(),
                active_character_id( simulation ),
                simulation.active_player_runtime().session_generation(),
                simulation.active_player_runtime().status() ),
    disconnect_grace_( disconnect_grace )
{
    if( lifecycle_.snapshot().server == multiplayer_root_server_state::faulted ) {
        invalid_reason_ = "selected root lifecycle is invalid";
        return;
    }
    if( !directory_.valid( invalid_reason_ ) ) {
        return;
    }
    valid_ = true;
}

multiplayer_single_root_owner::~multiplayer_single_root_owner() = default;

bool multiplayer_single_root_owner::valid( std::string &error ) const
{
    if( !valid_ ) {
        error = invalid_reason_.empty() ? "single-root owner is invalid" : invalid_reason_;
        return false;
    }
    error.clear();
    return true;
}

multiplayer_single_root_admission_plan multiplayer_single_root_owner::plan_admission(
    const multiplayer_session_admission_request &request ) const
{
    multiplayer_single_root_admission_plan plan;
    plan.owner_ = this;
    if( !valid_ || is_faulted() || pending_admission_ ) {
        plan.status_ = is_faulted() ? multiplayer_single_root_owner_status::faulted :
                       multiplayer_single_root_owner_status::invalid;
        return plan;
    }

    plan.directory_plan_ = directory_.plan_admission( request );
    if( !plan.directory_plan_ ) {
        plan.status_ = multiplayer_single_root_owner_status::rejected;
        return plan;
    }

    multiplayer_root_admission_plan root_plan =
        lifecycle_.plan_admission( plan.directory_plan_ );
    plan.lifecycle_result_ = root_plan.result();
    if( root_plan ) {
        plan.root_plan_ = std::move( root_plan );
        plan.status_ = multiplayer_single_root_owner_status::applied;
        return plan;
    }
    if( plan.lifecycle_result_.status ==
        multiplayer_root_lifecycle_status::resume_deferred_until_boundary ) {
        plan.status_ = multiplayer_single_root_owner_status::deferred;
    } else if( plan.lifecycle_result_.status == multiplayer_root_lifecycle_status::faulted ) {
        plan.status_ = multiplayer_single_root_owner_status::faulted;
    } else {
        plan.status_ = multiplayer_single_root_owner_status::rejected;
    }
    return plan;
}

multiplayer_single_root_owner_result multiplayer_single_root_owner::execute_admission(
    const multiplayer_single_root_admission_plan &plan,
    const admission_publish_callback &publish )
{
    const multiplayer_single_root_owner_result committed = commit_admission( plan );
    if( !committed ) {
        return committed;
    }

    const multiplayer_root_admission_plan &root_plan = *plan.root_plan_;
    multiplayer_single_root_admission_publish_receipt receipt;
    if( publish ) {
        try {
            receipt = publish( root_plan.admission_id(), root_plan.binding() );
        } catch( ... ) {
            return fault_after_external_change( false );
        }
    }
    if( receipt.outcome ==
        multiplayer_single_root_admission_publish_outcome::fatal_after_external_effect ) {
        return fault_after_external_change( false );
    }
    if( receipt.admission_id != root_plan.admission_id() ||
        receipt.binding != root_plan.binding() ) {
        return fault_after_external_change( false );
    }
    return complete_admission(
               plan, receipt.outcome ==
               multiplayer_single_root_admission_publish_outcome::published );
}

multiplayer_single_root_owner_result multiplayer_single_root_owner::commit_admission(
    const multiplayer_single_root_admission_plan &plan )
{
    if( is_faulted() ) {
        return result( multiplayer_single_root_owner_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( pending_admission_ || plan.owner_ != this || !plan || !plan.root_plan_ ) {
        return result( multiplayer_single_root_owner_status::invalid );
    }
    pending_admission_ = pending_admission { plan };
    if( !directory_.commit_admission( plan.directory_plan_ ) ) {
        pending_admission_.reset();
        return result( multiplayer_single_root_owner_status::invalid );
    }
    return result( multiplayer_single_root_owner_status::applied );
}

multiplayer_single_root_owner_result multiplayer_single_root_owner::complete_admission(
    const multiplayer_single_root_admission_plan &plan, const bool published )
{
    if( is_faulted() ) {
        return result( multiplayer_single_root_owner_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( !pending_admission_ || !same_admission( pending_admission_->plan, plan ) ||
        !pending_admission_->plan.root_plan_ ) {
        return pending_admission_ ? fault_after_external_change() :
               result( multiplayer_single_root_owner_status::invalid );
    }

    const multiplayer_root_admission_plan &root_plan =
        *pending_admission_->plan.root_plan_;
    if( !published ) {
        const multiplayer_session_directory_status cleanup =
            directory_.record_admission_unpublished( root_plan.binding() );
        if( cleanup != multiplayer_session_directory_status::success ) {
            return fault_after_external_change();
        }
    }

    const multiplayer_root_lifecycle_effect scheduler_effect = published ?
            root_plan.published_effect() : root_plan.unpublished_effect();
    if( !apply_admission_scheduler_effect( root_plan, scheduler_effect ) ) {
        return fault_after_external_change();
    }

    const multiplayer_root_lifecycle_result recorded = published ?
            lifecycle_.record_admission_published( root_plan ) :
            lifecycle_.record_admission_unpublished( root_plan );
    if( !lifecycle_applied_or_duplicate( recorded ) ) {
        return fault_after_external_change();
    }
    if( adapter_ ) {
        const multiplayer_selected_root_lifecycle_snapshot state = lifecycle_.snapshot();
        if( !state.participant ) {
            return fault_after_external_change( false );
        }
        turn_participant_ = *state.participant;
        if( state.connection == multiplayer_root_connection_state::connected && state.binding ) {
            turn_binding_ = *state.binding;
        } else {
            turn_binding_.reset();
        }
    }
    pending_admission_.reset();
    return result( recorded.status == multiplayer_root_lifecycle_status::duplicate ?
                   multiplayer_single_root_owner_status::duplicate :
                   multiplayer_single_root_owner_status::applied,
                   recorded.next_effect );
}

bool multiplayer_single_root_owner::admission_committed() const
{
    return pending_admission_.has_value();
}

multiplayer_session_directory_status
multiplayer_single_root_owner::record_session_confirmed(
    const multiplayer_session_binding &binding )
{
    if( is_faulted() || pending_admission_ ) {
        return multiplayer_session_directory_status::invalid_lifecycle_state;
    }
    const multiplayer_selected_root_lifecycle_snapshot state = lifecycle_.snapshot();
    const bool exact_connected =
        state.connection == multiplayer_root_connection_state::connected && state.binding &&
        same_binding( *state.binding, binding );
    const bool exact_graceful =
        state.connection == multiplayer_root_connection_state::graceful_release_pending &&
        state.departed_binding && same_binding( *state.departed_binding, binding );
    if( !exact_connected && !exact_graceful ) {
        return multiplayer_session_directory_status::stale_connection;
    }
    const multiplayer_session_directory_status status =
        directory_.record_session_confirmed( binding );
    if( status != multiplayer_session_directory_status::success ) {
        latch_fault( false );
    }
    return status;
}

std::optional<multiplayer_session_binding>
multiplayer_single_root_owner::session_for_player( const std::string &player_id ) const
{
    return directory_.session_for_player( player_id );
}

bool multiplayer_single_root_owner::matches_connected(
    const multiplayer_session_binding &binding ) const
{
    return directory_.matches_connected( binding );
}

multiplayer_single_root_owner_result multiplayer_single_root_owner::begin_turn()
{
    if( is_faulted() ) {
        return result( multiplayer_single_root_owner_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( pending_admission_ || adapter_ || turn_invocation_started_ ||
        !lifecycle_.can_begin_turn() ) {
        return result( multiplayer_single_root_owner_status::not_ready );
    }
    if( next_shared_turn_ == std::numeric_limits<std::uint64_t>::max() ) {
        return latch_fault( false );
    }

    const multiplayer_selected_root_lifecycle_snapshot state = lifecycle_.snapshot();
    if( !state.binding || !directory_.matches_connected( *state.binding ) ) {
        return latch_fault( false );
    }
    const multiplayer_turn_participant_key participant = {
        state.player_id, state.session_generation
    };
    const std::uint64_t shared_turn = next_shared_turn_;
    if( !scheduler_.begin_turn( shared_turn, { participant } ) ) {
        return latch_fault( false );
    }
    const multiplayer_root_lifecycle_result begun = lifecycle_.begin_turn(
                *state.binding, participant, shared_turn );
    if( begun.status != multiplayer_root_lifecycle_status::applied ) {
        return fault_after_external_change();
    }

    ++next_shared_turn_;
    turn_binding_ = *state.binding;
    turn_participant_ = participant;
    claimed_world_ticket_.reset();
    current_turn_invocation_ = 0;
    turn_invocation_started_ = false;
    adapter_ = std::make_unique<multiplayer_turn_phase_adapter>( simulation_, scheduler_ );
    return result( multiplayer_single_root_owner_status::applied );
}

multiplayer_owned_turn_result multiplayer_single_root_owner::execute_owned_turn(
    multiplayer_owned_remote_turn_hooks hooks )
{
    if( pending_admission_ ) {
        latch_fault( false );
        return {};
    }
    if( !adapter_ || !turn_participant_ || is_faulted() ) {
        return {};
    }
    if( turn_invocation_started_ ) {
        latch_fault( true );
        return {};
    }
    if( next_turn_invocation_ == std::numeric_limits<std::uint64_t>::max() ) {
        latch_fault( false );
        return {};
    }
    const multiplayer_selected_root_lifecycle_snapshot state = lifecycle_.snapshot();
    current_turn_invocation_ = next_turn_invocation_++;
    turn_invocation_started_ = true;
    hooks.completion_token_ = multiplayer_owned_turn_token(
                                  this, state.shared_turn, current_turn_invocation_ );
    return simulation_.do_turn_remote_owned( hooks );
}

multiplayer_turn_phase_adapter_status
multiplayer_single_root_owner::execute_current_player( const player_callback &callback )
{
    if( is_faulted() ) {
        return multiplayer_turn_phase_adapter_status::faulted;
    }
    if( pending_admission_ || !adapter_ || !turn_participant_ ||
        !lifecycle_.can_execute_command() ) {
        return multiplayer_turn_phase_adapter_status::invalid_scheduler_state;
    }
    const multiplayer_turn_phase_adapter_status status =
        adapter_->execute_current_player( *turn_participant_, callback );
    if( adapter_->is_faulted() ) {
        note_adapter_fault();
    } else if( status != multiplayer_turn_phase_adapter_status::completed &&
               status != multiplayer_turn_phase_adapter_status::wrong_thread ) {
        latch_fault( false );
    }
    return status;
}

multiplayer_single_root_owner_result multiplayer_single_root_owner::complete_player_phase()
{
    if( is_faulted() ) {
        return result( multiplayer_single_root_owner_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( pending_admission_ || !adapter_ || !turn_participant_ ) {
        return result( multiplayer_single_root_owner_status::not_ready );
    }

    if( scheduler_.stage() == multiplayer_turn_scheduler_stage::player_actions ) {
        const std::optional<multiplayer_turn_slot> slot = scheduler_.current_slot();
        if( !slot || slot->participant != *turn_participant_ ||
            slot->state != multiplayer_turn_participant_state::awaiting_command ) {
            return latch_fault( true );
        }
        if( !scheduler_.record_player_phase_completed( *turn_participant_ ) ) {
            return latch_fault( true );
        }
    }
    if( scheduler_.stage() != multiplayer_turn_scheduler_stage::world_ready ) {
        return result( multiplayer_single_root_owner_status::not_ready );
    }

    const multiplayer_selected_root_lifecycle_snapshot state = lifecycle_.snapshot();
    if( state.barrier == multiplayer_root_barrier_state::terminal ) {
        return result( multiplayer_single_root_owner_status::duplicate,
                       multiplayer_root_lifecycle_effect::claim_world );
    }
    if( !turn_binding_ || state.connection != multiplayer_root_connection_state::connected ) {
        return latch_fault( true );
    }
    const multiplayer_root_lifecycle_result terminal =
        lifecycle_.record_connected_barrier_terminal(
            *turn_binding_, *turn_participant_, state.shared_turn );
    if( !lifecycle_applied_or_duplicate( terminal ) ) {
        return fault_after_external_change( true );
    }
    return result( terminal.status == multiplayer_root_lifecycle_status::duplicate ?
                   multiplayer_single_root_owner_status::duplicate :
                   multiplayer_single_root_owner_status::applied,
                   terminal.next_effect );
}

multiplayer_turn_phase_adapter_status multiplayer_single_root_owner::execute_world(
    const world_callback &callback )
{
    if( is_faulted() ) {
        return multiplayer_turn_phase_adapter_status::faulted;
    }
    if( pending_admission_ || !adapter_ || !turn_participant_ || claimed_world_ticket_ ) {
        return multiplayer_turn_phase_adapter_status::invalid_scheduler_state;
    }

    bool claim_recorded = false;
    const multiplayer_turn_phase_adapter_status status = adapter_->execute_claimed_world(
    [&]( const multiplayer_world_ticket & ticket ) {
        const multiplayer_selected_root_lifecycle_snapshot state = lifecycle_.snapshot();
        multiplayer_root_lifecycle_result claimed;
        if( state.departure_kind == multiplayer_root_departure_kind::none ) {
            if( !turn_binding_ ) {
                return false;
            }
            claimed = lifecycle_.record_connected_world_claimed(
                          *turn_binding_, *turn_participant_, ticket );
        } else {
            const std::optional<multiplayer_root_departure_ticket> departure =
                lifecycle_.departure_ticket();
            if( !departure ) {
                return false;
            }
            claimed = lifecycle_.record_world_claimed( *departure, ticket );
        }
        if( claimed.status != multiplayer_root_lifecycle_status::applied ) {
            return false;
        }
        claim_recorded = true;
        claimed_world_ticket_ = ticket;
        return callback && callback( ticket );
    } );

    if( adapter_->is_faulted() ) {
        note_adapter_fault();
    } else if( status != multiplayer_turn_phase_adapter_status::completed ) {
        latch_fault( claim_recorded ||
                     status == multiplayer_turn_phase_adapter_status::world_already_attempted );
    }
    return status;
}

multiplayer_single_root_owner_result
multiplayer_single_root_owner::complete_turn_after_player_end(
    const multiplayer_owned_turn_result &turn_result )
{
    if( is_faulted() ) {
        return result( multiplayer_single_root_owner_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( !adapter_ || !turn_participant_ || !turn_invocation_started_ ) {
        return result( multiplayer_single_root_owner_status::not_ready );
    }
    const multiplayer_selected_root_lifecycle_snapshot state = lifecycle_.snapshot();
    if( turn_result.status != multiplayer_owned_turn_status::completed ||
        turn_result.progress != multiplayer_owned_turn_progress::turn_completed ||
        !turn_result.player_end_completed() ||
        !turn_result.matches_owner_turn(
            this, state.shared_turn, current_turn_invocation_ ) ) {
        return latch_fault( turn_result.gameplay_side_effects_may_have_occurred() );
    }
    if( pending_admission_ || !claimed_world_ticket_ ||
        scheduler_.stage() != multiplayer_turn_scheduler_stage::idle ) {
        return latch_fault( true );
    }

    multiplayer_root_lifecycle_result completed;
    if( state.departure_kind == multiplayer_root_departure_kind::none ) {
        if( !turn_binding_ ) {
            return fault_after_external_change( true );
        }
        completed = lifecycle_.record_connected_world_completed(
                        *turn_binding_, *turn_participant_, *claimed_world_ticket_ );
    } else {
        const std::optional<multiplayer_root_departure_ticket> departure =
            lifecycle_.departure_ticket();
        if( !departure ) {
            return fault_after_external_change( true );
        }
        completed = lifecycle_.record_world_completed( *departure,
                    *claimed_world_ticket_ );
    }
    if( !lifecycle_applied_or_duplicate( completed ) ) {
        return fault_after_external_change( true );
    }

    adapter_.reset();
    turn_binding_.reset();
    turn_participant_.reset();
    claimed_world_ticket_.reset();
    current_turn_invocation_ = 0;
    turn_invocation_started_ = false;
    if( completed.next_effect ==
        multiplayer_root_lifecycle_effect::transition_runtime_offline ) {
        return transition_runtime_offline();
    }
    return result( completed.status == multiplayer_root_lifecycle_status::duplicate ?
                   multiplayer_single_root_owner_status::duplicate :
                   multiplayer_single_root_owner_status::applied,
                   completed.next_effect );
}

multiplayer_single_root_owner_result
multiplayer_single_root_owner::record_graceful_departure(
    const multiplayer_session_binding &binding, const std::uint64_t request_sequence,
    const clock::time_point now )
{
    if( is_faulted() ) {
        return result( multiplayer_single_root_owner_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( pending_admission_ ) {
        return result( multiplayer_single_root_owner_status::not_ready );
    }
    if( request_sequence == 0 ) {
        return result( multiplayer_single_root_owner_status::invalid );
    }
    const multiplayer_selected_root_lifecycle_snapshot before = lifecycle_.snapshot();
    const bool exact_duplicate =
        before.connection == multiplayer_root_connection_state::graceful_release_pending &&
        before.departure_kind == multiplayer_root_departure_kind::graceful_release &&
        before.departed_binding && same_binding( *before.departed_binding, binding );
    if( !exact_duplicate &&
        ( before.connection != multiplayer_root_connection_state::connected ||
          !before.binding || !same_binding( *before.binding, binding ) ) ) {
        return result( multiplayer_single_root_owner_status::stale );
    }
    const multiplayer_session_directory_status directory_status =
        directory_.record_graceful_release_pending( binding );
    if( directory_status == multiplayer_session_directory_status::duplicate ) {
        if( exact_duplicate ) {
            return result( before.graceful_request_sequence == request_sequence ?
                           multiplayer_single_root_owner_status::duplicate :
                           multiplayer_single_root_owner_status::stale );
        }
        return fault_after_external_change( false );
    }
    if( exact_duplicate ) {
        return fault_after_external_change( false );
    }
    if( directory_status != multiplayer_session_directory_status::success ) {
        return fault_after_external_change( false );
    }

    const multiplayer_root_lifecycle_result departure =
        lifecycle_.record_departure_accepted(
            binding, multiplayer_root_departure_kind::graceful_release, now,
            clock::duration::zero(), request_sequence );
    if( departure.status != multiplayer_root_lifecycle_status::applied ) {
        return fault_after_external_change( false );
    }
    return apply_departure_effect( departure.next_effect );
}

multiplayer_single_root_owner_result
multiplayer_single_root_owner::record_transport_disconnected(
    const multiplayer_session_binding &binding, const clock::time_point now )
{
    if( is_faulted() ) {
        return result( multiplayer_single_root_owner_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( pending_admission_ ) {
        return result( multiplayer_single_root_owner_status::not_ready );
    }
    const multiplayer_selected_root_lifecycle_snapshot before = lifecycle_.snapshot();
    if( before.connection == multiplayer_root_connection_state::disconnected &&
        before.departed_binding && same_binding( *before.departed_binding, binding ) &&
        before.departure_kind != multiplayer_root_departure_kind::none ) {
        return result( multiplayer_single_root_owner_status::duplicate );
    }
    const bool closes_graceful = before.departure_kind ==
                                 multiplayer_root_departure_kind::graceful_release &&
                                 before.departed_binding &&
                                 same_binding( *before.departed_binding, binding );
    const bool starts_transport_loss = before.connection ==
                                       multiplayer_root_connection_state::connected &&
                                       before.binding && same_binding( *before.binding, binding );
    if( !closes_graceful && !starts_transport_loss ) {
        return result( multiplayer_single_root_owner_status::stale );
    }

    const multiplayer_session_directory_status directory_status =
        directory_.record_disconnected( binding );
    if( directory_status != multiplayer_session_directory_status::success ) {
        return fault_after_external_change( false );
    }
    if( closes_graceful ) {
        const multiplayer_root_lifecycle_result closed =
            lifecycle_.record_graceful_transport_closed( binding );
        if( !lifecycle_applied_or_duplicate( closed ) ) {
            return fault_after_external_change( false );
        }
        return result( closed.status == multiplayer_root_lifecycle_status::duplicate ?
                       multiplayer_single_root_owner_status::duplicate :
                       multiplayer_single_root_owner_status::applied,
                       closed.next_effect );
    }

    const multiplayer_root_lifecycle_result departure =
        lifecycle_.record_departure_accepted(
            binding, multiplayer_root_departure_kind::transport_loss, now,
            disconnect_grace_ );
    if( departure.status != multiplayer_root_lifecycle_status::applied ) {
        return fault_after_external_change( false );
    }
    return apply_departure_effect( departure.next_effect );
}

multiplayer_single_root_owner_result
multiplayer_single_root_owner::progress_departure( const clock::time_point now )
{
    if( is_faulted() ) {
        return result( multiplayer_single_root_owner_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( pending_admission_ ) {
        return result( multiplayer_single_root_owner_status::not_ready );
    }
    const multiplayer_selected_root_lifecycle_snapshot state = lifecycle_.snapshot();
    if( state.departure_kind == multiplayer_root_departure_kind::none ) {
        return result( multiplayer_single_root_owner_status::not_ready );
    }
    if( state.barrier == multiplayer_root_barrier_state::terminal ) {
        return result( multiplayer_single_root_owner_status::applied,
                       multiplayer_root_lifecycle_effect::claim_world );
    }
    if( state.barrier == multiplayer_root_barrier_state::world_processing ) {
        return result( multiplayer_single_root_owner_status::not_ready,
                       multiplayer_root_lifecycle_effect::execute_claimed_world );
    }
    if( state.server == multiplayer_root_server_state::dormant ) {
        const std::optional<multiplayer_single_root_graceful_completion> graceful =
            pending_graceful_completion();
        return result( multiplayer_single_root_owner_status::duplicate,
                       graceful ?
                       multiplayer_root_lifecycle_effect::complete_graceful_release :
                       multiplayer_root_lifecycle_effect::none );
    }
    if( state.barrier != multiplayer_root_barrier_state::disconnected_grace ) {
        return result( multiplayer_single_root_owner_status::not_ready );
    }

    const std::optional<multiplayer_root_departure_ticket> ticket =
        lifecycle_.departure_ticket();
    if( !ticket ) {
        return fault_after_external_change( false );
    }
    std::optional<multiplayer_turn_participant_key> current;
    if( const std::optional<multiplayer_turn_slot> slot = scheduler_.current_slot() ) {
        current = slot->participant;
    }
    const multiplayer_root_lifecycle_result timeout =
        lifecycle_.evaluate_disconnect_timeout( *ticket, now, current );
    if( timeout.status == multiplayer_root_lifecycle_status::deadline_not_reached ||
        timeout.status == multiplayer_root_lifecycle_status::not_current_slot ) {
        return result( multiplayer_single_root_owner_status::not_ready,
                       multiplayer_root_lifecycle_effect::wait_for_disconnect_deadline );
    }
    if( timeout.status == multiplayer_root_lifecycle_status::duplicate ) {
        return result( multiplayer_single_root_owner_status::duplicate );
    }
    if( timeout.status != multiplayer_root_lifecycle_status::applied ) {
        return fault_after_external_change( false );
    }
    return apply_departure_effect( timeout.next_effect );
}

std::optional<multiplayer_single_root_graceful_completion>
multiplayer_single_root_owner::pending_graceful_completion() const
{
    if( pending_admission_ || is_faulted() ) {
        return std::nullopt;
    }
    const multiplayer_selected_root_lifecycle_snapshot state = lifecycle_.snapshot();
    if( state.server != multiplayer_root_server_state::dormant ||
        state.connection != multiplayer_root_connection_state::graceful_release_pending ||
        state.departure_kind != multiplayer_root_departure_kind::graceful_release ||
        !state.departed_binding || !state.graceful_request_sequence ||
        state.graceful_completion_queued ) {
        return std::nullopt;
    }
    return multiplayer_single_root_graceful_completion {
        *state.departed_binding, *state.graceful_request_sequence
    };
}

multiplayer_single_root_owner_result
multiplayer_single_root_owner::record_graceful_completion_queued(
    const multiplayer_single_root_graceful_completion &completion )
{
    if( is_faulted() ) {
        return result( multiplayer_single_root_owner_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( pending_admission_ ) {
        return fault_after_external_change( false );
    }
    const multiplayer_root_lifecycle_result recorded =
        lifecycle_.record_graceful_completion_queued(
            completion.binding, completion.request_sequence );
    if( !lifecycle_applied_or_duplicate( recorded ) ) {
        return fault_after_external_change( false );
    }
    return result( recorded.status == multiplayer_root_lifecycle_status::duplicate ?
                   multiplayer_single_root_owner_status::duplicate :
                   multiplayer_single_root_owner_status::applied );
}

multiplayer_single_root_owner_result multiplayer_single_root_owner::latch_fault(
    const bool canonical_state_uncertain )
{
    // Session/directory transactions are ephemeral to the canonical world save.
    // An owned game turn is the boundary that makes a fatal save unsafe unless
    // the caller separately proves that canonical state is uncertain.
    const bool safe_boundary_before_fault = !adapter_;
    fault_save_allowed_ = fault_save_allowed_ ||
                          ( !canonical_state_uncertain && safe_boundary_before_fault );
    fault_open_turn_ = fault_open_turn_ ||
                       ( adapter_ && !canonical_state_uncertain );
    canonical_state_uncertain_ = canonical_state_uncertain_ || canonical_state_uncertain;
    scheduler_.latch_execution_fault();
    lifecycle_.latch_fault();
    return result( multiplayer_single_root_owner_status::faulted,
                   multiplayer_root_lifecycle_effect::fatal_shutdown );
}

multiplayer_selected_root_lifecycle_snapshot
multiplayer_single_root_owner::snapshot() const
{
    return lifecycle_.snapshot();
}

std::optional<multiplayer_turn_slot> multiplayer_single_root_owner::current_slot() const
{
    return scheduler_.current_slot();
}

bool multiplayer_single_root_owner::can_begin_turn() const
{
    return valid_ && !pending_admission_ && !adapter_ && !is_faulted() &&
           lifecycle_.can_begin_turn();
}

bool multiplayer_single_root_owner::can_execute_command() const
{
    if( !valid_ || pending_admission_ || !adapter_ || !turn_participant_ || is_faulted() ||
        !lifecycle_.can_execute_command() ) {
        return false;
    }
    const std::optional<multiplayer_turn_slot> slot = scheduler_.current_slot();
    return slot && slot->participant == *turn_participant_ &&
           slot->state == multiplayer_turn_participant_state::awaiting_command;
}

bool multiplayer_single_root_owner::is_dormant() const
{
    return lifecycle_.snapshot().server == multiplayer_root_server_state::dormant;
}

bool multiplayer_single_root_owner::is_faulted() const
{
    return !valid_ || scheduler_.is_faulted() ||
           lifecycle_.snapshot().server == multiplayer_root_server_state::faulted ||
           ( adapter_ && adapter_->is_faulted() );
}

multiplayer_single_root_save_disposition
multiplayer_single_root_owner::save_disposition() const
{
    if( canonical_state_uncertain_ ) {
        return multiplayer_single_root_save_disposition::canonical_state_uncertain;
    }
    if( is_faulted() ) {
        if( fault_save_allowed_ ) {
            return multiplayer_single_root_save_disposition::allowed;
        }
        if( fault_open_turn_ || adapter_ ) {
            return multiplayer_single_root_save_disposition::open_turn;
        }
        return multiplayer_single_root_save_disposition::faulted;
    }
    if( pending_admission_ || adapter_ || !lifecycle_.can_save_or_shutdown() ) {
        return multiplayer_single_root_save_disposition::open_turn;
    }
    return multiplayer_single_root_save_disposition::allowed;
}

multiplayer_single_root_owner_result multiplayer_single_root_owner::result(
    const multiplayer_single_root_owner_status status,
    const multiplayer_root_lifecycle_effect effect ) const
{
    return { status, effect };
}

bool multiplayer_single_root_owner::same_admission(
    const multiplayer_single_root_admission_plan &lhs,
    const multiplayer_single_root_admission_plan &rhs ) const
{
    if( lhs.owner_ != this || rhs.owner_ != this || lhs.status_ != rhs.status_ ||
        !lhs.root_plan_ || !rhs.root_plan_ ) {
        return false;
    }
    const multiplayer_session_admission_plan &left = lhs.directory_plan_;
    const multiplayer_session_admission_plan &right = rhs.directory_plan_;
    return left.request.admission_id == right.request.admission_id &&
           left.request.connection == right.request.connection &&
           left.request.session == right.request.session &&
           left.request.player_id == right.request.player_id &&
           left.request.character_id == right.request.character_id &&
           left.committed_session_generation == right.committed_session_generation &&
           left.directory_version == right.directory_version &&
           lhs.root_plan_->binding() == rhs.root_plan_->binding() &&
           lhs.root_plan_->admission_id() == rhs.root_plan_->admission_id();
}

bool multiplayer_single_root_owner::lifecycle_applied_or_duplicate(
    const multiplayer_root_lifecycle_result &value ) const
{
    return value.status == multiplayer_root_lifecycle_status::applied ||
           value.status == multiplayer_root_lifecycle_status::duplicate;
}

bool multiplayer_single_root_owner::apply_admission_scheduler_effect(
    const multiplayer_root_admission_plan &plan,
    const multiplayer_root_lifecycle_effect effect )
{
    switch( effect ) {
        case multiplayer_root_lifecycle_effect::none:
            return true;
        case multiplayer_root_lifecycle_effect::resume_barrier_plus_one:
            return scheduler_.resume_barrier_participant(
                       plan.old_participant(),
                       plan.committed_participant().session_generation );
        case multiplayer_root_lifecycle_effect::rebind_replayed_barrier_generation:
            return scheduler_.rebind_replayed_barrier_participant(
                       plan.old_participant() );
        case multiplayer_root_lifecycle_effect::repair_disconnected_barrier_generation:
            return scheduler_.repair_disconnected_barrier_generation(
                       plan.old_participant(),
                       plan.committed_participant().session_generation );
        default:
            return false;
    }
}

multiplayer_single_root_owner_result
multiplayer_single_root_owner::apply_departure_effect(
    const multiplayer_root_lifecycle_effect effect )
{
    switch( effect ) {
        case multiplayer_root_lifecycle_effect::none:
        case multiplayer_root_lifecycle_effect::wait_for_disconnect_deadline:
        case multiplayer_root_lifecycle_effect::claim_world:
        case multiplayer_root_lifecycle_effect::execute_claimed_world:
        case multiplayer_root_lifecycle_effect::complete_graceful_release:
        case multiplayer_root_lifecycle_effect::begin_next_turn:
            return result( multiplayer_single_root_owner_status::applied, effect );
        case multiplayer_root_lifecycle_effect::mark_barrier_disconnected: {
            const std::optional<multiplayer_root_departure_ticket> ticket =
                lifecycle_.departure_ticket();
            if( !ticket || !scheduler_.mark_barrier_disconnected( ticket->participant ) ) {
                return fault_after_external_change();
            }
            const multiplayer_root_lifecycle_result recorded =
                lifecycle_.record_barrier_disconnected( *ticket );
            if( !lifecycle_applied_or_duplicate( recorded ) ) {
                return fault_after_external_change();
            }
            return apply_departure_effect( recorded.next_effect );
        }
        case multiplayer_root_lifecycle_effect::request_automatic_wait: {
            const std::optional<multiplayer_root_departure_ticket> ticket =
                lifecycle_.departure_ticket();
            if( !ticket || !scheduler_.apply_disconnect_timeout(
                    ticket->participant,
                    multiplayer_disconnect_timeout_policy::automatic_wait ) ) {
                return fault_after_external_change();
            }
            const multiplayer_root_lifecycle_result pending =
                lifecycle_.record_forced_wait_pending( *ticket );
            if( !lifecycle_applied_or_duplicate( pending ) ) {
                return fault_after_external_change();
            }
            return apply_departure_effect( pending.next_effect );
        }
        case multiplayer_root_lifecycle_effect::execute_authoritative_wait: {
            const std::optional<multiplayer_root_departure_ticket> ticket =
                lifecycle_.departure_ticket();
            if( !ticket || !adapter_ ) {
                return fault_after_external_change();
            }
            const multiplayer_turn_phase_adapter_status waited =
                adapter_->execute_authoritative_wait( ticket->participant );
            if( waited != multiplayer_turn_phase_adapter_status::completed ) {
                note_adapter_fault();
                return result( multiplayer_single_root_owner_status::faulted,
                               multiplayer_root_lifecycle_effect::fatal_shutdown );
            }
            const multiplayer_root_lifecycle_result terminal =
                lifecycle_.record_forced_wait_terminal( *ticket );
            if( !lifecycle_applied_or_duplicate( terminal ) ) {
                return fault_after_external_change( true );
            }
            return apply_departure_effect( terminal.next_effect );
        }
        case multiplayer_root_lifecycle_effect::transition_runtime_offline:
            return transition_runtime_offline();
        case multiplayer_root_lifecycle_effect::fatal_shutdown:
            return latch_fault( adapter_ != nullptr );
        case multiplayer_root_lifecycle_effect::resume_barrier_plus_one:
        case multiplayer_root_lifecycle_effect::rebind_replayed_barrier_generation:
        case multiplayer_root_lifecycle_effect::repair_disconnected_barrier_generation:
            return fault_after_external_change();
    }
    return fault_after_external_change();
}

multiplayer_single_root_owner_result
multiplayer_single_root_owner::transition_runtime_offline()
{
    const std::optional<multiplayer_root_departure_ticket> ticket =
        lifecycle_.departure_ticket();
    if( !ticket ) {
        return fault_after_external_change();
    }
    const multiplayer_session_directory_status offline =
        directory_.record_runtime_offline( lifecycle_.runtime_key() );
    if( offline != multiplayer_session_directory_status::success &&
        offline != multiplayer_session_directory_status::duplicate ) {
        return fault_after_external_change();
    }
    const multiplayer_root_lifecycle_result recorded =
        lifecycle_.record_runtime_offline( *ticket );
    if( !lifecycle_applied_or_duplicate( recorded ) ) {
        return fault_after_external_change();
    }
    return result( recorded.status == multiplayer_root_lifecycle_status::duplicate ?
                   multiplayer_single_root_owner_status::duplicate :
                   multiplayer_single_root_owner_status::applied,
                   recorded.next_effect );
}

multiplayer_single_root_owner_result
multiplayer_single_root_owner::fault_after_external_change(
    const bool canonical_state_uncertain )
{
    return latch_fault( canonical_state_uncertain );
}

void multiplayer_single_root_owner::note_adapter_fault()
{
    latch_fault( true );
}

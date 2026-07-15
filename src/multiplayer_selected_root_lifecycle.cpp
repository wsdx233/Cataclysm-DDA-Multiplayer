#include "multiplayer_selected_root_lifecycle.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "multiplayer_session_generation.h"

namespace
{

bool same_binding( const multiplayer_session_binding &lhs,
                   const multiplayer_session_binding &rhs )
{
    return lhs.connection == rhs.connection && lhs.session == rhs.session &&
           lhs.player_id == rhs.player_id && lhs.character_id == rhs.character_id &&
           lhs.session_generation == rhs.session_generation;
}

bool same_participant( const multiplayer_turn_participant_key &lhs,
                       const multiplayer_turn_participant_key &rhs )
{
    return lhs == rhs;
}

bool session_is_empty( const multiplayer_session_id &session )
{
    return std::all_of( session.begin(), session.end(), []( const std::uint8_t byte ) {
        return byte == 0;
    } );
}

bool barrier_at_or_after( const multiplayer_root_barrier_state actual,
                          const multiplayer_root_barrier_state expected )
{
    return static_cast<std::uint8_t>( actual ) >= static_cast<std::uint8_t>( expected );
}

multiplayer_session_binding binding_from_plan(
    const multiplayer_session_admission_plan &plan )
{
    return {
        plan.request.connection,
        plan.request.session,
        plan.request.player_id,
        plan.request.character_id,
        plan.committed_session_generation
    };
}

} // namespace

multiplayer_selected_root_lifecycle::multiplayer_selected_root_lifecycle(
    const multiplayer_player_id &player_id, std::string character_id,
    const std::uint64_t session_generation,
    const multiplayer_player_status runtime_status ) :
    player_id_( player_id ),
    character_id_( std::move( character_id ) ),
    session_generation_( session_generation ),
    verified_runtime_status_( runtime_status )
{
    const bool usable_runtime = runtime_status == multiplayer_player_status::active ||
                                runtime_status == multiplayer_player_status::offline;
    if( !player_id_.is_valid() || character_id_.empty() ||
        !multiplayer_is_valid_session_generation( session_generation_ ) || !usable_runtime ) {
        server_ = multiplayer_root_server_state::faulted;
        return;
    }
    server_ = runtime_status == multiplayer_player_status::active ?
              multiplayer_root_server_state::running : multiplayer_root_server_state::dormant;
    connection_ = runtime_status == multiplayer_player_status::active ?
                  multiplayer_root_connection_state::unbound :
                  multiplayer_root_connection_state::disconnected;
}

multiplayer_root_lifecycle_result multiplayer_selected_root_lifecycle::begin_turn(
    const multiplayer_session_binding &binding,
    const multiplayer_turn_participant_key &participant,
    const std::uint64_t shared_turn )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( barrier_ == multiplayer_root_barrier_state::awaiting_command && binding_ &&
        participant_ && same_binding( *binding_, binding ) &&
        same_participant( *participant_, participant ) && shared_turn_ == shared_turn ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( !can_begin_turn() || !binding_ || !same_binding( *binding_, binding ) ||
        shared_turn == 0 || shared_turn <= last_started_shared_turn_ ||
        participant.player_id != player_id_ ||
        participant.session_generation != session_generation_ ||
        binding.player_id != player_id_.str() || binding.character_id != character_id_ ||
        binding.session_generation != session_generation_ ) {
        return result( multiplayer_root_lifecycle_status::invalid_transition );
    }

    participant_ = participant;
    shared_turn_ = shared_turn;
    last_started_shared_turn_ = shared_turn;
    barrier_ = multiplayer_root_barrier_state::awaiting_command;
    last_completed_world_ticket_.reset();
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied );
}

multiplayer_root_lifecycle_result
multiplayer_selected_root_lifecycle::record_connected_barrier_terminal(
    const multiplayer_session_binding &binding,
    const multiplayer_turn_participant_key &participant,
    const std::uint64_t shared_turn )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( barrier_ == multiplayer_root_barrier_state::terminal && binding_ && participant_ &&
        same_binding( *binding_, binding ) && same_participant( *participant_, participant ) ) {
        return shared_turn_ == shared_turn ?
               result( multiplayer_root_lifecycle_status::duplicate ) :
               result( multiplayer_root_lifecycle_status::invalid_transition );
    }
    if( server_ != multiplayer_root_server_state::running ||
        connection_ != multiplayer_root_connection_state::connected || !binding_ ||
        !same_binding( *binding_, binding ) || !participant_ ||
        !same_participant( *participant_, participant ) ||
        barrier_ != multiplayer_root_barrier_state::awaiting_command ||
        shared_turn == 0 || shared_turn_ != shared_turn ||
        verified_runtime_status_ != multiplayer_player_status::active ||
        participant.session_generation != session_generation_ ) {
        return result( multiplayer_root_lifecycle_status::invalid_transition );
    }

    barrier_ = multiplayer_root_barrier_state::terminal;
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied,
                   multiplayer_root_lifecycle_effect::claim_world );
}

multiplayer_root_lifecycle_result
multiplayer_selected_root_lifecycle::record_connected_world_claimed(
    const multiplayer_session_binding &binding,
    const multiplayer_turn_participant_key &participant,
    const multiplayer_world_ticket &world_ticket )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( barrier_ == multiplayer_root_barrier_state::world_processing && binding_ &&
        participant_ && world_ticket_ && same_binding( *binding_, binding ) &&
        same_participant( *participant_, participant ) && *world_ticket_ == world_ticket ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( server_ != multiplayer_root_server_state::running ||
        connection_ != multiplayer_root_connection_state::connected || !binding_ ||
        !same_binding( *binding_, binding ) || !participant_ ||
        !same_participant( *participant_, participant ) ||
        barrier_ != multiplayer_root_barrier_state::terminal ||
        world_ticket.shared_turn == 0 || world_ticket.shared_turn != shared_turn_ ||
        verified_runtime_status_ != multiplayer_player_status::active ) {
        return result( multiplayer_root_lifecycle_status::invalid_transition );
    }

    world_ticket_ = world_ticket;
    barrier_ = multiplayer_root_barrier_state::world_processing;
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied,
                   multiplayer_root_lifecycle_effect::execute_claimed_world );
}

multiplayer_root_lifecycle_result
multiplayer_selected_root_lifecycle::record_connected_world_completed(
    const multiplayer_session_binding &binding,
    const multiplayer_turn_participant_key &participant,
    const multiplayer_world_ticket &world_ticket )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( barrier_ == multiplayer_root_barrier_state::none && binding_ &&
        last_completed_world_ticket_ && same_binding( *binding_, binding ) &&
        *last_completed_world_ticket_ == world_ticket ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( server_ != multiplayer_root_server_state::running ||
        connection_ != multiplayer_root_connection_state::connected || !binding_ ||
        !same_binding( *binding_, binding ) || !participant_ ||
        !same_participant( *participant_, participant ) ||
        barrier_ != multiplayer_root_barrier_state::world_processing || !world_ticket_ ||
        !( *world_ticket_ == world_ticket ) ||
        verified_runtime_status_ != multiplayer_player_status::active ) {
        return result( multiplayer_root_lifecycle_status::invalid_transition );
    }

    last_completed_world_ticket_ = world_ticket;
    world_ticket_.reset();
    barrier_ = multiplayer_root_barrier_state::none;
    participant_.reset();
    shared_turn_ = 0;
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied,
                   multiplayer_root_lifecycle_effect::begin_next_turn );
}

multiplayer_root_lifecycle_result
multiplayer_selected_root_lifecycle::record_departure_accepted(
    const multiplayer_session_binding &binding,
    const multiplayer_root_departure_kind kind, const clock::time_point now,
    const clock::duration disconnect_grace,
    const std::optional<std::uint64_t> graceful_request_sequence )
{
    return record_departure( binding, kind, now, disconnect_grace,
                             graceful_request_sequence );
}

multiplayer_root_lifecycle_result multiplayer_selected_root_lifecycle::record_departure(
    const multiplayer_session_binding &binding,
    const multiplayer_root_departure_kind kind, const clock::time_point now,
    const clock::duration disconnect_grace,
    const std::optional<std::uint64_t> graceful_request_sequence )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( departed_binding_ && same_binding( *departed_binding_, binding ) &&
        departure_kind_ == kind &&
        ( kind != multiplayer_root_departure_kind::graceful_release ||
          graceful_request_sequence_ == graceful_request_sequence ) ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( !binding_ || !same_binding( *binding_, binding ) ) {
        return result( multiplayer_root_lifecycle_status::stale_binding );
    }
    const bool graceful = kind == multiplayer_root_departure_kind::graceful_release;
    const bool at_turn_boundary = barrier_ == multiplayer_root_barrier_state::none &&
                                  !participant_ && shared_turn_ == 0;
    const bool has_current_participant = participant_ &&
                                         participant_->player_id == player_id_ &&
                                         participant_->session_generation ==
                                         binding.session_generation;
    const bool can_depart_during_turn = has_current_participant &&
                                        ( barrier_ ==
                                          multiplayer_root_barrier_state::awaiting_command ||
                                          barrier_ == multiplayer_root_barrier_state::terminal ||
                                          barrier_ ==
                                          multiplayer_root_barrier_state::world_processing ||
                                          barrier_ == multiplayer_root_barrier_state::world_completed );
    if( kind == multiplayer_root_departure_kind::none ||
        disconnect_grace < clock::duration::zero() ||
        ( graceful && ( !graceful_request_sequence || *graceful_request_sequence == 0 ) ) ||
        ( !graceful && graceful_request_sequence ) ||
        server_ != multiplayer_root_server_state::running ||
        connection_ != multiplayer_root_connection_state::connected ||
        ( !at_turn_boundary && !can_depart_during_turn ) ||
        verified_runtime_status_ != multiplayer_player_status::active ||
        session_generation_ != binding.session_generation ) {
        return result( multiplayer_root_lifecycle_status::invalid_transition );
    }
    if( departure_epoch_ == std::numeric_limits<std::uint64_t>::max() ) {
        server_ = multiplayer_root_server_state::faulted;
        advance_version();
        return result( multiplayer_root_lifecycle_status::external_state_mismatch,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }

    departed_binding_ = binding;
    binding_.reset();
    departure_kind_ = kind;
    ++departure_epoch_;
    graceful_request_sequence_ = graceful_request_sequence;
    graceful_completion_queued_ = false;
    forced_wait_requested_ = false;
    connection_ = graceful ? multiplayer_root_connection_state::graceful_release_pending :
                  multiplayer_root_connection_state::disconnected;
    disconnect_deadline_ = graceful || barrier_ !=
                           multiplayer_root_barrier_state::awaiting_command ?
                           now : now + disconnect_grace;
    last_admission_outcome_.reset();
    if( at_turn_boundary ) {
        participant_ = multiplayer_turn_participant_key { player_id_, session_generation_ };
    }
    advance_version();
    departure_lifecycle_version_ = lifecycle_version_;
    refresh_departure_ticket();
    multiplayer_root_lifecycle_effect next_effect =
        multiplayer_root_lifecycle_effect::none;
    if( barrier_ == multiplayer_root_barrier_state::awaiting_command ) {
        next_effect = multiplayer_root_lifecycle_effect::mark_barrier_disconnected;
    } else if( barrier_ == multiplayer_root_barrier_state::terminal ) {
        next_effect = multiplayer_root_lifecycle_effect::claim_world;
    } else if( barrier_ == multiplayer_root_barrier_state::world_completed ||
               barrier_ == multiplayer_root_barrier_state::none ) {
        next_effect = multiplayer_root_lifecycle_effect::transition_runtime_offline;
    }
    return result( multiplayer_root_lifecycle_status::applied, next_effect );
}

multiplayer_root_lifecycle_result
multiplayer_selected_root_lifecycle::record_barrier_disconnected(
    const multiplayer_root_departure_ticket &ticket )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( ticket_matches_departure( ticket ) &&
        ( barrier_ == multiplayer_root_barrier_state::disconnected_grace ||
          ( forced_wait_requested_ &&
            barrier_at_or_after( barrier_,
                                 multiplayer_root_barrier_state::forced_wait_pending ) ) ) ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( !ticket_matches_current( ticket ) ) {
        return result( multiplayer_root_lifecycle_status::stale_binding );
    }
    if( barrier_ != multiplayer_root_barrier_state::awaiting_command ||
        connection_ == multiplayer_root_connection_state::connected ||
        verified_runtime_status_ != multiplayer_player_status::active ) {
        return result( multiplayer_root_lifecycle_status::invalid_transition );
    }

    barrier_ = multiplayer_root_barrier_state::disconnected_grace;
    if( departure_kind_ == multiplayer_root_departure_kind::graceful_release ) {
        forced_wait_requested_ = true;
    }
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied,
                   forced_wait_requested_ ?
                   multiplayer_root_lifecycle_effect::request_automatic_wait :
                   multiplayer_root_lifecycle_effect::wait_for_disconnect_deadline );
}

multiplayer_root_lifecycle_result
multiplayer_selected_root_lifecycle::evaluate_disconnect_timeout(
    const multiplayer_root_departure_ticket &ticket, const clock::time_point now,
    const std::optional<multiplayer_turn_participant_key> &current_slot )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( barrier_ != multiplayer_root_barrier_state::disconnected_grace ) {
        if( forced_wait_requested_ && ticket_matches_departure( ticket ) &&
            barrier_at_or_after( barrier_, multiplayer_root_barrier_state::forced_wait_pending ) ) {
            return result( multiplayer_root_lifecycle_status::duplicate );
        }
        return result( multiplayer_root_lifecycle_status::stale_binding );
    }
    if( forced_wait_requested_ && ticket_matches_departure( ticket ) ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( !ticket_matches_current( ticket ) ) {
        return result( multiplayer_root_lifecycle_status::stale_binding );
    }
    if( !disconnect_deadline_ || now < *disconnect_deadline_ ) {
        return result( multiplayer_root_lifecycle_status::deadline_not_reached );
    }
    if( !current_slot || current_slot->player_id != ticket.participant.player_id ) {
        return result( multiplayer_root_lifecycle_status::not_current_slot );
    }
    if( current_slot->session_generation != ticket.participant.session_generation ) {
        return result( multiplayer_root_lifecycle_status::stale_generation );
    }

    forced_wait_requested_ = true;
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied,
                   multiplayer_root_lifecycle_effect::request_automatic_wait );
}

multiplayer_root_lifecycle_result
multiplayer_selected_root_lifecycle::record_forced_wait_pending(
    const multiplayer_root_departure_ticket &ticket )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( forced_wait_requested_ && ticket_matches_departure( ticket ) &&
        barrier_at_or_after( barrier_, multiplayer_root_barrier_state::forced_wait_pending ) ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( !ticket_matches_current( ticket ) ) {
        return result( multiplayer_root_lifecycle_status::stale_binding );
    }
    if( barrier_ != multiplayer_root_barrier_state::disconnected_grace ||
        !forced_wait_requested_ || verified_runtime_status_ != multiplayer_player_status::active ) {
        return result( multiplayer_root_lifecycle_status::invalid_transition );
    }

    barrier_ = multiplayer_root_barrier_state::forced_wait_pending;
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied,
                   multiplayer_root_lifecycle_effect::execute_authoritative_wait );
}

multiplayer_root_lifecycle_result
multiplayer_selected_root_lifecycle::record_forced_wait_terminal(
    const multiplayer_root_departure_ticket &ticket )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( forced_wait_requested_ && ticket_matches_departure( ticket ) &&
        barrier_at_or_after( barrier_, multiplayer_root_barrier_state::terminal ) ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( !ticket_matches_current( ticket ) ) {
        return result( multiplayer_root_lifecycle_status::stale_binding );
    }
    if( barrier_ != multiplayer_root_barrier_state::forced_wait_pending ||
        verified_runtime_status_ != multiplayer_player_status::active ) {
        return result( multiplayer_root_lifecycle_status::invalid_transition );
    }

    barrier_ = multiplayer_root_barrier_state::terminal;
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied,
                   multiplayer_root_lifecycle_effect::claim_world );
}

multiplayer_root_lifecycle_result multiplayer_selected_root_lifecycle::record_world_claimed(
    const multiplayer_root_departure_ticket &ticket,
    const multiplayer_world_ticket &world_ticket )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( ticket_matches_departure( ticket ) && world_ticket_ &&
        *world_ticket_ == world_ticket &&
        barrier_ == multiplayer_root_barrier_state::world_processing ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( !ticket_matches_current( ticket ) ) {
        return result( multiplayer_root_lifecycle_status::stale_binding );
    }
    if( barrier_ != multiplayer_root_barrier_state::terminal ||
        world_ticket.shared_turn == 0 || world_ticket.shared_turn != shared_turn_ ||
        verified_runtime_status_ != multiplayer_player_status::active ) {
        return result( multiplayer_root_lifecycle_status::invalid_transition );
    }

    world_ticket_ = world_ticket;
    barrier_ = multiplayer_root_barrier_state::world_processing;
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied,
                   multiplayer_root_lifecycle_effect::execute_claimed_world );
}

multiplayer_root_lifecycle_result multiplayer_selected_root_lifecycle::record_world_completed(
    const multiplayer_root_departure_ticket &ticket,
    const multiplayer_world_ticket &world_ticket )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( ticket_matches_departure( ticket ) && last_completed_world_ticket_ &&
        *last_completed_world_ticket_ == world_ticket &&
        ( barrier_ == multiplayer_root_barrier_state::world_completed ||
          ( barrier_ == multiplayer_root_barrier_state::none &&
            server_ == multiplayer_root_server_state::dormant ) ) ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( !ticket_matches_current( ticket ) ) {
        return result( multiplayer_root_lifecycle_status::stale_binding );
    }
    if( barrier_ != multiplayer_root_barrier_state::world_processing || !world_ticket_ ||
        !( *world_ticket_ == world_ticket ) ) {
        return result( multiplayer_root_lifecycle_status::invalid_transition );
    }

    last_completed_world_ticket_ = world_ticket;
    world_ticket_.reset();
    barrier_ = multiplayer_root_barrier_state::world_completed;
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied,
                   multiplayer_root_lifecycle_effect::transition_runtime_offline );
}

multiplayer_root_lifecycle_result multiplayer_selected_root_lifecycle::record_runtime_offline(
    const multiplayer_root_departure_ticket &ticket )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( ticket_matches_departure( ticket ) &&
        server_ == multiplayer_root_server_state::dormant &&
        barrier_ == multiplayer_root_barrier_state::none &&
        verified_runtime_status_ == multiplayer_player_status::offline ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( !ticket_matches_current( ticket ) ) {
        return result( multiplayer_root_lifecycle_status::stale_binding );
    }
    const bool at_completed_world =
        barrier_ == multiplayer_root_barrier_state::world_completed;
    const bool at_empty_turn_boundary =
        barrier_ == multiplayer_root_barrier_state::none && shared_turn_ == 0;
    if( ( !at_completed_world && !at_empty_turn_boundary ) || binding_ ||
        connection_ == multiplayer_root_connection_state::connected ||
        verified_runtime_status_ != multiplayer_player_status::active ) {
        return result( multiplayer_root_lifecycle_status::invalid_transition );
    }

    verified_runtime_status_ = multiplayer_player_status::offline;
    server_ = multiplayer_root_server_state::dormant;
    barrier_ = multiplayer_root_barrier_state::none;
    participant_.reset();
    world_ticket_.reset();
    shared_turn_ = 0;
    forced_wait_requested_ = false;
    disconnect_deadline_.reset();
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied,
                   departure_kind_ == multiplayer_root_departure_kind::graceful_release &&
                   connection_ == multiplayer_root_connection_state::graceful_release_pending ?
                   multiplayer_root_lifecycle_effect::complete_graceful_release :
                   multiplayer_root_lifecycle_effect::none );
}

multiplayer_root_lifecycle_result
multiplayer_selected_root_lifecycle::record_graceful_completion_queued(
    const multiplayer_session_binding &binding, const std::uint64_t request_sequence )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( graceful_completion_queued_ && departed_binding_ &&
        same_binding( *departed_binding_, binding ) &&
        graceful_request_sequence_ == request_sequence ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( !departed_binding_ || !same_binding( *departed_binding_, binding ) ) {
        return result( multiplayer_root_lifecycle_status::stale_binding );
    }
    if( request_sequence == 0 || graceful_request_sequence_ != request_sequence ||
        departure_kind_ != multiplayer_root_departure_kind::graceful_release ||
        connection_ != multiplayer_root_connection_state::graceful_release_pending ||
        server_ != multiplayer_root_server_state::dormant ||
        barrier_ != multiplayer_root_barrier_state::none ||
        verified_runtime_status_ != multiplayer_player_status::offline ) {
        return result( multiplayer_root_lifecycle_status::invalid_transition );
    }

    graceful_completion_queued_ = true;
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied );
}

multiplayer_root_lifecycle_result
multiplayer_selected_root_lifecycle::record_graceful_transport_closed(
    const multiplayer_session_binding &binding )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( connection_ == multiplayer_root_connection_state::disconnected && departed_binding_ &&
        same_binding( *departed_binding_, binding ) &&
        departure_kind_ == multiplayer_root_departure_kind::graceful_release ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( !departed_binding_ || !same_binding( *departed_binding_, binding ) ) {
        return result( multiplayer_root_lifecycle_status::stale_binding );
    }
    if( departure_kind_ != multiplayer_root_departure_kind::graceful_release ||
        connection_ != multiplayer_root_connection_state::graceful_release_pending ) {
        return result( multiplayer_root_lifecycle_status::invalid_transition );
    }

    connection_ = multiplayer_root_connection_state::disconnected;
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied );
}

multiplayer_root_admission_plan multiplayer_selected_root_lifecycle::plan_admission(
    const multiplayer_session_admission_plan &directory_plan ) const
{
    multiplayer_root_admission_plan plan;
    plan.owner_ = this;
    if( server_ == multiplayer_root_server_state::faulted ) {
        plan.result_ = result( multiplayer_root_lifecycle_status::faulted,
                               multiplayer_root_lifecycle_effect::fatal_shutdown );
        return plan;
    }
    if( !directory_plan_is_well_formed( directory_plan ) ) {
        plan.result_ = result( multiplayer_root_lifecycle_status::invalid_transition );
        return plan;
    }
    if( forced_wait_requested_ ||
        barrier_ == multiplayer_root_barrier_state::forced_wait_pending ||
        barrier_ == multiplayer_root_barrier_state::terminal ||
        barrier_ == multiplayer_root_barrier_state::world_processing ||
        barrier_ == multiplayer_root_barrier_state::world_completed ) {
        plan.result_ = result(
                           multiplayer_root_lifecycle_status::resume_deferred_until_boundary );
        return plan;
    }

    plan.lifecycle_version_ = lifecycle_version_;
    plan.committed_participant_ = {
        player_id_, directory_plan.committed_session_generation
    };
    plan.binding_ = binding_from_plan( directory_plan );
    plan.admission_id_ = directory_plan.request.admission_id;
    plan.admission_kind_ = directory_plan.request.kind;
    plan.expected_generation_ = directory_plan.request.expected_session_generation;
    plan.directory_version_ = directory_plan.directory_version;
    plan.advances_generation_ = directory_plan.advances_generation;
    plan.replays_committed_generation_ = directory_plan.replays_committed_generation;

    if( server_ == multiplayer_root_server_state::running &&
        barrier_ == multiplayer_root_barrier_state::none &&
        connection_ == multiplayer_root_connection_state::unbound &&
        verified_runtime_status_ == multiplayer_player_status::active && !binding_ ) {
        const bool adopts_current = !directory_plan.advances_generation &&
                                    !directory_plan.replays_committed_generation &&
                                    directory_plan.committed_session_generation == session_generation_;
        const bool advances_current = directory_plan.advances_generation &&
                                      multiplayer_is_next_session_generation(
                                          session_generation_,
                                          directory_plan.committed_session_generation );
        if( !adopts_current && !advances_current ) {
            plan.result_ = result( multiplayer_root_lifecycle_status::stale_generation );
            return plan;
        }
        plan.origin_ = multiplayer_root_admission_origin::unbound_active;
        plan.barrier_mode_ = multiplayer_root_barrier_resume_mode::no_open_barrier;
        plan.result_ = result( multiplayer_root_lifecycle_status::applied );
        return plan;
    }

    if( server_ == multiplayer_root_server_state::running &&
        barrier_ == multiplayer_root_barrier_state::disconnected_grace &&
        connection_ == multiplayer_root_connection_state::disconnected &&
        departure_kind_ == multiplayer_root_departure_kind::transport_loss &&
        verified_runtime_status_ == multiplayer_player_status::active && participant_ &&
        directory_plan.request.kind == multiplayer_session_admission_kind::resume ) {
        plan.origin_ = multiplayer_root_admission_origin::disconnected_grace;
        plan.old_participant_ = *participant_;
        if( directory_plan.advances_generation &&
            multiplayer_is_next_session_generation(
                session_generation_, directory_plan.committed_session_generation ) &&
            participant_->session_generation == session_generation_ ) {
            plan.barrier_mode_ =
                multiplayer_root_barrier_resume_mode::advance_one_generation;
            plan.unpublished_effect_ =
                multiplayer_root_lifecycle_effect::repair_disconnected_barrier_generation;
            plan.result_ = result( multiplayer_root_lifecycle_status::applied,
                                   multiplayer_root_lifecycle_effect::resume_barrier_plus_one );
            return plan;
        }
        if( directory_plan.replays_committed_generation &&
            directory_plan.committed_session_generation == session_generation_ &&
            participant_->session_generation == session_generation_ ) {
            plan.barrier_mode_ =
                multiplayer_root_barrier_resume_mode::replay_same_generation;
            plan.result_ = result(
                               multiplayer_root_lifecycle_status::applied,
                               multiplayer_root_lifecycle_effect::rebind_replayed_barrier_generation );
            return plan;
        }
        plan.result_ = result( multiplayer_root_lifecycle_status::stale_generation );
        return plan;
    }

    if( server_ == multiplayer_root_server_state::dormant &&
        barrier_ == multiplayer_root_barrier_state::none && !binding_ &&
        verified_runtime_status_ == multiplayer_player_status::offline &&
        connection_ != multiplayer_root_connection_state::graceful_release_pending ) {
        const bool advances_current = directory_plan.advances_generation &&
                                      multiplayer_is_next_session_generation(
                                          session_generation_,
                                          directory_plan.committed_session_generation );
        const bool replays_current = directory_plan.replays_committed_generation &&
                                     directory_plan.committed_session_generation ==
                                     session_generation_;
        if( !advances_current && !replays_current ) {
            plan.result_ = result( multiplayer_root_lifecycle_status::stale_generation );
            return plan;
        }
        plan.origin_ = multiplayer_root_admission_origin::dormant;
        plan.barrier_mode_ = multiplayer_root_barrier_resume_mode::no_open_barrier;
        plan.result_ = result( multiplayer_root_lifecycle_status::applied );
        return plan;
    }

    plan.result_ = result( multiplayer_root_lifecycle_status::invalid_transition );
    return plan;
}

multiplayer_root_lifecycle_result
multiplayer_selected_root_lifecycle::record_admission_published(
    const multiplayer_root_admission_plan &plan )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( admission_outcome_matches( plan, true ) && binding_ &&
        same_binding( *binding_, plan.binding_ ) &&
        session_generation_ == plan.committed_participant_.session_generation &&
        connection_ == multiplayer_root_connection_state::connected ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( !plan || plan.owner_ != this || plan.lifecycle_version_ != lifecycle_version_ ||
        !plan_matches_current_origin( plan ) ) {
        server_ = multiplayer_root_server_state::faulted;
        advance_version();
        return result( multiplayer_root_lifecycle_status::external_state_mismatch,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }

    session_generation_ = plan.committed_participant_.session_generation;
    binding_ = plan.binding_;
    connection_ = multiplayer_root_connection_state::connected;
    verified_runtime_status_ = multiplayer_player_status::active;
    server_ = multiplayer_root_server_state::running;

    multiplayer_root_lifecycle_effect next_effect =
        multiplayer_root_lifecycle_effect::none;
    if( plan.origin_ == multiplayer_root_admission_origin::disconnected_grace ) {
        participant_ = plan.committed_participant_;
        barrier_ = multiplayer_root_barrier_state::awaiting_command;
    } else {
        participant_.reset();
        barrier_ = multiplayer_root_barrier_state::none;
        shared_turn_ = 0;
        next_effect = multiplayer_root_lifecycle_effect::begin_next_turn;
    }
    last_admission_outcome_ = admission_outcome {
        plan.admission_id_, plan.binding_,
        plan.committed_participant_.session_generation, true
    };
    clear_departure_for_connected_session();
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied, next_effect );
}

multiplayer_root_lifecycle_result
multiplayer_selected_root_lifecycle::record_admission_unpublished(
    const multiplayer_root_admission_plan &plan )
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::faulted,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    if( admission_outcome_matches( plan, false ) && !binding_ &&
        session_generation_ == plan.committed_participant_.session_generation ) {
        return result( multiplayer_root_lifecycle_status::duplicate );
    }
    if( !plan || plan.owner_ != this || plan.lifecycle_version_ != lifecycle_version_ ||
        !plan_matches_current_origin( plan ) ) {
        server_ = multiplayer_root_server_state::faulted;
        advance_version();
        return result( multiplayer_root_lifecycle_status::external_state_mismatch,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }

    session_generation_ = plan.committed_participant_.session_generation;
    binding_.reset();
    last_admission_outcome_ = admission_outcome {
        plan.admission_id_, plan.binding_,
        plan.committed_participant_.session_generation, false
    };

    multiplayer_root_lifecycle_effect next_effect =
        multiplayer_root_lifecycle_effect::none;
    switch( plan.origin_ ) {
        case multiplayer_root_admission_origin::unbound_active:
            connection_ = multiplayer_root_connection_state::unbound;
            verified_runtime_status_ = multiplayer_player_status::active;
            server_ = multiplayer_root_server_state::running;
            barrier_ = multiplayer_root_barrier_state::none;
            break;
        case multiplayer_root_admission_origin::disconnected_grace:
            connection_ = multiplayer_root_connection_state::disconnected;
            verified_runtime_status_ = multiplayer_player_status::active;
            server_ = multiplayer_root_server_state::running;
            barrier_ = multiplayer_root_barrier_state::disconnected_grace;
            participant_ = plan.committed_participant_;
            forced_wait_requested_ = false;
            next_effect =
                multiplayer_root_lifecycle_effect::wait_for_disconnect_deadline;
            break;
        case multiplayer_root_admission_origin::dormant:
            clear_departure_for_connected_session();
            connection_ = multiplayer_root_connection_state::disconnected;
            verified_runtime_status_ = multiplayer_player_status::offline;
            server_ = multiplayer_root_server_state::dormant;
            barrier_ = multiplayer_root_barrier_state::none;
            participant_.reset();
            shared_turn_ = 0;
            break;
    }
    advance_version();
    if( plan.origin_ == multiplayer_root_admission_origin::disconnected_grace ) {
        departure_lifecycle_version_ = lifecycle_version_;
        refresh_departure_ticket();
    }
    return result( multiplayer_root_lifecycle_status::applied, next_effect );
}

multiplayer_root_lifecycle_result multiplayer_selected_root_lifecycle::latch_fault()
{
    if( server_ == multiplayer_root_server_state::faulted ) {
        return result( multiplayer_root_lifecycle_status::duplicate,
                       multiplayer_root_lifecycle_effect::fatal_shutdown );
    }
    server_ = multiplayer_root_server_state::faulted;
    advance_version();
    return result( multiplayer_root_lifecycle_status::applied,
                   multiplayer_root_lifecycle_effect::fatal_shutdown );
}

multiplayer_selected_root_lifecycle_snapshot
multiplayer_selected_root_lifecycle::snapshot() const
{
    multiplayer_selected_root_lifecycle_snapshot value;
    value.player_id = player_id_;
    value.character_id = character_id_;
    value.session_generation = session_generation_;
    value.binding = binding_;
    value.departed_binding = departed_binding_;
    value.participant = participant_;
    value.world_ticket = world_ticket_;
    value.connection = connection_;
    value.barrier = barrier_;
    value.verified_runtime_status = verified_runtime_status_;
    value.server = server_;
    value.departure_kind = departure_kind_;
    value.lifecycle_version = lifecycle_version_;
    value.departure_epoch = departure_epoch_;
    value.shared_turn = shared_turn_;
    value.disconnect_deadline = disconnect_deadline_;
    value.graceful_request_sequence = graceful_request_sequence_;
    value.forced_wait_requested = forced_wait_requested_;
    value.graceful_completion_queued = graceful_completion_queued_;
    return value;
}

multiplayer_session_runtime_key multiplayer_selected_root_lifecycle::runtime_key() const
{
    return { player_id_.str(), character_id_, session_generation_ };
}

std::optional<multiplayer_root_departure_ticket>
multiplayer_selected_root_lifecycle::departure_ticket() const
{
    if( !exact_departure_ticket_ ||
        departure_kind_ == multiplayer_root_departure_kind::none ||
        server_ == multiplayer_root_server_state::faulted ) {
        return std::nullopt;
    }
    return exact_departure_ticket_;
}

bool multiplayer_selected_root_lifecycle::can_begin_turn() const
{
    return server_ == multiplayer_root_server_state::running &&
           connection_ == multiplayer_root_connection_state::connected && binding_ &&
           barrier_ == multiplayer_root_barrier_state::none &&
           verified_runtime_status_ == multiplayer_player_status::active;
}

bool multiplayer_selected_root_lifecycle::can_execute_command() const
{
    return server_ == multiplayer_root_server_state::running &&
           connection_ == multiplayer_root_connection_state::connected && binding_ && participant_ &&
           barrier_ == multiplayer_root_barrier_state::awaiting_command &&
           verified_runtime_status_ == multiplayer_player_status::active &&
           participant_->player_id == player_id_ &&
           participant_->session_generation == session_generation_ &&
           binding_->session_generation == session_generation_;
}

bool multiplayer_selected_root_lifecycle::can_create_guard() const
{
    if( server_ != multiplayer_root_server_state::running || !participant_ ||
        verified_runtime_status_ != multiplayer_player_status::active ) {
        return false;
    }
    return barrier_ == multiplayer_root_barrier_state::awaiting_command ||
           barrier_ == multiplayer_root_barrier_state::disconnected_grace ||
           barrier_ == multiplayer_root_barrier_state::forced_wait_pending;
}

bool multiplayer_selected_root_lifecycle::can_save_or_shutdown() const
{
    if( server_ == multiplayer_root_server_state::faulted ||
        barrier_ != multiplayer_root_barrier_state::none ) {
        return false;
    }
    const bool running_safe_point =
        server_ == multiplayer_root_server_state::running && !departed_binding_ &&
        verified_runtime_status_ == multiplayer_player_status::active &&
        ( ( connection_ == multiplayer_root_connection_state::connected && binding_ ) ||
          ( connection_ == multiplayer_root_connection_state::unbound && !binding_ ) );
    return running_safe_point ||
           ( server_ == multiplayer_root_server_state::dormant &&
             verified_runtime_status_ == multiplayer_player_status::offline );
}

multiplayer_root_lifecycle_result multiplayer_selected_root_lifecycle::result(
    const multiplayer_root_lifecycle_status status,
    const multiplayer_root_lifecycle_effect effect ) const
{
    return { status, effect };
}

bool multiplayer_selected_root_lifecycle::ticket_matches_current(
    const multiplayer_root_departure_ticket &ticket ) const
{
    return ticket_matches_departure( ticket ) && participant_ &&
           same_participant( ticket.participant, *participant_ ) && disconnect_deadline_ &&
           ticket.deadline == *disconnect_deadline_;
}

bool multiplayer_selected_root_lifecycle::ticket_matches_departure(
    const multiplayer_root_departure_ticket &ticket ) const
{
    return exact_departure_ticket_ && ticket == *exact_departure_ticket_;
}

bool multiplayer_selected_root_lifecycle::directory_plan_is_well_formed(
    const multiplayer_session_admission_plan &plan ) const
{
    if( !plan || plan.request.admission_id == 0 || plan.request.connection == 0 ||
        session_is_empty( plan.request.session ) ||
        plan.request.player_id != player_id_.str() ||
        plan.request.character_id != character_id_ ||
        !multiplayer_is_valid_session_generation( plan.committed_session_generation ) ||
        ( plan.advances_generation && plan.replays_committed_generation ) ) {
        return false;
    }
    if( plan.request.kind == multiplayer_session_admission_kind::authentication ) {
        return plan.request.expected_session_generation == 0 &&
               !plan.replays_committed_generation;
    }
    return multiplayer_is_valid_session_generation(
               plan.request.expected_session_generation ) &&
           plan.advances_generation != plan.replays_committed_generation &&
           multiplayer_is_next_session_generation(
               plan.request.expected_session_generation,
               plan.committed_session_generation );
}

bool multiplayer_selected_root_lifecycle::plan_matches_current_origin(
    const multiplayer_root_admission_plan &plan ) const
{
    if( plan.owner_ != this ||
        plan.result_.status != multiplayer_root_lifecycle_status::applied ||
        plan.admission_id_ == 0 || plan.binding_.connection == 0 ||
        session_is_empty( plan.binding_.session ) ||
        plan.binding_.player_id != player_id_.str() ||
        plan.binding_.character_id != character_id_ ||
        plan.committed_participant_.player_id != player_id_ ||
        !multiplayer_is_valid_session_generation(
            plan.committed_participant_.session_generation ) ||
        plan.binding_.session_generation !=
        plan.committed_participant_.session_generation ||
        ( plan.advances_generation_ && plan.replays_committed_generation_ ) ||
        ( plan.admission_kind_ == multiplayer_session_admission_kind::authentication &&
          ( plan.expected_generation_ != 0 || plan.replays_committed_generation_ ) ) ||
        ( plan.admission_kind_ == multiplayer_session_admission_kind::resume &&
          !multiplayer_is_valid_session_generation( plan.expected_generation_ ) ) ) {
        return false;
    }
    switch( plan.origin_ ) {
        case multiplayer_root_admission_origin::unbound_active:
            return plan.barrier_mode_ ==
                   multiplayer_root_barrier_resume_mode::no_open_barrier &&
                   plan.unpublished_effect_ == multiplayer_root_lifecycle_effect::none &&
                   plan.result_.next_effect == multiplayer_root_lifecycle_effect::none &&
                   !plan.replays_committed_generation_ &&
                   ( ( !plan.advances_generation_ &&
                       plan.committed_participant_.session_generation == session_generation_ ) ||
                     ( plan.advances_generation_ &&
                       multiplayer_is_next_session_generation(
                           session_generation_,
                           plan.committed_participant_.session_generation ) ) ) &&
                   server_ == multiplayer_root_server_state::running &&
                   connection_ == multiplayer_root_connection_state::unbound &&
                   barrier_ == multiplayer_root_barrier_state::none && !binding_ &&
                   verified_runtime_status_ == multiplayer_player_status::active;
        case multiplayer_root_admission_origin::disconnected_grace:
            if( server_ != multiplayer_root_server_state::running ||
                connection_ != multiplayer_root_connection_state::disconnected ||
                departure_kind_ != multiplayer_root_departure_kind::transport_loss ||
                barrier_ != multiplayer_root_barrier_state::disconnected_grace || binding_ ||
                !participant_ || !same_participant( *participant_, plan.old_participant_ ) ||
                verified_runtime_status_ != multiplayer_player_status::active ||
                forced_wait_requested_ ||
                plan.admission_kind_ != multiplayer_session_admission_kind::resume ) {
                return false;
            }
            if( plan.advances_generation_ ) {
                return !plan.replays_committed_generation_ &&
                       plan.expected_generation_ == session_generation_ &&
                       plan.barrier_mode_ ==
                       multiplayer_root_barrier_resume_mode::advance_one_generation &&
                       plan.result_.next_effect ==
                       multiplayer_root_lifecycle_effect::resume_barrier_plus_one &&
                       plan.unpublished_effect_ ==
                       multiplayer_root_lifecycle_effect::repair_disconnected_barrier_generation &&
                       multiplayer_is_next_session_generation(
                           session_generation_,
                           plan.committed_participant_.session_generation );
            }
            return plan.replays_committed_generation_ &&
                   multiplayer_is_next_session_generation(
                       plan.expected_generation_, session_generation_ ) &&
                   plan.committed_participant_.session_generation == session_generation_ &&
                   plan.barrier_mode_ ==
                   multiplayer_root_barrier_resume_mode::replay_same_generation &&
                   plan.result_.next_effect ==
                   multiplayer_root_lifecycle_effect::rebind_replayed_barrier_generation &&
                   plan.unpublished_effect_ == multiplayer_root_lifecycle_effect::none;
        case multiplayer_root_admission_origin::dormant:
            if( plan.barrier_mode_ !=
                multiplayer_root_barrier_resume_mode::no_open_barrier ||
                plan.unpublished_effect_ != multiplayer_root_lifecycle_effect::none ||
                plan.result_.next_effect != multiplayer_root_lifecycle_effect::none ||
                server_ != multiplayer_root_server_state::dormant ||
                connection_ == multiplayer_root_connection_state::graceful_release_pending ||
                barrier_ != multiplayer_root_barrier_state::none || binding_ ||
                verified_runtime_status_ != multiplayer_player_status::offline ) {
                return false;
            }
            if( plan.advances_generation_ ) {
                const bool expected_generation_matches =
                    plan.admission_kind_ == multiplayer_session_admission_kind::authentication ?
                    plan.expected_generation_ == 0 :
                    plan.expected_generation_ == session_generation_;
                return !plan.replays_committed_generation_ && expected_generation_matches &&
                       multiplayer_is_next_session_generation(
                           session_generation_,
                           plan.committed_participant_.session_generation );
            }
            return plan.admission_kind_ == multiplayer_session_admission_kind::resume &&
                   plan.replays_committed_generation_ &&
                   multiplayer_is_next_session_generation(
                       plan.expected_generation_, session_generation_ ) &&
                   plan.committed_participant_.session_generation == session_generation_;
    }
    return false;
}

bool multiplayer_selected_root_lifecycle::admission_outcome_matches(
    const multiplayer_root_admission_plan &plan, const bool published ) const
{
    return plan.owner_ == this && last_admission_outcome_ &&
           last_admission_outcome_->admission_id == plan.admission_id_ &&
           same_binding( last_admission_outcome_->binding, plan.binding_ ) &&
           last_admission_outcome_->committed_generation ==
           plan.committed_participant_.session_generation &&
           last_admission_outcome_->published == published;
}

void multiplayer_selected_root_lifecycle::clear_departure_for_connected_session()
{
    departed_binding_.reset();
    departure_lifecycle_version_ = 0;
    exact_departure_ticket_.reset();
    departure_kind_ = multiplayer_root_departure_kind::none;
    disconnect_deadline_.reset();
    graceful_request_sequence_.reset();
    graceful_completion_queued_ = false;
    forced_wait_requested_ = false;
    world_ticket_.reset();
    last_completed_world_ticket_.reset();
}

void multiplayer_selected_root_lifecycle::refresh_departure_ticket()
{
    if( !departed_binding_ || !participant_ || !disconnect_deadline_ ||
        departure_kind_ == multiplayer_root_departure_kind::none ) {
        exact_departure_ticket_.reset();
        return;
    }
    multiplayer_root_departure_ticket ticket;
    ticket.lifecycle_version = departure_lifecycle_version_;
    ticket.departure_epoch = departure_epoch_;
    ticket.binding = *departed_binding_;
    ticket.participant = *participant_;
    ticket.deadline = *disconnect_deadline_;
    ticket.owner_ = this;
    exact_departure_ticket_ = ticket;
}

void multiplayer_selected_root_lifecycle::advance_version()
{
    if( lifecycle_version_ == std::numeric_limits<std::uint64_t>::max() ) {
        server_ = multiplayer_root_server_state::faulted;
        return;
    }
    ++lifecycle_version_;
}

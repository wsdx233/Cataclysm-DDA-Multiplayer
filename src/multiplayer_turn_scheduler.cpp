#include "multiplayer_turn_scheduler.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

bool multiplayer_turn_scheduler::begin_turn(
    const std::uint64_t shared_turn,
    const std::vector<multiplayer_turn_participant_key> &roster )
{
    if( faulted_ || stage_ != multiplayer_turn_scheduler_stage::idle || roster.empty() ||
        roster.size() > maximum_participants ||
        ( last_shared_turn_ && shared_turn <= *last_shared_turn_ ) ) {
        return false;
    }

    std::vector<participant_record> next_participants;
    next_participants.reserve( roster.size() );
    for( const multiplayer_turn_participant_key &participant : roster ) {
        if( !participant.player_id.is_valid() || participant.session_generation == 0 ) {
            return false;
        }
        next_participants.push_back( { participant,
                                       multiplayer_turn_participant_state::awaiting_command,
                                       std::nullopt } );
    }

    std::sort( next_participants.begin(), next_participants.end(),
    []( const participant_record & lhs, const participant_record & rhs ) {
        return lhs.key.player_id.str() < rhs.key.player_id.str();
    } );
    const auto duplicate = std::adjacent_find(
                               next_participants.begin(), next_participants.end(),
    []( const participant_record & lhs, const participant_record & rhs ) {
        return lhs.key.player_id == rhs.key.player_id;
    } );
    if( duplicate != next_participants.end() ) {
        return false;
    }

    auto first = next_participants.begin();
    if( previous_first_player_ ) {
        const std::string &previous_id = previous_first_player_->str();
        first = std::upper_bound( next_participants.begin(), next_participants.end(), previous_id,
        []( const std::string & id, const participant_record & participant ) {
            return id < participant.key.player_id.str();
        } );
        if( first == next_participants.end() ) {
            first = next_participants.begin();
        }
    }
    std::rotate( next_participants.begin(), first, next_participants.end() );

    participants_ = std::move( next_participants );
    last_shared_turn_ = shared_turn;
    shared_turn_ = shared_turn;
    round_ = 0;
    cursor_ = 0;
    stage_ = multiplayer_turn_scheduler_stage::player_actions;
    return true;
}

multiplayer_turn_scheduler_stage multiplayer_turn_scheduler::stage() const
{
    return stage_;
}

bool multiplayer_turn_scheduler::is_faulted() const noexcept
{
    return faulted_;
}

void multiplayer_turn_scheduler::latch_execution_fault() noexcept
{
    faulted_ = true;
}

std::size_t multiplayer_turn_scheduler::participant_count() const
{
    return participants_.size();
}

bool multiplayer_turn_scheduler::has_participant(
    const multiplayer_player_id &player_id ) const
{
    return find_participant( player_id ).has_value();
}

std::optional<multiplayer_turn_participant_key> multiplayer_turn_scheduler::participant_key(
    const multiplayer_player_id &player_id ) const
{
    const std::optional<std::size_t> found = find_participant( player_id );
    if( !found ) {
        return std::nullopt;
    }
    return participants_[*found].key;
}

std::optional<multiplayer_turn_participant_state>
multiplayer_turn_scheduler::participant_state( const multiplayer_player_id &player_id ) const
{
    const std::optional<std::size_t> found = find_participant( player_id );
    if( !found ) {
        return std::nullopt;
    }
    return participants_[*found].state;
}

std::optional<multiplayer_turn_slot> multiplayer_turn_scheduler::current_slot() const
{
    if( faulted_ || stage_ != multiplayer_turn_scheduler_stage::player_actions ||
        cursor_ >= participants_.size() ) {
        return std::nullopt;
    }

    const participant_record &participant = participants_[cursor_];
    return multiplayer_turn_slot {
        participant.key,
        {
            shared_turn_, round_, static_cast<std::uint32_t>( cursor_ ),
            participant.key.player_id
        },
        participant.state
    };
}

bool multiplayer_turn_scheduler::record_action_result(
    const multiplayer_turn_participant_key &participant,
    const multiplayer_turn_action_disposition disposition )
{
    const std::optional<multiplayer_turn_slot> slot = current_slot();
    if( !slot || slot->participant != participant ||
        slot->state != multiplayer_turn_participant_state::awaiting_command ) {
        return false;
    }

    switch( disposition ) {
        case multiplayer_turn_action_disposition::rejected:
        case multiplayer_turn_action_disposition::duplicate:
            return true;
        case multiplayer_turn_action_disposition::accepted_remains_eligible:
            advance_from_current();
            return true;
        case multiplayer_turn_action_disposition::accepted_finished:
            participants_[cursor_].state = multiplayer_turn_participant_state::finished;
            advance_from_current();
            return true;
    }
    return false;
}

bool multiplayer_turn_scheduler::mark_barrier_disconnected(
    const multiplayer_turn_participant_key &participant )
{
    const std::optional<std::size_t> found = find_exact_participant( participant );
    if( faulted_ || stage_ != multiplayer_turn_scheduler_stage::player_actions || !found ||
        participants_[*found].state != multiplayer_turn_participant_state::awaiting_command ) {
        return false;
    }
    participants_[*found].state = multiplayer_turn_participant_state::disconnected_grace;
    return true;
}

bool multiplayer_turn_scheduler::resume_barrier_participant(
    const multiplayer_turn_participant_key &expected_participant,
    const std::uint64_t resumed_session_generation )
{
    const std::optional<std::size_t> found = find_exact_participant( expected_participant );
    if( faulted_ || stage_ != multiplayer_turn_scheduler_stage::player_actions || !found ||
        participants_[*found].state != multiplayer_turn_participant_state::disconnected_grace ||
        expected_participant.session_generation == std::numeric_limits<std::uint64_t>::max() ||
        resumed_session_generation != expected_participant.session_generation + 1 ) {
        return false;
    }

    participants_[*found].key.session_generation = resumed_session_generation;
    participants_[*found].state = multiplayer_turn_participant_state::awaiting_command;
    return true;
}

bool multiplayer_turn_scheduler::apply_disconnect_timeout(
    const multiplayer_turn_participant_key &participant,
    const multiplayer_disconnect_timeout_policy policy )
{
    const std::optional<std::size_t> found = find_exact_participant( participant );
    if( faulted_ || stage_ != multiplayer_turn_scheduler_stage::player_actions || !found ||
        *found != cursor_ ||
        participants_[*found].state != multiplayer_turn_participant_state::disconnected_grace ) {
        return false;
    }

    switch( policy ) {
        case multiplayer_disconnect_timeout_policy::keep_waiting:
            return true;
        case multiplayer_disconnect_timeout_policy::automatic_wait:
        case multiplayer_disconnect_timeout_policy::remove_from_barrier:
            participants_[*found].state =
                multiplayer_turn_participant_state::automatic_wait_pending;
            participants_[*found].pending_timeout_policy = policy;
            return true;
    }
    return false;
}

bool multiplayer_turn_scheduler::record_automatic_wait_executed(
    const multiplayer_turn_participant_key &participant )
{
    const std::optional<multiplayer_turn_slot> slot = current_slot();
    if( !slot || slot->participant != participant ||
        slot->state != multiplayer_turn_participant_state::automatic_wait_pending ) {
        return false;
    }

    participant_record &record = participants_[cursor_];
    if( !record.pending_timeout_policy ||
        *record.pending_timeout_policy == multiplayer_disconnect_timeout_policy::keep_waiting ) {
        return false;
    }
    record.state =
        *record.pending_timeout_policy == multiplayer_disconnect_timeout_policy::automatic_wait ?
        multiplayer_turn_participant_state::finished :
        multiplayer_turn_participant_state::removed;
    record.pending_timeout_policy.reset();
    advance_from_current();
    return true;
}

std::optional<multiplayer_world_ticket> multiplayer_turn_scheduler::claim_world()
{
    if( faulted_ || stage_ != multiplayer_turn_scheduler_stage::world_ready ) {
        return std::nullopt;
    }
    stage_ = multiplayer_turn_scheduler_stage::world_processing;
    return multiplayer_world_ticket{ shared_turn_ };
}

bool multiplayer_turn_scheduler::record_world_completed( const multiplayer_world_ticket &ticket )
{
    if( faulted_ || stage_ != multiplayer_turn_scheduler_stage::world_processing ||
        ticket.shared_turn != shared_turn_ || participants_.empty() ) {
        return false;
    }

    previous_first_player_ = participants_.front().key.player_id;
    participants_.clear();
    shared_turn_ = 0;
    round_ = 0;
    cursor_ = 0;
    stage_ = multiplayer_turn_scheduler_stage::idle;
    return true;
}

std::optional<std::size_t> multiplayer_turn_scheduler::find_participant(
    const multiplayer_player_id &player_id ) const
{
    const auto found = std::find_if( participants_.begin(), participants_.end(),
    [&player_id]( const participant_record & candidate ) {
        return candidate.key.player_id == player_id;
    } );
    if( found == participants_.end() ) {
        return std::nullopt;
    }
    return static_cast<std::size_t>( found - participants_.begin() );
}

std::optional<std::size_t> multiplayer_turn_scheduler::find_exact_participant(
    const multiplayer_turn_participant_key &participant ) const
{
    const std::optional<std::size_t> found = find_participant( participant.player_id );
    return found && participants_[*found].key == participant ? found : std::nullopt;
}

bool multiplayer_turn_scheduler::is_terminal( const participant_record &participant ) const
{
    return participant.state == multiplayer_turn_participant_state::finished ||
           participant.state == multiplayer_turn_participant_state::removed;
}

void multiplayer_turn_scheduler::advance_from_current()
{
    if( stage_ != multiplayer_turn_scheduler_stage::player_actions || participants_.empty() ) {
        return;
    }
    if( std::all_of( participants_.begin(), participants_.end(),
    [this]( const participant_record & participant ) {
    return is_terminal( participant );
    } ) ) {
        enter_world_ready();
        return;
    }

    do {
        ++cursor_;
        if( cursor_ == participants_.size() ) {
            cursor_ = 0;
            ++round_;
        }
    } while( is_terminal( participants_[cursor_] ) );
}

void multiplayer_turn_scheduler::enter_world_ready()
{
    stage_ = multiplayer_turn_scheduler_stage::world_ready;
    cursor_ = participants_.size();
}

#include "cata_catch.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "avatar.h"
#include "game.h"
#include "memory_fast.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_selected_root_lifecycle.h"
#include "multiplayer_turn_phase_adapter.h"
#include "player_helpers.h"

namespace
{

multiplayer_player_id root_player_id()
{
    return multiplayer_player_id::from_string(
               "00000000-0000-4000-8000-000000000001" );
}

multiplayer_session_id session_id( const std::uint8_t value )
{
    multiplayer_session_id result = {};
    result.fill( value );
    return result;
}

multiplayer_session_admission_plan admission_plan(
    const multiplayer_session_admission_kind kind,
    const std::uint64_t admission_id,
    const multiplayer_connection_id connection,
    const std::uint8_t session_value,
    const std::uint64_t committed_generation,
    const bool advances_generation,
    const bool replays_committed_generation,
    const std::uint64_t expected_generation = 0 )
{
    multiplayer_session_admission_plan plan;
    plan.status = multiplayer_session_directory_status::success;
    plan.request.kind = kind;
    plan.request.admission_id = admission_id;
    plan.request.connection = connection;
    plan.request.session = session_id( session_value );
    plan.request.player_id = root_player_id().str();
    plan.request.character_id = "root-character";
    plan.request.expected_session_generation = expected_generation;
    plan.committed_session_generation = committed_generation;
    plan.advances_generation = advances_generation;
    plan.replays_committed_generation = replays_committed_generation;
    return plan;
}

multiplayer_session_admission_request runtime_admission_request(
    const multiplayer_session_admission_kind kind,
    const std::uint64_t admission_id,
    const multiplayer_connection_id connection,
    const std::uint8_t session_value,
    const multiplayer_player_runtime &runtime,
    const std::uint64_t expected_generation = 0,
    const std::uint64_t last_server_revision = 0,
    const std::uint64_t last_client_sequence = 0 )
{
    multiplayer_session_admission_request request;
    request.kind = kind;
    request.admission_id = admission_id;
    request.connection = connection;
    request.session = session_id( session_value );
    request.player_id = runtime.player_id().str();
    request.character_id = std::to_string( runtime.player().getID().get_value() );
    request.expected_session_generation = expected_generation;
    request.last_server_revision = last_server_revision;
    request.last_client_sequence = last_client_sequence;
    return request;
}

class registered_lifecycle_player
{
    public:
        registered_lifecycle_player() : owner_( make_shared_fast<avatar>() ) {
            owner_->create( character_type::NOW );
            clear_character( *owner_ );
            owner_->setID( g->assign_npc_id(), true );
            registered_ = g->register_multiplayer_player( owner_ );
            if( registered_ ) {
                runtime_ = g->multiplayer_players().find_runtime( *owner_ );
            }
        }

        ~registered_lifecycle_player() {
            if( !registered_ ) {
                return;
            }
            if( runtime_ && runtime_->status() == multiplayer_player_status::active ) {
                g->disconnect_multiplayer_player( runtime_->player_id() );
            }
            g->unregister_multiplayer_player( *owner_ );
        }

        explicit operator bool() const {
            return registered_ && runtime_ != nullptr;
        }

        const shared_ptr_fast<multiplayer_player_runtime> &runtime() const {
            return runtime_;
        }

    private:
        shared_ptr_fast<avatar> owner_;
        shared_ptr_fast<multiplayer_player_runtime> runtime_;
        bool registered_ = false;
};

multiplayer_session_binding publish_initial_binding(
    multiplayer_selected_root_lifecycle &lifecycle,
    const std::uint64_t generation = 1,
    const multiplayer_connection_id connection = 101,
    const std::uint8_t session_value = 1 )
{
    const multiplayer_session_admission_plan directory_plan = admission_plan(
                multiplayer_session_admission_kind::authentication, 1, connection,
                session_value, generation, false, false );
    const multiplayer_root_admission_plan root_plan = lifecycle.plan_admission( directory_plan );
    REQUIRE( root_plan );
    REQUIRE( root_plan.origin() == multiplayer_root_admission_origin::unbound_active );
    REQUIRE( lifecycle.record_admission_published( root_plan ) );
    return root_plan.binding();
}

multiplayer_root_departure_ticket current_departure_ticket(
    const multiplayer_selected_root_lifecycle &lifecycle )
{
    const std::optional<multiplayer_root_departure_ticket> ticket =
        lifecycle.departure_ticket();
    REQUIRE( ticket );
    return *ticket;
}

void begin_bound_turn( multiplayer_selected_root_lifecycle &lifecycle,
                       const multiplayer_session_binding &binding,
                       const std::uint64_t shared_turn )
{
    REQUIRE( lifecycle.begin_turn(
                 binding, { root_player_id(), binding.session_generation }, shared_turn ) );
    REQUIRE( lifecycle.can_execute_command() );
}

multiplayer_root_departure_ticket drive_unexpected_root_to_dormant(
    multiplayer_selected_root_lifecycle &lifecycle,
    const multiplayer_session_binding &binding,
    const std::uint64_t shared_turn,
    const multiplayer_selected_root_lifecycle::clock::time_point now )
{
    REQUIRE( lifecycle.record_departure_accepted(
                 binding, multiplayer_root_departure_kind::transport_loss, now,
                 std::chrono::seconds( 5 ) ) );
    const multiplayer_root_departure_ticket ticket = current_departure_ticket( lifecycle );
    REQUIRE( lifecycle.record_barrier_disconnected( ticket ) );
    REQUIRE( lifecycle.evaluate_disconnect_timeout(
                 ticket, now + std::chrono::seconds( 5 ), ticket.participant ) );
    REQUIRE( lifecycle.record_forced_wait_pending( ticket ) );
    REQUIRE( lifecycle.record_forced_wait_terminal( ticket ) );
    REQUIRE( lifecycle.record_world_claimed( ticket, { shared_turn } ) );
    REQUIRE( lifecycle.record_world_completed( ticket, { shared_turn } ) );
    REQUIRE( lifecycle.record_runtime_offline( ticket ) );
    return ticket;
}

} // namespace

TEST_CASE( "multiplayer_selected_root_boundary_departure_goes_directly_offline",
           "[multiplayer][root_lifecycle]" )
{
    multiplayer_selected_root_lifecycle lifecycle(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    const multiplayer_session_binding binding = publish_initial_binding( lifecycle );
    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};

    SECTION( "transport_loss" ) {
        const multiplayer_root_lifecycle_result departure =
            lifecycle.record_departure_accepted(
                binding, multiplayer_root_departure_kind::transport_loss, now,
                std::chrono::seconds( 5 ) );
        REQUIRE( departure );
        CHECK( departure.next_effect ==
               multiplayer_root_lifecycle_effect::transition_runtime_offline );
        CHECK_FALSE( lifecycle.can_save_or_shutdown() );
        const multiplayer_root_departure_ticket ticket =
            current_departure_ticket( lifecycle );
        REQUIRE( lifecycle.record_runtime_offline( ticket ) );
        CHECK( lifecycle.snapshot().server == multiplayer_root_server_state::dormant );
        CHECK( lifecycle.can_save_or_shutdown() );
    }

    SECTION( "graceful_release" ) {
        const multiplayer_root_lifecycle_result departure =
            lifecycle.record_departure_accepted(
                binding, multiplayer_root_departure_kind::graceful_release, now,
                std::chrono::seconds( 5 ), 55 );
        REQUIRE( departure );
        CHECK( departure.next_effect ==
               multiplayer_root_lifecycle_effect::transition_runtime_offline );
        CHECK_FALSE( lifecycle.can_save_or_shutdown() );
        const multiplayer_root_departure_ticket ticket =
            current_departure_ticket( lifecycle );
        const multiplayer_root_lifecycle_result offline =
            lifecycle.record_runtime_offline( ticket );
        REQUIRE( offline );
        CHECK( offline.next_effect ==
               multiplayer_root_lifecycle_effect::complete_graceful_release );
        REQUIRE( lifecycle.record_graceful_completion_queued( binding, 55 ) );
        REQUIRE( lifecycle.record_graceful_transport_closed( binding ) );
        CHECK( lifecycle.can_save_or_shutdown() );
    }
}

TEST_CASE( "multiplayer_selected_root_unexpected_disconnect_reaches_dormant_exactly_once",
           "[multiplayer][root_lifecycle]" )
{
    multiplayer_selected_root_lifecycle lifecycle(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    const multiplayer_session_binding binding = publish_initial_binding( lifecycle );
    begin_bound_turn( lifecycle, binding, 41 );
    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};

    const multiplayer_root_lifecycle_result departure =
        lifecycle.record_departure_accepted(
            binding, multiplayer_root_departure_kind::transport_loss, now,
            std::chrono::seconds( 10 ) );
    REQUIRE( departure );
    CHECK( departure.next_effect ==
           multiplayer_root_lifecycle_effect::mark_barrier_disconnected );
    CHECK_FALSE( lifecycle.can_execute_command() );
    CHECK( lifecycle.can_create_guard() );

    const multiplayer_root_departure_ticket ticket = current_departure_ticket( lifecycle );
    const multiplayer_root_lifecycle_result barrier =
        lifecycle.record_barrier_disconnected( ticket );
    REQUIRE( barrier );
    CHECK( barrier.next_effect ==
           multiplayer_root_lifecycle_effect::wait_for_disconnect_deadline );
    CHECK( lifecycle.record_barrier_disconnected( ticket ).status ==
           multiplayer_root_lifecycle_status::duplicate );
    multiplayer_root_departure_ticket wrong_ticket = ticket;
    ++wrong_ticket.participant.session_generation;
    CHECK( lifecycle.record_barrier_disconnected( wrong_ticket ).status ==
           multiplayer_root_lifecycle_status::stale_binding );
    wrong_ticket = ticket;
    wrong_ticket.deadline += std::chrono::seconds( 1 );
    CHECK( lifecycle.record_barrier_disconnected( wrong_ticket ).status ==
           multiplayer_root_lifecycle_status::stale_binding );

    CHECK( lifecycle.evaluate_disconnect_timeout(
               ticket, now + std::chrono::seconds( 9 ), ticket.participant ).status ==
           multiplayer_root_lifecycle_status::deadline_not_reached );
    CHECK( lifecycle.evaluate_disconnect_timeout(
               ticket, now + std::chrono::seconds( 10 ), std::nullopt ).status ==
           multiplayer_root_lifecycle_status::not_current_slot );

    multiplayer_turn_participant_key wrong_generation = ticket.participant;
    ++wrong_generation.session_generation;
    CHECK( lifecycle.evaluate_disconnect_timeout(
               ticket, now + std::chrono::seconds( 10 ), wrong_generation ).status ==
           multiplayer_root_lifecycle_status::stale_generation );

    const multiplayer_root_lifecycle_result timeout =
        lifecycle.evaluate_disconnect_timeout(
            ticket, now + std::chrono::seconds( 10 ), ticket.participant );
    REQUIRE( timeout );
    CHECK( timeout.next_effect ==
           multiplayer_root_lifecycle_effect::request_automatic_wait );
    CHECK( lifecycle.evaluate_disconnect_timeout(
               ticket, now + std::chrono::seconds( 10 ), ticket.participant ).status ==
           multiplayer_root_lifecycle_status::duplicate );

    const multiplayer_root_lifecycle_result wait_pending =
        lifecycle.record_forced_wait_pending( ticket );
    REQUIRE( wait_pending );
    CHECK( wait_pending.next_effect ==
           multiplayer_root_lifecycle_effect::execute_authoritative_wait );

    const multiplayer_root_lifecycle_result terminal =
        lifecycle.record_forced_wait_terminal( ticket );
    REQUIRE( terminal );
    CHECK( terminal.next_effect == multiplayer_root_lifecycle_effect::claim_world );
    CHECK( lifecycle.record_forced_wait_terminal( ticket ).status ==
           multiplayer_root_lifecycle_status::duplicate );

    REQUIRE( lifecycle.record_world_claimed( ticket, { 41 } ) );
    const multiplayer_root_lifecycle_result world_complete =
        lifecycle.record_world_completed( ticket, { 41 } );
    REQUIRE( world_complete );
    CHECK( world_complete.next_effect ==
           multiplayer_root_lifecycle_effect::transition_runtime_offline );

    const multiplayer_root_lifecycle_result offline =
        lifecycle.record_runtime_offline( ticket );
    REQUIRE( offline );
    CHECK( offline.next_effect == multiplayer_root_lifecycle_effect::none );
    const multiplayer_selected_root_lifecycle_snapshot dormant = lifecycle.snapshot();
    CHECK( dormant.server == multiplayer_root_server_state::dormant );
    CHECK( dormant.barrier == multiplayer_root_barrier_state::none );
    CHECK( dormant.connection == multiplayer_root_connection_state::disconnected );
    CHECK( dormant.verified_runtime_status == multiplayer_player_status::offline );
    CHECK_FALSE( lifecycle.can_begin_turn() );
    CHECK_FALSE( lifecycle.can_execute_command() );
    CHECK_FALSE( lifecycle.can_create_guard() );
    CHECK( lifecycle.can_save_or_shutdown() );
    CHECK( lifecycle.record_runtime_offline( ticket ).status ==
           multiplayer_root_lifecycle_status::duplicate );
    CHECK( lifecycle.record_world_completed( ticket, { 41 } ).status ==
           multiplayer_root_lifecycle_status::duplicate );
}

TEST_CASE( "multiplayer_selected_root_graceful_release_waits_for_world_and_offline",
           "[multiplayer][root_lifecycle]" )
{
    multiplayer_selected_root_lifecycle lifecycle(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    const multiplayer_session_binding binding = publish_initial_binding( lifecycle );
    begin_bound_turn( lifecycle, binding, 51 );
    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};

    REQUIRE( lifecycle.record_departure_accepted(
                 binding, multiplayer_root_departure_kind::graceful_release, now,
                 std::chrono::seconds( 30 ), 77 ) );
    CHECK( lifecycle.record_graceful_completion_queued( binding, 77 ).status ==
           multiplayer_root_lifecycle_status::invalid_transition );

    const multiplayer_root_departure_ticket ticket = current_departure_ticket( lifecycle );
    SECTION( "ordered_ack_only_queues_after_runtime_offline" ) {
        const multiplayer_root_lifecycle_result barrier =
            lifecycle.record_barrier_disconnected( ticket );
        REQUIRE( barrier );
        CHECK( barrier.next_effect ==
               multiplayer_root_lifecycle_effect::request_automatic_wait );

        REQUIRE( lifecycle.record_forced_wait_pending( ticket ) );
        REQUIRE( lifecycle.record_forced_wait_terminal( ticket ) );
        CHECK( lifecycle.record_graceful_completion_queued( binding, 77 ).status ==
               multiplayer_root_lifecycle_status::invalid_transition );
        REQUIRE( lifecycle.record_world_claimed( ticket, { 51 } ) );
        CHECK( lifecycle.record_graceful_completion_queued( binding, 77 ).status ==
               multiplayer_root_lifecycle_status::invalid_transition );
        REQUIRE( lifecycle.record_world_completed( ticket, { 51 } ) );
        CHECK( lifecycle.record_graceful_completion_queued( binding, 77 ).status ==
               multiplayer_root_lifecycle_status::invalid_transition );
        const multiplayer_root_lifecycle_result offline =
            lifecycle.record_runtime_offline( ticket );
        REQUIRE( offline );
        CHECK( offline.next_effect ==
               multiplayer_root_lifecycle_effect::complete_graceful_release );
        CHECK( lifecycle.snapshot().connection ==
               multiplayer_root_connection_state::graceful_release_pending );

        REQUIRE( lifecycle.record_graceful_completion_queued( binding, 77 ) );
        CHECK( lifecycle.record_graceful_completion_queued( binding, 77 ).status ==
               multiplayer_root_lifecycle_status::duplicate );
        REQUIRE( lifecycle.record_graceful_transport_closed( binding ) );
        CHECK( lifecycle.record_graceful_transport_closed( binding ).status ==
               multiplayer_root_lifecycle_status::duplicate );
    }

    SECTION( "peer_close_before_ack_cancels_completion_without_blocking_dormant" ) {
        REQUIRE( lifecycle.record_graceful_transport_closed( binding ) );
        CHECK( lifecycle.snapshot().connection ==
               multiplayer_root_connection_state::disconnected );
        REQUIRE( lifecycle.record_barrier_disconnected( ticket ) );
        REQUIRE( lifecycle.record_forced_wait_pending( ticket ) );
        REQUIRE( lifecycle.record_forced_wait_terminal( ticket ) );
        REQUIRE( lifecycle.record_world_claimed( ticket, { 51 } ) );
        REQUIRE( lifecycle.record_world_completed( ticket, { 51 } ) );
        const multiplayer_root_lifecycle_result offline =
            lifecycle.record_runtime_offline( ticket );
        REQUIRE( offline );
        CHECK( offline.next_effect == multiplayer_root_lifecycle_effect::none );
        CHECK( lifecycle.record_graceful_completion_queued( binding, 77 ).status ==
               multiplayer_root_lifecycle_status::invalid_transition );

        const multiplayer_session_admission_plan resumed = admission_plan(
                    multiplayer_session_admission_kind::resume, 2, 102, 2, 2, true, false, 1 );
        CHECK( lifecycle.plan_admission( resumed ).origin() ==
               multiplayer_root_admission_origin::dormant );
    }

    CHECK( lifecycle.record_graceful_transport_closed( binding ).status ==
           multiplayer_root_lifecycle_status::duplicate );
    CHECK( lifecycle.snapshot().connection ==
           multiplayer_root_connection_state::disconnected );
    CHECK( lifecycle.record_departure_accepted(
               binding, multiplayer_root_departure_kind::graceful_release, now,
               std::chrono::seconds( 30 ), 77 ).status ==
           multiplayer_root_lifecycle_status::duplicate );
}

TEST_CASE( "multiplayer_selected_root_resume_beats_timeout_and_stales_old_events",
           "[multiplayer][root_lifecycle]" )
{
    multiplayer_selected_root_lifecycle lifecycle(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    const multiplayer_session_binding first_binding = publish_initial_binding( lifecycle );
    begin_bound_turn( lifecycle, first_binding, 61 );
    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};

    REQUIRE( lifecycle.record_departure_accepted(
                 first_binding, multiplayer_root_departure_kind::transport_loss, now,
                 std::chrono::seconds( 10 ) ) );
    multiplayer_root_departure_ticket old_ticket = current_departure_ticket( lifecycle );
    REQUIRE( lifecycle.record_barrier_disconnected( old_ticket ) );
    old_ticket = current_departure_ticket( lifecycle );

    const multiplayer_session_admission_plan directory_plan = admission_plan(
                multiplayer_session_admission_kind::resume, 2, 102, 2, 2, true, false, 1 );
    const multiplayer_root_admission_plan resume = lifecycle.plan_admission( directory_plan );
    REQUIRE( resume );
    CHECK( resume.origin() == multiplayer_root_admission_origin::disconnected_grace );
    CHECK( resume.barrier_mode() ==
           multiplayer_root_barrier_resume_mode::advance_one_generation );
    CHECK( resume.published_effect() ==
           multiplayer_root_lifecycle_effect::resume_barrier_plus_one );
    REQUIRE( lifecycle.record_admission_published( resume ) );

    CHECK( lifecycle.snapshot().session_generation == 2 );
    CHECK( lifecycle.snapshot().barrier ==
           multiplayer_root_barrier_state::awaiting_command );
    CHECK( lifecycle.can_execute_command() );
    CHECK( lifecycle.evaluate_disconnect_timeout(
               old_ticket, now + std::chrono::seconds( 20 ),
               old_ticket.participant ).status ==
           multiplayer_root_lifecycle_status::stale_binding );
    CHECK( lifecycle.record_departure_accepted(
               first_binding, multiplayer_root_departure_kind::transport_loss, now,
               std::chrono::seconds( 10 ) ).status ==
           multiplayer_root_lifecycle_status::stale_binding );
    CHECK( lifecycle.record_admission_published( resume ).status ==
           multiplayer_root_lifecycle_status::duplicate );
}

TEST_CASE( "multiplayer_selected_root_terminal_departure_does_not_execute_second_wait",
           "[multiplayer][root_lifecycle]" )
{
    multiplayer_selected_root_lifecycle lifecycle(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    const multiplayer_session_binding binding = publish_initial_binding( lifecycle );
    const multiplayer_turn_participant_key participant = {
        root_player_id(), binding.session_generation
    };
    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 66, { participant } ) );
    REQUIRE( lifecycle.begin_turn( binding, participant, 66 ) );
    REQUIRE( scheduler.record_action_result(
                 participant, multiplayer_turn_action_disposition::accepted_finished ) );
    REQUIRE( scheduler.stage() == multiplayer_turn_scheduler_stage::world_ready );
    REQUIRE( lifecycle.record_connected_barrier_terminal( binding, participant, 66 ) );
    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};

    const multiplayer_root_lifecycle_result departure =
        lifecycle.record_departure_accepted(
            binding, multiplayer_root_departure_kind::graceful_release, now,
            std::chrono::seconds( 30 ), 88 );
    REQUIRE( departure );
    CHECK( departure.next_effect == multiplayer_root_lifecycle_effect::claim_world );
    CHECK( lifecycle.snapshot().barrier == multiplayer_root_barrier_state::terminal );
    CHECK_FALSE( lifecycle.snapshot().forced_wait_requested );
    CHECK_FALSE( lifecycle.can_create_guard() );

    const multiplayer_root_departure_ticket ticket = current_departure_ticket( lifecycle );
    CHECK( lifecycle.record_barrier_disconnected( ticket ).status ==
           multiplayer_root_lifecycle_status::invalid_transition );
    CHECK( lifecycle.record_forced_wait_pending( ticket ).status ==
           multiplayer_root_lifecycle_status::invalid_transition );
    CHECK( lifecycle.record_forced_wait_terminal( ticket ).status ==
           multiplayer_root_lifecycle_status::invalid_transition );
    const std::optional<multiplayer_world_ticket> world_ticket = scheduler.claim_world();
    REQUIRE( world_ticket );
    const multiplayer_root_lifecycle_result claimed =
        lifecycle.record_world_claimed( ticket, *world_ticket );
    REQUIRE( claimed );
    CHECK( claimed.next_effect ==
           multiplayer_root_lifecycle_effect::execute_claimed_world );
    CHECK( lifecycle.record_world_claimed( ticket, *world_ticket ).status ==
           multiplayer_root_lifecycle_status::duplicate );
    REQUIRE( scheduler.record_world_completed( *world_ticket ) );
    REQUIRE( lifecycle.record_world_completed( ticket, *world_ticket ) );
    const multiplayer_root_lifecycle_result offline =
        lifecycle.record_runtime_offline( ticket );
    REQUIRE( offline );
    CHECK( offline.next_effect ==
           multiplayer_root_lifecycle_effect::complete_graceful_release );
    REQUIRE( lifecycle.record_graceful_completion_queued( binding, 88 ) );
    REQUIRE( lifecycle.record_graceful_transport_closed( binding ) );
}

TEST_CASE( "multiplayer_selected_root_connected_turn_returns_to_next_safe_boundary",
           "[multiplayer][root_lifecycle][scheduler]" )
{
    multiplayer_selected_root_lifecycle lifecycle(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    const multiplayer_session_binding binding = publish_initial_binding( lifecycle );
    const multiplayer_turn_participant_key participant = {
        root_player_id(), binding.session_generation
    };
    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 67, { participant } ) );
    REQUIRE( lifecycle.begin_turn( binding, participant, 67 ) );
    REQUIRE( scheduler.record_action_result(
                 participant, multiplayer_turn_action_disposition::accepted_finished ) );

    const multiplayer_root_lifecycle_result terminal =
        lifecycle.record_connected_barrier_terminal( binding, participant, 67 );
    REQUIRE( terminal );
    CHECK( terminal.next_effect == multiplayer_root_lifecycle_effect::claim_world );
    CHECK_FALSE( lifecycle.can_create_guard() );
    const std::optional<multiplayer_world_ticket> world_ticket = scheduler.claim_world();
    REQUIRE( world_ticket );
    REQUIRE( lifecycle.record_connected_world_claimed(
                 binding, participant, *world_ticket ) );
    REQUIRE( scheduler.record_world_completed( *world_ticket ) );
    const multiplayer_root_lifecycle_result completed =
        lifecycle.record_connected_world_completed( binding, participant, *world_ticket );
    REQUIRE( completed );
    CHECK( completed.next_effect == multiplayer_root_lifecycle_effect::begin_next_turn );
    CHECK( lifecycle.record_connected_world_completed(
               binding, participant, *world_ticket ).status ==
           multiplayer_root_lifecycle_status::duplicate );
    CHECK( lifecycle.can_begin_turn() );
    CHECK_FALSE( lifecycle.can_execute_command() );
    CHECK_FALSE( lifecycle.can_create_guard() );
    CHECK( lifecycle.can_save_or_shutdown() );
    CHECK( lifecycle.begin_turn( binding, participant, 67 ).status ==
           multiplayer_root_lifecycle_status::invalid_transition );
    REQUIRE( scheduler.begin_turn( 68, { participant } ) );
    REQUIRE( lifecycle.begin_turn( binding, participant, 68 ) );
    CHECK( lifecycle.record_connected_barrier_terminal(
               binding, participant, 67 ).status ==
           multiplayer_root_lifecycle_status::invalid_transition );
}

TEST_CASE( "multiplayer_selected_root_world_processing_departure_finishes_world_before_offline",
           "[multiplayer][root_lifecycle][scheduler]" )
{
    multiplayer_selected_root_lifecycle lifecycle(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    const multiplayer_session_binding binding = publish_initial_binding( lifecycle );
    const multiplayer_turn_participant_key participant = {
        root_player_id(), binding.session_generation
    };
    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( 68, { participant } ) );
    REQUIRE( lifecycle.begin_turn( binding, participant, 68 ) );
    REQUIRE( scheduler.record_action_result(
                 participant, multiplayer_turn_action_disposition::accepted_finished ) );
    REQUIRE( lifecycle.record_connected_barrier_terminal( binding, participant, 68 ) );
    const std::optional<multiplayer_world_ticket> world_ticket = scheduler.claim_world();
    REQUIRE( world_ticket );
    const multiplayer_root_lifecycle_result claimed =
        lifecycle.record_connected_world_claimed( binding, participant, *world_ticket );
    REQUIRE( claimed );
    CHECK( claimed.next_effect ==
           multiplayer_root_lifecycle_effect::execute_claimed_world );

    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};
    const multiplayer_root_lifecycle_result departure =
        lifecycle.record_departure_accepted(
            binding, multiplayer_root_departure_kind::transport_loss, now,
            std::chrono::seconds( 5 ) );
    REQUIRE( departure );
    CHECK( departure.next_effect == multiplayer_root_lifecycle_effect::none );
    CHECK_FALSE( lifecycle.can_save_or_shutdown() );
    const multiplayer_root_departure_ticket ticket = current_departure_ticket( lifecycle );
    REQUIRE( scheduler.record_world_completed( *world_ticket ) );
    const multiplayer_root_lifecycle_result completed =
        lifecycle.record_world_completed( ticket, *world_ticket );
    REQUIRE( completed );
    CHECK( completed.next_effect ==
           multiplayer_root_lifecycle_effect::transition_runtime_offline );
    REQUIRE( lifecycle.record_runtime_offline( ticket ) );
    CHECK( lifecycle.snapshot().server == multiplayer_root_server_state::dormant );
    CHECK( lifecycle.can_save_or_shutdown() );
}

TEST_CASE( "multiplayer_selected_root_timeout_defers_resume_until_dormant",
           "[multiplayer][root_lifecycle]" )
{
    multiplayer_selected_root_lifecycle lifecycle(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    const multiplayer_session_binding binding = publish_initial_binding( lifecycle );
    begin_bound_turn( lifecycle, binding, 71 );
    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};

    REQUIRE( lifecycle.record_departure_accepted(
                 binding, multiplayer_root_departure_kind::transport_loss, now,
                 std::chrono::seconds( 1 ) ) );
    multiplayer_root_departure_ticket ticket = current_departure_ticket( lifecycle );
    REQUIRE( lifecycle.record_barrier_disconnected( ticket ) );
    ticket = current_departure_ticket( lifecycle );
    REQUIRE( lifecycle.evaluate_disconnect_timeout(
                 ticket, now + std::chrono::seconds( 1 ), ticket.participant ) );
    const multiplayer_session_admission_plan directory_plan = admission_plan(
                multiplayer_session_admission_kind::resume, 2, 102, 2, 2, true, false, 1 );
    CHECK( lifecycle.plan_admission( directory_plan ).result().status ==
           multiplayer_root_lifecycle_status::resume_deferred_until_boundary );
    multiplayer_session_admission_plan rejected_plan;
    CHECK( lifecycle.plan_admission( rejected_plan ).result().status ==
           multiplayer_root_lifecycle_status::invalid_transition );
    ticket = current_departure_ticket( lifecycle );
    REQUIRE( lifecycle.record_forced_wait_pending( ticket ) );

    CHECK( lifecycle.plan_admission( directory_plan ).result().status ==
           multiplayer_root_lifecycle_status::resume_deferred_until_boundary );

    ticket = current_departure_ticket( lifecycle );
    REQUIRE( lifecycle.record_forced_wait_terminal( ticket ) );
    ticket = current_departure_ticket( lifecycle );
    REQUIRE( lifecycle.record_world_claimed( ticket, { 71 } ) );
    ticket = current_departure_ticket( lifecycle );
    REQUIRE( lifecycle.record_world_completed( ticket, { 71 } ) );
    ticket = current_departure_ticket( lifecycle );
    REQUIRE( lifecycle.record_runtime_offline( ticket ) );

    const multiplayer_root_admission_plan dormant_resume =
        lifecycle.plan_admission( directory_plan );
    REQUIRE( dormant_resume );
    CHECK( dormant_resume.origin() == multiplayer_root_admission_origin::dormant );
    CHECK( dormant_resume.barrier_mode() ==
           multiplayer_root_barrier_resume_mode::no_open_barrier );
    const multiplayer_root_lifecycle_result published =
        lifecycle.record_admission_published( dormant_resume );
    REQUIRE( published );
    CHECK( published.next_effect == multiplayer_root_lifecycle_effect::begin_next_turn );
    CHECK( lifecycle.snapshot().verified_runtime_status ==
           multiplayer_player_status::active );
    CHECK( lifecycle.can_begin_turn() );
    CHECK_FALSE( lifecycle.can_execute_command() );
}

TEST_CASE( "multiplayer_selected_root_replay_repairs_both_barrier_generation_states",
           "[multiplayer][root_lifecycle]" )
{
    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};

    SECTION( "already_committed_barrier_uses_same_generation_rebind" ) {
        multiplayer_selected_root_lifecycle lifecycle(
            root_player_id(), "root-character", 2, multiplayer_player_status::active );
        const multiplayer_session_binding binding = publish_initial_binding( lifecycle, 2 );
        begin_bound_turn( lifecycle, binding, 81 );
        REQUIRE( lifecycle.record_departure_accepted(
                     binding, multiplayer_root_departure_kind::transport_loss, now,
                     std::chrono::seconds( 10 ) ) );
        multiplayer_root_departure_ticket ticket = current_departure_ticket( lifecycle );
        REQUIRE( lifecycle.record_barrier_disconnected( ticket ) );

        const multiplayer_session_admission_plan replayed = admission_plan(
                    multiplayer_session_admission_kind::resume, 2, 102, 2, 2, false, true, 1 );
        const multiplayer_root_admission_plan plan = lifecycle.plan_admission( replayed );
        REQUIRE( plan );
        CHECK( plan.barrier_mode() ==
               multiplayer_root_barrier_resume_mode::replay_same_generation );
        CHECK( plan.published_effect() ==
               multiplayer_root_lifecycle_effect::rebind_replayed_barrier_generation );
    }

    SECTION( "unpublished_commit_repairs_disconnected_barrier_generation" ) {
        multiplayer_selected_root_lifecycle lifecycle(
            root_player_id(), "root-character", 1, multiplayer_player_status::active );
        const multiplayer_session_binding binding = publish_initial_binding( lifecycle );
        begin_bound_turn( lifecycle, binding, 82 );
        REQUIRE( lifecycle.record_departure_accepted(
                     binding, multiplayer_root_departure_kind::transport_loss, now,
                     std::chrono::seconds( 10 ) ) );
        multiplayer_root_departure_ticket ticket = current_departure_ticket( lifecycle );
        REQUIRE( lifecycle.record_barrier_disconnected( ticket ) );

        const multiplayer_session_admission_plan first_resume = admission_plan(
                    multiplayer_session_admission_kind::resume, 2, 102, 2, 2, true, false, 1 );
        const multiplayer_root_admission_plan first_plan =
            lifecycle.plan_admission( first_resume );
        REQUIRE( first_plan );
        CHECK( first_plan.unpublished_effect() ==
               multiplayer_root_lifecycle_effect::repair_disconnected_barrier_generation );
        const multiplayer_root_lifecycle_result unpublished =
            lifecycle.record_admission_unpublished( first_plan );
        REQUIRE( unpublished );
        CHECK( unpublished.next_effect ==
               multiplayer_root_lifecycle_effect::wait_for_disconnect_deadline );
        REQUIRE( lifecycle.snapshot().participant );
        CHECK( lifecycle.snapshot().session_generation == 2 );
        CHECK( lifecycle.snapshot().participant->session_generation == 2 );
        CHECK( lifecycle.record_admission_unpublished( first_plan ).status ==
               multiplayer_root_lifecycle_status::duplicate );
        CHECK( lifecycle.evaluate_disconnect_timeout(
                   ticket, now + std::chrono::seconds( 10 ), ticket.participant ).status ==
               multiplayer_root_lifecycle_status::stale_binding );
        ticket = current_departure_ticket( lifecycle );
        CHECK( ticket.participant.session_generation == 2 );

        const multiplayer_session_admission_plan replayed = admission_plan(
                    multiplayer_session_admission_kind::resume, 3, 103, 3, 2, false, true, 1 );
        const multiplayer_root_admission_plan repair = lifecycle.plan_admission( replayed );
        REQUIRE( repair );
        CHECK( repair.barrier_mode() ==
               multiplayer_root_barrier_resume_mode::replay_same_generation );
        CHECK( repair.published_effect() ==
               multiplayer_root_lifecycle_effect::rebind_replayed_barrier_generation );
    }
}

TEST_CASE( "multiplayer_selected_root_dormant_unpublished_resume_stays_offline",
           "[multiplayer][root_lifecycle]" )
{
    multiplayer_selected_root_lifecycle lifecycle(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    const multiplayer_session_binding binding = publish_initial_binding( lifecycle );
    begin_bound_turn( lifecycle, binding, 91 );
    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};
    drive_unexpected_root_to_dormant( lifecycle, binding, 91, now );

    const multiplayer_session_admission_plan normal_resume = admission_plan(
                multiplayer_session_admission_kind::resume, 2, 102, 2, 2, true, false, 1 );
    const multiplayer_root_admission_plan normal_plan =
        lifecycle.plan_admission( normal_resume );
    REQUIRE( normal_plan );
    REQUIRE( normal_plan.origin() == multiplayer_root_admission_origin::dormant );
    REQUIRE( lifecycle.record_admission_unpublished( normal_plan ) );
    CHECK( lifecycle.snapshot().session_generation == 2 );
    CHECK( lifecycle.snapshot().server == multiplayer_root_server_state::dormant );
    CHECK( lifecycle.snapshot().verified_runtime_status ==
           multiplayer_player_status::offline );
    CHECK_FALSE( lifecycle.can_create_guard() );

    const multiplayer_session_admission_plan replayed = admission_plan(
                multiplayer_session_admission_kind::resume, 3, 103, 3, 2, false, true, 1 );
    const multiplayer_root_admission_plan replay_plan = lifecycle.plan_admission( replayed );
    REQUIRE( replay_plan );
    CHECK( replay_plan.origin() == multiplayer_root_admission_origin::dormant );
    CHECK( replay_plan.barrier_mode() ==
           multiplayer_root_barrier_resume_mode::no_open_barrier );
    REQUIRE( lifecycle.record_admission_published( replay_plan ) );
    CHECK( lifecycle.snapshot().server == multiplayer_root_server_state::running );
    CHECK( lifecycle.snapshot().verified_runtime_status ==
           multiplayer_player_status::active );
    CHECK( lifecycle.can_begin_turn() );
}

TEST_CASE( "multiplayer_selected_root_fault_is_fail_stop",
           "[multiplayer][root_lifecycle]" )
{
    multiplayer_selected_root_lifecycle lifecycle(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    const multiplayer_session_binding binding = publish_initial_binding( lifecycle );
    begin_bound_turn( lifecycle, binding, 101 );

    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};
    REQUIRE( lifecycle.record_departure_accepted(
                 binding, multiplayer_root_departure_kind::transport_loss, now,
                 std::chrono::seconds( 1 ) ) );
    multiplayer_root_departure_ticket wrong_ticket =
        current_departure_ticket( lifecycle );
    wrong_ticket.deadline += std::chrono::seconds( 1 );
    CHECK( lifecycle.record_barrier_disconnected( wrong_ticket ).status ==
           multiplayer_root_lifecycle_status::stale_binding );

    const multiplayer_root_lifecycle_result fault = lifecycle.latch_fault();
    REQUIRE( fault );
    CHECK( fault.next_effect == multiplayer_root_lifecycle_effect::fatal_shutdown );
    CHECK( lifecycle.snapshot().server == multiplayer_root_server_state::faulted );
    CHECK_FALSE( lifecycle.can_begin_turn() );
    CHECK_FALSE( lifecycle.can_execute_command() );
    CHECK_FALSE( lifecycle.can_create_guard() );
    CHECK_FALSE( lifecycle.can_save_or_shutdown() );
    CHECK( lifecycle.latch_fault().status == multiplayer_root_lifecycle_status::duplicate );
    CHECK( lifecycle.record_departure_accepted(
               binding, multiplayer_root_departure_kind::transport_loss,
               now,
               std::chrono::seconds( 1 ) ).status ==
           multiplayer_root_lifecycle_status::faulted );
}

TEST_CASE( "multiplayer_selected_root_admission_recipes_are_owner_bound_and_fail_stop",
           "[multiplayer][root_lifecycle]" )
{
    multiplayer_selected_root_lifecycle first(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    multiplayer_selected_root_lifecycle second(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    const multiplayer_session_admission_plan directory_plan = admission_plan(
                multiplayer_session_admission_kind::authentication, 1, 101, 1, 1,
                false, false );
    const multiplayer_root_admission_plan first_plan = first.plan_admission( directory_plan );
    REQUIRE( first_plan );

    const multiplayer_root_lifecycle_result wrong_owner =
        second.record_admission_published( first_plan );
    CHECK( wrong_owner.status ==
           multiplayer_root_lifecycle_status::external_state_mismatch );
    CHECK( wrong_owner.next_effect ==
           multiplayer_root_lifecycle_effect::fatal_shutdown );
    CHECK( second.snapshot().server == multiplayer_root_server_state::faulted );
    CHECK_FALSE( second.can_save_or_shutdown() );
    CHECK_FALSE( second.departure_ticket() );

    const multiplayer_root_admission_plan competing_plan = first.plan_admission(
                admission_plan( multiplayer_session_admission_kind::authentication, 2,
                                102, 2, 1, false, false ) );
    REQUIRE( competing_plan );
    REQUIRE( first.record_admission_published( first_plan ) );
    const multiplayer_root_lifecycle_result stale_completion =
        first.record_admission_published( competing_plan );
    CHECK( stale_completion.status ==
           multiplayer_root_lifecycle_status::external_state_mismatch );
    CHECK( stale_completion.next_effect ==
           multiplayer_root_lifecycle_effect::fatal_shutdown );
    CHECK( first.snapshot().server == multiplayer_root_server_state::faulted );
    CHECK_FALSE( first.can_save_or_shutdown() );
}

TEST_CASE( "multiplayer_selected_root_departure_tickets_are_owner_bound",
           "[multiplayer][root_lifecycle]" )
{
    multiplayer_selected_root_lifecycle first(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    multiplayer_selected_root_lifecycle second(
        root_player_id(), "root-character", 1, multiplayer_player_status::active );
    const multiplayer_session_binding first_binding = publish_initial_binding( first );
    const multiplayer_session_binding second_binding = publish_initial_binding( second );
    begin_bound_turn( first, first_binding, 109 );
    begin_bound_turn( second, second_binding, 109 );
    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};
    REQUIRE( first.record_departure_accepted(
                 first_binding, multiplayer_root_departure_kind::transport_loss, now,
                 std::chrono::seconds( 1 ) ) );
    REQUIRE( second.record_departure_accepted(
                 second_binding, multiplayer_root_departure_kind::transport_loss, now,
                 std::chrono::seconds( 1 ) ) );

    const multiplayer_root_departure_ticket first_ticket =
        current_departure_ticket( first );
    CHECK( second.record_barrier_disconnected( first_ticket ).status ==
           multiplayer_root_lifecycle_status::stale_binding );
    REQUIRE( second.record_barrier_disconnected(
                 current_departure_ticket( second ) ) );
}

TEST_CASE( "multiplayer_selected_root_contract_coordinates_real_runtime_and_scheduler",
           "[multiplayer][root_lifecycle][session_directory][scheduler][phase_adapter]" )
{
    registered_lifecycle_player player;
    REQUIRE( player );
    const shared_ptr_fast<multiplayer_player_runtime> runtime = player.runtime();
    REQUIRE( runtime );
    multiplayer_player_runtime *const runtime_address = runtime.get();
    avatar *const avatar_address = &runtime->player();
    runtime->player().set_moves( 100 );

    multiplayer_session_directory directory( g->multiplayer_players(), 2 );
    const multiplayer_session_admission_plan authentication = directory.plan_admission(
                runtime_admission_request( multiplayer_session_admission_kind::authentication,
                                           1, 901, 81, *runtime ) );
    REQUIRE( authentication );
    REQUIRE( directory.commit_admission( authentication ) );
    const std::optional<multiplayer_session_binding> binding =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( binding );

    multiplayer_selected_root_lifecycle lifecycle(
        runtime->player_id(), binding->character_id, runtime->session_generation(),
        runtime->status() );
    const multiplayer_root_admission_plan initial = lifecycle.plan_admission( authentication );
    REQUIRE( initial );
    REQUIRE( lifecycle.record_admission_published( initial ) );

    constexpr std::uint64_t shared_turn = 111;
    const multiplayer_turn_participant_key participant = {
        runtime->player_id(), runtime->session_generation()
    };
    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( shared_turn, { participant } ) );
    REQUIRE( lifecycle.begin_turn( *binding, participant, shared_turn ) );

    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};
    REQUIRE( directory.record_disconnected( *binding ) ==
             multiplayer_session_directory_status::success );
    REQUIRE( lifecycle.record_departure_accepted(
                 *binding, multiplayer_root_departure_kind::transport_loss, now,
                 std::chrono::seconds( 1 ) ) );
    const multiplayer_root_departure_ticket ticket = current_departure_ticket( lifecycle );
    REQUIRE( scheduler.mark_barrier_disconnected( participant ) );
    REQUIRE( lifecycle.record_barrier_disconnected( ticket ) );
    REQUIRE( lifecycle.evaluate_disconnect_timeout(
                 ticket, now + std::chrono::seconds( 1 ), participant ) );
    REQUIRE( scheduler.apply_disconnect_timeout(
                 participant, multiplayer_disconnect_timeout_policy::automatic_wait ) );
    REQUIRE( lifecycle.record_forced_wait_pending( ticket ) );

    multiplayer_turn_phase_adapter adapter( *g, scheduler );
    REQUIRE( adapter.execute_authoritative_wait( participant ) ==
             multiplayer_turn_phase_adapter_status::completed );
    REQUIRE( lifecycle.record_forced_wait_terminal( ticket ) );
    CHECK( runtime->player().get_moves() <= 0 );
    CHECK( runtime->status() == multiplayer_player_status::active );

    const std::optional<multiplayer_world_ticket> world_ticket = scheduler.claim_world();
    REQUIRE( world_ticket );
    REQUIRE( lifecycle.record_world_claimed( ticket, *world_ticket ) );
    int world_callbacks = 0;
    ++world_callbacks;
    REQUIRE( scheduler.record_world_completed( *world_ticket ) );
    REQUIRE( lifecycle.record_world_completed( ticket, *world_ticket ) );
    CHECK( world_callbacks == 1 );
    CHECK( runtime->status() == multiplayer_player_status::active );

    REQUIRE( directory.record_runtime_offline( lifecycle.runtime_key() ) ==
             multiplayer_session_directory_status::success );
    REQUIRE( lifecycle.record_runtime_offline( ticket ) );
    CHECK( runtime->status() == multiplayer_player_status::offline );
    CHECK( lifecycle.snapshot().server == multiplayer_root_server_state::dormant );
    CHECK_FALSE( lifecycle.can_create_guard() );

    const multiplayer_session_admission_request resume_request = runtime_admission_request(
                multiplayer_session_admission_kind::resume, 2, 902, 82, *runtime,
                participant.session_generation, 17, 19 );
    const multiplayer_session_admission_plan resume = directory.plan_admission( resume_request );
    REQUIRE( resume );
    const multiplayer_root_admission_plan root_resume = lifecycle.plan_admission( resume );
    REQUIRE( root_resume );
    REQUIRE( root_resume.origin() == multiplayer_root_admission_origin::dormant );
    REQUIRE( directory.commit_admission( resume ) );
    REQUIRE( lifecycle.record_admission_published( root_resume ) );
    CHECK( runtime.get() == runtime_address );
    CHECK( &runtime->player() == avatar_address );
    CHECK( runtime->status() == multiplayer_player_status::active );
    CHECK( runtime->session_generation() == participant.session_generation + 1 );
    CHECK( lifecycle.can_begin_turn() );
}

TEST_CASE( "multiplayer_selected_root_terminal_graceful_release_coordinates_directory_cleanup",
           "[multiplayer][root_lifecycle][session_directory][scheduler]" )
{
    registered_lifecycle_player player;
    REQUIRE( player );
    const shared_ptr_fast<multiplayer_player_runtime> runtime = player.runtime();
    REQUIRE( runtime );

    multiplayer_session_directory directory( g->multiplayer_players(), 2 );
    const multiplayer_session_admission_plan authentication = directory.plan_admission(
                runtime_admission_request( multiplayer_session_admission_kind::authentication,
                                           1, 921, 101, *runtime ) );
    REQUIRE( authentication );
    REQUIRE( directory.commit_admission( authentication ) );
    const std::optional<multiplayer_session_binding> binding =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( binding );

    multiplayer_selected_root_lifecycle lifecycle(
        runtime->player_id(), binding->character_id, runtime->session_generation(),
        runtime->status() );
    const multiplayer_root_admission_plan initial = lifecycle.plan_admission( authentication );
    REQUIRE( initial );
    REQUIRE( lifecycle.record_admission_published( initial ) );

    constexpr std::uint64_t shared_turn = 131;
    const multiplayer_turn_participant_key participant = {
        runtime->player_id(), runtime->session_generation()
    };
    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( shared_turn, { participant } ) );
    REQUIRE( lifecycle.begin_turn( *binding, participant, shared_turn ) );
    REQUIRE( scheduler.record_action_result(
                 participant, multiplayer_turn_action_disposition::accepted_finished ) );
    REQUIRE( lifecycle.record_connected_barrier_terminal(
                 *binding, participant, shared_turn ) );

    REQUIRE( directory.record_graceful_release_pending( *binding ) ==
             multiplayer_session_directory_status::success );
    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};
    const multiplayer_root_lifecycle_result departure =
        lifecycle.record_departure_accepted(
            *binding, multiplayer_root_departure_kind::graceful_release, now,
            std::chrono::seconds( 30 ), 99 );
    REQUIRE( departure );
    CHECK( departure.next_effect == multiplayer_root_lifecycle_effect::claim_world );
    const multiplayer_root_departure_ticket ticket = current_departure_ticket( lifecycle );

    SECTION( "ordered_ack_close" ) {
        const std::optional<multiplayer_world_ticket> world_ticket = scheduler.claim_world();
        REQUIRE( world_ticket );
        REQUIRE( lifecycle.record_world_claimed( ticket, *world_ticket ) );
        REQUIRE( scheduler.record_world_completed( *world_ticket ) );
        REQUIRE( lifecycle.record_world_completed( ticket, *world_ticket ) );
        REQUIRE( directory.record_runtime_offline( lifecycle.runtime_key() ) ==
                 multiplayer_session_directory_status::success );
        const multiplayer_root_lifecycle_result offline =
            lifecycle.record_runtime_offline( ticket );
        REQUIRE( offline );
        CHECK( offline.next_effect ==
               multiplayer_root_lifecycle_effect::complete_graceful_release );
        REQUIRE( lifecycle.record_graceful_completion_queued( *binding, 99 ) );
        REQUIRE( directory.record_disconnected( *binding ) ==
                 multiplayer_session_directory_status::success );
        REQUIRE( lifecycle.record_graceful_transport_closed( *binding ) );
    }

    SECTION( "early_transport_close_cancels_ack_and_unblocks_resume" ) {
        REQUIRE( directory.record_disconnected( *binding ) ==
                 multiplayer_session_directory_status::success );
        REQUIRE( lifecycle.record_graceful_transport_closed( *binding ) );
        const std::optional<multiplayer_world_ticket> world_ticket = scheduler.claim_world();
        REQUIRE( world_ticket );
        REQUIRE( lifecycle.record_world_claimed( ticket, *world_ticket ) );
        REQUIRE( scheduler.record_world_completed( *world_ticket ) );
        REQUIRE( lifecycle.record_world_completed( ticket, *world_ticket ) );
        REQUIRE( directory.record_runtime_offline( lifecycle.runtime_key() ) ==
                 multiplayer_session_directory_status::success );
        const multiplayer_root_lifecycle_result offline =
            lifecycle.record_runtime_offline( ticket );
        REQUIRE( offline );
        CHECK( offline.next_effect == multiplayer_root_lifecycle_effect::none );
        CHECK( lifecycle.record_graceful_completion_queued( *binding, 99 ).status ==
               multiplayer_root_lifecycle_status::invalid_transition );
    }

    const multiplayer_session_admission_request resume_request = runtime_admission_request(
                multiplayer_session_admission_kind::resume, 2, 922, 102, *runtime,
                participant.session_generation, 23, 29 );
    const multiplayer_session_admission_plan resume = directory.plan_admission( resume_request );
    REQUIRE( resume );
    const multiplayer_root_admission_plan root_resume = lifecycle.plan_admission( resume );
    REQUIRE( root_resume );
    CHECK( root_resume.origin() == multiplayer_root_admission_origin::dormant );
}

TEST_CASE( "multiplayer_selected_root_unpublished_grace_resume_repairs_real_generation",
           "[multiplayer][root_lifecycle][session_directory][scheduler][phase_adapter]" )
{
    registered_lifecycle_player player;
    REQUIRE( player );
    const shared_ptr_fast<multiplayer_player_runtime> runtime = player.runtime();
    REQUIRE( runtime );
    runtime->player().set_moves( 100 );

    multiplayer_session_directory directory( g->multiplayer_players(), 2 );
    const multiplayer_session_admission_plan authentication = directory.plan_admission(
                runtime_admission_request( multiplayer_session_admission_kind::authentication,
                                           1, 911, 91, *runtime ) );
    REQUIRE( authentication );
    REQUIRE( directory.commit_admission( authentication ) );
    const std::optional<multiplayer_session_binding> binding =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( binding );

    multiplayer_selected_root_lifecycle lifecycle(
        runtime->player_id(), binding->character_id, runtime->session_generation(),
        runtime->status() );
    const multiplayer_root_admission_plan initial = lifecycle.plan_admission( authentication );
    REQUIRE( initial );
    REQUIRE( lifecycle.record_admission_published( initial ) );

    constexpr std::uint64_t shared_turn = 121;
    const multiplayer_turn_participant_key original_participant = {
        runtime->player_id(), runtime->session_generation()
    };
    multiplayer_turn_scheduler scheduler;
    REQUIRE( scheduler.begin_turn( shared_turn, { original_participant } ) );
    REQUIRE( lifecycle.begin_turn( *binding, original_participant, shared_turn ) );

    const auto now = multiplayer_selected_root_lifecycle::clock::time_point{};
    REQUIRE( directory.record_disconnected( *binding ) ==
             multiplayer_session_directory_status::success );
    REQUIRE( lifecycle.record_departure_accepted(
                 *binding, multiplayer_root_departure_kind::transport_loss, now,
                 std::chrono::seconds( 1 ) ) );
    const multiplayer_root_departure_ticket original_ticket =
        current_departure_ticket( lifecycle );
    REQUIRE( scheduler.mark_barrier_disconnected( original_participant ) );
    REQUIRE( lifecycle.record_barrier_disconnected( original_ticket ) );

    const multiplayer_session_admission_request first_resume_request =
        runtime_admission_request( multiplayer_session_admission_kind::resume, 2,
                                   912, 92, *runtime,
                                   original_participant.session_generation, 17, 19 );
    const multiplayer_session_admission_plan first_resume =
        directory.plan_admission( first_resume_request );
    REQUIRE( first_resume );
    const multiplayer_root_admission_plan root_resume =
        lifecycle.plan_admission( first_resume );
    REQUIRE( root_resume );
    CHECK( root_resume.unpublished_effect() ==
           multiplayer_root_lifecycle_effect::repair_disconnected_barrier_generation );
    REQUIRE( directory.commit_admission( first_resume ) );
    REQUIRE( directory.record_admission_unpublished( root_resume.binding() ) ==
             multiplayer_session_directory_status::success );
    REQUIRE( scheduler.repair_disconnected_barrier_generation(
                 root_resume.old_participant(),
                 root_resume.committed_participant().session_generation ) );
    REQUIRE( lifecycle.record_admission_unpublished( root_resume ) );

    const multiplayer_turn_participant_key repaired_participant =
        root_resume.committed_participant();
    CHECK( runtime->session_generation() == repaired_participant.session_generation );
    CHECK( runtime->status() == multiplayer_player_status::active );
    CHECK( scheduler.participant_key( runtime->player_id() ) == repaired_participant );
    CHECK( scheduler.participant_state( runtime->player_id() ) ==
           multiplayer_turn_participant_state::disconnected_grace );
    CHECK( lifecycle.evaluate_disconnect_timeout(
               original_ticket, now + std::chrono::seconds( 1 ),
               original_participant ).status ==
           multiplayer_root_lifecycle_status::stale_binding );
    const multiplayer_root_departure_ticket repaired_ticket =
        current_departure_ticket( lifecycle );

    SECTION( "timeout_after_enqueue_failure_reaches_dormant" ) {
        REQUIRE( lifecycle.evaluate_disconnect_timeout(
                     repaired_ticket, now + std::chrono::seconds( 1 ),
                     repaired_participant ) );
        REQUIRE( scheduler.apply_disconnect_timeout(
                     repaired_participant,
                     multiplayer_disconnect_timeout_policy::automatic_wait ) );
        REQUIRE( lifecycle.record_forced_wait_pending( repaired_ticket ) );

        multiplayer_turn_phase_adapter adapter( *g, scheduler );
        REQUIRE( adapter.execute_authoritative_wait( repaired_participant ) ==
                 multiplayer_turn_phase_adapter_status::completed );
        REQUIRE( lifecycle.record_forced_wait_terminal( repaired_ticket ) );
        const std::optional<multiplayer_world_ticket> world_ticket = scheduler.claim_world();
        REQUIRE( world_ticket );
        REQUIRE( lifecycle.record_world_claimed( repaired_ticket, *world_ticket ) );
        REQUIRE( scheduler.record_world_completed( *world_ticket ) );
        REQUIRE( lifecycle.record_world_completed( repaired_ticket, *world_ticket ) );
        REQUIRE( directory.record_runtime_offline( lifecycle.runtime_key() ) ==
                 multiplayer_session_directory_status::success );
        REQUIRE( lifecycle.record_runtime_offline( repaired_ticket ) );
        CHECK( runtime->status() == multiplayer_player_status::offline );
        CHECK( lifecycle.snapshot().server == multiplayer_root_server_state::dormant );
    }

    SECTION( "same_generation_retry_rebinds_without_another_generation_advance" ) {
        const multiplayer_session_admission_request retry_request =
            runtime_admission_request( multiplayer_session_admission_kind::resume, 3,
                                       913, 93, *runtime,
                                       original_participant.session_generation, 17, 19 );
        const multiplayer_session_admission_plan retry =
            directory.plan_admission( retry_request );
        REQUIRE( retry );
        REQUIRE( retry.replays_committed_generation );
        const multiplayer_root_admission_plan root_retry = lifecycle.plan_admission( retry );
        REQUIRE( root_retry );
        CHECK( root_retry.barrier_mode() ==
               multiplayer_root_barrier_resume_mode::replay_same_generation );
        REQUIRE( directory.commit_admission( retry ) );
        REQUIRE( scheduler.rebind_replayed_barrier_participant(
                     root_retry.committed_participant() ) );
        REQUIRE( lifecycle.record_admission_published( root_retry ) );
        CHECK( runtime->session_generation() == repaired_participant.session_generation );
        CHECK( scheduler.participant_state( runtime->player_id() ) ==
               multiplayer_turn_participant_state::awaiting_command );
        CHECK( lifecycle.can_execute_command() );
        CHECK( lifecycle.evaluate_disconnect_timeout(
                   repaired_ticket, now + std::chrono::seconds( 2 ),
                   repaired_participant ).status ==
               multiplayer_root_lifecycle_status::stale_binding );
    }
}

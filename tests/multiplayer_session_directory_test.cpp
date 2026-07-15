#include "cata_catch.h"

#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#include "avatar.h"
#include "game.h"
#include "memory_fast.h"
#include "multiplayer_player_context.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_player_runtime.h"
#include "multiplayer_session_directory.h"
#include "multiplayer_session_generation.h"
#include "player_helpers.h"

namespace
{

multiplayer_session_id session_id( const std::uint8_t value )
{
    multiplayer_session_id result = {};
    result.fill( value );
    return result;
}

std::string character_id_string( const avatar &player )
{
    return std::to_string( player.getID().get_value() );
}

multiplayer_session_admission_request admission_request(
    const multiplayer_session_admission_kind kind,
    const std::uint64_t admission_id,
    const multiplayer_connection_id connection,
    const multiplayer_session_id &session,
    const multiplayer_player_runtime &runtime,
    const std::uint64_t expected_generation = 0,
    const std::uint64_t last_server_revision = 0,
    const std::uint64_t last_client_sequence = 0 )
{
    multiplayer_session_admission_request request;
    request.kind = kind;
    request.admission_id = admission_id;
    request.connection = connection;
    request.session = session;
    request.player_id = runtime.player_id().str();
    request.character_id = character_id_string( runtime.player() );
    request.expected_session_generation = expected_generation;
    request.last_server_revision = last_server_revision;
    request.last_client_sequence = last_client_sequence;
    return request;
}

multiplayer_session_runtime_key runtime_key( const multiplayer_player_runtime &runtime )
{
    return {
        runtime.player_id().str(), character_id_string( runtime.player() ),
        runtime.session_generation()
    };
}

class registered_directory_player
{
    public:
        registered_directory_player() : owner_( make_shared_fast<avatar>() ) {
            owner_->create( character_type::NOW );
            clear_character( *owner_ );
            owner_->setID( g->assign_npc_id(), true );
            registered_ = g->register_multiplayer_player( owner_ );
            if( registered_ ) {
                runtime_ = g->multiplayer_players().find_runtime( *owner_ );
            }
        }

        ~registered_directory_player() {
            if( !registered_ ) {
                return;
            }
            if( runtime_ != nullptr &&
                runtime_->status() == multiplayer_player_status::active ) {
                g->disconnect_multiplayer_player( runtime_->player_id() );
            }
            g->unregister_multiplayer_player( *owner_ );
        }

        registered_directory_player( const registered_directory_player & ) = delete;
        registered_directory_player &operator=( const registered_directory_player & ) = delete;

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

} // namespace

TEST_CASE( "multiplayer_session_directory_admits_disconnects_and_replays_exact_generations",
           "[multiplayer][session_directory]" )
{
    registered_directory_player player;
    REQUIRE( player );
    const shared_ptr_fast<multiplayer_player_runtime> runtime = player.runtime();
    REQUIRE( runtime );
    REQUIRE( runtime->status() == multiplayer_player_status::active );
    REQUIRE( runtime->session_generation() == 1 );

    multiplayer_session_directory directory( g->multiplayer_players(), 2 );
    std::string error;
    REQUIRE( directory.valid( error ) );

    const multiplayer_session_admission_request authentication = admission_request(
                multiplayer_session_admission_kind::authentication, 1, 101, session_id( 1 ),
                *runtime );
    const multiplayer_session_admission_plan authentication_plan =
        directory.plan_admission( authentication );
    REQUIRE( authentication_plan );
    CHECK( authentication_plan.committed_session_generation == 1 );
    CHECK_FALSE( authentication_plan.advances_generation );
    CHECK_FALSE( authentication_plan.replays_committed_generation );
    REQUIRE( directory.commit_admission( authentication_plan ) );
    CHECK_FALSE( directory.commit_admission( authentication_plan ) );
    CHECK( runtime->session_generation() == 1 );
    CHECK( directory.session_count() == 1 );
    CHECK( directory.connected_count() == 1 );

    const std::optional<multiplayer_session_binding> generation_one =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( generation_one );
    CHECK( generation_one->session_generation == 1 );
    CHECK( directory.matches_connected( *generation_one ) );
    REQUIRE( directory.record_disconnected( *generation_one ) ==
             multiplayer_session_directory_status::success );
    CHECK( directory.connected_count() == 0 );
    CHECK( runtime->status() == multiplayer_player_status::active );
    CHECK( runtime->session_generation() == 1 );

    constexpr std::uint64_t first_resume_revision = 17;
    constexpr std::uint64_t first_resume_sequence = 23;
    const multiplayer_session_admission_request first_resume = admission_request(
                multiplayer_session_admission_kind::resume, 2, 102, session_id( 2 ), *runtime, 1,
                first_resume_revision, first_resume_sequence );
    const multiplayer_session_admission_plan first_resume_plan =
        directory.plan_admission( first_resume );
    REQUIRE( first_resume_plan );
    CHECK( first_resume_plan.committed_session_generation == 2 );
    CHECK( first_resume_plan.advances_generation );
    CHECK_FALSE( first_resume_plan.replays_committed_generation );

    multiplayer_session_admission_request stale_resume = first_resume;
    stale_resume.admission_id = 20;
    stale_resume.connection = 120;
    stale_resume.session = session_id( 20 );
    const multiplayer_session_admission_plan stale_resume_plan =
        directory.plan_admission( stale_resume );
    REQUIRE( stale_resume_plan );
    REQUIRE( directory.commit_admission( first_resume_plan ) );
    CHECK_FALSE( directory.commit_admission( stale_resume_plan ) );
    CHECK( runtime->session_generation() == 2 );

    const std::optional<multiplayer_session_binding> generation_two =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( generation_two );
    CHECK( generation_two->connection == 102 );
    CHECK( generation_two->session_generation == 2 );

    multiplayer_session_binding stale_binding = *generation_two;
    stale_binding.connection = 101;
    CHECK( directory.record_disconnected( stale_binding ) ==
           multiplayer_session_directory_status::stale_connection );
    stale_binding = *generation_two;
    stale_binding.session = session_id( 9 );
    CHECK( directory.record_disconnected( stale_binding ) ==
           multiplayer_session_directory_status::stale_connection );
    stale_binding = *generation_two;
    stale_binding.session_generation = 1;
    CHECK( directory.record_disconnected( stale_binding ) ==
           multiplayer_session_directory_status::stale_connection );
    stale_binding = *generation_two;
    stale_binding.character_id += "-stale";
    CHECK( directory.record_disconnected( stale_binding ) ==
           multiplayer_session_directory_status::stale_connection );
    CHECK( directory.matches_connected( *generation_two ) );
    REQUIRE( directory.record_disconnected( *generation_two ) ==
             multiplayer_session_directory_status::success );

    // The generation-2 response was lost.  The exact old generation and replay fingerprint
    // bind a new connection without consuming generation 3.
    const multiplayer_session_admission_request replayed_resume = admission_request(
                multiplayer_session_admission_kind::resume, 3, 103, session_id( 3 ), *runtime, 1,
                first_resume_revision, first_resume_sequence );
    const multiplayer_session_admission_plan replayed_resume_plan =
        directory.plan_admission( replayed_resume );
    REQUIRE( replayed_resume_plan );
    CHECK( replayed_resume_plan.committed_session_generation == 2 );
    CHECK_FALSE( replayed_resume_plan.advances_generation );
    CHECK( replayed_resume_plan.replays_committed_generation );
    REQUIRE( directory.commit_admission( replayed_resume_plan ) );
    CHECK( runtime->session_generation() == 2 );
    const std::optional<multiplayer_session_binding> replayed_generation_two =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( replayed_generation_two );
    REQUIRE( directory.record_disconnected( *replayed_generation_two ) ==
             multiplayer_session_directory_status::success );

    multiplayer_session_admission_request conflicting_fingerprint = replayed_resume;
    conflicting_fingerprint.admission_id = 4;
    conflicting_fingerprint.connection = 104;
    conflicting_fingerprint.session = session_id( 4 );
    ++conflicting_fingerprint.last_client_sequence;
    CHECK( directory.plan_admission( conflicting_fingerprint ).status ==
           multiplayer_session_directory_status::conflicting_resume_replay );

    multiplayer_session_admission_request skipped_generation = replayed_resume;
    skipped_generation.admission_id = 5;
    skipped_generation.connection = 105;
    skipped_generation.session = session_id( 5 );
    skipped_generation.expected_session_generation = 3;
    CHECK( directory.plan_admission( skipped_generation ).status ==
           multiplayer_session_directory_status::stale_session_generation );

    multiplayer_session_admission_request wrong_identity = replayed_resume;
    wrong_identity.admission_id = 6;
    wrong_identity.connection = 106;
    wrong_identity.session = session_id( 6 );
    wrong_identity.character_id += "-wrong";
    CHECK( directory.plan_admission( wrong_identity ).status ==
           multiplayer_session_directory_status::identity_mismatch );
    CHECK( directory.connected_count() == 0 );
    CHECK( runtime->session_generation() == 2 );

    // Once generation 2 was accepted, the next ordinary resume advances exactly to 3.
    const multiplayer_session_admission_request second_resume = admission_request(
                multiplayer_session_admission_kind::resume, 7, 107, session_id( 7 ), *runtime, 2,
                19, 29 );
    const multiplayer_session_admission_plan second_resume_plan =
        directory.plan_admission( second_resume );
    REQUIRE( second_resume_plan );
    CHECK( second_resume_plan.committed_session_generation == 3 );
    CHECK( second_resume_plan.advances_generation );
    REQUIRE( directory.commit_admission( second_resume_plan ) );
    CHECK( runtime->session_generation() == 3 );
    const std::optional<multiplayer_session_binding> generation_three =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( generation_three );
    REQUIRE( directory.record_disconnected( *generation_three ) ==
             multiplayer_session_directory_status::success );

    // A fresh authentication of a previously admitted player also advances once.
    const multiplayer_session_admission_request fresh_authentication = admission_request(
                multiplayer_session_admission_kind::authentication, 8, 108, session_id( 8 ),
                *runtime );
    const multiplayer_session_admission_plan fresh_authentication_plan =
        directory.plan_admission( fresh_authentication );
    REQUIRE( fresh_authentication_plan );
    CHECK( fresh_authentication_plan.committed_session_generation == 4 );
    CHECK( fresh_authentication_plan.advances_generation );
    CHECK_FALSE( fresh_authentication_plan.replays_committed_generation );
    REQUIRE( directory.commit_admission( fresh_authentication_plan ) );
    CHECK( runtime->session_generation() == 4 );
    const std::optional<multiplayer_session_binding> generation_four =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( generation_four );
    REQUIRE( directory.record_disconnected( *generation_four ) ==
             multiplayer_session_directory_status::success );
}

TEST_CASE( "multiplayer_session_directory_offline_transition_requires_an_exact_safe_point",
           "[multiplayer][session_directory][player_bridge]" )
{
    registered_directory_player player;
    REQUIRE( player );
    const shared_ptr_fast<multiplayer_player_runtime> runtime = player.runtime();
    REQUIRE( runtime );

    multiplayer_session_directory directory( g->multiplayer_players(), 1 );
    const multiplayer_session_admission_plan authentication = directory.plan_admission(
                admission_request( multiplayer_session_admission_kind::authentication,
                                   1, 601, session_id( 51 ), *runtime ) );
    REQUIRE( authentication );
    REQUIRE( directory.commit_admission( authentication ) );
    const std::optional<multiplayer_session_binding> binding =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( binding );
    CHECK( *binding == *binding );

    const multiplayer_session_runtime_key exact_key = runtime_key( *runtime );
    CHECK( directory.record_runtime_offline( exact_key ) ==
           multiplayer_session_directory_status::invalid_lifecycle_state );
    CHECK( runtime->status() == multiplayer_player_status::active );
    REQUIRE( directory.record_disconnected( *binding ) ==
             multiplayer_session_directory_status::success );

    multiplayer_session_runtime_key invalid_player = exact_key;
    invalid_player.player_id = "not-a-player-id";
    CHECK( directory.record_runtime_offline( invalid_player ) ==
           multiplayer_session_directory_status::invalid_request );

    multiplayer_session_runtime_key missing_player = exact_key;
    missing_player.player_id = multiplayer_player_id::random().str();
    CHECK( directory.record_runtime_offline( missing_player ) ==
           multiplayer_session_directory_status::player_not_found );

    multiplayer_session_runtime_key wrong_character = exact_key;
    wrong_character.character_id += "-stale";
    CHECK( directory.record_runtime_offline( wrong_character ) ==
           multiplayer_session_directory_status::identity_mismatch );

    multiplayer_session_runtime_key wrong_generation = exact_key;
    ++wrong_generation.session_generation;
    CHECK( directory.record_runtime_offline( wrong_generation ) ==
           multiplayer_session_directory_status::stale_session_generation );

    multiplayer_session_directory_status worker_status =
        multiplayer_session_directory_status::success;
    std::thread worker( [&]() {
        worker_status = directory.record_runtime_offline( exact_key );
    } );
    worker.join();
    CHECK( worker_status == multiplayer_session_directory_status::not_simulation_thread );
    CHECK( runtime->status() == multiplayer_player_status::active );

    {
        multiplayer_active_player_guard guard( *g, runtime->player_owner() );
        REQUIRE( guard.is_engaged() );
        CHECK( directory.record_runtime_offline( exact_key ) ==
               multiplayer_session_directory_status::invalid_lifecycle_state );
        CHECK( runtime->status() == multiplayer_player_status::active );
    }

    REQUIRE( directory.record_runtime_offline( exact_key ) ==
             multiplayer_session_directory_status::success );
    CHECK( runtime->status() == multiplayer_player_status::offline );
    CHECK( directory.record_runtime_offline( exact_key ) ==
           multiplayer_session_directory_status::duplicate );
}

TEST_CASE( "multiplayer_session_directory_graceful_release_loses_commandability_but_stays_active",
           "[multiplayer][session_directory]" )
{
    registered_directory_player player;
    REQUIRE( player );
    const shared_ptr_fast<multiplayer_player_runtime> runtime = player.runtime();
    REQUIRE( runtime );

    multiplayer_session_directory directory( g->multiplayer_players(), 1 );
    const multiplayer_session_admission_plan authentication = directory.plan_admission(
                admission_request( multiplayer_session_admission_kind::authentication,
                                   1, 651, session_id( 56 ), *runtime ) );
    REQUIRE( authentication );
    REQUIRE( directory.commit_admission( authentication ) );
    const std::optional<multiplayer_session_binding> binding =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( binding );

    multiplayer_session_binding stale = *binding;
    stale.session = session_id( 57 );
    CHECK( directory.record_graceful_release_pending( stale ) ==
           multiplayer_session_directory_status::stale_connection );
    CHECK( directory.matches_connected( *binding ) );
    CHECK( runtime->status() == multiplayer_player_status::active );

    REQUIRE( directory.record_graceful_release_pending( *binding ) ==
             multiplayer_session_directory_status::success );
    CHECK_FALSE( directory.session_for_player( runtime->player_id().str() ) );
    CHECK_FALSE( directory.matches_connected( *binding ) );
    CHECK( directory.connected_count() == 0 );
    CHECK( runtime->status() == multiplayer_player_status::active );
    CHECK( runtime->session_generation() == binding->session_generation );
    CHECK( directory.record_graceful_release_pending( *binding ) ==
           multiplayer_session_directory_status::duplicate );
    REQUIRE( directory.record_session_confirmed( *binding ) ==
             multiplayer_session_directory_status::success );

    const multiplayer_session_admission_request blocked_resume = admission_request(
                multiplayer_session_admission_kind::resume, 2, 652, session_id( 58 ), *runtime,
                binding->session_generation, 1, 1 );
    CHECK( directory.plan_admission( blocked_resume ).status ==
           multiplayer_session_directory_status::already_connected );

    REQUIRE( directory.record_runtime_offline( runtime_key( *runtime ) ) ==
             multiplayer_session_directory_status::success );
    CHECK( runtime->status() == multiplayer_player_status::offline );
    CHECK( directory.record_graceful_release_pending( *binding ) ==
           multiplayer_session_directory_status::duplicate );

    REQUIRE( directory.record_disconnected( *binding ) ==
             multiplayer_session_directory_status::success );
    CHECK( directory.record_disconnected( *binding ) ==
           multiplayer_session_directory_status::stale_connection );
    CHECK( runtime->status() == multiplayer_player_status::offline );
}

TEST_CASE( "multiplayer_session_directory_reactivates_the_same_offline_runtime",
           "[multiplayer][session_directory]" )
{
    registered_directory_player player;
    REQUIRE( player );
    const shared_ptr_fast<multiplayer_player_runtime> runtime = player.runtime();
    REQUIRE( runtime );
    multiplayer_player_runtime *const runtime_address = runtime.get();
    avatar *const avatar_address = &runtime->player();

    multiplayer_session_directory directory( g->multiplayer_players(), 1 );
    const multiplayer_session_admission_plan authentication = directory.plan_admission(
                admission_request( multiplayer_session_admission_kind::authentication,
                                   1, 701, session_id( 61 ), *runtime ) );
    REQUIRE( authentication );
    REQUIRE( directory.commit_admission( authentication ) );
    const std::optional<multiplayer_session_binding> generation_one =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( generation_one );
    REQUIRE( directory.record_disconnected( *generation_one ) ==
             multiplayer_session_directory_status::success );
    REQUIRE( directory.record_runtime_offline( runtime_key( *runtime ) ) ==
             multiplayer_session_directory_status::success );
    REQUIRE( runtime->status() == multiplayer_player_status::offline );

    constexpr std::uint64_t revision = 41;
    constexpr std::uint64_t sequence = 43;
    const multiplayer_session_admission_request resume = admission_request(
                multiplayer_session_admission_kind::resume, 2, 702, session_id( 62 ), *runtime,
                1, revision, sequence );
    const multiplayer_session_admission_plan resume_plan = directory.plan_admission( resume );
    REQUIRE( resume_plan );
    CHECK( resume_plan.advances_generation );
    CHECK( resume_plan.committed_session_generation == 2 );
    REQUIRE( directory.commit_admission( resume_plan ) );
    CHECK( runtime.get() == runtime_address );
    CHECK( &runtime->player() == avatar_address );
    CHECK( runtime->status() == multiplayer_player_status::active );
    CHECK( runtime->session_generation() == 2 );

    const std::optional<multiplayer_session_binding> generation_two =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( generation_two );
    REQUIRE( directory.record_disconnected( *generation_two ) ==
             multiplayer_session_directory_status::success );
    REQUIRE( directory.record_runtime_offline( runtime_key( *runtime ) ) ==
             multiplayer_session_directory_status::success );
    REQUIRE( runtime->status() == multiplayer_player_status::offline );

    multiplayer_session_admission_request replay = resume;
    replay.admission_id = 3;
    replay.connection = 703;
    replay.session = session_id( 63 );
    const multiplayer_session_admission_plan replay_plan = directory.plan_admission( replay );
    REQUIRE( replay_plan );
    CHECK_FALSE( replay_plan.advances_generation );
    CHECK( replay_plan.replays_committed_generation );
    CHECK( replay_plan.committed_session_generation == 2 );
    REQUIRE( directory.commit_admission( replay_plan ) );
    CHECK( runtime.get() == runtime_address );
    CHECK( &runtime->player() == avatar_address );
    CHECK( runtime->status() == multiplayer_player_status::active );
    CHECK( runtime->session_generation() == 2 );
}

TEST_CASE( "multiplayer_session_directory_unpublished_dormant_admission_restores_offline_replay",
           "[multiplayer][session_directory]" )
{
    registered_directory_player player;
    REQUIRE( player );
    const shared_ptr_fast<multiplayer_player_runtime> runtime = player.runtime();
    REQUIRE( runtime );
    multiplayer_player_runtime *const runtime_address = runtime.get();

    multiplayer_session_directory directory( g->multiplayer_players(), 1 );
    const multiplayer_session_admission_plan authentication = directory.plan_admission(
                admission_request( multiplayer_session_admission_kind::authentication,
                                   1, 801, session_id( 71 ), *runtime ) );
    REQUIRE( authentication );
    REQUIRE( directory.commit_admission( authentication ) );
    const std::optional<multiplayer_session_binding> generation_one =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( generation_one );
    REQUIRE( directory.record_disconnected( *generation_one ) ==
             multiplayer_session_directory_status::success );
    REQUIRE( directory.record_runtime_offline( runtime_key( *runtime ) ) ==
             multiplayer_session_directory_status::success );

    constexpr std::uint64_t revision = 47;
    constexpr std::uint64_t sequence = 53;
    const multiplayer_session_admission_request resume = admission_request(
                multiplayer_session_admission_kind::resume, 2, 802, session_id( 72 ), *runtime,
                1, revision, sequence );
    const multiplayer_session_admission_plan resume_plan = directory.plan_admission( resume );
    REQUIRE( resume_plan );
    REQUIRE( directory.commit_admission( resume_plan ) );
    REQUIRE( runtime->status() == multiplayer_player_status::active );
    REQUIRE( runtime->session_generation() == 2 );
    const std::optional<multiplayer_session_binding> unpublished =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( unpublished );

    multiplayer_session_binding wrong_binding = *unpublished;
    wrong_binding.connection = 899;
    CHECK( directory.record_admission_unpublished( wrong_binding ) ==
           multiplayer_session_directory_status::stale_connection );
    CHECK( directory.matches_connected( *unpublished ) );
    CHECK( runtime->status() == multiplayer_player_status::active );

    REQUIRE( directory.record_admission_unpublished( *unpublished ) ==
             multiplayer_session_directory_status::success );
    CHECK_FALSE( directory.session_for_player( runtime->player_id().str() ) );
    CHECK( runtime->status() == multiplayer_player_status::offline );
    CHECK( runtime->session_generation() == 2 );

    multiplayer_session_admission_request replay = resume;
    replay.admission_id = 3;
    replay.connection = 803;
    replay.session = session_id( 73 );
    const multiplayer_session_admission_plan replay_plan = directory.plan_admission( replay );
    REQUIRE( replay_plan );
    CHECK_FALSE( replay_plan.advances_generation );
    CHECK( replay_plan.replays_committed_generation );
    CHECK( replay_plan.committed_session_generation == 2 );
    REQUIRE( directory.commit_admission( replay_plan ) );
    CHECK( runtime.get() == runtime_address );
    CHECK( runtime->status() == multiplayer_player_status::active );
    CHECK( runtime->session_generation() == 2 );
}

TEST_CASE( "multiplayer_session_directory_unpublished_cleanup_preserves_active_origin",
           "[multiplayer][session_directory]" )
{
    registered_directory_player player;
    REQUIRE( player );
    const shared_ptr_fast<multiplayer_player_runtime> runtime = player.runtime();
    REQUIRE( runtime );

    multiplayer_session_directory directory( g->multiplayer_players(), 1 );
    const multiplayer_session_admission_plan authentication = directory.plan_admission(
                admission_request( multiplayer_session_admission_kind::authentication,
                                   1, 811, session_id( 81 ), *runtime ) );
    REQUIRE( authentication );
    REQUIRE( directory.commit_admission( authentication ) );
    const std::optional<multiplayer_session_binding> unpublished =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( unpublished );

    REQUIRE( directory.record_admission_unpublished( *unpublished ) ==
             multiplayer_session_directory_status::success );
    CHECK_FALSE( directory.session_for_player( runtime->player_id().str() ) );
    CHECK( runtime->status() == multiplayer_player_status::active );
    CHECK( runtime->session_generation() == unpublished->session_generation );
    CHECK( directory.record_admission_unpublished( *unpublished ) ==
           multiplayer_session_directory_status::stale_connection );
}

TEST_CASE( "multiplayer_session_directory_rejects_cross_player_connection_and_session_reuse",
           "[multiplayer][session_directory]" )
{
    registered_directory_player alpha;
    registered_directory_player beta;
    REQUIRE( alpha );
    REQUIRE( beta );
    const shared_ptr_fast<multiplayer_player_runtime> alpha_runtime = alpha.runtime();
    const shared_ptr_fast<multiplayer_player_runtime> beta_runtime = beta.runtime();
    REQUIRE( alpha_runtime );
    REQUIRE( beta_runtime );

    multiplayer_session_directory directory( g->multiplayer_players(), 2 );
    const multiplayer_session_admission_plan alpha_plan = directory.plan_admission(
                admission_request( multiplayer_session_admission_kind::authentication,
                                   1, 401, session_id( 31 ), *alpha_runtime ) );
    REQUIRE( alpha_plan );
    REQUIRE( directory.commit_admission( alpha_plan ) );

    CHECK_FALSE( directory.plan_admission( admission_request(
            multiplayer_session_admission_kind::authentication,
            2, 401, session_id( 32 ), *beta_runtime ) ) );
    CHECK_FALSE( directory.plan_admission( admission_request(
            multiplayer_session_admission_kind::authentication,
            3, 402, session_id( 31 ), *beta_runtime ) ) );
    CHECK( directory.session_count() == 1 );
    CHECK( directory.connected_count() == 1 );

    const multiplayer_session_admission_plan beta_plan = directory.plan_admission(
                admission_request( multiplayer_session_admission_kind::authentication,
                                   4, 402, session_id( 32 ), *beta_runtime ) );
    REQUIRE( beta_plan );
    REQUIRE( directory.commit_admission( beta_plan ) );
    CHECK( directory.session_count() == 2 );
    CHECK( directory.connected_count() == 2 );

    const std::optional<multiplayer_session_binding> alpha_binding =
        directory.session_for_player( alpha_runtime->player_id().str() );
    const std::optional<multiplayer_session_binding> beta_binding =
        directory.session_for_player( beta_runtime->player_id().str() );
    REQUIRE( alpha_binding );
    REQUIRE( beta_binding );
    REQUIRE( directory.record_disconnected( *alpha_binding ) ==
             multiplayer_session_directory_status::success );
    REQUIRE( directory.record_disconnected( *beta_binding ) ==
             multiplayer_session_directory_status::success );
}

TEST_CASE( "multiplayer_session_directory_consumes_resume_replay_after_client_confirmation",
           "[multiplayer][session_directory]" )
{
    registered_directory_player player;
    REQUIRE( player );
    const shared_ptr_fast<multiplayer_player_runtime> runtime = player.runtime();
    REQUIRE( runtime );

    multiplayer_session_directory directory( g->multiplayer_players(), 1 );
    const multiplayer_session_admission_plan authentication = directory.plan_admission(
                admission_request( multiplayer_session_admission_kind::authentication,
                                   1, 501, session_id( 41 ), *runtime ) );
    REQUIRE( authentication );
    REQUIRE( directory.commit_admission( authentication ) );
    const std::optional<multiplayer_session_binding> generation_one =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( generation_one );
    REQUIRE( directory.record_disconnected( *generation_one ) ==
             multiplayer_session_directory_status::success );

    const multiplayer_session_admission_request resume = admission_request(
                multiplayer_session_admission_kind::resume, 2, 502, session_id( 42 ), *runtime,
                1, 31, 37 );
    const multiplayer_session_admission_plan resume_plan = directory.plan_admission( resume );
    REQUIRE( resume_plan );
    REQUIRE( directory.commit_admission( resume_plan ) );
    const std::optional<multiplayer_session_binding> generation_two =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( generation_two );
    REQUIRE( directory.record_session_confirmed( *generation_two ) ==
             multiplayer_session_directory_status::success );

    multiplayer_session_binding stale_confirmation = *generation_two;
    stale_confirmation.session = session_id( 43 );
    CHECK( directory.record_session_confirmed( stale_confirmation ) ==
           multiplayer_session_directory_status::stale_connection );
    REQUIRE( directory.record_disconnected( *generation_two ) ==
             multiplayer_session_directory_status::success );

    multiplayer_session_admission_request old_generation_retry = resume;
    old_generation_retry.admission_id = 3;
    old_generation_retry.connection = 503;
    old_generation_retry.session = session_id( 43 );
    CHECK( directory.plan_admission( old_generation_retry ).status ==
           multiplayer_session_directory_status::stale_session_generation );
}

TEST_CASE( "multiplayer_session_directory_stops_before_the_signed_save_generation_limit",
           "[multiplayer][session_directory]" )
{
    const std::uint64_t maximum_generation =
        multiplayer_session_generation_exclusive_limit - 1;
    REQUIRE( multiplayer_is_valid_session_generation( maximum_generation ) );
    REQUIRE( multiplayer_is_next_session_generation( maximum_generation - 1,
             maximum_generation ) );
    CHECK_FALSE( multiplayer_is_valid_session_generation(
                     multiplayer_session_generation_exclusive_limit ) );
    CHECK_FALSE( multiplayer_is_next_session_generation( maximum_generation,
                 multiplayer_session_generation_exclusive_limit ) );

    shared_ptr_fast<avatar> owner = make_shared_fast<avatar>();
    owner->create( character_type::NOW );
    clear_character( *owner );
    owner->setID( g->assign_npc_id(), true );
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        make_shared_fast<multiplayer_player_runtime>(
            owner, multiplayer_player_runtime::achievement_callback{},
            multiplayer_player_runtime::achievement_callback{},
            multiplayer_player_id::random(), maximum_generation - 2 );
    multiplayer_player_registry registry;
    REQUIRE( registry.register_player( runtime ) );
    REQUIRE( registry.begin_session( runtime->player_id() ) );
    REQUIRE( runtime->session_generation() == maximum_generation - 1 );

    multiplayer_session_directory directory( registry, 1 );
    const multiplayer_session_admission_plan bootstrap = directory.plan_admission(
                admission_request( multiplayer_session_admission_kind::authentication,
                                   1, 200, session_id( 10 ), *runtime ) );
    REQUIRE( bootstrap );
    CHECK( bootstrap.committed_session_generation == maximum_generation - 1 );
    CHECK_FALSE( bootstrap.advances_generation );
    REQUIRE( directory.commit_admission( bootstrap ) );
    const std::optional<multiplayer_session_binding> bootstrap_binding =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( bootstrap_binding );
    REQUIRE( directory.record_disconnected( *bootstrap_binding ) ==
             multiplayer_session_directory_status::success );

    const multiplayer_session_admission_request final_authentication = admission_request(
                multiplayer_session_admission_kind::authentication, 2, 201, session_id( 11 ),
                *runtime );
    const multiplayer_session_admission_plan final_plan =
        directory.plan_admission( final_authentication );
    REQUIRE( final_plan );
    CHECK( final_plan.committed_session_generation == maximum_generation );
    CHECK( final_plan.advances_generation );
    REQUIRE( directory.commit_admission( final_plan ) );
    CHECK( runtime->session_generation() == maximum_generation );
    const std::optional<multiplayer_session_binding> final_binding =
        directory.session_for_player( runtime->player_id().str() );
    REQUIRE( final_binding );
    REQUIRE( directory.record_disconnected( *final_binding ) ==
             multiplayer_session_directory_status::success );

    const multiplayer_session_admission_request exhausted_authentication = admission_request(
                multiplayer_session_admission_kind::authentication, 3, 202, session_id( 12 ),
                *runtime );
    const multiplayer_session_admission_plan exhausted_plan =
        directory.plan_admission( exhausted_authentication );
    CHECK( exhausted_plan.status ==
           multiplayer_session_directory_status::generation_exhausted );
    CHECK_FALSE( directory.commit_admission( exhausted_plan ) );
    CHECK( runtime->session_generation() == maximum_generation );
    CHECK( directory.connected_count() == 0 );
}

TEST_CASE( "multiplayer_session_directory_rejects_non_simulation_thread_planning",
           "[multiplayer][session_directory]" )
{
    registered_directory_player player;
    REQUIRE( player );
    const shared_ptr_fast<multiplayer_player_runtime> runtime = player.runtime();
    REQUIRE( runtime );

    multiplayer_session_directory directory( g->multiplayer_players(), 2 );
    const multiplayer_session_admission_request request = admission_request(
                multiplayer_session_admission_kind::authentication, 1, 301, session_id( 21 ),
                *runtime );

    bool valid = true;
    std::string validation_error;
    multiplayer_session_directory_status status =
        multiplayer_session_directory_status::success;
    std::thread worker( [&]() {
        valid = directory.valid( validation_error );
        status = directory.plan_admission( request ).status;
    } );
    worker.join();

    CHECK_FALSE( valid );
    CHECK( validation_error.find( "simulation thread" ) != std::string::npos );
    CHECK( status == multiplayer_session_directory_status::not_simulation_thread );
    CHECK( directory.session_count() == 0 );
    CHECK( directory.connected_count() == 0 );
    CHECK( runtime->session_generation() == 1 );
}

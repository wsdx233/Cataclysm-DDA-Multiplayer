#include "multiplayer_session_directory.h"

#include <algorithm>
#include <map>
#include <thread>
#include <utility>

#include "avatar.h"
#include "cata_assert.h"
#include "memory_fast.h"
#include "multiplayer_player_registry.h"
#include "multiplayer_player_runtime.h"
#include "multiplayer_session_generation.h"

namespace
{

bool session_is_empty( const multiplayer_session_id &session )
{
    return std::all_of( session.begin(), session.end(), []( const std::uint8_t byte ) {
        return byte == 0;
    } );
}

multiplayer_session_admission_plan rejected_plan(
    const multiplayer_session_admission_request &request,
    const multiplayer_session_directory_status status )
{
    multiplayer_session_admission_plan plan;
    plan.status = status;
    plan.request = request;
    plan.message = multiplayer_session_directory_status_message( status );
    return plan;
}

} // namespace

const char *multiplayer_session_directory_status_message(
    const multiplayer_session_directory_status status )
{
    switch( status ) {
        case multiplayer_session_directory_status::success:
            return "session admission succeeded";
        case multiplayer_session_directory_status::not_simulation_thread:
            return "session directory requires the simulation thread";
        case multiplayer_session_directory_status::invalid_request:
            return "session admission request is invalid";
        case multiplayer_session_directory_status::capacity_full:
            return "session directory capacity is full";
        case multiplayer_session_directory_status::player_not_found:
            return "session player is not registered";
        case multiplayer_session_directory_status::identity_mismatch:
            return "session player identity does not match the registered runtime";
        case multiplayer_session_directory_status::runtime_unavailable:
            return "session player runtime is unavailable";
        case multiplayer_session_directory_status::already_connected:
            return "session player already has an active connection";
        case multiplayer_session_directory_status::stale_session_generation:
            return "session generation is stale";
        case multiplayer_session_directory_status::generation_exhausted:
            return "session generation is exhausted";
        case multiplayer_session_directory_status::conflicting_resume_replay:
            return "resume retry does not match the last committed admission";
        case multiplayer_session_directory_status::stale_connection:
            return "session connection tuple is stale";
    }
    return "unknown session directory status";
}

multiplayer_protocol_rejection multiplayer_session_admission_rejection(
    const multiplayer_session_admission_kind kind,
    const multiplayer_session_directory_status status )
{
    if( status == multiplayer_session_directory_status::capacity_full ) {
        return multiplayer_protocol_rejection::server_full;
    }
    if( status == multiplayer_session_directory_status::player_not_found ||
        status == multiplayer_session_directory_status::identity_mismatch ) {
        return kind == multiplayer_session_admission_kind::resume ?
               multiplayer_protocol_rejection::session_expired :
               multiplayer_protocol_rejection::permission_denied;
    }
    if( status == multiplayer_session_directory_status::already_connected ||
        status == multiplayer_session_directory_status::runtime_unavailable ) {
        return multiplayer_protocol_rejection::invalid_state;
    }
    if( status == multiplayer_session_directory_status::stale_session_generation ||
        status == multiplayer_session_directory_status::generation_exhausted ||
        status == multiplayer_session_directory_status::conflicting_resume_replay ) {
        return kind == multiplayer_session_admission_kind::resume ?
               multiplayer_protocol_rejection::session_expired :
               multiplayer_protocol_rejection::invalid_state;
    }
    return multiplayer_protocol_rejection::internal_error;
}

class multiplayer_session_directory::impl
{
    public:
        struct resume_fingerprint {
            std::uint64_t expected_generation = 0;
            std::uint64_t last_server_revision = 0;
            std::uint64_t last_client_sequence = 0;
        };

        struct entry {
            shared_ptr_fast<multiplayer_player_runtime> runtime;
            std::string character_id;
            std::uint64_t generation = 0;
            std::uint64_t version = 0;
            std::optional<multiplayer_session_binding> binding;
            std::optional<resume_fingerprint> last_resume;
        };

        impl( const multiplayer_player_registry &registry, const std::size_t maximum_players ) :
            registry( registry ),
            maximum_players( maximum_players ),
            simulation_thread( std::this_thread::get_id() ) {
        }

        bool on_simulation_thread() const {
            return std::this_thread::get_id() == simulation_thread;
        }

        const multiplayer_player_registry &registry;
        std::size_t maximum_players;
        std::thread::id simulation_thread;
        std::map<std::string, entry, std::less<>> entries;
};

multiplayer_session_directory::multiplayer_session_directory(
    const multiplayer_player_registry &registry, const std::size_t maximum_players ) :
    impl_( std::make_unique<impl>( registry, maximum_players ) )
{
}

multiplayer_session_directory::~multiplayer_session_directory() = default;

bool multiplayer_session_directory::valid( std::string &error ) const
{
    if( !impl_->on_simulation_thread() ) {
        error = multiplayer_session_directory_status_message(
                    multiplayer_session_directory_status::not_simulation_thread );
        return false;
    }
    if( impl_->maximum_players < 1 || impl_->maximum_players > 4 ) {
        error = "session directory maximum players must be between one and four";
        return false;
    }
    error.clear();
    return true;
}

multiplayer_session_admission_plan multiplayer_session_directory::plan_admission(
    const multiplayer_session_admission_request &request ) const
{
    if( !impl_->on_simulation_thread() ) {
        return rejected_plan( request,
                              multiplayer_session_directory_status::not_simulation_thread );
    }
    if( request.admission_id == 0 || request.connection == 0 ||
        session_is_empty( request.session ) || request.player_id.empty() ||
        request.character_id.empty() ||
        ( request.kind == multiplayer_session_admission_kind::authentication &&
          request.expected_session_generation != 0 ) ||
        ( request.kind == multiplayer_session_admission_kind::resume &&
          !multiplayer_is_valid_session_generation( request.expected_session_generation ) ) ) {
        return rejected_plan( request, multiplayer_session_directory_status::invalid_request );
    }

    const multiplayer_player_id player_id =
        multiplayer_player_id::from_string( request.player_id );
    if( !player_id.is_valid() ) {
        return rejected_plan( request, multiplayer_session_directory_status::invalid_request );
    }
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        impl_->registry.find_by_player_id( player_id );
    if( runtime == nullptr || !impl_->registry.owns( runtime ) ) {
        return rejected_plan( request, multiplayer_session_directory_status::player_not_found );
    }
    if( request.character_id != std::to_string( runtime->player().getID().get_value() ) ) {
        return rejected_plan( request, multiplayer_session_directory_status::identity_mismatch );
    }
    if( runtime->status() == multiplayer_player_status::dead ) {
        return rejected_plan( request, multiplayer_session_directory_status::runtime_unavailable );
    }

    const bool connection_or_session_in_use = std::any_of(
    impl_->entries.begin(), impl_->entries.end(), [&request]( const auto & item ) {
        return item.first != request.player_id && item.second.binding &&
               ( item.second.binding->connection == request.connection ||
                 item.second.binding->session == request.session );
    } );
    if( connection_or_session_in_use ) {
        return rejected_plan( request, multiplayer_session_directory_status::already_connected );
    }

    const auto found = impl_->entries.find( request.player_id );
    if( found == impl_->entries.end() ) {
        if( request.kind != multiplayer_session_admission_kind::authentication ) {
            return rejected_plan( request,
                                  multiplayer_session_directory_status::stale_session_generation );
        }
        if( impl_->entries.size() >= impl_->maximum_players ) {
            return rejected_plan( request, multiplayer_session_directory_status::capacity_full );
        }
        const std::uint64_t current_generation = runtime->session_generation();
        multiplayer_session_admission_plan plan;
        plan.status = multiplayer_session_directory_status::success;
        plan.request = request;
        plan.directory_version = 0;
        if( runtime->status() == multiplayer_player_status::active ) {
            if( !multiplayer_is_valid_session_generation( current_generation ) ) {
                return rejected_plan( request,
                                      multiplayer_session_directory_status::runtime_unavailable );
            }
            plan.committed_session_generation = current_generation;
        } else {
            if( !multiplayer_is_next_session_generation( current_generation,
                    current_generation + 1 ) ) {
                return rejected_plan( request,
                                      multiplayer_session_directory_status::generation_exhausted );
            }
            plan.committed_session_generation = current_generation + 1;
            plan.advances_generation = true;
        }
        plan.message = multiplayer_session_directory_status_message( plan.status );
        return plan;
    }

    const impl::entry &entry = found->second;
    if( entry.runtime == nullptr || !impl_->registry.owns( entry.runtime ) ||
        entry.runtime.get() != runtime.get() || entry.character_id != request.character_id ||
        entry.generation != runtime->session_generation() ) {
        return rejected_plan( request, multiplayer_session_directory_status::runtime_unavailable );
    }
    if( entry.binding ) {
        return rejected_plan( request, multiplayer_session_directory_status::already_connected );
    }

    multiplayer_session_admission_plan plan;
    plan.status = multiplayer_session_directory_status::success;
    plan.request = request;
    plan.directory_version = entry.version;
    plan.message = multiplayer_session_directory_status_message( plan.status );

    if( request.kind == multiplayer_session_admission_kind::authentication ) {
        if( !multiplayer_is_next_session_generation( entry.generation, entry.generation + 1 ) ) {
            return rejected_plan( request,
                                  multiplayer_session_directory_status::generation_exhausted );
        }
        plan.committed_session_generation = entry.generation + 1;
        plan.advances_generation = true;
        return plan;
    }

    if( request.expected_session_generation == entry.generation ) {
        if( !multiplayer_is_next_session_generation( entry.generation, entry.generation + 1 ) ) {
            return rejected_plan( request,
                                  multiplayer_session_directory_status::generation_exhausted );
        }
        plan.committed_session_generation = entry.generation + 1;
        plan.advances_generation = true;
        return plan;
    }

    const bool replay_generation_matches =
        request.expected_session_generation < multiplayer_session_generation_exclusive_limit - 1 &&
        request.expected_session_generation + 1 == entry.generation;
    const bool replay_fingerprint_matches = entry.last_resume &&
                                            entry.last_resume->expected_generation == request.expected_session_generation &&
                                            entry.last_resume->last_server_revision == request.last_server_revision &&
                                            entry.last_resume->last_client_sequence == request.last_client_sequence;
    if( replay_generation_matches && replay_fingerprint_matches ) {
        plan.committed_session_generation = entry.generation;
        plan.replays_committed_generation = true;
        return plan;
    }
    if( replay_generation_matches && entry.last_resume ) {
        return rejected_plan( request,
                              multiplayer_session_directory_status::conflicting_resume_replay );
    }
    return rejected_plan( request,
                          multiplayer_session_directory_status::stale_session_generation );
}

bool multiplayer_session_directory::commit_admission(
    const multiplayer_session_admission_plan &plan )
{
    cata_assert( impl_->on_simulation_thread() );
    if( !impl_->on_simulation_thread() || !plan ) {
        return false;
    }

    const multiplayer_session_admission_plan authoritative_plan =
        plan_admission( plan.request );
    if( !authoritative_plan ||
        plan.committed_session_generation !=
        authoritative_plan.committed_session_generation ||
        plan.advances_generation != authoritative_plan.advances_generation ||
        plan.replays_committed_generation !=
        authoritative_plan.replays_committed_generation ||
        plan.directory_version != authoritative_plan.directory_version ) {
        return false;
    }
    const multiplayer_player_id player_id =
        multiplayer_player_id::from_string( authoritative_plan.request.player_id );
    const shared_ptr_fast<multiplayer_player_runtime> runtime =
        impl_->registry.find_by_player_id( player_id );
    if( runtime == nullptr || !impl_->registry.owns( runtime ) ||
        authoritative_plan.request.character_id !=
        std::to_string( runtime->player().getID().get_value() ) ) {
        return false;
    }

    auto found = impl_->entries.find( authoritative_plan.request.player_id );
    if( found == impl_->entries.end() ) {
        if( authoritative_plan.directory_version != 0 ||
            impl_->entries.size() >= impl_->maximum_players ||
            authoritative_plan.request.kind !=
            multiplayer_session_admission_kind::authentication ) {
            return false;
        }
    } else if( found->second.version != authoritative_plan.directory_version ||
               found->second.binding ||
               found->second.runtime.get() != runtime.get() ||
               found->second.generation != runtime->session_generation() ) {
        return false;
    }

    if( authoritative_plan.advances_generation ) {
        const std::uint64_t expected_old =
            authoritative_plan.committed_session_generation - 1;
        if( !runtime->transition_session_generation( expected_old,
                authoritative_plan.committed_session_generation ) ) {
            return false;
        }
    } else if( runtime->session_generation() !=
               authoritative_plan.committed_session_generation ||
               runtime->status() != multiplayer_player_status::active ) {
        return false;
    }

    if( found == impl_->entries.end() ) {
        impl::entry entry;
        entry.runtime = runtime;
        entry.character_id = authoritative_plan.request.character_id;
        found = impl_->entries.emplace( authoritative_plan.request.player_id,
                                        std::move( entry ) ).first;
    }
    impl::entry &entry = found->second;
    entry.generation = authoritative_plan.committed_session_generation;
    entry.binding = multiplayer_session_binding {
        authoritative_plan.request.connection,
        authoritative_plan.request.session,
        authoritative_plan.request.player_id,
        authoritative_plan.request.character_id,
        authoritative_plan.committed_session_generation
    };
    if( authoritative_plan.request.kind == multiplayer_session_admission_kind::resume &&
        !authoritative_plan.replays_committed_generation ) {
        entry.last_resume = impl::resume_fingerprint {
            authoritative_plan.request.expected_session_generation,
            authoritative_plan.request.last_server_revision,
            authoritative_plan.request.last_client_sequence
        };
    } else if( authoritative_plan.request.kind ==
               multiplayer_session_admission_kind::authentication ) {
        entry.last_resume.reset();
    }
    ++entry.version;
    return true;
}

multiplayer_session_directory_status multiplayer_session_directory::record_session_confirmed(
    const multiplayer_session_binding &binding )
{
    cata_assert( impl_->on_simulation_thread() );
    if( !impl_->on_simulation_thread() ) {
        return multiplayer_session_directory_status::not_simulation_thread;
    }
    const auto found = impl_->entries.find( binding.player_id );
    if( found == impl_->entries.end() || !found->second.binding ||
        found->second.binding->connection != binding.connection ||
        found->second.binding->session != binding.session ||
        found->second.binding->character_id != binding.character_id ||
        found->second.binding->session_generation != binding.session_generation ) {
        return multiplayer_session_directory_status::stale_connection;
    }
    if( found->second.last_resume ) {
        found->second.last_resume.reset();
        ++found->second.version;
    }
    return multiplayer_session_directory_status::success;
}

multiplayer_session_directory_status multiplayer_session_directory::record_disconnected(
    const multiplayer_session_binding &binding )
{
    cata_assert( impl_->on_simulation_thread() );
    if( !impl_->on_simulation_thread() ) {
        return multiplayer_session_directory_status::not_simulation_thread;
    }
    const auto found = impl_->entries.find( binding.player_id );
    if( found == impl_->entries.end() || !found->second.binding ||
        found->second.binding->connection != binding.connection ||
        found->second.binding->session != binding.session ||
        found->second.binding->character_id != binding.character_id ||
        found->second.binding->session_generation != binding.session_generation ) {
        return multiplayer_session_directory_status::stale_connection;
    }
    found->second.binding.reset();
    ++found->second.version;
    return multiplayer_session_directory_status::success;
}

std::optional<multiplayer_session_binding> multiplayer_session_directory::session_for_player(
    const std::string &player_id ) const
{
    cata_assert( impl_->on_simulation_thread() );
    if( !impl_->on_simulation_thread() ) {
        return std::nullopt;
    }
    const auto found = impl_->entries.find( player_id );
    return found == impl_->entries.end() ? std::nullopt : found->second.binding;
}

bool multiplayer_session_directory::matches_connected(
    const multiplayer_session_binding &binding ) const
{
    const std::optional<multiplayer_session_binding> current =
        session_for_player( binding.player_id );
    return current && current->connection == binding.connection &&
           current->session == binding.session &&
           current->character_id == binding.character_id &&
           current->session_generation == binding.session_generation;
}

std::size_t multiplayer_session_directory::session_count() const
{
    cata_assert( impl_->on_simulation_thread() );
    return impl_->on_simulation_thread() ? impl_->entries.size() : 0;
}

std::size_t multiplayer_session_directory::connected_count() const
{
    cata_assert( impl_->on_simulation_thread() );
    if( !impl_->on_simulation_thread() ) {
        return 0;
    }
    return static_cast<std::size_t>( std::count_if( impl_->entries.begin(), impl_->entries.end(),
    []( const auto & item ) {
        return item.second.binding.has_value();
    } ) );
}

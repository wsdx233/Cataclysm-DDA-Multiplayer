#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif

#define ASIO_NO_DEPRECATED
#define ASIO_STANDALONE
#include <asio.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

#include "multiplayer_crypto.h"
#include "multiplayer_protocol.h"
#include "multiplayer_transport.h"

namespace
{

using tcp = asio::ip::tcp;

void require( const bool condition, const std::string &message )
{
    if( !condition ) {
        throw std::runtime_error( message );
    }
}

void send_envelope( tcp::socket &socket, const multiplayer_protocol_envelope &envelope )
{
    multiplayer_transport_payload protocol_bytes;
    multiplayer_transport_payload frame;
    std::string error;
    require( multiplayer_encode_protocol_envelope( envelope, protocol_bytes, error ), error );
    require( multiplayer_encode_transport_frame( protocol_bytes, frame, error ), error );
    asio::write( socket, asio::buffer( frame ) );
}

multiplayer_protocol_envelope read_envelope( tcp::socket &socket )
{
    std::array<std::uint8_t, multiplayer_transport_frame_header_size> header = {};
    asio::read( socket, asio::buffer( header ) );
    const std::uint32_t size = static_cast<std::uint32_t>( header[0] ) << 24U |
                               static_cast<std::uint32_t>( header[1] ) << 16U |
                               static_cast<std::uint32_t>( header[2] ) << 8U |
                               static_cast<std::uint32_t>( header[3] );
    require( size > 0 && size <= multiplayer_transport_maximum_frame_size,
             "server returned an invalid transport frame length" );
    multiplayer_transport_payload bytes( size );
    asio::read( socket, asio::buffer( bytes ) );
    multiplayer_protocol_envelope envelope;
    std::string error;
    require( multiplayer_decode_protocol_envelope( bytes, envelope, error ), error );
    return envelope;
}

multiplayer_protocol_envelope build_hello( const std::string &build_id,
        const std::string &content_manifest, const std::int32_t save_version )
{
    multiplayer_client_hello hello;
    hello.client_kind = multiplayer_protocol_client_kind::headless_test;
    hello.build_id = build_id;
    hello.content_manifest = content_manifest;
    hello.server_state_schema = multiplayer_server_state_schema_version;
    hello.savegame_version = save_version;
    hello.capabilities = { { "semantic-scene", 1, true } };
    hello.client_nonce[0] = 1;
    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::client_hello;
    std::string error;
    require( multiplayer_build_client_hello_payload( hello, envelope.payload, error ), error );
    return envelope;
}

multiplayer_server_hello exchange_hello( tcp::socket &socket, const std::string &build_id,
        const std::string &content_manifest,
        const std::int32_t save_version )
{
    send_envelope( socket, build_hello( build_id, content_manifest, save_version ) );
    const multiplayer_protocol_envelope response = read_envelope( socket );
    multiplayer_server_hello hello;
    std::string error;
    require( multiplayer_parse_server_hello_payload( response, hello, error ), error );
    return hello;
}

std::string read_private_token( const std::filesystem::path &path )
{
    std::ifstream input( path, std::ios::binary );
    require( static_cast<bool>( input ), "unable to open bearer token file" );
    std::string token{ std::istreambuf_iterator<char>( input ),
                       std::istreambuf_iterator<char>() };
    while( !token.empty() && ( token.back() == '\n' || token.back() == '\r' ) ) {
        token.pop_back();
    }
    require( multiplayer_is_valid_bearer_token( token ), "bearer token file is invalid" );
    return token;
}

multiplayer_scene_snapshot read_scene( tcp::socket &socket,
                                       multiplayer_protocol_envelope *raw = nullptr )
{
    multiplayer_protocol_envelope envelope = read_envelope( socket );
    require( envelope.message_type == multiplayer_protocol_message_type::scene_snapshot,
             "expected a full scene snapshot" );
    multiplayer_scene_snapshot snapshot;
    std::string error;
    require( multiplayer_parse_scene_snapshot_payload( envelope, snapshot, error ), error );
    if( raw != nullptr ) {
        *raw = std::move( envelope );
    }
    return snapshot;
}

} // namespace

int main( int argc, char **argv )
{
    try {
        require( argc == 4, "usage: headless_client_smoke HOST PORT TOKEN_FILE" );
        const std::string host = argv[1];
        const unsigned long parsed_port = std::stoul( argv[2] );
        require( parsed_port > 0 && parsed_port <= std::numeric_limits<std::uint16_t>::max(),
                 "port is outside the TCP range" );
        const std::uint16_t port = static_cast<std::uint16_t>( parsed_port );
        const std::string bearer_token = read_private_token( argv[3] );
        asio::io_context io;
        const tcp::endpoint endpoint( asio::ip::make_address( host ), port );

        tcp::socket probe( io );
        probe.connect( endpoint );
        const multiplayer_server_hello discovered = exchange_hello(
                    probe, "incompatible-probe-build",
                    "sha256-0000000000000000000000000000000000000000000000000000000000000000",
                    1 );
        require( !discovered.accepted, "incompatible discovery handshake was accepted" );
        require( !discovered.build_id.empty() && !discovered.content_manifest.empty() &&
                 discovered.savegame_version > 0,
                 "rejected server hello omitted compatibility diagnostics" );
        asio::error_code ignored;
        probe.close( ignored );

        tcp::socket client( io );
        client.connect( endpoint );
        const multiplayer_server_hello accepted = exchange_hello(
                    client, discovered.build_id, discovered.content_manifest,
                    discovered.savegame_version );
        require( accepted.accepted, "compatible handshake was rejected: " + accepted.message );

        multiplayer_authenticate_request authentication;
        authentication.display_name = "Headless Smoke Client";
        authentication.bearer_token = bearer_token;
        multiplayer_protocol_envelope request;
        request.message_type = multiplayer_protocol_message_type::authenticate;
        request.sequence = 1;
        std::string error;
        require( multiplayer_build_authenticate_payload( authentication, request.payload, error ),
                 error );
        send_envelope( client, request );
        multiplayer_protocol_envelope response = read_envelope( client );
        multiplayer_authentication_result authenticated;
        require( multiplayer_parse_authentication_result_payload( response, authenticated, error ),
                 error );
        require( authenticated.accepted, "authentication was rejected: " + authenticated.message );
        const multiplayer_session_id first_session = response.session;
        require( first_session != multiplayer_session_id {}, "authentication returned an empty session" );

        multiplayer_protocol_envelope initial_scene_envelope;
        const multiplayer_scene_snapshot initial_scene = read_scene(
                    client, &initial_scene_envelope );
        require( initial_scene.server_revision > 0 && !initial_scene.tiles.empty(),
                 "authenticated client did not receive a usable full scene" );
        require( initial_scene.player.player_id == authenticated.player_id &&
                 initial_scene.player.character_id == authenticated.character_id,
                 "scene player identity does not match authentication" );

        multiplayer_resync_request resync_request;
        resync_request.client_revision = initial_scene.server_revision;
        resync_request.reason = "process smoke full snapshot request";
        request = {};
        request.message_type = multiplayer_protocol_message_type::resync_request;
        request.session = first_session;
        request.sequence = 2;
        require( multiplayer_build_resync_request_payload(
                     resync_request, request.payload, error ), error );
        send_envelope( client, request );
        multiplayer_protocol_envelope resynchronized_scene_envelope;
        const multiplayer_scene_snapshot resynchronized_scene = read_scene(
                    client, &resynchronized_scene_envelope );
        require( resynchronized_scene.server_revision == initial_scene.server_revision,
                 "resync request did not return the current full snapshot" );
        require( resynchronized_scene_envelope.payload == initial_scene_envelope.payload,
                 "initial resync changed the last completed scene payload" );
        client.close( ignored );

        multiplayer_resume_request resume;
        resume.resume_token = authenticated.resume_token;
        resume.last_server_revision = resynchronized_scene.server_revision;
        resume.last_client_sequence = 2;
        resume.session_generation = authenticated.session_generation;

        tcp::socket barrier_resumed_client( io );
        barrier_resumed_client.connect( endpoint );
        const multiplayer_server_hello barrier_resumed_hello = exchange_hello(
                    barrier_resumed_client, discovered.build_id, discovered.content_manifest,
                    discovered.savegame_version );
        require( barrier_resumed_hello.accepted,
                 "open-barrier resume connection handshake was rejected" );
        request = {};
        request.message_type = multiplayer_protocol_message_type::resume_request;
        request.sequence = 1;
        require( multiplayer_build_resume_request_payload( resume, request.payload, error ), error );
        send_envelope( barrier_resumed_client, request );
        response = read_envelope( barrier_resumed_client );
        multiplayer_resume_result resume_result;
        require( multiplayer_parse_resume_result_payload( response, resume_result, error ), error );
        require( resume_result.accepted && resume_result.full_snapshot_required,
                 "open-barrier session resume was rejected: " + resume_result.message );
        require( resume_result.session_generation == authenticated.session_generation + 1,
                 "open-barrier resume did not advance generation exactly once" );
        const std::uint64_t barrier_resumed_generation = resume_result.session_generation;
        const multiplayer_session_id barrier_resumed_session = response.session;
        require( barrier_resumed_session != first_session &&
                 barrier_resumed_session != multiplayer_session_id {},
                 "open-barrier resume did not rotate the authenticated session id" );
        multiplayer_protocol_envelope barrier_resumed_scene_envelope;
        const multiplayer_scene_snapshot barrier_resumed_scene = read_scene(
                    barrier_resumed_client, &barrier_resumed_scene_envelope );
        require( barrier_resumed_scene.server_revision ==
                 resynchronized_scene.server_revision,
                 "open-barrier resume did not retain the last completed revision" );
        require( barrier_resumed_scene_envelope.payload ==
                 resynchronized_scene_envelope.payload,
                 "open-barrier resume exposed a partial live scene" );

        resync_request.client_revision = barrier_resumed_scene.server_revision;
        resync_request.reason = "process smoke open-barrier cached snapshot request";
        request = {};
        request.message_type = multiplayer_protocol_message_type::resync_request;
        request.session = barrier_resumed_session;
        request.sequence = 3;
        require( multiplayer_build_resync_request_payload(
                     resync_request, request.payload, error ), error );
        send_envelope( barrier_resumed_client, request );
        multiplayer_protocol_envelope barrier_resynchronized_scene_envelope;
        const multiplayer_scene_snapshot barrier_resynchronized_scene = read_scene(
                    barrier_resumed_client, &barrier_resynchronized_scene_envelope );
        require( barrier_resynchronized_scene.server_revision ==
                 barrier_resumed_scene.server_revision,
                 "open-barrier resync changed the completed scene revision" );
        require( barrier_resynchronized_scene_envelope.payload ==
                 resynchronized_scene_envelope.payload,
                 "open-barrier resync exposed a partial live scene" );

        multiplayer_player_command command;
        command.client_sequence = 4;
        command.base_revision = barrier_resynchronized_scene.server_revision;
        command.kind = multiplayer_command_kind::wait;
        request = {};
        request.message_type = multiplayer_protocol_message_type::player_command;
        request.session = barrier_resumed_session;
        request.sequence = command.client_sequence;
        require( multiplayer_build_player_command_payload( command, request.payload, error ), error );
        const multiplayer_transport_payload original_command_payload = request.payload;
        send_envelope( barrier_resumed_client, request );

        response = read_envelope( barrier_resumed_client );
        multiplayer_command_result command_result;
        require( multiplayer_parse_command_result_payload( response, command_result, error ), error );
        require( command_result.status == multiplayer_command_status::accepted,
                 "authoritative wait command was rejected: " + command_result.message );
        require( command_result.client_sequence == command.client_sequence &&
                 command_result.server_revision == barrier_resynchronized_scene.server_revision,
                 "command result revision/sequence correlation is invalid" );
        require( command_result.moves_spent > 0,
                 "authoritative wait command did not spend player moves" );
        const multiplayer_scene_snapshot completed_scene = read_scene( barrier_resumed_client );
        require( completed_scene.server_revision > command_result.server_revision,
                 "server did not publish one completed owned-turn scene" );
        barrier_resumed_client.close( ignored );

        tcp::socket resumed_client( io );
        resumed_client.connect( endpoint );
        const multiplayer_server_hello resumed_hello = exchange_hello(
                    resumed_client, discovered.build_id, discovered.content_manifest,
                    discovered.savegame_version );
        require( resumed_hello.accepted, "resume connection handshake was rejected" );
        resume.last_server_revision = barrier_resynchronized_scene.server_revision;
        resume.last_client_sequence = 3;
        resume.session_generation = barrier_resumed_generation;
        request = {};
        request.message_type = multiplayer_protocol_message_type::resume_request;
        request.sequence = 1;
        require( multiplayer_build_resume_request_payload( resume, request.payload, error ), error );
        send_envelope( resumed_client, request );
        response = read_envelope( resumed_client );
        resume_result = {};
        require( multiplayer_parse_resume_result_payload( response, resume_result, error ), error );
        require( resume_result.accepted && resume_result.full_snapshot_required,
                 "session resume was rejected: " + resume_result.message );
        require( resume_result.session_generation == barrier_resumed_generation + 1,
                 "session resume did not advance generation exactly once" );
        const std::uint64_t resumed_generation = resume_result.session_generation;
        const multiplayer_session_id resumed_session = response.session;
        require( resumed_session != barrier_resumed_session &&
                 resumed_session != multiplayer_session_id {},
                 "resume did not rotate the authenticated session id" );
        const multiplayer_scene_snapshot resumed_scene = read_scene( resumed_client );
        require( resumed_scene.server_revision == completed_scene.server_revision,
                 "resume full snapshot is not at the current authoritative revision" );

        request = {};
        request.message_type = multiplayer_protocol_message_type::player_command;
        request.session = resumed_session;
        request.sequence = command.client_sequence;
        request.payload = original_command_payload;
        send_envelope( resumed_client, request );
        response = read_envelope( resumed_client );
        multiplayer_command_result duplicate;
        require( multiplayer_parse_command_result_payload( response, duplicate, error ), error );
        require( duplicate.status == multiplayer_command_status::duplicate &&
                 duplicate.client_sequence == command.client_sequence &&
                 duplicate.server_revision == command_result.server_revision,
                 "reconnect replay was not answered from the idempotency cache" );

        multiplayer_player_command follow_up_command;
        follow_up_command.client_sequence = command.client_sequence + 1;
        follow_up_command.base_revision = resumed_scene.server_revision;
        follow_up_command.kind = multiplayer_command_kind::wait;
        request = {};
        request.message_type = multiplayer_protocol_message_type::player_command;
        request.session = resumed_session;
        request.sequence = follow_up_command.client_sequence;
        require( multiplayer_build_player_command_payload(
                     follow_up_command, request.payload, error ), error );
        send_envelope( resumed_client, request );
        response = read_envelope( resumed_client );
        multiplayer_command_result follow_up_result;
        require( multiplayer_parse_command_result_payload(
                     response, follow_up_result, error ), error );
        require( follow_up_result.status == multiplayer_command_status::accepted &&
                 follow_up_result.client_sequence == follow_up_command.client_sequence &&
                 follow_up_result.server_revision == resumed_scene.server_revision &&
                 follow_up_result.moves_spent > 0,
                 "fresh command after duplicate replay did not finish the resumed player phase" );
        const multiplayer_scene_snapshot replay_completed_scene = read_scene( resumed_client );
        require( replay_completed_scene.server_revision > resumed_scene.server_revision,
                 "resumed owned turn did not publish one new completed scene" );
        resumed_client.close( ignored );

        tcp::socket conflict_client( io );
        conflict_client.connect( endpoint );
        require( exchange_hello( conflict_client, discovered.build_id, discovered.content_manifest,
                                 discovered.savegame_version ).accepted,
                 "sequence-conflict connection handshake was rejected" );
        resume.session_generation = resumed_generation;
        request = {};
        request.message_type = multiplayer_protocol_message_type::resume_request;
        request.sequence = 1;
        require( multiplayer_build_resume_request_payload( resume, request.payload, error ), error );
        send_envelope( conflict_client, request );
        response = read_envelope( conflict_client );
        require( multiplayer_parse_resume_result_payload( response, resume_result, error ), error );
        require( resume_result.accepted &&
                 resume_result.session_generation == resumed_generation + 1,
                 "third resume was rejected or did not advance generation exactly once" );
        const multiplayer_session_id conflict_session = response.session;
        const multiplayer_scene_snapshot conflict_scene = read_scene( conflict_client );
        require( conflict_scene.server_revision == replay_completed_scene.server_revision,
                 "third resume did not replay the latest completed scene" );
        command.base_revision = conflict_scene.server_revision;
        request = {};
        request.message_type = multiplayer_protocol_message_type::player_command;
        request.session = conflict_session;
        request.sequence = command.client_sequence;
        require( multiplayer_build_player_command_payload( command, request.payload, error ), error );
        require( request.payload != original_command_payload,
                 "sequence-conflict command did not differ from the cached command" );
        send_envelope( conflict_client, request );
        std::array<std::uint8_t, 1> disconnected_byte = {};
        asio::error_code disconnect_error;
        const std::size_t disconnected_bytes = conflict_client.read_some(
                asio::buffer( disconnected_byte ), disconnect_error );
        require( disconnected_bytes == 0 && disconnect_error,
                 "sequence reuse with changed payload did not disconnect the client" );
        conflict_client.close( ignored );

        std::cout << "headless multiplayer smoke passed: handshake rejection/acceptance, auth, "
                  << "full scene/resync, open-barrier cached resume/resync, "
                  << "completed-turn scene, duplicate replay, "
                  << "same-turn follow-up, "
                  << "sequence-conflict disconnect; revisions "
                  << initial_scene.server_revision << " -> "
                  << completed_scene.server_revision << " -> "
                  << replay_completed_scene.server_revision << '\n';
        return 0;
    } catch( const std::exception &error ) {
        std::cerr << "headless multiplayer smoke failed: " << error.what() << '\n';
        return 1;
    }
}

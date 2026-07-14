#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cata_catch.h"
#include "get_version.h"
#include "multiplayer_protocol.h"

namespace
{

multiplayer_client_hello valid_client_hello()
{
    multiplayer_client_hello hello;
    hello.client_kind = multiplayer_protocol_client_kind::headless_test;
    hello.build_id = "server-build";
    hello.content_manifest = "test-content-0123456789abcdef";
    hello.savegame_version = 39;
    hello.capabilities = {
        { "semantic-scene", 1, true },
        { "optional-client-feature", 1, false }
    };
    hello.client_nonce[0] = 0x42;
    return hello;
}

multiplayer_protocol_envelope client_hello_envelope( const multiplayer_client_hello &hello )
{
    multiplayer_protocol_envelope envelope;
    envelope.protocol_major = hello.protocol_major;
    envelope.protocol_minor = hello.maximum_minor;
    envelope.message_type = multiplayer_protocol_message_type::client_hello;
    std::string error;
    REQUIRE( multiplayer_build_client_hello_payload( hello, envelope.payload, error ) );
    return envelope;
}

} // namespace

TEST_CASE( "multiplayer_build_id_excludes_local_ui_backend_suffixes",
           "[multiplayer][protocol]" )
{
    const std::string build_id = getMultiplayerBuildId();

    REQUIRE_FALSE( build_id.empty() );
    const bool clean_id = build_id.size() == 40;
    const bool dirty_id = build_id.size() == 46 && build_id.substr( 40 ) == "-dirty";
    REQUIRE( ( clean_id || dirty_id ) );
    CHECK( std::all_of( build_id.begin(), build_id.begin() + 40, []( const char ch ) {
        return ( ch >= '0' && ch <= '9' ) || ( ch >= 'a' && ch <= 'f' );
    } ) );
    CHECK( build_id.find( "+SDL3" ) == std::string::npos );

    multiplayer_client_hello hello = valid_client_hello();
    hello.build_id = build_id;
    multiplayer_protocol_envelope envelope = client_hello_envelope( hello );
    multiplayer_client_hello parsed;
    std::string error;
    REQUIRE( multiplayer_parse_client_hello_payload( envelope, parsed, error ) );
    CHECK( parsed.build_id == build_id );
}

TEST_CASE( "multiplayer_protocol_client_hello_round_trip_and_envelope_validation",
           "[multiplayer][protocol]" )
{
    const multiplayer_client_hello source = valid_client_hello();
    multiplayer_protocol_envelope source_envelope = client_hello_envelope( source );
    source_envelope.sequence = 0x0102030405060708ULL;
    source_envelope.session[0] = 0xaa;
    source_envelope.session[15] = 0x55;

    multiplayer_transport_payload encoded;
    std::string error;
    REQUIRE( multiplayer_encode_protocol_envelope( source_envelope, encoded, error ) );
    REQUIRE( encoded.size() == multiplayer_protocol_envelope_size + source_envelope.payload.size() );

    multiplayer_protocol_envelope decoded;
    REQUIRE( multiplayer_decode_protocol_envelope( encoded, decoded, error ) );
    CHECK( decoded.protocol_major == source.protocol_major );
    CHECK( decoded.protocol_minor == source.maximum_minor );
    CHECK( decoded.message_type == multiplayer_protocol_message_type::client_hello );
    CHECK( decoded.sequence == source_envelope.sequence );
    CHECK( decoded.session == source_envelope.session );

    multiplayer_client_hello parsed;
    REQUIRE( multiplayer_parse_client_hello_payload( decoded, parsed, error ) );
    CHECK( parsed.build_id == source.build_id );
    CHECK( parsed.content_manifest == source.content_manifest );
    CHECK( parsed.client_kind == source.client_kind );
    REQUIRE( parsed.capabilities.size() == source.capabilities.size() );
    CHECK( parsed.capabilities[0].id == "semantic-scene" );
    CHECK( parsed.capabilities[0].required );
    CHECK( parsed.client_nonce == source.client_nonce );

    SECTION( "bad magic" ) {
        encoded[0] ^= 0xffU;
        CHECK_FALSE( multiplayer_decode_protocol_envelope( encoded, decoded, error ) );
        CHECK( error.find( "magic" ) != std::string::npos );
    }
    SECTION( "unknown message type" ) {
        encoded[10] = 0x7f;
        encoded[11] = 0xff;
        CHECK_FALSE( multiplayer_decode_protocol_envelope( encoded, decoded, error ) );
        CHECK( error.find( "unknown" ) != std::string::npos );
    }
    SECTION( "unknown flags" ) {
        encoded[15] = 0x01;
        CHECK_FALSE( multiplayer_decode_protocol_envelope( encoded, decoded, error ) );
        CHECK( error.find( "flags" ) != std::string::npos );
    }
    SECTION( "declared payload mismatch" ) {
        encoded[43] ^= 0x01U;
        CHECK_FALSE( multiplayer_decode_protocol_envelope( encoded, decoded, error ) );
        CHECK( error.find( "lengths" ) != std::string::npos );
    }
    SECTION( "truncated envelope" ) {
        encoded.resize( multiplayer_protocol_envelope_size - 1 );
        CHECK_FALSE( multiplayer_decode_protocol_envelope( encoded, decoded, error ) );
    }
    SECTION( "malicious FlatBuffer" ) {
        REQUIRE( encoded.size() > multiplayer_protocol_envelope_size + 7 );
        encoded[multiplayer_protocol_envelope_size + 4] ^= 0xffU;
        CHECK_FALSE( multiplayer_decode_protocol_envelope( encoded, decoded, error ) );
        CHECK( error.find( "FlatBuffers" ) != std::string::npos );
    }
}

TEST_CASE( "multiplayer_protocol_hello_business_limits_are_enforced",
           "[multiplayer][protocol]" )
{
    multiplayer_client_hello hello = valid_client_hello();
    multiplayer_transport_payload payload;
    std::string error;

    hello.client_nonce.fill( 0 );
    CHECK_FALSE( multiplayer_build_client_hello_payload( hello, payload, error ) );
    CHECK( error.find( "nonce" ) != std::string::npos );

    hello = valid_client_hello();
    hello.capabilities.push_back( hello.capabilities.front() );
    CHECK_FALSE( multiplayer_build_client_hello_payload( hello, payload, error ) );
    CHECK( error.find( "unique" ) != std::string::npos );

    hello = valid_client_hello();
    hello.capabilities.front().id = "invalid/capability";
    CHECK_FALSE( multiplayer_build_client_hello_payload( hello, payload, error ) );

    hello = valid_client_hello();
    hello.minimum_minor = 2;
    hello.maximum_minor = 1;
    CHECK_FALSE( multiplayer_build_client_hello_payload( hello, payload, error ) );

    multiplayer_protocol_envelope envelope = client_hello_envelope( valid_client_hello() );
    envelope.protocol_minor = 1;
    multiplayer_client_hello parsed;
    CHECK_FALSE( multiplayer_parse_client_hello_payload( envelope, parsed, error ) );
    CHECK( error.find( "envelope" ) != std::string::npos );

    envelope = client_hello_envelope( valid_client_hello() );
    envelope.message_type = multiplayer_protocol_message_type::server_hello;
    multiplayer_transport_payload encoded;
    CHECK_FALSE( multiplayer_encode_protocol_envelope( envelope, encoded, error ) );
    CHECK( error.find( "does not match" ) != std::string::npos );
}

TEST_CASE( "multiplayer_protocol_negotiates_all_compatibility_axes_and_capabilities",
           "[multiplayer][protocol]" )
{
    const std::vector<multiplayer_protocol_capability> server_capabilities = {
        { "semantic-scene", 1, true },
        { "server-optional", 2, false }
    };
    multiplayer_client_hello client = valid_client_hello();
    multiplayer_server_hello result = multiplayer_negotiate_client_hello(
                                          client, server_capabilities, "server-build", "world-uuid",
                                          client.content_manifest, 39 );
    REQUIRE( result.accepted );
    CHECK( result.rejection == multiplayer_protocol_rejection::none );
    CHECK( result.protocol_minor == multiplayer_protocol_current_minor );

    SECTION( "server hello round trip" ) {
        result.server_nonce[3] = 0x7a;
        multiplayer_protocol_envelope envelope;
        envelope.message_type = multiplayer_protocol_message_type::server_hello;
        std::string error;
        REQUIRE( multiplayer_build_server_hello_payload( result, envelope.payload, error ) );
        multiplayer_transport_payload encoded;
        REQUIRE( multiplayer_encode_protocol_envelope( envelope, encoded, error ) );
        multiplayer_protocol_envelope decoded;
        REQUIRE( multiplayer_decode_protocol_envelope( encoded, decoded, error ) );
        multiplayer_server_hello parsed;
        REQUIRE( multiplayer_parse_server_hello_payload( decoded, parsed, error ) );
        CHECK( parsed.accepted );
        CHECK( parsed.world_id == "world-uuid" );
        CHECK( parsed.server_state_schema == multiplayer_server_state_schema_version );
        CHECK( parsed.savegame_version == 39 );
        CHECK( parsed.server_nonce == result.server_nonce );
    }

    SECTION( "major mismatch" ) {
        client.protocol_major = multiplayer_protocol_current_major + 1;
        result = multiplayer_negotiate_client_hello( client, server_capabilities,
                 "server-build", "world-uuid", client.content_manifest, 39 );
        CHECK_FALSE( result.accepted );
        CHECK( result.rejection == multiplayer_protocol_rejection::protocol_major_mismatch );
    }
    SECTION( "minor mismatch" ) {
        client.minimum_minor = multiplayer_protocol_current_minor + 1;
        client.maximum_minor = client.minimum_minor;
        result = multiplayer_negotiate_client_hello( client, server_capabilities,
                 "server-build", "world-uuid", client.content_manifest, 39 );
        CHECK_FALSE( result.accepted );
        CHECK( result.rejection == multiplayer_protocol_rejection::protocol_minor_mismatch );
    }
    SECTION( "build mismatch" ) {
        client.build_id = "different-build";
        result = multiplayer_negotiate_client_hello( client, server_capabilities,
                 "server-build", "world-uuid", client.content_manifest, 39 );
        CHECK_FALSE( result.accepted );
        CHECK( result.rejection == multiplayer_protocol_rejection::build_mismatch );
    }
    SECTION( "server state schema mismatch" ) {
        ++client.server_state_schema;
        result = multiplayer_negotiate_client_hello( client, server_capabilities,
                 "server-build", "world-uuid", client.content_manifest, 39 );
        CHECK_FALSE( result.accepted );
        CHECK( result.rejection == multiplayer_protocol_rejection::server_state_schema_mismatch );
    }
    SECTION( "savegame version mismatch" ) {
        ++client.savegame_version;
        result = multiplayer_negotiate_client_hello( client, server_capabilities,
                 "server-build", "world-uuid", client.content_manifest, 39 );
        CHECK_FALSE( result.accepted );
        CHECK( result.rejection == multiplayer_protocol_rejection::savegame_version_mismatch );
    }
    SECTION( "content mismatch" ) {
        result = multiplayer_negotiate_client_hello( client, server_capabilities,
                 "server-build", "world-uuid", "different-content", 39 );
        CHECK_FALSE( result.accepted );
        CHECK( result.rejection == multiplayer_protocol_rejection::content_mismatch );
    }
    SECTION( "missing required server capability" ) {
        client.capabilities.clear();
        result = multiplayer_negotiate_client_hello( client, server_capabilities,
                 "server-build", "world-uuid", client.content_manifest, 39 );
        CHECK_FALSE( result.accepted );
        CHECK( result.rejection == multiplayer_protocol_rejection::missing_capability );
    }
    SECTION( "unknown optional client capability" ) {
        client.capabilities.front().required = false;
        client.capabilities.front().id = "unknown-optional";
        client.capabilities.push_back( { "semantic-scene", 1, true } );
        result = multiplayer_negotiate_client_hello( client, server_capabilities,
                 "server-build", "world-uuid", client.content_manifest, 39 );
        CHECK( result.accepted );
    }
}

TEST_CASE( "multiplayer_protocol_command_and_scene_messages_round_trip_with_revision_rules",
           "[multiplayer][protocol]" )
{
    std::string error;
    multiplayer_resync_request resync;
    resync.client_revision = 3;
    resync.reason = "missed scene revision";
    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::resync_request;
    REQUIRE( multiplayer_build_resync_request_payload( resync, envelope.payload, error ) );
    multiplayer_resync_request parsed_resync;
    REQUIRE( multiplayer_parse_resync_request_payload( envelope, parsed_resync, error ) );
    CHECK( parsed_resync.client_revision == resync.client_revision );
    CHECK( parsed_resync.reason == resync.reason );
    resync.reason = std::string( 257, 'x' );
    CHECK_FALSE( multiplayer_build_resync_request_payload( resync, envelope.payload, error ) );

    multiplayer_player_command command;
    command.client_sequence = 9;
    command.base_revision = 4;
    command.kind = multiplayer_command_kind::move;
    command.direction = multiplayer_protocol_direction{ -1, 1, 0 };
    envelope = {};
    envelope.message_type = multiplayer_protocol_message_type::player_command;
    envelope.sequence = command.client_sequence;
    REQUIRE( multiplayer_build_player_command_payload( command, envelope.payload, error ) );
    multiplayer_player_command parsed_command;
    REQUIRE( multiplayer_parse_player_command_payload( envelope, parsed_command, error ) );
    CHECK( parsed_command.client_sequence == 9 );
    CHECK( parsed_command.base_revision == 4 );
    REQUIRE( parsed_command.direction );
    CHECK( parsed_command.direction->dx == -1 );
    CHECK( parsed_command.direction->dy == 1 );

    multiplayer_command_result result;
    result.client_sequence = command.client_sequence;
    result.status = multiplayer_command_status::accepted;
    result.server_revision = 5;
    result.moves_spent = 100;
    envelope = {};
    envelope.message_type = multiplayer_protocol_message_type::command_result;
    envelope.sequence = result.client_sequence;
    REQUIRE( multiplayer_build_command_result_payload( result, envelope.payload, error ) );
    multiplayer_command_result parsed_result;
    REQUIRE( multiplayer_parse_command_result_payload( envelope, parsed_result, error ) );
    CHECK( parsed_result.client_sequence == 9 );
    CHECK( parsed_result.server_revision == 5 );
    CHECK( parsed_result.moves_spent == 100 );

    multiplayer_scene_snapshot scene;
    scene.server_revision = 5;
    scene.turn = 12;
    scene.player.player_id = "12345678-1234-4234-9234-123456789abc";
    scene.player.character_id = "42";
    scene.player.revision = scene.server_revision;
    scene.player.position = { 100, 200, 0 };
    scene.player.moves = 50;
    scene.tiles.push_back( { { 100, 200, 0 }, "t_floor", "", "", 10 } );
    scene.entities.push_back( { multiplayer_visible_entity_kind::player, "player-42", 5,
        { 100, 200, 0 }, "avatar", "Test Player",
        multiplayer_visible_attitude::friendly, 100 } );
    envelope = {};
    envelope.message_type = multiplayer_protocol_message_type::scene_snapshot;
    envelope.sequence = scene.server_revision;
    REQUIRE( multiplayer_build_scene_snapshot_payload( scene, envelope.payload, error ) );
    CHECK( envelope.payload.size() < multiplayer_scene_snapshot_maximum_payload_size );
    multiplayer_scene_snapshot parsed_scene;
    REQUIRE( multiplayer_parse_scene_snapshot_payload( envelope, parsed_scene, error ) );
    CHECK( parsed_scene.server_revision == 5 );
    CHECK( parsed_scene.player.character_id == "42" );
    REQUIRE( parsed_scene.tiles.size() == 1 );
    CHECK( parsed_scene.tiles.front().terrain_id == "t_floor" );
    REQUIRE( parsed_scene.entities.size() == 1 );
    CHECK( parsed_scene.entities.front().stable_id == "player-42" );
}

TEST_CASE( "multiplayer_protocol_scene_snapshot_has_a_mobile_safe_size_budget",
           "[multiplayer][protocol]" )
{
    multiplayer_scene_snapshot scene;
    scene.server_revision = 1;
    scene.player.player_id = "12345678-1234-4234-9234-123456789abc";
    scene.player.character_id = "budget-character";
    scene.player.revision = scene.server_revision;
    const std::string terrain_id = "t_" + std::string( 125, 'a' );
    for( int index = 0; index < 4096; ++index ) {
        scene.tiles.push_back( { { index % 64, index / 64, 0 }, terrain_id, "", "", 0 } );
    }
    multiplayer_transport_payload payload;
    std::string error;
    CHECK_FALSE( multiplayer_build_scene_snapshot_payload( scene, payload, error ) );
    CHECK( error.find( "512 KiB" ) != std::string::npos );
}

TEST_CASE( "multiplayer_protocol_authentication_messages_are_bounded_and_typed",
           "[multiplayer][protocol]" )
{
    const std::string player_id = "12345678-1234-4234-9234-123456789abc";
    const std::string character_id = "42";
    const std::string token( 64, 'a' );
    multiplayer_authenticate_request request;
    request.player_id = player_id;
    request.character_id = character_id;
    request.display_name = "Test Player";
    request.bearer_token = token;

    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::authenticate;
    std::string error;
    REQUIRE( multiplayer_build_authenticate_payload( request, envelope.payload, error ) );
    multiplayer_transport_payload encoded;
    REQUIRE( multiplayer_encode_protocol_envelope( envelope, encoded, error ) );
    multiplayer_protocol_envelope decoded;
    REQUIRE( multiplayer_decode_protocol_envelope( encoded, decoded, error ) );
    multiplayer_authenticate_request parsed_request;
    REQUIRE( multiplayer_parse_authenticate_payload( decoded, parsed_request, error ) );
    CHECK( parsed_request.player_id == player_id );
    CHECK( parsed_request.character_id == character_id );
    CHECK( parsed_request.display_name == "Test Player" );
    CHECK( parsed_request.bearer_token == token );

    multiplayer_authentication_result source_result;
    source_result.accepted = true;
    source_result.player_id = player_id;
    source_result.character_id = character_id;
    source_result.resume_token = std::string( 64, 'b' );
    source_result.session_generation = 7;
    envelope.message_type = multiplayer_protocol_message_type::authentication_result;
    REQUIRE( multiplayer_build_authentication_result_payload( source_result, envelope.payload,
             error ) );
    multiplayer_authentication_result parsed_result;
    REQUIRE( multiplayer_parse_authentication_result_payload( envelope, parsed_result, error ) );
    CHECK( parsed_result.accepted );
    CHECK( parsed_result.player_id == player_id );
    CHECK( parsed_result.character_id == character_id );
    CHECK( parsed_result.session_generation == 7 );

    request.display_name = "bad\nname";
    CHECK_FALSE( multiplayer_build_authenticate_payload( request, envelope.payload, error ) );
    request.display_name = "Test Player";
    request.player_id = "not-a-uuid";
    CHECK_FALSE( multiplayer_build_authenticate_payload( request, envelope.payload, error ) );
    request.player_id = player_id;
    request.character_id = "not/a/stable/id";
    CHECK_FALSE( multiplayer_build_authenticate_payload( request, envelope.payload, error ) );

    source_result.rejection = multiplayer_protocol_rejection::authentication_failed;
    CHECK_FALSE( multiplayer_build_authentication_result_payload( source_result,
                 envelope.payload, error ) );
    source_result = {};
    source_result.rejection = multiplayer_protocol_rejection::authentication_failed;
    source_result.message = "authentication failed";
    REQUIRE( multiplayer_build_authentication_result_payload( source_result,
             envelope.payload, error ) );
}

TEST_CASE( "multiplayer_protocol_disconnect_notice_is_typed_and_bounded",
           "[multiplayer][protocol]" )
{
    multiplayer_disconnect_notice notice;
    notice.code = multiplayer_protocol_rejection::session_expired;
    notice.message = "server session expired";
    multiplayer_protocol_envelope envelope;
    envelope.message_type = multiplayer_protocol_message_type::disconnect_notice;
    std::string error;
    REQUIRE( multiplayer_build_disconnect_notice_payload( notice, envelope.payload, error ) );

    multiplayer_transport_payload encoded;
    REQUIRE( multiplayer_encode_protocol_envelope( envelope, encoded, error ) );
    multiplayer_protocol_envelope decoded;
    REQUIRE( multiplayer_decode_protocol_envelope( encoded, decoded, error ) );
    multiplayer_disconnect_notice parsed;
    REQUIRE( multiplayer_parse_disconnect_notice_payload( decoded, parsed, error ) );
    CHECK( parsed.code == notice.code );
    CHECK( parsed.message == notice.message );

    notice.message = "bad\nmessage";
    CHECK_FALSE( multiplayer_build_disconnect_notice_payload( notice, envelope.payload, error ) );
    notice.message.assign( 513, 'x' );
    CHECK_FALSE( multiplayer_build_disconnect_notice_payload( notice, envelope.payload, error ) );
    notice.message.clear();
    notice.code = static_cast<multiplayer_protocol_rejection>( 999 );
    CHECK_FALSE( multiplayer_build_disconnect_notice_payload( notice, envelope.payload, error ) );
}

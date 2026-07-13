#include "multiplayer_protocol.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "third-party/flatbuffers/flatbuffers.h"
#include <multiplayer_protocol_generated.h>

namespace protocol = cdda::multiplayer::protocol;

namespace
{

constexpr std::array<std::uint8_t, 4> protocol_magic = { 'C', 'D', 'M', 'P' };
constexpr std::uint32_t supported_protocol_flags = 0;
constexpr std::size_t maximum_capabilities = 32;
constexpr std::size_t maximum_capability_id_bytes = 64;
constexpr std::size_t maximum_build_id_bytes = 128;
constexpr std::size_t maximum_manifest_bytes = 128;
constexpr std::size_t maximum_world_id_bytes = 128;
constexpr std::size_t maximum_protocol_message_bytes = 512;
constexpr std::size_t maximum_display_name_bytes = 64;
constexpr std::size_t maximum_challenge_response_bytes = 64;
constexpr std::size_t maximum_command_message_bytes = 256;
constexpr std::size_t maximum_resync_reason_bytes = 256;
constexpr std::size_t maximum_scene_tiles = 16384;
constexpr std::size_t maximum_scene_entities = 4096;
constexpr std::size_t maximum_scene_identifier_bytes = 128;
constexpr std::size_t maximum_scene_display_name_bytes = 256;

static_assert( static_cast<std::uint16_t>( multiplayer_protocol_message_type::client_hello ) ==
               static_cast<std::uint16_t>( protocol::MessagePayload::ClientHello ) );
static_assert( static_cast<std::uint16_t>( multiplayer_protocol_message_type::chat_message ) ==
               static_cast<std::uint16_t>( protocol::MessagePayload::ChatMessage ) );
static_assert( static_cast<std::uint16_t>( multiplayer_protocol_rejection::internal_error ) ==
               static_cast<std::uint16_t>( protocol::RejectionCode::InternalError ) );
static_assert( static_cast<std::uint16_t>
               ( multiplayer_protocol_rejection::savegame_version_mismatch ) ==
               static_cast<std::uint16_t>( protocol::RejectionCode::SavegameVersionMismatch ) );

void append_u16( multiplayer_transport_payload &output, const std::uint16_t value )
{
    output.push_back( static_cast<std::uint8_t>( value >> 8U ) );
    output.push_back( static_cast<std::uint8_t>( value ) );
}

void append_u32( multiplayer_transport_payload &output, const std::uint32_t value )
{
    output.push_back( static_cast<std::uint8_t>( value >> 24U ) );
    output.push_back( static_cast<std::uint8_t>( value >> 16U ) );
    output.push_back( static_cast<std::uint8_t>( value >> 8U ) );
    output.push_back( static_cast<std::uint8_t>( value ) );
}

void append_u64( multiplayer_transport_payload &output, const std::uint64_t value )
{
    for( std::size_t index = 0; index < sizeof( value ); ++index ) {
        output.push_back( static_cast<std::uint8_t>( value >>( 56U - 8U * index ) ) );
    }
}

std::uint16_t read_u16( const std::uint8_t *input )
{
    return static_cast<std::uint16_t>( static_cast<std::uint16_t>( input[0] ) << 8U |
                                       static_cast<std::uint16_t>( input[1] ) );
}

std::uint32_t read_u32( const std::uint8_t *input )
{
    return static_cast<std::uint32_t>( input[0] ) << 24U |
           static_cast<std::uint32_t>( input[1] ) << 16U |
           static_cast<std::uint32_t>( input[2] ) << 8U |
           static_cast<std::uint32_t>( input[3] );
}

std::uint64_t read_u64( const std::uint8_t *input )
{
    std::uint64_t value = 0;
    for( std::size_t index = 0; index < sizeof( value ); ++index ) {
        value = value << 8U | input[index];
    }
    return value;
}

bool is_known_message_type( const multiplayer_protocol_message_type type )
{
    return type >= multiplayer_protocol_message_type::client_hello &&
           type <= multiplayer_protocol_message_type::chat_message;
}

bool is_valid_utf8( const std::string &text )
{
    std::size_t index = 0;
    while( index < text.size() ) {
        const std::uint8_t first = static_cast<std::uint8_t>( text[index] );
        std::size_t count = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if( first <= 0x7f ) {
            count = 1;
            codepoint = first;
        } else if( first >= 0xc2 && first <= 0xdf ) {
            count = 2;
            codepoint = first & 0x1fU;
            minimum = 0x80;
        } else if( first >= 0xe0 && first <= 0xef ) {
            count = 3;
            codepoint = first & 0x0fU;
            minimum = 0x800;
        } else if( first >= 0xf0 && first <= 0xf4 ) {
            count = 4;
            codepoint = first & 0x07U;
            minimum = 0x10000;
        } else {
            return false;
        }
        if( count > text.size() - index ) {
            return false;
        }
        for( std::size_t continuation = 1; continuation < count; ++continuation ) {
            const std::uint8_t next = static_cast<std::uint8_t>( text[index + continuation] );
            if( ( next & 0xc0U ) != 0x80U ) {
                return false;
            }
            codepoint = codepoint << 6U | ( next & 0x3fU );
        }
        if( codepoint < minimum || codepoint > 0x10ffffU ||
            ( codepoint >= 0xd800U && codepoint <= 0xdfffU ) ) {
            return false;
        }
        index += count;
    }
    return true;
}

bool is_valid_identifier( const std::string &value, const std::size_t maximum_size )
{
    return !value.empty() && value.size() <= maximum_size &&
    std::all_of( value.begin(), value.end(), []( const unsigned char ch ) {
        return ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) ||
               ( ch >= '0' && ch <= '9' ) || ch == '.' || ch == '_' || ch == '-' || ch == '+';
    } );
}

bool is_uuid_v4( const std::string &value )
{
    if( value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
        value[23] != '-' || value[14] != '4' ||
        std::string( "89ab" ).find( value[19] ) == std::string::npos ) {
        return false;
    }
    for( std::size_t index = 0; index < value.size(); ++index ) {
        if( index == 8 || index == 13 || index == 18 || index == 23 ) {
            continue;
        }
        const unsigned char ch = static_cast<unsigned char>( value[index] );
        if( !( ch >= '0' && ch <= '9' ) && !( ch >= 'a' && ch <= 'f' ) ) {
            return false;
        }
    }
    return true;
}

bool is_bearer_token( const std::string &value )
{
    return value.size() == 64 && std::all_of( value.begin(), value.end(),
    []( const unsigned char ch ) {
        return ( ch >= '0' && ch <= '9' ) || ( ch >= 'a' && ch <= 'f' );
    } );
}

bool is_safe_display_text( const std::string &value, const std::size_t maximum_size )
{
    return !value.empty() && value.size() <= maximum_size && is_valid_utf8( value ) &&
    std::none_of( value.begin(), value.end(), []( const unsigned char ch ) {
        return ch < 0x20 || ch == 0x7f;
    } );
}

template<typename Array>
bool contains_nonzero_byte( const Array &value )
{
    return std::any_of( value.begin(), value.end(), []( const std::uint8_t byte ) {
        return byte != 0;
    } );
}

bool validate_capabilities( const std::vector<multiplayer_protocol_capability> &capabilities,
                            std::string &error )
{
    if( capabilities.size() > maximum_capabilities ) {
        error = "protocol capability count exceeds 32";
        return false;
    }
    std::set<std::string> ids;
    for( const multiplayer_protocol_capability &capability : capabilities ) {
        if( !is_valid_identifier( capability.id, maximum_capability_id_bytes ) ||
            capability.version == 0 ) {
            error = "protocol capability has an invalid id or version";
            return false;
        }
        if( !ids.emplace( capability.id ).second ) {
            error = "protocol capability ids must be unique";
            return false;
        }
    }
    return true;
}

bool validate_client_hello( const multiplayer_client_hello &hello, std::string &error )
{
    if( hello.minimum_minor > hello.maximum_minor ) {
        error = "client protocol minor range is invalid";
        return false;
    }
    if( hello.client_kind < multiplayer_protocol_client_kind::graphical_desktop ||
        hello.client_kind > multiplayer_protocol_client_kind::headless_test ) {
        error = "client kind is invalid";
        return false;
    }
    if( !is_valid_identifier( hello.build_id, maximum_build_id_bytes ) ||
        !is_valid_identifier( hello.content_manifest, maximum_manifest_bytes ) ) {
        error = "client build id or content manifest is invalid";
        return false;
    }
    if( !contains_nonzero_byte( hello.client_nonce ) ) {
        error = "client nonce must not be all zero";
        return false;
    }
    if( hello.server_state_schema == 0 || hello.savegame_version <= 0 ) {
        error = "client compatibility schema versions must be positive";
        return false;
    }
    return validate_capabilities( hello.capabilities, error );
}

bool validate_server_hello( const multiplayer_server_hello &hello, std::string &error )
{
    if( !is_valid_identifier( hello.build_id, maximum_build_id_bytes ) ) {
        error = "server hello build id is invalid or oversized";
        return false;
    }
    if( hello.world_id.empty() || hello.world_id.size() > maximum_world_id_bytes ||
        !is_valid_utf8( hello.world_id ) ) {
        error = "server hello world id is invalid or oversized";
        return false;
    }
    if( !is_valid_identifier( hello.content_manifest, maximum_manifest_bytes ) ) {
        error = "server hello content manifest is invalid or oversized";
        return false;
    }
    if( hello.message.size() > maximum_protocol_message_bytes || !is_valid_utf8( hello.message ) ) {
        error = "server hello message is invalid or oversized";
        return false;
    }
    if( !contains_nonzero_byte( hello.server_nonce ) || hello.server_state_schema == 0 ||
        hello.savegame_version <= 0 ||
        hello.accepted != ( hello.rejection == multiplayer_protocol_rejection::none ) ) {
        error = "server hello nonce, compatibility versions, or acceptance state is invalid";
        return false;
    }
    return validate_capabilities( hello.capabilities, error );
}

bool validate_authenticate_request( const multiplayer_authenticate_request &request,
                                    std::string &error )
{
    if( ( !request.player_id.empty() && !is_uuid_v4( request.player_id ) ) ||
        ( !request.character_id.empty() &&
          !is_valid_identifier( request.character_id, maximum_capability_id_bytes ) ) ||
        !is_safe_display_text( request.display_name, maximum_display_name_bytes ) ||
        ( !request.bearer_token.empty() && !is_bearer_token( request.bearer_token ) ) ||
        request.challenge_response.size() > maximum_challenge_response_bytes ) {
        error = "authentication request contains an invalid or oversized field";
        return false;
    }
    return true;
}

bool validate_authentication_result( const multiplayer_authentication_result &result,
                                     std::string &error )
{
    const bool rejection_in_range = result.rejection >= multiplayer_protocol_rejection::none &&
                                    result.rejection <=
                                    multiplayer_protocol_rejection::savegame_version_mismatch;
    if( !rejection_in_range || result.message.size() > maximum_protocol_message_bytes ||
        ( !result.message.empty() && !is_valid_utf8( result.message ) ) ) {
        error = "authentication result contains an invalid rejection or message";
        return false;
    }
    if( result.accepted ) {
        if( result.rejection != multiplayer_protocol_rejection::none ||
            !is_uuid_v4( result.player_id ) ||
            !is_valid_identifier( result.character_id, maximum_capability_id_bytes ) ||
            !is_bearer_token( result.resume_token ) || result.session_generation == 0 ) {
            error = "accepted authentication result is incomplete";
            return false;
        }
    } else if( result.rejection == multiplayer_protocol_rejection::none ||
               !result.player_id.empty() || !result.character_id.empty() ||
               !result.resume_token.empty() || result.session_generation != 0 ) {
        error = "rejected authentication result contains accepted-session fields";
        return false;
    }
    return true;
}

bool validate_resume_result( const multiplayer_resume_result &result, std::string &error )
{
    const bool rejection_in_range = result.rejection >= multiplayer_protocol_rejection::none &&
                                    result.rejection <=
                                    multiplayer_protocol_rejection::savegame_version_mismatch;
    if( !rejection_in_range || result.message.size() > maximum_protocol_message_bytes ||
        ( !result.message.empty() && !is_valid_utf8( result.message ) ) ) {
        error = "resume result contains an invalid rejection or message";
        return false;
    }
    if( result.accepted ) {
        if( result.rejection != multiplayer_protocol_rejection::none ||
            !is_uuid_v4( result.player_id ) ||
            !is_valid_identifier( result.character_id, maximum_capability_id_bytes ) ||
            result.session_generation == 0 ) {
            error = "accepted resume result is incomplete";
            return false;
        }
    } else if( result.rejection == multiplayer_protocol_rejection::none ||
               !result.player_id.empty() || !result.character_id.empty() ||
               result.session_generation != 0 || result.replay_from_sequence != 0 ||
               !result.full_snapshot_required ) {
        error = "rejected resume result contains accepted-session fields";
        return false;
    }
    return true;
}

bool validate_resync_request( const multiplayer_resync_request &request, std::string &error )
{
    if( request.reason.size() > maximum_resync_reason_bytes ||
        ( !request.reason.empty() &&
          ( !is_valid_utf8( request.reason ) ||
    std::any_of( request.reason.begin(), request.reason.end(), []( const unsigned char ch ) {
    return ch < 0x20 || ch == 0x7f;
} ) ) ) ) {
        error = "resync request reason is invalid or oversized";
        return false;
    }
    return true;
}

bool validate_player_command( const multiplayer_player_command &command, std::string &error )
{
    if( command.client_sequence == 0 ) {
        error = "player command sequence must be nonzero";
        return false;
    }
    if( command.kind != multiplayer_command_kind::wait &&
        command.kind != multiplayer_command_kind::move ) {
        error = "player command kind is unsupported";
        return false;
    }
    if( command.kind == multiplayer_command_kind::wait && command.direction ) {
        error = "wait command must not contain a direction";
        return false;
    }
    if( command.kind == multiplayer_command_kind::move &&
        ( !command.direction || command.direction->dz != 0 ||
          command.direction->dx < -1 || command.direction->dx > 1 ||
          command.direction->dy < -1 || command.direction->dy > 1 ||
          ( command.direction->dx == 0 && command.direction->dy == 0 ) ) ) {
        error = "move command direction is invalid";
        return false;
    }
    return true;
}

bool validate_command_result( const multiplayer_command_result &result, std::string &error )
{
    if( result.client_sequence == 0 ||
        result.status < multiplayer_command_status::accepted ||
        result.status > multiplayer_command_status::pending_choice ||
        result.rejection < multiplayer_protocol_rejection::none ||
        result.rejection > multiplayer_protocol_rejection::savegame_version_mismatch ||
        result.message.size() > maximum_command_message_bytes ||
        ( !result.message.empty() && !is_valid_utf8( result.message ) ) ) {
        error = "command result contains an invalid or oversized field";
        return false;
    }
    const bool successful = result.status == multiplayer_command_status::accepted ||
                            result.status == multiplayer_command_status::duplicate;
    if( successful != ( result.rejection == multiplayer_protocol_rejection::none ) ) {
        error = "command result status and rejection are inconsistent";
        return false;
    }
    return true;
}

bool validate_scene_identifier( const std::string &value )
{
    return is_valid_identifier( value, maximum_scene_identifier_bytes );
}

bool validate_scene_snapshot( const multiplayer_scene_snapshot &snapshot, std::string &error )
{
    if( snapshot.server_revision == 0 || !is_uuid_v4( snapshot.player.player_id ) ||
        !is_valid_identifier( snapshot.player.character_id, maximum_capability_id_bytes ) ||
        snapshot.player.revision != snapshot.server_revision ||
        ( !snapshot.player.activity_id.empty() &&
          !validate_scene_identifier( snapshot.player.activity_id ) ) ) {
        error = "scene snapshot player state is invalid";
        return false;
    }
    if( snapshot.tiles.size() > maximum_scene_tiles ||
        snapshot.entities.size() > maximum_scene_entities ) {
        error = "scene snapshot exceeds entity or tile limits";
        return false;
    }
    for( const multiplayer_visible_tile &tile : snapshot.tiles ) {
        if( !validate_scene_identifier( tile.terrain_id ) ||
            ( !tile.furniture_id.empty() && !validate_scene_identifier( tile.furniture_id ) ) ||
            ( !tile.visible_trap_id.empty() && !validate_scene_identifier( tile.visible_trap_id ) ) ) {
            error = "scene snapshot tile identifier is invalid";
            return false;
        }
    }
    for( const multiplayer_visible_entity &entity : snapshot.entities ) {
        if( entity.kind <= multiplayer_visible_entity_kind::unknown ||
            entity.kind > multiplayer_visible_entity_kind::item ||
            !validate_scene_identifier( entity.stable_id ) ||
            !validate_scene_identifier( entity.appearance_id ) ||
            ( !entity.display_name.empty() &&
              ( entity.display_name.size() > maximum_scene_display_name_bytes ||
                !is_valid_utf8( entity.display_name ) ) ) ||
            entity.attitude < multiplayer_visible_attitude::unknown ||
            entity.attitude > multiplayer_visible_attitude::hostile ||
            entity.health_percent > 100 ) {
            error = "scene snapshot entity is invalid";
            return false;
        }
    }
    return true;
}

multiplayer_protocol_position parse_position( const protocol::AbsolutePosition &position )
{
    return { position.x(), position.y(), position.z() };
}

bool finish_payload( flatbuffers::FlatBufferBuilder &builder,
                     multiplayer_transport_payload &payload, std::string &error )
{
    if( builder.GetSize() > multiplayer_protocol_maximum_payload_size ) {
        error = "protocol payload exceeds the maximum frame size";
        return false;
    }
    payload.assign( builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() );
    error.clear();
    return true;
}

std::vector<flatbuffers::Offset<protocol::Capability>> build_capabilities(
            flatbuffers::FlatBufferBuilder &builder,
            const std::vector<multiplayer_protocol_capability> &capabilities )
{
    std::vector<flatbuffers::Offset<protocol::Capability>> result;
    result.reserve( capabilities.size() );
    for( const multiplayer_protocol_capability &capability : capabilities ) {
        result.emplace_back( protocol::CreateCapabilityDirect( builder, capability.id.c_str(),
                             capability.version, capability.required ) );
    }
    return result;
}

bool parse_capabilities(
    const flatbuffers::Vector<flatbuffers::Offset<protocol::Capability>> *input,
    std::vector<multiplayer_protocol_capability> &output, std::string &error )
{
    output.clear();
    if( input != nullptr ) {
        output.reserve( input->size() );
        for( const protocol::Capability *capability : *input ) {
            output.push_back( { capability->id()->str(), capability->version(),
                                capability->required() } );
        }
    }
    return validate_capabilities( output, error );
}

bool verify_payload( const multiplayer_protocol_message_type expected_type,
                     const multiplayer_transport_payload &payload, std::string &error )
{
    if( payload.empty() || payload.size() > multiplayer_protocol_maximum_payload_size ) {
        error = "protocol payload is empty or exceeds the frame limit";
        return false;
    }
    flatbuffers::Verifier verifier( payload.data(), payload.size(), 64, 10000 );
    if( !protocol::VerifyProtocolMessageBuffer( verifier ) ) {
        error = "protocol payload failed FlatBuffers verification";
        return false;
    }
    const protocol::ProtocolMessage *message = protocol::GetProtocolMessage( payload.data() );
    if( static_cast<std::uint16_t>( message->payload_type() ) !=
        static_cast<std::uint16_t>( expected_type ) ) {
        error = "protocol envelope type does not match the FlatBuffers payload type";
        return false;
    }
    error.clear();
    return true;
}

template<typename Array>
void copy_fixed_vector( const flatbuffers::Vector<std::uint8_t> *input, Array &output )
{
    std::copy( input->begin(), input->end(), output.begin() );
}

} // namespace

bool multiplayer_encode_protocol_envelope( const multiplayer_protocol_envelope &envelope,
        multiplayer_transport_payload &encoded, std::string &error )
{
    if( !is_known_message_type( envelope.message_type ) ) {
        error = "protocol message type is unknown";
        return false;
    }
    if( envelope.flags != supported_protocol_flags ) {
        error = "protocol envelope uses unsupported flags";
        return false;
    }
    if( !verify_payload( envelope.message_type, envelope.payload, error ) ) {
        return false;
    }

    encoded.clear();
    encoded.reserve( multiplayer_protocol_envelope_size + envelope.payload.size() );
    encoded.insert( encoded.end(), protocol_magic.begin(), protocol_magic.end() );
    append_u16( encoded, envelope.protocol_major );
    append_u16( encoded, envelope.protocol_minor );
    append_u16( encoded, multiplayer_protocol_envelope_size );
    append_u16( encoded, static_cast<std::uint16_t>( envelope.message_type ) );
    append_u32( encoded, envelope.flags );
    encoded.insert( encoded.end(), envelope.session.begin(), envelope.session.end() );
    append_u64( encoded, envelope.sequence );
    append_u32( encoded, static_cast<std::uint32_t>( envelope.payload.size() ) );
    append_u32( encoded, static_cast<std::uint32_t>( envelope.payload.size() ) );
    encoded.insert( encoded.end(), envelope.payload.begin(), envelope.payload.end() );
    error.clear();
    return true;
}

bool multiplayer_decode_protocol_envelope( const multiplayer_transport_payload &encoded,
        multiplayer_protocol_envelope &envelope, std::string &error )
{
    if( encoded.size() < multiplayer_protocol_envelope_size ||
        !std::equal( protocol_magic.begin(), protocol_magic.end(), encoded.begin() ) ) {
        error = "protocol envelope magic or size is invalid";
        return false;
    }
    if( read_u16( encoded.data() + 8 ) != multiplayer_protocol_envelope_size ) {
        error = "protocol envelope header size is unsupported";
        return false;
    }
    const multiplayer_protocol_message_type type =
        static_cast<multiplayer_protocol_message_type>( read_u16( encoded.data() + 10 ) );
    if( !is_known_message_type( type ) ) {
        error = "protocol message type is unknown";
        return false;
    }
    const std::uint32_t flags = read_u32( encoded.data() + 12 );
    if( flags != supported_protocol_flags ) {
        error = "protocol envelope uses unsupported flags";
        return false;
    }
    const std::uint32_t payload_size = read_u32( encoded.data() + 40 );
    const std::uint32_t uncompressed_size = read_u32( encoded.data() + 44 );
    if( payload_size != uncompressed_size || payload_size > multiplayer_protocol_maximum_payload_size ||
        payload_size != encoded.size() - multiplayer_protocol_envelope_size ) {
        error = "protocol envelope payload lengths are invalid";
        return false;
    }

    multiplayer_protocol_envelope decoded;
    decoded.protocol_major = read_u16( encoded.data() + 4 );
    decoded.protocol_minor = read_u16( encoded.data() + 6 );
    decoded.message_type = type;
    decoded.flags = flags;
    std::copy_n( encoded.begin() + 16, decoded.session.size(), decoded.session.begin() );
    decoded.sequence = read_u64( encoded.data() + 32 );
    decoded.payload.assign( encoded.begin() + multiplayer_protocol_envelope_size, encoded.end() );
    if( !verify_payload( decoded.message_type, decoded.payload, error ) ) {
        return false;
    }
    envelope = std::move( decoded );
    error.clear();
    return true;
}

bool multiplayer_build_client_hello_payload( const multiplayer_client_hello &hello,
        multiplayer_transport_payload &payload, std::string &error )
{
    if( !validate_client_hello( hello, error ) ) {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    const std::vector<flatbuffers::Offset<protocol::Capability>> capability_values =
                build_capabilities( builder, hello.capabilities );
    const std::vector<std::uint8_t> nonce( hello.client_nonce.begin(), hello.client_nonce.end() );
    const auto root = protocol::CreateClientHelloDirect(
                          builder, hello.protocol_major, hello.minimum_minor, hello.maximum_minor,
                          static_cast<protocol::ClientKind>( hello.client_kind ), hello.build_id.c_str(),
                          hello.content_manifest.c_str(), &capability_values, &nonce,
                          hello.server_state_schema, hello.savegame_version );
    const auto message = protocol::CreateProtocolMessage(
                             builder, protocol::MessagePayload::ClientHello, root.Union() );
    protocol::FinishProtocolMessageBuffer( builder, message );
    payload.assign( builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() );
    error.clear();
    return true;
}

bool multiplayer_parse_client_hello_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_client_hello &hello, std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::client_hello ||
        !verify_payload( envelope.message_type, envelope.payload, error ) ) {
        if( error.empty() ) {
            error = "protocol message is not a client hello";
        }
        return false;
    }
    const protocol::ClientHello *input =
        protocol::GetProtocolMessage( envelope.payload.data() )->payload_as_ClientHello();
    multiplayer_client_hello parsed;
    parsed.protocol_major = input->protocol_major();
    parsed.minimum_minor = input->minimum_minor();
    parsed.maximum_minor = input->maximum_minor();
    parsed.client_kind = static_cast<multiplayer_protocol_client_kind>( input->client_kind() );
    parsed.build_id = input->build_id()->str();
    parsed.content_manifest = input->content_manifest()->str();
    parsed.server_state_schema = input->server_state_schema();
    parsed.savegame_version = input->savegame_version();
    if( input->client_nonce() == nullptr ||
        input->client_nonce()->size() != parsed.client_nonce.size() ||
        !parse_capabilities( input->capabilities(), parsed.capabilities, error ) ) {
        if( error.empty() ) {
            error = "client nonce must contain exactly 32 bytes";
        }
        return false;
    }
    copy_fixed_vector( input->client_nonce(), parsed.client_nonce );
    if( envelope.protocol_major != parsed.protocol_major ||
        envelope.protocol_minor != parsed.maximum_minor ) {
        error = "client hello version does not match its protocol envelope";
        return false;
    }
    if( !validate_client_hello( parsed, error ) ) {
        return false;
    }
    hello = std::move( parsed );
    error.clear();
    return true;
}

bool multiplayer_build_server_hello_payload( const multiplayer_server_hello &hello,
        multiplayer_transport_payload &payload, std::string &error )
{
    if( !validate_server_hello( hello, error ) ) {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    const std::vector<flatbuffers::Offset<protocol::Capability>> capabilities =
                build_capabilities( builder, hello.capabilities );
    const std::vector<std::uint8_t> nonce( hello.server_nonce.begin(), hello.server_nonce.end() );
    const auto root = protocol::CreateServerHelloDirect(
                          builder, hello.accepted, hello.protocol_major, hello.protocol_minor,
                          hello.build_id.c_str(), hello.server_state_schema, hello.world_id.c_str(),
                          hello.content_manifest.c_str(), &capabilities, &nonce,
                          static_cast<protocol::RejectionCode>( hello.rejection ), hello.message.c_str(),
                          hello.savegame_version );
    const auto message = protocol::CreateProtocolMessage(
                             builder, protocol::MessagePayload::ServerHello, root.Union() );
    protocol::FinishProtocolMessageBuffer( builder, message );
    payload.assign( builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() );
    error.clear();
    return true;
}

bool multiplayer_parse_server_hello_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_server_hello &hello, std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::server_hello ||
        !verify_payload( envelope.message_type, envelope.payload, error ) ) {
        if( error.empty() ) {
            error = "protocol message is not a server hello";
        }
        return false;
    }
    const protocol::ServerHello *input =
        protocol::GetProtocolMessage( envelope.payload.data() )->payload_as_ServerHello();
    multiplayer_server_hello parsed;
    parsed.accepted = input->accepted();
    parsed.protocol_major = input->protocol_major();
    parsed.protocol_minor = input->protocol_minor();
    parsed.build_id = input->build_id() == nullptr ? std::string() : input->build_id()->str();
    parsed.server_state_schema = input->server_state_schema();
    parsed.world_id = input->world_id() == nullptr ? std::string() : input->world_id()->str();
    parsed.content_manifest = input->content_manifest() == nullptr ?
                              std::string() : input->content_manifest()->str();
    parsed.rejection = static_cast<multiplayer_protocol_rejection>( input->rejection() );
    parsed.message = input->message() == nullptr ? std::string() : input->message()->str();
    parsed.savegame_version = input->savegame_version();
    if( input->server_nonce() == nullptr ||
        input->server_nonce()->size() != parsed.server_nonce.size() ||
        !parse_capabilities( input->capabilities(), parsed.capabilities, error ) ) {
        if( error.empty() ) {
            error = "server nonce must contain exactly 32 bytes";
        }
        return false;
    }
    copy_fixed_vector( input->server_nonce(), parsed.server_nonce );
    if( envelope.protocol_major != parsed.protocol_major ||
        envelope.protocol_minor != parsed.protocol_minor ) {
        error = "server hello version does not match its protocol envelope";
        return false;
    }
    if( !validate_server_hello( parsed, error ) ) {
        return false;
    }
    hello = std::move( parsed );
    error.clear();
    return true;
}

bool multiplayer_build_authenticate_payload( const multiplayer_authenticate_request &request,
        multiplayer_transport_payload &payload, std::string &error )
{
    if( !validate_authenticate_request( request, error ) ) {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    const auto root = protocol::CreateAuthenticateDirect(
                          builder, request.player_id.empty() ? nullptr : request.player_id.c_str(),
                          request.character_id.empty() ? nullptr : request.character_id.c_str(),
                          request.display_name.c_str(),
                          request.bearer_token.empty() ? nullptr : request.bearer_token.c_str(),
                          request.challenge_response.empty() ? nullptr : &request.challenge_response );
    const auto message = protocol::CreateProtocolMessage(
                             builder, protocol::MessagePayload::Authenticate, root.Union() );
    protocol::FinishProtocolMessageBuffer( builder, message );
    payload.assign( builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() );
    error.clear();
    return true;
}

bool multiplayer_parse_authenticate_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_authenticate_request &request, std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::authenticate ) {
        error = "protocol message is not an authentication request";
        return false;
    }
    if( !verify_payload( envelope.message_type, envelope.payload, error ) ) {
        return false;
    }
    const protocol::Authenticate *input =
        protocol::GetProtocolMessage( envelope.payload.data() )->payload_as_Authenticate();
    multiplayer_authenticate_request parsed;
    parsed.player_id = input->player_id() == nullptr ? std::string() : input->player_id()->str();
    parsed.character_id = input->character_id() == nullptr ?
                          std::string() : input->character_id()->str();
    parsed.display_name = input->display_name()->str();
    parsed.bearer_token = input->bearer_token() == nullptr ?
                          std::string() : input->bearer_token()->str();
    if( input->challenge_response() != nullptr ) {
        parsed.challenge_response.assign( input->challenge_response()->begin(),
                                          input->challenge_response()->end() );
    }
    if( !validate_authenticate_request( parsed, error ) ) {
        return false;
    }
    request = std::move( parsed );
    error.clear();
    return true;
}

bool multiplayer_build_authentication_result_payload(
    const multiplayer_authentication_result &result,
    multiplayer_transport_payload &payload, std::string &error )
{
    if( !validate_authentication_result( result, error ) ) {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    const auto root = protocol::CreateAuthenticationResultDirect(
                          builder, result.accepted,
                          result.player_id.empty() ? nullptr : result.player_id.c_str(),
                          result.character_id.empty() ? nullptr : result.character_id.c_str(),
                          result.resume_token.empty() ? nullptr : result.resume_token.c_str(),
                          result.session_generation,
                          static_cast<protocol::RejectionCode>( result.rejection ),
                          result.message.empty() ? nullptr : result.message.c_str() );
    const auto message = protocol::CreateProtocolMessage(
                             builder, protocol::MessagePayload::AuthenticationResult, root.Union() );
    protocol::FinishProtocolMessageBuffer( builder, message );
    payload.assign( builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() );
    error.clear();
    return true;
}

bool multiplayer_parse_authentication_result_payload(
    const multiplayer_protocol_envelope &envelope,
    multiplayer_authentication_result &result, std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::authentication_result ) {
        error = "protocol message is not an authentication result";
        return false;
    }
    if( !verify_payload( envelope.message_type, envelope.payload, error ) ) {
        return false;
    }
    const protocol::AuthenticationResult *input =
        protocol::GetProtocolMessage( envelope.payload.data() )->payload_as_AuthenticationResult();
    multiplayer_authentication_result parsed;
    parsed.accepted = input->accepted();
    parsed.player_id = input->player_id() == nullptr ? std::string() : input->player_id()->str();
    parsed.character_id = input->character_id() == nullptr ?
                          std::string() : input->character_id()->str();
    parsed.resume_token = input->resume_token() == nullptr ?
                          std::string() : input->resume_token()->str();
    parsed.session_generation = input->session_generation();
    parsed.rejection = static_cast<multiplayer_protocol_rejection>( input->rejection() );
    parsed.message = input->message() == nullptr ? std::string() : input->message()->str();
    if( !validate_authentication_result( parsed, error ) ) {
        return false;
    }
    result = std::move( parsed );
    error.clear();
    return true;
}

bool multiplayer_build_resume_request_payload( const multiplayer_resume_request &request,
        multiplayer_transport_payload &payload, std::string &error )
{
    if( !is_bearer_token( request.resume_token ) ) {
        error = "resume token has an invalid format";
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    const auto root = protocol::CreateResumeRequestDirect(
                          builder, request.resume_token.c_str(), request.last_server_revision,
                          request.last_client_sequence );
    const auto message = protocol::CreateProtocolMessage(
                             builder, protocol::MessagePayload::ResumeRequest, root.Union() );
    protocol::FinishProtocolMessageBuffer( builder, message );
    payload.assign( builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() );
    error.clear();
    return true;
}

bool multiplayer_parse_resume_request_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_resume_request &request, std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::resume_request ) {
        error = "protocol message is not a resume request";
        return false;
    }
    if( !verify_payload( envelope.message_type, envelope.payload, error ) ) {
        return false;
    }
    const protocol::ResumeRequest *input =
        protocol::GetProtocolMessage( envelope.payload.data() )->payload_as_ResumeRequest();
    multiplayer_resume_request parsed;
    parsed.resume_token = input->resume_token()->str();
    parsed.last_server_revision = input->last_server_revision();
    parsed.last_client_sequence = input->last_client_sequence();
    if( !is_bearer_token( parsed.resume_token ) ) {
        error = "resume token has an invalid format";
        return false;
    }
    request = std::move( parsed );
    error.clear();
    return true;
}

bool multiplayer_build_resume_result_payload( const multiplayer_resume_result &result,
        multiplayer_transport_payload &payload, std::string &error )
{
    if( !validate_resume_result( result, error ) ) {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    const auto root = protocol::CreateResumeResultDirect(
                          builder, result.accepted,
                          result.player_id.empty() ? nullptr : result.player_id.c_str(),
                          result.character_id.empty() ? nullptr : result.character_id.c_str(),
                          result.session_generation, result.replay_from_sequence,
                          result.full_snapshot_required,
                          static_cast<protocol::RejectionCode>( result.rejection ),
                          result.message.empty() ? nullptr : result.message.c_str() );
    const auto message = protocol::CreateProtocolMessage(
                             builder, protocol::MessagePayload::ResumeResult, root.Union() );
    protocol::FinishProtocolMessageBuffer( builder, message );
    payload.assign( builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() );
    error.clear();
    return true;
}

bool multiplayer_parse_resume_result_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_resume_result &result, std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::resume_result ) {
        error = "protocol message is not a resume result";
        return false;
    }
    if( !verify_payload( envelope.message_type, envelope.payload, error ) ) {
        return false;
    }
    const protocol::ResumeResult *input =
        protocol::GetProtocolMessage( envelope.payload.data() )->payload_as_ResumeResult();
    multiplayer_resume_result parsed;
    parsed.accepted = input->accepted();
    parsed.player_id = input->player_id() == nullptr ? std::string() : input->player_id()->str();
    parsed.character_id = input->character_id() == nullptr ?
                          std::string() : input->character_id()->str();
    parsed.session_generation = input->session_generation();
    parsed.replay_from_sequence = input->replay_from_sequence();
    parsed.full_snapshot_required = input->full_snapshot_required();
    parsed.rejection = static_cast<multiplayer_protocol_rejection>( input->rejection() );
    parsed.message = input->message() == nullptr ? std::string() : input->message()->str();
    if( !validate_resume_result( parsed, error ) ) {
        return false;
    }
    result = std::move( parsed );
    error.clear();
    return true;
}

bool multiplayer_build_ping_payload( const multiplayer_protocol_heartbeat &ping,
                                     multiplayer_transport_payload &payload, std::string &error )
{
    flatbuffers::FlatBufferBuilder builder;
    const auto root = protocol::CreatePing( builder, ping.nonce, ping.monotonic_milliseconds );
    const auto message = protocol::CreateProtocolMessage(
                             builder, protocol::MessagePayload::Ping, root.Union() );
    protocol::FinishProtocolMessageBuffer( builder, message );
    payload.assign( builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() );
    error.clear();
    return true;
}

bool multiplayer_parse_ping_payload( const multiplayer_protocol_envelope &envelope,
                                     multiplayer_protocol_heartbeat &ping, std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::ping ) {
        error = "protocol message is not a ping";
        return false;
    }
    if( !verify_payload( envelope.message_type, envelope.payload, error ) ) {
        return false;
    }
    const protocol::Ping *input =
        protocol::GetProtocolMessage( envelope.payload.data() )->payload_as_Ping();
    ping.nonce = input->nonce();
    ping.monotonic_milliseconds = input->monotonic_milliseconds();
    error.clear();
    return true;
}

bool multiplayer_build_pong_payload( const multiplayer_protocol_heartbeat &pong,
                                     multiplayer_transport_payload &payload, std::string &error )
{
    flatbuffers::FlatBufferBuilder builder;
    const auto root = protocol::CreatePong( builder, pong.nonce, pong.monotonic_milliseconds );
    const auto message = protocol::CreateProtocolMessage(
                             builder, protocol::MessagePayload::Pong, root.Union() );
    protocol::FinishProtocolMessageBuffer( builder, message );
    payload.assign( builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize() );
    error.clear();
    return true;
}

bool multiplayer_parse_pong_payload( const multiplayer_protocol_envelope &envelope,
                                     multiplayer_protocol_heartbeat &pong, std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::pong ) {
        error = "protocol message is not a pong";
        return false;
    }
    if( !verify_payload( envelope.message_type, envelope.payload, error ) ) {
        return false;
    }
    const protocol::Pong *input =
        protocol::GetProtocolMessage( envelope.payload.data() )->payload_as_Pong();
    pong.nonce = input->nonce();
    pong.monotonic_milliseconds = input->monotonic_milliseconds();
    error.clear();
    return true;
}

bool multiplayer_build_resync_request_payload( const multiplayer_resync_request &request,
        multiplayer_transport_payload &payload, std::string &error )
{
    if( !validate_resync_request( request, error ) ) {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    const auto root = protocol::CreateResyncRequestDirect(
                          builder, request.client_revision,
                          request.reason.empty() ? nullptr : request.reason.c_str() );
    const auto message = protocol::CreateProtocolMessage(
                             builder, protocol::MessagePayload::ResyncRequest, root.Union() );
    protocol::FinishProtocolMessageBuffer( builder, message );
    return finish_payload( builder, payload, error );
}

bool multiplayer_parse_resync_request_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_resync_request &request, std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::resync_request ) {
        error = "protocol message is not a resync request";
        return false;
    }
    if( !verify_payload( envelope.message_type, envelope.payload, error ) ) {
        return false;
    }
    const protocol::ResyncRequest *input =
        protocol::GetProtocolMessage( envelope.payload.data() )->payload_as_ResyncRequest();
    multiplayer_resync_request parsed;
    parsed.client_revision = input->client_revision();
    parsed.reason = input->reason() == nullptr ? std::string() : input->reason()->str();
    if( !validate_resync_request( parsed, error ) ) {
        return false;
    }
    request = std::move( parsed );
    error.clear();
    return true;
}

bool multiplayer_build_player_command_payload( const multiplayer_player_command &command,
        multiplayer_transport_payload &payload, std::string &error )
{
    if( !validate_player_command( command, error ) ) {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    std::optional<protocol::Direction> direction;
    if( command.direction ) {
        direction.emplace( command.direction->dx, command.direction->dy, command.direction->dz );
    }
    const auto root = protocol::CreatePlayerCommand(
                          builder, command.client_sequence, command.base_revision,
                          static_cast<protocol::CommandKind>( command.kind ),
                          direction ? &*direction : nullptr );
    const auto message = protocol::CreateProtocolMessage(
                             builder, protocol::MessagePayload::PlayerCommand, root.Union() );
    protocol::FinishProtocolMessageBuffer( builder, message );
    return finish_payload( builder, payload, error );
}

bool multiplayer_parse_player_command_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_player_command &command, std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::player_command ) {
        error = "protocol message is not a player command";
        return false;
    }
    if( !verify_payload( envelope.message_type, envelope.payload, error ) ) {
        return false;
    }
    const protocol::PlayerCommand *input =
        protocol::GetProtocolMessage( envelope.payload.data() )->payload_as_PlayerCommand();
    multiplayer_player_command parsed;
    parsed.client_sequence = input->client_sequence();
    parsed.base_revision = input->base_revision();
    parsed.kind = static_cast<multiplayer_command_kind>( input->kind() );
    if( const protocol::Direction *direction = input->direction() ) {
        parsed.direction = multiplayer_protocol_direction{ direction->dx(), direction->dy(),
                                                           direction->dz() };
    }
    if( envelope.sequence != parsed.client_sequence ) {
        error = "player command sequence does not match its protocol envelope";
        return false;
    }
    if( !validate_player_command( parsed, error ) ) {
        return false;
    }
    command = std::move( parsed );
    error.clear();
    return true;
}

bool multiplayer_build_command_result_payload( const multiplayer_command_result &result,
        multiplayer_transport_payload &payload, std::string &error )
{
    if( !validate_command_result( result, error ) ) {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    const auto root = protocol::CreateCommandResultDirect(
                          builder, result.client_sequence,
                          static_cast<protocol::CommandStatus>( result.status ),
                          static_cast<protocol::RejectionCode>( result.rejection ),
                          result.server_revision, result.moves_spent,
                          result.message.empty() ? nullptr : result.message.c_str() );
    const auto message = protocol::CreateProtocolMessage(
                             builder, protocol::MessagePayload::CommandResult, root.Union() );
    protocol::FinishProtocolMessageBuffer( builder, message );
    return finish_payload( builder, payload, error );
}

bool multiplayer_parse_command_result_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_command_result &result, std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::command_result ) {
        error = "protocol message is not a command result";
        return false;
    }
    if( !verify_payload( envelope.message_type, envelope.payload, error ) ) {
        return false;
    }
    const protocol::CommandResult *input =
        protocol::GetProtocolMessage( envelope.payload.data() )->payload_as_CommandResult();
    multiplayer_command_result parsed;
    parsed.client_sequence = input->client_sequence();
    parsed.status = static_cast<multiplayer_command_status>( input->status() );
    parsed.rejection = static_cast<multiplayer_protocol_rejection>( input->rejection() );
    parsed.server_revision = input->server_revision();
    parsed.moves_spent = input->moves_spent();
    parsed.message = input->message() == nullptr ? std::string() : input->message()->str();
    if( envelope.sequence != parsed.client_sequence ) {
        error = "command result sequence does not match its protocol envelope";
        return false;
    }
    if( !validate_command_result( parsed, error ) ) {
        return false;
    }
    result = std::move( parsed );
    error.clear();
    return true;
}

bool multiplayer_build_scene_snapshot_payload( const multiplayer_scene_snapshot &snapshot,
        multiplayer_transport_payload &payload, std::string &error )
{
    if( !validate_scene_snapshot( snapshot, error ) ) {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    const protocol::AbsolutePosition player_position( snapshot.player.position.x,
            snapshot.player.position.y, snapshot.player.position.z );
    const auto player = protocol::CreatePlayerStateDirect(
                            builder, snapshot.player.player_id.c_str(),
                            snapshot.player.character_id.c_str(), snapshot.player.revision,
                            &player_position, snapshot.player.moves, snapshot.player.pain,
                            snapshot.player.stamina,
                            snapshot.player.activity_id.empty() ? nullptr :
                            snapshot.player.activity_id.c_str() );

    std::vector<flatbuffers::Offset<protocol::VisibleTile>> tiles;
    tiles.reserve( snapshot.tiles.size() );
    for( const multiplayer_visible_tile &tile : snapshot.tiles ) {
        const protocol::AbsolutePosition position( tile.position.x, tile.position.y, tile.position.z );
        tiles.emplace_back( protocol::CreateVisibleTileDirect(
                                builder, &position, tile.terrain_id.c_str(),
                                tile.furniture_id.empty() ? nullptr : tile.furniture_id.c_str(),
                                tile.visible_trap_id.empty() ? nullptr : tile.visible_trap_id.c_str(),
                                nullptr, tile.light_level ) );
    }

    std::vector<flatbuffers::Offset<protocol::VisibleEntity>> entities;
    entities.reserve( snapshot.entities.size() );
    for( const multiplayer_visible_entity &entity : snapshot.entities ) {
        const protocol::AbsolutePosition position( entity.position.x, entity.position.y,
                entity.position.z );
        entities.emplace_back( protocol::CreateVisibleEntityDirect(
                                   builder, static_cast<protocol::EntityKind>( entity.kind ),
                                   entity.stable_id.c_str(), entity.revision, &position,
                                   entity.appearance_id.c_str(),
                                   entity.display_name.empty() ? nullptr : entity.display_name.c_str(),
                                   static_cast<protocol::Attitude>( entity.attitude ),
                                   entity.health_percent ) );
    }
    const auto root = protocol::CreateSceneSnapshotDirect(
                          builder, snapshot.server_revision, snapshot.turn, player, &tiles, &entities );
    const auto message = protocol::CreateProtocolMessage(
                             builder, protocol::MessagePayload::SceneSnapshot, root.Union() );
    protocol::FinishProtocolMessageBuffer( builder, message );
    return finish_payload( builder, payload, error );
}

bool multiplayer_parse_scene_snapshot_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_scene_snapshot &snapshot, std::string &error )
{
    if( envelope.message_type != multiplayer_protocol_message_type::scene_snapshot ) {
        error = "protocol message is not a scene snapshot";
        return false;
    }
    if( !verify_payload( envelope.message_type, envelope.payload, error ) ) {
        return false;
    }
    const protocol::SceneSnapshot *input =
        protocol::GetProtocolMessage( envelope.payload.data() )->payload_as_SceneSnapshot();
    multiplayer_scene_snapshot parsed;
    parsed.server_revision = input->server_revision();
    parsed.turn = input->turn();
    const protocol::PlayerState *player = input->player();
    parsed.player.player_id = player->player_id()->str();
    parsed.player.character_id = player->character_id()->str();
    parsed.player.revision = player->revision();
    parsed.player.position = parse_position( *player->position() );
    parsed.player.moves = player->moves();
    parsed.player.pain = player->pain();
    parsed.player.stamina = player->stamina();
    parsed.player.activity_id = player->activity_id() == nullptr ? std::string() :
                                player->activity_id()->str();
    if( input->tiles() != nullptr ) {
        parsed.tiles.reserve( input->tiles()->size() );
        for( const protocol::VisibleTile *tile : *input->tiles() ) {
            parsed.tiles.push_back( { parse_position( *tile->position() ), tile->terrain_id()->str(),
                                      tile->furniture_id() == nullptr ? std::string() :
                                      tile->furniture_id()->str(),
                                      tile->visible_trap_id() == nullptr ? std::string() :
                                      tile->visible_trap_id()->str(), tile->light_level() } );
        }
    }
    if( input->entities() != nullptr ) {
        parsed.entities.reserve( input->entities()->size() );
        for( const protocol::VisibleEntity *entity : *input->entities() ) {
            parsed.entities.push_back( {
                static_cast<multiplayer_visible_entity_kind>( entity->kind() ),
                entity->stable_id()->str(), entity->revision(),
                parse_position( *entity->position() ), entity->appearance_id()->str(),
                entity->display_name() == nullptr ? std::string() : entity->display_name()->str(),
                static_cast<multiplayer_visible_attitude>( entity->attitude() ),
                entity->health_percent()
            } );
        }
    }
    if( envelope.sequence != parsed.server_revision ) {
        error = "scene snapshot revision does not match its protocol envelope";
        return false;
    }
    if( !validate_scene_snapshot( parsed, error ) ) {
        return false;
    }
    snapshot = std::move( parsed );
    error.clear();
    return true;
}

multiplayer_server_hello multiplayer_negotiate_client_hello(
    const multiplayer_client_hello &client,
    const std::vector<multiplayer_protocol_capability> &server_capabilities,
    const std::string &server_build_id, const std::string &world_id,
    const std::string &content_manifest, const std::int32_t server_savegame_version )
{
    multiplayer_server_hello result;
    result.build_id = server_build_id;
    result.world_id = world_id;
    result.content_manifest = content_manifest;
    result.capabilities = server_capabilities;
    result.savegame_version = server_savegame_version;

    if( client.protocol_major != multiplayer_protocol_current_major ) {
        result.rejection = multiplayer_protocol_rejection::protocol_major_mismatch;
        result.message = "protocol major version is incompatible";
        return result;
    }
    if( multiplayer_protocol_current_minor < client.minimum_minor ||
        multiplayer_protocol_current_minor > client.maximum_minor ) {
        result.rejection = multiplayer_protocol_rejection::protocol_minor_mismatch;
        result.message = "protocol minor ranges do not overlap";
        return result;
    }
    if( client.build_id != server_build_id ) {
        result.rejection = multiplayer_protocol_rejection::build_mismatch;
        result.message = "client build id does not match the server";
        return result;
    }
    if( client.server_state_schema != multiplayer_server_state_schema_version ) {
        result.rejection = multiplayer_protocol_rejection::server_state_schema_mismatch;
        result.message = "client server-state schema does not match the server";
        return result;
    }
    if( client.savegame_version != server_savegame_version ) {
        result.rejection = multiplayer_protocol_rejection::savegame_version_mismatch;
        result.message = "client savegame version does not match the server";
        return result;
    }
    if( client.content_manifest != content_manifest ) {
        result.rejection = multiplayer_protocol_rejection::content_mismatch;
        result.message = "gameplay content manifest does not match the server";
        return result;
    }

    const auto find_capability = []( const std::vector<multiplayer_protocol_capability> &values,
    const std::string & id ) {
        return std::find_if( values.begin(), values.end(), [&id](
        const multiplayer_protocol_capability & value ) {
            return value.id == id;
        } );
    };
    for( const multiplayer_protocol_capability &capability : client.capabilities ) {
        const auto server = find_capability( server_capabilities, capability.id );
        if( capability.required && ( server == server_capabilities.end() ||
                                     server->version < capability.version ) ) {
            result.rejection = multiplayer_protocol_rejection::missing_capability;
            result.message = "server does not support required client capability " + capability.id;
            return result;
        }
    }
    for( const multiplayer_protocol_capability &capability : server_capabilities ) {
        const auto client_value = find_capability( client.capabilities, capability.id );
        if( capability.required && ( client_value == client.capabilities.end() ||
                                     client_value->version < capability.version ) ) {
            result.rejection = multiplayer_protocol_rejection::missing_capability;
            result.message = "client does not support required server capability " + capability.id;
            return result;
        }
    }

    result.accepted = true;
    result.rejection = multiplayer_protocol_rejection::none;
    result.protocol_minor = multiplayer_protocol_current_minor;
    return result;
}

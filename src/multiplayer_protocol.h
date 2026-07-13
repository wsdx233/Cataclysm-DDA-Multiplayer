#pragma once
#ifndef CATA_SRC_MULTIPLAYER_PROTOCOL_H
#define CATA_SRC_MULTIPLAYER_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "multiplayer_transport.h"

constexpr std::uint16_t multiplayer_protocol_current_major = 1;
constexpr std::uint16_t multiplayer_protocol_current_minor = 0;
constexpr std::uint32_t multiplayer_server_state_schema_version = 1;
constexpr std::size_t multiplayer_protocol_envelope_size = 48;
constexpr std::size_t multiplayer_protocol_maximum_payload_size =
    multiplayer_transport_maximum_frame_size - multiplayer_protocol_envelope_size;

using multiplayer_session_id = std::array<std::uint8_t, 16>;

enum class multiplayer_protocol_message_type : std::uint16_t {
    none = 0,
    client_hello = 1,
    server_hello = 2,
    authenticate = 3,
    authentication_result = 4,
    resume_request = 5,
    resume_result = 6,
    ping = 7,
    pong = 8,
    disconnect_notice = 9,
    player_command = 10,
    command_result = 11,
    scene_snapshot = 12,
    scene_delta = 13,
    resync_request = 14,
    chat_message = 15
};

enum class multiplayer_protocol_rejection : std::uint16_t {
    none = 0,
    malformed_message = 1,
    protocol_major_mismatch = 2,
    protocol_minor_mismatch = 3,
    missing_capability = 4,
    content_mismatch = 5,
    authentication_required = 6,
    authentication_failed = 7,
    session_expired = 8,
    server_full = 9,
    rate_limited = 10,
    invalid_state = 11,
    permission_denied = 12,
    invalid_command = 13,
    stale_revision = 14,
    resource_limit = 15,
    internal_error = 16,
    build_mismatch = 17,
    server_state_schema_mismatch = 18,
    savegame_version_mismatch = 19
};

enum class multiplayer_protocol_client_kind : std::uint8_t {
    unknown = 0,
    graphical_desktop = 1,
    graphical_android = 2,
    headless_test = 3
};

struct multiplayer_protocol_envelope {
    std::uint16_t protocol_major = multiplayer_protocol_current_major;
    std::uint16_t protocol_minor = multiplayer_protocol_current_minor;
    multiplayer_protocol_message_type message_type = multiplayer_protocol_message_type::none;
    std::uint32_t flags = 0;
    multiplayer_session_id session = {};
    std::uint64_t sequence = 0;
    multiplayer_transport_payload payload;
};

bool multiplayer_encode_protocol_envelope( const multiplayer_protocol_envelope &envelope,
        multiplayer_transport_payload &encoded, std::string &error );
bool multiplayer_decode_protocol_envelope( const multiplayer_transport_payload &encoded,
        multiplayer_protocol_envelope &envelope, std::string &error );

struct multiplayer_protocol_capability {
    std::string id;
    std::uint32_t version = 1;
    bool required = false;
};

struct multiplayer_client_hello {
    std::uint16_t protocol_major = multiplayer_protocol_current_major;
    std::uint16_t minimum_minor = multiplayer_protocol_current_minor;
    std::uint16_t maximum_minor = multiplayer_protocol_current_minor;
    multiplayer_protocol_client_kind client_kind = multiplayer_protocol_client_kind::unknown;
    std::string build_id;
    std::string content_manifest;
    std::vector<multiplayer_protocol_capability> capabilities;
    std::array<std::uint8_t, 32> client_nonce = {};
    std::uint32_t server_state_schema = multiplayer_server_state_schema_version;
    std::int32_t savegame_version = 0;
};

struct multiplayer_server_hello {
    bool accepted = false;
    std::uint16_t protocol_major = multiplayer_protocol_current_major;
    std::uint16_t protocol_minor = multiplayer_protocol_current_minor;
    std::string build_id;
    std::uint32_t server_state_schema = multiplayer_server_state_schema_version;
    std::string world_id;
    std::string content_manifest;
    std::vector<multiplayer_protocol_capability> capabilities;
    std::array<std::uint8_t, 32> server_nonce = {};
    multiplayer_protocol_rejection rejection = multiplayer_protocol_rejection::none;
    std::string message;
    std::int32_t savegame_version = 0;
};

struct multiplayer_authenticate_request {
    std::string player_id;
    std::string character_id;
    std::string display_name;
    std::string bearer_token;
    std::vector<std::uint8_t> challenge_response;
};

struct multiplayer_authentication_result {
    bool accepted = false;
    std::string player_id;
    std::string character_id;
    std::string resume_token;
    std::uint64_t session_generation = 0;
    multiplayer_protocol_rejection rejection = multiplayer_protocol_rejection::none;
    std::string message;
};

struct multiplayer_resume_request {
    std::string resume_token;
    std::uint64_t last_server_revision = 0;
    std::uint64_t last_client_sequence = 0;
};

struct multiplayer_resume_result {
    bool accepted = false;
    std::string player_id;
    std::string character_id;
    std::uint64_t session_generation = 0;
    std::uint64_t replay_from_sequence = 0;
    bool full_snapshot_required = true;
    multiplayer_protocol_rejection rejection = multiplayer_protocol_rejection::none;
    std::string message;
};

struct multiplayer_protocol_heartbeat {
    std::uint64_t nonce = 0;
    std::uint64_t monotonic_milliseconds = 0;
};

struct multiplayer_resync_request {
    std::uint64_t client_revision = 0;
    std::string reason;
};

enum class multiplayer_command_kind : std::uint16_t {
    none = 0,
    wait = 1,
    move = 2
};

enum class multiplayer_command_status : std::uint8_t {
    accepted = 0,
    rejected = 1,
    duplicate = 2,
    pending_choice = 3
};

struct multiplayer_protocol_position {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
};

struct multiplayer_protocol_direction {
    std::int8_t dx = 0;
    std::int8_t dy = 0;
    std::int8_t dz = 0;
};

struct multiplayer_player_command {
    std::uint64_t client_sequence = 0;
    std::uint64_t base_revision = 0;
    multiplayer_command_kind kind = multiplayer_command_kind::none;
    std::optional<multiplayer_protocol_direction> direction;
};

struct multiplayer_command_result {
    std::uint64_t client_sequence = 0;
    multiplayer_command_status status = multiplayer_command_status::accepted;
    multiplayer_protocol_rejection rejection = multiplayer_protocol_rejection::none;
    std::uint64_t server_revision = 0;
    std::int32_t moves_spent = 0;
    std::string message;
};

enum class multiplayer_visible_entity_kind : std::uint8_t {
    unknown = 0,
    player = 1,
    npc = 2,
    monster = 3,
    vehicle = 4,
    item = 5
};

enum class multiplayer_visible_attitude : std::uint8_t {
    unknown = 0,
    friendly = 1,
    neutral = 2,
    hostile = 3
};

struct multiplayer_visible_tile {
    multiplayer_protocol_position position;
    std::string terrain_id;
    std::string furniture_id;
    std::string visible_trap_id;
    std::uint8_t light_level = 0;
};

struct multiplayer_visible_entity {
    multiplayer_visible_entity_kind kind = multiplayer_visible_entity_kind::unknown;
    std::string stable_id;
    std::uint64_t revision = 0;
    multiplayer_protocol_position position;
    std::string appearance_id;
    std::string display_name;
    multiplayer_visible_attitude attitude = multiplayer_visible_attitude::unknown;
    std::uint8_t health_percent = 100;
};

struct multiplayer_protocol_player_state {
    std::string player_id;
    std::string character_id;
    std::uint64_t revision = 0;
    multiplayer_protocol_position position;
    std::int32_t moves = 0;
    std::int32_t pain = 0;
    std::int32_t stamina = 0;
    std::string activity_id;
};

struct multiplayer_scene_snapshot {
    std::uint64_t server_revision = 0;
    std::int64_t turn = 0;
    multiplayer_protocol_player_state player;
    std::vector<multiplayer_visible_tile> tiles;
    std::vector<multiplayer_visible_entity> entities;
};

bool multiplayer_build_client_hello_payload( const multiplayer_client_hello &hello,
        multiplayer_transport_payload &payload, std::string &error );
bool multiplayer_parse_client_hello_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_client_hello &hello, std::string &error );
bool multiplayer_build_server_hello_payload( const multiplayer_server_hello &hello,
        multiplayer_transport_payload &payload, std::string &error );
bool multiplayer_parse_server_hello_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_server_hello &hello, std::string &error );
bool multiplayer_build_authenticate_payload( const multiplayer_authenticate_request &request,
        multiplayer_transport_payload &payload, std::string &error );
bool multiplayer_parse_authenticate_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_authenticate_request &request, std::string &error );
bool multiplayer_build_authentication_result_payload(
    const multiplayer_authentication_result &result,
    multiplayer_transport_payload &payload, std::string &error );
bool multiplayer_parse_authentication_result_payload(
    const multiplayer_protocol_envelope &envelope,
    multiplayer_authentication_result &result, std::string &error );
bool multiplayer_build_resume_request_payload( const multiplayer_resume_request &request,
        multiplayer_transport_payload &payload, std::string &error );
bool multiplayer_parse_resume_request_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_resume_request &request, std::string &error );
bool multiplayer_build_resume_result_payload( const multiplayer_resume_result &result,
        multiplayer_transport_payload &payload, std::string &error );
bool multiplayer_parse_resume_result_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_resume_result &result, std::string &error );
bool multiplayer_build_ping_payload( const multiplayer_protocol_heartbeat &ping,
                                     multiplayer_transport_payload &payload, std::string &error );
bool multiplayer_parse_ping_payload( const multiplayer_protocol_envelope &envelope,
                                     multiplayer_protocol_heartbeat &ping, std::string &error );
bool multiplayer_build_pong_payload( const multiplayer_protocol_heartbeat &pong,
                                     multiplayer_transport_payload &payload, std::string &error );
bool multiplayer_parse_pong_payload( const multiplayer_protocol_envelope &envelope,
                                     multiplayer_protocol_heartbeat &pong, std::string &error );
bool multiplayer_build_resync_request_payload( const multiplayer_resync_request &request,
        multiplayer_transport_payload &payload, std::string &error );
bool multiplayer_parse_resync_request_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_resync_request &request, std::string &error );
bool multiplayer_build_player_command_payload( const multiplayer_player_command &command,
        multiplayer_transport_payload &payload, std::string &error );
bool multiplayer_parse_player_command_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_player_command &command, std::string &error );
bool multiplayer_build_command_result_payload( const multiplayer_command_result &result,
        multiplayer_transport_payload &payload, std::string &error );
bool multiplayer_parse_command_result_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_command_result &result, std::string &error );
bool multiplayer_build_scene_snapshot_payload( const multiplayer_scene_snapshot &snapshot,
        multiplayer_transport_payload &payload, std::string &error );
bool multiplayer_parse_scene_snapshot_payload( const multiplayer_protocol_envelope &envelope,
        multiplayer_scene_snapshot &snapshot, std::string &error );

multiplayer_server_hello multiplayer_negotiate_client_hello(
    const multiplayer_client_hello &client,
    const std::vector<multiplayer_protocol_capability> &server_capabilities,
    const std::string &server_build_id, const std::string &world_id,
    const std::string &content_manifest, std::int32_t server_savegame_version );

#endif // CATA_SRC_MULTIPLAYER_PROTOCOL_H

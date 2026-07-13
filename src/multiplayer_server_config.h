#pragma once
#ifndef CATA_SRC_MULTIPLAYER_SERVER_CONFIG_H
#define CATA_SRC_MULTIPLAYER_SERVER_CONFIG_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

enum class multiplayer_transport_security : std::uint8_t {
    disabled,
    external_tunnel
};

enum class multiplayer_character_policy : std::uint8_t {
    server_owned,
    portable_lease,
    copy_in
};

struct multiplayer_server_world_config {
    std::string name = "coop-world";
    std::string seed = "server-owned-seed";
    std::vector<std::string> mods = { "dda" };
    std::map<std::string, std::string, std::less<>> options;
    std::string spawn_policy = "shared_start";
};

struct multiplayer_server_network_config {
    std::string listen = "127.0.0.1:27999";
    multiplayer_transport_security security = multiplayer_transport_security::disabled;
    bool allow_insecure_lan = false;
    std::uint32_t handshake_timeout_ms = 10000;
};

struct multiplayer_server_listen_endpoint {
    std::string host;
    std::uint16_t port = 0;
};

struct multiplayer_server_authentication_config {
    std::string mode = "token";
    std::string token_file = "server-auth-token.txt";
    std::uint32_t resume_token_seconds = 3600;
    std::uint32_t maximum_attempts_per_minute = 10;
};

struct multiplayer_server_player_config {
    int maximum = 1;
    multiplayer_character_policy character_policy =
        multiplayer_character_policy::server_owned;
    int tether_tiles = 48;
    int disconnect_grace_seconds = 30;
};

struct multiplayer_server_time_config {
    std::string policy = "turn_barrier";
    int idle_timeout_seconds = 0;
    std::string fast_forward = "all_players_safe";
};

struct multiplayer_server_save_config {
    int interval_turns = 300;
    int keep_generations = 1;
};

struct multiplayer_server_config {
    static constexpr int current_schema_version = 1;

    int schema_version = current_schema_version;
    multiplayer_server_world_config world;
    multiplayer_server_network_config network;
    multiplayer_server_authentication_config authentication;
    multiplayer_server_player_config players;
    multiplayer_server_time_config time;
    multiplayer_server_save_config save;
};

struct multiplayer_server_config_result {
    std::optional<multiplayer_server_config> config;
    std::string error;

    explicit operator bool() const noexcept {
        return config.has_value();
    }
};

const char *multiplayer_transport_security_name( multiplayer_transport_security security ) noexcept;
const char *multiplayer_character_policy_name( multiplayer_character_policy policy ) noexcept;
std::optional<std::string> parse_multiplayer_server_listen_endpoint(
    const std::string &text, multiplayer_server_listen_endpoint &result );

std::optional<std::string> validate_multiplayer_server_config(
    const multiplayer_server_config &config );
multiplayer_server_config_result parse_multiplayer_server_config( const std::string &json );
multiplayer_server_config_result load_multiplayer_server_config(
    const std::filesystem::path &path );
std::string serialize_multiplayer_server_config( const multiplayer_server_config &config );
bool write_default_multiplayer_server_config( const std::filesystem::path &path,
        std::string &error );
bool initialize_multiplayer_server_files( const std::filesystem::path &config_path,
        std::string &error );
bool load_multiplayer_server_bearer_token( const std::filesystem::path &config_path,
        const multiplayer_server_config &config, std::string &token, std::string &error );

#endif // CATA_SRC_MULTIPLAYER_SERVER_CONFIG_H

#include "multiplayer_server_config.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include "cata_assert.h"
#include "json.h"
#include "json_loader.h"
#include "multiplayer_crypto.h"

namespace
{

constexpr std::uintmax_t maximum_config_bytes = 1024 * 1024;

void reject_unknown_members( const JsonObject &object,
                             const std::initializer_list<std::string_view> allowed,
                             const std::string_view context )
{
    for( const JsonMember member : object ) {
        if( member.is_comment() ) {
            continue;
        }
        const bool is_allowed = std::find( allowed.begin(), allowed.end(), member.name() ) !=
                                allowed.end();
        if( !is_allowed ) {
            member.throw_error( "unknown " + std::string( context ) + " field '" +
                                member.name() + "'" );
        }
    }
}

JsonObject require_object( const JsonObject &parent, const std::string_view name )
{
    if( !parent.has_object( name ) ) {
        parent.throw_error_at( name, "required object is missing or has the wrong type" );
    }
    return parent.get_object( name );
}

bool has_forbidden_path_character( const std::string &value )
{
    for( const unsigned char ch : value ) {
        if( ch < 0x20 || ch == 0x7f || ch == '\\' || ch == '/' || ch == ':' || ch == '*' ||
            ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|' ) {
            return true;
        }
    }
    return false;
}

bool is_reserved_world_name( const std::string &value )
{
    if( value.back() == '.' || value.back() == ' ' ) {
        return true;
    }
    std::string lowercase = value;
    std::transform( lowercase.begin(), lowercase.end(), lowercase.begin(),
    []( const unsigned char ch ) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>( ch - 'A' + 'a' ) :
               static_cast<char>( ch );
    } );
    if( lowercase == "." || lowercase == ".." || lowercase == "save" ||
        lowercase == "tutorial" || lowercase == "defense" ) {
        return true;
    }
    const std::string stem = lowercase.substr( 0, lowercase.find( '.' ) );
    if( stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ) {
        return true;
    }
    return stem.size() == 4 && ( stem.compare( 0, 3, "com" ) == 0 ||
                                 stem.compare( 0, 3, "lpt" ) == 0 ) &&
           stem.back() >= '1' && stem.back() <= '9';
}

bool is_identifier( const std::string &value, const std::size_t maximum_length )
{
    if( value.empty() || value.size() > maximum_length ) {
        return false;
    }
    return std::all_of( value.begin(), value.end(), []( const unsigned char ch ) {
        return ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) ||
               ( ch >= '0' && ch <= '9' ) || ch == '_' || ch == '-' || ch == '.';
    } );
}

struct parsed_listen_endpoint {
    std::string host;
    unsigned int port = 0;
};

std::optional<std::string> parse_listen_endpoint( const std::string &text,
        parsed_listen_endpoint &result )
{
    if( text.empty() || text.size() > 320 ) {
        return "network.listen must contain a host and port";
    }

    std::string_view host;
    std::string_view port;
    bool bracketed = false;
    if( text.front() == '[' ) {
        bracketed = true;
        const std::size_t close = text.find( ']' );
        if( close == std::string::npos || close == 1 || close + 1 >= text.size() ||
            text[close + 1] != ':' ) {
            return "network.listen has an invalid bracketed IPv6 endpoint";
        }
        host = std::string_view( text ).substr( 1, close - 1 );
        port = std::string_view( text ).substr( close + 2 );
    } else {
        const std::size_t separator = text.rfind( ':' );
        if( separator == std::string::npos || separator == 0 || separator + 1 >= text.size() ||
            text.find( ':' ) != separator ) {
            return "network.listen must use host:port or [IPv6]:port";
        }
        host = std::string_view( text ).substr( 0, separator );
        port = std::string_view( text ).substr( separator + 1 );
    }

    const bool valid_host_characters = std::all_of( host.begin(), host.end(),
    [bracketed]( const unsigned char ch ) {
        if( bracketed ) {
            return ( ch >= '0' && ch <= '9' ) || ( ch >= 'a' && ch <= 'f' ) ||
                   ( ch >= 'A' && ch <= 'F' ) || ch == ':' || ch == '.';
        }
        return ( ch >= '0' && ch <= '9' ) || ( ch >= 'a' && ch <= 'z' ) ||
               ( ch >= 'A' && ch <= 'Z' ) || ch == '.' || ch == '-';
    } );
    if( host.size() > 253 || !valid_host_characters ) {
        return "network.listen contains an invalid host";
    }

    unsigned int parsed_port = 0;
    const std::from_chars_result converted = std::from_chars( port.data(),
            port.data() + port.size(), parsed_port );
    if( converted.ec != std::errc() || converted.ptr != port.data() + port.size() ||
        parsed_port == 0 || parsed_port > std::numeric_limits<std::uint16_t>::max() ) {
        return "network.listen port must be between 1 and 65535";
    }

    result.host.assign( host );
    result.port = parsed_port;
    return std::nullopt;
}

bool is_ipv4_loopback( const std::string &host )
{
    std::array<unsigned int, 4> octets = {};
    std::size_t start = 0;
    for( std::size_t index = 0; index < octets.size(); ++index ) {
        const std::size_t end = index + 1 == octets.size() ? host.size() : host.find( '.', start );
        if( end == std::string::npos || end == start ) {
            return false;
        }
        const std::string_view part( host.data() + start, end - start );
        const std::from_chars_result converted = std::from_chars( part.data(),
                part.data() + part.size(), octets[index] );
        if( converted.ec != std::errc() || converted.ptr != part.data() + part.size() ||
            octets[index] > 255 ) {
            return false;
        }
        start = end + 1;
    }
    return start == host.size() + 1 && octets.front() == 127;
}

bool is_loopback_host( const std::string &host )
{
    return host == "::1" || is_ipv4_loopback( host );
}

multiplayer_transport_security parse_security( const JsonObject &network )
{
    const std::string value = network.get_string( "tls" );
    if( value == "disabled" ) {
        return multiplayer_transport_security::disabled;
    }
    if( value == "external_tunnel" ) {
        return multiplayer_transport_security::external_tunnel;
    }
    if( value == "required" ) {
        network.throw_error_at( "tls", "embedded TLS is not available; use external_tunnel" );
    }
    network.throw_error_at( "tls", "expected disabled or external_tunnel" );
}

multiplayer_character_policy parse_character_policy( const JsonObject &players )
{
    const std::string value = players.get_string( "character_policy" );
    if( value == "server_owned" ) {
        return multiplayer_character_policy::server_owned;
    }
    if( value == "portable_lease" ) {
        return multiplayer_character_policy::portable_lease;
    }
    if( value == "copy_in" ) {
        return multiplayer_character_policy::copy_in;
    }
    players.throw_error_at( "character_policy",
                            "expected server_owned, portable_lease, or copy_in" );
}

multiplayer_server_config parse_config_object( const JsonObject &root )
{
    reject_unknown_members( root, { "server_config_schema", "world", "network",
                                    "authentication", "players", "time", "save"
                                  }, "server config" );
    if( !root.has_int( "server_config_schema" ) ) {
        root.throw_error_at( "server_config_schema", "required integer schema is missing" );
    }

    multiplayer_server_config config;
    config.schema_version = root.get_int( "server_config_schema" );
    if( config.schema_version != multiplayer_server_config::current_schema_version ) {
        root.throw_error_at( "server_config_schema", "unsupported server config schema" );
    }

    const JsonObject world = require_object( root, "world" );
    reject_unknown_members( world, { "name", "seed", "mods", "options", "spawn_policy" },
                            "world" );
    config.world.name = world.get_string( "name" );
    config.world.seed = world.get_string( "seed" );
    config.world.mods = world.get_string_array( "mods" );
    config.world.spawn_policy = world.get_string( "spawn_policy" );
    const JsonObject options = require_object( world, "options" );
    config.world.options.clear();
    for( const JsonMember option : options ) {
        if( option.is_comment() ) {
            continue;
        }
        config.world.options.emplace( option.name(), option.get_string() );
    }

    const JsonObject network = require_object( root, "network" );
    reject_unknown_members( network,
    { "listen", "tls", "allow_insecure_lan", "handshake_timeout_ms" },
    "network" );
    config.network.listen = network.get_string( "listen" );
    config.network.security = parse_security( network );
    config.network.allow_insecure_lan = network.get_bool( "allow_insecure_lan" );
    config.network.handshake_timeout_ms = network.get_int( "handshake_timeout_ms" );

    const JsonObject authentication = require_object( root, "authentication" );
    reject_unknown_members( authentication, {
        "mode", "token_file", "resume_token_seconds",
        "maximum_attempts_per_minute"
    }, "authentication" );
    config.authentication.mode = authentication.get_string( "mode" );
    config.authentication.token_file = authentication.get_string( "token_file" );
    config.authentication.resume_token_seconds = authentication.get_int( "resume_token_seconds" );
    config.authentication.maximum_attempts_per_minute =
        authentication.get_int( "maximum_attempts_per_minute" );

    const JsonObject players = require_object( root, "players" );
    reject_unknown_members( players, { "max", "character_policy", "tether_tiles",
                                       "disconnect_grace_seconds"
                                     }, "players" );
    config.players.maximum = players.get_int( "max" );
    config.players.character_policy = parse_character_policy( players );
    config.players.tether_tiles = players.get_int( "tether_tiles" );
    config.players.disconnect_grace_seconds = players.get_int( "disconnect_grace_seconds" );

    const JsonObject time = require_object( root, "time" );
    reject_unknown_members( time, { "policy", "idle_timeout_seconds", "fast_forward" },
                            "time" );
    config.time.policy = time.get_string( "policy" );
    config.time.idle_timeout_seconds = time.get_int( "idle_timeout_seconds" );
    config.time.fast_forward = time.get_string( "fast_forward" );

    const JsonObject save = require_object( root, "save" );
    reject_unknown_members( save, { "interval_turns", "keep_generations" }, "save" );
    config.save.interval_turns = save.get_int( "interval_turns" );
    config.save.keep_generations = save.get_int( "keep_generations" );
    return config;
}

} // namespace

const char *multiplayer_transport_security_name(
    const multiplayer_transport_security security ) noexcept
{
    switch( security ) {
        case multiplayer_transport_security::disabled:
            return "disabled";
        case multiplayer_transport_security::external_tunnel:
            return "external_tunnel";
    }
    cata_assert( false );
    return "unknown";
}

const char *multiplayer_character_policy_name( const multiplayer_character_policy policy ) noexcept
{
    switch( policy ) {
        case multiplayer_character_policy::server_owned:
            return "server_owned";
        case multiplayer_character_policy::portable_lease:
            return "portable_lease";
        case multiplayer_character_policy::copy_in:
            return "copy_in";
    }
    cata_assert( false );
    return "unknown";
}

std::optional<std::string> parse_multiplayer_server_listen_endpoint(
    const std::string &text, multiplayer_server_listen_endpoint &result )
{
    parsed_listen_endpoint parsed;
    if( const std::optional<std::string> error = parse_listen_endpoint( text, parsed ) ) {
        return error;
    }
    result.host = std::move( parsed.host );
    result.port = static_cast<std::uint16_t>( parsed.port );
    return std::nullopt;
}

std::optional<std::string> validate_multiplayer_server_config(
    const multiplayer_server_config &config )
{
    if( config.schema_version != multiplayer_server_config::current_schema_version ) {
        return "unsupported server config schema";
    }
    if( config.world.name.empty() || config.world.name.size() > 64 ||
        has_forbidden_path_character( config.world.name ) ||
        is_reserved_world_name( config.world.name ) ) {
        return "world.name must be a safe non-empty filename of at most 64 bytes";
    }
    if( config.world.seed.empty() || config.world.seed.size() > 128 ||
    std::any_of( config.world.seed.begin(), config.world.seed.end(), []( const unsigned char ch ) {
    return ch < 0x20 || ch == 0x7f;
} ) ) {
        return "world.seed must contain 1 to 128 printable bytes";
    }
    if( config.world.mods.empty() || config.world.mods.size() > 128 ) {
        return "world.mods must contain between 1 and 128 mod IDs";
    }
    std::set<std::string> unique_mods;
    for( const std::string &mod : config.world.mods ) {
        if( !is_identifier( mod, 128 ) ) {
            return "world.mods contains an invalid mod ID";
        }
        if( !unique_mods.insert( mod ).second ) {
            return "world.mods contains a duplicate mod ID";
        }
    }
    if( config.world.options.size() > 1024 ) {
        return "world.options contains too many entries";
    }
    for( const auto &entry : config.world.options ) {
        if( !is_identifier( entry.first, 128 ) || entry.second.size() > 1024 ) {
            return "world.options contains an invalid key or value";
        }
    }
    if( config.world.spawn_policy != "shared_start" ) {
        return "world.spawn_policy must be shared_start";
    }

    parsed_listen_endpoint endpoint;
    if( const std::optional<std::string> endpoint_error =
            parse_listen_endpoint( config.network.listen, endpoint ) ) {
        return endpoint_error;
    }
    if( !is_loopback_host( endpoint.host ) &&
        config.network.security == multiplayer_transport_security::disabled &&
        !config.network.allow_insecure_lan ) {
        return "non-loopback plaintext requires external_tunnel or allow_insecure_lan=true";
    }
    if( config.network.handshake_timeout_ms < 1000 ||
        config.network.handshake_timeout_ms > 60000 ) {
        return "network.handshake_timeout_ms must be between 1000 and 60000";
    }

    if( config.authentication.mode != "token" && config.authentication.mode != "disabled" ) {
        return "authentication.mode must be token or disabled";
    }
    if( config.authentication.mode == "token" ) {
        if( config.authentication.token_file.empty() ||
            config.authentication.token_file.size() > 64 ||
            has_forbidden_path_character( config.authentication.token_file ) ||
            is_reserved_world_name( config.authentication.token_file ) ) {
            return "authentication.token_file must be a safe relative filename";
        }
    } else if( !config.authentication.token_file.empty() ) {
        return "authentication.token_file must be empty when authentication is disabled";
    }
    if( config.authentication.resume_token_seconds < 60 ||
        config.authentication.resume_token_seconds > 86400 ) {
        return "authentication.resume_token_seconds must be between 60 and 86400";
    }
    if( config.authentication.maximum_attempts_per_minute == 0 ||
        config.authentication.maximum_attempts_per_minute > 120 ) {
        return "authentication.maximum_attempts_per_minute must be between 1 and 120";
    }
    if( !is_loopback_host( endpoint.host ) && config.authentication.mode == "disabled" ) {
        return "authentication cannot be disabled on a non-loopback listener";
    }

    if( config.players.maximum < 1 || config.players.maximum > 4 ) {
        return "players.max must be between 1 and 4";
    }
    if( config.players.maximum != 1 ) {
        return "players.max values above 1 require the Phase 3 shared turn scheduler, which is not implemented";
    }
    if( config.players.character_policy != multiplayer_character_policy::server_owned ) {
        return "players.character_policy is not implemented yet; use server_owned";
    }
    if( config.players.tether_tiles < 12 || config.players.tether_tiles > 48 ) {
        return "players.tether_tiles must be between 12 and 48";
    }
    if( config.players.disconnect_grace_seconds < 0 ||
        config.players.disconnect_grace_seconds > 3600 ) {
        return "players.disconnect_grace_seconds must be between 0 and 3600";
    }
    if( config.time.policy != "turn_barrier" ) {
        return "time.policy must be turn_barrier";
    }
    if( config.time.idle_timeout_seconds < 0 || config.time.idle_timeout_seconds > 86400 ) {
        return "time.idle_timeout_seconds must be between 0 and 86400";
    }
    if( config.time.fast_forward != "all_players_safe" &&
        config.time.fast_forward != "disabled" ) {
        return "time.fast_forward must be all_players_safe or disabled";
    }
    if( config.save.interval_turns < 0 || config.save.interval_turns > 1000000 ) {
        return "save.interval_turns must be between 0 and 1000000";
    }
    if( config.save.keep_generations < 1 || config.save.keep_generations > 100 ) {
        return "save.keep_generations must be between 1 and 100";
    }
    if( config.save.keep_generations != 1 ) {
        return "save.keep_generations values above 1 require Phase 5 generation saves, which are not implemented";
    }
    return std::nullopt;
}

multiplayer_server_config_result parse_multiplayer_server_config( const std::string &json )
{
    multiplayer_server_config_result result;
    if( json.size() > maximum_config_bytes ) {
        result.error = "server config exceeds the 1 MiB limit";
        return result;
    }
    try {
        JsonValue value = json_loader::from_string( json );
        if( !value.test_object() ) {
            value.throw_error( "server config root must be an object" );
        }
        multiplayer_server_config config = parse_config_object( value.get_object() );
        if( const std::optional<std::string> validation_error =
                validate_multiplayer_server_config( config ) ) {
            result.error = *validation_error;
            return result;
        }
        result.config = std::move( config );
    } catch( const std::exception &error ) {
        result.error = error.what();
    }
    return result;
}

multiplayer_server_config_result load_multiplayer_server_config(
    const std::filesystem::path &path )
{
    multiplayer_server_config_result result;
    std::error_code file_error;
    const std::uintmax_t size = std::filesystem::file_size( path, file_error );
    if( file_error ) {
        result.error = "unable to stat server config '" + path.u8string() + "': " +
                       file_error.message();
        return result;
    }
    if( size > maximum_config_bytes ) {
        result.error = "server config exceeds the 1 MiB limit";
        return result;
    }
    std::ifstream input( path, std::ios::binary );
    if( !input ) {
        result.error = "unable to open server config '" + path.u8string() + "'";
        return result;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if( input.bad() ) {
        result.error = "unable to read server config '" + path.u8string() + "'";
        return result;
    }
    return parse_multiplayer_server_config( buffer.str() );
}

std::string serialize_multiplayer_server_config( const multiplayer_server_config &config )
{
    std::ostringstream output;
    JsonOut json( output, true );
    json.start_object();
    json.member( "server_config_schema", config.schema_version );
    json.member( "world" );
    json.start_object();
    json.member( "name", config.world.name );
    json.member( "seed", config.world.seed );
    json.member( "mods", config.world.mods );
    json.member( "options", config.world.options );
    json.member( "spawn_policy", config.world.spawn_policy );
    json.end_object();
    json.member( "network" );
    json.start_object();
    json.member( "listen", config.network.listen );
    json.member( "tls", multiplayer_transport_security_name( config.network.security ) );
    json.member( "allow_insecure_lan", config.network.allow_insecure_lan );
    json.member( "handshake_timeout_ms", config.network.handshake_timeout_ms );
    json.end_object();
    json.member( "authentication" );
    json.start_object();
    json.member( "mode", config.authentication.mode );
    json.member( "token_file", config.authentication.token_file );
    json.member( "resume_token_seconds", config.authentication.resume_token_seconds );
    json.member( "maximum_attempts_per_minute",
                 config.authentication.maximum_attempts_per_minute );
    json.end_object();
    json.member( "players" );
    json.start_object();
    json.member( "max", config.players.maximum );
    json.member( "character_policy",
                 multiplayer_character_policy_name( config.players.character_policy ) );
    json.member( "tether_tiles", config.players.tether_tiles );
    json.member( "disconnect_grace_seconds", config.players.disconnect_grace_seconds );
    json.end_object();
    json.member( "time" );
    json.start_object();
    json.member( "policy", config.time.policy );
    json.member( "idle_timeout_seconds", config.time.idle_timeout_seconds );
    json.member( "fast_forward", config.time.fast_forward );
    json.end_object();
    json.member( "save" );
    json.start_object();
    json.member( "interval_turns", config.save.interval_turns );
    json.member( "keep_generations", config.save.keep_generations );
    json.end_object();
    json.end_object();
    output << '\n';
    return output.str();
}

bool write_default_multiplayer_server_config( const std::filesystem::path &path,
        std::string &error )
{
    std::error_code filesystem_error;
    if( std::filesystem::exists( path, filesystem_error ) ) {
        error = "refusing to overwrite existing server config '" + path.u8string() + "'";
        return false;
    }
    if( filesystem_error ) {
        error = "unable to inspect server config path '" + path.u8string() + "': " +
                filesystem_error.message();
        return false;
    }
    const std::filesystem::path parent = path.parent_path();
    if( !parent.empty() ) {
        const bool parent_is_directory = std::filesystem::is_directory( parent, filesystem_error );
        if( filesystem_error ) {
            error = "unable to inspect server config parent directory '" + parent.u8string() +
                    "': " + filesystem_error.message();
            return false;
        }
        if( !parent_is_directory ) {
            error = "server config parent directory does not exist: '" + parent.u8string() + "'";
            return false;
        }
    }
    if( !multiplayer_write_private_file_exclusive(
            path, serialize_multiplayer_server_config( multiplayer_server_config() ), error ) ) {
        error = "unable to create server config '" + path.u8string() + "': " + error;
        return false;
    }
    error.clear();
    return true;
}

bool initialize_multiplayer_server_files( const std::filesystem::path &config_path,
        std::string &error )
{
    const multiplayer_server_config defaults;
    const std::filesystem::path token_path = config_path.parent_path() /
            std::filesystem::u8path( defaults.authentication.token_file );
    std::error_code filesystem_error;
    if( std::filesystem::exists( config_path, filesystem_error ) ) {
        error = "refusing to overwrite existing server config '" + config_path.u8string() + "'";
        return false;
    }
    if( filesystem_error ) {
        error = "unable to inspect server config path '" + config_path.u8string() + "': " +
                filesystem_error.message();
        return false;
    }
    if( std::filesystem::exists( token_path, filesystem_error ) ) {
        error = "refusing to overwrite existing server token '" + token_path.u8string() + "'";
        return false;
    }
    if( filesystem_error ) {
        error = "unable to inspect server token path '" + token_path.u8string() + "': " +
                filesystem_error.message();
        return false;
    }

    std::string token;
    if( !multiplayer_generate_bearer_token( token, error ) ) {
        return false;
    }
    if( !multiplayer_write_private_file_exclusive( token_path, token + '\n', error ) ) {
        error = "unable to create server token '" + token_path.u8string() + "': " + error;
        return false;
    }
    if( !write_default_multiplayer_server_config( config_path, error ) ) {
        std::filesystem::remove( token_path, filesystem_error );
        return false;
    }
    error.clear();
    return true;
}

bool load_multiplayer_server_bearer_token( const std::filesystem::path &config_path,
        const multiplayer_server_config &config, std::string &token, std::string &error )
{
    token.clear();
    if( config.authentication.mode == "disabled" ) {
        error.clear();
        return true;
    }
    if( config.authentication.mode != "token" ) {
        error = "server authentication mode is unsupported";
        return false;
    }
    const std::filesystem::path token_path = config_path.parent_path() /
            std::filesystem::u8path( config.authentication.token_file );
    std::error_code filesystem_error;
    const std::filesystem::file_status token_status = std::filesystem::symlink_status(
                token_path, filesystem_error );
    if( filesystem_error || token_status.type() != std::filesystem::file_type::regular ) {
        error = "server authentication token path is not a regular file";
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size( token_path, filesystem_error );
    if( filesystem_error || size > 128 ) {
        error = "unable to read a bounded server authentication token file";
        return false;
    }
#if !defined(_WIN32)
    const std::filesystem::perms permissions =
        std::filesystem::status( token_path, filesystem_error ).permissions();
    constexpr std::filesystem::perms non_owner_permissions =
        std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    if( filesystem_error || ( permissions & non_owner_permissions ) != std::filesystem::perms::none ) {
        error = "server authentication token file must not grant group or other permissions";
        return false;
    }
#endif
    std::ifstream input( token_path, std::ios::binary );
    std::ostringstream contents;
    contents << input.rdbuf();
    if( !input || input.bad() ) {
        error = "unable to read server authentication token file";
        return false;
    }
    token = contents.str();
    if( !token.empty() && token.back() == '\n' ) {
        token.pop_back();
    }
    if( !token.empty() && token.back() == '\r' ) {
        token.pop_back();
    }
    if( !multiplayer_is_valid_bearer_token( token ) ) {
        token.clear();
        error = "server authentication token file has an invalid format";
        return false;
    }
    error.clear();
    return true;
}

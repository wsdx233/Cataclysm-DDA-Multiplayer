#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "multiplayer_runtime_mode.h"
#include "multiplayer_crypto.h"
#include "multiplayer_server_config.h"

TEST_CASE( "multiplayer_runtime_modes_classify_local_ui", "[multiplayer][server_config]" )
{
    CHECK( std::string( multiplayer_runtime_mode_name(
                            multiplayer_runtime_mode::local_client ) ) == "local_client" );
    CHECK( std::string( multiplayer_runtime_mode_name(
                            multiplayer_runtime_mode::dedicated_server ) ) == "dedicated_server" );
    CHECK( multiplayer_runtime_mode_uses_local_ui( multiplayer_runtime_mode::local_client ) );
    CHECK( multiplayer_runtime_mode_uses_local_ui( multiplayer_runtime_mode::network_client ) );
    CHECK_FALSE( multiplayer_runtime_mode_uses_local_ui(
                     multiplayer_runtime_mode::dedicated_server ) );
    CHECK_FALSE( multiplayer_runtime_mode_uses_local_ui( multiplayer_runtime_mode::test ) );
}

TEST_CASE( "multiplayer_server_config_default_round_trip", "[multiplayer][server_config]" )
{
    multiplayer_server_config config;
    config.world.options.emplace( "CITY_SIZE", "4" );
    const std::string serialized = serialize_multiplayer_server_config( config );
    const multiplayer_server_config_result loaded = parse_multiplayer_server_config( serialized );

    INFO( loaded.error );
    REQUIRE( loaded );
    CHECK( loaded.config->schema_version == multiplayer_server_config::current_schema_version );
    CHECK( loaded.config->world.name == "coop-world" );
    CHECK( loaded.config->world.mods == std::vector<std::string> { "dda" } );
    CHECK( loaded.config->world.options.at( "CITY_SIZE" ) == "4" );
    CHECK( loaded.config->network.listen == "127.0.0.1:27999" );
    CHECK( loaded.config->network.security == multiplayer_transport_security::disabled );
    CHECK_FALSE( loaded.config->network.allow_insecure_lan );
    CHECK( loaded.config->authentication.mode == "token" );
    CHECK( loaded.config->authentication.token_file == "server-auth-token.txt" );
    CHECK( loaded.config->players.maximum == 1 );
    CHECK( loaded.config->players.character_policy ==
           multiplayer_character_policy::server_owned );
    CHECK( loaded.config->time.policy == "turn_barrier" );
    CHECK( loaded.config->save.keep_generations == 1 );
}

TEST_CASE( "multiplayer_server_config_rejects_unknown_and_unsupported_security_fields",
           "[multiplayer][server_config]" )
{
    const std::string valid = serialize_multiplayer_server_config( multiplayer_server_config() );

    std::string unknown = valid;
    const std::string final_network_field = "\"allow_insecure_lan\": false";
    const std::size_t network_end = unknown.find( final_network_field );
    REQUIRE( network_end != std::string::npos );
    unknown.insert( network_end + final_network_field.size(),
                    ",\n        \"certificate\": \"server.crt\"" );
    const multiplayer_server_config_result unknown_result =
        parse_multiplayer_server_config( unknown );
    CHECK_FALSE( unknown_result );
    CHECK( unknown_result.error.find( "unknown network field 'certificate'" ) !=
           std::string::npos );

    std::string embedded_tls = valid;
    const std::size_t disabled = embedded_tls.find( "\"disabled\"" );
    REQUIRE( disabled != std::string::npos );
    embedded_tls.replace( disabled, std::string( "\"disabled\"" ).size(), "\"required\"" );
    const multiplayer_server_config_result tls_result =
        parse_multiplayer_server_config( embedded_tls );
    CHECK_FALSE( tls_result );
    CHECK( tls_result.error.find( "embedded TLS is not available" ) != std::string::npos );
}

TEST_CASE( "multiplayer_server_config_enforces_listener_security", "[multiplayer][server_config]" )
{
    multiplayer_server_config config;

    config.network.listen = "127.42.0.9:27999";
    CHECK_FALSE( validate_multiplayer_server_config( config ).has_value() );
    config.network.listen = "[::1]:27999";
    CHECK_FALSE( validate_multiplayer_server_config( config ).has_value() );

    config.network.listen = "0.0.0.0:27999";
    const std::optional<std::string> unsafe = validate_multiplayer_server_config( config );
    REQUIRE( unsafe );
    CHECK( unsafe->find( "non-loopback plaintext" ) != std::string::npos );

    config.network.allow_insecure_lan = true;
    CHECK_FALSE( validate_multiplayer_server_config( config ).has_value() );
    config.network.allow_insecure_lan = false;
    config.network.security = multiplayer_transport_security::external_tunnel;
    CHECK_FALSE( validate_multiplayer_server_config( config ).has_value() );

    config.authentication.mode = "disabled";
    config.authentication.token_file.clear();
    const std::optional<std::string> unauthenticated_remote =
        validate_multiplayer_server_config( config );
    REQUIRE( unauthenticated_remote );
    CHECK( unauthenticated_remote->find( "authentication cannot be disabled" ) !=
           std::string::npos );

    config.network.listen = "127.example:27999";
    config.network.security = multiplayer_transport_security::disabled;
    config.authentication.mode = "token";
    config.authentication.token_file = "server-auth-token.txt";
    const std::optional<std::string> deceptive_loopback =
        validate_multiplayer_server_config( config );
    REQUIRE( deceptive_loopback );
    CHECK( deceptive_loopback->find( "non-loopback plaintext" ) != std::string::npos );
}

TEST_CASE( "multiplayer_server_config_validates_paths_and_resource_limits",
           "[multiplayer][server_config]" )
{
    multiplayer_server_config config;

    config.world.name = "../world";
    CHECK( validate_multiplayer_server_config( config ).has_value() );
    config.world.name = "CON.txt";
    CHECK( validate_multiplayer_server_config( config ).has_value() );
    config.world.name = "coop-world";
    config.world.mods.push_back( "dda" );
    CHECK( validate_multiplayer_server_config( config ).has_value() );
    config.world.mods = { "dda" };
    config.players.maximum = 5;
    CHECK( validate_multiplayer_server_config( config ).has_value() );
    config.players.maximum = 4;
    const std::optional<std::string> unavailable_scheduler =
        validate_multiplayer_server_config( config );
    REQUIRE( unavailable_scheduler );
    CHECK( unavailable_scheduler->find( "not implemented" ) != std::string::npos );
    config.players.maximum = 1;
    config.players.character_policy = multiplayer_character_policy::portable_lease;
    const std::optional<std::string> unavailable_policy =
        validate_multiplayer_server_config( config );
    REQUIRE( unavailable_policy );
    CHECK( unavailable_policy->find( "not implemented" ) != std::string::npos );
    config.players.character_policy = multiplayer_character_policy::server_owned;
    config.players.tether_tiles = 49;
    CHECK( validate_multiplayer_server_config( config ).has_value() );
    config.players.tether_tiles = 48;
    config.save.keep_generations = 2;
    const std::optional<std::string> unavailable_generation_saves =
        validate_multiplayer_server_config( config );
    REQUIRE( unavailable_generation_saves );
    CHECK( unavailable_generation_saves->find( "not implemented" ) != std::string::npos );
    config.save.keep_generations = 1;
    config.authentication.token_file = "../token";
    CHECK( validate_multiplayer_server_config( config ).has_value() );
}

TEST_CASE( "multiplayer_server_config_file_init_refuses_overwrite",
           "[multiplayer][server_config]" )
{
    const std::string unique = "cdda-multiplayer-config-" + std::to_string(
                                   std::chrono::steady_clock::now().time_since_epoch().count() );
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / unique;
    REQUIRE( std::filesystem::create_directory( directory ) );
    on_out_of_scope cleanup( [&directory]() {
        std::error_code ignored;
        std::filesystem::remove_all( directory, ignored );
    } );
    const std::filesystem::path path = directory / "server.json";

    std::string error;
    REQUIRE( write_default_multiplayer_server_config( path, error ) );
    CHECK( error.empty() );
    CHECK_FALSE( write_default_multiplayer_server_config( path, error ) );
    CHECK( error.find( "refusing to overwrite" ) != std::string::npos );

    const multiplayer_server_config_result loaded = load_multiplayer_server_config( path );
    INFO( loaded.error );
    REQUIRE( loaded );
    CHECK( loaded.config->network.listen == "127.0.0.1:27999" );

    const std::filesystem::path bundle_path = directory / "bundle.json";
    REQUIRE( initialize_multiplayer_server_files( bundle_path, error ) );
    const std::filesystem::path token_path = directory / "server-auth-token.txt";
    REQUIRE( std::filesystem::is_regular_file( token_path ) );
    std::ifstream token_input( token_path, std::ios::binary );
    std::ostringstream token_buffer;
    token_buffer << token_input.rdbuf();
    std::string token = token_buffer.str();
    REQUIRE_FALSE( token.empty() );
    if( token.back() == '\n' ) {
        token.pop_back();
    }
    CHECK( multiplayer_is_valid_bearer_token( token ) );
    const multiplayer_server_config_result bundle_config =
        load_multiplayer_server_config( bundle_path );
    REQUIRE( bundle_config );
    std::string loaded_token;
    REQUIRE( load_multiplayer_server_bearer_token( bundle_path, *bundle_config.config,
             loaded_token, error ) );
    CHECK( loaded_token == token );
#if !defined(_WIN32)
    std::filesystem::permissions( token_path, std::filesystem::perms::group_read,
                                  std::filesystem::perm_options::add );
    CHECK_FALSE( load_multiplayer_server_bearer_token( bundle_path, *bundle_config.config,
                 loaded_token, error ) );
    CHECK( error.find( "group or other" ) != std::string::npos );
#endif
    CHECK_FALSE( initialize_multiplayer_server_files( bundle_path, error ) );
    CHECK( error.find( "refusing to overwrite" ) != std::string::npos );
}

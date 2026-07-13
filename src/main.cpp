/* Main Loop for cataclysm
 * Linux only I guess
 * But maybe not
 * Who knows
 */

// KG: Yes, the above is inaccurate now. It's also a poem, it stays.

// IWYU pragma: no_include <sys/signal.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include "cata_allocator.h"
#include "platform_win.h"
#endif

#include <flatbuffers/util.h>

#include "cached_options.h"
#include "cata_allocator.h"
#include "cata_path.h"
#include "avatar.h"
#include "color.h"
#include "compatibility.h"
#include "crash.h"
#include "cursesdef.h"
#include "debug.h"
#include "do_turn.h"
#include "event.h"
#include "event_bus.h"
#include "filesystem.h"
#include "game.h"
#include "game_constants.h"
#include "game_ui.h"
#include "get_version.h"
#include "help.h"
#include "input.h"
#include "main_menu.h"
#include "mapsharing.h"
#include "memory_fast.h"
#include "loading_ui.h"
#include "multiplayer_command_executor.h"
#include "multiplayer_player_runtime.h"
#include "multiplayer_runtime_mode.h"
#include "multiplayer_scene.h"
#include "multiplayer_server.h"
#include "multiplayer_server_config.h"
#include "multiplayer_server_log.h"
#include "options.h"
#include "ordered_static_globals.h"
#include "output.h"
#include "path_info.h"
#include "rng.h"
#include "system_locale.h"
#include "translations.h"
#include "type_id.h"
#include "ui_manager.h"
#include "worldfactory.h"
#include "cata_imgui.h"
#if defined(MACOSX) || defined(__CYGWIN__)
#   include <unistd.h> // getpid()
#endif

#if defined(EMSCRIPTEN)
#include <emscripten.h>
#endif

#if defined(PREFIX)
#   undef PREFIX
#   include "prefix.h"
#endif

class ui_adaptor;

#if defined(TILES) || defined(SDL_SOUND)
#   include "sdl_version_wrappers.h"
#endif

#if defined(TILES)
#   include "sdltiles.h"
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include "sdl_wrappers.h" // for GetAndroidExternalStoragePath(), SDL_main

// Taken from: https://codelab.wordpress.com/2014/11/03/how-to-use-standard-output-streams-for-logging-in-android-apps/
// Force Android standard output to adb logcat output

static int pfd[2];
static pthread_t thr;
static const char *tag = "cdda";

static void *thread_func( void * )
{
    ssize_t rdsz;
    char buf[128];
    for( ;; ) {
        if( ( ( rdsz = read( pfd[0], buf, sizeof buf - 1 ) ) > 0 ) ) {
            if( buf[rdsz - 1] == '\n' ) {
                --rdsz;
            }
            buf[rdsz] = 0;  /* add null-terminator */
            __android_log_write( ANDROID_LOG_DEBUG, tag, buf );
        }
    }
    return 0;
}

int start_logger( const char *app_name )
{
    tag = app_name;

    /* make stdout line-buffered and stderr unbuffered */
    setvbuf( stdout, 0, _IOLBF, 0 );
    setvbuf( stderr, 0, _IONBF, 0 );

    /* create the pipe and redirect stdout and stderr */
    pipe( pfd );
    dup2( pfd[1], 1 );
    dup2( pfd[1], 2 );

    /* spawn the logging thread */
    if( pthread_create( &thr, 0, thread_func, 0 ) == -1 ) {
        return -1;
    }
    pthread_detach( thr );
    return 0;
}

#endif //__ANDROID__

namespace
{

#if defined(_WIN32) and defined(TILES)
// Used only if AttachConsole() works
FILE *CONOUT;
#endif
void exit_handler( int s )
{
    const int old_timeout = inp_mngr.get_timeout();
    inp_mngr.reset_timeout();
    if( s != 2 || query_yn( _( "Really Quit?  All unsaved changes will be lost." ) ) ) {
        deinitDebug();

        int exit_status = 0;
        g.reset();

        catacurses::endwin();

#if defined(__ANDROID__)
        // Avoid capturing SIGABRT on exit on Android in crash report
        // Can be removed once the SIGABRT on exit problem is fixed
        signal( SIGABRT, SIG_DFL );
#endif

        imclient.reset();
        exit( exit_status );
    }
    inp_mngr.set_timeout( old_timeout );
    ui_manager::redraw_invalidated();
    catacurses::doupdate();
}

struct arg_handler {
    //! Handler function to be invoked when this argument is encountered. The handler will be
    //! called with the number of parameters after the flag was encountered, along with the array
    //! of following parameters. It must return an integer indicating how many parameters were
    //! consumed by the call or -1 to indicate that a required argument was missing.
    using handler_method = std::function<int ( int, const char ** )>;

    std::string_view flag;  //!< The commandline parameter to handle (e.g., "--seed").
    std::string_view param_documentation;  //!< Human readable description of this arguments parameter.
    std::string_view documentation;  //!< Human readable documentation for this argument.
    std::string_view help_group; //!< Section of the help message in which to include this argument.
    int num_args; //!< How many further arguments are expected for this parameter (usually 0 or 1).
    handler_method handler;  //!< The callback to be invoked when this argument is encountered.
};

template<typename FirstPassArgs, typename SecondPassArgs>
void printHelpMessage( const FirstPassArgs &first_pass_arguments,
                       const SecondPassArgs &second_pass_arguments )
{
    // Group all arguments by help_group.
    std::multimap<std::string, const arg_handler *> help_map;
    for( const arg_handler &handler : first_pass_arguments ) {
        help_map.emplace( handler.help_group, &handler );
    }
    for( const arg_handler &handler : second_pass_arguments ) {
        help_map.emplace( handler.help_group, &handler );
    }

    std::cout << "Command line parameters:\n";
    std::string current_help_group;
    for( std::pair<const std::string, const arg_handler *> &help_entry : help_map ) {
        if( help_entry.first != current_help_group ) {
            current_help_group = help_entry.first;
            std::cout << "\n" << current_help_group << "\n";
        }

        const arg_handler *handler = help_entry.second;
        std::cout << handler->flag << " " << handler->param_documentation;
        if( !handler->documentation.empty() ) {
            std::cout << "\n    " << handler->documentation << "\n";
        }
    }
    std::cout << std::endl;
}

/**
 * Displays current application version and compile options values
 */
void printVersionMessage()
{
#if defined(TILES)
    const bool hasTiles = true;
#else
    const bool hasTiles = false;
#endif

#if defined(SDL_SOUND)
    const bool hasSound = true;
#else
    const bool hasSound = false;
#endif

    printf( "Cataclysm Dark Days Ahead: %s\n\n"
            "%ctiles, %csound\n\n"
            "data dir: %s\nuser dir: %s\n",
            getVersionString(),
            hasTiles ? '+' : '-',
            hasSound ? '+' : '-',
            PATH_INFO::datadir().c_str(),
            PATH_INFO::user_dir().c_str() );
}

void process_args( const char **argv, int argc, const std::vector<arg_handler> &arg_handlers )
{
    while( argc ) {
        bool arg_handled = false;
        for( const arg_handler &handler : arg_handlers ) {
            if( handler.flag == argv[0] ) {
                argc--;
                argv++;
                if( argc < handler.num_args ) {
                    std::cout << "Missing expected argument to command line parameter " << handler.flag << std::endl;
                    std::exit( 1 );
                }
                int args_consumed = handler.handler( argc, argv );
                if( args_consumed < 0 ) {
                    printf( "Failed parsing parameter '%s'\n", *( argv - 1 ) );
                    std::exit( 1 );
                }
                argc -= args_consumed;
                argv += args_consumed;
                arg_handled = true;
                break;
            }
        }
        // Skip other options.
        if( !arg_handled ) {
            --argc;
            ++argv;
        }
    }
}

enum class server_config_operation : std::uint8_t {
    none,
    check,
    initialize
};

struct cli_opts {
    int seed = time( nullptr );
    bool verifyexit = false;
    bool noverify = false;
    bool check_mods = false;
    std::vector<std::string> opts;
    std::string world; /** if set try to load first save in this world on startup */
    bool disable_ascii_art = false;
    server_config_operation server_config_action = server_config_operation::none;
    std::string server_config_path;
    multiplayer_runtime_mode runtime_mode = multiplayer_runtime_mode::local_client;
};

cli_opts parse_commandline( int argc, const char **argv )
{
    cli_opts result;

    constexpr std::string_view section_default;
    constexpr std::string_view section_map_sharing = "Map sharing";
    constexpr std::string_view section_user_directory = "User directories";
    constexpr std::string_view section_accessibility = "Accessibility";
    constexpr std::string_view section_multiplayer_server = "Multiplayer server";
    const std::vector<arg_handler> first_pass_arguments = {{
            {
                "--seed", "<string of letters and or numbers>",
                "Sets the random number generator's seed value",
                section_default,
                1,
                [&result]( int, const char **params ) -> int {
                    const unsigned char *hash_input = reinterpret_cast<const unsigned char *>( params[0] );
                    result.seed = djb2_hash( hash_input );
                    return 1;
                }
            },
            {
                "--jsonverify", {},
                "Checks the CDDA json files and exits",
                section_default,
                0,
                [&result]( int, const char ** ) -> int {
                    result.verifyexit = true;
                    return 0;
                }
            },
            {
                "--check-mods", "[mod…]",
                "Checks the json files belonging to given CDDA mod and exits",
                section_default,
                1,
                [&result]( int n, const char **params ) -> int {
                    result.check_mods = true;
                    test_mode = true;
                    for( int i = 0; i < n; ++i )
                    {
                        result.opts.emplace_back( params[ i ] );
                    }
                    return 0;
                }
            },
            {
                "--noverify", {},
                "Skips JSON verification",
                section_default,
                0,
                [&result]( int, const char ** ) -> int {
                    result.noverify = true;
                    return 0;
                }
            },
            {
                "--world", "<name>",
                "Load world",
                section_default,
                1,
                [&result]( int, const char **params ) -> int {
                    result.world = params[0];
                    return 1;
                }
            },
            {
                "--basepath", "<path>",
                "Base path for all game data subdirectories",
                section_default,
                1,
                []( int, const char **params )
                {
                    PATH_INFO::init_base_path( params[0] );
                    PATH_INFO::set_standard_filenames();
                    return 1;
                }
            },
            {
                "--shared", {},
                "Activates the map-sharing mode",
                section_map_sharing,
                0,
                []( int, const char ** ) -> int {
                    MAP_SHARING::setSharing( true );
                    MAP_SHARING::setCompetitive( true );
                    MAP_SHARING::setWorldmenu( false );
                    return 0;
                }
            },
            {
                "--username", "<name>",
                "Instructs map-sharing code to use this name for your character.",
                section_map_sharing,
                1,
                []( int, const char **params ) -> int {
                    MAP_SHARING::setUsername( params[0] );
                    return 1;
                }
            },
            {
                "--addadmin", "<username>",
                "Instructs map-sharing code to use this name for your character and give you "
                "access to the cheat functions.",
                section_map_sharing,
                1,
                []( int, const char **params ) -> int {
                    MAP_SHARING::addAdmin( params[0] );
                    return 1;
                }
            },
            {
                "--adddebugger", "<username>",
                "Informs map-sharing code that you're running inside a debugger",
                section_map_sharing,
                1,
                []( int, const char **params ) -> int {
                    MAP_SHARING::addDebugger( params[0] );
                    return 1;
                }
            },
            {
                "--competitive", {},
                "Instructs map-sharing code to disable access to the in-game cheat functions",
                section_map_sharing,
                0,
                []( int, const char ** ) -> int {
                    MAP_SHARING::setCompetitive( true );
                    return 0;
                }
            },
            {
                "--userdir", "<path>",
                // NOLINTNEXTLINE(cata-text-style): the dot is not a period
                "Base path for user-overrides to files from the ./data directory and named below",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::init_user_dir( params[0] );
                    PATH_INFO::set_standard_filenames();
                    return 1;
                }
            },
            {
                "--disable-ascii-art", {},
                "Disable aesthetic ascii art in menus and descriptions.",
                section_accessibility,
                0,
                [&result]( int, const char ** ) -> int {
                    result.disable_ascii_art = true;
                    return 0;
                }
            },
            {
                "--server", "<config path>",
                "Run the dedicated multiplayer server",
                section_multiplayer_server,
                1,
                [&result]( int, const char **params ) -> int {
                    if( result.server_config_action != server_config_operation::none ||
                        result.runtime_mode != multiplayer_runtime_mode::local_client ||
                        !result.server_config_path.empty() )
                    {
                        return -1;
                    }
                    result.runtime_mode = multiplayer_runtime_mode::dedicated_server;
                    result.server_config_path = params[0];
                    return 1;
                }
            },
            {
                "--check-server-config", "<path>",
                "Validate a multiplayer server configuration and exit",
                section_multiplayer_server,
                1,
                [&result]( int, const char **params ) -> int {
                    if( result.server_config_action != server_config_operation::none ||
                        result.runtime_mode != multiplayer_runtime_mode::local_client ||
                        !result.server_config_path.empty() )
                    {
                        return -1;
                    }
                    result.server_config_action = server_config_operation::check;
                    result.server_config_path = params[0];
                    return 1;
                }
            },
            {
                "--init-server-config", "<path>",
                "Write a safe default multiplayer server configuration and exit",
                section_multiplayer_server,
                1,
                [&result]( int, const char **params ) -> int {
                    if( result.server_config_action != server_config_operation::none ||
                        result.runtime_mode != multiplayer_runtime_mode::local_client ||
                        !result.server_config_path.empty() )
                    {
                        return -1;
                    }
                    result.server_config_action = server_config_operation::initialize;
                    result.server_config_path = params[0];
                    return 1;
                }
            }
        }
    };

    // The following arguments are dependent on one or more of the previous flags and are run
    // in a second pass.
    const std::vector<arg_handler> second_pass_arguments = {{
            {
                "--worldmenu", {},
                "Enables the world menu in the map-sharing code",
                section_map_sharing,
                0,
                []( int, const char ** ) -> int {
                    MAP_SHARING::setWorldmenu( true );
                    return true;
                }
            },
            {
                "--datadir", "<directory name>",
                "Sub directory from which game data is loaded",
                {},
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_datadir( params[0] );
                    return 1;
                }
            },
            {
                "--savedir", "<directory name>",
                "Subdirectory for game saves",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_savedir( params[0] );
                    return 1;
                }
            },
            {
                "--configdir", "<directory name>",
                "Subdirectory for game configuration",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_config_dir( params[0] );
                    return 1;
                }
            },
            {
                "--memorialdir", "<directory name>",
                "Subdirectory for memorials",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_memorialdir( params[0] );
                    return 1;
                }
            },
            {
                "--optionfile", "<filename>",
                "Name of the options file within the configdir",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_options( params[0] );
                    return 1;
                }
            },
            {
                "--keymapfile", "<filename>",
                "Name of the keymap file within the configdir",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_keymap( params[0] );
                    return 1;
                }
            },
            {
                "--autopickupfile", "<filename>",
                "Name of the autopickup options file within the configdir",
                {},
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_autopickup( params[0] );
                    return 1;
                }
            },
            {
                "--motdfile", "<filename>",
                "Name of the message of the day file within the motd directory",
                {},
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_motd( params[0] );
                    return 1;
                }
            },
        }
    };

    if( std::count( argv, argv + argc, std::string( "--help" ) ) ) {
        printHelpMessage( first_pass_arguments, second_pass_arguments );
        std::exit( 0 );
    }

    if( std::count( argv, argv + argc, std::string( "--version" ) ) ) {
        printVersionMessage();
        std::exit( 0 );
    }

    // skip program name
    --argc;
    ++argv;

    process_args( argv, argc, first_pass_arguments );
    process_args( argv, argc, second_pass_arguments );

    return result;
}

bool assure_essential_dirs_exist()
{
    using namespace PATH_INFO;
    std::vector<std::string> essential_paths{
        config_dir(),
        savedir(),
        templatedir(),
        user_font(),
        user_sound().get_unrelative_path().u8string(),
        user_gfx().get_unrelative_path().u8string()
    };
    for( const std::string &path : essential_paths ) {
        if( !assure_dir_exist( path ) ) {
            popup( _( "Unable to make directory \"%s\".  Check permissions." ), path );
            return false;
        }
    }
    return true;
}

volatile std::sig_atomic_t dedicated_server_shutdown_requested = 0;

void dedicated_server_signal_handler( int )
{
    dedicated_server_shutdown_requested = 1;
}

bool assure_dedicated_server_dirs_exist( std::string &error )
{
    for( const std::string &path : {
             PATH_INFO::config_dir(), PATH_INFO::savedir(), PATH_INFO::templatedir()
         } ) {
        if( !assure_dir_exist( path ) ) {
            error = "unable to create dedicated server directory: " + path;
            return false;
        }
    }
    error.clear();
    return true;
}

void log_dedicated_server_startup_error( const std::string &message,
        const std::string &world_id = std::string() )
{
    multiplayer_server_log_fields fields = { { "message", message } };
    if( !world_id.empty() ) {
        fields.emplace_back( "world_id", world_id );
    }
    std::cerr << multiplayer_server_log_json(
                  multiplayer_server_log_severity::error, "startup_failed", fields ) << '\n';
}

bool prepare_dedicated_server_world( const multiplayer_server_config &config, std::string &error )
{
    world_generator->init();
    std::vector<mod_id> configured_mods;
    configured_mods.reserve( config.world.mods.size() );
    for( const std::string &mod : config.world.mods ) {
        const mod_id id( mod );
        if( !id.is_valid() ) {
            error = "configured world mod is not installed: " + mod;
            return false;
        }
        configured_mods.push_back( id );
    }

    const bool created = !world_generator->has_world( config.world.name );
    WORLD *world = created ? nullptr : world_generator->get_world( config.world.name );
    if( created ) {
        world = world_generator->make_new_world( config.world.name, configured_mods );
        if( world == nullptr ) {
            error = "unable to create configured multiplayer world";
            return false;
        }
    } else if( world->active_mod_order != configured_mods ) {
        error = "existing multiplayer world mod order differs from server config";
        return false;
    }

    for( const auto &configured_option : config.world.options ) {
        const auto option = world->WORLD_OPTIONS.find( configured_option.first );
        if( option == world->WORLD_OPTIONS.end() ) {
            error = "configured multiplayer world option is unknown: " + configured_option.first;
            return false;
        }
        if( option->second.getValue( true ) != configured_option.second ) {
            if( !created ) {
                error = "existing multiplayer world option differs from server config: " +
                        configured_option.first;
                return false;
            }
            option->second.setValue( configured_option.second );
        }
    }
    if( created && !world->save() ) {
        error = "unable to save configured multiplayer world metadata";
        return false;
    }

    world_generator->set_active_world( world );
    if( !g->start_dedicated_world() ) {
        error = world->world_saves.size() > 1 ?
                "existing world has multiple legacy avatar saves; dedicated import is ambiguous" :
                "unable to initialize or load the multiplayer world avatar";
        return false;
    }
    error.clear();
    return true;
}

int run_dedicated_server( const multiplayer_server_config &config,
                          const std::filesystem::path &config_path )
{
    dedicated_server_shutdown_requested = 0;
    std::signal( SIGINT, dedicated_server_signal_handler );
    std::signal( SIGTERM, dedicated_server_signal_handler );

    if( !g->active_avatar().getID().is_valid() ||
        !g->active_player_runtime().player_id().is_valid() ) {
        std::cerr << multiplayer_server_log_json(
        multiplayer_server_log_severity::error, "startup_failed", {
            { "message", "dedicated avatar has no stable server identity" },
            { "world_id", config.world.name }
        } ) << '\n';
        return 1;
    }
    multiplayer_server_player_identity identity;
    identity.player_id = g->active_player_runtime().player_id().str();
    identity.character_id = std::to_string( g->active_avatar().getID().get_value() );
    multiplayer_dedicated_server server( config, config_path, getVersionString(), {}, identity );
    std::string error;
    if( !server.start( error ) ) {
        std::cerr << multiplayer_server_log_json(
                      multiplayer_server_log_severity::error, "startup_failed",
        { { "message", error }, { "world_id", config.world.name } } ) << '\n';
        return 1;
    }
    std::cout << multiplayer_server_log_json(
    multiplayer_server_log_severity::info, "listening", {
        { "listen", config.network.listen }, { "world_id", config.world.name },
        { "build_id", getVersionString() },
        { "bound_port", std::to_string( server.bound_port() ) }
    } ) << std::endl;
    DebugLog( D_INFO, D_MAIN ) << "Dedicated multiplayer server started on " <<
                               config.network.listen;

    struct active_remote_session {
        multiplayer_connection_id connection = 0;
        multiplayer_session_id session = {};
        std::string player_id;
        std::string character_id;
        std::uint64_t generation = 0;
    } active_session;
    struct cached_remote_command {
        multiplayer_transport_payload payload;
        multiplayer_command_result result;
    };
    std::uint64_t server_revision = 1;
    std::map<std::uint64_t, cached_remote_command> command_cache;
    bool runtime_failed = false;
    bool game_over = false;
    std::uint64_t turns_since_save = 0;

    const auto save_world = [&]( const std::string & reason ) -> bool {
        const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        if( !g->save() )
        {
            error = "authoritative multiplayer world save failed";
            return false;
        }
        const std::int64_t duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started ).count();
        std::cout << multiplayer_server_log_json(
                      multiplayer_server_log_severity::info, "save_completed",
        {   { "reason", reason }, { "world_id", config.world.name },
            { "revision", std::to_string( server_revision ) },
            { "duration_ms", std::to_string( duration_ms ) }
        } ) << '\n';
        turns_since_save = 0;
        return true;
    };

    const auto send_scene = [&]() -> bool {
        if( active_session.connection == 0 )
        {
            return true;
        }
        multiplayer_scene_snapshot snapshot;
        if( !multiplayer_build_visible_scene( *g, active_session.player_id,
                                              active_session.character_id, server_revision, 30, snapshot, error ) )
        {
            return false;
        }
        multiplayer_protocol_envelope envelope;
        envelope.message_type = multiplayer_protocol_message_type::scene_snapshot;
        envelope.session = active_session.session;
        envelope.sequence = server_revision;
        if( !multiplayer_build_scene_snapshot_payload( snapshot, envelope.payload, error ) )
        {
            return false;
        }
        return server.send( active_session.connection, envelope, error );
    };
    const auto send_command_result = [&]( const multiplayer_command_result & result ) -> bool {
        multiplayer_protocol_envelope envelope;
        envelope.message_type = multiplayer_protocol_message_type::command_result;
        envelope.session = active_session.session;
        envelope.sequence = result.client_sequence;
        if( !multiplayer_build_command_result_payload( result, envelope.payload, error ) )
        {
            return false;
        }
        return server.send( active_session.connection, envelope, error );
    };
    const auto log_command_result = [&]( const multiplayer_player_command & command,
                                         const multiplayer_command_result & result,
    const std::int64_t duration_us ) {
        std::cout << multiplayer_server_log_json(
                      result.status == multiplayer_command_status::rejected ?
                      multiplayer_server_log_severity::warning :
                      multiplayer_server_log_severity::info,
        "command_result", {
            { "player_id", active_session.player_id },
            { "client_sequence", std::to_string( command.client_sequence ) },
            { "command_type", std::to_string( static_cast<int>( command.kind ) ) },
            { "status", std::to_string( static_cast<int>( result.status ) ) },
            { "rejection", std::to_string( static_cast<int>( result.rejection ) ) },
            { "revision", std::to_string( result.server_revision ) },
            { "moves_spent", std::to_string( result.moves_spent ) },
            { "duration_us", std::to_string( duration_us ) }
        } ) << '\n';
    };

    while( dedicated_server_shutdown_requested == 0 && !runtime_failed && !game_over ) {
        bool turn_had_action = false;
        const bool stopped = g->do_turn_remote( [&]() -> std::optional<bool> {
            while( dedicated_server_shutdown_requested == 0 )
            {
                if( !server.poll_once( multiplayer_dedicated_server::clock::now(), error ) ) {
                    runtime_failed = true;
                    return std::nullopt;
                }
                while( std::optional<multiplayer_server_lobby_event> event = server.poll_event() ) {
                    multiplayer_server_log_fields fields = {
                        { "player_id", event->player_id },
                        { "character_id", event->character_id },
                        { "session_generation", std::to_string( event->session_generation ) }
                    };
                    if( event->type == multiplayer_server_lobby_event_type::authenticated ||
                        event->type == multiplayer_server_lobby_event_type::resumed ) {
                        if( event->player_id != identity.player_id ||
                            event->character_id != identity.character_id ) {
                            server.disconnect( event->connection,
                                               "authenticated identity is not the server avatar" );
                            continue;
                        }
                        active_session = { event->connection, event->session, event->player_id,
                                           event->character_id, event->session_generation
                                         };
                        if( event->type == multiplayer_server_lobby_event_type::authenticated ) {
                            // A fresh authentication starts a new command sequence epoch.  A
                            // resume keeps the cache so an uncertain last command can be replayed.
                            command_cache.clear();
                        }
                        std::cout << multiplayer_server_log_json(
                                      multiplayer_server_log_severity::info,
                                      event->type == multiplayer_server_lobby_event_type::authenticated ?
                                      "player_authenticated" : "player_resumed", fields ) << '\n';
                        if( !send_scene() ) {
                            runtime_failed = true;
                            return std::nullopt;
                        }
                        continue;
                    }
                    if( event->type == multiplayer_server_lobby_event_type::disconnected ) {
                        std::cout << multiplayer_server_log_json(
                                      multiplayer_server_log_severity::info,
                                      "player_disconnected", fields ) << '\n';
                        if( event->connection == active_session.connection ) {
                            active_session = {};
                        }
                        continue;
                    }
                    if( event->message.message_type ==
                        multiplayer_protocol_message_type::resync_request ) {
                        multiplayer_resync_request request;
                        if( !multiplayer_parse_resync_request_payload(
                                event->message, request, error ) ||
                            request.client_revision > server_revision ) {
                            server.disconnect( event->connection, "invalid resync request" );
                            continue;
                        }
                        std::cout << multiplayer_server_log_json(
                                      multiplayer_server_log_severity::info,
                        "resync_requested", {
                            { "player_id", active_session.player_id },
                            {
                                "client_revision", std::to_string(
                                    request.client_revision )
                            },
                            { "server_revision", std::to_string( server_revision ) }
                        } ) << '\n';
                        if( !send_scene() ) {
                            runtime_failed = true;
                            return std::nullopt;
                        }
                        continue;
                    }
                    if( event->message.message_type !=
                        multiplayer_protocol_message_type::player_command ) {
                        fields.emplace_back( "message_type", std::to_string(
                                                 static_cast<int>( event->message.message_type ) ) );
                        fields.emplace_back( "sequence", std::to_string( event->message.sequence ) );
                        std::cout << multiplayer_server_log_json(
                                      multiplayer_server_log_severity::warning,
                                      "unsupported_application_message", fields ) << '\n';
                        continue;
                    }

                    const std::chrono::steady_clock::time_point command_started =
                        std::chrono::steady_clock::now();
                    multiplayer_player_command command;
                    if( !multiplayer_parse_player_command_payload( event->message, command, error ) ) {
                        server.disconnect( event->connection, "invalid semantic player command" );
                        continue;
                    }
                    const auto duplicate = command_cache.find( command.client_sequence );
                    if( duplicate != command_cache.end() ) {
                        if( duplicate->second.payload != event->message.payload ) {
                            std::cout << multiplayer_server_log_json(
                                          multiplayer_server_log_severity::warning,
                            "command_sequence_conflict", {
                                { "player_id", active_session.player_id },
                                {
                                    "client_sequence", std::to_string(
                                        command.client_sequence )
                                }
                            } ) << '\n';
                            server.disconnect( event->connection,
                                               "client sequence reused for a different command" );
                            continue;
                        }
                        multiplayer_command_result cached = duplicate->second.result;
                        if( cached.status == multiplayer_command_status::accepted ) {
                            cached.status = multiplayer_command_status::duplicate;
                        }
                        const std::int64_t duration_us =
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - command_started ).count();
                        log_command_result( command, cached, duration_us );
                        if( !send_command_result( cached ) || !send_scene() ) {
                            runtime_failed = true;
                            return std::nullopt;
                        }
                        continue;
                    }

                    multiplayer_command_result result;
                    result.client_sequence = command.client_sequence;
                    result.server_revision = server_revision;
                    bool action_taken = false;
                    if( command.base_revision != server_revision ) {
                        result.status = multiplayer_command_status::rejected;
                        result.rejection = multiplayer_protocol_rejection::stale_revision;
                        result.message = "command base revision is stale";
                    } else {
                        const multiplayer_command_execution execution =
                            multiplayer_execute_basic_command( *g, g->active_avatar(), command );
                        result.status = execution.status;
                        result.rejection = execution.rejection;
                        result.moves_spent = execution.moves_spent;
                        result.message = execution.message;
                        action_taken = execution.action_taken;
                        if( action_taken ) {
                            if( server_revision == std::numeric_limits<std::uint64_t>::max() ) {
                                error = "server scene revision exhausted";
                                runtime_failed = true;
                                return std::nullopt;
                            }
                            ++server_revision;
                            result.server_revision = server_revision;
                            turn_had_action = true;
                        }
                    }
                    command_cache.emplace( command.client_sequence,
                                           cached_remote_command{ event->message.payload, result } );
                    while( command_cache.size() > 256 ) {
                        command_cache.erase( command_cache.begin() );
                    }
                    const std::int64_t duration_us =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - command_started ).count();
                    log_command_result( command, result, duration_us );
                    if( !send_command_result( result ) || !send_scene() ) {
                        runtime_failed = true;
                        return std::nullopt;
                    }
                    if( action_taken ) {
                        return true;
                    }
                }
                std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
            }
            return std::nullopt;
        } );

        if( runtime_failed || dedicated_server_shutdown_requested != 0 ) {
            break;
        }
        if( stopped ) {
            // The callback only requests a stop for signal/runtime failure, both handled above.
            // A remaining stop therefore comes from the authoritative avatar reaching game over.
            game_over = true;
            break;
        }
        if( turn_had_action ) {
            ++turns_since_save;
            if( server_revision == std::numeric_limits<std::uint64_t>::max() ) {
                error = "server scene revision exhausted";
                runtime_failed = true;
                break;
            }
            ++server_revision;
            if( !send_scene() ) {
                runtime_failed = true;
                break;
            }
            if( config.save.interval_turns > 0 &&
                turns_since_save >= static_cast<std::uint64_t>( config.save.interval_turns ) &&
                !save_world( "interval" ) ) {
                runtime_failed = true;
                break;
            }
        }
    }
    server.stop();
    if( !runtime_failed && ( game_over || dedicated_server_shutdown_requested != 0 ) &&
        !save_world( game_over ? "game_over" : "shutdown" ) ) {
        runtime_failed = true;
    }
    if( runtime_failed ) {
        std::cerr << multiplayer_server_log_json(
                      multiplayer_server_log_severity::error, "runtime_failed",
        { { "message", error }, { "world_id", config.world.name } } ) << '\n';
        return 1;
    }
    if( game_over ) {
        std::cerr << multiplayer_server_log_json(
                      multiplayer_server_log_severity::error, "game_over",
        { { "world_id", config.world.name } } ) << '\n';
        return 1;
    }
    std::cout << multiplayer_server_log_json(
                  multiplayer_server_log_severity::info, "shutdown",
    { { "reason", "signal" }, { "world_id", config.world.name } } ) << '\n';
    DebugLog( D_INFO, D_MAIN ) << "Dedicated multiplayer server stopped cleanly.";
    return 0;
}

}  // namespace

#if defined(EMSCRIPTEN)
EM_ASYNC_JS( void, mount_idbfs, (), {
    console.log( "Mounting IDBFS for persistence..." );
    FS.mkdir( '/home/web_user/.cataclysm-dda' );
    FS.mount( IDBFS, {}, '/home/web_user/.cataclysm-dda' );
    await new Promise( function( resolve, reject )
    {
        FS.syncfs( true, function( err ) {
            if( err ) {
                reject( err );
            } else {
                console.log( "Successfully mounted IDBFS." );
                resolve();
            }
        } );
    } );

    let fsNeedsSync = false;
    window.setFsNeedsSync = function setFsNeedsSync()
    {
        if( !fsNeedsSync ) {
            requestAnimationFrame( syncFs );
        }
        fsNeedsSync = true;
    };

    function syncFs()
    {
        console.log( "Persisting to IDBFS..." );
        FS.syncfs( false, function( err ) {
            fsNeedsSync = false;
            if( err ) {
                console.error( err );
            } else {
                console.log( "Successfully persisted to IDBFS..." );
            }
        } );
    }
} );
#endif

#if defined(USE_WINMAIN)
int APIENTRY WinMain( _In_ HINSTANCE /* hInstance */, _In_opt_ HINSTANCE /* hPrevInstance */,
                      _In_ LPSTR /* lpCmdLine */, _In_ int /* nCmdShow */ )
{
    int argc = __argc;
    char **argv = __argv;
#elif defined(__ANDROID__)
extern "C" int SDL_main( int argc, char **argv ) {
#else
int main( int argc, const char *argv[] )
{
#endif

    cata::init_allocator();

    ordered_static_globals();
    init_crash_handlers();
    reset_floating_point_mode();
#if defined(FLATBUFFERS_LOCALE_INDEPENDENT) && (FLATBUFFERS_LOCALE_INDEPENDENT > 0)
    flatbuffers::ClassicLocale::Get();
#endif

#if defined(EMSCRIPTEN)
    mount_idbfs();
#endif

    on_out_of_scope json_member_reporting_guard{ [] {
            // Disable reporting unvisited members if stack unwinding leaves main early.
            Json::globally_report_unvisited_members( false );
        } };

#if defined(_WIN32) and defined(TILES)
    const HANDLE std_output { GetStdHandle( STD_OUTPUT_HANDLE ) }, std_error { GetStdHandle( STD_ERROR_HANDLE ) };
    if( std_output != INVALID_HANDLE_VALUE and std_error != INVALID_HANDLE_VALUE ) {
        if( AttachConsole( ATTACH_PARENT_PROCESS ) ) {
            if( std_output == nullptr ) {
                freopen_s( &CONOUT, "CONOUT$", "w", stdout );
            }
            if( std_error == nullptr ) {
                freopen_s( &CONOUT, "CONOUT$", "w", stderr );
            }
        }
    }
#endif
#if defined(__ANDROID__)
    // Start the standard output logging redirector
    start_logger( "cdda" );

    // On Android first launch, we copy all data files from the APK into the app's writeable folder so std::io stuff works.
    // Use the external storage so it's publicly modifiable data (so users can mess with installed data, save games etc.)
    std::string external_storage_path( GetAndroidExternalStoragePath() );

    PATH_INFO::init_base_path( external_storage_path );
#else
    // Set default file paths
#if defined(PREFIX)
    PATH_INFO::init_base_path( std::string( PREFIX ) );
#else
    PATH_INFO::init_base_path( "" );
#endif
#endif

#if defined(__ANDROID__)
    PATH_INFO::init_user_dir( external_storage_path );
#else
#   if defined(USE_HOME_DIR) || defined(USE_XDG_DIR) || defined(EMSCRIPTEN)
    PATH_INFO::init_user_dir( "" );
#   else
    PATH_INFO::init_user_dir( "." );
#   endif
#endif
    PATH_INFO::set_standard_filenames();

    MAP_SHARING::setDefaults();

    cli_opts cli = parse_commandline( argc, const_cast<const char **>( argv ) );

    if( cli.server_config_action != server_config_operation::none ) {
        json_error_output_colors = json_error_output_colors_t::no_colors;
        const std::filesystem::path config_path =
            std::filesystem::u8path( cli.server_config_path );
        if( cli.server_config_action == server_config_operation::initialize ) {
            std::string error;
            if( !initialize_multiplayer_server_files( config_path, error ) ) {
                std::cerr << "Server config initialization failed: " << error << '\n';
                return 1;
            }
            std::cout << "Created multiplayer server config: " << config_path.u8string() << '\n';
            return 0;
        }
        const multiplayer_server_config_result config =
            load_multiplayer_server_config( config_path );
        if( !config ) {
            std::cerr << "Server config validation failed: " << config.error << '\n';
            return 1;
        }
        std::cout << "Multiplayer server config is valid: " << config_path.u8string() << '\n';
        return 0;
    }

    const bool dedicated_server =
        cli.runtime_mode == multiplayer_runtime_mode::dedicated_server;
    set_multiplayer_runtime_mode( cli.runtime_mode );
    std::optional<multiplayer_server_config> dedicated_server_config;
    const std::filesystem::path dedicated_server_config_path =
        std::filesystem::u8path( cli.server_config_path );
    if( dedicated_server ) {
        if( cli.verifyexit || cli.check_mods || !cli.world.empty() ) {
            log_dedicated_server_startup_error(
                "dedicated server mode cannot be combined with verification, mod checks, or --world" );
            return 1;
        }
        json_error_output_colors = json_error_output_colors_t::no_colors;
        multiplayer_server_config_result loaded =
            load_multiplayer_server_config( dedicated_server_config_path );
        if( !loaded ) {
            log_dedicated_server_startup_error( "server config rejected: " + loaded.error );
            return 1;
        }
        dedicated_server_config = std::move( *loaded.config );
        std::string directory_error;
        if( !assure_dedicated_server_dirs_exist( directory_error ) ) {
            log_dedicated_server_startup_error( directory_error,
                                                dedicated_server_config->world.name );
            return 1;
        }
    }

    if( !dir_exist( PATH_INFO::datadir() ) ) {
        if( dedicated_server ) {
            log_dedicated_server_startup_error(
                "gameplay data directory is unavailable: " + PATH_INFO::datadir(),
                dedicated_server_config->world.name );
            return 1;
        }
        printf( "Fatal: Can't find data directory \"%s\"\nPlease ensure the current working directory is correct or specify data directory with --datadir.  Perhaps you meant to start \"cataclysm-launcher\"?\n",
                PATH_INFO::datadir().c_str() );
        exit( 1 );
    }

    if( !assure_dir_exist( PATH_INFO::user_dir() ) ) {
        if( dedicated_server ) {
            log_dedicated_server_startup_error(
                "user directory is unavailable: " + PATH_INFO::user_dir(),
                dedicated_server_config->world.name );
            return 1;
        }
        printf( "Can't open or create %s. Check permissions.\n",
                PATH_INFO::user_dir().c_str() );
        exit( 1 );
    }

#if defined(EMSCRIPTEN)
    setupDebug( DebugOutput::std_err );
#else
    setupDebug( DebugOutput::file );
#endif
    set_debugmsg_prompt_suppression( dedicated_server );
    loading_ui::set_suppressed( dedicated_server );
    set_popup_suppression( dedicated_server );
    // NOLINTNEXTLINE(cata-tests-must-restore-global-state)
    json_error_output_colors = dedicated_server ? json_error_output_colors_t::no_colors :
                               json_error_output_colors_t::color_tags;

    /**
     * OS X does not populate locale env vars correctly (they usually default to
     * "C") so don't bother trying to set the locale based on them.
     */
#if !defined(MACOSX)
    if( setlocale( LC_ALL, "" ) == nullptr ) {
        DebugLog( D_WARNING, D_MAIN ) << "Error while setlocale(LC_ALL, '').";
    } else {
#endif
        try {
            std::locale::global( std::locale( "" ) );
        } catch( const std::exception & ) {
            // if user default locale retrieval isn't implemented by system
            try {
                // default to basic C locale
                std::locale::global( std::locale::classic() );
            } catch( const std::exception &err ) {
                if( dedicated_server ) {
                    log_dedicated_server_startup_error(
                        "unable to initialize locale: " + std::string( err.what() ),
                        dedicated_server_config->world.name );
                    deinitDebug();
                    return 1;
                } else {
                    debugmsg( "%s", err.what() );
                    exit_handler( -999 );
                }
            }
        }
#if !defined(MACOSX)
    }
#endif

    DebugLog( D_INFO, DC_ALL ) << "[main] C locale set to " << setlocale( LC_ALL, nullptr );
    DebugLog( D_INFO, DC_ALL ) << "[main] C++ locale set to " << std::locale().name();

#if defined(TILES) || defined(SDL_SOUND)
    if( !dedicated_server ) {
        const SDLVersionInfo compiled = GetCompiledSDLVersion();
        DebugLog( D_INFO, DC_ALL ) << "SDL version used during compile is "
                                   << compiled.major << "."
                                   << compiled.minor << "."
                                   << compiled.patch;

        const SDLVersionInfo linked = GetLinkedSDLVersion();
        DebugLog( D_INFO, DC_ALL ) << "SDL version used during linking and in runtime is "
                                   << linked.major << "."
                                   << linked.minor << "."
                                   << linked.patch;
    }
#endif

#if !defined(TILES)
    get_options().init();
    get_options().load();
#else
    if( dedicated_server ) {
        get_options().init();
        get_options().load();
    }
#endif

    if( dedicated_server ) {
        init_colors();
    }

    // in test mode don't initialize curses to avoid escape sequences being inserted into output stream
    if( !test_mode && !dedicated_server ) {
        try {
            // set minimum FULL_SCREEN sizes
            FULL_SCREEN_WIDTH = EVEN_MINIMUM_TERM_WIDTH;
            FULL_SCREEN_HEIGHT = EVEN_MINIMUM_TERM_HEIGHT;
            catacurses::init_interface();
        } catch( const std::exception &err ) {
            // can't use any curses function as it has not been initialized
            std::cerr << "Error while initializing the interface: " << err.what() << std::endl;
            DebugLog( D_ERROR, DC_ALL ) << "Error while initializing the interface: " << err.what() << "\n";
            return 1;
        }
    } else if( cli.check_mods ) {
        get_options().init();
        get_options().load();
    }

    set_language_from_options();

    const int engine_seed = dedicated_server ? djb2_hash(
                                reinterpret_cast<const unsigned char *>(
                                    dedicated_server_config->world.seed.c_str() ) ) : cli.seed;
    rng_set_engine_seed( engine_seed );

    if( !dedicated_server ) {
        game_ui::init_ui();
    }

    g = std::make_unique<game>();

    // First load and initialize everything that does not
    // depend on the mods.
    try {
        g->load_static_data();
        if( cli.verifyexit ) {
            exit_handler( 0 );
        }
        if( cli.check_mods ) {
            init_colors();
            const std::vector<mod_id> mods( cli.opts.begin(), cli.opts.end() );
            exit( g->check_mod_data( mods ) && !debug_has_error_been_observed() ? 0 : 1 );
        }
    } catch( const std::exception &err ) {
        if( dedicated_server ) {
            log_dedicated_server_startup_error(
                "gameplay data loading failed: " + std::string( err.what() ),
                dedicated_server_config->world.name );
            g.reset();
            deinitDebug();
            return 1;
        } else {
            debugmsg( "%s", err.what() );
            exit_handler( -999 );
        }
    }

    if( dedicated_server ) {
        std::string world_error;
        if( !prepare_dedicated_server_world( *dedicated_server_config, world_error ) ) {
            log_dedicated_server_startup_error(
                "world loading failed: " + world_error,
                dedicated_server_config->world.name );
            g.reset();
            deinitDebug();
            return 1;
        }
        const int result = run_dedicated_server( *dedicated_server_config,
                           dedicated_server_config_path );
        g.reset();
        deinitDebug();
        return result;
    }

    // Load the colors of ImGui to match the colors set by the user.
    cataimgui::init_colors();

    // set decimal point for float input widgets
    // uses system locale, because that's what imgui uses to parse and display floats
    ImGui::GetPlatformIO().Platform_LocaleDecimalPoint =
        static_cast<unsigned char>( *localeconv()->decimal_point );

    // Override existing settings from cli  options
    if( cli.disable_ascii_art ) {
        get_options().get_option( "ENABLE_ASCII_ART" ).setValue( "false" );
        get_options().get_option( "ENABLE_ASCII_TITLE" ).setValue( "false" );
    }

    if( cli.noverify ) {
        get_options().get_option( "SKIP_VERIFICATION" ).setValue( "true" );
    }

    // Now we do the actual game.

#if defined(DEBUG_CURSES_CURSOR)
    catacurses::curs_set( 2 );
#else
    // I have no clue what this comment is on about
    // Any value works well enough for debugging at least
    catacurses::curs_set( 0 ); // Invisible cursor here, because MAPBUFFER.load() is crash-prone
#endif

#if !defined(_WIN32)
    struct sigaction sigIntHandler;
    sigIntHandler.sa_handler = exit_handler;
    sigemptyset( &sigIntHandler.sa_mask );
    sigIntHandler.sa_flags = 0;
    sigaction( SIGINT, &sigIntHandler, nullptr );
#endif

    if( !assure_essential_dirs_exist() ) {
        exit_handler( -999 );
        return 0;
    }

#if defined(LOCALIZE)
    if( get_option<std::string>( "USE_LANG" ).empty() && !SystemLocale::Language().has_value() ) {
#if defined(TILES)
        display_buffer_draw_scope draw_scope;
        if( !display_buffer_scope_is_invalid() ) {
#endif
            imclient->new_frame(); // we have to prime the pump, because of reasons
            imclient->end_frame();
            const std::string lang = select_language();
            get_options().get_option( "USE_LANG" ).setValue( lang );
            set_language_from_options();
#if defined(TILES)
        }
#endif
    }
#endif
    replay_buffered_debugmsg_prompts();

    main_menu::queued_world_to_load = std::move( cli.world );

    while( true ) {
        main_menu menu;
        if( !menu.opening_screen() ) {
            break;
        }

        shared_ptr_fast<ui_adaptor> ui = g->create_or_get_main_ui_adaptor();
        get_event_bus().send<event_type::game_begin>( getVersionString() );
        while( !g->do_turn() ) {}
    }

    exit_handler( -999 );
    return 0;
}

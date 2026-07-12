#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "calendar.h"
#include "coords_fwd.h"
#include "json.h"
#include "messages.h"

class Creature;
class JsonObject;
class JsonOut;
namespace debugmode
{
enum debug_filter : int;
}  // namespace debugmode
struct game_message_params;

namespace catacurses
{
class window;
}  // namespace catacurses

/**
 * For unit testing we just store all messages so they can be dumped in the
 * event of a test failure.
 */

class Messages::message_log
{
    public:
        std::vector<std::pair<std::string, std::string>> messages;
};

static shared_ptr_fast<Messages::message_log> active_messages =
    make_shared_fast<Messages::message_log>();

shared_ptr_fast<Messages::message_log> Messages::make_message_log()
{
    return make_shared_fast<message_log>();
}

shared_ptr_fast<Messages::message_log> Messages::current_message_log()
{
    return active_messages;
}

void Messages::set_current_message_log( const shared_ptr_fast<message_log> &messages )
{
    if( messages != nullptr ) {
        active_messages = messages;
    }
}

std::vector<std::pair<std::string, std::string>> Messages::recent_messages( size_t )
{
    return active_messages->messages;
}
std::vector<std::pair<std::string, std::string>> Messages::recent_messages(
            const shared_ptr_fast<message_log> &messages, size_t )
{
    return messages->messages;
}
bool Messages::has_debug_filter( debugmode::debug_filter )
{
    return true;
}
void Messages::add_msg( std::string m )
{
    if( !m.empty() ) {
        active_messages->messages.emplace_back( to_string_time_of_day( calendar::turn ),
                                                std::move( m ) );
    }
}
void Messages::add_msg( const game_message_params &, std::string m )
{
    add_msg( std::move( m ) );
}
void Messages::clear_messages()
{
    active_messages->messages.clear();
}
void Messages::deactivate() {}
size_t Messages::size()
{
    return active_messages->messages.size();
}
bool Messages::has_undisplayed_messages()
{
    return false;
}
void Messages::display_messages() {}
void Messages::display_messages( const catacurses::window &, int, int, int, int ) {}
void Messages::serialize( JsonOut &json )
{
    serialize( active_messages, json );
}
void Messages::serialize( const shared_ptr_fast<message_log> &messages, JsonOut &json )
{
    json.member( "player_messages" );
    json.start_object();
    json.member( "messages", messages->messages );
    json.end_object();
}
void Messages::deserialize( const JsonObject &json )
{
    deserialize( active_messages, json );
}
void Messages::deserialize( const shared_ptr_fast<message_log> &messages, const JsonObject &json )
{
    if( json.has_object( "player_messages" ) ) {
        json.get_object( "player_messages" ).read( "messages", messages->messages );
    }
}
void add_msg( std::string m )
{
    Messages::add_msg( std::move( m ) );
}
void add_msg( const game_message_params &, std::string m )
{
    Messages::add_msg( std::move( m ) );
}
void add_msg_if_player_sees( const tripoint_bub_ms &, std::string m )
{
    Messages::add_msg( std::move( m ) );
}
void add_msg_if_player_sees( const Creature &, std::string m )
{
    Messages::add_msg( std::move( m ) );
}
void add_msg_if_player_sees( const tripoint_bub_ms &, const game_message_params &, std::string m )
{
    Messages::add_msg( std::move( m ) );
}
void add_msg_if_player_sees( const Creature &, const game_message_params &, std::string m )
{
    Messages::add_msg( std::move( m ) );
}

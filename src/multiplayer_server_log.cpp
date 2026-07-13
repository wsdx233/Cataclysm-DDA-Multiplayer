#include "multiplayer_server_log.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace
{

std::string json_escape( const std::string &value )
{
    std::ostringstream output;
    output.imbue( std::locale::classic() );
    output << std::hex << std::setfill( '0' );
    for( const unsigned char ch : value ) {
        switch( ch ) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if( ch < 0x20 ) {
                    output << "\\u" << std::setw( 4 ) << static_cast<unsigned>( ch );
                } else {
                    output << static_cast<char>( ch );
                }
                break;
        }
    }
    return output.str();
}

const char *severity_name( const multiplayer_server_log_severity severity )
{
    switch( severity ) {
        case multiplayer_server_log_severity::debug:
            return "debug";
        case multiplayer_server_log_severity::info:
            return "info";
        case multiplayer_server_log_severity::warning:
            return "warning";
        case multiplayer_server_log_severity::error:
            return "error";
    }
    return "error";
}

std::string utc_timestamp( const std::chrono::system_clock::time_point timestamp )
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t( timestamp );
    std::tm utc = {};
#if defined(_WIN32)
    gmtime_s( &utc, &seconds );
#else
    gmtime_r( &seconds, &utc );
#endif
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  timestamp.time_since_epoch() ).count();
    const int fraction = static_cast<int>( ( milliseconds % 1000 + 1000 ) % 1000 );
    std::ostringstream output;
    output.imbue( std::locale::classic() );
    output << std::put_time( &utc, "%Y-%m-%dT%H:%M:%S" ) << '.' <<
           std::setfill( '0' ) << std::setw( 3 ) << fraction << 'Z';
    return output.str();
}

} // namespace

std::string multiplayer_server_log_json(
    const multiplayer_server_log_severity severity,
    const std::string &event,
    const multiplayer_server_log_fields &fields,
    const std::chrono::system_clock::time_point timestamp )
{
    std::ostringstream output;
    output.imbue( std::locale::classic() );
    output << "{\"timestamp\":\"" << utc_timestamp( timestamp ) <<
           "\",\"severity\":\"" << severity_name( severity ) <<
           "\",\"category\":\"multiplayer_server\",\"event\":\"" <<
           json_escape( event ) << '"';
    for( const auto &field : fields ) {
        output << ",\"" << json_escape( field.first ) << "\":\"" <<
               json_escape( field.second ) << '"';
    }
    output << '}';
    return output.str();
}

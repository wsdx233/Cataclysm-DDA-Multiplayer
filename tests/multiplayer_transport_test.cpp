#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif

#define ASIO_NO_DEPRECATED
#define ASIO_STANDALONE
#include <asio.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cata_catch.h"
#include "multiplayer_transport.h"

namespace
{

multiplayer_transport_payload payload( const std::string &text )
{
    return { text.begin(), text.end() };
}

std::string payload_text( const multiplayer_transport_payload &value )
{
    return { value.begin(), value.end() };
}

template<typename Transport>
std::optional<multiplayer_transport_event> wait_for_event(
    Transport &transport, const std::chrono::milliseconds timeout = std::chrono::seconds( 5 ) )
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if( std::optional<multiplayer_transport_event> event = transport.poll_event() ) {
            return event;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    } while( std::chrono::steady_clock::now() < deadline );
    return std::nullopt;
}

multiplayer_transport_payload read_frame( asio::ip::tcp::socket &socket )
{
    std::array<std::uint8_t, multiplayer_transport_frame_header_size> header = {};
    asio::error_code error;
    const std::size_t header_size = asio::read( socket, asio::buffer( header ), error );
    REQUIRE_FALSE( error );
    REQUIRE( header_size == header.size() );
    const std::uint32_t length =
        static_cast<std::uint32_t>( header[0] ) << 24U |
        static_cast<std::uint32_t>( header[1] ) << 16U |
        static_cast<std::uint32_t>( header[2] ) << 8U |
        static_cast<std::uint32_t>( header[3] );
    REQUIRE( length <= multiplayer_transport_maximum_frame_size );
    multiplayer_transport_payload result( length );
    if( length != 0 ) {
        const std::size_t payload_size = asio::read( socket, asio::buffer( result ), error );
        REQUIRE_FALSE( error );
        REQUIRE( payload_size == result.size() );
    }
    return result;
}

struct loopback_client {
    asio::io_context io;
    asio::ip::tcp::socket socket{ io };
};

void connect_client( loopback_client &client, const multiplayer_server_transport &server )
{
    client.socket.connect( asio::ip::tcp::endpoint( asio::ip::address_v4::loopback(),
                           server.bound_port() ) );
}

bool wait_for_socket_close( asio::ip::tcp::socket &socket,
                            const std::chrono::milliseconds timeout = std::chrono::seconds( 2 ) )
{
    asio::error_code error;
    socket.non_blocking( true, error );
    if( error ) {
        return false;
    }
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
    std::array<std::uint8_t, 1> buffer = {};
    do {
        error.clear();
        socket.read_some( asio::buffer( buffer ), error );
        if( error && error != asio::error::would_block && error != asio::error::try_again ) {
            return true;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    } while( std::chrono::steady_clock::now() < deadline );
    return false;
}

} // namespace

TEST_CASE( "multiplayer_transport_frame_codec_handles_stream_boundaries",
           "[multiplayer][transport]" )
{
    const multiplayer_transport_payload first_payload = payload( "fragmented" );
    multiplayer_transport_payload first_frame;
    std::string error;
    REQUIRE( multiplayer_encode_transport_frame( first_payload, first_frame, error ) );

    multiplayer_transport_frame_decoder fragmented_decoder;
    std::vector<multiplayer_transport_payload> decoded;
    for( const std::uint8_t byte : first_frame ) {
        REQUIRE( fragmented_decoder.push( &byte, 1, decoded, error ) );
    }
    REQUIRE( decoded == std::vector<multiplayer_transport_payload> { first_payload } );
    CHECK( fragmented_decoder.empty() );

    const multiplayer_transport_payload maximum_payload(
        multiplayer_transport_maximum_frame_size, static_cast<std::uint8_t>( 'x' ) );
    multiplayer_transport_payload maximum_frame;
    REQUIRE( multiplayer_encode_transport_frame( maximum_payload, maximum_frame, error ) );
    multiplayer_transport_payload following_frame;
    REQUIRE( multiplayer_encode_transport_frame( payload( "after-maximum" ), following_frame, error ) );

    multiplayer_transport_frame_decoder boundary_decoder;
    decoded.clear();
    REQUIRE( boundary_decoder.push( maximum_frame.data(), maximum_frame.size() - 3, decoded, error ) );
    CHECK( decoded.empty() );
    multiplayer_transport_payload final_chunk( maximum_frame.end() - 3, maximum_frame.end() );
    final_chunk.insert( final_chunk.end(), following_frame.begin(), following_frame.end() );
    REQUIRE( boundary_decoder.push( final_chunk.data(), final_chunk.size(), decoded, error ) );
    REQUIRE( decoded.size() == 2 );
    CHECK( decoded[0] == maximum_payload );
    CHECK( payload_text( decoded[1] ) == "after-maximum" );

    const std::array<std::uint8_t, multiplayer_transport_frame_header_size> oversized = {
        0x00, 0x10, 0x00, 0x01
    };
    multiplayer_transport_frame_decoder oversized_decoder;
    decoded.clear();
    CHECK_FALSE( oversized_decoder.push( oversized.data(), oversized.size(), decoded, error ) );
    CHECK( error.find( "1 MiB" ) != std::string::npos );
}

TEST_CASE( "multiplayer_server_transport_preserves_frames_and_ordered_half_close",
           "[multiplayer][transport]" )
{
    multiplayer_server_transport server;
    std::string error;
    REQUIRE( server.start( { "127.0.0.1", 0 }, error ) );
    REQUIRE( server.running() );
    REQUIRE( server.bound_port() != 0 );

    loopback_client client;
    connect_client( client, server );
    std::optional<multiplayer_transport_event> event = wait_for_event( server );
    REQUIRE( event );
    REQUIRE( event->type == multiplayer_transport_event_type::connected );
    const multiplayer_connection_id connection = event->connection;

    multiplayer_transport_payload fragmented;
    REQUIRE( multiplayer_encode_transport_frame( payload( "alpha" ), fragmented, error ) );
    for( const std::uint8_t byte : fragmented ) {
        asio::write( client.socket, asio::buffer( &byte, 1 ) );
    }
    multiplayer_transport_payload coalesced;
    multiplayer_transport_payload encoded;
    REQUIRE( multiplayer_encode_transport_frame( payload( "beta" ), coalesced, error ) );
    REQUIRE( multiplayer_encode_transport_frame( payload( "gamma" ), encoded, error ) );
    coalesced.insert( coalesced.end(), encoded.begin(), encoded.end() );
    asio::write( client.socket, asio::buffer( coalesced ) );
    client.socket.shutdown( asio::ip::tcp::socket::shutdown_send );

    for( const char *expected : {
             "alpha", "beta", "gamma"
         } ) {
        event = wait_for_event( server );
        REQUIRE( event );
        REQUIRE( event->type == multiplayer_transport_event_type::frame );
        CHECK( event->connection == connection );
        CHECK( payload_text( event->payload ) == expected );
    }
    event = wait_for_event( server );
    REQUIRE( event );
    CHECK( event->type == multiplayer_transport_event_type::peer_half_closed );
    CHECK( event->connection == connection );

    CHECK( server.send( connection, payload( "ack" ) ) ==
           multiplayer_transport_send_result::queued );
    REQUIRE( server.disconnect( connection, "test complete" ) );
    CHECK( payload_text( read_frame( client.socket ) ) == "ack" );

    std::array<std::uint8_t, 1> end = {};
    asio::error_code read_error;
    client.socket.read_some( asio::buffer( end ), read_error );
    CHECK( read_error == asio::error::eof );
    event = wait_for_event( server );
    REQUIRE( event );
    CHECK( event->type == multiplayer_transport_event_type::disconnected );
    CHECK( event->detail == "test complete" );

    server.stop();
    CHECK_FALSE( server.running() );
}

TEST_CASE( "multiplayer_server_transport_atomically_sends_before_ordered_disconnect",
           "[multiplayer][transport]" )
{
    SECTION( "successful write reports the requested graceful close reason" ) {
        multiplayer_server_transport server;
        std::string error;
        REQUIRE( server.start( { "127.0.0.1", 0 }, error ) );
        loopback_client client;
        connect_client( client, server );
        std::optional<multiplayer_transport_event> event = wait_for_event( server );
        REQUIRE( event );
        REQUIRE( event->type == multiplayer_transport_event_type::connected );

        const std::string reason = "atomic ordered close complete";
        CHECK( server.send_and_disconnect( event->connection, payload( "final-ack" ), reason ) ==
               multiplayer_transport_send_result::queued );
        CHECK( payload_text( read_frame( client.socket ) ) == "final-ack" );
        std::array<std::uint8_t, 1> end = {};
        asio::error_code read_error;
        client.socket.read_some( asio::buffer( end ), read_error );
        CHECK( read_error == asio::error::eof );

        event = wait_for_event( server );
        REQUIRE( event );
        CHECK( event->type == multiplayer_transport_event_type::disconnected );
        CHECK( event->detail == reason );
        server.stop();
    }

    SECTION( "per-connection enqueue failure reports a non-graceful transport error" ) {
        multiplayer_server_transport_settings settings;
        settings.pending_write_bytes_per_connection = multiplayer_transport_frame_header_size;
        multiplayer_server_transport server( settings );
        std::string error;
        REQUIRE( server.start( { "127.0.0.1", 0 }, error ) );
        loopback_client client;
        connect_client( client, server );
        std::optional<multiplayer_transport_event> event = wait_for_event( server );
        REQUIRE( event );
        REQUIRE( event->type == multiplayer_transport_event_type::connected );

        CHECK( server.send_and_disconnect( event->connection, payload( "cannot-fit" ),
                                           "must-not-be-reported" ) ==
               multiplayer_transport_send_result::queued );
        event = wait_for_event( server );
        REQUIRE( event );
        CHECK( event->type == multiplayer_transport_event_type::transport_error );
        CHECK( event->detail.find( "queue is full" ) != std::string::npos );
        CHECK( event->detail != "must-not-be-reported" );
        server.stop();
    }
}

TEST_CASE( "multiplayer_server_transport_retires_capacity_until_terminal_event_is_consumed",
           "[multiplayer][transport]" )
{
    multiplayer_server_transport_settings settings;
    settings.maximum_connections = 1;
    settings.inbound_event_count = 2;
    multiplayer_server_transport server( settings );
    std::string error;
    REQUIRE( server.start( { "127.0.0.1", 0 }, error ) );

    loopback_client first;
    connect_client( first, server );
    std::optional<multiplayer_transport_event> event = wait_for_event( server );
    REQUIRE( event );
    REQUIRE( event->type == multiplayer_transport_event_type::connected );
    const multiplayer_connection_id first_connection = event->connection;

    REQUIRE( server.disconnect( first_connection, "retired connection" ) );
    REQUIRE( wait_for_socket_close( first.socket ) );

    // The socket is gone, but its logical admission slot remains occupied until the
    // simulation thread consumes the terminal event.  A connection churner therefore
    // cannot reuse that slot to crowd the terminal event out of the bounded queue.
    loopback_client rejected;
    connect_client( rejected, server );
    REQUIRE( wait_for_socket_close( rejected.socket ) );

    event = wait_for_event( server );
    REQUIRE( event );
    CHECK( event->type == multiplayer_transport_event_type::disconnected );
    CHECK( event->connection == first_connection );
    CHECK( event->detail == "retired connection" );
    CHECK( server.running() );

    loopback_client replacement;
    connect_client( replacement, server );
    event = wait_for_event( server );
    REQUIRE( event );
    CHECK( event->type == multiplayer_transport_event_type::connected );
    server.stop();
}

TEST_CASE( "multiplayer_client_transport_connects_and_preserves_bidirectional_frames",
           "[multiplayer][transport]" )
{
    multiplayer_server_transport server;
    std::string error;
    REQUIRE( server.start( { "127.0.0.1", 0 }, error ) );

    multiplayer_client_transport client;
    REQUIRE( client.start( { "127.0.0.1", server.bound_port() }, error ) );
    std::optional<multiplayer_transport_event> client_event = wait_for_event( client );
    REQUIRE( client_event );
    REQUIRE( client_event->type == multiplayer_transport_event_type::connected );
    CHECK( client.connected() );

    std::optional<multiplayer_transport_event> server_event = wait_for_event( server );
    REQUIRE( server_event );
    REQUIRE( server_event->type == multiplayer_transport_event_type::connected );
    const multiplayer_connection_id connection = server_event->connection;

    CHECK( client.send( payload( "client-to-server" ) ) ==
           multiplayer_transport_send_result::queued );
    server_event = wait_for_event( server );
    REQUIRE( server_event );
    REQUIRE( server_event->type == multiplayer_transport_event_type::frame );
    CHECK( payload_text( server_event->payload ) == "client-to-server" );

    CHECK( server.send( connection, payload( "server-to-client" ) ) ==
           multiplayer_transport_send_result::queued );
    client_event = wait_for_event( client );
    REQUIRE( client_event );
    REQUIRE( client_event->type == multiplayer_transport_event_type::frame );
    CHECK( payload_text( client_event->payload ) == "server-to-client" );

    REQUIRE( server.disconnect( connection, "client transport test complete" ) );
    client_event = wait_for_event( client );
    REQUIRE( client_event );
    CHECK( ( client_event->type == multiplayer_transport_event_type::peer_half_closed ||
             client_event->type == multiplayer_transport_event_type::disconnected ) );

    client.stop();
    server.stop();
    CHECK_FALSE( client.running() );
    CHECK_FALSE( client.connected() );
}

TEST_CASE( "multiplayer_client_transport_reserves_a_terminal_event_under_saturation",
           "[multiplayer][transport]" )
{
    multiplayer_server_transport server;
    std::string error;
    REQUIRE( server.start( { "127.0.0.1", 0 }, error ) );

    multiplayer_client_transport_settings settings;
    settings.inbound_event_count = 3;
    multiplayer_client_transport client( settings );
    REQUIRE( client.start( { "127.0.0.1", server.bound_port() }, error ) );
    std::optional<multiplayer_transport_event> client_event = wait_for_event( client );
    REQUIRE( client_event );
    REQUIRE( client_event->type == multiplayer_transport_event_type::connected );
    std::optional<multiplayer_transport_event> server_event = wait_for_event( server );
    REQUIRE( server_event );
    REQUIRE( server_event->type == multiplayer_transport_event_type::connected );

    for( const char value : {
             'a', 'b', 'c'
         } ) {
        REQUIRE( server.send( server_event->connection,
                              multiplayer_transport_payload{ static_cast<std::uint8_t>( value ) } ) ==
                 multiplayer_transport_send_result::queued );
    }
    std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
    for( const char expected : {
             'a', 'b'
         } ) {
        client_event = wait_for_event( client );
        REQUIRE( client_event );
        REQUIRE( client_event->type == multiplayer_transport_event_type::frame );
        REQUIRE( client_event->payload.size() == 1 );
        CHECK( client_event->payload.front() == static_cast<std::uint8_t>( expected ) );
    }
    client_event = wait_for_event( client );
    REQUIRE( client_event );
    CHECK( client_event->type == multiplayer_transport_event_type::transport_error );
    CHECK( client_event->detail.find( "queue is full" ) != std::string::npos );
    client.stop();
    server.stop();
}

TEST_CASE( "multiplayer_server_transport_rejects_oversized_and_saturated_input",
           "[multiplayer][transport]" )
{
    SECTION( "oversized frame declaration" ) {
        multiplayer_server_transport server;
        std::string error;
        REQUIRE( server.start( { "127.0.0.1", 0 }, error ) );
        loopback_client client;
        connect_client( client, server );
        std::optional<multiplayer_transport_event> event = wait_for_event( server );
        REQUIRE( event );
        REQUIRE( event->type == multiplayer_transport_event_type::connected );

        const std::array<std::uint8_t, multiplayer_transport_frame_header_size> oversized = {
            0x00, 0x10, 0x00, 0x01
        };
        asio::write( client.socket, asio::buffer( oversized ) );
        event = wait_for_event( server );
        REQUIRE( event );
        CHECK( event->type == multiplayer_transport_event_type::protocol_error );
        CHECK( event->detail.find( "1 MiB" ) != std::string::npos );
        server.stop();
    }

    SECTION( "bounded inbound queue" ) {
        multiplayer_server_transport_settings settings;
        settings.maximum_connections = 1;
        settings.inbound_event_count = 3;
        multiplayer_server_transport server( settings );
        std::string error;
        REQUIRE( server.start( { "127.0.0.1", 0 }, error ) );
        loopback_client client;
        connect_client( client, server );
        std::optional<multiplayer_transport_event> event = wait_for_event( server );
        REQUIRE( event );
        REQUIRE( event->type == multiplayer_transport_event_type::connected );

        multiplayer_transport_payload empty_frame;
        REQUIRE( multiplayer_encode_transport_frame( {}, empty_frame, error ) );
        multiplayer_transport_payload two_empty_frames = empty_frame;
        two_empty_frames.insert( two_empty_frames.end(), empty_frame.begin(), empty_frame.end() );
        asio::write( client.socket, asio::buffer( two_empty_frames ) );
        std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );

        event = wait_for_event( server );
        REQUIRE( event );
        CHECK( event->type == multiplayer_transport_event_type::frame );
        event = wait_for_event( server );
        REQUIRE( event );
        CHECK( event->type == multiplayer_transport_event_type::transport_error );
        CHECK( event->detail.find( "queue is full" ) != std::string::npos );
        server.stop();
    }

    SECTION( "outbound frame limit and accept cancellation" ) {
        multiplayer_server_transport server;
        CHECK( server.send( 1, {} ) == multiplayer_transport_send_result::stopped );
        std::string error;
        REQUIRE( server.start( { "127.0.0.1", 0 }, error ) );
        CHECK( server.send( 1, multiplayer_transport_payload(
                                multiplayer_transport_maximum_frame_size + 1 ) ) ==
               multiplayer_transport_send_result::frame_too_large );
        server.stop();
        CHECK_FALSE( server.running() );
    }
}

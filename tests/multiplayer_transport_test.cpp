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

std::optional<multiplayer_transport_event> wait_for_event(
    multiplayer_server_transport &server, const std::chrono::milliseconds timeout =
        std::chrono::seconds( 5 ) )
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if( std::optional<multiplayer_transport_event> event = server.poll_event() ) {
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

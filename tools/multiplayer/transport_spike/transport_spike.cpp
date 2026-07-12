#include <asio.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

constexpr std::size_t frame_header_size = 4;
constexpr std::size_t maximum_frame_size = 1024 * 1024;
constexpr std::size_t network_read_size = 7;

void require( const bool condition, const char *message )
{
    if( !condition ) {
        throw std::runtime_error( message );
    }
}

std::vector<std::uint8_t> encode_frame( const std::string &payload )
{
    if( payload.size() > maximum_frame_size ||
        payload.size() > std::numeric_limits<std::uint32_t>::max() ) {
        throw std::length_error( "frame payload exceeds the spike limit" );
    }

    const std::uint32_t length = static_cast<std::uint32_t>( payload.size() );
    std::vector<std::uint8_t> result;
    result.reserve( frame_header_size + payload.size() );
    result.push_back( static_cast<std::uint8_t>( length >> 24U ) );
    result.push_back( static_cast<std::uint8_t>( length >> 16U ) );
    result.push_back( static_cast<std::uint8_t>( length >> 8U ) );
    result.push_back( static_cast<std::uint8_t>( length ) );
    result.insert( result.end(), payload.begin(), payload.end() );
    return result;
}

class frame_decoder
{
    public:
        std::vector<std::string> push( const std::uint8_t *data, const std::size_t size ) {
            std::vector<std::string> decoded;
            std::size_t consumed = 0;
            while( consumed < size ) {
                if( pending.size() < frame_header_size ) {
                    const std::size_t header_bytes = std::min(
                                                         frame_header_size - pending.size(), size - consumed );
                    pending.insert( pending.end(), data + consumed, data + consumed + header_bytes );
                    consumed += header_bytes;
                    if( pending.size() < frame_header_size ) {
                        continue;
                    }
                }

                const std::uint32_t length =
                    static_cast<std::uint32_t>( pending[0] ) << 24U |
                    static_cast<std::uint32_t>( pending[1] ) << 16U |
                    static_cast<std::uint32_t>( pending[2] ) << 8U |
                    static_cast<std::uint32_t>( pending[3] );
                if( length > maximum_frame_size ) {
                    throw std::length_error( "declared frame length exceeds the spike limit" );
                }
                const std::size_t encoded_size = frame_header_size + length;
                const std::size_t payload_bytes = std::min(
                                                      encoded_size - pending.size(), size - consumed );
                pending.insert( pending.end(), data + consumed, data + consumed + payload_bytes );
                consumed += payload_bytes;
                if( pending.size() == encoded_size ) {
                    decoded.emplace_back(
                        reinterpret_cast<const char *>( pending.data() + frame_header_size ), length );
                    pending.clear();
                }
            }
            return decoded;
        }

        bool empty() const {
            return pending.empty();
        }

    private:
        std::vector<std::uint8_t> pending;
};

class bounded_frame_queue
{
    public:
        bounded_frame_queue( const std::size_t maximum_count,
                             const std::size_t maximum_bytes ) :
            maximum_count( maximum_count ),
            maximum_bytes( maximum_bytes ) {
        }

        bool try_push( std::string value ) {
            std::lock_guard<std::mutex> lock( mutex );
            if( values.size() >= maximum_count || value.size() > maximum_bytes - bytes ) {
                return false;
            }
            bytes += value.size();
            values.emplace_back( std::move( value ) );
            return true;
        }

        std::optional<std::string> try_pop() {
            std::lock_guard<std::mutex> lock( mutex );
            if( values.empty() ) {
                return std::nullopt;
            }
            std::string result = std::move( values.front() );
            values.pop_front();
            bytes -= result.size();
            return result;
        }

    private:
        const std::size_t maximum_count;
        const std::size_t maximum_bytes;
        std::mutex mutex;
        std::deque<std::string> values;
        std::size_t bytes = 0;
};

class loopback_echo_server
{
    public:
        loopback_echo_server() :
            acceptor( io, asio::ip::tcp::endpoint( asio::ip::address_v4::loopback(), 0 ) ),
            socket( io ),
            inbound( 8, maximum_frame_size ) {
            require( acceptor.local_endpoint().address().is_loopback(),
                     "spike acceptor did not bind loopback" );
            listen_port = acceptor.local_endpoint().port();
            acceptor.async_accept( socket, [this]( const asio::error_code & error ) {
                if( error ) {
                    fail( "accept", error );
                    return;
                }
                read_next();
            } );
            io_thread = std::thread( [this]() {
                io.run();
            } );
        }

        ~loopback_echo_server() {
            io.stop();
            if( io_thread.joinable() ) {
                io_thread.join();
            }
            asio::error_code ignored;
            acceptor.close( ignored );
            socket.close( ignored );
        }

        std::uint16_t port() const {
            return listen_port;
        }

        void join() {
            if( io_thread.joinable() ) {
                io_thread.join();
            }
            if( !failure.empty() ) {
                throw std::runtime_error( failure );
            }
        }

    private:
        void read_next() {
            socket.async_read_some( asio::buffer( read_buffer ),
            [this]( const asio::error_code & error, const std::size_t size ) {
                if( !error ) {
                    try {
                        for( std::string &frame : decoder.push( read_buffer.data(), size ) ) {
                            if( !inbound.try_push( std::move( frame ) ) ) {
                                throw std::runtime_error( "server inbound queue is full" );
                            }
                        }
                    } catch( const std::exception &exception ) {
                        fail( exception.what() );
                        return;
                    }
                    read_next();
                    return;
                }
                if( error == asio::error::eof ) {
                    if( !decoder.empty() ) {
                        fail( "peer half-closed with a partial frame" );
                        return;
                    }
                    write_responses();
                    return;
                }
                fail( "read", error );
            } );
        }

        void write_responses() {
            std::vector<std::string> values;
            while( std::optional<std::string> value = inbound.try_pop() ) {
                values.emplace_back( std::move( *value ) );
            }
            for( const std::string &value : values ) {
                const std::vector<std::uint8_t> encoded = encode_frame( "ack:" + value );
                response_bytes.insert( response_bytes.end(), encoded.begin(), encoded.end() );
            }
            asio::async_write( socket, asio::buffer( response_bytes ),
            [this]( const asio::error_code & error, const std::size_t ) {
                if( error ) {
                    fail( "write", error );
                    return;
                }
                asio::error_code shutdown_error;
                socket.shutdown( asio::ip::tcp::socket::shutdown_send, shutdown_error );
                if( shutdown_error ) {
                    fail( "shutdown", shutdown_error );
                    return;
                }
                asio::error_code ignored;
                socket.close( ignored );
                acceptor.close( ignored );
            } );
        }

        void fail( const std::string &operation, const asio::error_code &error ) {
            fail( operation + ": " + error.message() );
        }

        void fail( std::string message ) {
            failure = std::move( message );
            asio::error_code ignored;
            socket.close( ignored );
            acceptor.close( ignored );
        }

        asio::io_context io;
        asio::ip::tcp::acceptor acceptor;
        asio::ip::tcp::socket socket;
        frame_decoder decoder;
        bounded_frame_queue inbound;
        std::array<std::uint8_t, network_read_size> read_buffer = {};
        std::vector<std::uint8_t> response_bytes;
        std::thread io_thread;
        std::string failure;
        std::uint16_t listen_port = 0;
};

void test_fragmentation_and_coalescing()
{
    const std::vector<std::uint8_t> first = encode_frame( "fragmented" );
    const std::vector<std::uint8_t> second = encode_frame( "coalesced-one" );
    const std::vector<std::uint8_t> third = encode_frame( "coalesced-two" );

    frame_decoder fragmented_decoder;
    std::vector<std::string> fragmented_result;
    for( const std::uint8_t byte : first ) {
        std::vector<std::string> decoded = fragmented_decoder.push( &byte, 1 );
        fragmented_result.insert( fragmented_result.end(), decoded.begin(), decoded.end() );
    }
    require( fragmented_result == std::vector<std::string> { "fragmented" },
             "one-byte frame fragmentation failed" );

    std::vector<std::uint8_t> coalesced = second;
    coalesced.insert( coalesced.end(), third.begin(), third.end() );
    frame_decoder coalesced_decoder;
    require( coalesced_decoder.push( coalesced.data(), coalesced.size() ) ==
             std::vector<std::string> { "coalesced-one", "coalesced-two" },
             "coalesced frame decoding failed" );

    const std::string maximum_payload( maximum_frame_size, 'x' );
    const std::vector<std::uint8_t> maximum_frame = encode_frame( maximum_payload );
    const std::vector<std::uint8_t> following_frame = encode_frame( "after-maximum" );
    frame_decoder boundary_decoder;
    require( boundary_decoder.push( maximum_frame.data(), maximum_frame.size() - 3 ).empty(),
             "partial maximum frame decoded too early" );
    std::vector<std::uint8_t> boundary_chunk( maximum_frame.end() - 3, maximum_frame.end() );
    boundary_chunk.insert( boundary_chunk.end(), following_frame.begin(), following_frame.end() );
    require( boundary_decoder.push( boundary_chunk.data(), boundary_chunk.size() ) ==
             std::vector<std::string> { maximum_payload, "after-maximum" },
             "maximum frame plus coalesced next header failed" );

    const std::array<std::uint8_t, frame_header_size> oversized_header = {
        0x00, 0x10, 0x00, 0x01
    };
    bool oversized_rejected = false;
    try {
        frame_decoder oversized_decoder;
        oversized_decoder.push( oversized_header.data(), oversized_header.size() );
    } catch( const std::length_error & ) {
        oversized_rejected = true;
    }
    require( oversized_rejected, "oversized frame declaration was accepted" );
}

void test_bounded_queue()
{
    bounded_frame_queue queue( 2, 8 );
    require( queue.try_push( "one" ), "first queue insertion failed" );
    require( queue.try_push( "two" ), "second queue insertion failed" );
    require( !queue.try_push( "three" ), "full queue accepted another frame" );
    require( queue.try_pop() == std::optional<std::string>( "one" ),
             "queue order changed" );
    require( queue.try_pop() == std::optional<std::string>( "two" ),
             "queue order changed" );
    require( !queue.try_pop(), "empty queue returned a frame" );
}

void test_accept_cancellation()
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(
        io, asio::ip::tcp::endpoint( asio::ip::address_v4::loopback(), 0 ) );
    asio::ip::tcp::socket socket( io );
    bool handler_ran = false;
    bool operation_was_cancelled = false;
    acceptor.async_accept( socket, [&]( const asio::error_code & error ) {
        handler_ran = true;
        operation_was_cancelled = error == asio::error::operation_aborted;
    } );
    asio::error_code cancel_error;
    acceptor.cancel( cancel_error );
    require( !cancel_error, "accept cancellation returned an error" );
    io.run();
    require( handler_ran && operation_was_cancelled,
             "cancelled accept did not complete with operation_aborted" );
}

void test_loopback_half_close_and_ordered_shutdown()
{
    loopback_echo_server server;
    asio::io_context client_io;
    asio::ip::tcp::socket client( client_io );
    client.connect( asio::ip::tcp::endpoint( asio::ip::address_v4::loopback(), server.port() ) );

    const std::vector<std::uint8_t> first = encode_frame( "alpha" );
    for( const std::uint8_t byte : first ) {
        asio::write( client, asio::buffer( &byte, 1 ) );
    }
    std::vector<std::uint8_t> remaining = encode_frame( "beta" );
    const std::vector<std::uint8_t> third = encode_frame( "gamma" );
    remaining.insert( remaining.end(), third.begin(), third.end() );
    asio::write( client, asio::buffer( remaining ) );

    client.shutdown( asio::ip::tcp::socket::shutdown_send );

    frame_decoder response_decoder;
    std::vector<std::string> responses;
    std::array<std::uint8_t, 5> response_buffer = {};
    while( true ) {
        asio::error_code error;
        const std::size_t size = client.read_some( asio::buffer( response_buffer ), error );
        if( size > 0 ) {
            std::vector<std::string> decoded = response_decoder.push( response_buffer.data(), size );
            responses.insert( responses.end(), decoded.begin(), decoded.end() );
        }
        if( error == asio::error::eof ) {
            break;
        }
        if( error ) {
            throw asio::system_error( error );
        }
    }
    client.close();
    server.join();

    require( response_decoder.empty(), "server response ended with a partial frame" );
    require( responses == std::vector<std::string> { "ack:alpha", "ack:beta", "ack:gamma" },
             "half-close response order changed" );
}

} // namespace

int main()
{
    try {
        test_fragmentation_and_coalescing();
        test_bounded_queue();
        test_accept_cancellation();
        test_loopback_half_close_and_ordered_shutdown();
        std::cout << "transport spike passed: fragmentation, coalescing, half-close, "
                  << "cancellation, queue-full, ordered-shutdown\n";
        return 0;
    } catch( const std::exception &error ) {
        std::cerr << "transport spike failed: " << error.what() << '\n';
        return 1;
    }
}

#pragma once
#ifndef CATA_SRC_MULTIPLAYER_TRANSPORT_H
#define CATA_SRC_MULTIPLAYER_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

constexpr std::size_t multiplayer_transport_frame_header_size = 4;
constexpr std::size_t multiplayer_transport_maximum_frame_size = 1024 * 1024;

using multiplayer_connection_id = std::uint64_t;
using multiplayer_transport_payload = std::vector<std::uint8_t>;

bool multiplayer_encode_transport_frame( const multiplayer_transport_payload &payload,
        multiplayer_transport_payload &encoded, std::string &error );

class multiplayer_transport_frame_decoder
{
    public:
        bool push( const std::uint8_t *data, std::size_t size,
                   std::vector<multiplayer_transport_payload> &decoded, std::string &error );
        bool empty() const;
        void reset();

    private:
        multiplayer_transport_payload pending_;
};

enum class multiplayer_transport_event_type : std::uint8_t {
    connected,
    frame,
    peer_half_closed,
    disconnected,
    protocol_error,
    transport_error
};

struct multiplayer_transport_event {
    multiplayer_transport_event_type type = multiplayer_transport_event_type::transport_error;
    multiplayer_connection_id connection = 0;
    multiplayer_transport_payload payload;
    std::string detail;
    std::string peer_address;
};

struct multiplayer_transport_endpoint {
    std::string host = "127.0.0.1";
    std::uint16_t port = 27999;
};

struct multiplayer_server_transport_settings {
    std::size_t maximum_connections = 8;
    std::size_t inbound_event_count = 1024;
    std::size_t inbound_payload_bytes = 16 * 1024 * 1024;
    std::size_t outbound_command_count = 1024;
    std::size_t outbound_payload_bytes = 16 * 1024 * 1024;
    std::size_t pending_writes_per_connection = 64;
    std::size_t pending_write_bytes_per_connection = 8 * 1024 * 1024;
};

struct multiplayer_client_transport_settings {
    std::size_t inbound_event_count = 128;
    std::size_t inbound_payload_bytes = 8 * 1024 * 1024;
    std::size_t outbound_command_count = 128;
    std::size_t outbound_payload_bytes = 8 * 1024 * 1024;
    std::size_t pending_writes = 32;
    std::size_t pending_write_bytes = 4 * 1024 * 1024;
};

enum class multiplayer_transport_send_result : std::uint8_t {
    queued,
    stopped,
    frame_too_large,
    queue_full
};

class multiplayer_server_transport
{
    public:
        explicit multiplayer_server_transport( multiplayer_server_transport_settings settings = {} );
        ~multiplayer_server_transport();

        multiplayer_server_transport( const multiplayer_server_transport & ) = delete;
        multiplayer_server_transport &operator=( const multiplayer_server_transport & ) = delete;

        bool start( const multiplayer_transport_endpoint &endpoint, std::string &error );
        void stop();
        bool running() const;
        std::string failure_detail() const;
        std::uint16_t bound_port() const;

        std::optional<multiplayer_transport_event> poll_event();
        multiplayer_transport_send_result send( multiplayer_connection_id connection,
                                                multiplayer_transport_payload payload );
        multiplayer_transport_send_result send_and_disconnect(
            multiplayer_connection_id connection, multiplayer_transport_payload payload,
            std::string reason );
        bool disconnect( multiplayer_connection_id connection, std::string reason );

    private:
        class impl;
        std::unique_ptr<impl> impl_;
};

class multiplayer_client_transport
{
    public:
        explicit multiplayer_client_transport( multiplayer_client_transport_settings settings = {} );
        ~multiplayer_client_transport();

        multiplayer_client_transport( const multiplayer_client_transport & ) = delete;
        multiplayer_client_transport &operator=( const multiplayer_client_transport & ) = delete;

        bool start( const multiplayer_transport_endpoint &endpoint, std::string &error );
        void stop();
        bool running() const;
        bool connected() const;

        std::optional<multiplayer_transport_event> poll_event();
        multiplayer_transport_send_result send( multiplayer_transport_payload payload );

    private:
        class impl;
        std::unique_ptr<impl> impl_;
};

#endif // CATA_SRC_MULTIPLAYER_TRANSPORT_H

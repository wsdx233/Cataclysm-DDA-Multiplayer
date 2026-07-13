#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif

#define ASIO_NO_DEPRECATED
#define ASIO_STANDALONE
#include <asio.hpp>

#include "multiplayer_transport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

bool multiplayer_encode_transport_frame( const multiplayer_transport_payload &payload,
        multiplayer_transport_payload &encoded, std::string &error )
{
    if( payload.size() > multiplayer_transport_maximum_frame_size ||
        payload.size() > std::numeric_limits<std::uint32_t>::max() ) {
        error = "transport frame exceeds the 1 MiB limit";
        return false;
    }

    const std::uint32_t length = static_cast<std::uint32_t>( payload.size() );
    encoded.clear();
    encoded.reserve( multiplayer_transport_frame_header_size + payload.size() );
    encoded.push_back( static_cast<std::uint8_t>( length >> 24U ) );
    encoded.push_back( static_cast<std::uint8_t>( length >> 16U ) );
    encoded.push_back( static_cast<std::uint8_t>( length >> 8U ) );
    encoded.push_back( static_cast<std::uint8_t>( length ) );
    encoded.insert( encoded.end(), payload.begin(), payload.end() );
    error.clear();
    return true;
}

bool multiplayer_transport_frame_decoder::push( const std::uint8_t *data, const std::size_t size,
        std::vector<multiplayer_transport_payload> &decoded, std::string &error )
{
    if( data == nullptr && size != 0 ) {
        error = "transport decoder received a null buffer";
        return false;
    }

    std::size_t consumed = 0;
    while( consumed < size ) {
        if( pending_.size() < multiplayer_transport_frame_header_size ) {
            const std::size_t header_bytes = std::min(
                                                 multiplayer_transport_frame_header_size - pending_.size(), size - consumed );
            pending_.insert( pending_.end(), data + consumed, data + consumed + header_bytes );
            consumed += header_bytes;
            if( pending_.size() < multiplayer_transport_frame_header_size ) {
                continue;
            }
        }

        const std::uint32_t length =
            static_cast<std::uint32_t>( pending_[0] ) << 24U |
            static_cast<std::uint32_t>( pending_[1] ) << 16U |
            static_cast<std::uint32_t>( pending_[2] ) << 8U |
            static_cast<std::uint32_t>( pending_[3] );
        if( length > multiplayer_transport_maximum_frame_size ) {
            error = "declared transport frame exceeds the 1 MiB limit";
            return false;
        }

        const std::size_t encoded_size = multiplayer_transport_frame_header_size + length;
        const std::size_t payload_bytes = std::min( encoded_size - pending_.size(), size - consumed );
        pending_.insert( pending_.end(), data + consumed, data + consumed + payload_bytes );
        consumed += payload_bytes;
        if( pending_.size() == encoded_size ) {
            decoded.emplace_back( pending_.begin() + multiplayer_transport_frame_header_size,
                                  pending_.end() );
            pending_.clear();
        }
    }
    error.clear();
    return true;
}

bool multiplayer_transport_frame_decoder::empty() const
{
    return pending_.empty();
}

void multiplayer_transport_frame_decoder::reset()
{
    pending_.clear();
}

namespace
{

std::size_t event_payload_size( const multiplayer_transport_event &event )
{
    return event.payload.size();
}

struct outbound_command {
    enum class type : std::uint8_t {
        send,
        disconnect
    };

    type operation = type::send;
    multiplayer_connection_id connection = 0;
    std::shared_ptr<const multiplayer_transport_payload> encoded_frame;
    std::string reason;
};

std::size_t command_payload_size( const outbound_command &command )
{
    return command.encoded_frame ? command.encoded_frame->size() : command.reason.size();
}

template<typename T, std::size_t( *PayloadSize )( const T & )>
class bounded_transport_queue
{
    public:
        bounded_transport_queue( const std::size_t maximum_count, const std::size_t maximum_bytes ) :
            maximum_count_( maximum_count ),
            maximum_bytes_( maximum_bytes ) {
        }

        bool try_push( T value, const std::size_t reserved_count = 0 ) {
            std::lock_guard<std::mutex> lock( mutex_ );
            const std::size_t value_bytes = PayloadSize( value );
            if( closed_ || reserved_count > maximum_count_ ||
                values_.size() >= maximum_count_ - reserved_count ||
                value_bytes > maximum_bytes_ - bytes_ ) {
                return false;
            }
            bytes_ += value_bytes;
            values_.emplace_back( std::move( value ) );
            return true;
        }

        std::optional<T> try_pop() {
            std::lock_guard<std::mutex> lock( mutex_ );
            if( values_.empty() ) {
                return std::nullopt;
            }
            T value = std::move( values_.front() );
            values_.pop_front();
            bytes_ -= PayloadSize( value );
            return value;
        }

        void close() {
            std::lock_guard<std::mutex> lock( mutex_ );
            closed_ = true;
        }

        void clear() {
            std::lock_guard<std::mutex> lock( mutex_ );
            values_.clear();
            bytes_ = 0;
        }

    private:
        const std::size_t maximum_count_;
        const std::size_t maximum_bytes_;
        std::mutex mutex_;
        std::deque<T> values_;
        std::size_t bytes_ = 0;
        bool closed_ = false;
};

} // namespace

class multiplayer_server_transport::impl
{
    public:
        explicit impl( multiplayer_server_transport_settings settings ) :
            settings_( std::move( settings ) ),
            work_( asio::make_work_guard( io_ ) ),
            acceptor_( io_ ),
            inbound_( settings_.inbound_event_count, settings_.inbound_payload_bytes ),
            outbound_( settings_.outbound_command_count, settings_.outbound_payload_bytes ) {
        }

        ~impl() {
            stop();
        }

        bool start( const multiplayer_transport_endpoint &endpoint, std::string &error ) {
            if( started_ ) {
                error = "server transport instances cannot be restarted";
                return false;
            }
            if( settings_.maximum_connections == 0 || settings_.inbound_event_count <
                settings_.maximum_connections * 2 || settings_.outbound_command_count == 0 ||
                settings_.pending_writes_per_connection == 0 ) {
                error = "server transport queue settings are invalid";
                return false;
            }

            asio::error_code asio_error;
            asio::ip::tcp::resolver resolver( io_ );
            const asio::ip::tcp::resolver::results_type endpoints = resolver.resolve(
                        endpoint.host, std::to_string( endpoint.port ),
                        asio::ip::tcp::resolver::passive, asio_error );
            if( asio_error || endpoints.empty() ) {
                error = "unable to resolve listen endpoint: " + asio_error.message();
                return false;
            }

            std::string last_error;
            for( const asio::ip::tcp::resolver::results_type::value_type &entry : endpoints ) {
                const asio::ip::tcp::endpoint candidate = entry.endpoint();
                acceptor_.open( candidate.protocol(), asio_error );
                if( asio_error ) {
                    last_error = asio_error.message();
                    continue;
                }
                acceptor_.set_option( asio::socket_base::reuse_address( true ), asio_error );
                if( !asio_error ) {
                    acceptor_.bind( candidate, asio_error );
                }
                if( !asio_error ) {
                    acceptor_.listen( asio::socket_base::max_listen_connections, asio_error );
                }
                if( !asio_error ) {
                    break;
                }
                last_error = asio_error.message();
                asio::error_code ignored;
                acceptor_.close( ignored );
            }
            if( !acceptor_.is_open() || asio_error ) {
                error = "unable to bind listen endpoint: " + last_error;
                return false;
            }

            bound_port_ = acceptor_.local_endpoint( asio_error ).port();
            if( asio_error ) {
                error = "unable to inspect listen endpoint: " + asio_error.message();
                asio::error_code ignored;
                acceptor_.close( ignored );
                return false;
            }

            running_.store( true );
            started_ = true;
            accept_next();
            io_thread_ = std::thread( [this]() {
                try {
                    io_.run();
                } catch( const std::exception &exception ) {
                    push_control_event( { multiplayer_transport_event_type::transport_error, 0, {},
                                          std::string( "transport I/O thread failed: " ) + exception.what(), {} } );
                    running_.store( false );
                }
            } );
            error.clear();
            return true;
        }

        void stop() {
            if( !started_ ) {
                return;
            }
            if( !running_.exchange( false ) ) {
                work_.reset();
                io_.stop();
                if( io_thread_.joinable() ) {
                    io_thread_.join();
                }
                asio::error_code ignored;
                acceptor_.close( ignored );
                for( const auto &entry : connections_ ) {
                    entry.second->close_socket();
                }
                connections_.clear();
                return;
            }

            outbound_.close();
            asio::post( io_, [this]() {
                asio::error_code ignored;
                acceptor_.cancel( ignored );
                acceptor_.close( ignored );
                for( const auto &entry : connections_ ) {
                    entry.second->close_socket();
                }
                connections_.clear();
                work_.reset();
            } );
            if( io_thread_.joinable() ) {
                io_thread_.join();
            }
            outbound_.clear();
        }

        bool running() const {
            return running_.load();
        }

        std::uint16_t bound_port() const {
            return bound_port_;
        }

        std::optional<multiplayer_transport_event> poll_event() {
            return inbound_.try_pop();
        }

        multiplayer_transport_send_result send( const multiplayer_connection_id connection,
                                                multiplayer_transport_payload payload ) {
            if( !running_.load() ) {
                return multiplayer_transport_send_result::stopped;
            }
            multiplayer_transport_payload encoded;
            std::string error;
            if( !multiplayer_encode_transport_frame( payload, encoded, error ) ) {
                return multiplayer_transport_send_result::frame_too_large;
            }
            outbound_command command;
            command.connection = connection;
            command.encoded_frame = std::make_shared<const multiplayer_transport_payload>(
                                        std::move( encoded ) );
            if( !outbound_.try_push( std::move( command ) ) ) {
                return multiplayer_transport_send_result::queue_full;
            }
            asio::post( io_, [this]() {
                drain_outbound();
            } );
            return multiplayer_transport_send_result::queued;
        }

        bool disconnect( const multiplayer_connection_id connection, std::string reason ) {
            if( !running_.load() ) {
                return false;
            }
            outbound_command command;
            command.operation = outbound_command::type::disconnect;
            command.connection = connection;
            command.reason = std::move( reason );
            if( !outbound_.try_push( std::move( command ) ) ) {
                return false;
            }
            asio::post( io_, [this]() {
                drain_outbound();
            } );
            return true;
        }

    private:
        class connection : public std::enable_shared_from_this<connection>
        {
            public:
                connection( impl &owner, asio::ip::tcp::socket socket,
                            const multiplayer_connection_id id ) :
                    owner_( owner ), socket_( std::move( socket ) ), id_( id ) {
                    asio::error_code error;
                    const asio::ip::tcp::endpoint remote = socket_.remote_endpoint( error );
                    if( !error ) {
                        peer_address_ = remote.address().to_string();
                    }
                }

                void start() {
                    asio::error_code ignored;
                    socket_.set_option( asio::ip::tcp::no_delay( true ), ignored );
                    read_header();
                }

                const std::string &peer_address() const {
                    return peer_address_;
                }

                bool enqueue_write( std::shared_ptr<const multiplayer_transport_payload> frame ) {
                    if( closed_ || close_after_writes_ ||
                        writes_.size() >= owner_.settings_.pending_writes_per_connection ||
                        frame->size() > owner_.settings_.pending_write_bytes_per_connection - write_bytes_ ) {
                        return false;
                    }
                    const bool idle = writes_.empty();
                    write_bytes_ += frame->size();
                    writes_.emplace_back( std::move( frame ) );
                    if( idle ) {
                        write_next();
                    }
                    return true;
                }

                void request_close( std::string detail ) {
                    if( closed_ || close_after_writes_ ) {
                        return;
                    }
                    close_after_writes_ = true;
                    close_detail_ = std::move( detail );
                    if( writes_.empty() ) {
                        close( multiplayer_transport_event_type::disconnected,
                               std::move( close_detail_ ) );
                    }
                }

                void close( const multiplayer_transport_event_type type, std::string detail ) {
                    if( closed_ ) {
                        return;
                    }
                    closed_ = true;
                    close_socket();
                    owner_.connections_.erase( id_ );
                    owner_.push_control_event( { type, id_, {}, std::move( detail ), peer_address_ } );
                }

                void close_socket() {
                    asio::error_code ignored;
                    socket_.cancel( ignored );
                    socket_.shutdown( asio::ip::tcp::socket::shutdown_both, ignored );
                    socket_.close( ignored );
                    closed_ = true;
                }

            private:
                void read_header() {
                    const std::shared_ptr<connection> self = shared_from_this();
                    asio::async_read( socket_, asio::buffer( header_ ),
                    [self]( const asio::error_code & error, const std::size_t transferred ) {
                        if( error ) {
                            self->handle_read_error( error, transferred, false );
                            return;
                        }
                        const std::uint32_t length =
                            static_cast<std::uint32_t>( self->header_[0] ) << 24U |
                            static_cast<std::uint32_t>( self->header_[1] ) << 16U |
                            static_cast<std::uint32_t>( self->header_[2] ) << 8U |
                            static_cast<std::uint32_t>( self->header_[3] );
                        if( length > multiplayer_transport_maximum_frame_size ) {
                            self->close( multiplayer_transport_event_type::protocol_error,
                                         "declared transport frame exceeds the 1 MiB limit" );
                            return;
                        }
                        self->payload_.assign( length, 0 );
                        self->read_payload();
                    } );
                }

                void read_payload() {
                    if( payload_.empty() ) {
                        deliver_payload();
                        return;
                    }
                    const std::shared_ptr<connection> self = shared_from_this();
                    asio::async_read( socket_, asio::buffer( payload_ ),
                    [self]( const asio::error_code & error, const std::size_t transferred ) {
                        if( error ) {
                            self->handle_read_error( error, transferred, true );
                            return;
                        }
                        self->deliver_payload();
                    } );
                }

                void deliver_payload() {
                    multiplayer_transport_event event;
                    event.type = multiplayer_transport_event_type::frame;
                    event.connection = id_;
                    event.payload = std::move( payload_ );
                    if( !owner_.push_frame_event( std::move( event ) ) ) {
                        close( multiplayer_transport_event_type::transport_error,
                               "inbound transport queue is full" );
                        return;
                    }
                    read_header();
                }

                void handle_read_error( const asio::error_code &error, const std::size_t transferred,
                                        const bool reading_payload ) {
                    if( error == asio::error::operation_aborted && closed_ ) {
                        return;
                    }
                    if( error == asio::error::eof ) {
                        if( transferred != 0 || reading_payload ) {
                            close( multiplayer_transport_event_type::protocol_error,
                                   std::string( "peer half-closed with a partial frame " ) +
                                   ( reading_payload ? "payload" : "header" ) );
                            return;
                        }
                        if( !owner_.push_control_event( {
                        multiplayer_transport_event_type::peer_half_closed, id_, {},
                        "peer half-closed its send stream", peer_address_
                    } ) ) {
                            close( multiplayer_transport_event_type::transport_error,
                                   "inbound transport queue is full" );
                        }
                        return;
                    }
                    close( multiplayer_transport_event_type::disconnected,
                           "transport read failed: " + error.message() );
                }

                void write_next() {
                    const std::shared_ptr<connection> self = shared_from_this();
                    asio::async_write( socket_, asio::buffer( *writes_.front() ),
                    [self]( const asio::error_code & error, const std::size_t ) {
                        if( error ) {
                            self->close( multiplayer_transport_event_type::disconnected,
                                         "transport write failed: " + error.message() );
                            return;
                        }
                        self->write_bytes_ -= self->writes_.front()->size();
                        self->writes_.pop_front();
                        if( !self->writes_.empty() ) {
                            self->write_next();
                        } else if( self->close_after_writes_ ) {
                            self->close( multiplayer_transport_event_type::disconnected,
                                         std::move( self->close_detail_ ) );
                        }
                    } );
                }

                impl &owner_;
                asio::ip::tcp::socket socket_;
                multiplayer_connection_id id_;
                std::string peer_address_;
                std::array<std::uint8_t, multiplayer_transport_frame_header_size> header_ = {};
                multiplayer_transport_payload payload_;
                std::deque<std::shared_ptr<const multiplayer_transport_payload>> writes_;
                std::size_t write_bytes_ = 0;
                std::string close_detail_;
                bool closed_ = false;
                bool close_after_writes_ = false;
        };

        void accept_next() {
            acceptor_.async_accept( [this]( const asio::error_code & error,
            asio::ip::tcp::socket socket ) {
                if( !error ) {
                    if( connections_.size() < settings_.maximum_connections ) {
                        const multiplayer_connection_id id = next_connection_id_++;
                        const std::shared_ptr<connection> accepted =
                            std::make_shared<connection>( *this, std::move( socket ), id );
                        connections_.emplace( id, accepted );
                        if( push_control_event( { multiplayer_transport_event_type::connected, id, {}, {},
                                                  accepted->peer_address() } ) ) {
                            accepted->start();
                        } else {
                            accepted->close_socket();
                            connections_.erase( id );
                        }
                    } else {
                        asio::error_code ignored;
                        socket.close( ignored );
                    }
                } else if( error != asio::error::operation_aborted && running_.load() ) {
                    push_control_event( { multiplayer_transport_event_type::transport_error, 0, {},
                                          "transport accept failed: " + error.message(), {} } );
                }
                if( running_.load() && acceptor_.is_open() ) {
                    accept_next();
                }
            } );
        }

        bool push_frame_event( multiplayer_transport_event event ) {
            return inbound_.try_push( std::move( event ), settings_.maximum_connections * 2 );
        }

        bool push_control_event( multiplayer_transport_event event ) {
            return inbound_.try_push( std::move( event ) );
        }

        void drain_outbound() {
            while( std::optional<outbound_command> command = outbound_.try_pop() ) {
                const auto found = connections_.find( command->connection );
                if( found == connections_.end() ) {
                    continue;
                }
                if( command->operation == outbound_command::type::disconnect ) {
                    found->second->request_close( command->reason.empty() ?
                                                  "server disconnected peer" :
                                                  std::move( command->reason ) );
                } else if( !found->second->enqueue_write( std::move( command->encoded_frame ) ) ) {
                    found->second->close( multiplayer_transport_event_type::transport_error,
                                          "outbound per-connection queue is full" );
                }
            }
        }

        multiplayer_server_transport_settings settings_;
        asio::io_context io_;
        asio::executor_work_guard<asio::io_context::executor_type> work_;
        asio::ip::tcp::acceptor acceptor_;
        bounded_transport_queue<multiplayer_transport_event, event_payload_size> inbound_;
        bounded_transport_queue<outbound_command, command_payload_size> outbound_;
        std::map<multiplayer_connection_id, std::shared_ptr<connection>> connections_;
        std::thread io_thread_;
        std::atomic<bool> running_ = false;
        bool started_ = false;
        multiplayer_connection_id next_connection_id_ = 1;
        std::uint16_t bound_port_ = 0;
};

multiplayer_server_transport::multiplayer_server_transport(
    multiplayer_server_transport_settings settings ) :
    impl_( std::make_unique<impl>( std::move( settings ) ) )
{
}

multiplayer_server_transport::~multiplayer_server_transport() = default;

bool multiplayer_server_transport::start( const multiplayer_transport_endpoint &endpoint,
        std::string &error )
{
    return impl_->start( endpoint, error );
}

void multiplayer_server_transport::stop()
{
    impl_->stop();
}

bool multiplayer_server_transport::running() const
{
    return impl_->running();
}

std::uint16_t multiplayer_server_transport::bound_port() const
{
    return impl_->bound_port();
}

std::optional<multiplayer_transport_event> multiplayer_server_transport::poll_event()
{
    return impl_->poll_event();
}

multiplayer_transport_send_result multiplayer_server_transport::send(
    const multiplayer_connection_id connection, multiplayer_transport_payload payload )
{
    return impl_->send( connection, std::move( payload ) );
}

bool multiplayer_server_transport::disconnect( const multiplayer_connection_id connection,
        std::string reason )
{
    return impl_->disconnect( connection, std::move( reason ) );
}

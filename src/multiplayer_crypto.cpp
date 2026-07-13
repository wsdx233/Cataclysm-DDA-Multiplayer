#include "multiplayer_crypto.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

bool multiplayer_fill_secure_random( std::uint8_t *output, const std::size_t size,
                                     std::string &error )
{
    if( output == nullptr && size != 0 ) {
        error = "secure random output buffer is null";
        return false;
    }
#if defined(_WIN32)
    if( size > static_cast<std::size_t>( std::numeric_limits<ULONG>::max() ) ) {
        error = "secure random request is too large";
        return false;
    }
    const NTSTATUS status = BCryptGenRandom( nullptr, output, static_cast<ULONG>( size ),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG );
    if( status < 0 ) {
        error = "Windows cryptographic random generator failed";
        return false;
    }
#else
    const int descriptor = open( "/dev/urandom", O_RDONLY | O_CLOEXEC );
    if( descriptor < 0 ) {
        error = std::string( "unable to open operating-system random source: " ) +
                std::strerror( errno );
        return false;
    }
    std::size_t offset = 0;
    while( offset < size ) {
        const ssize_t count = read( descriptor, output + offset, size - offset );
        if( count > 0 ) {
            offset += static_cast<std::size_t>( count );
        } else if( count < 0 && errno == EINTR ) {
            continue;
        } else {
            const int read_error = errno;
            close( descriptor );
            error = std::string( "unable to read operating-system random source: " ) +
                    std::strerror( read_error );
            return false;
        }
    }
    if( close( descriptor ) != 0 ) {
        error = std::string( "unable to close operating-system random source: " ) +
                std::strerror( errno );
        return false;
    }
#endif
    error.clear();
    return true;
}

bool multiplayer_generate_bearer_token( std::string &token, std::string &error )
{
    std::array<std::uint8_t, 32> bytes = {};
    if( !multiplayer_fill_secure_random( bytes.data(), bytes.size(), error ) ) {
        return false;
    }
    constexpr char hexadecimal[] = "0123456789abcdef";
    token.clear();
    token.reserve( bytes.size() * 2 );
    for( const std::uint8_t byte : bytes ) {
        token.push_back( hexadecimal[byte >> 4U] );
        token.push_back( hexadecimal[byte & 0x0fU] );
    }
    error.clear();
    return true;
}

bool multiplayer_generate_uuid_v4( std::string &uuid, std::string &error )
{
    std::array<std::uint8_t, 16> bytes = {};
    if( !multiplayer_fill_secure_random( bytes.data(), bytes.size(), error ) ) {
        return false;
    }
    bytes[6] = static_cast<std::uint8_t>( ( bytes[6] & 0x0fU ) | 0x40U );
    bytes[8] = static_cast<std::uint8_t>( ( bytes[8] & 0x3fU ) | 0x80U );
    constexpr char hexadecimal[] = "0123456789abcdef";
    uuid.clear();
    uuid.reserve( 36 );
    for( std::size_t index = 0; index < bytes.size(); ++index ) {
        if( index == 4 || index == 6 || index == 8 || index == 10 ) {
            uuid.push_back( '-' );
        }
        uuid.push_back( hexadecimal[bytes[index] >> 4U] );
        uuid.push_back( hexadecimal[bytes[index] & 0x0fU] );
    }
    error.clear();
    return true;
}

bool multiplayer_is_valid_bearer_token( const std::string &token )
{
    if( token.size() != 64 ) {
        return false;
    }
    for( const unsigned char ch : token ) {
        if( !( ch >= '0' && ch <= '9' ) && !( ch >= 'a' && ch <= 'f' ) ) {
            return false;
        }
    }
    return true;
}

bool multiplayer_constant_time_token_equal( const std::string &left, const std::string &right )
{
    const std::size_t maximum_size = std::max( left.size(), right.size() );
    std::size_t difference = left.size() ^ right.size();
    for( std::size_t index = 0; index < maximum_size; ++index ) {
        const std::uint8_t left_byte = index < left.size() ?
                                       static_cast<std::uint8_t>( left[index] ) : 0;
        const std::uint8_t right_byte = index < right.size() ?
                                        static_cast<std::uint8_t>( right[index] ) : 0;
        difference |= left_byte ^ right_byte;
    }
    return difference == 0;
}

bool multiplayer_write_private_file_exclusive( const std::filesystem::path &path,
        const std::string &contents, std::string &error )
{
#if defined(_WIN32)
    if( contents.size() > static_cast<std::size_t>( std::numeric_limits<DWORD>::max() ) ) {
        error = "private file contents are too large";
        return false;
    }
    const HANDLE file = CreateFileW( path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, nullptr );
    if( file == INVALID_HANDLE_VALUE ) {
        error = "unable to create private file exclusively (Windows error " +
                std::to_string( GetLastError() ) + ")";
        return false;
    }
    DWORD written = 0;
    const bool write_succeeded = WriteFile( file, contents.data(),
                                            static_cast<DWORD>( contents.size() ),
                                            &written, nullptr ) != 0 &&
                                 static_cast<std::size_t>( written ) == contents.size();
    const DWORD write_error = write_succeeded ? ERROR_SUCCESS : GetLastError();
    const bool close_succeeded = CloseHandle( file ) != 0;
    const DWORD close_error = close_succeeded ? ERROR_SUCCESS : GetLastError();
    if( !write_succeeded || !close_succeeded ) {
        DeleteFileW( path.c_str() );
        error = "unable to write private file (Windows error " +
                std::to_string( write_succeeded ? close_error : write_error ) + ")";
        return false;
    }
#else
    const int descriptor = open( path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                 S_IRUSR | S_IWUSR );
    if( descriptor < 0 ) {
        error = std::string( "unable to create private file exclusively: " ) + std::strerror( errno );
        return false;
    }
    std::size_t offset = 0;
    while( offset < contents.size() ) {
        const ssize_t count = write( descriptor, contents.data() + offset, contents.size() - offset );
        if( count > 0 ) {
            offset += static_cast<std::size_t>( count );
        } else if( count < 0 && errno == EINTR ) {
            continue;
        } else {
            const int write_error = errno;
            close( descriptor );
            unlink( path.c_str() );
            error = std::string( "unable to write private file: " ) + std::strerror( write_error );
            return false;
        }
    }
    if( close( descriptor ) != 0 ) {
        unlink( path.c_str() );
        error = std::string( "unable to close private file: " ) + std::strerror( errno );
        return false;
    }
#endif
    error.clear();
    return true;
}

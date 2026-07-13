#include "multiplayer_content_manifest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

class sha256_hasher
{
    public:
        void update( const std::uint8_t *data, std::size_t size ) {
            if( finalized_ || size == 0 ) {
                return;
            }
            total_bytes_ += size;
            while( size > 0 ) {
                const std::size_t copy = std::min( size, block_.size() - block_size_ );
                std::copy_n( data, copy, block_.begin() + block_size_ );
                block_size_ += copy;
                data += copy;
                size -= copy;
                if( block_size_ == block_.size() ) {
                    transform( block_.data() );
                    block_size_ = 0;
                }
            }
        }

        void update( const std::string &value ) {
            update( reinterpret_cast<const std::uint8_t *>( value.data() ), value.size() );
        }

        std::array<std::uint8_t, 32> finish() {
            if( !finalized_ ) {
                const std::uint64_t bit_count = total_bytes_ * 8U;
                block_[block_size_++] = 0x80U;
                if( block_size_ > 56 ) {
                    std::fill( block_.begin() + block_size_, block_.end(), 0 );
                    transform( block_.data() );
                    block_size_ = 0;
                }
                std::fill( block_.begin() + block_size_, block_.begin() + 56, 0 );
                for( std::size_t index = 0; index < sizeof( bit_count ); ++index ) {
                    block_[63 - index] = static_cast<std::uint8_t>( bit_count >>( index * 8U ) );
                }
                transform( block_.data() );
                block_size_ = 0;
                finalized_ = true;
            }
            std::array<std::uint8_t, 32> digest = {};
            for( std::size_t word = 0; word < state_.size(); ++word ) {
                digest[word * 4] = static_cast<std::uint8_t>( state_[word] >> 24U );
                digest[word * 4 + 1] = static_cast<std::uint8_t>( state_[word] >> 16U );
                digest[word * 4 + 2] = static_cast<std::uint8_t>( state_[word] >> 8U );
                digest[word * 4 + 3] = static_cast<std::uint8_t>( state_[word] );
            }
            return digest;
        }

    private:
        static std::uint32_t rotate_right( const std::uint32_t value, const unsigned count ) {
            return value >> count | value << ( 32U - count );
        }

        void transform( const std::uint8_t *block ) {
            static constexpr std::array<std::uint32_t, 64> constants = {
                0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
                0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
                0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
                0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
                0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
                0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
                0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
                0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
                0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
                0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
                0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
                0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
                0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
                0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
                0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
                0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
            };
            std::array<std::uint32_t, 64> schedule = {};
            for( std::size_t index = 0; index < 16; ++index ) {
                schedule[index] = static_cast<std::uint32_t>( block[index * 4] ) << 24U |
                                  static_cast<std::uint32_t>( block[index * 4 + 1] ) << 16U |
                                  static_cast<std::uint32_t>( block[index * 4 + 2] ) << 8U |
                                  static_cast<std::uint32_t>( block[index * 4 + 3] );
            }
            for( std::size_t index = 16; index < schedule.size(); ++index ) {
                const std::uint32_t previous_15 = schedule[index - 15];
                const std::uint32_t previous_2 = schedule[index - 2];
                const std::uint32_t sigma0 = rotate_right( previous_15, 7 ) ^
                                             rotate_right( previous_15, 18 ) ^
                                             ( previous_15 >> 3U );
                const std::uint32_t sigma1 = rotate_right( previous_2, 17 ) ^
                                             rotate_right( previous_2, 19 ) ^
                                             ( previous_2 >> 10U );
                schedule[index] = schedule[index - 16] + sigma0 + schedule[index - 7] + sigma1;
            }

            std::uint32_t a = state_[0];
            std::uint32_t b = state_[1];
            std::uint32_t c = state_[2];
            std::uint32_t d = state_[3];
            std::uint32_t e = state_[4];
            std::uint32_t f = state_[5];
            std::uint32_t g = state_[6];
            std::uint32_t h = state_[7];
            for( std::size_t index = 0; index < schedule.size(); ++index ) {
                const std::uint32_t sum1 = rotate_right( e, 6 ) ^ rotate_right( e, 11 ) ^
                                           rotate_right( e, 25 );
                const std::uint32_t choose = ( e & f ) ^ ( ~e & g );
                const std::uint32_t temporary1 = h + sum1 + choose + constants[index] +
                                                 schedule[index];
                const std::uint32_t sum0 = rotate_right( a, 2 ) ^ rotate_right( a, 13 ) ^
                                           rotate_right( a, 22 );
                const std::uint32_t majority = ( a & b ) ^ ( a & c ) ^ ( b & c );
                const std::uint32_t temporary2 = sum0 + majority;
                h = g;
                g = f;
                f = e;
                e = d + temporary1;
                d = c;
                c = b;
                b = a;
                a = temporary1 + temporary2;
            }
            state_[0] += a;
            state_[1] += b;
            state_[2] += c;
            state_[3] += d;
            state_[4] += e;
            state_[5] += f;
            state_[6] += g;
            state_[7] += h;
        }

        std::array<std::uint32_t, 8> state_ = {
            0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
        };
        std::array<std::uint8_t, 64> block_ = {};
        std::uint64_t total_bytes_ = 0;
        std::size_t block_size_ = 0;
        bool finalized_ = false;
};

struct content_file {
    std::string relative_name;
    std::filesystem::path path;
    std::uint64_t size = 0;
};

void hash_u64( sha256_hasher &hasher, const std::uint64_t value )
{
    std::array<std::uint8_t, 8> bytes = {};
    for( std::size_t index = 0; index < bytes.size(); ++index ) {
        bytes[bytes.size() - 1 - index] = static_cast<std::uint8_t>( value >>( index * 8U ) );
    }
    hasher.update( bytes.data(), bytes.size() );
}

void hash_string( sha256_hasher &hasher, const std::string &value )
{
    hash_u64( hasher, value.size() );
    hasher.update( value );
}

bool safe_root_id( const std::string &value )
{
    return !value.empty() && value.size() <= 128 &&
    std::all_of( value.begin(), value.end(), []( const unsigned char ch ) {
        return ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) ||
               ( ch >= '0' && ch <= '9' ) || ch == '.' || ch == '_' || ch == '-' || ch == '+';
    } );
}

bool collect_content_files( const multiplayer_content_root &root,
                            std::vector<content_file> &files, std::string &error )
{
    files.clear();
    std::error_code status_error;
    const std::filesystem::file_status root_status = std::filesystem::symlink_status(
                root.path, status_error );
    if( status_error ) {
        error = "unable to inspect content root '" + root.id + "': " + status_error.message();
        return false;
    }
    if( std::filesystem::is_symlink( root_status ) ) {
        error = "content root '" + root.id + "' must not be a symbolic link";
        return false;
    }
    if( std::filesystem::is_regular_file( root_status ) ) {
        const std::string name = root.path.filename().generic_u8string();
        if( name.empty() ) {
            error = "content root '" + root.id + "' has no canonical file name";
            return false;
        }
        const std::uintmax_t size = std::filesystem::file_size( root.path, status_error );
        if( status_error || size > std::numeric_limits<std::uint64_t>::max() ) {
            error = "unable to determine content file size for root '" + root.id + "'";
            return false;
        }
        files.push_back( { name, root.path, static_cast<std::uint64_t>( size ) } );
        return true;
    }
    if( !std::filesystem::is_directory( root_status ) ) {
        error = "content root '" + root.id + "' is not a regular file or directory";
        return false;
    }

    std::error_code iterator_error;
    std::filesystem::recursive_directory_iterator iterator( root.path,
            std::filesystem::directory_options::none, iterator_error );
    const std::filesystem::recursive_directory_iterator end;
    if( iterator_error ) {
        error = "unable to enumerate content root '" + root.id + "': " +
                iterator_error.message();
        return false;
    }
    while( iterator != end ) {
        const std::filesystem::directory_entry &entry = *iterator;
        const std::filesystem::file_status status = entry.symlink_status( iterator_error );
        if( iterator_error ) {
            error = "unable to inspect an entry in content root '" + root.id + "': " +
                    iterator_error.message();
            return false;
        }
        if( std::filesystem::is_symlink( status ) ) {
            error = "content root '" + root.id + "' contains a symbolic link: " +
                    entry.path().generic_u8string();
            return false;
        }
        if( std::filesystem::is_regular_file( status ) ) {
            const std::filesystem::path relative = entry.path().lexically_relative( root.path );
            const std::string relative_name = relative.generic_u8string();
            if( relative.empty() || relative.is_absolute() || relative_name.empty() ||
                relative.begin() == relative.end() || *relative.begin() == ".." ) {
                error = "content root '" + root.id + "' contains a non-canonical path";
                return false;
            }
            const std::uintmax_t size = entry.file_size( iterator_error );
            if( iterator_error || size > std::numeric_limits<std::uint64_t>::max() ) {
                error = "unable to determine a content file size in root '" + root.id + "'";
                return false;
            }
            files.push_back( { relative_name, entry.path(), static_cast<std::uint64_t>( size ) } );
        } else if( !std::filesystem::is_directory( status ) ) {
            error = "content root '" + root.id + "' contains a non-regular filesystem entry";
            return false;
        }
        iterator.increment( iterator_error );
        if( iterator_error ) {
            error = "unable to enumerate content root '" + root.id + "': " +
                    iterator_error.message();
            return false;
        }
    }
    std::sort( files.begin(), files.end(), []( const content_file & lhs, const content_file & rhs ) {
        return lhs.relative_name < rhs.relative_name;
    } );
    return true;
}

bool hash_file( sha256_hasher &hasher, const content_file &file, std::string &error )
{
    std::ifstream input( file.path, std::ios::binary );
    if( !input ) {
        error = "unable to open gameplay content file: " + file.path.generic_u8string();
        return false;
    }
    std::array<char, 64 * 1024> buffer = {};
    std::uint64_t bytes_read = 0;
    while( input ) {
        input.read( buffer.data(), buffer.size() );
        const std::streamsize count = input.gcount();
        if( count > 0 ) {
            hasher.update( reinterpret_cast<const std::uint8_t *>( buffer.data() ),
                           static_cast<std::size_t>( count ) );
            bytes_read += static_cast<std::uint64_t>( count );
        }
    }
    if( !input.eof() || bytes_read != file.size ) {
        error = "unable to read stable gameplay content bytes from: " +
                file.path.generic_u8string();
        return false;
    }
    return true;
}

std::string digest_hex( const std::array<std::uint8_t, 32> &digest )
{
    std::ostringstream output;
    output.imbue( std::locale::classic() );
    output << std::hex << std::setfill( '0' );
    for( const std::uint8_t byte : digest ) {
        output << std::setw( 2 ) << static_cast<unsigned>( byte );
    }
    return output.str();
}

} // namespace

bool multiplayer_build_content_manifest(
    const std::vector<multiplayer_content_root> &roots,
    std::string &manifest,
    multiplayer_content_manifest_stats &stats,
    std::string &error )
{
    if( roots.empty() ) {
        error = "multiplayer gameplay content manifest requires at least one root";
        return false;
    }
    std::set<std::string> root_ids;
    sha256_hasher hasher;
    hasher.update( "CDDA-MULTIPLAYER-CONTENT-MANIFEST" );
    hash_u64( hasher, 1 );
    hash_u64( hasher, roots.size() );
    multiplayer_content_manifest_stats result_stats;

    for( const multiplayer_content_root &root : roots ) {
        if( !safe_root_id( root.id ) || !root_ids.emplace( root.id ).second ) {
            error = "multiplayer gameplay content root id is invalid or duplicated";
            return false;
        }
        std::vector<content_file> files;
        if( !collect_content_files( root, files, error ) ) {
            return false;
        }
        hash_string( hasher, root.id );
        hash_u64( hasher, files.size() );
        for( const content_file &file : files ) {
            if( result_stats.file_count == std::numeric_limits<std::uint64_t>::max() ||
                file.size > std::numeric_limits<std::uint64_t>::max() - result_stats.byte_count ) {
                error = "multiplayer gameplay content manifest counters overflow";
                return false;
            }
            ++result_stats.file_count;
            result_stats.byte_count += file.size;
            hash_string( hasher, file.relative_name );
            hash_u64( hasher, file.size );
            if( !hash_file( hasher, file, error ) ) {
                return false;
            }
        }
    }

    manifest = "sha256-" + digest_hex( hasher.finish() );
    stats = result_stats;
    error.clear();
    return true;
}

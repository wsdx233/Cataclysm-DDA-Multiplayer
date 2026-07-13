#pragma once
#ifndef CATA_SRC_MULTIPLAYER_CRYPTO_H
#define CATA_SRC_MULTIPLAYER_CRYPTO_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

bool multiplayer_fill_secure_random( std::uint8_t *output, std::size_t size,
                                     std::string &error );
bool multiplayer_generate_bearer_token( std::string &token, std::string &error );
bool multiplayer_generate_uuid_v4( std::string &uuid, std::string &error );
bool multiplayer_is_valid_bearer_token( const std::string &token );
bool multiplayer_constant_time_token_equal( const std::string &left, const std::string &right );
bool multiplayer_write_private_file_exclusive( const std::filesystem::path &path,
        const std::string &contents, std::string &error );

#endif // CATA_SRC_MULTIPLAYER_CRYPTO_H

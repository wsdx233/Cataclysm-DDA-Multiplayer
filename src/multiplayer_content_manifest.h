#pragma once
#ifndef CATA_SRC_MULTIPLAYER_CONTENT_MANIFEST_H
#define CATA_SRC_MULTIPLAYER_CONTENT_MANIFEST_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/** One ordered gameplay-content root included in the multiplayer compatibility hash. */
struct multiplayer_content_root {
    std::string id;
    std::filesystem::path path;
};

struct multiplayer_content_manifest_stats {
    std::uint64_t file_count = 0;
    std::uint64_t byte_count = 0;
};

/**
 * Builds a deterministic SHA-256 manifest from ordered content roots.
 *
 * Root IDs, normalized relative file names, file lengths, and file bytes are
 * hashed.  Roots retain caller order while files inside each root are sorted.
 * Symbolic links and non-regular filesystem entries are rejected so the hash
 * cannot silently depend on files outside a configured content root.
 */
bool multiplayer_build_content_manifest(
    const std::vector<multiplayer_content_root> &roots,
    std::string &manifest,
    multiplayer_content_manifest_stats &stats,
    std::string &error );

#endif // CATA_SRC_MULTIPLAYER_CONTENT_MANIFEST_H

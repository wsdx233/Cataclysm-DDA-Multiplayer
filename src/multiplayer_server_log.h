#pragma once
#ifndef CATA_SRC_MULTIPLAYER_SERVER_LOG_H
#define CATA_SRC_MULTIPLAYER_SERVER_LOG_H

#include <chrono>
#include <string>
#include <utility>
#include <vector>

enum class multiplayer_server_log_severity {
    debug,
    info,
    warning,
    error
};

using multiplayer_server_log_fields = std::vector<std::pair<std::string, std::string>>;

/** Builds one newline-free JSON log record.  Field values are always JSON strings. */
std::string multiplayer_server_log_json(
    multiplayer_server_log_severity severity,
    const std::string &event,
    const multiplayer_server_log_fields &fields = {},
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now() );

#endif // CATA_SRC_MULTIPLAYER_SERVER_LOG_H

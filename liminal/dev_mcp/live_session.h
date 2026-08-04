#pragma once

#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/types.hpp>

namespace liminal::dev_mcp {

using namespace lighter::types;

struct LiveSnapshot {
    std::string driver = "liminal.pty";
    i32 columns = 0;
    i32 rows = 0;
    bool running = false;
    std::optional<i32> exit_code;
    u64 process_id = 0;
    u64 output_offset = 0;
    std::string output_encoding = "escaped-control-bytes";
    std::string output;
    std::vector<std::string> visible_text;
    i32 cursor_row = 0;
    i32 cursor_column = 0;
    bool cursor_visible = false;
};

struct LiveSession {
    static std::expected<std::unique_ptr<LiveSession>, std::string> create(std::string_view working_directory, i32 columns, i32 rows);

    ~LiveSession();

    LiveSession(const LiveSession &) = delete;
    LiveSession &operator=(const LiveSession &) = delete;
    LiveSession(LiveSession &&) = delete;
    LiveSession &operator=(LiveSession &&) = delete;

    std::expected<void, std::string> write(std::string_view bytes);
    std::expected<void, std::string> key(std::string_view name);
    std::expected<void, std::string> resize(i32 columns, i32 rows);
    std::expected<void, std::string> wait(std::chrono::milliseconds duration);
    std::expected<void, std::string> terminate();
    std::expected<LiveSnapshot, std::string> inspect();

private:
    struct Impl;

    explicit LiveSession(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl;
};

} // namespace liminal::dev_mcp

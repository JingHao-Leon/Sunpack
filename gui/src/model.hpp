#pragma once
#include "platform.hpp"
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace spg {
using Json = nlohmann::ordered_json;
Json parse_json(const std::string& text);
Json read_json(const fs::path& path);
void validate_passwords(const Json& document);
void save_config_field(const fs::path& path, const std::string& pointer, const std::string& original,
    const std::string& type, const std::string& text, bool boolean);
// original is the exact selected record, not an index that external edits can move.
void edit_password(const fs::path& path, const std::string& operation, const Json& original,
    const Json& replacement, const std::string& scope = {});
struct WatchRoot { fs::path input; std::string output; bool deep = false; };
struct WatchRoots { std::vector<WatchRoot> entries; std::vector<std::string> comments; };
WatchRoots parse_roots(const std::string& text, const fs::path& base);
WatchRoots read_roots(const fs::path& path);
void edit_root(const fs::path& path, const std::string& operation, const std::string& original,
    const std::string& input, const std::string& output, bool deep);
std::string root_for(const fs::path& path, const WatchRoots& roots);
struct Task {
    std::string path, status, time;
    double timestamp = 0;
    uint64_t sequence = 0;
    Json details, cleanup;
};
class EventLog {
    fs::path path_;
    uint64_t offset_ = 0, identity_ = 0, sequence_ = 0;
    DWORD volume_ = 0;
    std::string prefix_, anchor_, pending_;
public:
    std::unordered_map<std::wstring, Task> tasks;
    size_t malformed = 0;
    uint64_t bytes_parsed = 0; // Diagnostic counter, never persisted.
    bool retry_required = false;
    void set_path(const fs::path& path);
    bool refresh(bool missing_is_final = false);
    void accept(const Json& event);
    const fs::path& path() const { return path_; }
};
struct View {
    std::string task_query, task_root, task_status, password_query, password_scope = "@global";
    size_t task_page = 0, password_page = 0;
};
Json task_rows(const EventLog& log, const WatchRoots& roots, const View& view);
Json password_rows(const Json& passwords, const View& view);
Json config_fields(const Json& config, const std::string& source);
Json directory_rows(const WatchRoots& roots, const Json& passwords, const Json& config);
fs::path log_directory(const fs::path& data_dir, const Json& config);
}

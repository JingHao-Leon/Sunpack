#pragma once
#include "model.hpp"
#include <ui_core.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <atomic>

namespace spg {
struct Options {
    fs::path data_dir;
    CliCommand cli;
    bool allow_cli = true;
};
class Application {
    Options options_;
    UiPage page_ = 0;
    UiWindow window_ = 0;
    HWND hwnd_ = nullptr;
    Handle stop_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Json> commands_;
    Json pending_ = Json::object();
    bool notified_ = false, stopping_ = false;
    unsigned dirty_ = 15;
    std::thread worker_, registry_;
    void work();
    void publish(Json patch);
    void changed(unsigned flags);
    void dispatch();
    void apply_pending();
    static LRESULT CALLBACK subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
public:
    explicit Application(Options options) : options_(std::move(options)) {}
    ~Application();
    void open();
    int run();
    UiPage page() const { return page_; }
    UiWindow window() const { return window_; }
};
Options parse_options(const std::vector<std::wstring>& args);
}

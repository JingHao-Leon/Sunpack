#include "application.hpp"
#include "main_uix.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <chrono>
#include <map>

namespace spg {
namespace {
constexpr UINT Dispatch = WM_APP + 101, Update = WM_APP + 102;
constexpr unsigned Config = 1, Passwords = 2, Roots = 4, Log = 8, Startup = 16;
using Clock=std::chrono::steady_clock;
struct ReadRetry {
    unsigned attempts=0;
    Clock::time_point due=Clock::time_point::max();
};
UiThemeMode system_theme() {
    DWORD light=1, size=sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&size);
    return light?UI_THEME_LIGHT:UI_THEME_DARK;
}
Json page_value(UiPage page,const char* name) {
    char* value=ui_page_get_json(page,name);
    if(!value) return Json();
    std::string text(value); ui_page_free(value); return parse_json(text);
}
void set_text(UiPage page,const char* name,const std::string& value) {
    auto encoded=Json(value).dump(); ui_page_set_json(page,name,encoded.c_str());
}
void style_frame(HWND hwnd,UiThemeMode mode) {
    BOOL dark=mode==UI_THEME_DARK;
    COLORREF background=dark?RGB(32,32,32):RGB(243,243,243);
    COLORREF foreground=dark?RGB(255,255,255):RGB(26,26,26);
    auto corners=DWMWCP_ROUND;
    // These optional Win11 attributes fail harmlessly on older Windows builds.
    DwmSetWindowAttribute(hwnd,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    DwmSetWindowAttribute(hwnd,DWMWA_CAPTION_COLOR,&background,sizeof(background));
    DwmSetWindowAttribute(hwnd,DWMWA_TEXT_COLOR,&foreground,sizeof(foreground));
    DwmSetWindowAttribute(hwnd,DWMWA_WINDOW_CORNER_PREFERENCE,&corners,sizeof(corners));
}
void apply_theme(UiPage page,UiThemeMode mode,HWND hwnd=nullptr) {
    ui_theme_set_mode(mode);
    set_text(page,"appearance",mode==UI_THEME_DARK?"dark":"light");
    if(hwnd) style_frame(hwnd,mode);
}
}
Options parse_options(const std::vector<std::wstring>& args) {
    Options result; auto exe=executable_path().parent_path();
    bool custom=false;
    for(size_t i=0;i<args.size();++i) {
        if(args[i]==L"--data-dir" && i+1<args.size()) { result.data_dir=resolve_path(args[++i]); custom=true; }
        else if(args[i]==L"--cli" && i+1<args.size()) result.cli.executable=resolve_path(args[++i]);
        else if(args[i]==L"--cli-prefix" && i+1<args.size()) result.cli.prefix.push_back(args[++i]);
        else throw std::runtime_error("参数：--data-dir <目录> --cli <可执行文件> [--cli-prefix <单个参数>]...");
    }
    if(result.data_dir.empty()) {
        PWSTR dir=nullptr;
        if(FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData,0,nullptr,&dir))) throw std::runtime_error("无法确定 ProgramData");
        result.data_dir=fs::path(dir)/"SunPack"; CoTaskMemFree(dir);
    }
    // A fixture data directory cannot control the installed runtime accidentally.
    result.allow_cli=!custom || !result.cli.executable.empty();
    if(result.cli.executable.empty()) result.cli.executable=exe/"bin"/"sunpack.exe";
    result.cli.cwd=result.data_dir;
    return result;
}
void Application::changed(unsigned flags) {
    { std::lock_guard<std::mutex> lock(mutex_); if(stopping_) return; dirty_|=flags; }
    cv_.notify_one();
}
void Application::publish(Json patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(stopping_) return;
    for(auto it=patch.begin();it!=patch.end();++it) pending_[it.key()]=std::move(it.value());
    if(!notified_) { notified_=true; PostMessageW(hwnd_,Update,0,0); }
}
void Application::apply_pending() {
    Json patch;
    { std::lock_guard<std::mutex> lock(mutex_); patch=std::move(pending_); pending_=Json::object(); notified_=false; }
    for(auto it=patch.begin();it!=patch.end();++it) {
        auto text=it.value().dump();
        if(ui_page_set_json(page_,it.key().c_str(),text.c_str())!=0) throw std::runtime_error("Core UI 数据绑定失败");
    }
}
void Application::dispatch() {
    auto command=page_value(page_,"request");
    ui_page_set_bool(page_,"dispatch",false);
    if(!command.is_object()) return;
    auto op=command.value("op",std::string());
    auto p=command.value("payload",Json::object());
    if(op=="browse") {
        auto selected=choose_directory(hwnd_);
        if(!selected.empty()) {
            auto target=p.value("target",std::string());
            if(target=="scopeText") {
                auto text=page_value(page_,"scopeText").get<std::string>();
                if(!text.empty() && text.back()!='\n') text+='\n';
                text+=utf8(selected.wstring()); set_text(page_,"scopeText",text);
            } else if(target=="rootInput" || target=="rootOutput" || target=="configText") set_text(page_,target.c_str(),utf8(selected.wstring()));
        }
        return;
    }
    if(op=="openPath") {
        auto path=wide(p.value("path",std::string()));
        if(!path.empty() && fs::exists(path)) {
            auto folder=fs::is_directory(path)?fs::path(path):fs::path(path).parent_path();
            PIDLIST_ABSOLUTE parent=ILCreateFromPathW(folder.c_str()), item=ILCreateFromPathW(path.c_str());
            HRESULT result=E_FAIL;
            if(parent && item) {
                PCUITEMID_CHILD child=ILFindLastID(item);
                result=SHOpenFolderAndSelectItems(parent,fs::is_directory(path)?0:1,&child,0);
            }
            CoTaskMemFree(item); CoTaskMemFree(parent);
            if(FAILED(result)) throw std::runtime_error("无法在资源管理器中打开路径");
        } else throw std::runtime_error("路径当前不存在");
        return;
    }
    { std::lock_guard<std::mutex> lock(mutex_); commands_.push_back(std::move(command)); }
    cv_.notify_one();
}
LRESULT CALLBACK Application::subclass(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR data) {
    auto* self=reinterpret_cast<Application*>(data);
    try {
        if(message==Dispatch) { self->dispatch(); return 0; }
        if(message==Update) { self->apply_pending(); return 0; }
        if(message==WM_ACTIVATE && LOWORD(wparam)!=WA_INACTIVE) self->changed(Startup);
        if(message==WM_SETTINGCHANGE && page_value(self->page_,"theme")=="system") apply_theme(self->page_,system_theme(),self->hwnd_);
    } catch(const std::exception& e) { set_text(self->page_,"notice",e.what()); }
    return DefSubclassProc(hwnd,message,wparam,lparam);
}
void Application::open() {
    const char* stage="初始化渲染器";
    try {
    ui_init_with_theme(system_theme());
    stage="设置中文字体";
    ui_theme_set_cjk_font(L"Segoe UI Variable Text",L"Microsoft YaHei UI");
    stage="加载布局";
    page_=ui_page_load_string(kMainUix);
    if(!page_) throw std::runtime_error("无法加载管理面板布局");
    apply_theme(page_,system_theme());
    stage="创建窗口";
    UiWindowConfig window_config{}; window_config.skip_animation=1;
    window_=ui_page_prepare_window(page_,&window_config);
    if(!window_) throw std::runtime_error(ui_page_last_error(page_));
    stage="绑定窗口事件";
    hwnd_=static_cast<HWND>(ui_window_hwnd(window_));
    style_frame(hwnd_,system_theme());
    auto icon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1));
    SendMessageW(hwnd_,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(icon));
    SendMessageW(hwnd_,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(icon));
    SetPropW(hwnd_,mutex_name(options_.data_dir,L"SunPack.GUI").c_str(),reinterpret_cast<HANDLE>(1));
    if(!SetWindowSubclass(hwnd_,subclass,1,reinterpret_cast<DWORD_PTR>(this))) win_error("GUI subclass");
    ui_page_on_widget_mount(page_,"dispatch",[](UiPage,UiWidget,void* context){
        auto* self=static_cast<Application*>(context); PostMessageW(self->hwnd_,Dispatch,0,0);
    },this);
    ui_page_on_widget_unmount(page_,"dispatch",[](UiPage,UiWidget widget,void*){
        // The lifecycle API registers an owning handle; release it on every dispatch.
        if(widget) ui_widget_destroy(widget);
    },nullptr);
    ui_page_on_widget_mount(page_,"taskDetails",[](UiPage,UiWidget widget,void*){
        ui_text_area_set_read_only(widget,true);
    },nullptr);
    ui_page_on_widget_mount(page_,"themeSelector",[](UiPage,UiWidget widget,void* context){
        // Core UI's QuickJS @change hook does not receive ComboBox selections.
        ui_combobox_on_changed(widget,[](UiWidget,int index,void* context){
            auto* self=static_cast<Application*>(context);
            try {
                ui_page_set_int(self->page_,"themeChoice",index);
                set_text(self->page_,"theme",index==1?"light":index==2?"dark":"system");
                apply_theme(self->page_,index==1?UI_THEME_LIGHT:index==2?UI_THEME_DARK:system_theme(),self->hwnd_);
            } catch(const std::exception& e) { set_text(self->page_,"notice",e.what()); }
        },context);
    },this);
    ui_window_on_close(window_,[](UiWindow,void* context){
        auto* self=static_cast<Application*>(context);
        { std::lock_guard<std::mutex> lock(self->mutex_); self->stopping_=true; }
        SetEvent(self->stop_); self->cv_.notify_all(); ui_quit(0);
    },this);
    set_text(page_,"dataDirectory",utf8(options_.data_dir.wstring()));
    ui_page_set_bool(page_,"cliAllowed",options_.allow_cli);
    ui_window_show_immediate(window_);
    stage="启动文件观察器";
    worker_=std::thread([this]{ work(); });
    registry_=std::thread([this]{
        HKEY key{};
        // Observe the existing Run key; do not create startup entries merely by opening the UI.
        if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_NOTIFY,&key)!=ERROR_SUCCESS) return;
        Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        HANDLE waits[]={stop_,event};
        while(RegNotifyChangeKeyValue(key,FALSE,REG_NOTIFY_CHANGE_LAST_SET,event,TRUE)==ERROR_SUCCESS) {
            if(WaitForMultipleObjects(2,waits,FALSE,INFINITE)!=WAIT_OBJECT_0+1) break;
            ResetEvent(event); changed(Startup);
        }
        RegCloseKey(key);
    });
    } catch(const std::exception& e) { throw std::runtime_error(std::string(stage)+"："+e.what()); }
}
int Application::run() { open(); return ui_run(); }
Application::~Application() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_=true; }
    SetEvent(stop_); cv_.notify_all();
    if(worker_.joinable()) worker_.join();
    if(registry_.joinable()) registry_.join();
    if(hwnd_ && IsWindow(hwnd_)) { RemoveWindowSubclass(hwnd_,subclass,1); ui_window_destroy(window_); }
    if(page_) ui_page_destroy(page_);
    ui_shutdown();
}
void Application::work() {
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    try {
        Json config=Json::object(), passwords={{"passwords",Json::array()}};
        WatchRoots roots; EventLog log; View view;
        Json errors=Json::object();
        auto dir=options_.data_dir;
        auto on_data=[this,dir](const fs::path& path){
            auto name=path.filename().wstring(); CharLowerBuffW(name.data(),DWORD(name.size()));
            if(!path.empty() && is_under(path_key(dir),path_key(path))) { changed(Config|Passwords|Roots); return; }
            if(name.empty() || name==L"sunpack_config.json") changed(Config);
            if(name.empty() || name==L"builtin_passwords.json") changed(Passwords);
            if(name.empty() || name==L"sunpack_watch_roots.txt") changed(Roots);
        };
        std::unique_ptr<DirectoryWatcher> data_watcher;
        std::unique_ptr<DirectoryWatcher> log_watcher;
        std::string last_config,last_passwords,last_roots;
        bool config_read=false,passwords_read=false,roots_read=false;
        Json previous_patch=Json::object();
        std::map<unsigned,ReadRetry> retries;
        auto retry_io=[&](unsigned bit,const WinError& error) {
            if(error.code!=ERROR_SHARING_VIOLATION && error.code!=ERROR_LOCK_VIOLATION && error.code!=ERROR_FILE_NOT_FOUND && error.code!=ERROR_PATH_NOT_FOUND) return;
            auto& retry=retries[bit];
            // A rename notification can precede the writer releasing its sharing lock.
            // Bounded one-shot deadlines exist only after an observed I/O failure.
            constexpr unsigned delays[]={5,20,80};
            if(retry.attempts<3) retry.due=Clock::now()+std::chrono::milliseconds(delays[retry.attempts++]);
        };
        for(;;) {
            unsigned flags; std::deque<Json> commands;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                auto deadline=Clock::time_point::max();
                for(auto& retry:retries) deadline=std::min(deadline,retry.second.due);
                auto awake=[&]{return stopping_ || dirty_ || !commands_.empty();};
                if(deadline==Clock::time_point::max()) cv_.wait(lock,awake);
                else cv_.wait_until(lock,deadline,awake);
                if(stopping_) break;
                flags=dirty_; dirty_=0; commands.swap(commands_);
            }
            for(auto& retry:retries) {
                if(flags&retry.first) { retry.second=ReadRetry{}; }
                else if(retry.second.due<=Clock::now()) { flags|=retry.first; retry.second.due=Clock::time_point::max(); }
            }
            bool tasks_changed=false,passwords_changed=false,dirs_changed=false,config_changed=false;
            if(!data_watcher || !data_watcher->alive() || (!data_watcher->direct() && fs::is_directory(dir))) {
                try { data_watcher=std::make_unique<DirectoryWatcher>(dir,on_data); errors.erase("文件观察器"); }
                catch(const std::exception& e) { errors["文件观察器"]=e.what(); }
            }
            for(auto& command:commands) {
                auto op=command.value("op",std::string()); auto p=command.value("payload",Json::object());
                try {
                    if(op=="filterTasks") {
                        view.task_query=p.value("query",std::string()); view.task_root=p.value("root",std::string());
                        view.task_status=p.value("status",std::string()); view.task_page=p.value("page",size_t(0)); tasks_changed=true;
                    } else if(op=="filterPasswords") {
                        view.password_query=p.value("query",std::string()); view.password_scope=p.value("scope",std::string("@global"));
                        if(view.password_scope.empty()) view.password_scope="@global";
                        view.password_page=p.value("page",size_t(0)); passwords_changed=true;
                    } else if(op=="refresh") { flags|=Config|Passwords|Roots|Log|Startup; retries.clear(); }
                    else if(op=="saveConfig") {
                        save_config_field(dir/"sunpack_config.json",p.at("pointer"),p.at("original"),p.at("type"),p.at("text"),p.at("boolean")); flags|=Config;
                    } else if(op=="password") {
                        Json replacement={{"value",p.value("value",std::string())}};
                        auto scopes=p.value("scopes",Json::array());
                        if(!scopes.empty()) replacement["scopes"]=scopes;
                        auto original=p.value("original",std::string());
                        edit_password(dir/"builtin_passwords.json",p.at("operation"),original.empty()?Json():parse_json(original),replacement,p.value("scope",std::string())); flags|=Passwords;
                    } else if(op=="root") {
                        edit_root(dir/"sunpack_watch_roots.txt",p.at("operation"),p.value("original",std::string()),p.at("input"),p.at("output"),p.at("deep")); flags|=Roots;
                    } else if(op=="cli") {
                        if(!options_.allow_cli) throw std::runtime_error("测试数据目录未绑定 CLI；请显式传入 --cli");
                        auto action=p.value("action",std::string());
                        std::vector<std::wstring> args={L"watch"};
                        if(action=="start" || action=="stop" || action=="reload" || action=="status") args.push_back(wide(action));
                        else if(action=="startupEnable" || action=="startupDisable") { args.push_back(L"startup"); args.push_back(action=="startupEnable"?L"enable":L"disable"); }
                        else throw std::runtime_error("不支持的 CLI 操作");
                        publish({{"busy",true},{"notice","正在执行 CLI 命令…"}});
                        auto result=run_cli(options_.cli,args,stop_);
                        auto message="CLI 退出码 "+std::to_string(result.code)+"\n"+result.output;
                        // The CLI normally emits UTF-8; malformed output must not break the UI.
                        try { wide(message); } catch(...) { message="CLI 退出码 "+std::to_string(result.code)+"（输出编码无效）"; }
                        publish({{"busy",false},{"notice",message}}); flags|=Startup;
                    } else throw std::runtime_error("未知界面操作");
                    if(op=="saveConfig" || op=="password" || op=="root") publish({{"notice","已保存；正在回读实际文件。"}});
                } catch(const std::exception& e) { publish({{"busy",false},{"notice",e.what()}}); }
            }
            auto read=[&](unsigned bit,const char* file,std::string& previous,bool& loaded,auto apply) {
                if(!(flags&bit)) return;
                try {
                    auto text=read_file(dir/file);
                    if(!loaded || text!=previous) { apply(text); previous=std::move(text); loaded=true; }
                    errors.erase(file);
                    retries.erase(bit);
                } catch(const WinError& e) {
                    if(bit==Roots && (e.code==ERROR_FILE_NOT_FOUND || e.code==ERROR_PATH_NOT_FOUND) && retries.count(bit) && retries.at(bit).attempts>=3) {
                        apply(std::string()); previous.clear(); loaded=true; errors.erase(file); retries.erase(bit);
                    } else { errors[file]=e.what(); retry_io(bit,e); }
                }
                catch(const std::exception& e) { errors[file]=e.what(); }
            };
            read(Config,"sunpack_config.json",last_config,config_read,[&](const std::string& text){
                auto value=parse_json(text); if(!value.is_object()) throw std::runtime_error("配置顶层必须是 JSON 对象");
                config=std::move(value); config_changed=dirs_changed=true;
            });
            read(Passwords,"builtin_passwords.json",last_passwords,passwords_read,[&](const std::string& text){
                auto value=parse_json(text); validate_passwords(value); passwords=std::move(value); passwords_changed=dirs_changed=true;
            });
            if(flags&Roots) {
                read(Roots,"sunpack_watch_roots.txt",last_roots,roots_read,[&](const std::string& text){roots=parse_roots(text,dir); tasks_changed=dirs_changed=true;});
            }
            auto log_dir=log_directory(dir,config);
            if(log.path()!=log_dir/"events.jsonl" || !log_watcher || !log_watcher->alive() || (!log_watcher->direct() && fs::is_directory(log_dir))) {
                log_watcher.reset();
                try {
                    log_watcher=std::make_unique<DirectoryWatcher>(log_dir,[this](const fs::path& path){
                        auto name=path.filename().wstring(); CharLowerBuffW(name.data(),DWORD(name.size()));
                        std::error_code ec;
                        if(path.empty() || name==L"events.jsonl" || fs::is_directory(path,ec)) changed(Log);
                    }); errors.erase("日志观察器");
                } catch(const std::exception& e) { errors["日志观察器"]=e.what(); }
                log.set_path(log_dir/"events.jsonl"); flags|=Log; tasks_changed=true;
            }
            if(flags&Log) {
                try { tasks_changed|=log.refresh(retries.count(Log) && retries.at(Log).attempts>=3); if(log.retry_required) changed(Log); errors.erase("events.jsonl"); retries.erase(Log); }
                catch(const WinError& e){ errors["events.jsonl"]=e.what(); retry_io(Log,e); }
                catch(const std::exception& e){ errors["events.jsonl"]=e.what(); }
            }
            Json patch=Json::object();
            if(tasks_changed || !previous_patch.contains("tasks")) patch["tasks"]=task_rows(log,roots,view);
            if(passwords_changed || !previous_patch.contains("passwords")) patch["passwords"]=password_rows(passwords,view);
            if(config_changed || !previous_patch.contains("fields")) patch["fields"]=config_fields(config,last_config);
            if(dirs_changed || !previous_patch.contains("directories")) patch["directories"]=directory_rows(roots,passwords,config);
            if(flags&Startup || !previous_patch.contains("startup")) {
                try { auto state=read_startup(); patch["startup"]={{"enabled",state.enabled},{"command",state.command}}; errors.erase("startup"); }
                catch(const std::exception& e){errors["startup"]=e.what();}
            }
            patch["errors"]=errors; patch["logPath"]=utf8(log.path().wstring()); patch["ready"]=true;
            Json changed_patch=Json::object();
            for(auto it=patch.begin();it!=patch.end();++it) if(!previous_patch.contains(it.key()) || previous_patch[it.key()]!=it.value()) {
                previous_patch[it.key()]=it.value(); changed_patch[it.key()]=std::move(it.value());
            }
            if(!changed_patch.empty()) publish(std::move(changed_patch));
        }
    } catch(const std::exception& e) { publish({{"notice",std::string("文件观察器停止：")+e.what()},{"ready",true}}); }
    CoUninitialize();
}
}

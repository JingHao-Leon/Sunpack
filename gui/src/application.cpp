#include "application.hpp"
#include "motion.hpp"
#include "presentation.hpp"
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
// Keep host messages outside Core UI's startup/render message range.
constexpr UINT Dispatch = WM_APP + 0x401, Update = WM_APP + 0x402;
constexpr unsigned Config = 1, Passwords = 2, Roots = 4, Log = 8, Startup = 16;
using Clock=std::chrono::steady_clock;
struct VisualBatch {
    UiWindow window;
    explicit VisualBatch(UiWindow w):window(w) { ui_window_begin_canvas_visual_transaction(w); }
    ~VisualBatch() { ui_window_end_canvas_visual_transaction(window); }
};
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
    BOOL disable_transitions=FALSE;
    DwmSetWindowAttribute(hwnd,DWMWA_TRANSITIONS_FORCEDISABLED,&disable_transitions,sizeof(disable_transitions));
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
    auto appearance=mode==UI_THEME_DARK?"dark":"light";
    if(page_value(page,"appearance")==appearance) return;
    ui_theme_set_mode(mode);
    set_text(page,"appearance",appearance);
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
    auto transition=patch.value("viewTransition",Json::object()); patch.erase("viewTransition");
    VisualBatch visual(window_);
    if(patch.contains("stagedEdit")) {
        const auto& staged=patch["stagedEdit"];
        bool password=staged["kind"]=="passwords";
        auto key=password?"passwordEdits":"rootEdits";
        auto edits=page_value(page_,key); edits.push_back(staged["edit"]);
        auto encoded=edits.dump(); ui_page_set_json(page_,key,encoded.c_str());
        for(auto it=staged["nextState"].begin();it!=staged["nextState"].end();++it) {
            auto value=it.value().dump(); ui_page_set_json(page_,it.key().c_str(),value.c_str());
        }
        if(password && page_value(page_,"passwordEditor")==false) {
            for(auto name:{"passwordValue","passwordOriginal","scopeText","passwordBaseline"}) set_text(page_,name,"");
        }
        if(staged["nextState"].value(password?"passwordEditor":"rootEditor",false)) reveal_form(password);
        patch.erase("stagedEdit");
    }
    if(patch.contains("batchAck")) {
        auto ack=patch["batchAck"], submitted=ack["payload"];
        auto saved=[&](const char* section){return std::find(ack["saved"].begin(),ack["saved"].end(),section)!=ack["saved"].end();};
        for(auto name:{"drafts","toggleDrafts"}) {
        auto drafts=page_value(page_,name);
        if(saved("config")) for(auto& edit:submitted["fields"]) {
            auto id=edit["pointer"].get<std::string>();
            if(!drafts.contains(id)) continue;
            auto& draft=drafts[id];
            auto submitted=edit["draft"];
            if(draft==submitted) drafts.erase(id);
            else if(draft["original"]==submitted["original"]) {
                // A user may keep typing while the write completes. Keep those
                // edits, advancing only the token written by our own save.
                draft["original"]=edit["type"]=="string"?Json(edit["text"]).dump():edit["type"]=="boolean"?Json(edit["boolean"]).dump():edit["text"].get<std::string>();
                draft["baselineText"]=edit["draft"]["text"];
                draft["baselineBoolean"]=edit["draft"]["boolean"];
            }
        }
        auto encoded=drafts.dump(); ui_page_set_json(page_,name,encoded.c_str());
        }
        for(auto section:{"passwords","roots"}) if(saved(section)) {
            bool password=std::string(section)=="passwords";
            auto key=password?"passwordEdits":"rootEdits";
            auto current=page_value(page_,key); const auto& prefix=submitted[key];
            if(current.size()>=prefix.size() && std::equal(prefix.begin(),prefix.end(),current.begin())) current.erase(current.begin(),current.begin()+prefix.size());
            auto encoded=current.dump(); ui_page_set_json(page_,key,encoded.c_str());
            if(!submitted[password?"passwordForm":"rootForm"].is_null()) {
                ui_page_set_bool(page_,password?"passwordEditor":"rootEditor",false);
                if(password) for(auto name:{"passwordValue","passwordOriginal","scopeText","passwordBaseline"}) set_text(page_,name,"");
            }
        }
        ui_page_set_int(page_,"saveGeneration",page_value(page_,"saveGeneration").get<int>()+1);
        patch.erase("batchAck");
    }
    if(patch.contains("fields")) bind_controls(patch["fields"]);
    for(auto it=patch.begin();it!=patch.end();++it) {
        auto text=it.value().dump();
        if(ui_page_set_json(page_,it.key().c_str(),text.c_str())!=0) throw std::runtime_error("Core UI 数据绑定失败");
    }
    if(patch.contains("fields")) {
        auto drafts=page_value(page_,"drafts");
        for(auto it=controls_.begin();it!=controls_.end();) {
            bool exists=std::any_of(patch["fields"].begin(),patch["fields"].end(),[&](const Json& f){return f["id"]==it->first;});
            if(!exists) {
                auto host=(it->second->field["editor"]=="toggle"?"toggle-host-":"choice-")+it->first;
                ui_page_on_widget_mount(page_,host.c_str(),nullptr,nullptr);
                ui_page_on_widget_unmount(page_,host.c_str(),nullptr,nullptr);
                it=controls_.erase(it); continue;
            }
            if(it->second->field["editor"]=="toggle") { ++it; continue; }
            auto widget=ui_widget_find_by_id(ui_page_root(page_),("select-"+it->first).c_str());
            if(widget) {
                if(!drafts.contains(it->first)) ui_combobox_set_selected(widget,it->second->field["choice"].get<int>());
                ui_widget_destroy(widget);
            }
            ++it;
        }
    }
    if(patch.contains("fields") || patch.contains("saving")) sync_toggles();
    if(!transition.empty()) {
        ui_window_relayout(window_);
        motion::enter(window_,transition.at("id").get<std::string>().c_str(),transition.value("x",0.0f),transition.value("y",0.0f));
    }
    if(patch.contains("language")) {
        auto language=patch["language"].get<std::string>();
        ui_window_set_title(window_,wide(tr("title",language)).c_str());
        rebuild_theme(language);
    }
}
void Application::navigate(const std::string& tab) {
    if(tab!="tasks" && tab!="config" && tab!="passwords") return;
    if(page_value(page_,"tab")==tab) return;
    VisualBatch visual(window_);
    motion::finish(window_,("page-"+page_value(page_,"tab").get<std::string>()).c_str());
    set_text(page_,"tab",tab);
    set_text(page_,"actionError","");
    ui_window_relayout(window_);
    motion::enter(window_,("page-"+tab).c_str());
    motion::select(window_,"navIndicator",tab=="tasks"?"tabTasks":tab=="config"?"tabConfig":"tabPasswords");
}
void Application::sync_toggles() {
    auto drafts=page_value(page_,"toggleDrafts");
    for(auto& pair:controls_) if(pair.second->field["editor"]=="toggle") {
        auto widget=ui_widget_find_by_id(ui_page_root(page_),("toggle-"+pair.first).c_str());
        if(!widget) continue;
        bool on=drafts.contains(pair.first)?drafts[pair.first]["boolean"].get<bool>():pair.second->field["boolean"].get<bool>();
        ui_toggle_set_on(widget,on); ui_widget_destroy(widget);
    }
}
void Application::reveal_form(bool password) {
    ui_window_relayout(window_);
    auto scroll=ui_widget_find_by_id(ui_page_root(page_),password?"passwordListScroll":"taskListScroll");
    // Core UI exposes programmatic scrolling through this public entry point;
    // it only adjusts the viewport and does not start a debug server.
    if(scroll) { ui_debug_scroll_set(scroll,0); ui_widget_destroy(scroll); }
    auto input=ui_widget_find_by_id(ui_page_root(page_),password?"passwordValue":"rootInput");
    if(input) { ui_window_focus_widget(window_,input); ui_widget_destroy(input); }
    motion::enter(window_,password?"passwordForm":"rootForm",0,12);
}
void Application::bind_controls(const Json& fields) {
    for(auto& field:fields) {
        auto editor=field.value("editor",std::string());
        if(editor!="choice" && editor!="recursive" && editor!="toggle") continue;
        auto id=field["id"].get<std::string>();
        auto& binding=controls_[id];
        if(!binding) binding=std::make_unique<ControlBinding>(ControlBinding{this,field});
        else binding->field=field;
        auto host=(editor=="toggle"?"toggle-host-":"choice-")+id;
        ui_page_on_widget_mount(page_,host.c_str(),[](UiPage,UiWidget parent,void* context){
            auto* binding=static_cast<ControlBinding*>(context); auto* self=binding->owner;
            auto id=binding->field["id"].get<std::string>();
            bool toggle=binding->field["editor"]=="toggle";
            // Nested scope mounting visits descendants again when its parent
            // mounts. One native selector belongs to each host for its lifetime.
            auto existing=ui_widget_find_by_id(parent,((toggle?"toggle-":"select-")+id).c_str());
            if(existing) { ui_widget_destroy(existing); return; }
            if(toggle) {
                auto widget=ui_toggle(L""); ui_widget_set_id(widget,("toggle-"+id).c_str());
                ui_widget_set_width(widget,44); ui_widget_set_height(widget,26);
                auto drafts=page_value(self->page_,"toggleDrafts");
                ui_toggle_set_on_immediate(widget,drafts.contains(id)?drafts[id]["boolean"].get<bool>():binding->field["boolean"].get<bool>());
                ui_toggle_on_changed(widget,[](UiWidget,int value,void* context){
                    auto* binding=static_cast<ControlBinding*>(context); auto* self=binding->owner;
                    try {
                    auto id=binding->field["id"].get<std::string>();
                    // A switch owns its finite animation. Keep its draft channel
                    // separate so a click doesn't rebind every text/list control.
                    auto drafts=page_value(self->page_,"toggleDrafts");
                    Json draft=drafts.contains(id)?drafts[id]:Json{{"original",binding->field["original"]},{"text",binding->field["text"]},{"boolean",binding->field["boolean"]},{"baselineText",binding->field["text"]},{"baselineBoolean",binding->field["boolean"]}};
                    draft["boolean"]=value!=0;
                    if(draft["boolean"]==draft["baselineBoolean"]) drafts.erase(id); else drafts[id]=std::move(draft);
                    auto encoded=drafts.dump(); ui_page_set_json(self->page_,"toggleDrafts",encoded.c_str());
                    if(page_value(self->page_,"saveError")!="") set_text(self->page_,"saveError","");
                    } catch(const std::exception& e) { set_text(self->page_,"saveError",translated_error(e.what(),page_value(self->page_,"language").get<std::string>())); }
                },binding);
                ui_widget_add_child(parent,widget); ui_widget_destroy(widget); return;
            }
            std::vector<std::wstring> labels; std::vector<const wchar_t*> texts;
            for(auto& option:binding->field["options"]) labels.push_back(wide(option["label"].get<std::string>()));
            for(auto& label:labels) texts.push_back(label.c_str());
            auto combo=ui_combobox(texts.data(),int(texts.size()));
            ui_widget_set_id(combo,("select-"+id).c_str()); ui_widget_set_expand(combo,true); ui_widget_set_height(combo,32);
            auto drafts=page_value(self->page_,"drafts");
            int selected=drafts.contains(id)?drafts[id].value("choice",binding->field["choice"].get<int>()):binding->field["choice"].get<int>();
            ui_combobox_set_selected(combo,selected);
            ui_combobox_on_changed(combo,[](UiWidget,int index,void* context){
                auto* binding=static_cast<ControlBinding*>(context); auto* self=binding->owner;
                try {
                    auto id=binding->field["id"].get<std::string>();
                    auto drafts=page_value(self->page_,"drafts");
                    Json draft=drafts.contains(id)?drafts[id]:Json{{"original",binding->field["original"]},{"text",binding->field["text"]},{"boolean",binding->field["boolean"]},{"choice",binding->field["choice"]},{"baselineText",binding->field["text"]},{"baselineBoolean",binding->field["boolean"]}};
                    if(index<0 || size_t(index)>=binding->field["options"].size()) return;
                    auto value=binding->field["options"][index]["value"].get<std::string>();
                    draft["choice"]=index; draft["text"]=value=="#"?"1":value;
                    if(draft["text"]==draft["baselineText"]) drafts.erase(id); else drafts[id]=std::move(draft);
                    auto encoded=drafts.dump(); ui_page_set_json(self->page_,"drafts",encoded.c_str());
                    set_text(self->page_,"saveError","");
                } catch(const std::exception& e) { set_text(self->page_,"actionError",translated_error(e.what(),page_value(self->page_,"language").get<std::string>())); }
            },binding);
            ui_widget_add_child(parent,combo); ui_widget_destroy(combo);
        },binding.get());
        ui_page_on_widget_unmount(page_,host.c_str(),[](UiPage,UiWidget widget,void*){ui_widget_destroy(widget);},nullptr);
    }
}
void Application::rebuild_theme(const std::string& language) {
    auto host=ui_widget_find_by_id(ui_page_root(page_),"themeHost");
    if(!host) return;
    if(theme_widget_) { ui_widget_remove_child(host,theme_widget_); ui_widget_destroy(theme_widget_); theme_widget_=0; }
    std::wstring labels[]={wide(tr("system",language)),wide(tr("light",language)),wide(tr("dark",language))};
    const wchar_t* texts[]={labels[0].c_str(),labels[1].c_str(),labels[2].c_str()};
    theme_widget_=ui_combobox(texts,3); ui_widget_set_id(theme_widget_,"themeSelector"); ui_widget_set_width(theme_widget_,180); ui_widget_set_height(theme_widget_,32);
    ui_combobox_set_selected(theme_widget_,page_value(page_,"themeChoice").get<int>());
    ui_combobox_on_changed(theme_widget_,[](UiWidget,int index,void* context){
        auto* self=static_cast<Application*>(context);
        ui_page_set_int(self->page_,"themeChoice",index);
        set_text(self->page_,"theme",index==1?"light":index==2?"dark":"system");
        apply_theme(self->page_,index==1?UI_THEME_LIGHT:index==2?UI_THEME_DARK:system_theme(),self->hwnd_);
    },this);
    ui_widget_add_child(host,theme_widget_); ui_widget_destroy(host);
    host=ui_widget_find_by_id(ui_page_root(page_),"taskStatusHost");
    if(!host) return;
    if(status_widget_) { ui_widget_remove_child(host,status_widget_); ui_widget_destroy(status_widget_); }
    std::vector<std::wstring> statuses={wide(tr("allStatus",language))};
    for(auto& status:page_value(page_,"statusNames")) if(status!="") statuses.push_back(wide(tr(status.get<std::string>(),language)));
    std::vector<const wchar_t*> status_texts; for(auto& status:statuses) status_texts.push_back(status.c_str());
    status_widget_=ui_combobox(status_texts.data(),int(status_texts.size()));
    ui_widget_set_id(status_widget_,"taskStatus"); ui_widget_set_expand(status_widget_,true); ui_widget_set_height(status_widget_,32);
    ui_combobox_set_selected(status_widget_,page_value(page_,"taskStatus").get<int>());
    ui_combobox_on_changed(status_widget_,[](UiWidget,int index,void* context){
        auto* self=static_cast<Application*>(context);
        ui_page_set_int(self->page_,"taskStatus",index);
        auto statuses=page_value(self->page_,"statusNames");
        self->handle_command(Json{{"op","filterTasks"},{"payload",{{"query",page_value(self->page_,"taskQuery")},{"root",page_value(self->page_,"taskRoot")},{"status",index>=0 && size_t(index)<statuses.size()?statuses[index]:Json("")},{"page",0}}}});
    },this);
    ui_widget_add_child(host,status_widget_); ui_widget_destroy(host);
}
void Application::dispatch() {
    VisualBatch visual(window_);
    auto commands=page_value(page_,"requests");
    ui_page_set_json(page_,"requests","[]");
    ui_page_set_bool(page_,"dispatch",false);
    if(commands.is_array()) for(auto& command:commands) {
        try { handle_command(std::move(command)); }
        catch(const std::exception& e) { set_text(page_,"actionError",translated_error(e.what(),page_value(page_,"language").get<std::string>())); }
    }
}
void Application::handle_command(Json command) {
    if(!command.is_object()) return;
    auto op=command.value("op",std::string());
    auto p=command.value("payload",Json::object());
    if(op=="navigate") { navigate(p.value("tab",std::string())); return; }
    if(op=="animateView") {
        auto id=p.value("id",std::string());
        if(id=="configBlocks") { VisualBatch visual(window_); ui_window_relayout(window_); motion::enter(window_,id.c_str(),0,12); }
        return;
    }
    if(op=="revealForm") { reveal_form(p.value("kind",std::string())=="password"); return; }
    if(op=="browse") {
        auto selected=choose_directory(hwnd_);
        if(!selected.empty()) {
            auto target=p.value("target",std::string());
            if(target=="scopeText" || (target=="rootInput" && page_value(page_,"rootOriginal")=="")) {
                auto text=page_value(page_,target.c_str()).get<std::string>();
                if(!text.empty() && text.back()!='\n') text+='\n';
                text+=utf8(selected.wstring()); set_text(page_,target.c_str(),text);
            } else if(target=="rootInput" || target=="rootOutput") set_text(page_,target.c_str(),utf8(selected.wstring()));
        }
        return;
    }
    if(op=="discard") sync_toggles();
    if(op=="openTask") {
        auto tasks=page_value(page_,"tasks");
        auto id=p.value("id",std::string());
        const auto& rows=tasks.at("rows");
        auto task=std::find_if(rows.begin(),rows.end(),[&](const Json& row){return row["id"]==id;});
        if(task==rows.end()) return;
        p=Json{{"path",utf8(task_directory(*task).wstring())}};
        op="openPath";
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
        if(message==WM_CLOSE && !self->stopping_) {
            auto language=page_value(self->page_,"language").get<std::string>();
            if(page_value(self->page_,"saving")==true || page_value(self->page_,"staging")==true) return 0;
            if(page_value(self->page_,"changeCount").get<int>()>0 && MessageBoxW(hwnd,wide(tr("closeUnsaved",language)).c_str(),
                wide(tr("title",language)).c_str(),MB_OKCANCEL|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDOK) return 0;
        }
        if(message==Dispatch) { self->dispatch(); return 0; }
        if(message==Update) { self->apply_pending(); return 0; }
        if(message==WM_ACTIVATE && LOWORD(wparam)!=WA_INACTIVE) { self->changed(Startup); SetEvent(self->runtime_changed_); }
        if(message==WM_SETTINGCHANGE && page_value(self->page_,"theme")=="system") apply_theme(self->page_,system_theme(),self->hwnd_);
    } catch(const std::exception& e) { set_text(self->page_,"actionError",translated_error(e.what(),page_value(self->page_,"language").get<std::string>())); }
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
    auto texts=interface_texts("en").dump(); ui_page_set_json(page_,"texts",texts.c_str());
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
    if(!runtime_changed_.value) win_error("Runtime change event");
    ui_page_on_widget_mount(page_,"dispatch",[](UiPage,UiWidget,void* context){
        auto* self=static_cast<Application*>(context); PostMessageW(self->hwnd_,Dispatch,0,0);
    },this);
    ui_page_on_widget_unmount(page_,"dispatch",[](UiPage,UiWidget widget,void*){
        // The lifecycle API registers an owning handle; release it on every dispatch.
        if(widget) ui_widget_destroy(widget);
    },nullptr);
    rebuild_theme("en");
    ui_window_on_close(window_,[](UiWindow,void* context){
        auto* self=static_cast<Application*>(context);
        { std::lock_guard<std::mutex> lock(self->mutex_); self->stopping_=true; }
        SetEvent(self->stop_); self->cv_.notify_all(); ui_quit(0);
    },this);
    ui_page_set_bool(page_,"cliAllowed",options_.allow_cli);
    ui_window_show_immediate(window_);
    // Keep native maximize/minimize transitions enabled after preparation/show.
    style_frame(hwnd_,system_theme());
    stage="启动文件观察器";
    worker_=std::thread([this]{ work(); });
    registry_=std::thread([this]{
        HKEY key{};
        // Observe the existing Run key; do not create startup entries merely by opening the UI.
        RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_NOTIFY,&key);
        Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        bool watching=key && RegNotifyChangeKeyValue(key,FALSE,REG_NOTIFY_CHANGE_LAST_SET,event,TRUE)==ERROR_SUCCESS;
        RuntimeMonitor runtime(executable_path().parent_path()/"sunpack-runtime.exe");
        Json previous;
        bool refresh=true;
        for(;;) {
            if(refresh) {
                Json status;
                try { status={{"known",true},{"running",runtime.running()}}; }
                catch(...) { status={{"known",false},{"running",false}}; }
                if(status!=previous) { publish({{"runtime",status}}); previous=std::move(status); }
                refresh=false;
            }
            // Filesystem hints remain signaled until consumed; a process that
            // exits during discovery leaves its kernel handle signaled as well.
            HANDLE waits[]={stop_,runtime_changed_,nullptr,nullptr};
            DWORD count=2, process_index=MAXDWORD, registry_index=MAXDWORD;
            if(auto process=runtime.process_handle()) { process_index=count; waits[count++]=process; }
            if(watching) { registry_index=count; waits[count++]=event; }
            auto signal=WaitForMultipleObjects(count,waits,FALSE,INFINITE);
            if(signal==WAIT_OBJECT_0 || signal==WAIT_FAILED) break;
            if(signal==WAIT_OBJECT_0+1 || (process_index!=MAXDWORD && signal==WAIT_OBJECT_0+process_index)) refresh=true;
            if(registry_index!=MAXDWORD && signal==WAIT_OBJECT_0+registry_index) {
                ResetEvent(event); changed(Startup);
                watching=RegNotifyChangeKeyValue(key,FALSE,REG_NOTIFY_CHANGE_LAST_SET,event,TRUE)==ERROR_SUCCESS;
            }
        }
        if(key) RegCloseKey(key);
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
    if(theme_widget_) ui_widget_destroy(theme_widget_);
    if(status_widget_) ui_widget_destroy(status_widget_);
    ui_shutdown();
}
void Application::work() {
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    try {
        Json config=Json::object(), passwords={{"passwords",Json::array()}};
        std::string language="en";
        WatchRoots roots; EventLog log; View view;
        Json password_edits=Json::array(),root_edits=Json::array(),preview_passwords;
        WatchRoots preview_roots;
        Json errors=Json::object();
        auto dir=options_.data_dir;
        auto dir_key=path_key(dir);
        auto on_data=[this,dir_key](const fs::path& path){
            auto name=path.filename().wstring(); CharLowerBuffW(name.data(),DWORD(name.size()));
            if(path.empty() || is_under(dir_key,path_key(path))) {
                SetEvent(runtime_changed_); changed(Config|Passwords|Roots); return;
            }
            if(is_runtime_state_name(name) && path_key(path.parent_path())==dir_key) SetEvent(runtime_changed_);
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
            Json edit_feedback=Json::object();
            if(!data_watcher || !data_watcher->alive() || (!data_watcher->direct() && fs::is_directory(dir))) {
                try {
                    data_watcher=std::make_unique<DirectoryWatcher>(dir,on_data); errors.erase("文件观察器");
                    // Reconcile after arming, covering starts between the initial
                    // process snapshot and this watch (also after a rebind).
                    SetEvent(runtime_changed_);
                }
                catch(const std::exception& e) { errors["文件观察器"]=e.what(); }
            }
            for(auto& command:commands) {
                auto op=command.value("op",std::string()); auto p=command.value("payload",Json::object());
                try {
                    if(op=="filterTasks") {
                        bool switch_root=view.task_root!=p.value("root",std::string());
                        view.task_query=p.value("query",std::string()); view.task_root=p.value("root",std::string());
                        view.task_status=p.value("status",std::string()); view.task_page=p.value("page",size_t(0)); tasks_changed=true;
                        if(switch_root) edit_feedback["viewTransition"]={{"id","taskBody"},{"y",12}};
                    } else if(op=="filterPasswords") {
                        auto scope=p.value("scope",std::string("@all"));
                        if(view.password_scope!=scope) edit_feedback["viewTransition"]={{"id","passwordBody"},{"x",scope=="@history"?20:-20}};
                        view.password_query=p.value("query",std::string()); view.password_scope=p.value("scope",std::string("@all"));
                        view.password_page=p.value("page",size_t(0)); passwords_changed=true;
                    } else if(op=="stagePassword" || op=="stageRoot") {
                        bool password=op=="stagePassword";
                        auto edit=p; edit.erase("nextState");
                        auto edits=password?password_edits:root_edits; edits.push_back(edit);
                        if(password) { preview_passwords=apply_password_changes(passwords,edits); password_edits=std::move(edits); passwords_changed=true; }
                        else { preview_roots=apply_root_changes(roots,dir,edits); root_edits=std::move(edits); tasks_changed=true; }
                        if(p.contains("nextState")) {
                            if(p["nextState"].contains("passwordScope")) { view.password_scope=p["nextState"]["passwordScope"]; view.password_page=0; passwords_changed=true; }
                            if(p["nextState"].contains("taskRoot")) { view.task_root=p["nextState"]["taskRoot"]; view.task_page=0; tasks_changed=true; }
                        }
                        dirs_changed=true;
                        edit_feedback.update(Json{{"stagedEdit",{{"kind",password?"passwords":"roots"},{"edit",std::move(edit)},{"nextState",p["nextState"]}}},{"staging",false},{"saveError",""}});
                    } else if(op=="discard") {
                        password_edits=Json::array(); root_edits=Json::array();
                        passwords_changed=tasks_changed=dirs_changed=true;
                    } else if(op=="saveAll") {
                        auto password_changes=p.at("passwordEdits"),root_changes=p.at("rootEdits");
                        if(!p["passwordForm"].is_null()) password_changes.push_back(p["passwordForm"]);
                        if(!p["rootForm"].is_null()) root_changes.push_back(p["rootForm"]);
                        auto result=save_changes(dir,p.at("fields"),password_changes,root_changes);
                        for(auto& section:result["saved"]) {
                            if(section=="config") flags|=Config;
                            if(section=="passwords") { password_edits=Json::array(); flags|=Passwords; passwords_changed=dirs_changed=true; }
                            if(section=="roots") { root_edits=Json::array(); flags|=Roots; tasks_changed=dirs_changed=true; }
                        }
                        auto message=translated_error(result["error"].get<std::string>(),language);
                        if(result.contains("field")) for(auto& field:p["fields"]) if(field["pointer"]==result["field"]) {
                            message=field.value("label",field["pointer"].get<std::string>())+": "+message; break;
                        }
                        if(!message.empty() && !result["saved"].empty()) message=tr("partialSave",language)+message;
                        edit_feedback.update(Json{{"batchAck",{{"payload",p},{"saved",result["saved"]}}},{"saving",false},{"saveError",message}});
                    } else if(op=="cli") {
                        if(!options_.allow_cli) throw std::runtime_error("测试数据目录未绑定 CLI；请显式传入 --cli");
                        auto action=p.value("action",std::string());
                        std::vector<std::wstring> args={L"watch"};
                        if(action=="start" || action=="stop") args.push_back(wide(action));
                        else if(action=="startupEnable" || action=="startupDisable") { args.push_back(L"startup"); args.push_back(action=="startupEnable"?L"enable":L"disable"); }
                        else throw std::runtime_error("不支持的 CLI 操作");
                        auto result=run_cli(options_.cli,args,stop_);
                        SetEvent(runtime_changed_);
                        try { wide(result.output); } catch(...) { result.output=tr("cliEncoding",language); }
                        Json feedback={{"busy",false},{"lastCommand",{{"action",action},{"code",result.code}}}};
                        if(result.code!=0) {
                            auto message=tr("cliFailed",language); auto at=message.find("{code}"); message.replace(at,6,std::to_string(result.code));
                            feedback["actionError"]=message+"\n"+result.output;
                        }
                        publish(std::move(feedback)); flags|=Startup;
                    } else throw std::runtime_error("未知界面操作");
                } catch(const std::exception& e) {
                    auto message=translated_error(e.what(),language);
                    if(op=="saveAll" || op=="stagePassword" || op=="stageRoot") edit_feedback.update(Json{{"saving",false},{"staging",false},{"saveError",message}});
                    else publish({{"busy",false},{"actionError",message}});
                }
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
            if(passwords_changed) {
                try { preview_passwords=password_edits.empty()?Json():apply_password_changes(passwords,password_edits); }
                catch(const std::exception& e) { publish({{"saveError",translated_error(e.what(),language)}}); }
            }
            if(tasks_changed) {
                try { preview_roots=root_edits.empty()?WatchRoots{}:apply_root_changes(roots,dir,root_edits); }
                catch(const std::exception& e) { publish({{"saveError",translated_error(e.what(),language)}}); }
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
            const auto& shown_roots=root_edits.empty()?roots:preview_roots;
            const auto& shown_passwords=password_edits.empty()?passwords:preview_passwords;
            auto next_language=gui_language(config);
            bool language_changed=language!=next_language || !previous_patch.contains("language");
            language=std::move(next_language);
            if(language_changed) { patch["language"]=language; patch["texts"]=interface_texts(language); }
            if(tasks_changed || language_changed || !previous_patch.contains("tasks")) patch["tasks"]=task_rows(log,shown_roots,view,language);
            if(passwords_changed || language_changed || !previous_patch.contains("passwords")) patch["passwords"]=password_rows(shown_passwords,view,language);
            if(config_changed || language_changed || !previous_patch.contains("fields")) patch["fields"]=config_fields(config,last_config,language);
            if(dirs_changed || !previous_patch.contains("directories")) patch["directories"]=directory_rows(shown_roots,config);
            if(flags&Startup || !previous_patch.contains("startup")) {
                try { auto state=read_startup(); patch["startup"]={{"enabled",state.enabled},{"command",state.command}}; errors.erase("startup"); }
                catch(const std::exception& e){errors["startup"]=e.what();}
            }
            auto displayed_errors=Json::object();
            for(auto it=errors.begin();it!=errors.end();++it) displayed_errors[tr(it.key(),language)]=translated_error(it.value().get<std::string>(),language);
            patch["errors"]=std::move(displayed_errors); patch["logPath"]=utf8(log.path().wstring()); patch["ready"]=true;
            Json changed_patch=Json::object();
            for(auto it=patch.begin();it!=patch.end();++it) if(!previous_patch.contains(it.key()) || previous_patch[it.key()]!=it.value()) {
                previous_patch[it.key()]=it.value(); changed_patch[it.key()]=std::move(it.value());
            }
            changed_patch.update(edit_feedback);
            if(!changed_patch.empty()) publish(std::move(changed_patch));
        }
    } catch(const std::exception& e) { publish({{"actionError",translated_error(e.what(),"en")},{"ready",true}}); }
    CoUninitialize();
}
}

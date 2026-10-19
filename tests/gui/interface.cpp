#include "application.hpp"
#include "ui_defaults_json.h"
#include <cassert>
#include <iostream>
#include <dwmapi.h>
using namespace spg;
namespace {
Json state(UiPage page,const char* key) { char* text=ui_page_get_json(page,key); if(!text) throw std::runtime_error(std::string("UI state unavailable: ")+key); auto j=parse_json(text); ui_page_free(text); return j; }
void pump(const std::function<bool()>& condition) {
    auto deadline=GetTickCount64()+10000;
    for(;;) {
        try { if(condition()) return; }
        catch(const WinError& error) {
            // Test-side reads can overlap ReplaceFile's rename/share window.
            // Wait for the actual queued UI/file event, with the same deadline.
            if(error.code!=ERROR_FILE_NOT_FOUND && error.code!=ERROR_PATH_NOT_FOUND && error.code!=ERROR_SHARING_VIOLATION) throw;
        }
        DWORD left=DWORD(deadline>GetTickCount64()?deadline-GetTickCount64():0);
        if(!left) throw std::runtime_error("UI event timed out");
        MsgWaitForMultipleObjectsEx(0,nullptr,left,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        MSG message;
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { if(message.message==WM_QUIT) continue; TranslateMessage(&message); DispatchMessageW(&message); }
    }
}
void click(Application& app,const char* id) {
    ui_window_relayout(app.window());
    auto widget=ui_widget_find_by_id(ui_page_root(app.page()),id); assert(widget);
    if(ui_debug_widget_is_visible(widget)!=1) {
        char* info=ui_debug_dump_widget(widget); std::cerr<<id<<": "<<(info?info:"")<<"\n"; ui_debug_free(info);
    }
    assert(ui_debug_widget_is_visible(widget)==1);
    assert(ui_debug_click(app.window(),widget)==0);
    ui_widget_destroy(widget);
}
void screenshot(Application& app,const wchar_t* name) {
    wchar_t path[32768];
    auto n=GetEnvironmentVariableW(L"SUNPACK_GUI_SCREENSHOTS",path,32768);
    if(!n || n>=32768) return;
    fs::create_directories(path); ui_debug_pump(); ui_window_relayout(app.window());
    char* tree=ui_debug_dump_tree(app.window());
    if(tree) { atomic_write(fs::path(path)/(std::wstring(name)+L".json"),tree); ui_debug_free(tree); }
    assert(ui_debug_screenshot(app.window(),(fs::path(path)/name).c_str())==0);
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc>1 && std::wstring(argv[1])==L"--fake-cli") {
        Json args=Json::array(); for(int i=3;i<argc;++i) args.push_back(utf8(argv[i])); atomic_write(fs::current_path()/"cli-arguments.json",args.dump());
        Handle gate(OpenEventW(SYNCHRONIZE,FALSE,argv[2])); assert(gate.value);
        assert(WaitForSingleObject(gate,10000)==WAIT_OBJECT_0);
        std::cout<<args.dump(); return 0;
    }
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    auto dir=fs::temp_directory_path()/(L"sunpack-gui-ui-"+std::to_wstring(GetCurrentProcessId())); fs::create_directory(dir);
    auto gate_name=L"Local\\SunPack.GUI.TestCommand."+std::to_wstring(GetCurrentProcessId());
    Handle gate(CreateEventW(nullptr,TRUE,TRUE,gate_name.c_str()));
    try {
        atomic_write(dir/"sunpack_config.json",R"({"cli":{"language":"zh"},"runtime":{"process_mode":"normal"},"recursive_extract":"*","watch":{"state_dir":".sunpack_watch","out_dir":".","toast_enabled":true},"unknown":9007199254740993})");
        atomic_write(dir/"builtin_passwords.json",R"({"passwords":[{"value":"  #中文  "}],"clipboard_history":["history"]})");
        atomic_write(dir/"sunpack_watch_roots.txt",utf8(dir.wstring())+"\n");
        atomic_write(dir/".sunpack_watch"/"events.jsonl",Json({{"event","done"},{"path",utf8((dir/"carrier.001").wstring())}}).dump()+"\n");
        {
            Options options; options.data_dir=dir; options.allow_cli=true;
            options.cli={executable_path(),dir,{L"--fake-cli",gate_name}};
            Application app(options); std::cerr<<"Opening UI\n"; app.open(); std::cerr<<"UI opened\n";
            pump([&]{return state(app.page(),"ready")==true;});
            BOOL disabled=TRUE;
            if(SUCCEEDED(DwmGetWindowAttribute(static_cast<HWND>(ui_window_hwnd(app.window())),DWMWA_TRANSITIONS_FORCEDISABLED,&disabled,sizeof(disabled)))) assert(!disabled);
            std::cerr<<"Initial files loaded\n";
            assert(state(app.page(),"tasks")["total"]==1);
            auto error=ui_page_last_error(app.page()); if(error && *error) std::cerr<<error<<"\n";
            screenshot(app,L"tasks.png");
            click(app,"watchStart");
            pump([&]{return state(app.page(),"lastCommand").value("action",std::string())=="start";});
            assert(read_json(dir/"cli-arguments.json")==Json::array({"watch","start"}));
            assert(state(app.page(),"actionError")=="");
            pump([&]{return state(app.page(),"runtime")["known"]==true;});
            assert(!ui_widget_find_by_id(ui_page_root(app.page()),"refresh"));
            auto stable_commands=[&](const char* id,const std::string& action,const Json& args) {
                auto tab=state(app.page(),"tab").get<std::string>();
                auto page=ui_widget_find_by_id(ui_page_root(app.page()),("page-"+tab).c_str());
                auto rect=ui_widget_get_rect(page);
                fs::remove(dir/"cli-arguments.json");
                ResetEvent(gate);
                click(app,id);
                // The dispatch signal is mounted before its posted message is
                // handled. Force this intermediate layout to catch page jumps.
                ui_window_relayout(app.window());
                auto during=ui_widget_get_rect(page);
                assert(during.left==rect.left && during.top==rect.top && during.right==rect.right && during.bottom==rect.bottom);
                pump([&]{return state(app.page(),"busy")==true && read_json(dir/"cli-arguments.json")==args;});
                assert(ui_widget_get_opacity(page)>=0.999f && ui_debug_widget_is_visible(page)==1);
                SetEvent(gate);
                pump([&]{return state(app.page(),"lastCommand").value("action",std::string())==action && state(app.page(),"busy")==false;});
                assert(read_json(dir/"cli-arguments.json")==args);
                assert(state(app.page(),"actionError")=="");
                auto after=ui_widget_get_rect(page);
                assert(after.left==rect.left && after.top==rect.top && after.right==rect.right && after.bottom==rect.bottom);
                assert(ui_widget_get_opacity(page)>=0.999f && ui_debug_widget_is_visible(page)==1);
                ui_widget_destroy(page);
            };
            stable_commands("watchStop","stop",Json::array({"watch","stop"}));
            stable_commands("watchStart","start",Json::array({"watch","start"}));
            click(app,"tabConfig"); pump([&]{return state(app.page(),"tab")=="config";});
            auto content=ui_widget_find_by_id(ui_page_root(app.page()),"page-config");
            assert(content && ui_widget_get_opacity(content)>0); // No blank incoming frame.
            pump([&]{return ui_widget_get_opacity(content)>=0.999f;}); ui_widget_destroy(content);
            stable_commands("watchReload","reload",Json::array({"watch","reload"}));
            auto startup_enabled=state(app.page(),"startup")["enabled"].get<bool>();
            stable_commands("startupToggle",startup_enabled?"startupDisable":"startupEnable",Json::array({"watch","startup",startup_enabled?"disable":"enable"}));
            auto alternate=Json{{"enabled",!startup_enabled},{"command",""}}.dump();
            ui_page_set_json(app.page(),"startup",alternate.c_str());
            stable_commands("startupToggle",startup_enabled?"startupEnable":"startupDisable",Json::array({"watch","startup",startup_enabled?"enable":"disable"}));
            auto original_startup=Json{{"enabled",startup_enabled},{"command",""}}.dump();
            ui_page_set_json(app.page(),"startup",original_startup.c_str());
            std::cerr<<"Configuration view mounted\n";
            auto selector_host=ui_widget_find_by_id(ui_page_root(app.page()),"choice-/runtime/process_mode");
            char* selector_tree=ui_debug_dump_widget(selector_host); assert(selector_tree);
            auto selector_info=parse_json(selector_tree); ui_debug_free(selector_tree); ui_widget_destroy(selector_host);
            assert(selector_info["children"].size()==1);
            screenshot(app,L"configuration.png");
            auto system_appearance=state(app.page(),"appearance");
            auto theme=ui_widget_find_by_id(ui_page_root(app.page()),"themeSelector"); assert(theme);
            assert(ui_debug_combo_select(app.window(),theme,1)==0);
            pump([&]{return state(app.page(),"appearance")=="light";});
            screenshot(app,L"configuration-light.png");
            assert(ui_debug_combo_select(app.window(),theme,2)==0);
            pump([&]{return state(app.page(),"appearance")=="dark";});
            assert(ui_debug_combo_select(app.window(),theme,0)==0);
            assert(state(app.page(),"appearance")==system_appearance);
            // FindById reuses Application's owned theme handle, rather than
            // adding a reference. Its owner releases it during locale changes.
            // Native enum control writes a constrained value inline.
            auto priority=ui_widget_find_by_id(ui_page_root(app.page()),"select-/runtime/process_mode"); assert(priority);
            assert(ui_debug_combo_select(app.window(),priority,2)==0);
            ui_widget_destroy(priority);
            click(app,"save-/runtime/process_mode");
            pump([&]{return read_json(dir/"sunpack_config.json")["runtime"]["process_mode"]=="high";});
            ui_page_set_text(app.page(),"configQuery",L"toast_enabled");
            auto toast=ui_widget_find_by_id(ui_page_root(app.page()),"toggle-/watch/toast_enabled"); assert(toast);
            assert(ui_debug_toggle_set(app.window(),toast,0)==0);
            ui_widget_destroy(toast);
            click(app,"save-/watch/toast_enabled");
            click(app,"tabTasks"); // Rapid navigation must not overwrite the save command.
            pump([&]{return read_json(dir/"sunpack_config.json")["watch"]["toast_enabled"]==false;});
            pump([&]{return state(app.page(),"tab")=="tasks";});
            click(app,"tabConfig"); pump([&]{return state(app.page(),"tab")=="config";});
            ui_page_set_text(app.page(),"configQuery",L"");
            click(app,"configSearch"); ui_debug_type_text(app.window(),L"out_dir");
            if(state(app.page(),"configQuery")!="out_dir")
                throw std::runtime_error("Config search: "+state(app.page(),"configQuery").dump()+" error="+ui_page_last_error(app.page()));
            ui_window_set_size(app.window(),850,560);
            auto output=ui_widget_find_by_id(ui_page_root(app.page()),"input-/watch/out_dir"); assert(output);
            ui_text_input_set_text(output,L""); click(app,"input-/watch/out_dir"); ui_debug_type_text(app.window(),L"输出目录");
            ui_widget_destroy(output);
            assert(state(app.page(),"drafts")["/watch/out_dir"]["text"]=="输出目录");
            screenshot(app,L"configuration-inline-small.png");
            click(app,"save-/watch/out_dir");
            // More typing can arrive before the queued dispatch/save completes.
            // Saving must keep that newer draft instead of clearing it.
            click(app,"input-/watch/out_dir"); ui_debug_type_text(app.window(),L"后续");
            try { pump([&]{return read_json(dir/"sunpack_config.json")["watch"]["out_dir"]=="输出目录";}); }
            catch(...) { std::cerr<<"Save config: "<<state(app.page(),"settingError")<<"\n"; throw; }
            pump([&]{return state(app.page(),"savedField")=="/watch/out_dir";});
            assert(state(app.page(),"drafts")["/watch/out_dir"]["text"]=="输出目录后续");
            click(app,"save-/watch/out_dir");
            pump([&]{return read_json(dir/"sunpack_config.json")["watch"]["out_dir"]=="输出目录后续";});
            pump([&]{return !state(app.page(),"drafts").contains("/watch/out_dir");});
            // Keep unsaved input when an external writer changes the same field;
            // the save must report a conflict without overwriting that writer.
            output=ui_widget_find_by_id(ui_page_root(app.page()),"input-/watch/out_dir");
            ui_text_input_set_text(output,L""); click(app,"input-/watch/out_dir"); ui_debug_type_text(app.window(),L"unsaved");
            ui_widget_destroy(output);
            auto changed=read_json(dir/"sunpack_config.json"); changed["watch"]["out_dir"]="external"; atomic_write(dir/"sunpack_config.json",changed.dump());
            pump([&]{for(auto& field:state(app.page(),"fields")) if(field["id"]=="/watch/out_dir") return field["text"]=="external"; return false;});
            click(app,"save-/watch/out_dir"); pump([&]{return state(app.page(),"settingError").value("id",std::string())=="/watch/out_dir";});
            assert(read_json(dir/"sunpack_config.json")["watch"]["out_dir"]=="external");
            assert(state(app.page(),"drafts")["/watch/out_dir"]["text"]=="unsaved");
            // External language replacement updates visible and generated controls.
            auto conf=read_json(dir/"sunpack_config.json"); conf["cli"]["language"]="en"; atomic_write(dir/"sunpack_config.json",conf.dump());
            pump([&]{return state(app.page(),"language")=="en";});
            assert(state(app.page(),"texts")["config"]=="Settings");
            assert(state(app.page(),"fields")[0]["label"]=="Language");
            screenshot(app,L"configuration-english.png");
            ui_page_set_text(app.page(),"configQuery",L"language");
            auto language=ui_widget_find_by_id(ui_page_root(app.page()),"select-/cli/language"); assert(language);
            assert(ui_debug_combo_select(app.window(),language,0)==0);
            ui_widget_destroy(language);
            click(app,"save-/cli/language");
            pump([&]{return state(app.page(),"language")=="zh";});
            ui_window_set_size(app.window(),1120,720);
            click(app,"tabPasswords"); pump([&]{return state(app.page(),"tab")=="passwords";});
            std::cerr<<"Password view mounted\n";
            assert(state(app.page(),"passwords")["rows"][0]["value"]=="  #中文  ");
            click(app,"newPassword"); assert(state(app.page(),"passwordEditor")==true);
            click(app,"passwordValue"); ui_debug_type_text(app.window(),L"添加的密码\n  #批量  \n\n添加的密码");
            assert(state(app.page(),"passwordValue")=="添加的密码\n  #批量  \n\n添加的密码");
            screenshot(app,L"passwords.png");
            ui_window_set_size(app.window(),850,560);
            screenshot(app,L"password-editor-small.png"); click(app,"savePassword");
            try { pump([&]{return read_json(dir/"builtin_passwords.json")["passwords"].size()==3;}); }
            catch(...) { std::cerr<<"Save password: "<<state(app.page(),"actionError")<<"\n"; throw; }
            assert(read_json(dir/"builtin_passwords.json")["passwords"][2]["value"]=="  #批量  ");
            std::cerr<<"Password saved\n";
            try { pump([&]{return state(app.page(),"passwords")["total"]==3;}); }
            catch(...) { std::cerr<<"Password model: "<<state(app.page(),"passwords")<<" errors="<<state(app.page(),"errors")<<"\n"; throw; }
            std::cerr<<"Password model updated\n";
            atomic_write(dir/".sunpack_watch"/"events.jsonl",Json({{"event","failed_password"},{"path",utf8((dir/"carrier.001").wstring())}}).dump()+"\n");
            try { pump([&]{return state(app.page(),"tasks")["rows"][0]["status"]=="挂起：等待密码";}); }
            catch(...) { std::cerr<<"Log model: "<<state(app.page(),"tasks")<<" errors="<<state(app.page(),"errors")<<"\n"; throw; }
            std::cerr<<"Log replacement observed\n";
            atomic_write(dir/"sunpack_config.json","{");
            pump([&]{return state(app.page(),"errors").contains("sunpack_config.json");});
            std::cerr<<"Invalid configuration retained\n";
            assert(!state(app.page(),"fields").empty());
            atomic_write(dir/"elsewhere"/"events.jsonl",Json({{"event","done"},{"path",utf8((dir/"other.fake").wstring())}}).dump()+"\n");
            atomic_write(dir/"sunpack_config.json",R"({"watch":{"state_dir":"elsewhere"}})");
            try { pump([&]{return state(app.page(),"tasks")["rows"][0]["name"]=="other.fake";}); }
            catch(...) { std::cerr<<"Rebound log: "<<state(app.page(),"tasks")<<" errors="<<state(app.page(),"errors")<<" log="<<state(app.page(),"logPath")<<" fields="<<state(app.page(),"fields")<<"\n"; throw; }
            // Exercise an ordinary config containing all advanced settings.
            auto complete=parse_json(kUiDefaultsJson); complete["watch"]["state_dir"]="elsewhere";
            atomic_write(dir/"sunpack_config.json",complete.dump());
            pump([&]{return state(app.page(),"fields").size()>100;});
            ui_page_set_json(app.page(),"drafts","{}"); ui_page_set_text(app.page(),"configQuery",L"");
            click(app,"tabConfig"); pump([&]{return state(app.page(),"tab")=="config";});
            ui_window_relayout(app.window());
            for(auto pair:{std::pair<const char*,const char*>{"categoryScroll","category-cli"},{"configListScroll","field-/cli/language"}}) {
                auto scroll=ui_widget_find_by_id(ui_page_root(app.page()),pair.first);
                auto item=ui_widget_find_by_id(ui_page_root(app.page()),pair.second);
                assert(scroll && item);
                assert(ui_widget_get_rect(scroll).right-ui_widget_get_rect(item).right>=17.0f);
                ui_widget_destroy(item); ui_widget_destroy(scroll);
            }
            screenshot(app,L"advanced-settings-small.png");
            ui_page_set_text(app.page(),"configQuery",L"白名单");
            auto blocks=state(app.page(),"shownBlocks");
            if(blocks.size()!=1 || blocks[0]["fields"].size()!=5) std::cerr<<"Whitelist groups: "<<blocks<<"\n";
            assert(blocks.size()==1 && blocks[0]["fields"].size()==5);
            assert(blocks[0]["primary"]==true && blocks[0]["fields"][0]["label"]=="白名单");
            auto block=ui_widget_find_by_id(ui_page_root(app.page()),"block-/filesystem/scan_filters/1"); assert(block);
            for(auto& f:blocks[0]["fields"]) {
                auto item=ui_widget_find_by_id(block,("field-"+f["id"].get<std::string>()).c_str()); assert(item); ui_widget_destroy(item);
            }
            ui_widget_destroy(block);
            screenshot(app,L"whitelist-block-small.png");
            PostMessageW(static_cast<HWND>(ui_window_hwnd(app.window())),WM_CLOSE,0,0);
            pump([&]{return !IsWindow(static_cast<HWND>(ui_window_hwnd(app.window())));});
        }
        fs::remove_all(dir); std::cout<<"GUI interface and observation passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; std::error_code ec; fs::remove_all(dir,ec); return 1; }
    CoUninitialize(); return 0;
}

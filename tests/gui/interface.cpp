#include "application.hpp"
#include <cassert>
#include <iostream>
using namespace spg;
namespace {
Json state(UiPage page,const char* key) { char* text=ui_page_get_json(page,key); if(!text) throw std::runtime_error(std::string("UI state unavailable: ")+key); auto j=parse_json(text); ui_page_free(text); return j; }
void pump(const std::function<bool()>& condition) {
    auto deadline=GetTickCount64()+10000;
    while(!condition()) {
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
        Json args=Json::array(); for(int i=2;i<argc;++i) args.push_back(utf8(argv[i])); std::cout<<args.dump(); return 0;
    }
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    auto dir=fs::temp_directory_path()/(L"sunpack-gui-ui-"+std::to_wstring(GetCurrentProcessId())); fs::create_directory(dir);
    try {
        atomic_write(dir/"sunpack_config.json",R"({"watch":{"state_dir":".sunpack_watch","out_dir":"."},"unknown":9007199254740993})");
        atomic_write(dir/"builtin_passwords.json",R"({"passwords":[{"value":"  #中文  "}],"clipboard_history":["history"]})");
        atomic_write(dir/"sunpack_watch_roots.txt",utf8(dir.wstring())+"\n");
        atomic_write(dir/".sunpack_watch"/"events.jsonl",Json({{"event","done"},{"path",utf8((dir/"carrier.001").wstring())}}).dump()+"\n");
        {
            Options options; options.data_dir=dir; options.allow_cli=true;
            options.cli={executable_path(),dir,{L"--fake-cli"}};
            Application app(options); std::cerr<<"Opening UI\n"; app.open(); std::cerr<<"UI opened\n";
            pump([&]{return state(app.page(),"ready")==true;});
            std::cerr<<"Initial files loaded\n";
            assert(state(app.page(),"tasks")["total"]==1);
            auto error=ui_page_last_error(app.page()); if(error && *error) std::cerr<<error<<"\n";
            screenshot(app,L"tasks.png");
            click(app,"watchStart");
            pump([&]{return state(app.page(),"notice").get<std::string>().find("[\"watch\",\"start\"]")!=std::string::npos;});
            click(app,"tabConfig"); pump([&]{return state(app.page(),"tab")=="config";});
            std::cerr<<"Configuration view mounted\n";
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
            click(app,"configSearch"); ui_debug_type_text(app.window(),L"out_dir");
            if(state(app.page(),"configQuery")!="out_dir")
                throw std::runtime_error("Config search: "+state(app.page(),"configQuery").dump()+" error="+ui_page_last_error(app.page()));
            click(app,"field-/watch/out_dir");
            assert(state(app.page(),"configPointer")=="/watch/out_dir");
            ui_window_set_size(app.window(),850,560);
            ui_page_set_text(app.page(),"configText",L"输出目录");
            screenshot(app,L"configuration-editor-small.png");
            click(app,"saveConfig");
            try { pump([&]{return read_json(dir/"sunpack_config.json")["watch"]["out_dir"]=="输出目录";}); }
            catch(...) { std::cerr<<"Save config: "<<state(app.page(),"notice")<<" request="<<state(app.page(),"request")<<" dispatch="<<state(app.page(),"dispatch")<<"\n"; throw; }
            click(app,"closeConfig");
            if(state(app.page(),"configPointer")!="") {
                screenshot(app,L"close-config-failed.png");
                throw std::runtime_error(std::string("Close config: ")+ui_page_last_error(app.page())+" request="+state(app.page(),"request").dump());
            }
            ui_window_set_size(app.window(),1120,720);
            click(app,"tabPasswords"); pump([&]{return state(app.page(),"tab")=="passwords";});
            std::cerr<<"Password view mounted\n";
            assert(state(app.page(),"passwords")["rows"][0]["value"]=="  #中文  ");
            click(app,"newPassword"); assert(state(app.page(),"passwordEditor")==true);
            click(app,"passwordValue"); ui_debug_type_text(app.window(),L"添加的密码");
            assert(state(app.page(),"passwordValue")=="添加的密码");
            screenshot(app,L"passwords.png");
            ui_window_set_size(app.window(),850,560);
            screenshot(app,L"password-editor-small.png"); click(app,"savePassword");
            try { pump([&]{return read_json(dir/"builtin_passwords.json")["passwords"].size()==2;}); }
            catch(...) { std::cerr<<"Save password: "<<state(app.page(),"notice")<<" request="<<state(app.page(),"request")<<"\n"; throw; }
            std::cerr<<"Password saved\n";
            try { pump([&]{return state(app.page(),"passwords")["total"]==2;}); }
            catch(...) { std::cerr<<"Password model: "<<state(app.page(),"passwords")<<" errors="<<state(app.page(),"errors")<<" notice="<<state(app.page(),"notice")<<"\n"; throw; }
            std::cerr<<"Password model updated\n";
            atomic_write(dir/".sunpack_watch"/"events.jsonl",Json({{"event","failed_password"},{"path",utf8((dir/"carrier.001").wstring())}}).dump()+"\n");
            try { pump([&]{return state(app.page(),"tasks")["rows"][0]["status"]=="挂起：等待密码";}); }
            catch(...) { std::cerr<<"Log model: "<<state(app.page(),"tasks")<<" errors="<<state(app.page(),"errors")<<" notice="<<state(app.page(),"notice")<<"\n"; throw; }
            std::cerr<<"Log replacement observed\n";
            atomic_write(dir/"sunpack_config.json","{");
            pump([&]{return state(app.page(),"errors").contains("sunpack_config.json");});
            std::cerr<<"Invalid configuration retained\n";
            assert(!state(app.page(),"fields").empty());
            atomic_write(dir/"elsewhere"/"events.jsonl",Json({{"event","done"},{"path",utf8((dir/"other.fake").wstring())}}).dump()+"\n");
            atomic_write(dir/"sunpack_config.json",R"({"watch":{"state_dir":"elsewhere"}})");
            try { pump([&]{return state(app.page(),"tasks")["rows"][0]["name"]=="other.fake";}); }
            catch(...) { std::cerr<<"Rebound log: "<<state(app.page(),"tasks")<<" errors="<<state(app.page(),"errors")<<" log="<<state(app.page(),"logPath")<<" notice="<<state(app.page(),"notice")<<" fields="<<state(app.page(),"fields")<<"\n"; throw; }
            PostMessageW(static_cast<HWND>(ui_window_hwnd(app.window())),WM_CLOSE,0,0);
            pump([&]{return !IsWindow(static_cast<HWND>(ui_window_hwnd(app.window())));});
        }
        fs::remove_all(dir); std::cout<<"GUI interface and observation passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; std::error_code ec; fs::remove_all(dir,ec); return 1; }
    CoUninitialize(); return 0;
}

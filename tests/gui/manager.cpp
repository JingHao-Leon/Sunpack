#include "model.hpp"
#include "presentation.hpp"
#include "defaults_json.h"
#include <cassert>
#include <fstream>
#include <iostream>

using namespace spg;
namespace {
void fails(const std::function<void()>& action) { bool failed=false; try { action(); } catch(const std::exception&) { failed=true; } assert(failed); }
struct Fixture {
    fs::path dir;
    Fixture() { GUID id{}; CoCreateGuid(&id); wchar_t token[40]; StringFromGUID2(id,token,40); dir=fs::temp_directory_path()/token; fs::create_directory(dir); }
    ~Fixture() { std::error_code ec; fs::remove_all(dir,ec); }
};
void overwrite(const fs::path& p,const std::string& text,bool append=false) {
    std::ofstream out(p,std::ios::binary|(append?std::ios::app:std::ios::trunc)); out<<text;
}
std::string event(const std::string& type,const fs::path& p,int ts) {
    return Json({{"event",type},{"path",utf8(p.wstring())},{"ts",ts}}).dump()+"\n";
}
// These coarse scenarios cover independent contracts: projection/reset, coordinated edits,
// Windows identity/root policy. No benchmark assertions or implementation-mirroring tests.
void projection(Fixture& f) {
    auto p=f.dir/"events.jsonl", source=f.dir/L"伪装.bin.001";
    EventLog log; log.set_path(p);
    overwrite(p,event("candidate_active",source,1)+event("processing_started",source,2)+event("done",source,3)+event("cleanup",source,4));
    assert(log.refresh()); assert(log.tasks.size()==1);
    auto& task=log.tasks.begin()->second;
    assert(task.status=="成功" && task.cleanup["event"]=="cleanup");
    auto parsed=log.bytes_parsed; assert(!log.refresh()); assert(log.bytes_parsed==parsed);
    for(auto type:{"failed_terminal","failed_password","suspended_missing_volume","retry_password_failure","done","partial_rejected","pipeline_request_coalesced","no_tasks_found"}) {
        auto line=event(type,source,5); overwrite(p,line.substr(0,line.size()-2),true); log.refresh();
        auto status=log.tasks.begin()->second.status;
        overwrite(p,line.substr(line.size()-2),true); log.refresh();
        assert(log.tasks.begin()->second.details["event"]==type);
    }
    auto reset_source=f.dir/L"载体.fake";
    std::string replacement=event("processing_started",reset_source,50);
    for(int i=0;i<120;++i) replacement+=Json({{"event","scheduler_tick"},{"padding",std::string(80,'x')}}).dump()+"\n";
    overwrite(p,replacement); log.refresh();
    assert(log.tasks.size()==1 && log.tasks.begin()->second.path==utf8(reset_source.wstring()));
    // Same first record and file ID, truncate/regrow past old offset: tail anchor detects it.
    auto first=event("processing_started",reset_source,50);
    replacement=first+event("done",source,51)+std::string(replacement.size()+40,' ')+"\n";
    overwrite(p,replacement); log.refresh(); assert(log.tasks.size()==2);
    overwrite(p,"{invalid}\n"+event("failed_password",reset_source,52)); log.refresh();
    assert(log.malformed==1 && log.tasks.size()==1);
    fs::remove(p); fails([&]{log.refresh();}); assert(!log.tasks.empty()); log.refresh(true); assert(log.tasks.empty());
}
void editing(Fixture& f) {
    auto p=f.dir/"builtin_passwords.json";
    Json original={{"value","  #中文 密码  "},{"scopes",{"C:/A","D:/B"}}};
    atomic_write(p,Json({{"passwords",{original}},{"clipboard_history",{"后台密码"}}}).dump());
    edit_password(p,"removeScope",original,Json(),"c:\\a");
    auto j=read_json(p); assert(j["passwords"][0]["scopes"]==Json::array({"D:/B"}));
    assert(j["clipboard_history"][0]=="后台密码");
    edit_password(p,"removeScope",j["passwords"][0],Json(),"d:\\b");
    assert(read_json(p)["passwords"].empty());
    edit_password(p,"add",Json(),Json({{"value"," p # "}})); assert(read_json(p)["passwords"][0]["value"]==" p # ");
    fails([&]{edit_password(p,"replace",original,Json({{"value","wrong record"}}));});
    for(auto raw:{"{\"passwords\":[{\"value\":\"a\",\"scopes\":[]}]}","{\"passwords\":[],\"unknown\":1}","{\"passwords\":[{\"value\":\"a\",\"unknown\":1}]}","{\"passwords\":[],\"passwords\":[]}"})
        fails([&]{validate_passwords(parse_json(raw));});
    validate_passwords(parse_json(R"({"passwords":[{"value":"#","scopes":["\\\\server\\share","C:/中文"]}],"clipboard_history":[]})"));
    // Coordinate with a second writer using the exact production lock.
    std::thread writer([&]{ FileMutex lock(p); auto current=read_json(p); current["clipboard_history"].push_back("latest"); atomic_write(p,current.dump()); }); writer.join();
    edit_password(p,"add",Json(),Json({{"value","after history"}})); assert(read_json(p)["clipboard_history"].back()=="latest");
    auto before=read_file(p);
    fails([&]{edit_password(p,"addMany",Json(),Json::array({Json{{"value","valid"}},Json{{"value","invalid"},{"scopes",Json::array()}}}));});
    assert(read_file(p)==before); // A bad record cannot commit a partial batch.
    edit_password(p,"addMany",Json(),Json::array({Json{{"value"," # batch "}},Json{{"value"," # batch "}},Json{{"value","other"}}}));
    assert(read_json(p)["passwords"].size()==4 && read_json(p)["clipboard_history"].back()=="latest");
    auto c=f.dir/"sunpack_config.json";
    atomic_write(c,R"({"watch":{"out_dir":"."},"unknown":{"large":9007199254740993,"huge":123456789012345678901234567890},"filters":[{"name":"x","rules":["*","r",3]}]})");
    save_config_field(c,"/watch/out_dir","\".\"","string","中文",false);
    auto conf=read_json(c); assert(conf["unknown"]["large"].dump()=="9007199254740993");
    assert(read_file(c).find("123456789012345678901234567890")!=std::string::npos);
    auto fields=config_fields(conf,read_file(c));
    auto huge=std::find_if(fields.begin(),fields.end(),[](const Json& field){return field["id"]=="/unknown/huge";});
    assert((*huge)["text"]=="123456789012345678901234567890");
    assert(conf["filters"][0]["rules"]==Json::array({"*","r",3}));
    fails([&]{save_config_field(c,"/watch/out_dir","\".\"","string","stale",false);});
    fails([&]{validate_setting("/runtime/process_mode",Json("typo"));});
    fails([&]{validate_setting("/watch/max_folders",Json(0));});
    fails([&]{validate_setting("/embedded_scan/recursive_candidate_ratio",Json(1.5));});
    validate_setting("/recursive_extract",Json("12"));
    fails([&]{validate_setting("/recursive_extract",Json("0"));});
    // One coarse catalog contract covers every shipped setting, including
    // fields nested in filter/module arrays, in both CLI-supported languages.
    for(auto language:{"zh","en"}) {
        auto defaults=parse_json(kDefaultsJson);
        for(auto& field:config_fields(defaults,kDefaultsJson,language)) {
            if(!field["known"].get<bool>()) throw std::runtime_error("Missing setting description: "+field["id"].get<std::string>());
            assert(!field["label"].get<std::string>().empty() && !field["description"].get<std::string>().empty());
        }
        assert(!describe_setting("/retired/enabled","",language)["known"].get<bool>());
        assert(!describe_setting("/watch/directory_password_file_auto_create","",language)["known"].get<bool>());
        assert(!describe_setting("/passwords/directory_passwords_enabled","",language)["known"].get<bool>());
        // Module identity, rather than position, survives copying/reordering.
        auto module=config_fields(parse_json(R"({"analysis":{"modules":[{"name":"gzip","max_probe_bytes":2048}]}})"),"",language);
        assert(module.size()==1 && module[0]["known"].get<bool>());
    }
    assert(log_directory(f.dir,Json({{"watch",{{"state_dir","other"}}}}))==f.dir/"other");
}
void installer_upgrade(Fixture& fixture) {
    auto path=fixture.dir/"upgrade.json";
    const std::string keep=R"("advanced":{"integer":123456789012345678901234567890,"text":"directory_password_file_auto_create"})";
    const std::vector<std::string> inputs={
        R"({"watch":{"directory_password_file_auto_create":true,"password_retry_include_subtree":false}})",
        R"({"passwords":{"directory_passwords_enabled":true,"directory_passwords_max_file_bytes":1048576,"directory_passwords_max_password_length":512},"watch":{"directory_password_file_auto_create":false}})",
        "{\"watch\":{\"directory_password_file_auto_create\":true,\"quiet_max_seconds\":13,\"password_retry_include_subtree\":false},"+keep+"}",
        "{\"passwords\":{\"clipboard_passwords_enabled\":true,\"directory_passwords_enabled\":false,\"directory_passwords_max_file_bytes\":1,\"directory_passwords_max_password_length\":1},"+keep+"}"
    };
    for(auto& input:inputs) {
        atomic_write(path,input); upgrade_config_file(path);
        auto output=read_file(path); auto config=parse_json(output);
        assert(!config.contains(Json::json_pointer("/watch/directory_password_file_auto_create")));
        assert(!config.contains(Json::json_pointer("/watch/password_retry_include_subtree")));
        for(auto name:{"directory_passwords_enabled","directory_passwords_max_file_bytes","directory_passwords_max_password_length"})
            assert(!config.contains(Json::json_pointer(std::string("/passwords/")+name)));
        if(config.contains("advanced")) assert(output.find(keep)!=std::string::npos);
        if(config.contains("watch")) assert(config["watch"]["quiet_max_seconds"]==13);
        if(config.contains("passwords")) assert(config["passwords"]["clipboard_passwords_enabled"]==true);
        upgrade_config_file(path); assert(read_file(path)==output);
    }
    // Retained advanced values are not normalized or reformatted on upgrade.
    atomic_write(path,kDefaultsJson); upgrade_config_file(path); assert(read_file(path)==kDefaultsJson);
    atomic_write(path,"{invalid}"); fails([&]{upgrade_config_file(path);}); assert(read_file(path)=="{invalid}");
}
void roots(Fixture& f) {
    auto p=f.dir/"sunpack_watch_roots.txt";
    atomic_write(p,"# 保留注释\n"+utf8(f.dir.wstring())+" | relative | false\n"+utf8(f.dir.wstring())+" | 末条 | TRUE\n");
    auto roots=read_roots(p); assert(roots.entries.size()==1 && roots.entries[0].deep && roots.entries[0].output=="末条");
    auto nested=f.dir/"Data"; fs::create_directory(nested);
    edit_root(p,"save","",utf8(nested.wstring()),"",false);
    auto all=read_roots(p); assert(all.entries.size()==2);
    assert(root_for(nested/"x.bin",all)==utf8(nested.wstring()));
    assert(root_for(f.dir/"Data2"/"x.bin",all)==utf8(f.dir.wstring()));
    fails([&]{edit_root(p,"save","",utf8(nested.wstring()),"",false);});
    edit_root(p,"delete",utf8(nested.wstring()),utf8(nested.wstring()),"",false);
    assert(read_file(p).find("# 保留注释")!=std::string::npos);
    assert(path_key(L"C:/İ/中文/../A")==path_key(L"c:\\İ\\a"));
    assert(path_key(L"C:/Data/")==path_key(L"c:\\data"));
    assert(!is_under(path_key(L"C:/Data2/a"),path_key(L"C:/Data")));
    View view;
    EventLog log;
    for(int i=0;i<501;++i) {
        all.entries.push_back({f.dir/std::to_wstring(i),"",false});
        log.accept(Json({{"event","done"},{"path",utf8((f.dir/std::to_wstring(i)/"file").wstring())},{"ts",i}}));
    }
    auto rows=task_rows(log,all,view); assert(rows["total"]==501 && rows["rows"].size()==100 && rows["pages"]==6);
    assert(directory_rows(all,Json({{"passwords",Json::array()}}),Json::object()).size()==503);
}
}
int wmain(int argc,wchar_t** argv) {
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(argc==3 && std::wstring(argv[1])==L"--mutex") { std::cout<<utf8(mutex_name(argv[2]))<<"\n"<<utf8(mutex_name(argv[2],L"SunPackWatchRoots"))<<"\n"; return 0; }
    if(argc>=2 && std::wstring(argv[1])==L"--echo") { Json args=Json::array(); for(int i=2;i<argc;++i) args.push_back(utf8(argv[i])); std::cout<<args.dump(); return 7; }
    if(argc==4 && std::wstring(argv[1])==L"--password-add") { edit_password(argv[2],"add",Json(),Json({{"value",utf8(argv[3])}})); return 0; }
    try { Fixture f; projection(f); editing(f); roots(f); installer_upgrade(f); std::cout<<"GUI file contracts passed\n"; }
    catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
    CliCommand cli{executable_path(),fs::temp_directory_path(),{L"--echo"}};
    Handle stop(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    auto output=run_cli(cli,{L"watch",L"start",L"中文 空格",L"quote\"tail\\"},stop);
    assert(output.code==7 && parse_json(output.output)==Json::array({"watch","start","中文 空格","quote\"tail\\"}));
    CoUninitialize(); return 0;
}

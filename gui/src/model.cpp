#include "model.hpp"
#include "presentation.hpp"
#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>
#include <iomanip>
#include <ctime>
#include <cmath>
#include <optional>

namespace spg {
namespace {
std::string trim(std::string s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    return s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
}
bool contains(const std::string& text, const std::string& query) {
    // Search uses the same non-expanding Windows casing as path identity.
    auto fold = [](std::string s) {
        auto w = wide(s);
        CharLowerBuffW(w.data(), DWORD(w.size()));
        return w;
    };
    return query.empty() || fold(text).find(fold(query)) != std::wstring::npos;
}
bool absolute_windows(const std::string& value) {
    auto p = wide(value); std::replace(p.begin(), p.end(), L'/', L'\\');
    if (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && p[2] == L'\\') return true;
    if (p.rfind(L"\\\\", 0) != 0) return false;
    auto slash = p.find(L'\\', 2);
    return slash != std::wstring::npos && slash > 2 && slash+1 < p.size() && p[slash+1] != L'\\';
}
std::wstring scope_key(const fs::path& path) {
    auto key=path_key(path);
    while(!key.empty() && (key.back()==L'\\' || key.back()==L'/')) key.pop_back();
    return key;
}
void check_text(const Json& value) {
    if (!value.is_string() || value.get_ref<const std::string&>().find('\0') != std::string::npos)
        throw std::runtime_error("密码及作用域必须是无 NUL 的有效 Unicode 字符串");
    wide(value.get<std::string>());
}
std::string pointer_escape(const std::string& key) {
    std::string out;
    for (char c : key) out += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1,c);
    return out;
}
// Locate original JSON tokens after syntax validation. Splicing just the edited
// token preserves unknown fields, formatting, and numbers beyond uint64 exactly.
struct JsonTokens {
    const std::string& source;
    size_t at=0;
    bool collect_members=false;
    std::unordered_map<std::string,std::pair<size_t,size_t>> ranges;
    std::unordered_map<std::string,std::pair<size_t,size_t>> members;
    void space() { while(at<source.size() && (source[at]==' ' || source[at]=='\t' || source[at]=='\r' || source[at]=='\n')) ++at; }
    void string() {
        ++at;
        while(at<source.size()) { auto c=source[at++]; if(c=='\\') ++at; else if(c=='"') return; }
    }
    void value(const std::string& path) {
        space(); auto start=at; auto c=source.at(at);
        if(c=='{') {
            ++at; space();
            while(source.at(at)!='}') {
                auto begin=at; string();
                auto key=parse_json(source.substr(begin,at-begin)).get<std::string>();
                auto child=path+"/"+pointer_escape(key);
                space(); ++at; value(child);
                if(collect_members) members[child]={begin,at-begin}; space();
                if(source.at(at)!=',') break; ++at; space();
            }
            ++at;
        } else if(c=='[') {
            ++at; space(); size_t index=0;
            while(source.at(at)!=']') {
                value(path+"/"+std::to_string(index++)); space();
                if(source.at(at)!=',') break; ++at; space();
            }
            ++at;
        } else if(c=='"') string();
        else while(at<source.size() && std::string(",]} \t\r\n").find(source[at])==std::string::npos) ++at;
        ranges[path]={start,at-start};
    }
    explicit JsonTokens(const std::string& text,bool properties=false):source(text),collect_members(properties) { if(text.rfind("\xef\xbb\xbf",0)==0) at=3; value(""); }
    std::string token(const std::string& path) const { auto r=ranges.at(path); return source.substr(r.first,r.second); }
};
std::string text_at(HANDLE f, uint64_t offset, size_t count) {
    LARGE_INTEGER pos{}; pos.QuadPart = offset;
    if (!SetFilePointerEx(f, pos, nullptr, FILE_BEGIN)) win_error("Seek log");
    std::string out(count, 0); DWORD got = 0;
    if (count && !ReadFile(f, out.data(), DWORD(count), &got, nullptr)) win_error("Read log");
    out.resize(got); return out;
}
std::string event_status(const std::string& event) {
    static const std::unordered_map<std::string,std::string> names = {
        {"candidate_active","等待处理"}, {"candidate_busy","等待文件释放"}, {"candidate_quiet","准备处理"},
        {"processing_started","正在处理"}, {"done","成功"}, {"failed_terminal","失败"}, {"partial_rejected","失败"},
        {"failed_password","挂起：等待密码"}, {"suspended_missing_volume","挂起：等待分卷"},
        {"retry_password_failure","等待重试"}, {"no_tasks_found","跳过"}, {"pipeline_request_coalesced","已合并"}};
    auto it = names.find(event); return it == names.end() ? "" : it->second;
}
using RootIndex=std::vector<std::pair<std::wstring,std::string>>;
RootIndex index_roots(const WatchRoots& roots) {
    RootIndex index; index.reserve(roots.entries.size());
    for(auto& root:roots.entries) index.emplace_back(path_key(root.input),utf8(root.input.wstring()));
    std::stable_sort(index.begin(),index.end(),[](auto& a,auto& b){return a.first.size()>b.first.size();});
    return index;
}
std::string indexed_root(const std::wstring& key,const RootIndex& index) {
    for(auto& root:index) if(is_under(key,root.first)) return root.second;
    return "@other";
}
std::optional<double> iso_timestamp(const std::string& text) {
    std::tm time{}; std::istringstream in(text);
    in>>std::get_time(&time,"%Y-%m-%dT%H:%M:%S");
    if(in.fail()) return {};
    double seconds=double(_mkgmtime64(&time));
    size_t at=19;
    if(at<text.size() && text[at]=='.') {
        char* end=nullptr; seconds+=std::strtod(text.c_str()+at,&end); at=size_t(end-text.c_str());
    }
    if(at+5<text.size() && (text[at]=='+' || text[at]=='-')) {
        auto zone=text.substr(at+1,2)+text.substr(at+4,2);
        if(zone.find_first_not_of("0123456789")==std::string::npos) {
            int minutes=std::stoi(zone.substr(0,2))*60+std::stoi(zone.substr(2,2));
            seconds+=text[at]=='+'?-minutes*60:minutes*60;
        }
    }
    return seconds;
}
std::string task_time(const Task& task) {
    if(!task.has_timestamp || !std::isfinite(task.timestamp) || task.timestamp<0 || task.timestamp>32535215999.0) return {};
    auto seconds=static_cast<__time64_t>(task.timestamp);
    std::tm local{};
    if(_localtime64_s(&local,&seconds)) return {};
    char text[32]{};
    std::strftime(text,sizeof(text),"%Y-%m-%d %H:%M:%S",&local);
    return text;
}
}
Json parse_json(const std::string& text) {
    std::vector<std::set<std::string>> fields;
    return Json::parse(text, [&fields](int, Json::parse_event_t event, Json& value) {
        if (event == Json::parse_event_t::object_start) fields.emplace_back();
        else if (event == Json::parse_event_t::key && !fields.back().insert(value.get<std::string>()).second)
            throw std::runtime_error("JSON 包含重复字段");
        else if (event == Json::parse_event_t::object_end) fields.pop_back();
        return true;
    });
}
Json read_json(const fs::path& path) { return parse_json(read_file(path)); }
void validate_passwords(const Json& j) {
    if (!j.is_object() || !j.contains("passwords") || !j["passwords"].is_array())
        throw std::runtime_error("密码文件必须包含 passwords 数组");
    for (auto it=j.begin(); it!=j.end(); ++it)
        if (it.key() != "passwords" && it.key() != "clipboard_history") throw std::runtime_error("密码文件含未知字段");
    for (auto& record : j["passwords"]) {
        if (!record.is_object() || !record.contains("value")) throw std::runtime_error("密码记录缺少 value");
        check_text(record["value"]);
        for (auto it=record.begin(); it!=record.end(); ++it)
            if (it.key() != "value" && it.key() != "scopes") throw std::runtime_error("密码记录含未知字段");
        if (record.contains("scopes")) {
            if (!record["scopes"].is_array() || record["scopes"].empty()) throw std::runtime_error("目录密码的 scopes 不能为空");
            for (auto& scope : record["scopes"]) {
                check_text(scope);
                if (!absolute_windows(scope.get<std::string>())) throw std::runtime_error("密码作用域必须是 Windows 绝对目录");
            }
        }
    }
    if (j.contains("clipboard_history")) {
        if (!j["clipboard_history"].is_array()) throw std::runtime_error("clipboard_history 必须为数组");
        for (auto& item : j["clipboard_history"]) check_text(item);
    }
}
namespace {
struct ConfigEditError:std::runtime_error {
    std::string pointer;
    ConfigEditError(std::string pointer,const std::string& message):std::runtime_error(message),pointer(std::move(pointer)) {}
};
}
static std::string patch_config(std::string source,const Json& edits) {
    Json j=parse_json(source); JsonTokens tokens(source);
    struct Change { size_t start,length; std::string value; };
    std::vector<Change> changes;
    for(auto& edit:edits) {
        std::string pointer=edit.at("pointer"), original=edit.at("original"), type=edit.at("type"), text=edit.at("text");
        bool boolean=edit.at("boolean");
        try {
        Json::json_pointer target(pointer);
        if (!j.contains(target) || tokens.token(pointer) != original) throw std::runtime_error("该字段已被外部修改，请重新选择后保存");
        Json replacement = type == "string" ? Json(text) : type == "boolean" ? Json(boolean) : parse_json(text);
        if ((type == "number" && !replacement.is_number()) || (type == "array" && !replacement.is_array()) ||
            (type == "object" && !replacement.is_object())) throw std::runtime_error("输入类型与字段类型不一致");
        std::string module;
        auto parent=pointer.substr(0,pointer.find_last_of('/'));
        if(j.contains(Json::json_pointer(parent))) {
            const auto& owner=j.at(Json::json_pointer(parent));
            if(owner.is_object() && owner.contains("name") && owner["name"].is_string()) module=owner["name"].get<std::string>();
        }
        validate_setting(pointer,replacement,module);
        auto range=tokens.ranges.at(pointer);
        changes.push_back({range.first,range.second,type=="string" || type=="boolean"?replacement.dump():text});
        } catch(const std::exception& error) { throw ConfigEditError(pointer,error.what()); }
    }
    std::sort(changes.begin(),changes.end(),[](const Change& a,const Change& b){return a.start>b.start;});
    size_t boundary=source.size();
    for(auto& change:changes) {
        if(change.start+change.length>boundary) throw std::runtime_error("validation");
        source.replace(change.start,change.length,change.value); boundary=change.start;
    }
    return source;
}
void save_config_field(const fs::path& p,const std::string& pointer,const std::string& original,
    const std::string& type,const std::string& text,bool boolean) {
    FileMutex lock(p);
    atomic_write(p,patch_config(read_file(p),Json::array({Json{{"pointer",pointer},{"original",original},{"type",type},{"text",text},{"boolean",boolean}}})));
}
void upgrade_config_file(const fs::path& path) {
    FileMutex lock(path);
    if(!fs::exists(path)) return;
    auto source=read_file(path);
    auto document=parse_json(source);
    if(!document.is_object()) throw std::runtime_error("Configuration must be a JSON object");
    bool changed=false;
    auto erase_member=[&](const std::string& pointer) {
        JsonTokens tokens(source,true);
        auto range=tokens.members.at(pointer);
        size_t begin=range.first, end=range.first+range.second, next=end;
        auto whitespace=[](char c){return c==' ' || c=='\t' || c=='\r' || c=='\n';};
        while(next<source.size() && whitespace(source[next])) ++next;
        if(next<source.size() && source[next]==',') end=next+1;
        else {
            auto previous=begin;
            while(previous && whitespace(source[previous-1])) --previous;
            if(previous && source[previous-1]==',') begin=previous-1;
        }
        source.erase(begin,end-begin); changed=true;
    };
    // Removed by the structured-password refactor after the dev branch was
    // created. These paths are migration data, never GUI field aliases.
    const std::pair<const char*,std::vector<const char*>> retired[]={
        {"watch",{"directory_password_file_auto_create","password_retry_include_subtree"}},
        {"passwords",{"directory_passwords_enabled","directory_passwords_max_file_bytes","directory_passwords_max_password_length"}}
    };
    for(auto& section:retired) {
        if(!document.contains(section.first) || !document[section.first].is_object()) continue;
        auto& object=document[section.first]; bool removed=false;
        for(auto field:section.second) if(object.contains(field)) {
            erase_member(std::string("/")+section.first+"/"+field);
            object.erase(field); removed=true;
        }
        if(removed && object.empty()) { erase_member(std::string("/")+section.first); document.erase(section.first); }
    }
    if(changed) { parse_json(source); atomic_write(path,source); }
}
static void patch_password(Json& j, const std::string& op, const Json& original,
    const Json& replacement, const std::string& scope) {
    auto& records = j["passwords"];
    if (op == "add") records.push_back(replacement);
    else if(op=="addMany") {
        if(!replacement.is_array() || replacement.empty()) throw std::runtime_error("batchEmpty");
        // One production mutex and one atomic write for the complete batch. Exact
        // duplicates are ignored; whitespace and # are part of the password.
        std::set<std::string> existing;
        for(auto& record:records) existing.insert(record.dump());
        for(auto& record:replacement) if(existing.insert(record.dump()).second) records.push_back(record);
    }
    else {
        auto it = std::find(records.begin(), records.end(), original);
        if (it == records.end()) throw std::runtime_error("密码记录已被外部修改，请重新选择");
        if (op == "delete") records.erase(it);
        else if (op == "removeScope") {
            if (!it->contains("scopes") || scope.empty()) throw std::runtime_error("只能移除直接绑定的作用域");
            auto& scopes = (*it)["scopes"];
            auto end = std::remove_if(scopes.begin(), scopes.end(), [selected_scope=scope_key(wide(scope))](const Json& s) {
                return scope_key(wide(s.get<std::string>())) == selected_scope;
            });
            if (end == scopes.end()) throw std::runtime_error("当前目录不是直接作用域");
            scopes.erase(end, scopes.end());
            if (scopes.empty()) records.erase(it);
        } else if (op == "replace") *it = replacement;
        else throw std::runtime_error("未知密码操作");
    }
    validate_passwords(j);
}
void edit_password(const fs::path& p,const std::string& op,const Json& original,const Json& replacement,const std::string& scope) {
    FileMutex lock(p); auto document=read_json(p); validate_passwords(document);
    patch_password(document,op,original,replacement,scope); atomic_write(p,document.dump(2)+"\n");
}
Json apply_password_changes(Json document,const Json& edits) {
    validate_passwords(document);
    for(auto& edit:edits) {
        Json replacement={{"value",edit.value("value",std::string())}};
        auto scopes=edit.value("scopes",Json::array()); if(!scopes.empty()) replacement["scopes"]=scopes;
        auto op=edit.at("operation").get<std::string>();
        if(op=="addMany") {
            Json batch=Json::array();
            for(auto& value:edit.at("values")) { auto record=replacement; record["value"]=value; batch.push_back(std::move(record)); }
            replacement=std::move(batch);
        }
        auto original=edit.value("original",std::string());
        patch_password(document,op,original.empty()?Json():parse_json(original),replacement,edit.value("scope",std::string()));
    }
    return document;
}
WatchRoots parse_roots(const std::string& raw, const fs::path& base) {
    WatchRoots result; std::unordered_map<std::wstring,size_t> positions;
    std::string text = raw;
    if (text.rfind("\xef\xbb\xbf",0) == 0) text.erase(0,3);
    std::istringstream in(text); std::string line;
    while (std::getline(in, line)) {
        auto value = trim(line);
        if (value.empty()) continue;
        if (value[0]=='#') { if (!line.empty() && line.back()=='\r') line.pop_back(); result.comments.push_back(line); continue; }
        std::vector<std::string> parts; size_t at=0;
        for (;;) {
            auto sep=value.find('|',at); parts.push_back(trim(value.substr(at,sep==std::string::npos?sep:sep-at)));
            if (sep==std::string::npos) break; at=sep+1;
        }
        if (parts.size()>3) throw std::runtime_error("监控目录每行最多包含 input | output | deep_detect");
        if (parts[0].empty()) continue;
        std::string deep = parts.size()==3 ? parts[2] : "";
        std::transform(deep.begin(),deep.end(),deep.begin(),[](unsigned char c){ return char(tolower(c)); });
        if (!deep.empty() && deep!="true" && deep!="false") throw std::runtime_error("deep_detect 只能是 true 或 false");
        WatchRoot entry{resolve_path(wide(parts[0]),base), parts.size()>=2?parts[1]:"", deep=="true"};
        auto key=path_key(entry.input); auto it=positions.find(key);
        if (it==positions.end()) { positions[key]=result.entries.size(); result.entries.push_back(entry); }
        else { entry.input=result.entries[it->second].input; result.entries[it->second]=entry; }
    }
    return result;
}
WatchRoots read_roots(const fs::path& p) {
    if (!fs::exists(p)) return {};
    return parse_roots(read_file(p),p.parent_path());
}
WatchRoots apply_root_changes(WatchRoots roots,const fs::path& base,const Json& edits) {
    auto apply=[&](const Json& edit,const std::string& input,bool batch) {
        std::string op=edit.at("operation"), original=edit.value("original",std::string()), output=edit.at("output");
        bool deep=edit.at("deep");
        auto path = resolve_path(wide(input),base);
        if (input.empty() || input.find_first_of("|\r\n") != std::string::npos || output.find_first_of("|\r\n") != std::string::npos)
            throw std::runtime_error("目录不能为空且不能包含分隔符或换行");
        if (op!="delete" && !fs::is_directory(path)) throw std::runtime_error("监控目录必须是已存在的文件夹");
        auto previous = original.empty()?std::wstring():path_key(wide(original));
        auto it=std::find_if(roots.entries.begin(),roots.entries.end(),[&](auto& r){return path_key(r.input)==previous;});
        if (!original.empty() && it==roots.entries.end()) throw std::runtime_error("监控目录已被外部删除");
        if(!original.empty() && edit.contains("expected") && !edit["expected"].is_null() &&
            (it->output!=edit["expected"].value("output",std::string()) || it->deep!=edit["expected"].value("deep",false)))
            throw std::runtime_error("rootConflict");
        if (op=="delete") { if(it!=roots.entries.end()) roots.entries.erase(it); }
        else {
            auto key=path_key(path);
            auto duplicate=std::find_if(roots.entries.begin(),roots.entries.end(),[&](auto& r){return path_key(r.input)==key;});
            if (duplicate!=roots.entries.end() && duplicate!=it) {
                if(batch && duplicate->output==trim(output) && duplicate->deep==deep) return;
                throw std::runtime_error("监控目录已存在");
            }
            WatchRoot replacement{path,trim(output),deep};
            if(it==roots.entries.end()) roots.entries.push_back(replacement); else *it=replacement;
        }
    };
    for(auto& edit:edits) {
        if(edit.at("operation")=="addMany") {
            const auto& inputs=edit.at("inputs");
            if(!inputs.is_array() || inputs.empty()) throw std::runtime_error("rootsEmpty");
            for(auto& input:inputs) apply(edit,input.get<std::string>(),true);
        } else apply(edit,edit.at("input").get<std::string>(),false);
    }
    return roots;
}
static std::string roots_text(const WatchRoots& roots) {
    std::string content;
    for(auto& c:roots.comments) content+=c+"\n";
    for(auto& r:roots.entries) content+=utf8(r.input.wstring())+
        (r.deep?" | "+r.output+" | true":r.output.empty()?"":" | "+r.output)+"\n";
    return content;
}
void edit_root(const fs::path& p,const std::string& op,const std::string& original,const std::string& input,const std::string& output,bool deep) {
    FileMutex lock(p,L"SunPackWatchRoots");
    atomic_write(p,roots_text(apply_root_changes(read_roots(p),p.parent_path(),Json::array({Json{{"operation",op},{"original",original},{"input",input},{"output",output},{"deep",deep}}}))));
}
Json save_changes(const fs::path& dir,const Json& fields,const Json& passwords,const Json& roots) {
    Json result={{"saved",Json::array()},{"error",""},{"section",""}};
    struct Write { std::string section; fs::path path; std::string text; };
    std::vector<Write> writes;
    std::unique_ptr<FileMutex> config_lock,password_lock,roots_lock;
    std::string section;
    try {
        // Fixed acquisition order; runtime writers acquire only their own file.
        if(!fields.empty()) config_lock=std::make_unique<FileMutex>(dir/"sunpack_config.json");
        if(!passwords.empty()) password_lock=std::make_unique<FileMutex>(dir/"builtin_passwords.json");
        if(!roots.empty()) roots_lock=std::make_unique<FileMutex>(dir/"sunpack_watch_roots.txt",L"SunPackWatchRoots");
        if(!fields.empty()) { section="config"; auto path=dir/"sunpack_config.json"; writes.push_back({section,path,patch_config(read_file(path),fields)}); }
        if(!passwords.empty()) { section="passwords"; auto path=dir/"builtin_passwords.json"; writes.push_back({section,path,apply_password_changes(read_json(path),passwords).dump(2)+"\n"}); }
        if(!roots.empty()) { section="roots"; auto path=dir/"sunpack_watch_roots.txt"; writes.push_back({section,path,roots_text(apply_root_changes(read_roots(path),dir,roots))}); }
        for(auto& write:writes) {
            section=write.section; atomic_write(write.path,write.text); result["saved"].push_back(section);
        }
    } catch(const ConfigEditError& error) { result["error"]=error.what(); result["section"]=section; result["field"]=error.pointer; }
    catch(const std::exception& error) { result["error"]=error.what(); result["section"]=section; }
    return result;
}
std::string root_for(const fs::path& path,const WatchRoots& roots) {
    return indexed_root(path_key(path),index_roots(roots));
}
void EventLog::set_path(const fs::path& p) {
    if (path_==p) return;
    path_=p; offset_=identity_=sequence_=volume_=0; prefix_.clear(); anchor_.clear(); pending_.clear(); tasks.clear(); malformed=0;
}
void EventLog::accept(const Json& j) {
    if (!j.is_object() || !j.contains("event") || !j["event"].is_string() || !j.contains("path") || !j["path"].is_string()) return;
    auto event=j["event"].get<std::string>(), path=j["path"].get<std::string>();
    if (path.empty()) return;
    auto status=event_status(event); auto key=path_key(wide(path));
    if (status.empty()) return;
    Task t; t.path=path; t.status=status; t.sequence=++sequence_;
    if(j.contains("output_dirs") && j["output_dirs"].is_array())
        for(auto& output:j["output_dirs"]) if(output.is_string() && !output.get_ref<const std::string&>().empty()) t.outputs.push_back(output.get<std::string>());
    if(j.contains("ts") && j["ts"].is_number()) { t.timestamp=j["ts"].get<double>(); t.has_timestamp=std::isfinite(t.timestamp); if(!t.has_timestamp) t.timestamp=0; }
    else if(j.contains("time") && j["time"].is_string()) {
        if(auto parsed=iso_timestamp(j["time"].get<std::string>())) { t.timestamp=*parsed; t.has_timestamp=true; }
    }
    tasks[key]=std::move(t);
}
bool EventLog::refresh(bool missing_is_final) {
    retry_required=false;
    Handle f(CreateFileW(path_.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(f.value==INVALID_HANDLE_VALUE) {
        if(GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND) {
            if(!missing_is_final) { SetLastError(ERROR_FILE_NOT_FOUND); win_error("Read event log"); }
            bool changed=!tasks.empty(); offset_=identity_=volume_=0; prefix_.clear(); anchor_.clear(); pending_.clear(); tasks.clear(); malformed=0; return changed;
        }
        win_error("Read event log");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(f,&info)) win_error("Log identity");
    uint64_t id=(uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow;
    uint64_t size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
    bool reset=id!=identity_ || info.dwVolumeSerialNumber!=volume_ || size<offset_;
    if(!reset && !prefix_.empty()) reset=text_at(f,0,prefix_.size())!=prefix_;
    if(!reset && !anchor_.empty()) reset=text_at(f,offset_-anchor_.size(),anchor_.size())!=anchor_;
    bool changed=reset;
    if(reset) { offset_=sequence_=0; prefix_.clear(); anchor_.clear(); pending_.clear(); tasks.clear(); malformed=0; }
    identity_=id; volume_=info.dwVolumeSerialNumber;
    // Parse bounded chunks, retaining only the unfinished final record and one state per path.
    while(offset_<size) {
        auto chunk=text_at(f,offset_,size_t(std::min<uint64_t>(65536,size-offset_)));
        if(chunk.empty()) break;
        offset_+=chunk.size(); bytes_parsed+=chunk.size(); pending_+=chunk;
        size_t start=0, end;
        while((end=pending_.find('\n',start))!=std::string::npos) {
            try { auto event=parse_json(pending_.substr(start,end-start)); auto old=sequence_; accept(event); changed|=sequence_!=old; }
            catch(const std::exception&) { ++malformed; changed=true; }
            start=end+1;
        }
        pending_.erase(0,start);
        if(pending_.size()>16*1024*1024) throw std::runtime_error("日志单行超过大小限制");
    }
    if(prefix_.empty() && offset_) {
        prefix_=text_at(f,0,size_t(std::min<uint64_t>(4096,offset_)));
        auto line=prefix_.find('\n'); if(line!=std::string::npos) prefix_.resize(line+1);
    }
    anchor_=text_at(f,offset_-std::min<uint64_t>(256,offset_),size_t(std::min<uint64_t>(256,offset_)));
    BY_HANDLE_FILE_INFORMATION after{};
    if(!GetFileInformationByHandle(f,&after)) win_error("Log generation");
    if(after.nFileSizeHigh!=info.nFileSizeHigh || after.nFileSizeLow!=info.nFileSizeLow ||
        CompareFileTime(&after.ftLastWriteTime,&info.ftLastWriteTime)!=0) {
        // A write overlapped this read. Rebuild from the newly observed generation;
        // this is a response to actual I/O, never an idle retry timer.
        identity_=0; retry_required=true;
    }
    return changed;
}
Json task_rows(const EventLog& log,const WatchRoots& roots,const View& v,const std::string& language) {
    const auto root_index=index_roots(roots);
    std::vector<const Task*> rows;
    for(auto& pair:log.tasks) {
        auto& t=pair.second;
        if(!contains(t.path,v.task_query) || (!v.task_status.empty() && t.status!=v.task_status)) continue;
        if(!v.task_root.empty() && indexed_root(pair.first,root_index)!=v.task_root) continue;
        rows.push_back(&t);
    }
    std::sort(rows.begin(),rows.end(),[](auto a,auto b){
        if(a->timestamp!=b->timestamp) return a->timestamp>b->timestamp;
        return a->sequence>b->sequence;
    });
    size_t pages=std::max<size_t>(1,(rows.size()+99)/100), page=std::min(v.task_page,pages-1);
    Json result={{"rows",Json::array()},{"total",rows.size()},{"page",page},{"pages",pages},{"malformed",log.malformed}};
    for(size_t i=page*100;i<std::min(rows.size(),(page+1)*100);++i) {
        auto& t=*rows[i];
        const char* tone=t.status=="成功"?"success":t.status=="失败"?"failure":
            (t.status.rfind("等待",0)==0 || t.status.rfind("挂起",0)==0 || t.status=="准备处理")?"waiting":"info";
        result["rows"].push_back({{"id",utf8(path_key(wide(t.path)))},{"name",utf8(fs::path(wide(t.path)).filename().wstring())},
            {"path",t.path},{"root",indexed_root(path_key(wide(t.path)),root_index)},{"status",t.status},{"statusText",tr(t.status,language)},{"time",task_time(t)},
            {"statusTone",tone},{"outputs",t.outputs}});
    }
    return result;
}
fs::path task_directory(const Json& row) {
    auto root=row.value("root",std::string());
    auto input=fs::path(wide(row.value("path",std::string())));
    auto monitored=root.empty() || root=="@other"?input.parent_path():fs::path(wide(root));
    auto exists=[](const fs::path& path) { std::error_code error; return !path.empty() && fs::is_directory(path,error); };
    if(row.contains("outputs") && row["outputs"].is_array()) for(auto& output:row["outputs"]) {
        if(!output.is_string() || output.get_ref<const std::string&>().empty()) continue;
        auto directory=resolve_path(wide(output.get<std::string>()),monitored);
        if(exists(directory)) return directory;
    }
    if(exists(monitored)) return monitored;
    if(exists(input.parent_path())) return input.parent_path();
    throw std::runtime_error("路径当前不存在");
}
Json password_rows(const Json& passwords,const View& v,const std::string& language) {
    Json rows=Json::array();
    if(v.password_scope=="@history") {
        for(auto& p:passwords.value("clipboard_history",Json::array())) if(contains(p.get<std::string>(),v.password_query))
            rows.push_back({{"id",std::to_string(rows.size())},{"value",p},{"original",""},{"scopes",""}});
    } else {
        for(auto& p:passwords.value("passwords",Json::array())) {
            std::string scopes;
            if(p.contains("scopes")) for(auto& s:p["scopes"]) {
                auto scope=s.get<std::string>(); if(!scopes.empty()) scopes+="\n"; scopes+=scope;
            }
            if(!contains(p["value"].get<std::string>()+"\n"+(scopes.empty()?tr("allFolders",language):scopes),v.password_query)) continue;
            rows.push_back({{"id",p.dump()+"#"+std::to_string(rows.size())},{"value",p["value"]},{"original",p.dump()},{"scopes",scopes}});
        }
    }
    size_t pages=std::max<size_t>(1,(rows.size()+99)/100), page=std::min(v.password_page,pages-1);
    Json subset=Json::array();
    for(size_t i=page*100;i<std::min(rows.size(),(page+1)*100);++i) subset.push_back(std::move(rows[i]));
    return {{"rows",subset},{"total",rows.size()},{"page",page},{"pages",pages}};
}
Json config_fields(const Json& config,const std::string& source,const std::string& language) {
    Json rows=Json::array();
    if(config.empty()) return rows;
    std::unique_ptr<JsonTokens> tokens;
    if(!source.empty()) tokens=std::make_unique<JsonTokens>(source);
    std::function<void(const Json&,std::string,std::string)> walk;
    walk=[&](const Json& item,std::string pointer,std::string module) {
        if(item.is_object() && !item.empty()) {
            if(item.contains("name") && item["name"].is_string()) module=item["name"].get<std::string>();
            for(auto it=item.begin();it!=item.end();++it) if(it.key()!="name" || module.empty()) walk(it.value(),pointer+"/"+pointer_escape(it.key()),module);
            return;
        }
        if(item.is_array() && !item.empty() && std::all_of(item.begin(),item.end(),[](const Json& i){return i.is_object();})) {
            for(size_t i=0;i<item.size();++i) walk(item[i],pointer+"/"+std::to_string(i),module); return;
        }
        std::string type=item.is_boolean()?"boolean":item.is_string()?"string":item.is_number()?"number":item.is_array()?"array":item.is_object()?"object":"null";
        auto raw=tokens?tokens->token(pointer):item.dump(2);
        auto field=describe_setting(pointer,module,language);
        std::string editor=field["editor"], text=item.is_string()?item.get<std::string>():raw;
        bool lines=item.is_array() && std::all_of(item.begin(),item.end(),[](const Json& i){return i.is_string();});
        if(lines) { text.clear(); for(auto& i:item) { if(!text.empty()) text+='\n'; text+=i.get<std::string>(); } }
        if(editor.empty()) editor=!field["known"].get<bool>()?"readonly":!field["options"].empty()?"choice":lines?"lines":type=="boolean"?"toggle":type=="string"||type=="number"?"text":"readonly";
        int choice=-1;
        for(size_t i=0;i<field["options"].size();++i) if(field["options"][i]["value"]==item) choice=int(i);
        if(editor=="recursive" && choice<0) choice=2;
        field.update(Json{{"id",pointer},{"key",pointer+"@"+language},{"type",type},{"original",raw},{"text",text},{"boolean",item.is_boolean()?item.get<bool>():false},
            {"editor",editor},{"choice",choice}});
        rows.push_back(std::move(field));
    };
    walk(config,"",""); return rows;
}
Json directory_rows(const WatchRoots& roots,const Json& config) {
    Json rows=Json::array();
    std::string default_out=".";
    if(config.contains("watch") && config["watch"].is_object() && config["watch"].contains("out_dir") && config["watch"]["out_dir"].is_string()) default_out=config["watch"]["out_dir"].get<std::string>();
    for(auto& r:roots.entries) {
        auto key=path_key(r.input);
        rows.push_back({{"id",utf8(key)},{"path",utf8(r.input.wstring())},{"name",utf8(r.input.filename().wstring())},
            {"watch",true},{"output",r.output},{"effectiveOutput",utf8(resolve_path(wide(r.output.empty()?default_out:r.output),r.input).wstring())},{"deep",r.deep}});
    }
    return rows;
}
fs::path log_directory(const fs::path& dir,const Json& config) {
    if(config.contains("watch") && config["watch"].is_object() && config["watch"].contains("state_dir") && config["watch"]["state_dir"].is_string()) {
        auto s=trim(config["watch"]["state_dir"].get<std::string>()); if(!s.empty()) return resolve_path(wide(s),dir);
    }
    return dir/".sunpack_watch";
}
}

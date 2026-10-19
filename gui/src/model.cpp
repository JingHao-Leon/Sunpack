#include "model.hpp"
#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>
#include <iomanip>
#include <ctime>

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
    std::unordered_map<std::string,std::pair<size_t,size_t>> ranges;
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
                space(); ++at; value(path+"/"+pointer_escape(key)); space();
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
    explicit JsonTokens(const std::string& text):source(text) { if(text.rfind("\xef\xbb\xbf",0)==0) at=3; value(""); }
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
double iso_timestamp(const std::string& text) {
    std::tm time{}; std::istringstream in(text);
    in>>std::get_time(&time,"%Y-%m-%dT%H:%M:%S");
    if(in.fail()) return 0;
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
void save_config_field(const fs::path& p, const std::string& pointer, const std::string& original,
    const std::string& type, const std::string& text, bool boolean) {
    FileMutex lock(p);
    auto source=read_file(p); Json j=parse_json(source); JsonTokens tokens(source);
    Json::json_pointer target(pointer);
    if (!j.contains(target) || tokens.token(pointer) != original) throw std::runtime_error("该字段已被外部修改，请重新选择后保存");
    Json replacement = type == "string" ? Json(text) : type == "boolean" ? Json(boolean) : parse_json(text);
    if ((type == "number" && !replacement.is_number()) || (type == "array" && !replacement.is_array()) ||
        (type == "object" && !replacement.is_object())) throw std::runtime_error("输入类型与字段类型不一致");
    auto range=tokens.ranges.at(pointer);
    source.replace(range.first,range.second,type=="string" || type=="boolean"?replacement.dump():text);
    atomic_write(p,source);
}
void edit_password(const fs::path& p, const std::string& op, const Json& original,
    const Json& replacement, const std::string& scope) {
    FileMutex lock(p);
    Json j = read_json(p); validate_passwords(j);
    auto& records = j["passwords"];
    if (op == "add") records.push_back(replacement);
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
    atomic_write(p, j.dump(2) + "\n");
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
void edit_root(const fs::path& p, const std::string& op, const std::string& original,
    const std::string& input, const std::string& output, bool deep) {
    FileMutex lock(p,L"SunPackWatchRoots");
    auto roots = read_roots(p);
    auto path = resolve_path(wide(input),p.parent_path());
    if (input.empty() || input.find_first_of("|\r\n") != std::string::npos || output.find_first_of("|\r\n") != std::string::npos)
        throw std::runtime_error("目录不能为空且不能包含分隔符或换行");
    if (op!="delete" && !fs::is_directory(path)) throw std::runtime_error("监控目录必须是已存在的文件夹");
    auto previous = original.empty()?std::wstring():path_key(wide(original));
    auto it=std::find_if(roots.entries.begin(),roots.entries.end(),[&](auto& r){return path_key(r.input)==previous;});
    if (!original.empty() && it==roots.entries.end()) throw std::runtime_error("监控目录已被外部删除");
    if (op=="delete") { if(it!=roots.entries.end()) roots.entries.erase(it); }
    else {
        auto key=path_key(path);
        auto duplicate=std::find_if(roots.entries.begin(),roots.entries.end(),[&](auto& r){return path_key(r.input)==key;});
        if (duplicate!=roots.entries.end() && duplicate!=it) throw std::runtime_error("监控目录已存在");
        WatchRoot replacement{path,trim(output),deep};
        if(it==roots.entries.end()) roots.entries.push_back(replacement); else *it=replacement;
    }
    std::string content;
    for(auto& c:roots.comments) content+=c+"\n";
    for(auto& r:roots.entries) content+=utf8(r.input.wstring())+
        (r.deep?" | "+r.output+" | true":r.output.empty()?"":" | "+r.output)+"\n";
    atomic_write(p,content);
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
    if (event=="cleanup") { auto it=tasks.find(key); if(it!=tasks.end()) it->second.cleanup=j; return; }
    if (status.empty()) return;
    Task t; t.path=path; t.status=status; t.details=j; t.sequence=++sequence_;
    if(j.contains("time") && j["time"].is_string()) t.time=j["time"].get<std::string>();
    if(j.contains("ts") && j["ts"].is_number()) t.timestamp=j["ts"].get<double>();
    else if(!t.time.empty()) t.timestamp=iso_timestamp(t.time);
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
            try { auto event=parse_json(pending_.substr(start,end-start)); auto old=sequence_; accept(event); changed|=sequence_!=old || event.value("event",std::string())=="cleanup"; }
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
Json task_rows(const EventLog& log,const WatchRoots& roots,const View& v) {
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
        auto& t=*rows[i]; auto details=t.details;
        if(!t.cleanup.is_null()) details["cleanup"]=t.cleanup;
        Json outputs=Json::array();
        if(t.details.contains("output_dirs") && t.details["output_dirs"].is_array())
            for(auto& output:t.details["output_dirs"]) if(output.is_string()) outputs.push_back(output);
        std::string summary=t.details.value("event",std::string());
        if(t.details.contains("success_count")) summary="成功项数："+t.details["success_count"].dump();
        if(t.details.contains("error")) summary=t.details["error"].is_string()?t.details["error"].get<std::string>():t.details["error"].dump();
        result["rows"].push_back({{"id",utf8(path_key(wide(t.path)))},{"name",utf8(fs::path(wide(t.path)).filename().wstring())},
            {"path",t.path},{"root",indexed_root(path_key(wide(t.path)),root_index)},{"status",t.status},{"time",t.time.empty()?(t.details.contains("ts")?t.details["ts"].dump():""):t.time},
            {"summary",summary},{"outputs",outputs},
            {"details",details.dump(2)}});
    }
    return result;
}
Json password_rows(const Json& passwords,const View& v) {
    Json rows=Json::array();
    if(v.password_scope=="@history") {
        for(auto& p:passwords.value("clipboard_history",Json::array())) if(contains(p.get<std::string>(),v.password_query))
            rows.push_back({{"id",std::to_string(rows.size())},{"value",p},{"source","剪贴板历史（只读）"},{"original",""},{"scopes",""},{"direct",false}});
    } else {
        for(auto& p:passwords.value("passwords",Json::array())) {
            if(!contains(p["value"].get<std::string>(),v.password_query)) continue;
            bool global=!p.contains("scopes"), direct=global; std::string source=global?"全局密码":"";
            std::string scopes;
            if(!global) for(auto& s:p["scopes"]) {
                auto scope=s.get<std::string>(); if(!scopes.empty()) scopes+="\n"; scopes+=scope;
                if(v.password_scope.size() && v.password_scope[0]!='@' && is_under(scope_key(wide(v.password_scope)),scope_key(wide(scope)))) {
                    if(scope_key(wide(v.password_scope))==scope_key(wide(scope))) { direct=true; source="直接绑定"; }
                    else if(!direct) source="继承自 "+scope;
                }
            }
            bool include=v.password_scope=="@all" || (v.password_scope=="@global" && global) || (v.password_scope[0]!='@' && !source.empty());
            if(include) rows.push_back({{"id",p.dump()+"#"+std::to_string(rows.size())},{"value",p["value"]},{"source",source.empty()?"目录密码":source},
                {"original",p.dump()},{"scopes",scopes},{"direct",direct && !global}});
        }
    }
    size_t pages=std::max<size_t>(1,(rows.size()+99)/100), page=std::min(v.password_page,pages-1);
    Json subset=Json::array();
    for(size_t i=page*100;i<std::min(rows.size(),(page+1)*100);++i) subset.push_back(std::move(rows[i]));
    return {{"rows",subset},{"total",rows.size()},{"page",page},{"pages",pages}};
}
Json config_fields(const Json& config,const std::string& source) {
    Json rows=Json::array();
    if(config.empty()) return rows;
    std::unique_ptr<JsonTokens> tokens;
    if(!source.empty()) tokens=std::make_unique<JsonTokens>(source);
    std::function<void(const Json&,std::string,std::string,std::string)> walk;
    walk=[&](const Json& item,std::string pointer,std::string label,std::string category) {
        if(item.is_object() && !item.empty()) { for(auto it=item.begin();it!=item.end();++it) walk(it.value(),pointer+"/"+pointer_escape(it.key()),label.empty()?it.key():label+"."+it.key(),category.empty()?it.key():category); return; }
        std::string type=item.is_boolean()?"boolean":item.is_string()?"string":item.is_number()?"number":item.is_array()?"array":item.is_object()?"object":"null";
        auto raw=tokens?tokens->token(pointer):item.dump(2);
        rows.push_back({{"id",pointer},{"label",label},{"category",category},{"type",type},{"original",raw},
            {"text",item.is_string()?item.get<std::string>():raw},{"boolean",item.is_boolean()?item.get<bool>():false}});
    };
    walk(config,"","",""); return rows;
}
Json directory_rows(const WatchRoots& roots,const Json& passwords,const Json& config) {
    Json rows=Json::array(); std::set<std::wstring> seen;
    std::string default_out=".";
    if(config.contains("watch") && config["watch"].is_object() && config["watch"].contains("out_dir") && config["watch"]["out_dir"].is_string()) default_out=config["watch"]["out_dir"].get<std::string>();
    for(auto& r:roots.entries) {
        auto key=path_key(r.input); seen.insert(scope_key(r.input));
        rows.push_back({{"id",utf8(key)},{"path",utf8(r.input.wstring())},{"name",utf8(r.input.filename().wstring())},
            {"watch",true},{"output",r.output},{"effectiveOutput",utf8(resolve_path(wide(r.output.empty()?default_out:r.output),r.input).wstring())},{"deep",r.deep}});
    }
    for(auto& p:passwords.value("passwords",Json::array())) if(p.contains("scopes")) for(auto& scope:p["scopes"]) {
        auto path=wide(scope.get<std::string>()); auto key=scope_key(path);
        if(seen.insert(key).second) rows.push_back({{"id",utf8(key)},{"path",scope},{"name",utf8(fs::path(path).filename().wstring())},{"watch",false}});
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

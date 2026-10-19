#include "presentation.hpp"
#include "catalog_json.h"
#include <algorithm>
#include <cmath>

namespace spg {
namespace {
const Json& catalog() { static const auto data=parse_json(kCatalogJson); return data; }
std::string local(const Json& value,const std::string& language) {
    return value.value(language,value.value("en",std::string()));
}
const Json& setting(const std::string& pointer,const std::string& module) {
    const auto& fields=catalog()["fields"];
    auto key=pointer;
    // Named arrays may be reordered or partially copied into the ordinary
    // config. Descriptions follow canonical module names, never array indexes
    // or global leaf-name aliases that could recognize retired fields.
    for(const std::string prefix:{"/filesystem/scan_filters/","/analysis/modules/","/verification/methods/"}) {
        if(pointer.rfind(prefix,0)!=0) continue;
        auto suffix=pointer.find('/',prefix.size());
        if(suffix!=std::string::npos && !module.empty()) key=prefix+"@"+module+pointer.substr(suffix);
        break;
    }
    if(fields.contains(key)) return fields[key];
    static const Json empty=Json::object(); return empty;
}
}
std::string gui_language(const Json& config) {
    if(config.contains("cli") && config["cli"].is_object() && config["cli"].contains("language") && config["cli"]["language"].is_string()) {
        auto value=config["cli"]["language"].get<std::string>();
        value.erase(0,value.find_first_not_of(" \t\r\n"));
        auto end=value.find_last_not_of(" \t\r\n"); if(end!=std::string::npos) value.resize(end+1);
        std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return char(tolower(c));});
        if(value=="zh") return "zh";
    }
    return "en"; // Same fallback as CLI normalize_language.
}
std::string tr(const std::string& key,const std::string& language) {
    const auto& texts=catalog()["ui"];
    return texts.contains(key)?local(texts[key],language):key;
}
Json interface_texts(const std::string& language) {
    Json result=Json::object();
    for(auto it=catalog()["ui"].begin();it!=catalog()["ui"].end();++it) result[it.key()]=local(it.value(),language);
    return result;
}
Json describe_setting(const std::string& pointer,const std::string& module,const std::string& language) {
    const auto& meta=setting(pointer,module);
    auto group=pointer.substr(1,pointer.find('/',1)==std::string::npos?std::string::npos:pointer.find('/',1)-1);
    auto category=group=="recursive_authorization"?"recursive_extract":group;
    auto& groups=catalog()["groups"];
    std::string label=meta.contains("label")?local(meta["label"],language):tr("readOnly",language);
    if(!module.empty()) {
        const auto& modules=catalog()["modules"];
        label=(modules.contains(module)?local(modules[module],language):module)+" · "+label;
    }
    Json result={{"label",label},{"category",category},{"categoryLabel",groups.contains(category)?local(groups[category],language):tr("readOnly",language)},
        {"description",meta.contains("description")?local(meta["description"],language):tr("unknownDescription",language)},
        {"editor",meta.value("editor",std::string())},{"options",Json::array()},{"known",!meta.empty()}};
    if(meta.contains("options")) for(auto& option:meta["options"]) result["options"].push_back({{"value",option["value"]},{"label",local(option["label"],language)}});
    return result;
}
void validate_setting(const std::string& pointer,const Json& value,const std::string& module) {
    const auto& meta=setting(pointer,module);
    if(pointer=="/recursive_extract") {
        auto text=value.is_string()?value.get<std::string>():value.dump();
        if(text=="*" || text=="?") return;
        if(text.empty() || text.find_first_not_of("0123456789")!=std::string::npos || text.find_first_not_of('0')==std::string::npos)
            throw std::runtime_error("validation");
    } else if(meta.contains("options")) {
        if(std::none_of(meta["options"].begin(),meta["options"].end(),[&](const Json& o){return o["value"]==value;})) throw std::runtime_error("validation");
    }
    if(value.is_number()) {
        double number=value.get<double>();
        if(!std::isfinite(number) || (meta.contains("min") && !meta["min"].is_null() && (meta.value("exclusive",false)?number<=meta["min"].get<double>():number<meta["min"].get<double>())) ||
            (meta.contains("max") && !meta["max"].is_null() && number>meta["max"].get<double>()) || (meta.value("integer",false) && std::floor(number)!=number)) throw std::runtime_error("validation");
    }
}
std::string translated_error(const std::string& message,const std::string& language) {
    if(catalog()["ui"].contains(message)) return tr(message,language);
    // Keep OS / parser diagnostics, translated application errors come from the catalog.
    if(language=="zh") return message;
    static const std::pair<const char*,const char*> errors[]={
        {"该字段已被外部修改，请重新选择后保存","This setting changed in another program. Refresh and edit it again."},
        {"输入类型与字段类型不一致","The value has the wrong type for this setting."},
        {"密码记录已被外部修改，请重新选择","This password changed in another program. Refresh and edit it again."},
        {"密码作用域必须是 Windows 绝对目录","Password folders must use absolute Windows paths."},
        {"目录不能为空且不能包含分隔符或换行","The folder must not be empty or contain separators or line breaks."},
        {"监控目录必须是已存在的文件夹","Choose an existing folder to monitor."},
        {"监控目录已存在","This folder is already monitored."},
        {"监控目录已被外部删除","This monitored folder was removed in another program."},
        {"路径当前不存在","The path does not currently exist."},
        {"无法在资源管理器中打开路径","Could not open the path in Explorer."},
        {"未找到 SunPack CLI；请安装完整发行包或传入 --cli","SunPack CLI was not found. Install the complete package or specify --cli."},
        {"测试数据目录未绑定 CLI；请显式传入 --cli","This data folder has no CLI configured. Specify --cli."},
        {"配置顶层必须是 JSON 对象","The configuration must be a JSON object."},
        {"JSON 包含重复字段","The JSON contains duplicate properties."},
        {"文件超过可编辑大小限制","The file exceeds the editable size limit."},
        {"目录不支持文件变化通知","The folder does not support file change notifications."},
        {"无法打开目录选择器","Could not open the folder picker."},
        {"日志单行超过大小限制","A log entry exceeds the size limit."},
        {"目录密码的 scopes 不能为空","A folder password needs at least one folder."},
        {"密码及作用域必须是无 NUL 的有效 Unicode 字符串","Passwords and folders must be valid Unicode without NUL characters."},
        {"密码文件必须包含 passwords 数组","The password file must contain a passwords array."},
        {"密码文件含未知字段","The password file contains an unknown property."},
        {"密码记录缺少 value","A password record is missing its value."},
        {"密码记录含未知字段","A password record contains an unknown property."},
        {"clipboard_history 必须为数组","Clipboard history must be an array."},
        {"只能移除直接绑定的作用域","Only directly assigned folders can be removed."},
        {"当前目录不是直接作用域","This folder is inherited and cannot be removed here."},
        {"监控目录每行最多包含 input | output | deep_detect","Each monitored-folder line accepts input | output | deep_detect."},
        {"deep_detect 只能是 true 或 false","Deep detection must be true or false."},
    };
    for(auto& item:errors) if(message==item.first) return item.second;
    return message;
}
}

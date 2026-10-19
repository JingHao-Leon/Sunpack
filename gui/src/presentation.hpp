#pragma once
#include "model.hpp"

namespace spg {
std::string gui_language(const Json& config);
std::string tr(const std::string& key, const std::string& language);
std::string translated_error(const std::string& message, const std::string& language);
Json interface_texts(const std::string& language);
Json describe_setting(const std::string& pointer, const std::string& module,
    const std::string& language);
void validate_setting(const std::string& pointer, const Json& value, const std::string& module = {});
}

#pragma once
#include <string>
#include <map>
#include <utility>

bool StartTrayProcessIfNeeded();
bool SendToTray(const std::string& message, std::string& outResponse);

std::pair<bool, std::string>
SendVerb(const std::string& verb,
    const std::map<std::string, std::string>& kv);

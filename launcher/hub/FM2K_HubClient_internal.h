#pragma once
// FM2K_HubClient.cpp minimal JSON helpers, shared across the split
// FM2K_HubClient_*.cpp TUs (json defines them; outbound/transport build
// outbound messages with EscapeJsonString; dispatch parses with GetStr/
// GetInt/GetSub). Promoted from the original file-local anonymous namespace --
// these names are unique to the hub client in the launcher (verified no
// free-function collisions; ImGuiStorage::GetInt is an unrelated member).
#include "FM2K_HubClient.h"   // HubUser / HubRoom
#include <string>
#include <vector>

namespace fm2k {

// Scalar JSON extractors.
std::string EscapeJsonString(const std::string& s);
std::string GetStr(const std::string& s, const std::string& key);
int         GetInt(const std::string& s, const std::string& key, int def = 0);
bool        GetBool(const std::string& s, const std::string& key, bool def = false);
std::string GetSub(const std::string& s, const std::string& key);

// Object/array extractors (json defines; dispatch uses).
std::vector<std::string> SplitObjectArray(const std::string& arr);
std::vector<std::string> SplitStringArray(const std::string& arr);
HubUser      ParseUser(const std::string& obj);
HubRoom      ParseRoom(const std::string& obj);

}  // namespace fm2k

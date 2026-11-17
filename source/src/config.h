#pragma once
// key = value config (da2vr_config.txt next to the exe). Unknown keys are kept; defaults are written on first run.
#include <string>

void ConfigLoad(const wchar_t* path);
int ConfigInt(const char* key, int def);
float ConfigFloat(const char* key, float def);
std::string ConfigStr(const char* key, const char* def);
bool ConfigBool(const char* key, bool def);

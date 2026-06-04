#pragma once
#include <string>
#include <vector>
enum class WarnSeverity { Block, Warn };
struct Warning { WarnSeverity severity; std::string code; std::string message; };

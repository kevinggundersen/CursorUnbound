#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// CommonLibSSE-NG hard-errors if the Windows API is included before it (see
// REX/W32/BASE.h), because REX::W32 declares its own import thunks. Windows.h has to
// come after, and it does still get included - we need the real HCURSOR/HWND/WIC types.
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <Windows.h>

// Module enumeration for the ShowCursor import-table sweep. Needs Windows.h first.
#include <TlHelp32.h>

using namespace std::literals;

#pragma once

#define NOMMNOSOUND
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include <REL/Relocation.h>

#include <Windows.h>

using namespace std::literals;

#include "Logger.h"

using namespace f4cf;

#define DLLEXPORT __declspec(dllexport)

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

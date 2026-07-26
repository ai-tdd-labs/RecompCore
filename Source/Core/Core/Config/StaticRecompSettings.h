// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"

namespace Config
{
extern const Info<bool> MAIN_STATICRECOMP_MODULE;
extern const Info<bool> MAIN_STATICRECOMP_ALLOW_FALLBACK;
extern const Info<u32> MAIN_STATICRECOMP_IDLE_PC;
extern const Info<std::string> MAIN_STATICRECOMP_SYMBOL_MAP;
extern const Info<std::string> MAIN_STATICRECOMP_FUNCTION_PROFILE;
extern const Info<u64> MAIN_STATICRECOMP_FUNCTION_PROFILE_START_FRAME;
extern const Info<u64> MAIN_STATICRECOMP_FUNCTION_PROFILE_END_FRAME;
extern const Info<bool> MAIN_STATICRECOMP_TRACE_FUNCTIONS;
extern const Info<std::string> MAIN_STATICRECOMP_TRACE_FUNCTION;
}  // namespace Config

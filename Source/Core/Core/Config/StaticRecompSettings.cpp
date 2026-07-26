// SPDX-License-Identifier: GPL-2.0-or-later
#include "Core/Config/StaticRecompSettings.h"
#include "Core/System.h"

namespace Config
{
const Info<bool> MAIN_STATICRECOMP_MODULE{{System::Main, "Core", "StaticRecompModule"}, true};
const Info<bool> MAIN_STATICRECOMP_ALLOW_FALLBACK{
    {System::Main, "Core", "StaticRecompAllowFallback"}, true};
const Info<u32> MAIN_STATICRECOMP_IDLE_PC{{System::Main, "Core", "StaticRecompIdlePC"}, 0};
const Info<std::string> MAIN_STATICRECOMP_SYMBOL_MAP{
    {System::Main, "Core", "StaticRecompSymbolMap"}, ""};
const Info<std::string> MAIN_STATICRECOMP_FUNCTION_PROFILE{
    {System::Main, "Core", "StaticRecompFunctionProfile"}, ""};
const Info<bool> MAIN_STATICRECOMP_TRACE_FUNCTIONS{
    {System::Main, "Core", "StaticRecompTraceFunctions"}, false};
const Info<std::string> MAIN_STATICRECOMP_TRACE_FUNCTION{
    {System::Main, "Core", "StaticRecompTraceFunction"}, ""};
}  // namespace Config

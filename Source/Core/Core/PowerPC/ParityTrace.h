// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace PowerPC
{
// Emit CPU state at a boot boundary owned outside the interpreter run loop.
void DolphinParityTraceBootContext(Core::System& system, const char* action, u32 pc);

void DolphinParityTraceHardwareEvent(Core::System& system, const char* family,
                                     const char* action, u32 subject, u64 a,
                                     u64 b, u64 c, u64 d);
}  // namespace PowerPC

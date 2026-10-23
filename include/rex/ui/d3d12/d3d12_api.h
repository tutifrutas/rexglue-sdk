/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

// clang - format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ObjBase.h>

#include <DXProgrammableCapture.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <dxgidebug.h>
// For Microsoft::WRL::ComPtr.
#include <wrl/client.h>
#include <dxcapi.h>
#include "DxbcConverter.h"

#ifdef __MINGW32__
// The vendored DXC headers provide MSVC UUID attributes but not MinGW's
// __mingw_uuidof specializations used by IID_PPV_ARGS.
__CRT_UUID_DECL(IDxbcConverter, 0x5f956ed5, 0x78d1, 0x4b15,
                0x82, 0x47, 0xf7, 0x18, 0x76, 0x14, 0xa0, 0x41)
__CRT_UUID_DECL(IDxcCompiler, 0x8c210bf3, 0x011f, 0x4422,
                0x8d, 0x70, 0x6f, 0x9a, 0xcb, 0x8d, 0xb6, 0x17)
__CRT_UUID_DECL(IDxcUtils, 0x4605c4cb, 0x2019, 0x492a,
                0xad, 0xa4, 0x65, 0xf2, 0x0b, 0xb7, 0xd6, 0x7f)
#endif

// clang-format on

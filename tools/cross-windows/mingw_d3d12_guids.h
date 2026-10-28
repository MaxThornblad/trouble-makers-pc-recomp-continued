/* MinGW: DirectX-Headers' d3d12.h only carries IIDs via __declspec(uuid);
   dxguids.h turns them into __CRT_UUID_DECL specializations for GCC. */
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxguids/dxguids.h>

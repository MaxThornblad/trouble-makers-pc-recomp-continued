/* Force-included in the MinGW cross build: fills gaps between MinGW-w64 11
   headers and what MSVC-targeted code expects. */
#pragma once
#ifdef __MINGW32__
#ifndef _In_
#define _In_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Outptr_
#define _Outptr_
#endif
#ifndef _Maybenull_
#define _Maybenull_
#endif
#ifndef _declspec
#define _declspec(x) __declspec(x)
#endif
#ifdef __cplusplus
extern "C" __declspec(dllimport) long __stdcall SetThreadDescription(void* hThread, const wchar_t* lpThreadDescription);
#endif
#endif

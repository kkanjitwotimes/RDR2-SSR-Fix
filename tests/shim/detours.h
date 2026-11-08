#pragma once
#include "windows.h"
inline LONG DetourTransactionBegin() { return 0; }
inline LONG DetourUpdateThread(HANDLE) { return 0; }
inline LONG DetourAttach(PVOID*, PVOID) { return 0; }
inline LONG DetourTransactionCommit() { return 0; }
inline LONG DetourTransactionAbort() { return 0; }

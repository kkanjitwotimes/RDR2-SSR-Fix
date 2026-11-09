// Minimal fake of the Win32 API surface rdr2_overscan.cpp uses, so the real source can run natively in tests.
#pragma once
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>
#include <sys/time.h>
typedef uint32_t DWORD; typedef int BOOL; typedef void* HANDLE; typedef void* HMODULE; typedef void* LPVOID; typedef void* PVOID; typedef long LONG;
typedef const wchar_t* LPCWSTR; typedef const char* LPCSTR; typedef wchar_t* LPWSTR; typedef char* LPSTR; typedef long long (*FARPROC)(); typedef void* HWND;
typedef unsigned short WORD; typedef unsigned int UINT;
#define APIENTRY
#define WINAPI
#define TRUE 1
#define FALSE 0
#define NO_ERROR 0
#define MAX_PATH 260
#define DLL_PROCESS_ATTACH 1
#define GENERIC_WRITE 0x40000000
#define FILE_APPEND_DATA 4
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define CREATE_NEW 1
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_NORMAL 0x80
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; };
struct SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; };
struct WIN32_FILE_ATTRIBUTE_DATA { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; };
enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard };
union LARGE_INTEGER { struct { DWORD LowPart; LONG HighPart; }; long long QuadPart; };
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }

// test controls
inline bool g_fakeKeyDown = false;
inline DWORD g_fakeTid = 4242;
inline HANDLE CreateFileW(LPCWSTR path, DWORD, DWORD, void*, DWORD, DWORD, HANDLE) {
  std::string p; for (const wchar_t* c = path; *c; ++c) p += (char)*c;
  int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644); return fd < 0 ? INVALID_HANDLE_VALUE : (HANDLE)(intptr_t)(fd + 1);
}
inline BOOL WriteFile(HANDLE h, const void* d, DWORD n, DWORD* w, void*) { ssize_t r = ::write((int)(intptr_t)h - 1, d, n); if (w) *w = (DWORD)(r < 0 ? 0 : r); return r >= 0; }
inline BOOL CloseHandle(HANDLE h) { return ::close((int)(intptr_t)h - 1) == 0; }
inline DWORD GetFileAttributesW(LPCWSTR) { return INVALID_FILE_ATTRIBUTES; }
inline BOOL GetFileAttributesExW(LPCWSTR, GET_FILEEX_INFO_LEVELS, void*) { return FALSE; }
inline int CompareFileTime(const FILETIME* a, const FILETIME* b) { return std::memcmp(a, b, sizeof *a); }
inline UINT GetPrivateProfileIntW(LPCWSTR, LPCWSTR, int def, LPCWSTR) { return (UINT)def; }
inline DWORD GetPrivateProfileStringW(LPCWSTR, LPCWSTR, LPCWSTR def, LPWSTR out, DWORD n, LPCWSTR) { wcsncpy(out, def, n); return 0; }
inline void GetLocalTime(SYSTEMTIME* t) { timeval tv; gettimeofday(&tv, nullptr); tm l; localtime_r(&tv.tv_sec, &l); t->wYear = l.tm_year + 1900; t->wMonth = l.tm_mon + 1; t->wDay = l.tm_mday; t->wHour = l.tm_hour; t->wMinute = l.tm_min; t->wSecond = l.tm_sec; t->wMilliseconds = tv.tv_usec / 1000; }
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000000LL; return TRUE; }
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c) { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); c->QuadPart = ts.tv_sec * 1000000000LL + ts.tv_nsec; return TRUE; }
typedef unsigned long long ULONGLONG;
inline ULONGLONG GetTickCount64() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (ULONGLONG)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000; }
inline DWORD GetCurrentThreadId() { return g_fakeTid; }
inline DWORD GetCurrentProcessId() { return 777; }
inline HANDLE GetCurrentThread() { return nullptr; }
inline short GetAsyncKeyState(int) { return g_fakeKeyDown ? (short)0x8000 : 0; }
inline HWND GetForegroundWindow() { return (HWND)1; }
inline DWORD GetWindowThreadProcessId(HWND, DWORD* pid) { if (pid) *pid = 777; return 1; }
inline HMODULE GetModuleHandleW(LPCWSTR) { return nullptr; }
inline HMODULE LoadLibraryW(LPCWSTR) { return nullptr; }
inline FARPROC GetProcAddress(HMODULE, LPCSTR) { return nullptr; }
inline DWORD GetLastError() { return 0; }
inline BOOL DisableThreadLibraryCalls(HMODULE) { return TRUE; }
inline DWORD GetModuleFileNameW(HMODULE, LPWSTR, DWORD) { return 0; }
inline DWORD GetModuleFileNameA(HMODULE, LPSTR, DWORD) { return 0; }

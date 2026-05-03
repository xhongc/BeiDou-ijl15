#include "stdafx.h"

#include <cstdio>
#include <cstring>

namespace {
bool g_debugLogEnabled = true;

void BuildDebugLogPath(char* buffer, size_t bufferSize)
{
	if (buffer == nullptr || bufferSize == 0) {
		return;
	}

	DWORD length = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(bufferSize));
	if (length == 0 || length >= bufferSize) {
		strncpy_s(buffer, bufferSize, "ezorsia-debug.log", _TRUNCATE);
		return;
	}

	char* slash = strrchr(buffer, '\\');
	if (slash == nullptr) {
		slash = strrchr(buffer, '/');
	}

	if (slash == nullptr) {
		strncpy_s(buffer, bufferSize, "ezorsia-debug.log", _TRUNCATE);
		return;
	}

	strcpy_s(slash + 1, bufferSize - static_cast<size_t>((slash + 1) - buffer), "ezorsia-debug.log");
}
}

void SetEzorsiaDebugLogEnabled(bool enabled)
{
	g_debugLogEnabled = enabled;
}

void DebugLog(const char* format, ...)
{
	if (!g_debugLogEnabled || format == nullptr) {
		return;
	}

	char message[1024]{};
	va_list args;
	va_start(args, format);
	vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
	va_end(args);

	SYSTEMTIME now{};
	GetLocalTime(&now);

	char line[1200]{};
	_snprintf_s(
		line,
		sizeof(line),
		_TRUNCATE,
		"[%04u-%02u-%02u %02u:%02u:%02u.%03u][tid=%lu] %s\r\n",
		now.wYear,
		now.wMonth,
		now.wDay,
		now.wHour,
		now.wMinute,
		now.wSecond,
		now.wMilliseconds,
		GetCurrentThreadId(),
		message);

	char logPath[MAX_PATH]{};
	BuildDebugLogPath(logPath, sizeof(logPath));

	HANDLE file = CreateFileA(
		logPath,
		FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}

	DWORD written = 0;
	WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
	CloseHandle(file);
}

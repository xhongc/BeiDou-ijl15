#include "stdafx.h"
#include "DamageMeter.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {
constexpr unsigned short kOpcodeDamageMeterSync = 0x1001;
constexpr unsigned char kModeHidden = 0;
constexpr unsigned char kModeParty = 1;
constexpr DWORD kTooltipSetStringAddr = 0x008E6E7D;
constexpr DWORD kTooltipClearAddr = 0x008E6E23;
constexpr DWORD kTooltipDisposeAddr = 0x008E6BA3;
constexpr DWORD kTooltipCreateAddr = 0x008E49B5;
constexpr DWORD kTooltipBufferSize = 1304;
constexpr DWORD kSyncStaleMs = 3000;

struct DamageEntry {
    unsigned int characterId;
    std::string name;
    unsigned long long damage;
    double percent;
};

using UIToolTipSetString_t = void(__fastcall*)(int pThis, void* edx, int x, int y, const char* text);
using UIToolTipClear_t = void(__fastcall*)(int pThis, void* edx);

static auto s_SetToolTipString = reinterpret_cast<UIToolTipSetString_t>(kTooltipSetStringAddr);
static auto s_ClearToolTip = reinterpret_cast<UIToolTipClear_t>(kTooltipClearAddr);
static auto s_DisposeToolTip = reinterpret_cast<UIToolTipClear_t>(kTooltipDisposeAddr);
static auto s_CreateToolTip = reinterpret_cast<UIToolTipClear_t>(kTooltipCreateAddr);

static char s_toolTip[kTooltipBufferSize];
static std::vector<DamageEntry> s_entries;
static unsigned int s_sessionId = 0;
static unsigned char s_mode = kModeHidden;
static DWORD s_lastSyncTick = 0;
static bool s_enabled = true;
static int s_maxRows = 6;
static int s_offsetX = 0;
static int s_offsetY = 0;
static bool s_toolTipCreated = false;
static bool s_overlayVisible = false;

static bool ReadU8(const unsigned char* data, size_t size, size_t& cursor, unsigned char& out)
{
    if (cursor + 1 > size) {
        return false;
    }
    out = data[cursor];
    ++cursor;
    return true;
}

static bool ReadU16(const unsigned char* data, size_t size, size_t& cursor, unsigned short& out)
{
    if (cursor + 2 > size) {
        return false;
    }
    out = static_cast<unsigned short>(data[cursor] | (data[cursor + 1] << 8));
    cursor += 2;
    return true;
}

static bool ReadU32(const unsigned char* data, size_t size, size_t& cursor, unsigned int& out)
{
    if (cursor + 4 > size) {
        return false;
    }
    out = static_cast<unsigned int>(data[cursor]) |
        (static_cast<unsigned int>(data[cursor + 1]) << 8) |
        (static_cast<unsigned int>(data[cursor + 2]) << 16) |
        (static_cast<unsigned int>(data[cursor + 3]) << 24);
    cursor += 4;
    return true;
}

static bool ReadMapleString(const unsigned char* data, size_t size, size_t& cursor, std::string& out)
{
    unsigned short length = 0;
    if (!ReadU16(data, size, cursor, length)) {
        return false;
    }
    if (cursor + length > size) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data + cursor), length);
    cursor += length;
    return true;
}

static void ClearOverlay()
{
    s_overlayVisible = false;
}

static void ResetState()
{
    s_entries.clear();
    s_sessionId = 0;
    s_mode = kModeHidden;
    s_lastSyncTick = 0;
    ClearOverlay();
}

static int ClampRows(int value)
{
    if (value < 1) {
        return 1;
    }
    if (value > 10) {
        return 10;
    }
    return value;
}

static int ClampX(int value)
{
    if (value < 0) {
        return 0;
    }
    const int maxX = Client::m_nGameWidth > 40 ? Client::m_nGameWidth - 40 : 0;
    return value > maxX ? maxX : value;
}

static int ClampY(int value)
{
    if (value < 0) {
        return 0;
    }
    const int maxY = Client::m_nGameHeight > 40 ? Client::m_nGameHeight - 40 : 0;
    return value > maxY ? maxY : value;
}

static std::string FormatDamage(unsigned long long value)
{
    std::string digits = std::to_string(value);
    std::string formatted;
    formatted.reserve(digits.size() + digits.size() / 3);

    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && ((digits.size() - i) % 3 == 0)) {
            formatted.push_back(',');
        }
        formatted.push_back(digits[i]);
    }

    return formatted;
}

static void SortAndFinalizeEntries()
{
    std::sort(s_entries.begin(), s_entries.end(), [](const DamageEntry& left, const DamageEntry& right) {
        if (left.damage != right.damage) {
            return left.damage > right.damage;
        }
        return left.characterId < right.characterId;
    });

    unsigned long long totalDamage = 0;
    for (const auto& entry : s_entries) {
        totalDamage += entry.damage;
    }

    for (auto& entry : s_entries) {
        entry.percent = totalDamage == 0 ? 0.0 : (static_cast<double>(entry.damage) * 100.0 / static_cast<double>(totalDamage));
    }
}

static void ApplySnapshot(unsigned int sessionId, unsigned char mode, std::vector<DamageEntry>& parsedEntries)
{
    s_sessionId = sessionId;
    s_mode = mode;
    s_entries.swap(parsedEntries);
    SortAndFinalizeEntries();
    s_lastSyncTick = GetTickCount();

    if (s_mode == kModeHidden || s_entries.empty()) {
        ClearOverlay();
    }
}

static std::string BuildOverlayText()
{
    std::string text("Party Damage");
    const int rows = (std::min)(static_cast<int>(s_entries.size()), s_maxRows);
    char line[256]{};

    for (int i = 0; i < rows; ++i) {
        const DamageEntry& entry = s_entries[static_cast<size_t>(i)];
        const std::string damage = FormatDamage(entry.damage);
        std::snprintf(
            line,
            sizeof(line),
            "\r\n%d. %s %s (%.1f%%)",
            i + 1,
            entry.name.c_str(),
            damage.c_str(),
            entry.percent);
        text += line;
    }

    return text;
}
} // namespace

void DamageMeter::Configure(bool enabled, int maxRows, int offsetX, int offsetY)
{
    s_enabled = enabled;
    s_maxRows = ClampRows(maxRows);
    s_offsetX = offsetX;
    s_offsetY = offsetY;

    if (!s_enabled) {
        ClearOverlay();
    }
}

bool DamageMeter::HandlePacket(const void* dataPtr, unsigned long sizeValue)
{
    return false;
}

void DamageMeter::OnFieldInit()
{
    return;
}

void DamageMeter::OnFieldDispose()
{
    return;
}

void DamageMeter::UpdateOverlay()
{
    return;
}

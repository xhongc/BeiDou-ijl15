#include "stdafx.h"
#include "DamageMeter.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
constexpr size_t kMaxPacketCopySize = 16 * 1024;

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
    if (s_toolTipCreated) {
        s_ClearToolTip(reinterpret_cast<int>(&s_toolTip), nullptr);
    }
    s_overlayVisible = false;
}

static bool EnsureToolTipCreated()
{
    if (s_toolTipCreated) {
        return true;
    }

    __try {
        memset(s_toolTip, 0, sizeof(s_toolTip));
        s_CreateToolTip(reinterpret_cast<int>(&s_toolTip), nullptr);
        s_toolTipCreated = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_toolTipCreated = false;
        return false;
    }
}

static bool TryCopyPacketData(const void* dataPtr, size_t size, unsigned char* outBuffer)
{
    __try {
        memcpy(outBuffer, dataPtr, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool TrySetOverlayText(int x, int y, const char* text)
{
    __try {
        s_SetToolTipString(reinterpret_cast<int>(&s_toolTip), nullptr, x, y, text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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

    DebugLog("DamageMeter::Configure enabled=%d rows=%d offset=(%d,%d)", enabled ? 1 : 0, s_maxRows, s_offsetX, s_offsetY);
}

bool DamageMeter::HandlePacket(const void* dataPtr, unsigned long sizeValue)
{
    if (!s_enabled || dataPtr == nullptr || sizeValue < 6 || sizeValue > kMaxPacketCopySize) {
        return false;
    }

    std::vector<unsigned char> buffer(static_cast<size_t>(sizeValue));
    if (!TryCopyPacketData(dataPtr, static_cast<size_t>(sizeValue), buffer.data())) {
        return false;
    }

    const auto* data = buffer.data();
    const size_t size = buffer.size();
    size_t cursor = 4;
    unsigned short opcode = 0;
    if (!ReadU16(data, size, cursor, opcode) || opcode != kOpcodeDamageMeterSync) {
        return false;
    }

    DebugLog("DamageMeter::HandlePacket opcode=0x%04X size=%lu", opcode, sizeValue);

    if (size < 13) {
        DebugLog("DamageMeter::HandlePacket too short for payload");
        return true;
    }

    unsigned int sessionId = 0;
    unsigned char mode = 0;
    unsigned char reason = 0;
    unsigned char entryCount = 0;
    if (!ReadU32(data, size, cursor, sessionId) ||
        !ReadU8(data, size, cursor, mode) ||
        !ReadU8(data, size, cursor, reason) ||
        !ReadU8(data, size, cursor, entryCount)) {
        DebugLog("DamageMeter::HandlePacket header parse failed");
        return true;
    }

    DebugLog("DamageMeter::HandlePacket session=%u mode=%u reason=%u entryCount=%u", sessionId, mode, reason, entryCount);

    if (mode == kModeHidden) {
        ResetState();
        DebugLog("DamageMeter::HandlePacket reset hidden");
        return true;
    }

    std::vector<DamageEntry> parsedEntries;
    parsedEntries.reserve(entryCount);
    for (unsigned char i = 0; i < entryCount; ++i) {
        DamageEntry entry{};
        unsigned int damageLow = 0;
        unsigned int damageHigh = 0;
        if (!ReadU32(data, size, cursor, entry.characterId) ||
            !ReadMapleString(data, size, cursor, entry.name) ||
            !ReadU32(data, size, cursor, damageLow) ||
            !ReadU32(data, size, cursor, damageHigh)) {
            DebugLog("DamageMeter::HandlePacket entry parse failed index=%u", static_cast<unsigned int>(i));
            return true;
        }
        entry.damage = static_cast<unsigned long long>(damageLow) |
            (static_cast<unsigned long long>(damageHigh) << 32);
        parsedEntries.push_back(entry);
    }

    ApplySnapshot(sessionId, mode, parsedEntries);

    if (reason == 1 && s_entries.empty()) {
        ClearOverlay();
    }

    DebugLog("DamageMeter::HandlePacket applied entries=%u", static_cast<unsigned int>(s_entries.size()));

    return true;
}

void DamageMeter::OnFieldInit()
{
    ResetState();
    DebugLog("DamageMeter::OnFieldInit");
}

void DamageMeter::OnFieldDispose()
{
    ResetState();
    DebugLog("DamageMeter::OnFieldDispose");
}

void DamageMeter::UpdateOverlay()
{
    if (!s_enabled) {
        return;
    }

    if (s_mode != kModeParty || s_entries.empty()) {
        if (s_overlayVisible) {
            ClearOverlay();
        }
        return;
    }

    const DWORD now = GetTickCount();
    if (s_lastSyncTick == 0 || now - s_lastSyncTick > kSyncStaleMs) {
        ClearOverlay();
        return;
    }

    if (!EnsureToolTipCreated()) {
        return;
    }

    const std::string overlayText = BuildOverlayText();
    const int x = ClampX(16 + s_offsetX);
    const int y = ClampY(Client::m_nGameHeight - 180 + s_offsetY);

    if (TrySetOverlayText(x, y, overlayText.c_str())) {
        s_overlayVisible = true;
    } else {
        s_overlayVisible = false;
    }
}

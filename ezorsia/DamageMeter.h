#pragma once

class DamageMeter
{
public:
    static void Configure(bool enabled, int maxRows, int offsetX, int offsetY);
    static void HandlePacket(const void* data, unsigned long size);
    static void OnFieldInit();
    static void OnFieldDispose();
    static void UpdateOverlay();
};


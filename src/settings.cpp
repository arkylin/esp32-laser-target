#include "settings.h"
#include "config.h"

#include <Preferences.h>

static Preferences s_prefs;
static bool        s_ready = false;

void settingsBegin()
{
    // 命名空间 "laser"；false = 可读可写
    s_ready = s_prefs.begin("laser", false);
    if (s_ready) {
        LOGLN("[settings] NVS ready (settings survive reset / power off)");
    } else {
        LOGLN("[settings] NVS unavailable: run with defaults, changes won't be saved");
    }
}

uint8_t settingsGetU8(const char* key, uint8_t def)
{
    return s_ready ? s_prefs.getUChar(key, def) : def;
}

uint16_t settingsGetU16(const char* key, uint16_t def)
{
    return s_ready ? s_prefs.getUShort(key, def) : def;
}

void settingsPutU8(const char* key, uint8_t v)
{
    if (s_ready) {
        s_prefs.putUChar(key, v);
    }
}

void settingsPutU16(const char* key, uint16_t v)
{
    if (s_ready) {
        s_prefs.putUShort(key, v);
    }
}

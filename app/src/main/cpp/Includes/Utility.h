#ifndef ZYGISK_IMGUI_MOD_MENU_MAIN_UTILITY_H
#define ZYGISK_IMGUI_MOD_MENU_MAIN_UTILITY_H

#include <cstdint>
#include <cstring>
#include <chrono>
#include "imgui.h"
#define INTERNAL __attribute__((visibility("hidden")))

struct UE4Info {
    uintptr_t base = 0;
    char      apkPath[512] = {};
    void*     handle = nullptr;
};

struct UEVersionInfo {
    bool     ok = false;
    bool     exact = false;
    int32_t  major = -1;
    int32_t  minor = -1;
    int32_t  patch = -1;
    uint32_t changelist = 0;
    char     branch[128] = {};
    char     method[32] = {};
    char     detail[192] = {};
};

UE4Info FindAndOpenLibUE4();
UEVersionInfo FindUEVersion(const UE4Info& info);
void* FindSymbolDynamic(uintptr_t base, const char* name);
void* FindSymbolDisk(const char* path, uintptr_t base, const char* name);


INTERNAL static void* findSymbol(const UE4Info& info, char* name)
{
    void* sym = FindSymbolDynamic(info.base, name);
    if (!sym) sym = FindSymbolDisk(info.apkPath, info.base, name);
    memset(name, 0, strlen(name));
    return sym;
}

#define UE4SYM(info, var, symbol)                                       \
    do {                                                                 \
        var = reinterpret_cast<decltype(var)>(findSymbol(info, symbol));     \
    } while (0)


namespace mem {
    uintptr_t FindTargetByAdrpAdd(uintptr_t search_start, size_t search_size = 0x100);
    uintptr_t FindInsideOrFollowBranches(uintptr_t search_start, size_t search_size = 0x100);
}


#endif //ZYGISK_IMGUI_MOD_MENU_MAIN_UTILITY_H

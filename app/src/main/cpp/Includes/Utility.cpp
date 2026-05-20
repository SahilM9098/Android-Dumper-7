#include "Utility.h"
#include "SafeMemory.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <cstdio>
#include <link.h>
#include <dlfcn.h>
#include <elf.h>
#include <jni.h>
#include <string>
#include <GLES3/gl3.h>
// ─────────────────────────────────────────────────────────────────
// Memory Utilities for AArch64 (Internal to .cpp)
// ─────────────────────────────────────────────────────────────────


namespace {
    using JNI_GetCreatedJavaVMs_t = jint (*)(JavaVM**, jsize, jsize*);
    using AndroidRuntimeGetJavaVM_t = JavaVM* (*)();

    const char* PathBaseName(const char* path) noexcept {
        if (path == nullptr) return "";
        const char* slash = strrchr(path, '/');
        return slash ? slash + 1 : path;
    }

    int ue_lib_rank(const char* path) noexcept {
        const char* name = PathBaseName(path);
        if (strcmp(name, "libUE4.so") == 0) return 0;
        if (strcmp(name, "libUnreal.so") == 0) return 1;
        return -1;
    }

    void copy_path(char* dst, size_t dstSize, const char* src) noexcept {
        if (dst == nullptr || dstSize == 0) return;
        dst[0] = '\0';
        if (src == nullptr) return;
        strncpy(dst, src, dstSize - 1);
        dst[dstSize - 1] = '\0';
    }

    void* dlopen_no_load(const char* path) noexcept {
        if (path == nullptr || *path == '\0') return nullptr;

        void* handle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
        if (handle != nullptr) return handle;

        const char* name = PathBaseName(path);
        if (name != path && *name != '\0') {
            handle = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
        }
        return handle;
    }

    JavaVM* try_resolve_vm_from_handle(void* handle) noexcept {
        if (!handle) return nullptr;

        auto getVms = reinterpret_cast<JNI_GetCreatedJavaVMs_t>(
                dlsym(handle, "JNI_GetCreatedJavaVMs"));
        if (getVms) {
            JavaVM* vm = nullptr;
            jsize count = 0;
            if (getVms(&vm, 1, &count) == JNI_OK && count > 0 && vm) {
                return vm;
            }
        }

        auto getVm = reinterpret_cast<AndroidRuntimeGetJavaVM_t>(
                dlsym(handle, "AndroidRuntimeGetJavaVM"));
        return getVm ? getVm() : nullptr;
    }

    JavaVM* resolve_java_vm() noexcept {
        if (JavaVM* vm = try_resolve_vm_from_handle(RTLD_DEFAULT)) {
            return vm;
        }

        constexpr const char* kLibs[] = {
                "libandroid_runtime.so",
                "/system/lib64/libandroid_runtime.so",
                "libnativehelper.so",
                "/apex/com.android.art/lib64/libnativehelper.so",
                "libart.so",
                "/apex/com.android.art/lib64/libart.so",
        };
        for (const char* lib : kLibs) {
            void* handle = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
            if (JavaVM* vm = try_resolve_vm_from_handle(handle)) {
                return vm;
            }
        }
        return nullptr;
    }

    void copy_text(char* dst, size_t dstSize, const char* src) noexcept {
        if (!dst || dstSize == 0) return;
        dst[0] = '\0';
        if (!src) return;
        std::snprintf(dst, dstSize, "%s", src);
    }

    bool valid_engine_version(int major, int minor, int patch) noexcept {
        if (major != 4 && major != 5) return false;
        if (minor < 0 || minor > 99) return false;
        if (patch < -1 || patch > 99) return false;
        return true;
    }

    bool parse_int_after_key(const std::string& text,
                             const char* key,
                             int& out) noexcept {
        size_t pos = text.find(key);
        if (pos == std::string::npos) return false;
        pos = text.find(':', pos);
        if (pos == std::string::npos) return false;
        ++pos;
        while (pos < text.size() &&
               !std::isdigit(static_cast<unsigned char>(text[pos])) &&
               text[pos] != '-') {
            ++pos;
        }
        if (pos >= text.size()) return false;
        char* end = nullptr;
        long v = std::strtol(text.c_str() + pos, &end, 10);
        if (end == text.c_str() + pos) return false;
        out = static_cast<int>(v);
        return true;
    }

    bool parse_uint_after_key(const std::string& text,
                              const char* key,
                              uint32_t& out) noexcept {
        int signedValue = 0;
        if (!parse_int_after_key(text, key, signedValue) || signedValue < 0) {
            return false;
        }
        out = static_cast<uint32_t>(signedValue);
        return true;
    }

    bool parse_string_after_key(const std::string& text,
                                const char* key,
                                std::string& out) noexcept {
        size_t pos = text.find(key);
        if (pos == std::string::npos) return false;
        pos = text.find(':', pos);
        if (pos == std::string::npos) return false;
        pos = text.find('"', pos);
        if (pos == std::string::npos) return false;
        size_t end = text.find('"', pos + 1);
        if (end == std::string::npos || end <= pos + 1) return false;
        out.assign(text.data() + pos + 1, end - pos - 1);
        return true;
    }

    bool parse_version_at(const char* s,
                          size_t maxLen,
                          int& major,
                          int& minor,
                          int& patch) noexcept {
        size_t pos = 0;
        while (pos < maxLen && !std::isdigit(static_cast<unsigned char>(s[pos]))) {
            ++pos;
        }
        if (pos >= maxLen) return false;

        char* end = nullptr;
        long a = std::strtol(s + pos, &end, 10);
        if (end == s + pos || end >= s + maxLen || *end != '.') return false;
        char* second = end + 1;
        long b = std::strtol(second, &end, 10);
        if (end == second) return false;

        long c = -1;
        if (end < s + maxLen && *end == '.') {
            char* third = end + 1;
            c = std::strtol(third, &end, 10);
            if (end == third) c = -1;
        }

        if (!valid_engine_version(static_cast<int>(a),
                                  static_cast<int>(b),
                                  static_cast<int>(c))) {
            return false;
        }
        major = static_cast<int>(a);
        minor = static_cast<int>(b);
        patch = static_cast<int>(c);
        return true;
    }

    std::string lower_ascii(std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    bool key_looks_like_engine_version(const std::string& key) {
        std::string k = lower_ascii(key);
        if (k.find("gameactivity.engineversion") != std::string::npos) {
            return true;
        }

        const bool engineish =
                k.find("engine") != std::string::npos ||
                k.find("unreal") != std::string::npos ||
                k.find("ue4") != std::string::npos ||
                k.find("ue5") != std::string::npos ||
                k.find(".ue.") != std::string::npos;
        const bool versionish =
                k.find("version") != std::string::npos ||
                k.find("build") != std::string::npos;
        return engineish && versionish;
    }

    bool parse_version_from_text(const std::string& text,
                                 int& major,
                                 int& minor,
                                 int& patch) noexcept {
        for (size_t i = 0; i < text.size(); ++i) {
            int a = -1;
            int b = -1;
            int c = -1;
            if (parse_version_at(text.c_str() + i, text.size() - i, a, b, c)) {
                major = a;
                minor = b;
                patch = c;
                return true;
            }
        }
        return false;
    }

    struct ReadableSegment {
        const char* start = nullptr;
        size_t size = 0;
        uintptr_t displayBase = 0;
    };

    std::vector<ReadableSegment> collect_readable_segments(uintptr_t base) {
        std::vector<ReadableSegment> out;
        struct Ctx {
            uintptr_t base;
            std::vector<ReadableSegment>* out;
        };
        Ctx ctx{base, &out};
        dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
            auto* c = static_cast<Ctx*>(data);
            if (!info || info->dlpi_addr != c->base) return 0;
            for (uint16_t i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr)& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_LOAD || (ph.p_flags & PF_R) == 0) continue;
                uintptr_t start = info->dlpi_addr + ph.p_vaddr;
                if (ph.p_memsz == 0) continue;
                c->out->push_back({
                        reinterpret_cast<const char*>(start),
                        static_cast<size_t>(ph.p_memsz),
                        start
                });
            }
            return 1;
        }, &ctx);
        return out;
    }

    const char* find_bytes(const char* start,
                           const char* end,
                           const char* needle) noexcept {
        const size_t needleLen = std::strlen(needle);
        if (!start || !end || start >= end || needleLen == 0) return nullptr;
        const char* it = std::search(start, end, needle, needle + needleLen);
        return it == end ? nullptr : it;
    }

    const char* text_width_name(int width) noexcept {
        switch (width) {
            case 2: return "utf16";
            case 4: return "utf32";
            default: return "ascii";
        }
    }

    std::string encode_text(const char* text, int width) {
        std::string out;
        if (!text || width <= 1) return text ? std::string(text) : std::string{};
        const size_t len = std::strlen(text);
        out.reserve(len * static_cast<size_t>(width));
        for (size_t i = 0; i < len; ++i) {
            out.push_back(text[i]);
            for (int z = 1; z < width; ++z) out.push_back('\0');
        }
        return out;
    }

    const char* find_text(const char* start,
                          const char* end,
                          const char* needle,
                          int width) {
        if (width <= 1) return find_bytes(start, end, needle);
        std::string encoded = encode_text(needle, width);
        if (encoded.empty() || start >= end) return nullptr;
        const char* it = std::search(start, end, encoded.begin(), encoded.end());
        return it == end ? nullptr : it;
    }

    bool read_code_unit(const char* p, int width, uint32_t& out) noexcept {
        out = 0;
        if (width <= 1) {
            out = static_cast<unsigned char>(*p);
            return true;
        }
        if (width == 2) {
            uint16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            out = v;
            return true;
        }
        if (width == 4) {
            uint32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            out = v;
            return true;
        }
        return false;
    }

    bool is_text_unit(uint32_t ch) noexcept {
        return ch == '\n' || ch == '\r' || ch == '\t' ||
               (ch >= 0x20 && ch <= 0x7e);
    }

    bool is_inline_printable(uint32_t ch) noexcept {
        return ch >= 0x20 && ch <= 0x7e;
    }

    std::string decode_text_window(const ReadableSegment& seg,
                                   const char* match,
                                   size_t leftChars,
                                   size_t rightChars,
                                   int width) {
        const char* segStart = seg.start;
        const char* segEnd = seg.start + seg.size;
        const size_t stride = static_cast<size_t>(std::max(width, 1));

        size_t before = static_cast<size_t>(match - segStart);
        before = std::min(before, leftChars * stride);
        before -= before % stride;

        size_t after = static_cast<size_t>(segEnd - match);
        after = std::min(after, rightChars * stride);
        after -= after % stride;

        const char* p = match - before;
        const char* end = match + after;

        std::string out;
        out.reserve((before + after) / stride);
        for (; p + stride <= end; p += stride) {
            uint32_t ch = 0;
            if (!read_code_unit(p, width, ch)) break;
            out.push_back(is_text_unit(ch) ? static_cast<char>(ch) : ' ');
        }
        return out;
    }

    std::string decode_printable_around(const ReadableSegment& seg,
                                        const char* match,
                                        int width) {
        const char* segStart = seg.start;
        const char* segEnd = seg.start + seg.size;
        const size_t stride = static_cast<size_t>(std::max(width, 1));

        const char* start = match;
        while (start >= segStart + stride) {
            uint32_t ch = 0;
            if (!read_code_unit(start - stride, width, ch) ||
                !is_inline_printable(ch)) {
                break;
            }
            start -= stride;
        }

        const char* end = match;
        while (end + stride <= segEnd) {
            uint32_t ch = 0;
            if (!read_code_unit(end, width, ch) ||
                !is_inline_printable(ch)) {
                break;
            }
            end += stride;
        }

        std::string out;
        out.reserve((end - start) / stride);
        for (const char* p = start; p + stride <= end; p += stride) {
            uint32_t ch = 0;
            if (!read_code_unit(p, width, ch) || !is_inline_printable(ch)) break;
            out.push_back(static_cast<char>(ch));
        }
        return out;
    }

    bool make_result(UEVersionInfo& out,
                     int major,
                     int minor,
                     int patch,
                     uint32_t changelist,
                     const std::string& branch,
                     const char* method,
                     const char* detail,
                     bool exact) noexcept {
        if (!valid_engine_version(major, minor, patch)) return false;
        out.ok = true;
        out.exact = exact && patch >= 0;
        out.major = major;
        out.minor = minor;
        out.patch = patch;
        out.changelist = changelist;
        copy_text(out.branch, sizeof(out.branch), branch.c_str());
        copy_text(out.method, sizeof(out.method), method);
        copy_text(out.detail, sizeof(out.detail), detail);
        return true;
    }

    bool try_engine_version_symbol(const UE4Info& info, UEVersionInfo& out) noexcept {
        constexpr const char* kSymbols[] = {
                "_ZN14FEngineVersion7CurrentEv",
                "FEngineVersion::Current",
        };

        for (const char* name : kSymbols) {
            void* sym = FindSymbolDynamic(info.base, name);
            if (!sym && info.apkPath[0] != '\0') {
                sym = FindSymbolDisk(info.apkPath, info.base, name);
            }
            if (!sym) continue;

            const void* versionPtr = nullptr;
            SafeMemory::ScopedSigSegvGuard guard;
            if (!guard.Try([&] {
                using Fn = const void* (*)();
                versionPtr = reinterpret_cast<Fn>(sym)();
            })) {
                continue;
            }
            if (!versionPtr) continue;

            int major = -1;
            int minor = -1;
            int patch = -1;
            uint32_t changelist = 0;
            bool readOk = guard.Try([&] {
                const auto* p = static_cast<const uint8_t*>(versionPtr);
                uint16_t a = 0;
                uint16_t b = 0;
                uint16_t c = 0;
                std::memcpy(&a, p + 0, sizeof(a));
                std::memcpy(&b, p + 2, sizeof(b));
                std::memcpy(&c, p + 4, sizeof(c));
                std::memcpy(&changelist, p + 8, sizeof(changelist));
                major = a;
                minor = b;
                patch = c;
            });
            if (!readOk) continue;
            if (make_result(out, major, minor, patch, changelist, {},
                            "FEngineVersion::Current", name, true)) {
                return true;
            }
        }
        return false;
    }

    std::string jstring_to_string(JNIEnv* env, jstring s) {
        if (!env || !s) return {};
        const char* chars = env->GetStringUTFChars(s, nullptr);
        if (!chars) return {};
        std::string out(chars);
        env->ReleaseStringUTFChars(s, chars);
        return out;
    }

    std::string jobject_to_string(JNIEnv* env, jobject obj) {
        if (!env || !obj) return {};
        jclass objectClass = env->FindClass("java/lang/Object");
        if (!objectClass) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return {};
        }
        jmethodID toString = env->GetMethodID(
                objectClass, "toString", "()Ljava/lang/String;");
        env->DeleteLocalRef(objectClass);
        if (!toString) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return {};
        }
        auto text = static_cast<jstring>(env->CallObjectMethod(obj, toString));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return {};
        }
        std::string out = jstring_to_string(env, text);
        env->DeleteLocalRef(text);
        return out;
    }

    struct PackageContext {
        jobject packageManager = nullptr;
        jstring packageName = nullptr;
    };

    void release_package_context(JNIEnv* env, PackageContext& ctx) noexcept {
        if (!env) return;
        if (ctx.packageManager) {
            env->DeleteLocalRef(ctx.packageManager);
            ctx.packageManager = nullptr;
        }
        if (ctx.packageName) {
            env->DeleteLocalRef(ctx.packageName);
            ctx.packageName = nullptr;
        }
    }

    bool get_package_context(JNIEnv* env, PackageContext& ctx) noexcept {
        jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
        if (!activityThreadClass) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }

        jmethodID currentApplication = env->GetStaticMethodID(
                activityThreadClass, "currentApplication", "()Landroid/app/Application;");
        if (!currentApplication) {
            env->DeleteLocalRef(activityThreadClass);
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }

        jobject app = env->CallStaticObjectMethod(activityThreadClass, currentApplication);
        env->DeleteLocalRef(activityThreadClass);
        if (env->ExceptionCheck() || !app) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }

        jclass contextClass = env->FindClass("android/content/Context");
        if (!contextClass) {
            env->DeleteLocalRef(app);
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }

        jmethodID getPackageManager = env->GetMethodID(
                contextClass, "getPackageManager", "()Landroid/content/pm/PackageManager;");
        jmethodID getPackageName = env->GetMethodID(
                contextClass, "getPackageName", "()Ljava/lang/String;");
        env->DeleteLocalRef(contextClass);
        if (!getPackageManager || !getPackageName) {
            env->DeleteLocalRef(app);
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }

        ctx.packageManager = env->CallObjectMethod(app, getPackageManager);
        ctx.packageName = static_cast<jstring>(env->CallObjectMethod(app, getPackageName));
        env->DeleteLocalRef(app);
        if (env->ExceptionCheck() || !ctx.packageManager || !ctx.packageName) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            release_package_context(env, ctx);
            return false;
        }
        return true;
    }

    jobject get_application_info_meta_data_bundle(JNIEnv* env) {
        PackageContext ctx;
        if (!get_package_context(env, ctx)) return nullptr;

        jclass pmClass = env->FindClass("android/content/pm/PackageManager");
        jmethodID getApplicationInfo = pmClass
                ? env->GetMethodID(pmClass, "getApplicationInfo",
                                   "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;")
                : nullptr;
        if (pmClass) env->DeleteLocalRef(pmClass);
        if (!getApplicationInfo) {
            release_package_context(env, ctx);
            if (env->ExceptionCheck()) env->ExceptionClear();
            return nullptr;
        }

        constexpr jint kGetMetaData = 0x00000080;
        jobject appInfo = env->CallObjectMethod(ctx.packageManager, getApplicationInfo,
                                                ctx.packageName, kGetMetaData);
        release_package_context(env, ctx);
        if (env->ExceptionCheck() || !appInfo) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return nullptr;
        }

        jclass appInfoClass = env->FindClass("android/content/pm/ApplicationInfo");
        jfieldID metaDataField = appInfoClass
                ? env->GetFieldID(appInfoClass, "metaData", "Landroid/os/Bundle;")
                : nullptr;
        if (appInfoClass) env->DeleteLocalRef(appInfoClass);
        if (!metaDataField) {
            env->DeleteLocalRef(appInfo);
            if (env->ExceptionCheck()) env->ExceptionClear();
            return nullptr;
        }

        jobject metaData = env->GetObjectField(appInfo, metaDataField);
        env->DeleteLocalRef(appInfo);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return nullptr;
        }
        return metaData;
    }

    bool try_manifest_bundle(JNIEnv* env,
                             jobject metaData,
                             const char* scope,
                             UEVersionInfo& out) noexcept {
        if (!env || !metaData) return false;
        jclass bundleClass = env->FindClass("android/os/Bundle");
        jmethodID keySetMethod = bundleClass
                ? env->GetMethodID(bundleClass, "keySet", "()Ljava/util/Set;")
                : nullptr;
        jmethodID getMethod = bundleClass
                ? env->GetMethodID(bundleClass, "get", "(Ljava/lang/String;)Ljava/lang/Object;")
                : nullptr;
        if (bundleClass) env->DeleteLocalRef(bundleClass);

        if (!keySetMethod || !getMethod) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }

        jobject keySet = env->CallObjectMethod(metaData, keySetMethod);
        if (env->ExceptionCheck() || !keySet) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }

        jclass setClass = env->FindClass("java/util/Set");
        jmethodID toArrayMethod = setClass
                ? env->GetMethodID(setClass, "toArray", "()[Ljava/lang/Object;")
                : nullptr;
        if (setClass) env->DeleteLocalRef(setClass);
        jobjectArray keys = toArrayMethod
                ? static_cast<jobjectArray>(env->CallObjectMethod(keySet, toArrayMethod))
                : nullptr;
        env->DeleteLocalRef(keySet);
        if (env->ExceptionCheck() || !keys) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }

        const jsize count = env->GetArrayLength(keys);
        bool found = false;
        for (jsize i = 0; i < count && !found; ++i) {
            auto keyObj = static_cast<jstring>(env->GetObjectArrayElement(keys, i));
            std::string key = jstring_to_string(env, keyObj);
            if (!key_looks_like_engine_version(key)) {
                env->DeleteLocalRef(keyObj);
                continue;
            }

            jobject valueObj = env->CallObjectMethod(metaData, getMethod, keyObj);
            std::string value = jobject_to_string(env, valueObj);
            if (valueObj) env->DeleteLocalRef(valueObj);
            env->DeleteLocalRef(keyObj);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                continue;
            }

            int major = -1;
            int minor = -1;
            int patch = -1;
            if (!parse_version_from_text(value, major, minor, patch)) {
                continue;
            }

            char detail[192];
            if (scope && *scope) {
                std::snprintf(detail, sizeof(detail), "%s:%s=%s",
                              scope, key.c_str(), value.c_str());
            } else {
                std::snprintf(detail, sizeof(detail), "%s=%s",
                              key.c_str(), value.c_str());
            }
            found = make_result(out, major, minor, patch, 0, {},
                                "AndroidManifest", detail, patch >= 0);
        }

        env->DeleteLocalRef(keys);
        return found;
    }

    bool try_activity_info_meta_data(JNIEnv* env, UEVersionInfo& out) noexcept {
        PackageContext ctx;
        if (!get_package_context(env, ctx)) return false;

        jclass pmClass = env->FindClass("android/content/pm/PackageManager");
        jmethodID getPackageInfo = pmClass
                ? env->GetMethodID(pmClass, "getPackageInfo",
                                   "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;")
                : nullptr;
        if (pmClass) env->DeleteLocalRef(pmClass);
        if (!getPackageInfo) {
            release_package_context(env, ctx);
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }

        constexpr jint kGetActivities = 0x00000001;
        constexpr jint kGetMetaData = 0x00000080;
        jobject packageInfo = env->CallObjectMethod(ctx.packageManager,
                                                    getPackageInfo,
                                                    ctx.packageName,
                                                    kGetActivities | kGetMetaData);
        release_package_context(env, ctx);
        if (env->ExceptionCheck() || !packageInfo) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }

        jclass packageInfoClass = env->FindClass("android/content/pm/PackageInfo");
        jfieldID activitiesField = packageInfoClass
                ? env->GetFieldID(packageInfoClass, "activities",
                                  "[Landroid/content/pm/ActivityInfo;")
                : nullptr;
        if (packageInfoClass) env->DeleteLocalRef(packageInfoClass);
        if (!activitiesField) {
            env->DeleteLocalRef(packageInfo);
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }

        auto activities = static_cast<jobjectArray>(
                env->GetObjectField(packageInfo, activitiesField));
        env->DeleteLocalRef(packageInfo);
        if (env->ExceptionCheck() || !activities) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }

        jclass activityInfoClass = env->FindClass("android/content/pm/ActivityInfo");
        jfieldID metaDataField = activityInfoClass
                ? env->GetFieldID(activityInfoClass, "metaData", "Landroid/os/Bundle;")
                : nullptr;
        jfieldID nameField = activityInfoClass
                ? env->GetFieldID(activityInfoClass, "name", "Ljava/lang/String;")
                : nullptr;
        if (activityInfoClass) env->DeleteLocalRef(activityInfoClass);
        if (!metaDataField) {
            env->DeleteLocalRef(activities);
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }

        const jsize count = env->GetArrayLength(activities);
        bool found = false;
        for (jsize i = 0; i < count && !found; ++i) {
            jobject activityInfo = env->GetObjectArrayElement(activities, i);
            if (!activityInfo) continue;

            std::string scope = "activity";
            if (nameField) {
                auto activityName = static_cast<jstring>(
                        env->GetObjectField(activityInfo, nameField));
                if (!env->ExceptionCheck() && activityName) {
                    scope = jstring_to_string(env, activityName);
                    env->DeleteLocalRef(activityName);
                } else if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                }
            }

            jobject metaData = env->GetObjectField(activityInfo, metaDataField);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                env->DeleteLocalRef(activityInfo);
                continue;
            }

            if (metaData) {
                found = try_manifest_bundle(env, metaData, scope.c_str(), out);
                env->DeleteLocalRef(metaData);
            }
            env->DeleteLocalRef(activityInfo);
        }

        env->DeleteLocalRef(activities);
        return found;
    }

    bool try_manifest_meta_data(UEVersionInfo& out) noexcept {
        JavaVM* vm = resolve_java_vm();
        if (!vm) return false;

        JNIEnv* env = nullptr;
        bool attached = false;
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
                return false;
            }
            attached = true;
        }

        jobject metaData = get_application_info_meta_data_bundle(env);
        if (metaData) {
            if (try_manifest_bundle(env, metaData, "application", out)) {
                env->DeleteLocalRef(metaData);
                if (attached) vm->DetachCurrentThread();
                return true;
            }
            env->DeleteLocalRef(metaData);
        }

        const bool found = try_activity_info_meta_data(env, out);
        if (attached) vm->DetachCurrentThread();
        return found;
    }

    bool try_build_version_json(const std::vector<ReadableSegment>& segments,
                                UEVersionInfo& out,
                                const char* sourceLabel) noexcept {
        constexpr const char* kNeedle = "\"MajorVersion\"";
        for (const auto& seg : segments) {
            for (int width : {1, 2, 4}) {
                const char* cur = seg.start;
                const char* end = seg.start + seg.size;
                while ((cur = find_text(cur, end, kNeedle, width)) != nullptr) {
                    std::string text = decode_text_window(seg, cur, 512, 2048, width);

                    int major = -1;
                    int minor = -1;
                    int patch = -1;
                    uint32_t changelist = 0;
                    std::string branch;
                    if (parse_int_after_key(text, "\"MajorVersion\"", major) &&
                        parse_int_after_key(text, "\"MinorVersion\"", minor) &&
                        parse_int_after_key(text, "\"PatchVersion\"", patch) &&
                        valid_engine_version(major, minor, patch)) {
                        parse_uint_after_key(text, "\"Changelist\"", changelist);
                        parse_string_after_key(text, "\"BranchName\"", branch);
                        char detail[160];
                        std::snprintf(detail, sizeof(detail),
                                      "%s Build.version JSON @ 0x%lx (%s)",
                                      sourceLabel,
                                      static_cast<unsigned long>(
                                              seg.displayBase +
                                              static_cast<uintptr_t>(cur - seg.start)),
                                      text_width_name(width));
                        return make_result(out, major, minor, patch, changelist,
                                           branch, "Build.version", detail, true);
                    }
                    cur += std::max(width, 1) * static_cast<int>(std::strlen(kNeedle));
                }
            }
        }
        return false;
    }

    bool parse_release_text(const std::string& text,
                            int& major,
                            int& minor,
                            int& patch) noexcept {
        size_t pos = text.find("Release-");
        if (pos == std::string::npos) return false;
        const char* versionStart = text.c_str() + pos + std::strlen("Release-");
        if (!parse_version_at(versionStart,
                              text.size() - pos - std::strlen("Release-"),
                              major, minor, patch)) {
            return false;
        }

        int prefixMajor = -1;
        int prefixMinor = -1;
        int prefixPatch = -1;
        for (size_t i = 0; i < pos; ++i) {
            int a = -1;
            int b = -1;
            int c = -1;
            if (parse_version_at(text.c_str() + i, pos - i, a, b, c) && c >= 0) {
                prefixMajor = a;
                prefixMinor = b;
                prefixPatch = c;
            }
        }
        if (prefixPatch >= 0) {
            major = prefixMajor;
            minor = prefixMinor;
            patch = prefixPatch;
        }
        return true;
    }

    bool try_release_branch_string(const std::vector<ReadableSegment>& segments,
                                   UEVersionInfo& out,
                                   const char* sourceLabel) noexcept {
        constexpr const char* kNeedles[] = {
                "++UE4+Release-", "++UE5+Release-",
                "UE4+Release-",   "UE5+Release-",
                "Release-4.",     "Release-5.",
        };
        for (const auto& seg : segments) {
            for (const char* needle : kNeedles) {
                for (int width : {1, 2, 4}) {
                    const char* cur = seg.start;
                    const char* end = seg.start + seg.size;
                    while ((cur = find_text(cur, end, needle, width)) != nullptr) {
                        int major = -1;
                        int minor = -1;
                        int patch = -1;
                        std::string branch = decode_printable_around(seg, cur, width);
                        if (parse_release_text(branch, major, minor, patch)) {
                            char detail[160];
                            std::snprintf(detail, sizeof(detail),
                                          "%s release branch @ 0x%lx (%s)",
                                          sourceLabel,
                                          static_cast<unsigned long>(
                                                  seg.displayBase +
                                                  static_cast<uintptr_t>(cur - seg.start)),
                                          text_width_name(width));
                            return make_result(out, major, minor, patch, 0,
                                               branch, "release-string", detail,
                                               patch >= 0);
                        }
                        cur += std::max(width, 1) * static_cast<int>(std::strlen(needle));
                    }
                }
            }
        }
        return false;
    }

    bool try_version_file_scan(const char* path, UEVersionInfo& out) noexcept {
        if (!path || !*path) return false;
        FILE* f = std::fopen(path, "rb");
        if (!f) return false;

        constexpr size_t kChunkSize = 2 * 1024 * 1024;
        constexpr size_t kOverlap = 16 * 1024;
        std::vector<char> buffer(kChunkSize + kOverlap);
        size_t carry = 0;
        uintptr_t fileOffset = 0;

        while (true) {
            size_t read = std::fread(buffer.data() + carry, 1, kChunkSize, f);
            if (read == 0 && carry == 0) break;

            ReadableSegment seg{
                    buffer.data(),
                    carry + read,
                    fileOffset >= carry ? fileOffset - carry : 0
            };
            std::vector<ReadableSegment> one{seg};
            if (try_build_version_json(one, out, "file") ||
                try_release_branch_string(one, out, "file")) {
                std::fclose(f);
                return true;
            }

            if (read < kChunkSize) break;
            carry = std::min(kOverlap, carry + read);
            std::memmove(buffer.data(),
                         buffer.data() + (seg.size - carry),
                         carry);
            fileOffset += read;
        }

        std::fclose(f);
        return false;
    }

    bool is_adrp(uint32_t ins) {
        return (ins & 0x9F000000) == 0x90000000;
    }

    bool is_add(uint32_t ins) {
        return (ins & 0xFF000000) == 0x91000000;
    }

    uintptr_t get_page(uintptr_t addr) {
        return addr & ~0xFFFULL;
    }

    uintptr_t decode_adrp(uintptr_t pc, uint32_t ins) {
        uint32_t immlo = (ins >> 29) & 3;
        uint32_t immhi = (ins >> 5) & 0x7FFFF;
        int32_t imm = (immhi << 2) | immlo;
        if (imm & 0x100000) imm |= 0xFFE00000; // Sign extend
        return get_page(pc) + ((int64_t)imm << 12);
    }

    uint32_t decode_add(uint32_t ins) {
        return (ins >> 10) & 0xFFF;
    }

    bool is_b_or_bl(uint32_t ins) {
        return (ins & 0x7C000000) == 0x14000000;
    }

    uintptr_t decode_b_or_bl(uintptr_t pc, uint32_t ins) {
        int32_t imm26 = ins & 0x03FFFFFF;
        if (imm26 & 0x02000000) imm26 |= 0xFC000000; // Sign extend
        return pc + (imm26 * 4);
    }
}

// ─────────────────────────────────────────────────────────────────
// Function Implementations
// ─────────────────────────────────────────────────────────────────

UE4Info FindAndOpenLibUE4() {
    UE4Info info;

    struct Ctx {
        UE4Info* out;
        int      bestRank;
    };
    Ctx ctx = { &info, 999 };

    dl_iterate_phdr([](dl_phdr_info* phdr, size_t, void* data) {
        auto* c = (Ctx*)data;
        if (!phdr || !phdr->dlpi_name || phdr->dlpi_name[0] == '\0') return 0;

        const int rank = ue_lib_rank(phdr->dlpi_name);
        if (rank >= 0 && rank < c->bestRank) {
            c->bestRank = rank;
            c->out->base = phdr->dlpi_addr;
            copy_path(c->out->apkPath, sizeof(c->out->apkPath), phdr->dlpi_name);
            c->out->handle = dlopen_no_load(phdr->dlpi_name);

            // Prefer libUE4.so when both names are present, otherwise keep
            // iterating so a later libUE4.so can beat libUnreal.so.
            return rank == 0 ? 1 : 0;
        }
        return 0;
    }, &ctx);

    return info;
}

UEVersionInfo FindUEVersion(const UE4Info& info) {
    UEVersionInfo out;

    if (try_manifest_meta_data(out)) return out;
    if (info.base != 0 && try_engine_version_symbol(info, out)) return out;

    copy_text(out.method, sizeof(out.method), "unresolved");
    copy_text(out.detail, sizeof(out.detail),
              "AndroidManifest metadata missing; slow UE version scan disabled");
    return out;
}

void* FindSymbolDynamic(uintptr_t base, const char* name)
{
    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(base);
    auto* phdr = reinterpret_cast<Elf64_Phdr*>(base + ehdr->e_phoff);

    Elf64_Dyn*  dyn    = nullptr;
    Elf64_Sym*  dynsym = nullptr;
    const char* dynstr = nullptr;
    size_t      syment = sizeof(Elf64_Sym);
    size_t      symcnt = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<Elf64_Dyn*>(base + phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return nullptr;

    uint32_t* gnu_hash = nullptr;
    uint32_t* sysv_hash = nullptr;
    for (Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   dynsym   = reinterpret_cast<Elf64_Sym*> (base + d->d_un.d_ptr); break;
            case DT_STRTAB:   dynstr   = reinterpret_cast<const char*>(base + d->d_un.d_ptr); break;
            case DT_SYMENT:   syment   = d->d_un.d_val; break;
            case DT_GNU_HASH: gnu_hash = reinterpret_cast<uint32_t*>  (base + d->d_un.d_ptr); break;
            case DT_HASH:     sysv_hash = reinterpret_cast<uint32_t*> (base + d->d_un.d_ptr); break;
        }
    }
    if (!dynsym || !dynstr) return nullptr;

    if (gnu_hash) {
        uint32_t nbuckets  = gnu_hash[0];
        uint32_t symoffset = gnu_hash[1];
        uint32_t bloom_sz  = gnu_hash[2];
        uint32_t* buckets  = gnu_hash + 4 + bloom_sz * 2;
        uint32_t* chains   = buckets + nbuckets;

        uint32_t max_sym = symoffset;
        for (uint32_t b = 0; b < nbuckets; b++) {
            uint32_t idx = buckets[b];
            if (!idx) continue;
            while (!(chains[idx - symoffset] & 1)) idx++;
            if (idx > max_sym) max_sym = idx;
        }
        symcnt = max_sym + 1;
    } else if (sysv_hash) {
        symcnt = sysv_hash[1];  // nchain
    }

    for (size_t i = 0; i < symcnt; i++) {
        auto* sym = reinterpret_cast<Elf64_Sym*>(
                reinterpret_cast<uint8_t*>(dynsym) + i * syment);
        if (!strcmp(dynstr + sym->st_name, name))
            return reinterpret_cast<void*>(base + sym->st_value);
    }
    return nullptr;
}

void* FindSymbolDisk(const char* path, uintptr_t base, const char* name)
{
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;

    Elf64_Ehdr ehdr{};
    fread(&ehdr, sizeof(ehdr), 1, f);

    std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
    fseek(f, (long)ehdr.e_shoff, SEEK_SET);
    fread(shdrs.data(), sizeof(Elf64_Shdr), ehdr.e_shnum, f);

    std::vector<char> shstrtab(shdrs[ehdr.e_shstrndx].sh_size);
    fseek(f, (long)shdrs[ehdr.e_shstrndx].sh_offset, SEEK_SET);
    fread(shstrtab.data(), 1, shstrtab.size(), f);

    Elf64_Shdr *symtab = nullptr, *strtab = nullptr;

    for (auto& s : shdrs) {
        const char* sname = shstrtab.data() + s.sh_name;
        if (!strcmp(sname, ".symtab")) symtab = &s;
        if (!strcmp(sname, ".strtab")) strtab = &s;
    }

    if (!symtab || !strtab) { fclose(f); return nullptr; }

    std::vector<Elf64_Sym> syms(symtab->sh_size / sizeof(Elf64_Sym));
    fseek(f, (long)symtab->sh_offset, SEEK_SET);
    fread(syms.data(), sizeof(Elf64_Sym), syms.size(), f);

    std::vector<char> strtab_data(strtab->sh_size);
    fseek(f, (long)strtab->sh_offset, SEEK_SET);
    fread(strtab_data.data(), 1, strtab_data.size(), f);
    fclose(f);

    for (auto& sym : syms) {
        if (!strcmp(strtab_data.data() + sym.st_name, name))
            return reinterpret_cast<void*>(base + sym.st_value);
    }
    return nullptr;
}

namespace mem {
    uintptr_t FindTargetByAdrpAdd(uintptr_t search_start, size_t search_size) {
        uint32_t* ptr = (uint32_t*)search_start;
        uint32_t* end = ptr + (search_size / sizeof(uint32_t));
        
        uintptr_t adrp_target = 0;
        
        while (ptr < end) {
            uint32_t ins = *ptr;
            if (is_adrp(ins)) {
                adrp_target = decode_adrp((uintptr_t)ptr, ins);
            } 
            else if (is_add(ins) && adrp_target != 0) {
                return adrp_target + decode_add(ins);
            }
            ptr++;
        }
        return 0;
    }

    uintptr_t FindInsideOrFollowBranches(uintptr_t search_start, size_t search_size) {
        uintptr_t target = FindTargetByAdrpAdd(search_start, search_size);
        if (target) return target;

        uint32_t* ptr = (uint32_t*)search_start;
        uint32_t* end = ptr + (search_size / sizeof(uint32_t));
        
        while (ptr < end) {
            uint32_t ins = *ptr;
            if (is_b_or_bl(ins)) {
                uintptr_t branch_target = decode_b_or_bl((uintptr_t)ptr, ins);
                uintptr_t recurse_target = FindTargetByAdrpAdd(branch_target, 0x100);
                if (recurse_target) return recurse_target;
            }
            ptr++;
        }
        return 0;
    }
}

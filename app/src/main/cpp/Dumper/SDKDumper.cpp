// =============================================================================
// Dumper/SDKDumper.cpp
// -----------------------------------------------------------------------------
// Generates complete SDK headers and out-of-line function implementation files.
// Walk through reflection objects, topologically sort packages, and emit
// classes, structs, enums, parameter frames, and ProcessEvent/direct native
// call wrapper functions.
// =============================================================================

#include "SDKDumper.h"

#include "NameArray.h"
#include "ObjectArray.h"
#include "SafeMemory.h"
#include "StructOffsetFinder.h"
#include "UEStructs.h"

#include <algorithm>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <fstream>
#include <functional>
#include <link.h>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define DUMPER_LOG_TAG "Dumper7"
#define DLOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)
#define DLOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN, DUMPER_LOG_TAG, fmt, ##__VA_ARGS__)

namespace SDKDumper {

namespace {

// =============================================================================
// Inline UE read helpers (small enough to duplicate; avoids cross-TU coupling).
// =============================================================================

INTERNAL std::string ReadFNameAt(uintptr_t addr) noexcept {
    auto comp = SafeMemory::SafeReadAny<int32_t>(addr);
    if (!comp || *comp < 0 || *comp > 0x40000000) return {};
    return UE::NameArray::GetName(*comp);
}
INTERNAL std::string ReadObjectName(uintptr_t obj) noexcept {
    return ReadFNameAt(obj + Off::UObject::NamePrivate);
}
INTERNAL uintptr_t ReadOuter(uintptr_t obj) noexcept {
    auto v = SafeMemory::SafeReadAny<uintptr_t>(obj + Off::UObject::OuterPrivate);
    return v ? *v : 0;
}
INTERNAL uintptr_t ReadClassPtr(uintptr_t obj) noexcept {
    auto v = SafeMemory::SafeReadAny<uintptr_t>(obj + Off::UObject::ClassPrivate);
    return v ? *v : 0;
}
INTERNAL std::string ReadObjectClassName(uintptr_t obj) noexcept {
    uintptr_t cls = ReadClassPtr(obj);
    return cls ? ReadObjectName(cls) : std::string{};
}

// =============================================================================
// Collection-time domain types.
// =============================================================================

enum class Kind { Other, Package, Class, Struct, Enum, Function };

struct ObjMeta {
    uintptr_t   address        = 0;
    int32_t     index          = 0;
    std::string name;
    Kind        kind           = Kind::Other;
    uintptr_t   outerPackage   = 0;
    uintptr_t   superStruct    = 0;
    int32_t     propertiesSize = 0;   // UStruct::PropertiesSize (struct total size)
    int32_t     minAlignment   = 0;   // UStruct::MinAlignment (required alignment)
    uint32_t    classFlags     = 0;   // UClass::ClassFlags (Abstract, Interface, etc.)
};

struct PackageInfo {
    uintptr_t              address = 0;
    std::string            name;
    std::string            displayName;
    std::vector<uintptr_t> classes;
    std::vector<uintptr_t> structs;
    std::vector<uintptr_t> enums;
    std::vector<uintptr_t> functions;
    std::set<uintptr_t>    dependsOn;
};

INTERNAL std::string CleanUnrealDisplayPath(std::string name) noexcept {
    constexpr const char* kScriptPrefix = "/Script/";
    constexpr size_t kScriptPrefixLen = 8;

    if (name.rfind(kScriptPrefix, 0) == 0) {
        name.erase(0, kScriptPrefixLen);
    } else {
        size_t start = 0;
        while (start < name.size() && (name[start] == '/' || name[start] == '\\')) {
            ++start;
        }
        if (start > 0) name.erase(0, start);
    }

    for (char& c : name) {
        if (c == '/' || c == '\\') c = '.';
    }
    return name;
}

INTERNAL Kind ClassifyByName(const std::string& className) noexcept {
    if (className == "Class")        return Kind::Class;
    if (className == "ScriptStruct") return Kind::Struct;
    if (className == "Enum")         return Kind::Enum;
    if (className == "Function")     return Kind::Function;
    if (className == "Package")      return Kind::Package;
    return Kind::Other;
}

// =============================================================================
// Atomic state.
// =============================================================================

std::mutex          g_mu;
Progress            g_progress;
std::atomic<bool>   g_running{false};
std::atomic<bool>   g_cancel{false};
std::thread         g_worker;

// Maps an enum's address to its smallest observed field storage size (in bytes).
// Capping the enum's underlying type to match this storage size ensures
// parameters and struct fields have correct compile-time offsets and alignment.
std::unordered_map<uintptr_t, int32_t> g_enumSizeHints;
std::mutex                              g_enumSizeHintsMutex;

INTERNAL void SetPhase(Phase p) noexcept {
    std::lock_guard<std::mutex> lk(g_mu);
    g_progress.phase = p;
}
INTERNAL void SetCurrentPackage(const std::string& s) noexcept {
    std::lock_guard<std::mutex> lk(g_mu);
    g_progress.currentPackage = s;
}
INTERNAL void IncWritten() noexcept {
    std::lock_guard<std::mutex> lk(g_mu);
    ++g_progress.writtenPackages;
}
INTERNAL void SetError(const std::string& s) noexcept {
    std::lock_guard<std::mutex> lk(g_mu);
    g_progress.error = s;
    g_progress.phase = Phase::Failed;
}

INTERNAL bool EnsurePropertyLayoutReady() noexcept {
    auto adv = StructOffsetFinder::FindAdvancedLayout();
    if (adv.ok) {
        StructOffsetFinder::Apply(adv);
    }

    DLOGI("[sdk][layout] advanced ok=%d samples class=%d struct=%d func=%d fprop=%d uprop=%d",
          (int)adv.ok, adv.uclassSamples, adv.uscriptStructSamples,
          adv.ufuncSamples, adv.fpropSamples, adv.upropSamples);
    DLOGI("[sdk][layout] UStruct ChildProperties=0x%x PropertiesSize=0x%x UField::Next=0x%x",
          Off::UStruct::ChildProperties, Off::UStruct::PropertiesSize, Off::UField::Next);
    DLOGI("[sdk][layout] FField Class=0x%x Next=0x%x Name=0x%x FFieldClass::Name=0x%x",
          Off::FField::ClassPrivate, Off::FField::Next,
          Off::FField::NamePrivate, Off::FFieldClass::Name);
    DLOGI("[sdk][layout] FProperty ArrayDim=0x%x ElementSize=0x%x Offset_Internal=0x%x Next=0x%x Size=0x%x",
          Off::FProperty::ArrayDim, Off::FProperty::ElementSize,
          Off::FProperty::Offset_Internal, Off::FProperty::Next,
          Off::FProperty::Size);
    DLOGI("[sdk][layout] UProperty ArrayDim=0x%x ElementSize=0x%x Offset_Internal=0x%x Size=0x%x",
          Off::UProperty::ArrayDim, Off::UProperty::ElementSize,
          Off::UProperty::Offset_Internal, Off::UProperty::Size);
    return adv.ok;
}

// =============================================================================
// Phase 1 — collection.
// =============================================================================

INTERNAL bool CollectObjects(std::vector<ObjMeta>& metas,
                             std::unordered_map<uintptr_t, PackageInfo>& packages) noexcept {
    SafeMemory::ScopedSigSegvGuard guard;
    int32_t total = UE::ObjectArray::Num();
    metas.reserve(static_cast<size_t>(total));

    // Cache: class pointer → class name. Most objects share one of <500 classes,
    // so caching collapses the per-object decode cost.
    std::unordered_map<uintptr_t, std::string> classNameCache;
    classNameCache.reserve(1024);

    auto classNameOf = [&](uintptr_t obj) -> std::string {
        uintptr_t cls = 0;
        guard.Try([&] { cls = ReadClassPtr(obj); });
        if (cls == 0) return {};
        auto it = classNameCache.find(cls);
        if (it != classNameCache.end()) return it->second;
        std::string n;
        guard.Try([&] { n = ReadObjectName(cls); });
        classNameCache.emplace(cls, n);
        return n;
    };

    UE::ObjectArray::ForEach([&](UE::UObject* obj) {
        if (g_cancel.load()) return;
        uintptr_t addr = reinterpret_cast<uintptr_t>(obj);

        std::string clsName = classNameOf(addr);
        Kind kind = ClassifyByName(clsName);
        if (kind == Kind::Other) return;  // not a reflection object

        ObjMeta m;
        m.address = addr;
        m.kind    = kind;
        guard.Try([&] {
            auto idx = SafeMemory::SafeReadAny<int32_t>(addr + Off::UObject::InternalIndex);
            if (idx) m.index = *idx;
            m.name = ReadObjectName(addr);
        });

        // Resolve outer package by walking OuterPrivate chain.
        uintptr_t pkg = 0;
        if (kind == Kind::Package) {
            pkg = addr;  // packages are their own anchor
        } else {
            uintptr_t cur = addr;
            for (int hop = 0; hop < 32; ++hop) {
                uintptr_t outer = 0;
                guard.Try([&] { outer = ReadOuter(cur); });
                if (outer == 0) break;
                if (classNameOf(outer) == "Package") { pkg = outer; break; }
                cur = outer;
            }
        }
        if (pkg == 0) return;
        m.outerPackage = pkg;

        // SuperStruct + PropertiesSize + MinAlignment: UStruct fields.
        // ClassFlags: UClass-specific (Abstract, Interface, Config, etc.)
        if (kind == Kind::Class || kind == Kind::Struct || kind == Kind::Function) {
            guard.Try([&] {
                auto super = SafeMemory::SafeReadAny<uintptr_t>(
                        addr + Off::UStruct::SuperStruct);
                if (super) m.superStruct = *super;
                auto sz = SafeMemory::SafeReadAny<int32_t>(
                        addr + Off::UStruct::PropertiesSize);
                if (sz && *sz > 0 && *sz < (1 << 24)) m.propertiesSize = *sz;
                auto align = SafeMemory::SafeReadAny<int32_t>(
                        addr + Off::UStruct::MinAlignment);
                if (align && *align > 0 && *align <= 128) m.minAlignment = *align;
            });
            if (kind == Kind::Class) {
                guard.Try([&] {
                    auto flags = SafeMemory::SafeReadAny<uint32_t>(
                            addr + Off::UClass::ClassFlags);
                    if (flags) m.classFlags = *flags;
                });
            }
        }

        // Register the package once.
        if (packages.find(pkg) == packages.end()) {
            PackageInfo p;
            p.address = pkg;
            std::string raw;
            guard.Try([&] { raw = ReadObjectName(pkg); });
            p.displayName = CleanUnrealDisplayPath(raw);
            // UE package names are paths like "/Script/Engine" or "/Game/Foo".
            // Strip the leading slashes so files become "Script_Engine_classes.hpp"
            // rather than "_Script_Engine_classes.hpp".
            size_t startPos = 0;
            while (startPos < raw.size() && raw[startPos] == '/') ++startPos;
            std::string safe;
            safe.reserve(raw.size() - startPos);
            for (size_t i = startPos; i < raw.size(); ++i) {
                char c = raw[i];
                if (c == '/' || c == '\\' || c == '.' || c == ' ' || c == '-') safe += '_';
                else safe += c;
            }
            if (safe.empty()) safe = "UnnamedPackage";
            p.name = safe;
            if (p.displayName.empty()) p.displayName = safe;
            packages.emplace(pkg, std::move(p));
        }

        metas.push_back(std::move(m));

        std::lock_guard<std::mutex> lk(g_mu);
        ++g_progress.processedObjects;
        if      (kind == Kind::Class)    ++g_progress.classCount;
        else if (kind == Kind::Struct)   ++g_progress.structCount;
        else if (kind == Kind::Enum)     ++g_progress.enumCount;
        else if (kind == Kind::Function) ++g_progress.functionCount;
    });

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_progress.totalObjects  = total;
        g_progress.totalPackages = static_cast<int32_t>(packages.size());
    }

    // Distribute metas into their package's typed buckets.
    for (const auto& m : metas) {
        auto it = packages.find(m.outerPackage);
        if (it == packages.end()) continue;
        switch (m.kind) {
            case Kind::Class:    it->second.classes.push_back(m.address);   break;
            case Kind::Struct:   it->second.structs.push_back(m.address);   break;
            case Kind::Enum:     it->second.enums.push_back(m.address);     break;
            case Kind::Function: it->second.functions.push_back(m.address); break;
            default: break;
        }
    }

    // Inter-package SuperStruct dependencies.
    std::unordered_map<uintptr_t, uintptr_t> objToPackage;
    objToPackage.reserve(metas.size());
    for (const auto& m : metas) objToPackage[m.address] = m.outerPackage;
    for (const auto& m : metas) {
        if (m.superStruct == 0) continue;
        auto it = objToPackage.find(m.superStruct);
        if (it == objToPackage.end()) continue;
        if (it->second == m.outerPackage) continue;
        packages[m.outerPackage].dependsOn.insert(it->second);
    }
    return !metas.empty();
}

// =============================================================================
// Phase 2 — topological sort.
// =============================================================================

INTERNAL std::vector<uintptr_t> TopoSortPackages(
        const std::unordered_map<uintptr_t, PackageInfo>& packages) noexcept {
    std::vector<uintptr_t> order;
    order.reserve(packages.size());
    std::unordered_set<uintptr_t> visited, onStack;

    std::function<void(uintptr_t)> visit = [&](uintptr_t a) {
        if (visited.count(a)) return;
        if (onStack.count(a)) return;  // cycle — break by returning
        onStack.insert(a);
        auto it = packages.find(a);
        if (it != packages.end()) {
            for (auto dep : it->second.dependsOn) visit(dep);
        }
        onStack.erase(a);
        visited.insert(a);
        order.push_back(a);
    };
    for (const auto& kv : packages) visit(kv.first);
    return order;
}

// =============================================================================
// Phase 3 — header emission.
// =============================================================================

// Forward-declared — defined alongside MakeDirRec further down.
INTERNAL std::string SanitizeIdent(const std::string& name) noexcept;
INTERNAL std::string CppStringLiteral(const std::string& s) noexcept;

// =============================================================================
// Helper structures and methods to traverse FProperty and UProperty chains.
// Uses runtime offset layouts discovered by StructOffsetFinder to resolve types,
// sizes, and offsets.
// =============================================================================

struct PropertyInfo {
    std::string name;
    std::string typeName;
    uintptr_t   structTarget = 0;
    uintptr_t   enumTarget = 0;
    std::vector<uintptr_t> typeTargets;
    uint64_t    propertyFlags = 0;
    int32_t     offset      = 0;
    int32_t     elementSize = 0;
    int32_t     arrayDim    = 1;
    bool        isBool      = false;
    bool        isBitfield  = false;
    uint8_t     boolMask    = 0;
};

struct FunctionInfo {
    std::string name;       // sanitized C++ identifier
    std::string rawName;    // original UE name for runtime lookup
    std::string returnType;
    std::vector<PropertyInfo> params;
    PropertyInfo returnParam;
    uint32_t    functionFlags = 0;
    int32_t     numParms = 0;
    int32_t     paramsSize = 0;   // UFunction::PropertiesSize — total parameter frame size
    uintptr_t   nativeFuncPtr = 0; // UFunction::Func — native implementation address (0 if BP-only)
    bool        hasReturnParam = false;
};

constexpr uint64_t CPF_ConstParm     = 0x0000000000000002ULL;
constexpr uint64_t CPF_BlueprintVisible = 0x0000000000000004ULL;
constexpr uint64_t CPF_BlueprintReadOnly = 0x0000000000000010ULL;
constexpr uint64_t CPF_Net           = 0x0000000000000020ULL;
constexpr uint64_t CPF_Parm          = 0x0000000000000080ULL;
constexpr uint64_t CPF_OutParm       = 0x0000000000000100ULL;
constexpr uint64_t CPF_ReturnParm    = 0x0000000000000400ULL;
constexpr uint64_t CPF_ReferenceParm = 0x0000000000000800ULL;
constexpr uint64_t CPF_Config        = 0x0000000000004000ULL;
constexpr uint64_t CPF_EditConst     = 0x0000000000020000ULL;
constexpr uint64_t CPF_Transient     = 0x0000000000002000ULL;
constexpr uint64_t CPF_RepNotify     = 0x0000000100000000ULL;
constexpr uint64_t CPF_SaveGame      = 0x0000000001000000ULL;

constexpr uint32_t FUNC_Static = 0x00002000U;

// Build a short metadata tag string from property flags.
// Shown as a trailing comment on each field for quick reference.
INTERNAL std::string PropertyFlagTags(uint64_t flags) noexcept {
    std::string tags;
    if (flags & CPF_Net)              tags += "Net,";
    if (flags & CPF_Config)           tags += "Config,";
    if (flags & CPF_Transient)        tags += "Transient,";
    if (flags & CPF_RepNotify)        tags += "RepNotify,";
    if (flags & CPF_SaveGame)         tags += "SaveGame,";
    if (flags & CPF_EditConst)        tags += "EditConst,";
    if (flags & CPF_BlueprintReadOnly) tags += "BPReadOnly,";
    if (!tags.empty()) tags.pop_back(); // remove trailing comma
    return tags;
}

INTERNAL std::string ReadFieldClassName(
        uintptr_t fieldAddr,
        std::unordered_map<uintptr_t, std::string>& cache) noexcept {
    SafeMemory::ScopedSigSegvGuard guard;
    uintptr_t cls = 0;
    guard.Try([&] {
        auto v = SafeMemory::SafeReadAny<uintptr_t>(fieldAddr + Off::FField::ClassPrivate);
        if (v) cls = *v;
    });
    if (cls == 0) return {};
    auto it = cache.find(cls);
    if (it != cache.end()) return it->second;
    std::string name;
    guard.Try([&] {
        auto comp = SafeMemory::SafeReadAny<int32_t>(cls + Off::FFieldClass::Name);
        if (comp) name = UE::NameArray::GetName(*comp);
    });
    cache.emplace(cls, name);
    return name;
}

INTERNAL std::string ReadFieldNameAt(uintptr_t fieldAddr, int32_t nameOff) noexcept {
    SafeMemory::ScopedSigSegvGuard guard;
    std::string name;
    guard.Try([&] {
        auto comp = SafeMemory::SafeReadAny<int32_t>(fieldAddr + nameOff);
        if (comp) name = UE::NameArray::GetName(*comp);
    });
    return name;
}

INTERNAL bool IsPropertyTypeName(const std::string& name) noexcept {
    return name == "BoolProperty" || name == "ByteProperty" ||
           name == "Int8Property" || name == "Int16Property" ||
           name == "IntProperty" || name == "Int64Property" ||
           name == "UInt16Property" || name == "UInt32Property" ||
           name == "UInt64Property" || name == "FloatProperty" ||
           name == "DoubleProperty" || name == "NameProperty" ||
           name == "StrProperty" || name == "TextProperty" ||
           name == "ObjectProperty" || name == "ClassProperty" ||
           name == "WeakObjectProperty" || name == "LazyObjectProperty" ||
           name == "SoftObjectProperty" || name == "SoftClassProperty" ||
           name == "InterfaceProperty" || name == "StructProperty" ||
           name == "EnumProperty" || name == "ArrayProperty" ||
           name == "SetProperty" || name == "MapProperty" ||
           name == "DelegateProperty" || name == "MulticastDelegateProperty" ||
           name == "MulticastInlineDelegateProperty" ||
           name == "MulticastSparseDelegateProperty" ||
           name == "FieldPathProperty";
}

INTERNAL bool IsLikelyMemberName(const std::string& name) noexcept {
    if (name.empty() || name.size() > 128) return false;
    if (IsPropertyTypeName(name)) return false;
    if (name.find("Property__") != std::string::npos) return false;
    if (name.find("StructProperty") != std::string::npos) return false;
    if (name == "None" || name == "Class" || name == "Function" ||
        name == "ScriptStruct" || name == "Package") return false;

    bool sawAlpha = false;
    for (unsigned char c : name) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) sawAlpha = true;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
            continue;
        }
        return false;
    }
    return sawAlpha;
}

INTERNAL std::string ReadFieldName(uintptr_t fieldAddr) noexcept {
    std::string primary = ReadFieldNameAt(fieldAddr, Off::FField::NamePrivate);
    if (IsLikelyMemberName(primary)) return primary;

    for (int32_t off = 0x18; off <= 0x60; off += 4) {
        if (off == Off::FField::NamePrivate) continue;
        std::string candidate = ReadFieldNameAt(fieldAddr, off);
        if (IsLikelyMemberName(candidate)) {
            static std::atomic<int> fallbackLogs{0};
            int logIndex = fallbackLogs.fetch_add(1, std::memory_order_relaxed);
            if (logIndex < 16) {
                DLOGI("[sdk][field] FField name fallback: configured=0x%x used=0x%x name=%s",
                      Off::FField::NamePrivate, off, candidate.c_str());
            }
            return candidate;
        }
    }
    return primary;
}

// Forward decl — ResolveTypeName recurses into ResolveSubproperty for
// container element types (TArray<T>, TMap<K,V>, TSet<T>).
INTERNAL int32_t PropertySubtypeBase(bool legacyProperty) noexcept;

INTERNAL std::string ResolveTypeName(
        uintptr_t propAddr,
        const std::string& propClass,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::unordered_map<uintptr_t, std::string>& fieldClassCache,
        int depth,
        bool legacyProperty) noexcept;

INTERNAL std::string ResolveSubproperty(
        uintptr_t innerProp,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::unordered_map<uintptr_t, std::string>& fieldClassCache,
        int depth,
        bool legacyProperty) noexcept {
    if (innerProp == 0 || depth > 8) return "uint8_t";
    std::string cls = legacyProperty
            ? ReadObjectClassName(innerProp)
            : ReadFieldClassName(innerProp, fieldClassCache);
    if (cls.empty()) return "uint8_t";
    return ResolveTypeName(innerProp, cls, byAddr, fieldClassCache,
                           depth + 1, legacyProperty);
}

INTERNAL bool StartsWith(const std::string& s, const char* prefix) noexcept {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

INTERNAL std::string ClassType(const std::string& name) noexcept {
    if (name.empty() || name == "void" || name == "void*" ||
        name == "uint8_t" || name == "UObject") {
        return name == "UObject" ? "class UObject" : name;
    }
    if (StartsWith(name, "class ")) return name;
    return "class " + name;
}

INTERNAL std::string StructType(const std::string& name) noexcept {
    if (name.empty() || name == "void" || name == "void*" ||
        name == "uint8_t" || name == "FName" || name == "FString" ||
        name == "FText") {
        return name;
    }
    if (StartsWith(name, "struct ")) return name;
    return "struct " + name;
}

INTERNAL std::string ResolveTypeName(
        uintptr_t propAddr,
        const std::string& propClass,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::unordered_map<uintptr_t, std::string>& fieldClassCache,
        int depth,
        bool legacyProperty) noexcept {

    // Trivial scalar/string/delegate types — no subclass field reads.
    if (propClass == "BoolProperty")    return "bool";
    if (propClass == "Int8Property")    return "int8_t";
    if (propClass == "Int16Property")   return "int16_t";
    if (propClass == "IntProperty")     return "int32_t";
    if (propClass == "Int64Property")   return "int64_t";
    if (propClass == "UInt16Property")  return "uint16_t";
    if (propClass == "UInt32Property")  return "uint32_t";
    if (propClass == "UInt64Property")  return "uint64_t";
    if (propClass == "FloatProperty")   return "float";
    if (propClass == "DoubleProperty")  return "double";
    if (propClass == "NameProperty")    return "FName";
    if (propClass == "StrProperty")     return "FString";
    if (propClass == "TextProperty")    return "FText";
    if (propClass == "DelegateProperty") return "FScriptDelegate";
    if (propClass == "MulticastDelegateProperty" ||
        propClass == "MulticastInlineDelegateProperty") return "FMulticastScriptDelegate";
    if (propClass == "MulticastSparseDelegateProperty") return "FMulticastSparseDelegate";

    SafeMemory::ScopedSigSegvGuard guard;
    auto readPtrAt = [&](int32_t off) -> uintptr_t {
        uintptr_t p = 0;
        guard.Try([&] {
            auto v = SafeMemory::SafeReadAny<uintptr_t>(propAddr + off);
            if (v) p = *v;
        });
        return p;
    };

    auto namedTarget = [&](uintptr_t addr,
                           char prefix,
                           const char* fallback,
                           const char* expectedClass) -> std::string {
        if (addr == 0) return fallback;
        auto it = byAddr.find(addr);
        if (it != byAddr.end()) {
            std::string out; out += prefix;
            out += SanitizeIdent(it->second->name);
            return out;
        }

        std::string clsName;
        std::string objName;
        guard.Try([&] {
            clsName = ReadObjectClassName(addr);
            if (!expectedClass || clsName == expectedClass) {
                objName = ReadObjectName(addr);
            }
        });
        if (expectedClass && clsName != expectedClass) return fallback;
        if (!IsLikelyMemberName(objName)) return fallback;

        static std::atomic<int> directTargetLogs{0};
        int logIndex = directTargetLogs.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 24) {
            DLOGI("[sdk][type] direct target name: expected=%s target=0x%lx name=%s",
                  expectedClass ? expectedClass : "?",
                  static_cast<unsigned long>(addr), objName.c_str());
        }

        std::string out; out += prefix;
        out += SanitizeIdent(objName);
        return out;
    };

    int32_t subtypeBase = PropertySubtypeBase(legacyProperty);
    const int32_t subSlot0 = subtypeBase;
    const int32_t subSlot1 = subtypeBase + (int32_t)sizeof(uintptr_t);

    if (propClass == "ByteProperty") {
        return namedTarget(readPtrAt(subSlot0), 'E', "uint8_t", "Enum");
    }
    if (propClass == "ObjectProperty") {
        return ClassType(namedTarget(readPtrAt(subSlot0), 'U', "UObject", "Class")) + "*";
    }
    if (propClass == "ClassProperty") {
        return "TSubclassOf<" +
               ClassType(namedTarget(readPtrAt(subSlot0), 'U', "UObject", "Class")) + ">";
    }
    if (propClass == "WeakObjectProperty") {
        return "TWeakObjectPtr<" +
               ClassType(namedTarget(readPtrAt(subSlot0), 'U', "UObject", "Class")) + ">";
    }
    if (propClass == "LazyObjectProperty") {
        return "TWeakObjectPtr<" +
               ClassType(namedTarget(readPtrAt(subSlot0), 'U', "UObject", "Class")) + ">";
    }
    if (propClass == "SoftObjectProperty") {
        return "TSoftObjectPtr<" +
               ClassType(namedTarget(readPtrAt(subSlot0), 'U', "UObject", "Class")) + ">";
    }
    if (propClass == "SoftClassProperty") {
        return "TSoftClassPtr<" +
               ClassType(namedTarget(readPtrAt(subSlot0), 'U', "UObject", "Class")) + ">";
    }
    if (propClass == "InterfaceProperty") {
        // InterfaceProperty is backed by FScriptInterface (UObject* + void*),
        // so it must be emitted as a 16-byte TScriptInterface<T>, not a plain
        // pointer (which would be 8 bytes and break member offsets).
        return "TScriptInterface<" +
               ClassType(namedTarget(readPtrAt(subSlot0), 'U', "UObject", "Class")) + ">";
    }
    if (propClass == "StructProperty") {
        return StructType(namedTarget(readPtrAt(subSlot0), 'F', "uint8_t", "ScriptStruct"));
    }
    if (propClass == "EnumProperty") {
        return namedTarget(readPtrAt(subSlot1), 'E', "uint8_t", "Enum");
    }
    if (propClass == "ArrayProperty") {
        return "TArray<" +
               ResolveSubproperty(readPtrAt(subSlot0),
                                  byAddr, fieldClassCache, depth,
                                  legacyProperty) + ">";
    }
    if (propClass == "SetProperty") {
        return "TSet<" +
               ResolveSubproperty(readPtrAt(subSlot0),
                                  byAddr, fieldClassCache, depth,
                                  legacyProperty) + ">";
    }
    if (propClass == "MapProperty") {
        std::string k = ResolveSubproperty(readPtrAt(subSlot0),
                                           byAddr, fieldClassCache, depth,
                                           legacyProperty);
        std::string v = ResolveSubproperty(readPtrAt(subSlot1),
                                           byAddr, fieldClassCache, depth,
                                           legacyProperty);
        return "TMap<" + k + ", " + v + ">";
    }
    if (propClass == "FieldPathProperty") return "void*";

    return "uint8_t";  // unknown — emit a single byte and let elementSize set the size
}

INTERNAL bool IsPropertyClassName(const std::string& cls) noexcept {
    // Filter for UProperty subclasses on the Children chain. Their class
    // names always end with "Property" — IntProperty, ObjectProperty, etc.
    if (cls.size() < 8) return false;
    return cls.compare(cls.size() - 8, 8, "Property") == 0;
}

INTERNAL std::string ReadUObjectClassNameCached(
        uintptr_t obj,
        std::unordered_map<uintptr_t, std::string>& cache) noexcept {
    if (obj == 0) return {};
    uintptr_t cls = 0;
    SafeMemory::ScopedSigSegvGuard guard;
    guard.Try([&] { cls = ReadClassPtr(obj); });
    if (cls == 0) return {};

    auto it = cache.find(cls);
    if (it != cache.end()) return it->second;

    std::string name;
    guard.Try([&] { name = ReadObjectName(cls); });
    cache.emplace(cls, name);
    return name;
}

INTERNAL int32_t PropertySubtypeBase(bool legacyProperty) noexcept {
    int32_t subtypeBase = legacyProperty ? Off::UProperty::Size : Off::FProperty::Size;
    if (subtypeBase < 0x40 || subtypeBase > 0x180) {
        subtypeBase = legacyProperty ? 0x70 : 0x78;
    }
    return subtypeBase;
}

INTERNAL int32_t BitIndex(uint8_t mask) noexcept {
    for (int32_t i = 0; i < 8; ++i) {
        if (mask & (uint8_t)(1u << i)) return i;
    }
    return 8;
}

INTERNAL void FillBoolMetadata(uintptr_t propAddr,
                               bool legacyProperty,
                               PropertyInfo& p) noexcept {
    p.isBool = true;

    const int32_t base = PropertySubtypeBase(legacyProperty);
    SafeMemory::ScopedSigSegvGuard guard;
    uint8_t fieldSize = 0;
    uint8_t byteOffset = 0;
    uint8_t byteMask = 0;
    uint8_t fieldMask = 0;

    guard.Try([&] {
        auto fs = SafeMemory::SafeReadAny<uint8_t>(propAddr + base + 0);
        auto bo = SafeMemory::SafeReadAny<uint8_t>(propAddr + base + 1);
        auto bm = SafeMemory::SafeReadAny<uint8_t>(propAddr + base + 2);
        auto fm = SafeMemory::SafeReadAny<uint8_t>(propAddr + base + 3);
        if (fs) fieldSize = *fs;
        if (bo) byteOffset = *bo;
        if (bm) byteMask = *bm;
        if (fm) fieldMask = *fm;
    });

    uint8_t mask = fieldMask ? fieldMask : byteMask;
    if (fieldSize == 0 || fieldSize > 8 || byteOffset >= fieldSize || mask == 0) {
        return;
    }

    p.offset += byteOffset;
    p.elementSize = 1;
    p.arrayDim = 1;
    p.boolMask = mask;
    p.isBitfield = mask != 0xFF;
}

INTERNAL void AddUniqueTypeTarget(PropertyInfo& p, uintptr_t target) noexcept {
    if (target == 0) return;
    if (std::find(p.typeTargets.begin(), p.typeTargets.end(), target) ==
        p.typeTargets.end()) {
        p.typeTargets.push_back(target);
    }
}

INTERNAL uintptr_t ReadPropertyPayloadPtr(uintptr_t propAddr, int32_t off) noexcept {
    uintptr_t out = 0;
    SafeMemory::ScopedSigSegvGuard guard;
    guard.Try([&] {
        auto v = SafeMemory::SafeReadAny<uintptr_t>(propAddr + off);
        if (v) out = *v;
    });
    return out;
}

INTERNAL void CollectPropertyTypeTargets(
        uintptr_t propAddr,
        const std::string& clsName,
        bool legacyProperty,
        PropertyInfo& p,
        std::unordered_map<uintptr_t, std::string>& fieldClassCache,
        int depth = 0) noexcept {
    if (propAddr == 0 || clsName.empty() || depth > 8) return;

    const int32_t subtypeBase = PropertySubtypeBase(legacyProperty);
    const int32_t subSlot0 = subtypeBase;
    const int32_t subSlot1 = subtypeBase + (int32_t)sizeof(uintptr_t);

    if (clsName == "StructProperty") {
        p.structTarget = ReadPropertyPayloadPtr(propAddr, subSlot0);
        AddUniqueTypeTarget(p, p.structTarget);
        return;
    }
    if (clsName == "ByteProperty") {
        p.enumTarget = ReadPropertyPayloadPtr(propAddr, subSlot0);
        AddUniqueTypeTarget(p, p.enumTarget);
        return;
    }
    if (clsName == "EnumProperty") {
        p.enumTarget = ReadPropertyPayloadPtr(propAddr, subSlot1);
        AddUniqueTypeTarget(p, p.enumTarget);
        return;
    }

    auto collectInner = [&](uintptr_t inner) {
        if (inner == 0) return;
        std::string innerClass = legacyProperty
                ? ReadObjectClassName(inner)
                : ReadFieldClassName(inner, fieldClassCache);
        CollectPropertyTypeTargets(inner, innerClass, legacyProperty, p,
                                   fieldClassCache, depth + 1);
    };

    if (clsName == "ArrayProperty" || clsName == "SetProperty") {
        collectInner(ReadPropertyPayloadPtr(propAddr, subSlot0));
    } else if (clsName == "MapProperty") {
        collectInner(ReadPropertyPayloadPtr(propAddr, subSlot0));
        collectInner(ReadPropertyPayloadPtr(propAddr, subSlot1));
    }
}

// Dynamic sizes detected from reflection data for standard UE types.
// Used during header generation to ensure sizes match the active game engine.
INTERNAL struct DetectedSdkSizes {
    int32_t text = 0;
    int32_t softObjectPath = 0;
    int32_t softObjectPtr = 0;
    int32_t scriptDelegate = 0;
    int32_t multicastScriptDelegate = 0;
    int32_t multicastSparseDelegate = 0;
    int32_t set = 0;
    int32_t map = 0;
} g_detectedSizes;

INTERNAL void ResetDetectedSdkSizes() noexcept {
    g_detectedSizes = DetectedSdkSizes{};
}

INTERNAL void AcceptDetectedSize(int32_t& slot, int32_t value) noexcept {
    if (value > 0 && value <= 0x10000) slot = value;
}

INTERNAL std::vector<PropertyInfo> CollectStructProperties(
        uintptr_t structAddr,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::unordered_map<uintptr_t, std::string>& fieldClassCache) noexcept {
    std::vector<PropertyInfo> props;
    SafeMemory::ScopedSigSegvGuard guard;

    auto trySanitizeAndPush = [&](const std::string& propName,
                                  const std::string& clsName,
                                  uintptr_t propAddr,
                                  bool legacyProperty,
                                  PropertyInfo& p) {
        if (propName.empty() || p.elementSize <= 0 || clsName.empty()) return;
        if (!IsLikelyMemberName(propName)) return;
        if (clsName == "BoolProperty") {
            FillBoolMetadata(propAddr, legacyProperty, p);
        }
        if (p.offset < 0 || p.offset >= (1 << 24)) return;
        if (p.arrayDim < 1 || p.arrayDim > 1024) return;
        int64_t totalFieldSize = static_cast<int64_t>(p.elementSize) * p.arrayDim;
        if (totalFieldSize <= 0 || totalFieldSize > 0x100000) return;
        p.name     = SanitizeIdent(propName);
        CollectPropertyTypeTargets(propAddr, clsName, legacyProperty, p,
                                   fieldClassCache);
        p.typeName = ResolveTypeName(propAddr, clsName, byAddr,
                                     fieldClassCache, 0, legacyProperty);

        // Track the storage size of enums to align the generated enum's underlying type.
        if (p.enumTarget != 0 && p.elementSize > 0 && p.elementSize <= 8) {
            std::lock_guard<std::mutex> lock(g_enumSizeHintsMutex);
            auto it = g_enumSizeHints.find(p.enumTarget);
            if (it == g_enumSizeHints.end() || it->second > p.elementSize) {
                g_enumSizeHints[p.enumTarget] = p.elementSize;
            }
        }

        // Detect standard type sizes to emit correctly in Containers.hpp.
        if (clsName == "TextProperty") {
            AcceptDetectedSize(g_detectedSizes.text, p.elementSize);
        } else if (clsName == "SoftObjectProperty" ||
                   clsName == "SoftClassProperty") {
            AcceptDetectedSize(g_detectedSizes.softObjectPtr, p.elementSize);
        } else if (clsName == "DelegateProperty") {
            AcceptDetectedSize(g_detectedSizes.scriptDelegate, p.elementSize);
        } else if (clsName == "MulticastDelegateProperty" ||
                   clsName == "MulticastInlineDelegateProperty") {
            AcceptDetectedSize(g_detectedSizes.multicastScriptDelegate, p.elementSize);
        } else if (clsName == "MulticastSparseDelegateProperty") {
            AcceptDetectedSize(g_detectedSizes.multicastSparseDelegate, p.elementSize);
        } else if (clsName == "SetProperty") {
            AcceptDetectedSize(g_detectedSizes.set, p.elementSize);
        } else if (clsName == "MapProperty") {
            AcceptDetectedSize(g_detectedSizes.map, p.elementSize);
        }

        props.push_back(std::move(p));
    };

    // ---- Pass 1: ChildProperties (FField/FProperty — UE4.25+ new path) ----
    {
        uintptr_t cur = 0;
        guard.Try([&] {
            auto v = SafeMemory::SafeReadAny<uintptr_t>(
                    structAddr + Off::UStruct::ChildProperties);
            if (v) cur = *v;
        });

        int32_t safety = 0;
        while (cur && safety++ < 4096) {
            std::string clsName  = ReadFieldClassName(cur, fieldClassCache);
            std::string propName = ReadFieldName(cur);

            PropertyInfo p;
            guard.Try([&] {
                auto off = SafeMemory::SafeReadAny<int32_t>(cur + Off::FProperty::Offset_Internal);
                auto sz  = SafeMemory::SafeReadAny<int32_t>(cur + Off::FProperty::ElementSize);
                auto dim = SafeMemory::SafeReadAny<int32_t>(cur + Off::FProperty::ArrayDim);
                auto flg = SafeMemory::SafeReadAny<uint64_t>(cur + Off::FProperty::PropertyFlags);
                if (off) p.offset = *off;
                if (sz && *sz > 0 && *sz <= 0x10000) p.elementSize = *sz;
                if (dim && *dim >= 1 && *dim <= 1024) p.arrayDim = *dim;
                else p.arrayDim = 1;
                if (flg) p.propertyFlags = *flg;
            });
            trySanitizeAndPush(propName, clsName, cur, false, p);

            uintptr_t next = 0;
            guard.Try([&] {
                auto v = SafeMemory::SafeReadAny<uintptr_t>(cur + Off::FProperty::Next);
                if (v) next = *v;
            });
            cur = next;
        }
    }

    // ---- Pass 2: Children (UField/UProperty — legacy UE4 path) ----
    // Children also holds UFunctions + nested UStructs; filter to entries
    // whose class name ends with "Property".
    {
        uintptr_t cur = 0;
        guard.Try([&] {
            auto v = SafeMemory::SafeReadAny<uintptr_t>(
                    structAddr + Off::UStruct::Children);
            if (v) cur = *v;
        });

        int32_t safety = 0;
        while (cur && safety++ < 4096) {
            std::string clsName = ReadUObjectClassNameCached(cur, fieldClassCache);
            if (IsPropertyClassName(clsName)) {
                std::string propName;
                PropertyInfo p;
                guard.Try([&] {
                    auto nm  = SafeMemory::SafeReadAny<int32_t>(
                            cur + Off::UObject::NamePrivate);
                    if (nm) propName = UE::NameArray::GetName(*nm);
                    auto off = SafeMemory::SafeReadAny<int32_t>(cur + Off::UProperty::Offset_Internal);
                    auto sz  = SafeMemory::SafeReadAny<int32_t>(cur + Off::UProperty::ElementSize);
                    auto dim = SafeMemory::SafeReadAny<int32_t>(cur + Off::UProperty::ArrayDim);
                    auto flg = SafeMemory::SafeReadAny<uint64_t>(cur + Off::UProperty::PropertyFlags);
                    if (off) p.offset = *off;
                    if (sz && *sz > 0 && *sz <= 0x10000) p.elementSize = *sz;
                    if (dim && *dim >= 1 && *dim <= 1024) p.arrayDim = *dim;
                    else p.arrayDim = 1;
                    if (flg) p.propertyFlags = *flg;
                });
                trySanitizeAndPush(propName, clsName, cur, true, p);
            }

            uintptr_t next = 0;
            guard.Try([&] {
                auto v = SafeMemory::SafeReadAny<uintptr_t>(cur + Off::UField::Next);
                if (v) next = *v;
            });
            cur = next;
        }
    }

    std::sort(props.begin(), props.end(),
              [](const PropertyInfo& a, const PropertyInfo& b) {
                  if (a.offset != b.offset) return a.offset < b.offset;
                  if (a.isBitfield != b.isBitfield) return a.isBitfield;
                  if (a.isBitfield && b.isBitfield) {
                      int32_t abit = BitIndex(a.boolMask);
                      int32_t bbit = BitIndex(b.boolMask);
                      if (abit != bbit) return abit < bbit;
                  }
                  return a.elementSize < b.elementSize;
              });
    return props;
}

// Returns the expected C++ size for built-in UE types.
// If the game's actual element size differs from our expected size, the field is
// emitted as a raw byte array to maintain correct member alignment.

// Returns 0 for custom classes, structs, enums, and pointers, which are handled dynamically.
INTERNAL int32_t ExpectedCppSizeForType(const std::string& typeName) noexcept {
    if (typeName.empty()) return 0;

    // Strip leading "class " / "struct " keywords.
    auto strip = [](const std::string& s) -> std::string {
        if (s.rfind("class ", 0) == 0)  return s.substr(6);
        if (s.rfind("struct ", 0) == 0) return s.substr(7);
        return s;
    };
    std::string t = strip(typeName);

    // Primitives.
    if (t == "bool" || t == "char" || t == "int8_t" || t == "uint8_t" ||
        t == "int8" || t == "uint8") return 1;
    if (t == "int16_t" || t == "uint16_t" || t == "int16" || t == "uint16" ||
        t == "char16_t" || t == "TCHAR" || t == "wchar_t") return 2;
    if (t == "int32_t" || t == "uint32_t" || t == "int32" || t == "uint32" ||
        t == "float") return 4;
    if (t == "int64_t" || t == "uint64_t" || t == "int64" || t == "uint64" ||
        t == "double") return 8;

    // Pointers and TSubclassOf<T> (single pointer).
    if (!t.empty() && t.back() == '*') return 8;
    if (t.rfind("TSubclassOf<", 0) == 0) return 8;

    // Core containers and helper structs the dumper emits verbatim.
    if (t == "FName" || t == "FWeakObjectPtr") return 8;
    if (t.rfind("TWeakObjectPtr<", 0) == 0) return 8;
    if (t.rfind("TLazyObjectPtr<", 0) == 0) return 8;

    if (t == "FString" || t == "FScriptInterface") return 16;
    if (t == "FScriptDelegate") return g_detectedSizes.scriptDelegate;
    if (t == "FMulticastScriptDelegate") return g_detectedSizes.multicastScriptDelegate;
    if (t == "FMulticastSparseDelegate") return g_detectedSizes.multicastSparseDelegate;
    if (t.rfind("TArray<", 0) == 0) return 16;
    if (t.rfind("TScriptInterface<", 0) == 0) return 16;

    if (t == "FText") return g_detectedSizes.text;
    if (t == "FSoftObjectPath" || t == "FSoftClassPath") return g_detectedSizes.softObjectPath;

    // TSoftObjectPtr/TSoftClassPtr/FSoftObjectPtr — size varies too much
    // across UE versions to predict reliably. Use detected size.
    if (t == "FSoftObjectPtr") return g_detectedSizes.softObjectPtr;
    if (t.rfind("TSoftObjectPtr<", 0) == 0) return g_detectedSizes.softObjectPtr;
    if (t.rfind("TSoftClassPtr<", 0) == 0)  return g_detectedSizes.softObjectPtr;

    if (t.rfind("TSet<", 0) == 0) return g_detectedSizes.set;
    if (t.rfind("TMap<", 0) == 0) return g_detectedSizes.map;

    // User structs (F-prefixed), classes, enums (E-prefixed): size derived
    // from reflection at emit time. Return 0 so the caller uses the typed
    // emission path.
    return 0;
}

// Checks if the C++ type's size mismatches the game's reflected element size.
// If so, the field must be emitted as a byte array to preserve layout.
INTERNAL bool IsTypeSizeMismatch(const std::string& typeName,
                                 int32_t elementSize) noexcept {
    const int32_t expected = ExpectedCppSizeForType(typeName);
    return expected > 0 && elementSize > 0 && expected != elementSize;
}

// Generates struct/class fields with padding.
// Emits bool properties as bitfields to preserve layout and allow direct access.
INTERNAL void EmitFields(std::ofstream& f,
                         const std::vector<PropertyInfo>& props,
                         int32_t baseSize,
                         int32_t totalSize) noexcept {
    int32_t cursor = baseSize;
    std::unordered_map<std::string, int32_t> totalByName;
    std::unordered_map<std::string, int32_t> nextByName;
    totalByName.reserve(props.size());
    nextByName.reserve(props.size());
    for (const auto& p : props) {
        if (!p.name.empty()) ++totalByName[p.name];
    }

    auto emitName = [&](const PropertyInfo& p) -> std::string {
        std::string base = p.name.empty() ? "Field" : p.name;
        auto totalIt = totalByName.find(base);
        if (totalIt == totalByName.end() || totalIt->second <= 1) return base;
        int32_t next = ++nextByName[base];
        return base + "_" + std::to_string(next);
    };

    for (size_t i = 0; i < props.size();) {
        const auto& p = props[i];
        if (p.offset < 0 || p.elementSize <= 0 || p.arrayDim <= 0) {
            ++i;
            continue;
        }

        if (p.isBitfield) {
            const int32_t byteOffset = p.offset;
            if (totalSize > 0 && byteOffset >= totalSize) {
                ++i;
                continue;
            }

            if (byteOffset < cursor) {
                while (i < props.size() &&
                       props[i].isBitfield &&
                       props[i].offset == byteOffset) {
                    std::string name = emitName(props[i]);
                    f << "    // uint8_t " << name
                      << " : 1;  // bitfield/overlap @ 0x"
                      << std::hex << std::uppercase << props[i].offset
                      << " (mask=0x" << (int)props[i].boolMask << ")"
                      << std::dec << "\n";
                    ++i;
                }
                continue;
            }

            if (byteOffset > cursor) {
                f << "    uint8_t Pad_" << std::hex << std::uppercase << cursor
                  << "[0x" << (byteOffset - cursor) << "];" << std::dec << "\n";
                cursor = byteOffset;
            }

            while (i < props.size() &&
                   props[i].isBitfield &&
                   props[i].offset == byteOffset) {
                const auto& bp = props[i];
                std::string name = emitName(bp);
                f << "    uint8_t " << name
                  << " : 1;  // 0x" << std::hex << std::uppercase
                  << bp.offset << " (mask=0x" << (int)bp.boolMask
                  << ", size=0x1)" << std::dec << "\n";
                ++i;
            }
            cursor = byteOffset + 1;
            continue;
        }

        int64_t fieldSize64 = static_cast<int64_t>(p.elementSize) * p.arrayDim;
        if (fieldSize64 <= 0 || fieldSize64 > 0x100000) {
            ++i;
            continue;
        }
        if (totalSize > 0) {
            if (p.offset >= totalSize) {
                ++i;
                continue;
            }
            if (p.offset + fieldSize64 > static_cast<int64_t>(totalSize) + 0x100) {
                ++i;
                continue;
            }
        }
        int32_t fieldSize = static_cast<int32_t>(fieldSize64);

        if (p.offset < cursor) {
            // Overlap — typically a bitfield sibling. Emit as a comment so
            // the reader can see the field exists without breaking layout.
            std::string name = emitName(p);
            f << "    // " << p.typeName << " " << name
              << ";  // bitfield/overlap @ 0x" << std::hex << std::uppercase
              << p.offset << std::dec << "\n";
            ++i;
            continue;
        }
        if (p.offset > cursor) {
            f << "    uint8_t Pad_" << std::hex << std::uppercase << cursor
              << "[0x" << (p.offset - cursor) << "];" << std::dec << "\n";
            cursor = p.offset;
        }

        std::string name = emitName(p);
        const bool sizeMismatch =
                p.arrayDim <= 1 && IsTypeSizeMismatch(p.typeName, p.elementSize);
        // Property metadata: show replication/config/save flags as short tags
        std::string tags = PropertyFlagTags(p.propertyFlags);
        std::string tagStr = tags.empty() ? "" : " [" + tags + "]";
        if (sizeMismatch) {
            f << "    uint8_t " << name << "[0x"
              << std::hex << std::uppercase << p.elementSize << "];"
              << std::dec << "  // 0x" << std::hex << std::uppercase << p.offset
              << " (size=0x" << fieldSize << ", was " << p.typeName << ")"
              << tagStr << std::dec << "\n";
        } else if (p.arrayDim > 1) {
            f << "    " << p.typeName << " " << name << "[" << p.arrayDim
              << "];  // 0x" << std::hex << std::uppercase << p.offset
              << " (size=0x" << fieldSize << ")" << tagStr << std::dec << "\n";
        } else {
            f << "    " << p.typeName << " " << name
              << ";  // 0x" << std::hex << std::uppercase << p.offset
              << " (size=0x" << fieldSize << ")" << tagStr << std::dec << "\n";
        }
        cursor = p.offset + fieldSize;
        ++i;

    }

    if (totalSize > cursor) {
        f << "    uint8_t Pad_" << std::hex << std::uppercase << cursor
          << "[0x" << (totalSize - cursor) << "];  // tail" << std::dec << "\n";
    }
}

INTERNAL std::string SignatureType(const PropertyInfo& p, bool asReturn) noexcept {
    std::string type = p.typeName.empty() ? "void*" : p.typeName;
    if (asReturn) return type;

    // Match Dumper-7 convention:
    //   CPF_OutParm | CPF_ReferenceParm  -> emit as Type&    (by-ref out)
    //   CPF_OutParm alone                -> emit as Type*    (by-ptr out)
    //   otherwise                        -> by value (or Type* for fixed arrays)
    const bool isRef = (p.propertyFlags & CPF_ReferenceParm) != 0;
    const bool isOut = (p.propertyFlags & CPF_OutParm) != 0 || isRef;
    if (isOut) {
        const bool constQual = (p.propertyFlags & CPF_ConstParm) != 0 &&
                               (p.propertyFlags & CPF_OutParm) == 0;
        if (constQual) type = "const " + type;
        type += isRef ? "&" : "*";
        return type;
    }

    if (p.arrayDim > 1) type += "*";
    return type;
}

INTERNAL std::string UniqueParamName(const std::string& raw,
                                     std::unordered_set<std::string>& used,
                                     int32_t index) noexcept {
    std::string base = raw.empty() || raw == "_" || raw == "ReturnValue"
            ? "Param_" + std::to_string(index)
            : SanitizeIdent(raw);
    std::string out = base;
    int32_t suffix = 1;
    while (!used.insert(out).second) {
        out = base + "_" + std::to_string(suffix++);
    }
    return out;
}

INTERNAL FunctionInfo BuildFunctionInfo(
        uintptr_t fnAddr,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::unordered_map<uintptr_t, std::string>& fieldClassCache) noexcept {
    FunctionInfo fn;
    fn.returnType = "void";
    fn.rawName = ReadObjectName(fnAddr);
    fn.name = SanitizeIdent(fn.rawName);
    if (fn.name.empty() || fn.name == "_") fn.name = "UnnamedFunction";

    SafeMemory::ScopedSigSegvGuard guard;
    guard.Try([&] {
        auto flags = SafeMemory::SafeReadAny<uint32_t>(
                fnAddr + Off::UFunction::FunctionFlags);
        auto num = SafeMemory::SafeReadAny<uint8_t>(
                fnAddr + Off::UFunction::NumParms);
        auto sz = SafeMemory::SafeReadAny<int32_t>(
                fnAddr + Off::UStruct::PropertiesSize);
        auto func = SafeMemory::SafeReadAny<uintptr_t>(
                fnAddr + Off::UFunction::Func);
        if (flags) fn.functionFlags = *flags;
        if (num) fn.numParms = *num;
        if (sz && *sz >= 0 && *sz <= 0x10000) fn.paramsSize = *sz;
        if (func && *func != 0) fn.nativeFuncPtr = *func;
    });

    std::vector<PropertyInfo> props =
            CollectStructProperties(fnAddr, byAddr, fieldClassCache);

    int32_t paramIndex = 0;
    for (const auto& p : props) {
        if ((p.propertyFlags & CPF_Parm) == 0) continue;
        if ((p.propertyFlags & CPF_ReturnParm) != 0) {
            fn.returnType = SignatureType(p, true);
            fn.returnParam = p;
            fn.hasReturnParam = true;
            continue;
        }
        PropertyInfo param = p;
        fn.params.push_back(std::move(param));
        ++paramIndex;
    }

    if (fn.params.empty() && fn.returnType == "void" &&
        fn.numParms > 0 && fn.numParms <= 64 && !props.empty()) {
        std::unordered_set<std::string> used;
        int32_t taken = 0;
        for (const auto& p : props) {
            if (taken >= fn.numParms) break;
            if (p.name == "ReturnValue") {
                fn.returnType = SignatureType(p, true);
                fn.returnParam = p;
                fn.hasReturnParam = true;
            } else {
                PropertyInfo param = p;
                param.name = UniqueParamName(param.name, used, taken);
                fn.params.push_back(std::move(param));
            }
            ++taken;
        }
    } else {
        std::unordered_set<std::string> used;
        for (int32_t i = 0; i < (int32_t)fn.params.size(); ++i) {
            fn.params[i].name = UniqueParamName(fn.params[i].name, used, i);
        }
    }

    return fn;
}

INTERNAL std::vector<FunctionInfo> CollectStructFunctions(
        uintptr_t structAddr,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::unordered_map<uintptr_t, std::string>& fieldClassCache) noexcept {
    std::vector<FunctionInfo> funcs;
    SafeMemory::ScopedSigSegvGuard guard;

    uintptr_t cur = 0;
    guard.Try([&] {
        auto v = SafeMemory::SafeReadAny<uintptr_t>(
                structAddr + Off::UStruct::Children);
        if (v) cur = *v;
    });

    int32_t safety = 0;
    while (cur && safety++ < 4096) {
        std::string clsName = ReadUObjectClassNameCached(cur, fieldClassCache);
        if (clsName == "Function") {
            funcs.push_back(BuildFunctionInfo(cur, byAddr, fieldClassCache));
        }

        uintptr_t next = 0;
        guard.Try([&] {
            auto v = SafeMemory::SafeReadAny<uintptr_t>(cur + Off::UField::Next);
            if (v) next = *v;
        });
        cur = next;
    }
    return funcs;
}

INTERNAL void EmitFunctions(std::ofstream& f,
                            const std::vector<FunctionInfo>& funcs) noexcept {
    if (funcs.empty()) return;

    f << "\n    // Functions\n";
    for (const auto& fn : funcs) {
        f << "    ";
        if ((fn.functionFlags & FUNC_Static) != 0) f << "static ";
        f << fn.returnType << " " << fn.name << "(";
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i) f << ", ";
            const auto& p = fn.params[i];
            f << SignatureType(p, false) << " " << p.name;
        }
        f << ");";
        // Readable function flag tags
        std::string fnTags;
        if (fn.functionFlags & 0x00000400) fnTags += "Native,";
        if (fn.functionFlags & 0x00000040) fnTags += "Net,";
        if (fn.functionFlags & 0x00200000) fnTags += "NetServer,";
        if (fn.functionFlags & 0x01000000) fnTags += "NetClient,";
        if (fn.functionFlags & 0x04000000) fnTags += "BlueprintCallable,";
        if (fn.functionFlags & 0x08000000) fnTags += "BlueprintEvent,";
        if (fn.functionFlags & 0x10000000) fnTags += "BlueprintPure,";
        if (fn.functionFlags & 0x00000200) fnTags += "Exec,";
        if (!fnTags.empty()) fnTags.pop_back();
        f << "  //";
        if (!fnTags.empty()) f << " [" << fnTags << "]";
        if (fn.nativeFuncPtr != 0) {
            f << " native=0x" << std::hex << std::uppercase
              << (fn.nativeFuncPtr - Off::Resolved::LibBase) << std::dec;
        }
        f << "\n";
    }
}

// Emit one out-of-line function body that boxes its parameters into a flat
// byte buffer (matching UFunction's parameter frame layout), looks up the
// UFunction* via UClass::GetFunction (runtime FindFunction), then dispatches
// through ProcessEvent (whose vtable index is resolved at SDK init).

// Returns true if the given param was emitted as a byte array in the param
// struct because the generated C++ helper type size does not match the game's
// reported frame element size.
INTERNAL bool IsParamByteArray(const PropertyInfo& p) noexcept {
    return p.arrayDim <= 1 && IsTypeSizeMismatch(p.typeName, p.elementSize);
}

INTERNAL void EmitFunctionImpl(std::ofstream& f,
                               const std::string& className,
                               const FunctionInfo& fn) noexcept {
    if (fn.name.empty()) return;
    const bool isStatic = (fn.functionFlags & FUNC_Static) != 0;
    const bool hasFrame = !fn.params.empty() || fn.hasReturnParam;
    // Params structs are named without the leading "U"/"A" prefix.
    std::string paramsStructName = className;
    if (!paramsStructName.empty() && (paramsStructName[0] == 'U' || paramsStructName[0] == 'A')) {
        paramsStructName.erase(0, 1);
    }
    paramsStructName += "_" + fn.name;

    f << "// " << fn.rawName;
    f << "  // flags=0x" << std::hex << std::uppercase
      << fn.functionFlags << std::dec
      << " params=" << fn.numParms
      << " size=0x" << std::hex << std::uppercase
      << (fn.paramsSize > 0 ? fn.paramsSize : 0) << std::dec;
    // Show native function pointer if available (useful for direct hooks)
    if (fn.nativeFuncPtr != 0) {
        f << " native=0x" << std::hex << std::uppercase
          << (fn.nativeFuncPtr - Off::Resolved::LibBase) << std::dec;
    }
    f << "\n";

    f << fn.returnType << " " << className << "::" << fn.name << "(";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (i) f << ", ";
        const auto& p = fn.params[i];
        f << SignatureType(p, false) << " " << p.name;
    }
    f << ")\n{\n";

    f << "    static UFunction* Func = nullptr;\n";
    if (isStatic) {
        f << "    class UClass* Class = " << className << "::StaticClass();\n";
        f << "    if (!Class) {\n";
        if (fn.returnType != "void") f << "        return {};\n";
        else f << "        return;\n";
        f << "    }\n";
        f << "    class UObject* __CDO = Class->GetDefaultObject();\n";
        f << "    if (!__CDO) {\n";
        if (fn.returnType != "void") f << "        return {};\n";
        else f << "        return;\n";
        f << "    }\n";
        f << "    if (!Func) Func = Class->GetFunction(" << CppStringLiteral(fn.rawName) << ");\n";
        if (hasFrame) f << "    Params::" << paramsStructName << " Parms{};\n";
        for (const auto& p : fn.params) {
            if (p.name.empty()) continue;
            const bool isOutOnly =
                    (p.propertyFlags & CPF_OutParm) != 0 &&
                    (p.propertyFlags & CPF_ReferenceParm) == 0;
            if (isOutOnly) continue;
            if (p.arrayDim > 1) {
                f << "    if (" << p.name << ") std::memcpy(Parms." << p.name
                  << ", " << p.name << ", sizeof(Parms." << p.name << "));\n";
            } else if (IsParamByteArray(p)) {
                f << "    std::memcpy(Parms." << p.name << ", &" << p.name
                  << ", sizeof(Parms." << p.name << ") < sizeof(" << p.name
                  << ") ? sizeof(Parms." << p.name << ") : sizeof(" << p.name << "));\n";
            } else {
                f << "    Parms." << p.name << " = " << p.name << ";\n";
            }
        }
        f << "    auto Flgs = Func->FunctionFlags;\n";
        f << "    Func->FunctionFlags |= (EFunctionFlags)0x400;\n";
        // Direct native call when enabled and function has a native impl
        if (SDKDumper::g_useDirectCalls && fn.nativeFuncPtr != 0) {
            f << "    // Direct native call (offset 0x" << std::hex << std::uppercase
              << (fn.nativeFuncPtr - Off::Resolved::LibBase) << std::dec << ")\n";
            f << "    reinterpret_cast<void(*)(class UObject*, void*, void*)>(Func->Func)(__CDO, "
              << (hasFrame ? "&Parms" : "nullptr") << ", nullptr);\n";
        } else {
            f << "    __CDO->ProcessEvent(Func, " << (hasFrame ? "&Parms" : "nullptr") << ");\n";
        }
        f << "    Func->FunctionFlags = Flgs;\n";
        for (const auto& p : fn.params) {
            if (p.name.empty()) continue;
            if ((p.propertyFlags & CPF_OutParm) == 0) continue;
            if ((p.propertyFlags & CPF_ConstParm) != 0) continue;
            if (p.arrayDim > 1) {
                f << "    if (" << p.name << ") std::memcpy(" << p.name
                  << ", Parms." << p.name << ", sizeof(Parms." << p.name << "));\n";
            } else if (IsParamByteArray(p)) {
                const bool byRef = (p.propertyFlags & CPF_ReferenceParm) != 0;
                if (byRef) f << "    std::memcpy(&" << p.name << ", Parms." << p.name
                             << ", sizeof(" << p.name << ") < sizeof(Parms." << p.name
                             << ") ? sizeof(" << p.name << ") : sizeof(Parms." << p.name << "));\n";
                else f << "    if (" << p.name << ") std::memcpy(" << p.name
                       << ", Parms." << p.name << ", sizeof(*" << p.name
                       << ") < sizeof(Parms." << p.name << ") ? sizeof(*" << p.name
                       << ") : sizeof(Parms." << p.name << "));\n";
            } else {
                // Mirror SignatureType(): ReferenceParm -> Type& (write directly),
                // OutParm alone -> Type* (dereference to write).
                const bool byRef = (p.propertyFlags & CPF_ReferenceParm) != 0;
                if (byRef) f << "    " << p.name << " = Parms." << p.name << ";\n";
                else f << "    *" << p.name << " = Parms." << p.name << ";\n";
            }
        }
        if (fn.returnType != "void" && fn.hasReturnParam) {
            if (IsParamByteArray(fn.returnParam)) {
                f << "    " << fn.returnType << " __RetVal{};\n";
                f << "    std::memcpy(&__RetVal, Parms.ReturnValue, sizeof(__RetVal) < sizeof(Parms.ReturnValue) ? sizeof(__RetVal) : sizeof(Parms.ReturnValue));\n";
                f << "    return __RetVal;\n";
            } else {
                f << "    return Parms.ReturnValue;\n";
            }
        }
    } else {
        f << "    if (!Func) Func = Class->GetFunction(" << CppStringLiteral(fn.rawName) << ");\n";
        if (hasFrame) f << "    Params::" << paramsStructName << " Parms{};\n";
        for (const auto& p : fn.params) {
            if (p.name.empty()) continue;
            const bool isOutOnly =
                    (p.propertyFlags & CPF_OutParm) != 0 &&
                    (p.propertyFlags & CPF_ReferenceParm) == 0;
            if (isOutOnly) continue;
            if (p.arrayDim > 1) {
                f << "    if (" << p.name << ") std::memcpy(Parms." << p.name
                  << ", " << p.name << ", sizeof(Parms." << p.name << "));\n";
            } else if (IsParamByteArray(p)) {
                f << "    std::memcpy(Parms." << p.name << ", &" << p.name
                  << ", sizeof(Parms." << p.name << ") < sizeof(" << p.name
                  << ") ? sizeof(Parms." << p.name << ") : sizeof(" << p.name << "));\n";
            } else {
                f << "    Parms." << p.name << " = " << p.name << ";\n";
            }
        }
        f << "    auto Flgs = Func->FunctionFlags;\n";
        f << "    Func->FunctionFlags |= (EFunctionFlags)0x400;\n";
        // Direct native call when enabled and function has a native impl
        if (SDKDumper::g_useDirectCalls && fn.nativeFuncPtr != 0) {
            f << "    // Direct native call (offset 0x" << std::hex << std::uppercase
              << (fn.nativeFuncPtr - Off::Resolved::LibBase) << std::dec << ")\n";
            f << "    reinterpret_cast<void(*)(class UObject*, void*, void*)>(Func->Func)(this, "
              << (hasFrame ? "&Parms" : "nullptr") << ", nullptr);\n";
        } else {
            f << "    ProcessEvent(Func, " << (hasFrame ? "&Parms" : "nullptr") << ");\n";
        }
        f << "    Func->FunctionFlags = Flgs;\n";
        for (const auto& p : fn.params) {
            if (p.name.empty()) continue;
            if ((p.propertyFlags & CPF_OutParm) == 0) continue;
            if ((p.propertyFlags & CPF_ConstParm) != 0) continue;
            if (p.arrayDim > 1) {
                f << "    if (" << p.name << ") std::memcpy(" << p.name
                  << ", Parms." << p.name << ", sizeof(Parms." << p.name << "));\n";
            } else if (IsParamByteArray(p)) {
                const bool byRef = (p.propertyFlags & CPF_ReferenceParm) != 0;
                if (byRef) f << "    std::memcpy(&" << p.name << ", Parms." << p.name
                             << ", sizeof(" << p.name << ") < sizeof(Parms." << p.name
                             << ") ? sizeof(" << p.name << ") : sizeof(Parms." << p.name << "));\n";
                else f << "    if (" << p.name << ") std::memcpy(" << p.name
                       << ", Parms." << p.name << ", sizeof(*" << p.name
                       << ") < sizeof(Parms." << p.name << ") ? sizeof(*" << p.name
                       << ") : sizeof(Parms." << p.name << "));\n";
            } else {
                // Mirror SignatureType(): ReferenceParm -> Type& (write directly),
                // OutParm alone -> Type* (dereference to write).
                const bool byRef = (p.propertyFlags & CPF_ReferenceParm) != 0;
                if (byRef) f << "    " << p.name << " = Parms." << p.name << ";\n";
                else f << "    *" << p.name << " = Parms." << p.name << ";\n";
            }
        }
        if (fn.returnType != "void" && fn.hasReturnParam) {
            if (IsParamByteArray(fn.returnParam)) {
                f << "    " << fn.returnType << " __RetVal{};\n";
                f << "    std::memcpy(&__RetVal, Parms.ReturnValue, sizeof(__RetVal) < sizeof(Parms.ReturnValue) ? sizeof(__RetVal) : sizeof(Parms.ReturnValue));\n";
                f << "    return __RetVal;\n";
            } else {
                f << "    return Parms.ReturnValue;\n";
            }
        }
    }

    f << "}\n\n";
}

INTERNAL bool MakeDirRec(const std::string& path) noexcept {
    if (path.empty()) return false;
    size_t pos = (path[0] == '/') ? 1 : 0;
    while (pos < path.size()) {
        size_t next = path.find('/', pos);
        if (next == std::string::npos) next = path.size();
        std::string sub = path.substr(0, next);
        if (!sub.empty() && mkdir(sub.c_str(), 0755) != 0 && errno != EEXIST) {
            DLOGW("[sdk] mkdir(%s) errno=%d", sub.c_str(), errno);
        }
        pos = next + 1;
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

INTERNAL bool IsCppKeyword(const std::string& s) noexcept {
    static constexpr const char* kKeywords[] = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand",
        "bitor", "bool", "break", "case", "catch", "char", "char8_t",
        "char16_t", "char32_t", "class", "compl", "concept", "const",
        "consteval", "constexpr", "constinit", "const_cast", "continue",
        "co_await", "co_return", "co_yield", "decltype", "default",
        "delete", "do", "double", "dynamic_cast", "else", "enum",
        "explicit", "export", "extern", "false", "float", "for", "friend",
        "goto", "if", "inline", "int", "long", "mutable", "namespace",
        "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or",
        "or_eq", "private", "protected", "public", "register",
        "reinterpret_cast", "requires", "return", "short", "signed",
        "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true",
        "try", "typedef", "typeid", "typename", "union", "unsigned",
        "using", "virtual", "void", "volatile", "wchar_t", "while",
        "xor", "xor_eq"
    };
    for (const char* kw : kKeywords) {
        if (s == kw) return true;
    }
    return false;
}

INTERNAL std::string SanitizeIdent(const std::string& name) noexcept {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out += c;
        } else {
            out += '_';
        }
    }
    if (out.empty()) out = "_";
    if (out[0] >= '0' && out[0] <= '9') out = "_" + out;
    if (IsCppKeyword(out)) out += "_";
    return out;
}

INTERNAL bool IsMacroCollisionIdent(const std::string& s) noexcept {
    // Generated SDK headers are commonly included after hook/runtime headers.
    // Macro expansion happens before C++ parsing, so enum values like
    // RT_SUCCESS must not be emitted as bare identifiers when Dobby is present.
    static constexpr const char* kNames[] = {
        "RT_SUCCESS",
        "RT_FAILED",
    };
    for (const char* name : kNames) {
        if (s == name) return true;
    }
    return false;
}

INTERNAL std::string CppStringLiteral(const std::string& s) noexcept {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

INTERNAL std::string JoinPath(const std::string& a, const std::string& b) noexcept {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

INTERNAL uintptr_t CurrentLibBase() noexcept {
    if (Off::Resolved::LibBase != 0) return Off::Resolved::LibBase;
    return SafeMemory::LibBase();
}

INTERNAL uintptr_t OffsetFromLibBase(uintptr_t addr) noexcept {
    const uintptr_t base = CurrentLibBase();
    if (addr == 0 || base == 0 || addr < base) return 0;
    return addr - base;
}

INTERNAL std::string HexPtr(uintptr_t v) noexcept {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%lx", static_cast<unsigned long>(v));
    return std::string(b);
}

INTERNAL std::string EngineVersionString() noexcept {
    if (Off::Discovery::EngineVersion[0] != '\0') {
        return Off::Discovery::EngineVersion;
    }
    if (Off::InSDK::EngineMajor < 0 || Off::InSDK::EngineMinor < 0) {
        return "unknown";
    }
    char b[32];
    if (Off::InSDK::EnginePatch >= 0) {
        std::snprintf(b, sizeof(b), "%d.%d.%d",
                      Off::InSDK::EngineMajor,
                      Off::InSDK::EngineMinor,
                      Off::InSDK::EnginePatch);
    } else {
        std::snprintf(b, sizeof(b), "%d.%d.x",
                      Off::InSDK::EngineMajor,
                      Off::InSDK::EngineMinor);
    }
    return std::string(b);
}

INTERNAL void CopyDiscovery(char* dst, size_t dstSize, const char* src) noexcept {
    if (!dst || dstSize == 0) return;
    std::snprintf(dst, dstSize, "%s", src ? src : "");
}

INTERNAL UE4Info BuildCurrentUE4Info() noexcept {
    UE4Info info;
    info.base = CurrentLibBase();
    std::snprintf(info.apkPath, sizeof(info.apkPath), "%s", Off::Discovery::LibPath);
    return info;
}

INTERNAL void* FindExactSymbolAny(const UE4Info& info, const char* name,
                                  bool* fromDisk = nullptr) noexcept {
    if (fromDisk) *fromDisk = false;
    if (info.base == 0 || !name || !*name) return nullptr;
    void* sym = FindSymbolDynamic(info.base, name);
    if (!sym && info.apkPath[0] != '\0') {
        sym = FindSymbolDisk(info.apkPath, info.base, name);
        if (sym && fromDisk) *fromDisk = true;
    }
    return sym;
}

INTERNAL bool IsExecutableCodeAddress(uintptr_t addr, size_t size = sizeof(uint32_t)) noexcept {
    if (addr == 0) return false;
    if (size == 0) size = 1;
    uintptr_t end = addr + size;
    if (end < addr) return false;

    struct Ctx { uintptr_t addr; uintptr_t end; bool found; };
    Ctx ctx{ addr, end, false };
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        auto* c = static_cast<Ctx*>(data);
        for (uint16_t i = 0; i < info->dlpi_phnum; ++i) {
            const ElfW(Phdr)& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0) continue;
            uintptr_t segStart = info->dlpi_addr + ph.p_vaddr;
            uintptr_t segEnd   = segStart + ph.p_memsz;
            if (c->addr >= segStart && c->end <= segEnd) {
                c->found = true;
                return 1;
            }
        }
        return 0;
    }, &ctx);
    return ctx.found;
}

INTERNAL bool IsExecutableSymbolAddress(uintptr_t addr) noexcept {
    return IsExecutableCodeAddress(addr, sizeof(uint32_t));
}

template <class Fn>
INTERNAL void IterateDynsymSymbols(const UE4Info& info, Fn&& visit) noexcept {
    if (info.base == 0) return;
    SafeMemory::ScopedSigSegvGuard guard;
    guard.Try([&] {
        auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(info.base);
        auto* phdr = reinterpret_cast<Elf64_Phdr*>(info.base + ehdr->e_phoff);

        Elf64_Dyn*  dyn       = nullptr;
        Elf64_Sym*  dynsym    = nullptr;
        const char* dynstr    = nullptr;
        uint32_t*   gnuHash   = nullptr;
        uint32_t*   sysvHash  = nullptr;
        size_t      syment    = sizeof(Elf64_Sym);

        for (int i = 0; i < ehdr->e_phnum; ++i) {
            if (phdr[i].p_type == PT_DYNAMIC) {
                dyn = reinterpret_cast<Elf64_Dyn*>(info.base + phdr[i].p_vaddr);
                break;
            }
        }
        if (!dyn) return;

        for (Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; ++d) {
            switch (d->d_tag) {
                case DT_SYMTAB:   dynsym  = reinterpret_cast<Elf64_Sym*>(info.base + d->d_un.d_ptr); break;
                case DT_STRTAB:   dynstr  = reinterpret_cast<const char*>(info.base + d->d_un.d_ptr); break;
                case DT_SYMENT:   syment  = d->d_un.d_val; break;
                case DT_GNU_HASH: gnuHash = reinterpret_cast<uint32_t*>(info.base + d->d_un.d_ptr); break;
                case DT_HASH:     sysvHash = reinterpret_cast<uint32_t*>(info.base + d->d_un.d_ptr); break;
            }
        }
        if (!dynsym || !dynstr) return;

        size_t symCount = 0;
        if (gnuHash) {
            const uint32_t nbuckets  = gnuHash[0];
            const uint32_t symoffset = gnuHash[1];
            const uint32_t bloomSize = gnuHash[2];
            uint32_t* const buckets  = gnuHash + 4 + bloomSize * 2;
            uint32_t* const chains   = buckets + nbuckets;

            uint32_t maxSym = symoffset;
            for (uint32_t b = 0; b < nbuckets; ++b) {
                uint32_t idx = buckets[b];
                if (!idx) continue;
                while (!(chains[idx - symoffset] & 1)) ++idx;
                if (idx > maxSym) maxSym = idx;
            }
            symCount = static_cast<size_t>(maxSym) + 1;
        } else if (sysvHash) {
            symCount = sysvHash[1];  // nchain
        } else {
            return;
        }

        for (size_t i = 0; i < symCount; ++i) {
            auto* sym = reinterpret_cast<Elf64_Sym*>(
                    reinterpret_cast<uint8_t*>(dynsym) + i * syment);
            if (sym->st_value == 0 || sym->st_name == 0) continue;
            const char* name = dynstr + sym->st_name;
            visit(name, sym, info.base + sym->st_value);
        }
    });
}

template <class Fn>
INTERNAL void IterateDiskSymbols(const UE4Info& info, Fn&& visit) noexcept {
    if (info.base == 0 || info.apkPath[0] == '\0') return;

    FILE* f = std::fopen(info.apkPath, "rb");
    if (!f) return;

    auto closeFile = [&] { std::fclose(f); };

    Elf64_Ehdr ehdr{};
    if (std::fread(&ehdr, sizeof(ehdr), 1, f) != 1 ||
        std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr.e_shoff == 0 || ehdr.e_shnum == 0) {
        closeFile();
        return;
    }

    std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
    if (std::fseek(f, static_cast<long>(ehdr.e_shoff), SEEK_SET) != 0 ||
        std::fread(shdrs.data(), sizeof(Elf64_Shdr), shdrs.size(), f) != shdrs.size()) {
        closeFile();
        return;
    }

    for (const Elf64_Shdr& symSec : shdrs) {
        if (symSec.sh_type != SHT_SYMTAB && symSec.sh_type != SHT_DYNSYM) continue;
        if (symSec.sh_entsize == 0 || symSec.sh_size == 0) continue;
        if (symSec.sh_link >= shdrs.size()) continue;

        const Elf64_Shdr& strSec = shdrs[symSec.sh_link];
        if (strSec.sh_type != SHT_STRTAB || strSec.sh_size == 0) continue;

        const size_t symCount = static_cast<size_t>(symSec.sh_size / symSec.sh_entsize);
        std::vector<uint8_t> symBytes(static_cast<size_t>(symSec.sh_size));
        std::vector<char> strtab(static_cast<size_t>(strSec.sh_size));

        if (std::fseek(f, static_cast<long>(symSec.sh_offset), SEEK_SET) != 0 ||
            std::fread(symBytes.data(), 1, symBytes.size(), f) != symBytes.size()) {
            continue;
        }
        if (std::fseek(f, static_cast<long>(strSec.sh_offset), SEEK_SET) != 0 ||
            std::fread(strtab.data(), 1, strtab.size(), f) != strtab.size()) {
            continue;
        }

        const char* source = symSec.sh_type == SHT_DYNSYM ? ".dynsym" : ".symtab";
        for (size_t i = 0; i < symCount; ++i) {
            auto* sym = reinterpret_cast<const Elf64_Sym*>(
                    symBytes.data() + i * symSec.sh_entsize);
            if (sym->st_value == 0 || sym->st_name == 0) continue;
            if (sym->st_name >= strtab.size()) continue;
            const char* name = strtab.data() + sym->st_name;
            if (!name || !*name) continue;
            visit(name, sym, info.base + sym->st_value, source);
        }
    }

    closeFile();
}

INTERNAL bool ReadPointer(uintptr_t addr, uintptr_t& out) noexcept {
    out = 0;
    SafeMemory::ScopedSigSegvGuard guard;
    bool ok = false;
    guard.Try([&] {
        auto v = SafeMemory::SafeReadAny<uintptr_t>(addr);
        if (v) {
            out = *v;
            ok = true;
        }
    });
    return ok;
}

INTERNAL bool LooksLikeWorldObject(uintptr_t obj) noexcept {
    if (obj == 0) return false;
    std::string cls;
    std::string name;
    SafeMemory::ScopedSigSegvGuard guard;
    guard.Try([&] {
        cls = ReadObjectClassName(obj);
        name = ReadObjectName(obj);
    });
    return cls == "World" || cls == "UWorld" ||
           cls.find("World") != std::string::npos ||
           name.find("World") != std::string::npos;
}

INTERNAL bool GlobalStorageReadable(uintptr_t storage) noexcept {
    uintptr_t value = 0;
    return ReadPointer(storage, value);
}

INTERNAL bool ResolveGWorld(const UE4Info& info) noexcept {
    if (Off::Resolved::GWorld != 0) return true;

    constexpr const char* kExactNames[] = {
        "GWorld",
        "_Z6GWorld",
        "_ZL6GWorld",
        "UWorldProxy::GWorld",
        "_ZN11UWorldProxy6GWorldE",
    };

    for (const char* name : kExactNames) {
        bool fromDisk = false;
        void* sym = FindExactSymbolAny(info, name, &fromDisk);
        if (!sym) continue;
        uintptr_t addr = reinterpret_cast<uintptr_t>(sym);
        if (!GlobalStorageReadable(addr)) continue;

        Off::Resolved::GWorld = addr;
        CopyDiscovery(Off::Discovery::GWorldMethod,
                      sizeof(Off::Discovery::GWorldMethod), "symbol");
        char detail[192];
        std::snprintf(detail, sizeof(detail), "%s (%s)",
                      name, fromDisk ? "disk" : "dynsym");
        CopyDiscovery(Off::Discovery::GWorldDetail,
                      sizeof(Off::Discovery::GWorldDetail), detail);
        return true;
    }

    uintptr_t best = 0;
    std::string bestName;
    IterateDynsymSymbols(info, [&](const char* name, const Elf64_Sym* sym, uintptr_t addr) {
        if (best != 0 || !name || std::strstr(name, "GWorld") == nullptr) return;
        const unsigned char type = ELF64_ST_TYPE(sym->st_info);
        if (type != STT_OBJECT && type != STT_NOTYPE) return;
        uintptr_t world = 0;
        if (!ReadPointer(addr, world)) return;
        if (world != 0 && !LooksLikeWorldObject(world)) return;
        best = addr;
        bestName = name;
    });

    if (best != 0) {
        Off::Resolved::GWorld = best;
        CopyDiscovery(Off::Discovery::GWorldMethod,
                      sizeof(Off::Discovery::GWorldMethod), "dynsym-substring");
        CopyDiscovery(Off::Discovery::GWorldDetail,
                      sizeof(Off::Discovery::GWorldDetail), bestName.c_str());
        return true;
    }

    CopyDiscovery(Off::Discovery::GWorldMethod,
                  sizeof(Off::Discovery::GWorldMethod), "unresolved");
    CopyDiscovery(Off::Discovery::GWorldDetail,
                  sizeof(Off::Discovery::GWorldDetail), "no validated GWorld symbol");
    return false;
}

INTERNAL bool ResolveProcessEvent(const UE4Info& info) noexcept {
    if (Off::Resolved::ProcessEvent != 0) return true;

    constexpr const char* kExactNames[] = {
        "_ZN7UObject12ProcessEventEP9UFunctionPv",
        "_ZN7UObject12ProcessEventEP9UFunctionP11FFramePv",
        "UObject::ProcessEvent",
        "ProcessEvent",
    };

    for (const char* name : kExactNames) {
        bool fromDisk = false;
        void* sym = FindExactSymbolAny(info, name, &fromDisk);
        if (!sym) continue;
        uintptr_t addr = reinterpret_cast<uintptr_t>(sym);
        if (!IsExecutableSymbolAddress(addr)) {
            continue;
        }

        Off::Resolved::ProcessEvent = addr;
        CopyDiscovery(Off::Discovery::ProcessEventMethod,
                      sizeof(Off::Discovery::ProcessEventMethod), "symbol");
        char detail[192];
        std::snprintf(detail, sizeof(detail), "%s (%s)",
                      name, fromDisk ? "disk" : "dynsym");
        CopyDiscovery(Off::Discovery::ProcessEventDetail,
                      sizeof(Off::Discovery::ProcessEventDetail), detail);
        return true;
    }

    struct ProcessEventHit {
        uintptr_t addr = 0;
        int score = -1;
        std::string name;
        std::string source;
    };

    auto consider = [&](ProcessEventHit& bestHit,
                        const char* name,
                        const Elf64_Sym* sym,
                        uintptr_t addr,
                        const char* source) {
        if (!name || std::strstr(name, "ProcessEvent") == nullptr) return;
        const unsigned char type = ELF64_ST_TYPE(sym->st_info);
        if (type != STT_FUNC && type != STT_NOTYPE) return;
        if (!IsExecutableSymbolAddress(addr)) return;

        int score = 10;
        if (std::strstr(name, "_ZN7UObject12ProcessEvent") != nullptr) score += 120;
        if (std::strstr(name, "UObject") != nullptr ||
            std::strstr(name, "7UObject") != nullptr) score += 70;
        if (std::strcmp(name, "ProcessEvent") == 0) score += 40;
        if (std::strstr(name, "Default__") != nullptr) score -= 20;

        if (score > bestHit.score) {
            bestHit.addr = addr;
            bestHit.score = score;
            bestHit.name = name;
            bestHit.source = source ? source : "symbol";
        }
    };

    ProcessEventHit bestDyn;
    IterateDynsymSymbols(info, [&](const char* name, const Elf64_Sym* sym, uintptr_t addr) {
        consider(bestDyn, name, sym, addr, ".dynsym");
    });

    if (bestDyn.addr != 0) {
        Off::Resolved::ProcessEvent = bestDyn.addr;
        CopyDiscovery(Off::Discovery::ProcessEventMethod,
                      sizeof(Off::Discovery::ProcessEventMethod), "dynsym-substring");
        CopyDiscovery(Off::Discovery::ProcessEventDetail,
                      sizeof(Off::Discovery::ProcessEventDetail), bestDyn.name.c_str());
        return true;
    }

    ProcessEventHit bestDisk;
    IterateDiskSymbols(info, [&](const char* name,
                                 const Elf64_Sym* sym,
                                 uintptr_t addr,
                                 const char* source) {
        consider(bestDisk, name, sym, addr, source);
    });

    if (bestDisk.addr != 0) {
        Off::Resolved::ProcessEvent = bestDisk.addr;
        CopyDiscovery(Off::Discovery::ProcessEventMethod,
                      sizeof(Off::Discovery::ProcessEventMethod), "disk-substring");
        char detail[192];
        std::snprintf(detail, sizeof(detail), "%s (%s)",
                      bestDisk.name.c_str(), bestDisk.source.c_str());
        CopyDiscovery(Off::Discovery::ProcessEventDetail,
                      sizeof(Off::Discovery::ProcessEventDetail), detail);
        return true;
    }

    CopyDiscovery(Off::Discovery::ProcessEventMethod,
                  sizeof(Off::Discovery::ProcessEventMethod), "unresolved");
    CopyDiscovery(Off::Discovery::ProcessEventDetail,
                  sizeof(Off::Discovery::ProcessEventDetail), "no validated ProcessEvent symbol");
    return false;
}

INTERNAL bool DecodeArm64UnsignedLoad(uint32_t ins,
                                      uint32_t& rn,
                                      uint32_t& rt,
                                      uint32_t& offset) noexcept {
    // Decodes ARM64 LDR (unsigned immediate) instruction.
    if ((ins & 0x3B000000u) != 0x39000000u) return false;
    if ((ins & (1u << 22)) == 0) return false;  // store, not load
    const uint32_t size = (ins >> 30) & 0x3u;
    const uint32_t imm12 = (ins >> 10) & 0xFFFu;
    rn = (ins >> 5) & 0x1Fu;
    rt = ins & 0x1Fu;
    offset = imm12 << size;
    return true;
}

INTERNAL bool DecodeArm64DirectBranch(uintptr_t pc,
                                      uint32_t ins,
                                      uintptr_t& target) noexcept {
    // Decodes ARM64 unconditional branch (B). Skip branch-with-link (BL).
    if ((ins & 0xFC000000u) != 0x14000000u) return false;
    int32_t imm26 = static_cast<int32_t>(ins & 0x03FFFFFFu);
    if (imm26 & 0x02000000) imm26 |= static_cast<int32_t>(0xFC000000u);
    target = pc + static_cast<int64_t>(imm26) * 4;
    return target != pc;
}

INTERNAL uintptr_t ResolveArm64BranchThunk(uintptr_t fn) noexcept {
    uintptr_t cur = fn;
    SafeMemory::ScopedSigSegvGuard guard;
    for (int depth = 0; depth < 4; ++depth) {
        if (!IsExecutableCodeAddress(cur, sizeof(uint32_t))) break;
        uintptr_t next = 0;
        bool ok = false;
        guard.Try([&] {
            auto ins = SafeMemory::SafeReadAny<uint32_t>(cur);
            if (ins) ok = DecodeArm64DirectBranch(cur, *ins, next);
        });
        if (!ok || next == 0 || !IsExecutableCodeAddress(next, sizeof(uint32_t))) break;
        cur = next;
    }
    return cur;
}

INTERNAL bool DecodeArm64MovReg(uint32_t ins,
                                uint32_t& rd,
                                uint32_t& rm) noexcept {
    // MOV Xd, Xm / MOV Wd, Wm aliases: ORR dst, ZR, src.
    if ((ins & 0xFFE0FFE0u) != 0xAA0003E0u &&
        (ins & 0xFFE0FFE0u) != 0x2A0003E0u) {
        return false;
    }
    rd = ins & 0x1Fu;
    rm = (ins >> 16) & 0x1Fu;
    return true;
}

INTERNAL bool DecodeArm64TestBit(uint32_t ins,
                                 uint32_t& rt,
                                 uint32_t& bit) noexcept {
    // Decodes ARM64 TBZ/TBNZ instructions used to test FunctionFlags bits.
    if ((ins & 0x7E000000u) != 0x36000000u) return false;
    rt = ins & 0x1Fu;
    bit = ((ins >> 19) & 0x1Fu) | ((ins >> 26) & 0x20u);
    return true;
}

INTERNAL int32_t ScoreProcessEventCode(uintptr_t fn) noexcept {
    fn = ResolveArm64BranchThunk(fn);
    if (!IsExecutableCodeAddress(fn, sizeof(uint32_t))) return 0;

    bool aliasOfFunctionArg[32] = {};
    aliasOfFunctionArg[1] = true;  // x1 holds UFunction* in ProcessEvent
    bool functionFlagsReg[32] = {};

    bool sawFlags = false;
    bool sawFunc = false;
    bool sawNumParms = false;
    bool sawNativeFlagTest = false;
    bool sawOutParmsFlagTest = false;
    int32_t nearbyFunctionReads = 0;

    SafeMemory::ScopedSigSegvGuard guard;
    guard.Try([&] {
        constexpr int32_t kMaxInstructions = 512;
        for (int32_t i = 0; i < kMaxInstructions; ++i) {
            auto insOpt = SafeMemory::SafeReadAny<uint32_t>(
                    fn + static_cast<uintptr_t>(i) * sizeof(uint32_t));
            if (!insOpt) return;
            const uint32_t ins = *insOpt;

            // Stop scanning at RET instruction.
            if ((ins & 0xFFFFFC1Fu) == 0xD65F0000u) return;

            uint32_t rd = 0, rm = 0;
            if (DecodeArm64MovReg(ins, rd, rm)) {
                if (rd != 31) {
                    aliasOfFunctionArg[rd] = aliasOfFunctionArg[rm];
                    functionFlagsReg[rd] = functionFlagsReg[rm];
                }
                continue;
            }

            uint32_t testReg = 0, testBit = 0;
            if (DecodeArm64TestBit(ins, testReg, testBit)) {
                if (testReg < 32 && functionFlagsReg[testReg]) {
                    if (testBit == 10) sawNativeFlagTest = true;      // FUNC_Native
                    if (testBit == 22) sawOutParmsFlagTest = true;    // FUNC_HasOutParms
                }
                continue;
            }

            uint32_t rn = 0, rt = 0, off = 0;
            if (!DecodeArm64UnsignedLoad(ins, rn, rt, off)) continue;

            const bool fromFunctionArg = rn < 32 && aliasOfFunctionArg[rn];
            if (rt != rn && rt < 32) {
                aliasOfFunctionArg[rt] = false;
                functionFlagsReg[rt] = false;
            }
            if (!fromFunctionArg) continue;

            if (static_cast<int32_t>(off) == Off::UFunction::FunctionFlags) {
                sawFlags = true;
                if (rt < 32) functionFlagsReg[rt] = true;
            }
            if (static_cast<int32_t>(off) == Off::UFunction::Func) sawFunc = true;
            if (static_cast<int32_t>(off) == Off::UFunction::NumParms) sawNumParms = true;
            if (static_cast<int32_t>(off) >= Off::UFunction::FunctionFlags - 0x20 &&
                static_cast<int32_t>(off) <= Off::UFunction::Size + 0x40) {
                ++nearbyFunctionReads;
            }
        }
    });

    int32_t score = 0;
    if (sawFlags) score += 9;
    if (sawFunc) score += 10;
    if (sawNumParms) score += 4;
    if (sawNativeFlagTest) score += 9;
    if (sawOutParmsFlagTest) score += 9;
    score += std::min(nearbyFunctionReads, 8);
    return score;
}

INTERNAL std::vector<uintptr_t> CollectSampleVTables(int32_t maxObjects,
                                                     int32_t maxSamples) noexcept {
    std::vector<uintptr_t> out;
    std::unordered_set<uintptr_t> used;
    SafeMemory::ScopedSigSegvGuard guard;

    const int32_t n = std::min(UE::ObjectArray::Num(), maxObjects);
    out.reserve(static_cast<size_t>(std::min(n, maxSamples)));
    for (int32_t i = 0; i < n && static_cast<int32_t>(out.size()) < maxSamples; ++i) {
        uintptr_t obj = 0;
        guard.Try([&] {
            obj = reinterpret_cast<uintptr_t>(UE::ObjectArray::GetByIndex(i));
        });
        if (obj == 0) continue;

        uintptr_t vtable = 0;
        guard.Try([&] {
            auto v = SafeMemory::SafeReadAny<uintptr_t>(obj + Off::UObject::VTable);
            if (v) vtable = *v;
        });
        if (vtable == 0) continue;
        if (!SafeMemory::IsInLibUE4(vtable, sizeof(uintptr_t)) &&
            !SafeMemory::IsExecutable(vtable, sizeof(uintptr_t))) {
            continue;
        }
        if (used.insert(vtable).second) out.push_back(vtable);
    }
    return out;
}

INTERNAL bool ResolveProcessEventVTableByCodeScan(const std::vector<uintptr_t>& vtables) noexcept {
    if (vtables.empty()) return false;

    struct Hit {
        int32_t idx = -1;
        int32_t codeScore = 0;
        uintptr_t fn = 0;
        uintptr_t slot = 0;
    };

    Hit best;
    std::unordered_map<uintptr_t, int32_t> scoreCache;
    SafeMemory::ScopedSigSegvGuard guard;

    const int32_t tableCount = std::min<int32_t>(static_cast<int32_t>(vtables.size()), 16);
    constexpr int32_t kMaxVTableEntries = 0x120;

    for (int32_t tableIdx = 0; tableIdx < tableCount; ++tableIdx) {
        const uintptr_t vtable = vtables[static_cast<size_t>(tableIdx)];
        for (int32_t idx = 0; idx < kMaxVTableEntries; ++idx) {
            uintptr_t fn = 0;
            const uintptr_t slot = vtable + static_cast<uintptr_t>(idx) * sizeof(uintptr_t);
            guard.Try([&] {
                auto v = SafeMemory::SafeReadAny<uintptr_t>(slot);
                if (v) fn = *v;
            });
            if (fn == 0 || !IsExecutableCodeAddress(fn, sizeof(uint32_t))) continue;

            const uintptr_t resolvedFn = ResolveArm64BranchThunk(fn);
            if (!IsExecutableCodeAddress(resolvedFn, sizeof(uint32_t))) continue;

            auto [it, inserted] = scoreCache.emplace(resolvedFn, 0);
            if (inserted) it->second = ScoreProcessEventCode(resolvedFn);
            const int32_t score = it->second;
            if (score > best.codeScore) {
                best.idx = idx;
                best.codeScore = score;
                best.fn = resolvedFn;
                best.slot = slot;
            }
        }
    }

    if (best.idx < 0 || best.codeScore < 12) {
        DLOGW("[sdk][pe-vtable] pattern rejected best idx=0x%x score=%d fn=%s tables=%d",
              best.idx,
              best.codeScore,
              HexPtr(best.fn).c_str(),
              tableCount);
        return false;
    }

    Off::Resolved::ProcessEventVTableIndex = best.idx;
    Off::Resolved::ProcessEventVTableSlot = best.slot;
    Off::Resolved::ProcessEvent = best.fn;
    CopyDiscovery(Off::Discovery::ProcessEventMethod,
                  sizeof(Off::Discovery::ProcessEventMethod), "vtable-pattern");
    char detail[192];
    std::snprintf(detail, sizeof(detail),
                  "idx=0x%x fn=%s codeScore=%d tables=%d",
                  best.idx,
                  HexPtr(best.fn).c_str(),
                  best.codeScore,
                  tableCount);
    CopyDiscovery(Off::Discovery::ProcessEventDetail,
                  sizeof(Off::Discovery::ProcessEventDetail), detail);
    DLOGI("[sdk][pe-vtable] %s", detail);
    return true;
}

INTERNAL bool ResolveProcessEventVTableByHeuristic() noexcept {
    if (Off::Resolved::ProcessEventVTableIndex >= 0) return true;

    struct Candidate {
        int32_t idx = -1;
        int32_t validSlots = 0;
        int32_t commonCount = 0;
        int32_t codeScore = 0;
        int32_t finalScore = 0;
        uintptr_t fn = 0;
        uintptr_t slot = 0;
    };

    const std::vector<uintptr_t> vtables = CollectSampleVTables(4096, 512);
    DLOGI("[sdk][pe-vtable] sample vtables=%zu", vtables.size());
    if (vtables.empty()) {
        DLOGW("[sdk][pe-vtable] no sample vtables");
        return false;
    }

    if (ResolveProcessEventVTableByCodeScan(vtables)) return true;

    if (vtables.size() < 3) {
        DLOGW("[sdk][pe-vtable] not enough sample vtables for known-index fallback: %zu",
              vtables.size());
        return false;
    }

    Candidate best;
    SafeMemory::ScopedSigSegvGuard guard;

    // Scan typical UE vtable ranges for ProcessEvent (usually 0x43 to 0x45).
    constexpr int32_t kKnownMin = 0x43;
    constexpr int32_t kKnownMax = 0x45;
    constexpr int32_t kKnownCenter = 0x44;
    for (int32_t idx = 0x34; idx <= 0x58; ++idx) {
        std::unordered_map<uintptr_t, int32_t> freq;
        int32_t validSlots = 0;
        uintptr_t firstSlot = 0;

        for (uintptr_t vtable : vtables) {
            uintptr_t fn = 0;
            const uintptr_t slot = vtable + static_cast<uintptr_t>(idx) * sizeof(uintptr_t);
            guard.Try([&] {
                auto v = SafeMemory::SafeReadAny<uintptr_t>(slot);
                if (v) fn = *v;
            });
            if (fn == 0 || !IsExecutableCodeAddress(fn, sizeof(uint32_t))) continue;
            ++validSlots;
            ++freq[fn];
            if (firstSlot == 0) firstSlot = slot;
        }

        if (validSlots < static_cast<int32_t>(vtables.size() / 2)) continue;

        uintptr_t commonFn = 0;
        int32_t commonCount = 0;
        for (const auto& kv : freq) {
            if (kv.second > commonCount) {
                commonFn = kv.first;
                commonCount = kv.second;
            }
        }
        if (commonFn == 0) continue;

        const int32_t codeScore = ScoreProcessEventCode(commonFn);
        const int32_t dist = std::abs(idx - kKnownCenter);
        const int32_t knownBonus = std::max(0, 12 - dist);
        const int32_t finalScore =
                codeScore * 1000 +
                commonCount * 8 +
                validSlots * 3 +
                knownBonus;

        if (finalScore > best.finalScore) {
            best.idx = idx;
            best.validSlots = validSlots;
            best.commonCount = commonCount;
            best.codeScore = codeScore;
            best.finalScore = finalScore;
            best.fn = commonFn;
            best.slot = firstSlot;
        }
    }

    if (best.idx < 0) {
        DLOGW("[sdk][pe-vtable] no valid known-index candidate (vtables=%zu)",
              vtables.size());
        return false;
    }

    const bool strongCodeMatch = best.codeScore >= 10;
    const int32_t indexDistance = std::abs(best.idx - kKnownCenter);
    const bool indexInKnownRange = best.idx >= kKnownMin && best.idx <= kKnownMax;
    const bool knownIndexFallback = indexInKnownRange &&
            (vtables.size() < 12
                    ? (best.validSlots == static_cast<int32_t>(vtables.size()) &&
                       best.commonCount == static_cast<int32_t>(vtables.size()))
                    : (best.validSlots >= static_cast<int32_t>((vtables.size() * 3) / 4) &&
                       best.commonCount >= std::max<int32_t>(
                               6, static_cast<int32_t>(vtables.size() / 5))));
    if (!strongCodeMatch && !knownIndexFallback) {
        DLOGW("[sdk][pe-vtable] rejected best idx=0x%x codeScore=%d valid=%d/%zu common=%d",
              best.idx, best.codeScore, best.validSlots, vtables.size(), best.commonCount);
        return false;
    }

    Off::Resolved::ProcessEventVTableIndex = best.idx;
    Off::Resolved::ProcessEventVTableSlot = best.slot;
    Off::Resolved::ProcessEvent = best.fn;
    const char* method = strongCodeMatch ? "vtable-heuristic"
            : (best.idx == kKnownCenter ? "vtable-known-index"
                                        : "vtable-known-index-range");
    CopyDiscovery(Off::Discovery::ProcessEventMethod,
                  sizeof(Off::Discovery::ProcessEventMethod),
                  method);
    char detail[192];
    std::snprintf(detail, sizeof(detail),
                  "idx=0x%x fn=%s codeScore=%d valid=%d/%zu common=%d dist=%d",
                  best.idx,
                  HexPtr(best.fn).c_str(),
                  best.codeScore,
                  best.validSlots,
                  vtables.size(),
                  best.commonCount,
                  indexDistance);
    CopyDiscovery(Off::Discovery::ProcessEventDetail,
                  sizeof(Off::Discovery::ProcessEventDetail), detail);
    DLOGI("[sdk][pe-vtable] %s", detail);
    return true;
}

INTERNAL bool ResolveProcessEventVTableIndex() noexcept {
    if (Off::Resolved::ProcessEventVTableIndex >= 0) return true;

    if (Off::Resolved::ProcessEvent == 0) {
        return ResolveProcessEventVTableByHeuristic();
    }

    SafeMemory::ScopedSigSegvGuard guard;
    const uintptr_t target = Off::Resolved::ProcessEvent;
    constexpr int32_t kMaxObjects = 2048;
    constexpr int32_t kMaxVTableEntries = 256;

    const int32_t n = std::min(UE::ObjectArray::Num(), kMaxObjects);
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t obj = 0;
        guard.Try([&] {
            obj = reinterpret_cast<uintptr_t>(UE::ObjectArray::GetByIndex(i));
        });
        if (obj == 0) continue;

        uintptr_t vtable = 0;
        bool readVtable = false;
        guard.Try([&] {
            auto v = SafeMemory::SafeReadAny<uintptr_t>(obj + Off::UObject::VTable);
            if (v) {
                vtable = *v;
                readVtable = true;
            }
        });
        if (!readVtable || vtable == 0) continue;

        for (int32_t idx = 0; idx < kMaxVTableEntries; ++idx) {
            uintptr_t fn = 0;
            bool readFn = false;
            guard.Try([&] {
                auto v = SafeMemory::SafeReadAny<uintptr_t>(
                        vtable + static_cast<uintptr_t>(idx) * sizeof(uintptr_t));
                if (v) {
                    fn = *v;
                    readFn = true;
                }
            });
            if (!readFn) break;
            if (fn != target) continue;

            Off::Resolved::ProcessEventVTableIndex = idx;
            Off::Resolved::ProcessEventVTableSlot =
                    vtable + static_cast<uintptr_t>(idx) * sizeof(uintptr_t);
            return true;
        }
    }
    return ResolveProcessEventVTableByHeuristic();
}

INTERNAL void ResolveSdkExtraOffsets() noexcept {
    if (Off::Resolved::LibBase == 0) Off::Resolved::LibBase = SafeMemory::LibBase();
    UE4Info info = BuildCurrentUE4Info();
    if (info.base == 0) return;

    ResolveGWorld(info);
    ResolveProcessEvent(info);
    ResolveProcessEventVTableIndex();

    DLOGI("[sdk][extra] GWorld=%s off=%s method=%s",
          HexPtr(Off::Resolved::GWorld).c_str(),
          HexPtr(OffsetFromLibBase(Off::Resolved::GWorld)).c_str(),
          Off::Discovery::GWorldMethod);
    DLOGI("[sdk][extra] ProcessEvent=%s off=%s vtableIndex=%d method=%s",
          HexPtr(Off::Resolved::ProcessEvent).c_str(),
          HexPtr(OffsetFromLibBase(Off::Resolved::ProcessEvent)).c_str(),
          Off::Resolved::ProcessEventVTableIndex,
          Off::Discovery::ProcessEventMethod);

    // Auto-detect non-reflected ULevel::Actors offset by scanning UWorld for 
    // a ULevel, then scanning ULevel for a valid TArray<AActor*> pattern.
    if (Off::Resolved::GWorld != 0) {
        SafeMemory::ScopedSigSegvGuard guard;
        guard.Try([&] {
            // Read UWorld*
            auto worldPtr = SafeMemory::SafeReadAny<uintptr_t>(Off::Resolved::GWorld);
            if (!worldPtr || *worldPtr == 0) return;
            uintptr_t world = *worldPtr;

            // Scan UWorld for a pointer to ULevel.
            uintptr_t levelAddr = 0;
            for (int32_t off = 0x20; off < 0x200; off += 8) {
                auto candidate = SafeMemory::SafeReadAny<uintptr_t>(world + off);
                if (!candidate || *candidate == 0) continue;
                // Check if this pointer's class is "Level"
                uintptr_t cls = 0;
                auto clsPtr = SafeMemory::SafeReadAny<uintptr_t>(
                        *candidate + Off::UObject::ClassPrivate);
                if (!clsPtr || *clsPtr == 0) continue;
                cls = *clsPtr;
                auto comp = SafeMemory::SafeReadAny<int32_t>(cls + Off::UObject::NamePrivate);
                if (!comp) continue;
                std::string clsName = UE::NameArray::GetName(*comp);
                if (clsName == "Level") {
                    levelAddr = *candidate;
                    break;
                }
            }
            if (levelAddr == 0) return;

            // Scan ULevel memory for the TArray<AActor*> pattern.
            for (int32_t off = 0x80; off < 0x300; off += 8) {
                auto data = SafeMemory::SafeReadAny<uintptr_t>(levelAddr + off);
                auto num  = SafeMemory::SafeReadAny<int32_t>(levelAddr + off + 8);
                auto max  = SafeMemory::SafeReadAny<int32_t>(levelAddr + off + 12);
                if (!data || !num || !max) continue;
                if (*data == 0 || *num < 5 || *num > 50000) continue;
                if (*max < *num) continue;

                // Verify first element is a valid UObject.
                auto firstElem = SafeMemory::SafeReadAny<uintptr_t>(*data);
                if (!firstElem || *firstElem == 0) continue;
                auto vtable = SafeMemory::SafeReadAny<uintptr_t>(*firstElem);
                if (!vtable || *vtable == 0) continue;
                if (!SafeMemory::IsInLibUE4(*vtable, sizeof(uintptr_t))) continue;

                // Verify the object inherits from Actor.
                auto elemCls = SafeMemory::SafeReadAny<uintptr_t>(
                        *firstElem + Off::UObject::ClassPrivate);
                if (!elemCls || *elemCls == 0) continue;

                // Walk super chain looking for "Actor"
                uintptr_t cur = *elemCls;
                bool isActor = false;
                for (int depth = 0; depth < 32 && cur; ++depth) {
                    auto nameComp = SafeMemory::SafeReadAny<int32_t>(
                            cur + Off::UObject::NamePrivate);
                    if (nameComp) {
                        std::string n = UE::NameArray::GetName(*nameComp);
                        if (n == "Actor") { isActor = true; break; }
                    }
                    auto super = SafeMemory::SafeReadAny<uintptr_t>(
                            cur + Off::UStruct::SuperStruct);
                    cur = (super && *super) ? *super : 0;
                }
                if (!isActor) continue;

                // Found it!
                Off::AutoDetected::ULevelActors = off;
                DLOGI("[sdk][extra] ULevel::Actors auto-detected at offset 0x%x (num=%d)",
                      off, *num);
                return;
            }
            DLOGI("[sdk][extra] ULevel::Actors not found (level=0x%lx)",
                  (unsigned long)levelAddr);
        });
    }
}

struct EnumEntry {
    std::string name;
    int64_t     value = 0;
};

struct ArrayHeader {
    uintptr_t data = 0;
    int32_t   num  = 0;
    int32_t   max  = 0;
};

INTERNAL bool ReadArrayHeader(uintptr_t addr, ArrayHeader& out) noexcept {
    SafeMemory::ScopedSigSegvGuard guard;
    bool ok = false;
    guard.Try([&] {
        auto data = SafeMemory::SafeReadAny<uintptr_t>(addr);
        auto num  = SafeMemory::SafeReadAny<int32_t>(addr + 0x8);
        auto max  = SafeMemory::SafeReadAny<int32_t>(addr + 0xC);
        if (!data || !num || !max) return;
        out.data = *data;
        out.num = *num;
        out.max = *max;
        ok = true;
    });
    return ok;
}

INTERNAL bool IsPlausibleEnumRawName(const std::string& name) noexcept {
    if (name.empty() || name.size() > 256) return false;
    if (IsPropertyTypeName(name)) return false;
    if (name == "None" || name == "Package" || name == "Class" ||
        name == "ScriptStruct" || name == "Function") return false;

    bool sawAlpha = false;
    for (unsigned char c : name) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) sawAlpha = true;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '.') {
            continue;
        }
        return false;
    }
    return sawAlpha;
}

INTERNAL bool ReadEnumCandidate(uintptr_t arrayAddr,
                                int32_t entryStride,
                                int32_t valueOffset,
                                std::vector<EnumEntry>& out,
                                int32_t& score) noexcept {
    out.clear();
    score = 0;

    ArrayHeader h;
    if (!ReadArrayHeader(arrayAddr, h)) return false;
    if (h.num <= 0 || h.num > 4096) return false;
    if (h.max < h.num || h.max > 8192) return false;
    if (h.data == 0 || (h.data & 0x7) != 0) return false;

    SafeMemory::ScopedSigSegvGuard guard;
    std::unordered_set<std::string> distinctNames;
    std::unordered_set<int64_t> distinctValues;

    bool ok = false;
    guard.Try([&] {
        for (int32_t i = 0; i < h.num; ++i) {
            uintptr_t entry = h.data + static_cast<uintptr_t>(i) * entryStride;
            auto comp = SafeMemory::SafeReadAny<int32_t>(entry);
            auto val  = SafeMemory::SafeReadAny<int64_t>(entry + valueOffset);
            if (!comp || !val) return;
            if (*comp < 0 || *comp > 0x40000000) return;

            std::string raw = UE::NameArray::GetName(*comp);
            if (!IsPlausibleEnumRawName(raw)) return;

            EnumEntry e;
            e.name = std::move(raw);
            e.value = *val;
            out.push_back(std::move(e));
            distinctNames.insert(out.back().name);
            distinctValues.insert(out.back().value);
        }
        ok = true;
    });
    if (!ok || (int32_t)out.size() != h.num) {
        out.clear();
        return false;
    }

    score = h.num * 16 +
            static_cast<int32_t>(distinctNames.size()) * 4 +
            static_cast<int32_t>(distinctValues.size());
    return true;
}

INTERNAL std::vector<EnumEntry> ReadEnumEntries(uintptr_t enumAddr) noexcept {
    struct Candidate {
        int32_t arrayOff;
        int32_t stride;
        int32_t valueOff;
    };

    std::vector<Candidate> candidates;
    auto addCandidatesForOffset = [&](int32_t off) {
        candidates.push_back({ off, 0x10, 0x08 });  // FName(8) + int64
        candidates.push_back({ off, 0x18, 0x10 });  // case-preserving FName + pad + int64
        candidates.push_back({ off, 0x20, 0x10 });
    };

    addCandidatesForOffset(Off::UEnum::Names);
    for (int32_t off = Off::UObject::Size; off <= 0x90; off += 8) {
        if (off == Off::UEnum::Names) continue;
        addCandidatesForOffset(off);
    }

    std::vector<EnumEntry> best;
    int32_t bestScore = 0;
    int32_t bestOff = -1;
    int32_t bestStride = 0;

    for (const auto& c : candidates) {
        std::vector<EnumEntry> entries;
        int32_t score = 0;
        if (!ReadEnumCandidate(enumAddr + c.arrayOff, c.stride, c.valueOff,
                               entries, score)) {
            continue;
        }
        if (score > bestScore) {
            bestScore = score;
            best = std::move(entries);
            bestOff = c.arrayOff;
            bestStride = c.stride;
        }
    }

    static std::atomic<int> enumLayoutLogs{0};
    int logIndex = enumLayoutLogs.fetch_add(1, std::memory_order_relaxed);
    if (logIndex < 16 && !best.empty()) {
        DLOGI("[sdk][enum] UEnum::Names @ 0x%x stride=0x%x values=%zu",
              bestOff, bestStride, best.size());
    }
    return best;
}

INTERNAL std::string CleanEnumValueName(const std::string& enumName,
                                        const std::string& rawName,
                                        int32_t index) noexcept {
    std::string n = rawName;

    size_t scope = n.rfind("::");
    if (scope != std::string::npos) {
        n = n.substr(scope + 2);
    } else {
        size_t dot = n.rfind('.');
        if (dot != std::string::npos) n = n.substr(dot + 1);
    }

    std::string safeEnum = SanitizeIdent(enumName);
    if (n.size() > safeEnum.size() + 1 &&
        n.compare(0, safeEnum.size(), safeEnum) == 0 &&
        n[safeEnum.size()] == '_') {
        n = n.substr(safeEnum.size() + 1);
    }

    std::string out = SanitizeIdent(n);
    if (out == "_" || out.empty()) {
        out = "Value_" + std::to_string(index);
    }
    if (IsMacroCollisionIdent(out)) {
        out += "_";
    }
    return out;
}

// Picks the narrowest underlying integer type that fits all values, capped by sizeHint.
INTERNAL const char* EnumUnderlyingType(const std::vector<EnumEntry>& entries,
                                        int32_t sizeHint = 0) noexcept {
    bool fitsU8 = true;
    bool fitsU16 = true;
    bool fitsI32 = true;
    for (const auto& e : entries) {
        auto u = static_cast<uint64_t>(e.value);
        if (e.value < 0 || u > 0xFFull) fitsU8 = false;
        if (e.value < 0 || u > 0xFFFFull) fitsU16 = false;
        if (e.value < -2147483648LL || e.value > 2147483647LL) fitsI32 = false;
    }
    // Honor sizeHint storage cap; values are masked at emit time.
    if (sizeHint == 1) return "uint8_t";
    if (sizeHint == 2) return "uint16_t";
    if (sizeHint == 4) return "uint32_t";
    if (sizeHint == 8) return "uint64_t";
    if (fitsU8) return "uint8_t";
    if (fitsU16) return "uint16_t";
    if (fitsI32) return "int32_t";
    return "int64_t";
}

INTERNAL int32_t EnumSizeHintFor(uintptr_t enumAddr) noexcept {
    std::lock_guard<std::mutex> lock(g_enumSizeHintsMutex);
    auto it = g_enumSizeHints.find(enumAddr);
    return it == g_enumSizeHints.end() ? 0 : it->second;
}

INTERNAL int64_t MaskEnumValueToFit(int64_t value, int32_t sizeBytes) noexcept {
    switch (sizeBytes) {
        case 1: return static_cast<int64_t>(static_cast<uint8_t>(value & 0xFFll));
        case 2: return static_cast<int64_t>(static_cast<uint16_t>(value & 0xFFFFll));
        case 4: return static_cast<int64_t>(static_cast<uint32_t>(value & 0xFFFFFFFFll));
        default: return value;
    }
}

INTERNAL void EmitEnum(std::ofstream& f, const ObjMeta& meta) noexcept {
    std::vector<EnumEntry> entries = ReadEnumEntries(meta.address);
    const int32_t sizeHint = EnumSizeHintFor(meta.address);
    f << "enum class E" << SanitizeIdent(meta.name)
      << " : " << EnumUnderlyingType(entries, sizeHint) << " {\n";

    if (entries.empty()) {
        f << "    // values unavailable\n";
        f << "};\n\n";
        return;
    }

    std::unordered_set<std::string> used;
    for (int32_t i = 0; i < (int32_t)entries.size(); ++i) {
        std::string name = CleanEnumValueName(meta.name, entries[i].name, i);
        std::string unique = name;
        int suffix = 1;
        while (!used.insert(unique).second) {
            unique = name + "_" + std::to_string(suffix++);
        }
        // Mask the value to fit the sizeHint underlying type.
        int64_t value = entries[i].value;
        if (sizeHint > 0 && sizeHint < 8) {
            value = MaskEnumValueToFit(value, sizeHint);
        }
        f << "    " << unique << " = " << value << ",\n";
    }
    f << "};\n\n";
}

INTERNAL bool IsCoreUObjectClassName(const std::string& name) noexcept {
    return name == "Object" || name == "Field" || name == "Struct" ||
           name == "ScriptStruct" || name == "Class" || name == "Function" ||
           name == "Enum" || name == "Package";
}

INTERNAL bool IsPredefinedStructName(const std::string& name) noexcept {
    // Skip opaque/support structures defined in Containers.hpp.
    return name == "Text" ||
           name == "WeakObjectPtr" ||
           name == "ScriptInterface" ||
           name == "ScriptDelegate" ||
           name == "MulticastScriptDelegate" ||
           name == "MulticastSparseDelegate" ||
           name == "SoftObjectPtr";
}

INTERNAL bool WriteBasicHpp(const std::string& outDir) noexcept {
    std::ofstream f(outDir + "/Basic.hpp");
    if (!f) return false;
    f << "// Auto-generated by Dumper-7 [Android]. Core SDK support types.\n";
    f << "#pragma once\n\n";
    f << "#include <cstdint>\n";
    f << "#include <cstddef>\n";
    f << "#include <cstring>\n";
    f << "#include <stdexcept>\n";
    f << "#include <string>\n";
    f << "#include <type_traits>\n";
    f << "#include <vector>\n\n";
    f << "namespace SDK {\n\n";
    f << "using int8   = std::int8_t;\n";
    f << "using int16  = std::int16_t;\n";
    f << "using int32  = std::int32_t;\n";
    f << "using int64  = std::int64_t;\n";
    f << "using uint8  = std::uint8_t;\n";
    f << "using uint16 = std::uint16_t;\n";
    f << "using uint32 = std::uint32_t;\n";
    f << "using uint64 = std::uint64_t;\n";
    f << "using TCHAR  = char16_t;\n\n";
    f << "struct SDKOffsets {\n";
    f << "    static constexpr std::uintptr_t LibBase = "
      << HexPtr(CurrentLibBase()) << ";\n";
    f << "    static constexpr const char* EngineVersion = "
      << CppStringLiteral(EngineVersionString()) << ";\n";
    f << "    static constexpr int32 EngineMajor = "
      << Off::InSDK::EngineMajor << ";\n";
    f << "    static constexpr int32 EngineMinor = "
      << Off::InSDK::EngineMinor << ";\n";
    f << "    static constexpr int32 EnginePatch = "
      << Off::InSDK::EnginePatch << ";\n";
    f << "    static constexpr uint32 EngineChangelist = "
      << Off::InSDK::EngineChangelist << ";\n";
    f << "    static constexpr std::uintptr_t GObjects = "
      << HexPtr(OffsetFromLibBase(Off::Resolved::GObjects)) << ";\n";
    f << "    static constexpr std::uintptr_t GNames = "
      << HexPtr(OffsetFromLibBase(Off::Resolved::GNames)) << ";\n";
    f << "    static constexpr std::uintptr_t GWorld = "
      << HexPtr(OffsetFromLibBase(Off::Resolved::GWorld)) << ";\n";
    f << "    static constexpr std::uintptr_t ProcessEvent = "
      << HexPtr(OffsetFromLibBase(Off::Resolved::ProcessEvent)) << ";\n";
    f << "    static constexpr int32 ProcessEventVTableIndex = "
      << Off::Resolved::ProcessEventVTableIndex << ";\n";
    f << "    static constexpr std::uintptr_t ProcessEventVTableOffset = "
      << HexPtr(Off::Resolved::ProcessEventVTableIndex >= 0
              ? static_cast<uintptr_t>(Off::Resolved::ProcessEventVTableIndex) * sizeof(void*)
              : 0) << ";\n";
    f << "    static constexpr int32 FNamePoolBlocksOffset = "
      << Off::FNamePool::BlocksOffset << ";\n";
    f << "    static constexpr int32 FNamePoolStride = "
      << Off::FNamePool::Stride << ";\n";
    f << "    static constexpr uint32 FNamePoolBlockOffsetBits = "
      << Off::FNamePool::BlockOffsetBits << ";\n";
    f << "    static constexpr uint32 FNamePoolBlockOffsetMask = "
      << Off::FNamePool::BlockOffsetMask << ";\n";
    f << "    static constexpr bool FNamePoolCasePreserving = "
      << (Off::InSDK::bIsCasePreserving ? "true" : "false") << ";\n";
    f << "    static constexpr bool GNamesIsDoubleIndirect = "
      << (UE::NameArray::IsDoubleIndirect() ? "true" : "false") << ";\n";
    f << "    static constexpr int32 UObjectClassPrivate = "
      << Off::UObject::ClassPrivate << ";\n";
    f << "    static constexpr int32 UObjectOuterPrivate = "
      << Off::UObject::OuterPrivate << ";\n";
    f << "    static constexpr int32 UStructSuperStruct = "
      << Off::UStruct::SuperStruct << ";\n";
    f << "    static constexpr int32 UClassClassDefaultObject = "
      << Off::UClass::ClassDefaultObject << ";\n";
    // Auto-detected non-reflected offsets (-1 = not found)
    f << "    static constexpr int32 ULevelActors = "
      << Off::AutoDetected::ULevelActors << ";\n";
    f << "    static constexpr int32 GObjectsObjObjectsOffset = "
      << Off::InSDK::GObjectsObjObjectsOffset << ";\n";
    f << "};\n\n";
    f << "inline void* GNames = nullptr;\n";
    f << "inline void* GWorld = nullptr;\n";
    f << "using ProcessEventFn = void(*)(class UObject*, class UFunction*, void*);\n";
    f << "inline ProcessEventFn ProcessEvent = nullptr;\n";
    f << "inline int32 ProcessEventVTableIndex = SDKOffsets::ProcessEventVTableIndex;\n\n";
    f << "enum class EObjectFlags : uint32 {\n";
    f << "    NoFlags = 0x00000000,\n";
    f << "    Public = 0x00000001,\n";
    f << "    Standalone = 0x00000002,\n";
    f << "    MarkAsNative = 0x00000004,\n";
    f << "    Transactional = 0x00000008,\n";
    f << "    ClassDefaultObject = 0x00000010,\n";
    f << "    ArchetypeObject = 0x00000020,\n";
    f << "    Transient = 0x00000040,\n";
    f << "    MarkAsRootSet = 0x00000080,\n";
    f << "    NeedInitialization = 0x00000100,\n";
    f << "    NeedLoad = 0x00000200,\n";
    f << "    KeepForCooker = 0x00000400,\n";
    f << "    NeedPostLoad = 0x00001000,\n";
    f << "    NeedPostLoadSubobjects = 0x00002000,\n";
    f << "    NewerVersionExists = 0x00004000,\n";
    f << "    BeginDestroyed = 0x00008000,\n";
    f << "    FinishDestroyed = 0x00010000,\n";
    f << "    BeingRegenerated = 0x00020000,\n";
    f << "    DefaultSubObject = 0x00040000,\n";
    f << "    WasLoaded = 0x00080000,\n";
    f << "    TextExportTransient = 0x00100000,\n";
    f << "    LoadCompleted = 0x00200000,\n";
    f << "    InheritableComponentTemplate = 0x00400000,\n";
    f << "    DuplicateTransient = 0x00800000,\n";
    f << "    StrongRefOnFrame = 0x01000000,\n";
    f << "    NonPIEDuplicateTransient = 0x02000000,\n";
    f << "    Dynamic = 0x04000000,\n";
    f << "    WillBeLoaded = 0x08000000,\n";
    f << "};\n\n";
    f << "enum class EFunctionFlags : uint32 {\n";
    f << "    None = 0x00000000,\n";
    f << "    Final = 0x00000001,\n";
    f << "    RequiredAPI = 0x00000002,\n";
    f << "    BlueprintAuthorityOnly = 0x00000004,\n";
    f << "    BlueprintCosmetic = 0x00000008,\n";
    f << "    Net = 0x00000040,\n";
    f << "    NetReliable = 0x00000080,\n";
    f << "    Exec = 0x00000200,\n";
    f << "    Native = 0x00000400,\n";
    f << "    Event = 0x00000800,\n";
    f << "    Static = 0x00002000,\n";
    f << "    Public = 0x00020000,\n";
    f << "    Private = 0x00040000,\n";
    f << "    Protected = 0x00080000,\n";
    f << "    Delegate = 0x00100000,\n";
    f << "    NetServer = 0x00200000,\n";
    f << "    HasOutParms = 0x00400000,\n";
    f << "    HasDefaults = 0x00800000,\n";
    f << "    NetClient = 0x01000000,\n";
    f << "    BlueprintCallable = 0x04000000,\n";
    f << "    BlueprintEvent = 0x08000000,\n";
    f << "    BlueprintPure = 0x10000000,\n";
    f << "    Const = 0x40000000,\n";
    f << "    NetValidate = 0x80000000,\n";
    f << "};\n\n";

    // Bitmask operators for strongly-typed enum flags (enum class).
    f << "template<typename Enum>\n";
    f << "inline constexpr std::enable_if_t<std::is_enum<Enum>::value, Enum>\n";
    f << "operator|(Enum L, Enum R) {\n";
    f << "    using U = std::underlying_type_t<Enum>;\n";
    f << "    return static_cast<Enum>(static_cast<U>(L) | static_cast<U>(R));\n";
    f << "}\n";
    f << "template<typename Enum>\n";
    f << "inline constexpr std::enable_if_t<std::is_enum<Enum>::value, Enum&>\n";
    f << "operator|=(Enum& L, Enum R) {\n";
    f << "    L = (L | R);\n";
    f << "    return L;\n";
    f << "}\n\n";

    f << "template<typename Fn>\n";
    f << "inline Fn GetVFunction(const void* Instance, std::size_t Index) {\n";
    f << "    auto VTable = *reinterpret_cast<void***>(const_cast<void*>(Instance));\n";
    f << "    return reinterpret_cast<Fn>(VTable[Index]);\n";
    f << "}\n\n";
    f << "inline uint32 fnv_hash_runtime(const char* Str) {\n";
    f << "    static constexpr uint32 Prime = 16777619u;\n";
    f << "    uint32 Hash = 2166136261u;\n";
    f << "    do { Hash ^= static_cast<uint8>(*Str++); Hash *= Prime; } while (*(Str - 1) != 0);\n";
    f << "    return Hash;\n";
    f << "}\n\n";
    f << "class FUObjectItem {\n";
    f << "public:\n";
    f << "    class UObject* Object = nullptr;\n";
    f << "    int32 Flags = 0;\n";
    f << "    int32 ClusterRootIndex = 0;\n";
    f << "    int32 SerialNumber = 0;\n";
    if (Off::InSDK::FUObjectItemSize > 0x18) {
        f << "    uint8 Pad[0x" << std::hex << std::uppercase
          << (Off::InSDK::FUObjectItemSize - 0x14) << std::dec << "];\n";
    }
    f << "};\n";
    f << "static_assert(sizeof(FUObjectItem) == 0x" << std::hex << std::uppercase
      << Off::InSDK::FUObjectItemSize << std::dec
      << ", \"FUObjectItem size mismatch\");\n\n";
    f << "class FFixedUObjectArray {\n";
    f << "public:\n";
    f << "    FUObjectItem* Objects = nullptr;\n";
    f << "    int32 MaxElements = 0;\n";
    f << "    int32 NumElements = 0;\n\n";
    f << "    int32 Num() const { return NumElements; }\n";
    f << "    FUObjectItem* GetItemByIndex(int32 Index) const {\n";
    f << "        return (Objects && Index >= 0 && Index < NumElements) ? &Objects[Index] : nullptr;\n";
    f << "    }\n";
    f << "    class UObject* GetByIndex(int32 Index) const {\n";
    f << "        FUObjectItem* Item = GetItemByIndex(Index);\n";
    f << "        return Item ? Item->Object : nullptr;\n";
    f << "    }\n";
    f << "    struct Iterator {\n";
    f << "        const FFixedUObjectArray* Owner = nullptr;\n";
    f << "        int32 Index = 0;\n";
    f << "        class UObject* operator*() const { return Owner ? Owner->GetByIndex(Index) : nullptr; }\n";
    f << "        Iterator& operator++() { ++Index; return *this; }\n";
    f << "        bool operator!=(const Iterator& Other) const { return Owner != Other.Owner || Index != Other.Index; }\n";
    f << "    };\n";
    f << "    Iterator begin() const { return { this, 0 }; }\n";
    f << "    Iterator end() const { return { this, NumElements }; }\n";
    f << "};\n\n";
    f << "class FChunkedFixedUObjectArray {\n";
    f << "public:\n";
    f << "    FUObjectItem** Objects = nullptr;\n";
    f << "    FUObjectItem* PreAllocatedObjects = nullptr;\n";
    f << "    int32 MaxElements = 0;\n";
    f << "    int32 NumElements = 0;\n";
    f << "    int32 MaxChunks = 0;\n";
    f << "    int32 NumChunks = 0;\n\n";
    f << "    static constexpr int32 ElementsPerChunk = "
      << Off::FChunkedFixedUObjectArray::ElementsPerChunk << ";\n";
    f << "    int32 Num() const { return NumElements; }\n";
    f << "    FUObjectItem* GetItemByIndex(int32 Index) const {\n";
    f << "        if (!Objects || Index < 0 || Index >= NumElements) return nullptr;\n";
    f << "        const int32 ChunkIndex = Index / ElementsPerChunk;\n";
    f << "        const int32 WithinChunkIndex = Index % ElementsPerChunk;\n";
    f << "        FUObjectItem* Chunk = Objects[ChunkIndex];\n";
    f << "        return Chunk ? &Chunk[WithinChunkIndex] : nullptr;\n";
    f << "    }\n";
    f << "    class UObject* GetByIndex(int32 Index) const {\n";
    f << "        FUObjectItem* Item = GetItemByIndex(Index);\n";
    f << "        return Item ? Item->Object : nullptr;\n";
    f << "    }\n";
    f << "    struct Iterator {\n";
    f << "        const FChunkedFixedUObjectArray* Owner = nullptr;\n";
    f << "        int32 Index = 0;\n";
    f << "        class UObject* operator*() const { return Owner ? Owner->GetByIndex(Index) : nullptr; }\n";
    f << "        Iterator& operator++() { ++Index; return *this; }\n";
    f << "        bool operator!=(const Iterator& Other) const { return Owner != Other.Owner || Index != Other.Index; }\n";
    f << "    };\n";
    f << "    Iterator begin() const { return { this, 0 }; }\n";
    f << "    Iterator end() const { return { this, NumElements }; }\n";
    f << "};\n\n";

    const bool isInner = (Off::InSDK::GObjectsObjObjectsOffset == 0);
    const char* innerType = Off::InSDK::bIsChunked
        ? "FChunkedFixedUObjectArray" : "FFixedUObjectArray";

    if (!isInner) {
        // GObjects points to the full FUObjectArray (with header before ObjObjects)
        f << "class FUObjectArray {\n";
        f << "public:\n";
        f << "    uint8 Header[0x" << std::hex << std::uppercase
          << Off::InSDK::GObjectsObjObjectsOffset << std::dec << "] = {};\n";
        f << "    " << innerType << " ObjObjects;\n";
        f << "    int32 Num() const { return ObjObjects.Num(); }\n";
        f << "    class UObject* GetByIndex(int32 Index) const { return ObjObjects.GetByIndex(Index); }\n";
        f << "    auto begin() const { return ObjObjects.begin(); }\n";
        f << "    auto end() const { return ObjObjects.end(); }\n";
        f << "};\n\n";
    } else {
        // GObjects points directly to the inner array (no FUObjectArray header)
        f << "class FUObjectArray : public " << innerType << " {};\n\n";
    }
    f << "inline FUObjectArray* GObjects = nullptr;\n\n";

    f << "struct FName {\n";
    f << "    int32 ComparisonIndex = 0;\n";
    f << "    int32 Number = 0;\n\n";
    f << "    constexpr FName() = default;\n";
    f << "    constexpr FName(int32 InComparisonIndex, int32 InNumber = 0)\n";
    f << "        : ComparisonIndex(InComparisonIndex), Number(InNumber) {}\n";
    f << "    FName(const char* InName) { *this = FindName(InName); }\n";
    f << "    FName(const std::string& InName) { *this = FindName(InName.c_str()); }\n";
    f << "    constexpr bool IsNone() const { return ComparisonIndex == 0 && Number == 0; }\n";
    f << "    constexpr bool operator==(const FName& Other) const {\n";
    f << "        return ComparisonIndex == Other.ComparisonIndex && Number == Other.Number;\n";
    f << "    }\n";
    f << "    constexpr bool operator!=(const FName& Other) const { return !(*this == Other); }\n";
    f << "    inline static void*& Names = SDK::GNames;\n";
    f << "    static void Init(void* InGNames) { SDK::GNames = InGNames; }\n";
    f << "    static void** GetBlocksArray() {\n";
    f << "        if (!SDK::GNames) return nullptr;\n";
    f << "        auto Base = reinterpret_cast<std::uintptr_t>(SDK::GNames);\n";
    f << "        if constexpr (SDKOffsets::GNamesIsDoubleIndirect) {\n";
    f << "            return *reinterpret_cast<void***>(Base);\n";
    f << "        } else {\n";
    f << "            return reinterpret_cast<void**>(Base + SDKOffsets::FNamePoolBlocksOffset);\n";
    f << "        }\n";
    f << "    }\n";
    f << "    static std::string DecodeEntry(std::uintptr_t Entry) {\n";
    f << "        if (!Entry) return {};\n";
    f << "        const uint16 Header = *reinterpret_cast<const uint16*>(Entry);\n";
    f << "        const bool bWide = (Header & 0x1) != 0;\n";
    f << "        const int32 Len = SDKOffsets::FNamePoolCasePreserving\n";
    f << "            ? static_cast<int32>((Header >> 1) & 0x7FFF)\n";
    f << "            : static_cast<int32>((Header >> 6) & 0x3FF);\n";
    f << "        if (Len <= 0 || Len > 1023) return {};\n";
    f << "        std::string Out;\n";
    f << "        Out.resize(static_cast<std::size_t>(Len));\n";
    f << "        if (!bWide) {\n";
    f << "            const char* Str = reinterpret_cast<const char*>(Entry + 2);\n";
    f << "            std::memcpy(Out.data(), Str, static_cast<std::size_t>(Len));\n";
    f << "            return Out;\n";
    f << "        }\n";
    f << "        std::string Wide2 = DecodeWideEntry(Entry, Len, 2);\n";
    f << "        if (WideDecodeScore(Wide2) >= Len) return Wide2;\n";
    f << "        std::string Wide4 = DecodeWideEntry(Entry, Len, 4);\n";
    f << "        return WideDecodeScore(Wide4) > WideDecodeScore(Wide2) ? Wide4 : Wide2;\n";
    f << "    }\n";
    f << "    static std::string DecodeWideEntry(std::uintptr_t Entry, int32 Len, int32 Stride) {\n";
    f << "        std::string Out;\n";
    f << "        Out.resize(static_cast<std::size_t>(Len));\n";
    f << "        for (int32 i = 0; i < Len; ++i) {\n";
    f << "            uint32 Code = 0;\n";
    f << "            if (Stride == 2) {\n";
    f << "                uint16 Unit = 0;\n";
    f << "                std::memcpy(&Unit, reinterpret_cast<const void*>(Entry + 2 + static_cast<std::uintptr_t>(i) * 2), sizeof(Unit));\n";
    f << "                Code = Unit;\n";
    f << "            } else {\n";
    f << "                uint32 Unit = 0;\n";
    f << "                std::memcpy(&Unit, reinterpret_cast<const void*>(Entry + 2 + static_cast<std::uintptr_t>(i) * 4), sizeof(Unit));\n";
    f << "                Code = Unit;\n";
    f << "            }\n";
    f << "            if (Code == 0) return {};\n";
    f << "            Out[static_cast<std::size_t>(i)] = Code < 0x80 ? static_cast<char>(Code) : '?';\n";
    f << "        }\n";
    f << "        return Out;\n";
    f << "    }\n";
    f << "    static int32 WideDecodeScore(const std::string& S) {\n";
    f << "        if (S.empty()) return -100000;\n";
    f << "        int32 Score = 0;\n";
    f << "        for (unsigned char C : S) {\n";
    f << "            if (C == 0) return -100000;\n";
    f << "            if ((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||\n";
    f << "                (C >= '0' && C <= '9') || C == '_' || C == '/' ||\n";
    f << "                C == '.' || C == ':' || C == '-') Score += 2;\n";
    f << "            else if (C >= 0x20 && C < 0x7F) Score += 1;\n";
    f << "            else if (C == '?') Score += 0;\n";
    f << "            else Score -= 2;\n";
    f << "        }\n";
    f << "        return Score;\n";
    f << "    }\n";
    f << "    static std::string GetNameByIndex(int32 Index) {\n";
    f << "        void** Blocks = GetBlocksArray();\n";
    f << "        if (!Blocks || Index < 0) return {};\n";
    f << "        const uint32 Block = static_cast<uint32>(Index) >> SDKOffsets::FNamePoolBlockOffsetBits;\n";
    f << "        const uint32 Offset = static_cast<uint32>(Index) & SDKOffsets::FNamePoolBlockOffsetMask;\n";
    f << "        void* BlockPtr = Blocks[Block];\n";
    f << "        if (!BlockPtr) return {};\n";
    f << "        const std::uintptr_t Entry = reinterpret_cast<std::uintptr_t>(BlockPtr) +\n";
    f << "            static_cast<std::uintptr_t>(Offset) * SDKOffsets::FNamePoolStride;\n";
    f << "        return DecodeEntry(Entry);\n";
    f << "    }\n";
    f << "    static bool ReadEntryHeader(std::uintptr_t Entry, bool& bWide, int32& Len) {\n";
    f << "        if (!Entry) return false;\n";
    f << "        const uint16 Header = *reinterpret_cast<const uint16*>(Entry);\n";
    f << "        bWide = (Header & 0x1) != 0;\n";
    f << "        Len = SDKOffsets::FNamePoolCasePreserving\n";
    f << "            ? static_cast<int32>((Header >> 1) & 0x7FFF)\n";
    f << "            : static_cast<int32>((Header >> 6) & 0x3FF);\n";
    f << "        return Len > 0 && Len <= 1023;\n";
    f << "    }\n";
    f << "    static int32 WideStrideForEntry(std::uintptr_t Entry, int32 Len) {\n";
    f << "        std::string Wide2 = DecodeWideEntry(Entry, Len, 2);\n";
    f << "        if (WideDecodeScore(Wide2) >= Len) return 2;\n";
    f << "        std::string Wide4 = DecodeWideEntry(Entry, Len, 4);\n";
    f << "        return WideDecodeScore(Wide4) > WideDecodeScore(Wide2) ? 4 : 2;\n";
    f << "    }\n";
    f << "    static int32 EntrySizeBytes(std::uintptr_t Entry) {\n";
    f << "        bool bWide = false;\n";
    f << "        int32 Len = 0;\n";
    f << "        if (!ReadEntryHeader(Entry, bWide, Len)) return 0;\n";
    f << "        int32 CharStride = bWide ? WideStrideForEntry(Entry, Len) : 1;\n";
    f << "        int32 Size = 2 + Len * CharStride;\n";
    f << "        const int32 Align = SDKOffsets::FNamePoolStride;\n";
    f << "        return (Size + Align - 1) & ~(Align - 1);\n";
    f << "    }\n";
    f << "    static int32 FindNameIndex(const char* InName) {\n";
    f << "        if (!InName || !*InName) return 0;\n";
    f << "        void** Blocks = GetBlocksArray();\n";
    f << "        if (!Blocks) return 0;\n";
    f << "        for (uint32 Block = 0; Block < 8192; ++Block) {\n";
    f << "            void* BlockPtr = Blocks[Block];\n";
    f << "            if (!BlockPtr) {\n";
    f << "                if (Block > 0) break;\n";
    f << "                continue;\n";
    f << "            }\n";
    f << "            uint32 Offset = 0;\n";
    f << "            while (Offset < (1u << SDKOffsets::FNamePoolBlockOffsetBits)) {\n";
    f << "                std::uintptr_t Entry = reinterpret_cast<std::uintptr_t>(BlockPtr) +\n";
    f << "                    static_cast<std::uintptr_t>(Offset) * SDKOffsets::FNamePoolStride;\n";
    f << "                int32 SizeBytes = EntrySizeBytes(Entry);\n";
    f << "                if (SizeBytes <= 0) break;\n";
    f << "                std::string Candidate = DecodeEntry(Entry);\n";
    f << "                if (Candidate == InName) {\n";
    f << "                    return static_cast<int32>((Block << SDKOffsets::FNamePoolBlockOffsetBits) | Offset);\n";
    f << "                }\n";
    f << "                Offset += static_cast<uint32>(SizeBytes / SDKOffsets::FNamePoolStride);\n";
    f << "            }\n";
    f << "        }\n";
    f << "        return 0;\n";
    f << "    }\n";
    f << "    static FName FindName(const char* InName) {\n";
    f << "        if (!InName || !*InName) return FName();\n";
    f << "        int32 Exact = FindNameIndex(InName);\n";
    f << "        if (Exact != 0 || std::strcmp(InName, \"None\") == 0) return FName(Exact, 0);\n";
    f << "        const char* LastUnderscore = nullptr;\n";
    f << "        for (const char* P = InName; *P; ++P) if (*P == '_') LastUnderscore = P;\n";
    f << "        if (!LastUnderscore || !LastUnderscore[1]) return FName();\n";
    f << "        int32 ParsedNumber = 0;\n";
    f << "        for (const char* P = LastUnderscore + 1; *P; ++P) {\n";
    f << "            if (*P < '0' || *P > '9') return FName();\n";
    f << "            ParsedNumber = ParsedNumber * 10 + (*P - '0');\n";
    f << "        }\n";
    f << "        std::string Base(InName, static_cast<std::size_t>(LastUnderscore - InName));\n";
    f << "        int32 BaseIndex = FindNameIndex(Base.c_str());\n";
    f << "        return BaseIndex ? FName(BaseIndex, ParsedNumber + 1) : FName();\n";
    f << "    }\n";
    f << "    std::string ToString() const {\n";
    f << "        std::string Out = GetNameByIndex(ComparisonIndex);\n";
    f << "        if (Number > 0) {\n";
    f << "            Out += \"_\";\n";
    f << "            Out += std::to_string(Number - 1);\n";
    f << "        }\n";
    f << "        return Out;\n";
    f << "    }\n";
    f << "};\n";
    f << "static_assert(sizeof(FName) == 0x8, \"FName layout must stay 8 bytes\");\n\n";
    f << "inline void InitSDKFromAbsolute(std::uintptr_t InGObjects,\n";
    f << "                                std::uintptr_t InGNames,\n";
    f << "                                std::uintptr_t InGWorld = 0,\n";
    f << "                                std::uintptr_t InProcessEvent = 0) {\n";
    f << "    SDK::GObjects = reinterpret_cast<class FUObjectArray*>(InGObjects);\n";
    f << "    FName::Init(reinterpret_cast<void*>(InGNames));\n";
    f << "    SDK::GWorld = reinterpret_cast<void*>(InGWorld);\n";
    f << "    SDK::ProcessEvent = reinterpret_cast<ProcessEventFn>(InProcessEvent);\n";
    f << "}\n\n";
    f << "inline void InitSDK(std::uintptr_t LibBase) {\n";
    f << "    InitSDKFromAbsolute(\n";
    f << "        SDKOffsets::GObjects ? LibBase + SDKOffsets::GObjects : 0,\n";
    f << "        SDKOffsets::GNames ? LibBase + SDKOffsets::GNames : 0,\n";
    f << "        SDKOffsets::GWorld ? LibBase + SDKOffsets::GWorld : 0,\n";
    f << "        SDKOffsets::ProcessEvent ? LibBase + SDKOffsets::ProcessEvent : 0);\n";
    f << "}\n\n";
    f << "inline void InitSDK() { InitSDK(SDKOffsets::LibBase); }\n\n";
    f << "}  // namespace SDK\n";
    return true;
}

// ---- Variable-size type detection ----
// Holds the actual sizes of types whose size varies across UE versions,
// as reported by the game's reflection system (UStruct::PropertiesSize).
// When a size is 0 it means the struct was not found and we use a fallback.
struct CoreMathSizes {
    int32_t text      = 0;  // "Text"      — 0x18 (UE4) or 0x10 (some UE5)
    int32_t softObjectPath = 0; // "SoftObjectPath" — 0x18 or 0x20
};

INTERNAL CoreMathSizes DetectCoreMathSizes(
        const std::vector<ObjMeta>& metas) noexcept {
    CoreMathSizes s;
    for (const auto& m : metas) {
        if (m.kind != Kind::Struct) continue;
        if (m.propertiesSize <= 0) continue;
        if      (m.name == "Text")           s.text           = m.propertiesSize;
        else if (m.name == "SoftObjectPath") s.softObjectPath = m.propertiesSize;
    }
    return s;
}

INTERNAL bool WriteContainersHpp(const std::string& outDir,
                                 const CoreMathSizes& mathSizes) noexcept {
    std::ofstream f(outDir + "/Containers.hpp");
    if (!f) return false;

    // Update global detected sizes so ExpectedCppSizeForType returns accurate
    // values for the current game during emission. Property-detected values
    // (from TextProperty/SoftObjectProperty elementSize) take priority.
    if (g_detectedSizes.text <= 0) {
        g_detectedSizes.text = mathSizes.text > 0 ? mathSizes.text : 0x18;
    }
    if (g_detectedSizes.softObjectPath <= 0) {
        g_detectedSizes.softObjectPath = mathSizes.softObjectPath > 0 ? mathSizes.softObjectPath : 0x18;
    }
    if (g_detectedSizes.softObjectPtr <= 0) {
        g_detectedSizes.softObjectPtr = mathSizes.softObjectPath > 0 ? (mathSizes.softObjectPath + 0x10) : 0x28;
    }
    if (g_detectedSizes.scriptDelegate <= 0) g_detectedSizes.scriptDelegate = 0x10;
    if (g_detectedSizes.multicastScriptDelegate <= 0) g_detectedSizes.multicastScriptDelegate = 0x10;
    if (g_detectedSizes.multicastSparseDelegate <= 0) g_detectedSizes.multicastSparseDelegate = 0x1;
    if (g_detectedSizes.set <= 0) g_detectedSizes.set = 0x50;
    if (g_detectedSizes.map <= 0) g_detectedSizes.map = 0x50;

    f << "// Auto-generated by Dumper-7 [Android]. Containers and common UE structs.\n";
    f << "#pragma once\n\n";
    f << "#include \"Basic.hpp\"\n";
    f << "#include <cmath>\n";
    f << "#include <initializer_list>\n\n";
    f << "namespace SDK {\n\n";
    f << "template<class T>\n";
    f << "struct TArray {\n";
    f << "    T* Data = nullptr;\n";
    f << "    int32 Num = 0;\n";
    f << "    int32 Max = 0;\n\n";
    f << "    constexpr TArray() = default;\n";
    f << "    constexpr TArray(T* InData, int32 InNum) : Data(InData), Num(InNum), Max(InNum) {}\n";
    f << "    constexpr int32 Count() const { return Num; }\n";
    f << "    constexpr int32 size() const { return Num; }\n";
    f << "    constexpr int32 NumElements() const { return Num; }\n";
    f << "    constexpr int32 MaxElements() const { return Max; }\n";
    f << "    constexpr bool IsEmpty() const { return Num <= 0; }\n";
    f << "    constexpr bool empty() const { return IsEmpty(); }\n";
    f << "    constexpr bool IsValid() const { return Data != nullptr && Num >= 0 && Max >= Num; }\n";
    f << "    constexpr bool IsValidIndex(int32 Index) const { return Index >= 0 && Index < Num; }\n";
    f << "    T* GetData() { return Data; }\n";
    f << "    const T* GetData() const { return Data; }\n";
    f << "    T& At(int32 Index) { return (*this)[Index]; }\n";
    f << "    const T& At(int32 Index) const { return (*this)[Index]; }\n";
    f << "    T& operator[](int32 Index) { if (!IsValidIndex(Index)) throw std::out_of_range(\"TArray index\"); return Data[Index]; }\n";
    f << "    const T& operator[](int32 Index) const { if (!IsValidIndex(Index)) throw std::out_of_range(\"TArray index\"); return Data[Index]; }\n";
    f << "    T& Front() { return (*this)[0]; }\n";
    f << "    const T& Front() const { return (*this)[0]; }\n";
    f << "    T& Back() { return (*this)[Num - 1]; }\n";
    f << "    const T& Back() const { return (*this)[Num - 1]; }\n";
    f << "    int32 Find(const T& Value) const { for (int32 i = 0; i < Num; ++i) if (Data[i] == Value) return i; return -1; }\n";
    f << "    bool Contains(const T& Value) const { return Find(Value) >= 0; }\n";
    f << "    template<class Fn> void ForEach(Fn FnValue) { for (int32 i = 0; i < Num; ++i) FnValue(Data[i]); }\n";
    f << "    template<class Fn> void ForEach(Fn FnValue) const { for (int32 i = 0; i < Num; ++i) FnValue(Data[i]); }\n";
    f << "    T* begin() { return Data; }\n";
    f << "    T* end() { return Data ? Data + Num : nullptr; }\n";
    f << "    const T* begin() const { return Data; }\n";
    f << "    const T* end() const { return Data ? Data + Num : nullptr; }\n";
    f << "};\n";
    f << "static_assert(sizeof(TArray<uint8>) == 0x10, \"TArray layout must stay 16 bytes\");\n\n";
    f << "template<class K, class V>\n";
    f << "struct TPair {\n";
    f << "    K Key;\n";
    f << "    V Value;\n";
    f << "};\n\n";
    f << "template<class T>\n";
    f << "struct TSet {\n";
    f << "    uint8 Data[0x" << std::hex << std::uppercase
      << g_detectedSizes.set << std::dec << "]{};\n";
    f << "    static constexpr int32 InlineSize = 0x" << std::hex << std::uppercase
      << g_detectedSizes.set << std::dec << ";\n";
    f << "    constexpr int32 RawSize() const { return InlineSize; }\n";
    f << "    constexpr bool IsOpaque() const { return true; }\n";
    f << "    constexpr bool IsValid() const { return true; }\n";
    f << "    uint8* GetRawData() { return Data; }\n";
    f << "    const uint8* GetRawData() const { return Data; }\n";
    f << "    uint8* begin() { return Data; }\n";
    f << "    uint8* end() { return Data + InlineSize; }\n";
    f << "    const uint8* begin() const { return Data; }\n";
    f << "    const uint8* end() const { return Data + InlineSize; }\n";
    f << "};\n";
    f << "static_assert(sizeof(TSet<uint8>) == 0x" << std::hex << std::uppercase
      << g_detectedSizes.set << std::dec << ", \"TSet opaque layout size mismatch\");\n\n";
    f << "template<class K, class V>\n";
    f << "struct TMap {\n";
    f << "    uint8 Data[0x" << std::hex << std::uppercase
      << g_detectedSizes.map << std::dec << "]{};\n";
    f << "    static constexpr int32 InlineSize = 0x" << std::hex << std::uppercase
      << g_detectedSizes.map << std::dec << ";\n";
    f << "    constexpr int32 RawSize() const { return InlineSize; }\n";
    f << "    constexpr bool IsOpaque() const { return true; }\n";
    f << "    constexpr bool IsValid() const { return true; }\n";
    f << "    uint8* GetRawData() { return Data; }\n";
    f << "    const uint8* GetRawData() const { return Data; }\n";
    f << "    uint8* begin() { return Data; }\n";
    f << "    uint8* end() { return Data + InlineSize; }\n";
    f << "    const uint8* begin() const { return Data; }\n";
    f << "    const uint8* end() const { return Data + InlineSize; }\n";
    f << "};\n";
    f << "static_assert(sizeof(TMap<uint8, uint8>) == 0x" << std::hex << std::uppercase
      << g_detectedSizes.map << std::dec << ", \"TMap opaque layout size mismatch\");\n\n";
    f << "struct FString : public TArray<TCHAR> {\n";
    f << "    using TArray<TCHAR>::TArray;\n";
    f << "    const TCHAR* CStr() const { return Data ? Data : u\"\"; }\n";
    f << "};\n";
    f << "static_assert(sizeof(FString) == 0x10, \"FString layout must stay 16 bytes\");\n\n";
    // FText — size varies across UE versions. Use game-detected size.
    f << "struct FText { uint8 Data[0x" << std::hex << std::uppercase
      << g_detectedSizes.text << std::dec << "]{}; };\n\n";
    f << "struct FWeakObjectPtr {\n";
    f << "    int32 ObjectIndex = 0;\n";
    f << "    int32 ObjectSerialNumber = 0;\n";
    f << "};\n\n";
    f << "template<class T> struct TWeakObjectPtr : public FWeakObjectPtr { using ElementType = T; };\n";
    f << "template<class T> struct TLazyObjectPtr : public FWeakObjectPtr { using ElementType = T; };\n\n";
    // Keep reflected math/core structs out of here. Only opaque runtime helper
    // types are emitted, and their sizes come from this game's property data.
    f << "template<class T>\n";
    f << "struct TSubclassOf {\n";
    f << "    class UClass* Class = nullptr;\n";
    f << "    constexpr TSubclassOf() = default;\n";
    f << "    constexpr TSubclassOf(class UClass* InClass) : Class(InClass) {}\n";
    f << "    constexpr operator class UClass*() const { return Class; }\n";
    f << "    constexpr class UClass* operator->() const { return Class; }\n";
    f << "    constexpr bool operator==(const TSubclassOf& Other) const { return Class == Other.Class; }\n";
    f << "    constexpr bool operator!=(const TSubclassOf& Other) const { return Class != Other.Class; }\n";
    f << "};\n\n";
    f << "struct FScriptInterface {\n";
    f << "    class UObject* ObjectPointer = nullptr;\n";
    f << "    void* InterfacePointer = nullptr;\n";
    f << "};\n";
    f << "static_assert(sizeof(FScriptInterface) == 0x10, \"FScriptInterface layout must stay 16 bytes\");\n";
    f << "template<class T>\n";
    f << "struct TScriptInterface : public FScriptInterface { using InterfaceType = T; };\n\n";
    f << "struct FScriptDelegate { uint8 Data[0x" << std::hex << std::uppercase
      << g_detectedSizes.scriptDelegate << std::dec << "]{}; };\n";
    f << "struct FMulticastScriptDelegate { uint8 Data[0x" << std::hex << std::uppercase
      << g_detectedSizes.multicastScriptDelegate << std::dec << "]{}; };\n";
    f << "struct FMulticastSparseDelegate { uint8 Data[0x" << std::hex << std::uppercase
      << g_detectedSizes.multicastSparseDelegate << std::dec << "]{}; };\n\n";
    f << "struct FSoftObjectPtr { uint8 Data[0x" << std::hex << std::uppercase
      << g_detectedSizes.softObjectPtr << std::dec << "]{}; };\n";
    f << "template<class T> struct TSoftObjectPtr : public FSoftObjectPtr {};\n";
    f << "template<class T> struct TSoftClassPtr : public FSoftObjectPtr {};\n\n";
    f << "}  // namespace SDK\n";
    return true;
}

INTERNAL std::string Hex32(int32_t v) noexcept {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%X", v);
    return std::string(b);
}

INTERNAL void EmitKnownCoreField(std::ofstream& f,
                                 int32_t& cursor,
                                 int32_t off,
                                 int32_t size,
                                 const char* type,
                                 const char* name) noexcept {
    if (off < cursor) {
        f << "    // " << type << " " << name << ";  // known core field @ "
          << Hex32(off) << " overlaps previous emitted data\n";
        return;
    }
    if (off > cursor) {
        f << "    uint8 Pad_" << std::hex << std::uppercase << cursor
          << "[0x" << (off - cursor) << "];" << std::dec << "\n";
        cursor = off;
    }
    f << "    " << type << " " << name << ";  // " << Hex32(off)
      << " (size=" << Hex32(size) << ") NOT AUTO-GENERATED PROPERTY\n";
    cursor = off + size;
}

INTERNAL void EmitCoreTail(std::ofstream& f, int32_t& cursor, int32_t totalSize) noexcept {
    if (totalSize > cursor) {
        f << "    uint8 Pad_" << std::hex << std::uppercase << cursor
          << "[0x" << (totalSize - cursor) << "];  // tail" << std::dec << "\n";
        cursor = totalSize;
    }
}

INTERNAL int32_t CoreFallbackSize(const std::string& name, int32_t reflectedSize) noexcept {
    int32_t size = reflectedSize;
    if (name == "Object") size = std::max(size, Off::UObject::Size);
    else if (name == "Field") size = std::max(size, Off::UField::Size);
    else if (name == "Struct" || name == "ScriptStruct")
        size = std::max(size, Off::UStruct::PropertiesSize + 4);
    else if (name == "Class")
        size = std::max(size, Off::UClass::ClassDefaultObject + (int32_t)sizeof(void*));
    else if (name == "Function")
        size = std::max(size, Off::UFunction::Func + (int32_t)sizeof(void*));
    else if (name == "Enum")
        size = std::max(size, Off::UEnum::Names + 0x10);
    if (size <= 0) size = Off::UObject::Size;
    return size;
}

INTERNAL bool EmitCoreUObjectFields(std::ofstream& f,
                                    const ObjMeta& meta,
                                    int32_t baseSize) noexcept {
    const std::string& name = meta.name;
    if (!IsCoreUObjectClassName(name)) return false;

    int32_t cursor = baseSize;
    int32_t totalSize = CoreFallbackSize(name, meta.propertiesSize);

    if (name == "Object") {
        cursor = 0;
        EmitKnownCoreField(f, cursor, Off::UObject::VTable, sizeof(void*), "void*", "VTable");
        EmitKnownCoreField(f, cursor, Off::UObject::ObjectFlags, 4, "EObjectFlags", "Flags");
        EmitKnownCoreField(f, cursor, Off::UObject::InternalIndex, 4, "int32", "Index");
        EmitKnownCoreField(f, cursor, Off::UObject::ClassPrivate, sizeof(void*), "class UClass*", "Class");
        EmitKnownCoreField(f, cursor, Off::UObject::NamePrivate, sizeof(UE::FName), "FName", "Name");
        EmitKnownCoreField(f, cursor, Off::UObject::OuterPrivate, sizeof(void*), "class UObject*", "Outer");
        EmitCoreTail(f, cursor, totalSize);
        f << "\n";
        f << "    enum class Flag : uint32 {\n";
        f << "        None = 0,\n";
        f << "        Class = 1 << 0,\n";
        f << "        DefaultObject = 1 << 1,\n";
        f << "    };\n";
        f << "    friend constexpr Flag operator|(Flag A, Flag B) {\n";
        f << "        return static_cast<Flag>(static_cast<uint32>(A) | static_cast<uint32>(B));\n";
        f << "    }\n";
        f << "    friend constexpr Flag operator&(Flag A, Flag B) {\n";
        f << "        return static_cast<Flag>(static_cast<uint32>(A) & static_cast<uint32>(B));\n";
        f << "    }\n";
        f << "    static constexpr bool HasFlag(Flag Value, Flag Test) {\n";
        f << "        return (static_cast<uint32>(Value) & static_cast<uint32>(Test)) != 0;\n";
        f << "    }\n";
        f << "    static void Init(class FUObjectArray* InGObjects) { SDK::GObjects = InGObjects; }\n";
        f << "    static class FUObjectArray& ObjectList() { return *SDK::GObjects; }\n";
    if (Off::InSDK::GObjectsObjObjectsOffset == 0) {
        // GObjects points directly to the inner array
        f << "    static auto& GetGlobalObjects() { return *SDK::GObjects; }\n";
    } else {
        // GObjects points to outer FUObjectArray with header
        f << "    static auto& GetGlobalObjects() { return SDK::GObjects->ObjObjects; }\n";
    }
        f << "    static auto& Objects() { return GetGlobalObjects(); }\n";
        f << "    static std::string CleanObjectName(std::string InName) {\n";
        f << "        constexpr const char* ScriptPrefix = \"/Script/\";\n";
        f << "        if (InName.rfind(ScriptPrefix, 0) == 0) {\n";
        f << "            InName.erase(0, 8);\n";
        f << "        } else {\n";
        f << "            size_t Start = 0;\n";
        f << "            while (Start < InName.size() && (InName[Start] == '/' || InName[Start] == '\\\\')) ++Start;\n";
        f << "            if (Start > 0) InName.erase(0, Start);\n";
        f << "        }\n";
        f << "        for (char& Ch : InName) {\n";
        f << "            if (Ch == '/' || Ch == '\\\\') Ch = '.';\n";
        f << "        }\n";
        f << "        return InName;\n";
        f << "    }\n";
        f << "    std::string GetRawName() const { return Name.ToString(); }\n";
        f << "    std::string GetName() const { return CleanObjectName(GetRawName()); }\n";
        f << "    std::string GetClassName() const { return Class ? reinterpret_cast<const UObject*>(Class)->GetName() : std::string{}; }\n";
        f << "    class UObject* GetOuter() const { return Outer; }\n";
        f << "    std::string GetPathName() const {\n";
        f << "        std::string Path;\n";
        f << "        const UObject* Current = this;\n";
        f << "        for (int32 Depth = 0; Current && Depth < 64; ++Depth) {\n";
        f << "            std::string Part = Current->GetName();\n";
        f << "            if (!Part.empty()) Path = Path.empty() ? Part : Part + \".\" + Path;\n";
        f << "            Current = Current->Outer;\n";
        f << "        }\n";
        f << "        return Path;\n";
        f << "    }\n";
        f << "    std::string GetFullName() const {\n";
        f << "        std::string ClassName = GetClassName();\n";
        f << "        std::string Path = GetPathName();\n";
        f << "        if (ClassName.empty()) return Path;\n";
        f << "        return Path.empty() ? ClassName : ClassName + \" \" + Path;\n";
        f << "    }\n";
        f << "    static class UObject* GetClassDefaultObject(class UClass* ClassObject) {\n";
        f << "        if (!ClassObject) return nullptr;\n";
        f << "        return *reinterpret_cast<class UObject**>(reinterpret_cast<std::uintptr_t>(ClassObject) + SDKOffsets::UClassClassDefaultObject);\n";
        f << "    }\n";
        f << "    class UObject* GetDefaultObject() const { return GetClassDefaultObject(Class); }\n";
        f << "    template<class T> T* GetDefaultObject() const { return reinterpret_cast<T*>(GetDefaultObject()); }\n";
        f << "    bool IsA(class UClass* TargetClass) const {\n";
        f << "        if (!TargetClass) return false;\n";
        f << "        for (class UClass* Current = Class; Current; ) {\n";
        f << "            if (Current == TargetClass) return true;\n";
        f << "            Current = *reinterpret_cast<class UClass**>(reinterpret_cast<std::uintptr_t>(Current) + SDKOffsets::UStructSuperStruct);\n";
        f << "        }\n";
        f << "        return false;\n";
        f << "    }\n";
        f << "    static class UClass* GetClassClass() {\n";
        f << "        static class UClass* CachedClassClass = nullptr;\n";
        f << "        if (CachedClassClass || !SDK::GObjects) return CachedClassClass;\n";
        f << "        FName ClassName(\"Class\");\n";
        f << "        if (ClassName.IsNone()) return nullptr;\n";
        f << "        for (UObject* Obj : Objects()) {\n";
        f << "            if (Obj && Obj->Name == ClassName) {\n";
        f << "                CachedClassClass = reinterpret_cast<class UClass*>(Obj);\n";
        f << "                return CachedClassClass;\n";
        f << "            }\n";
        f << "        }\n";
        f << "        return nullptr;\n";
        f << "    }\n";
        f << "    static bool IsClassObject(const UObject* Obj) {\n";
        f << "        if (!Obj || !Obj->Class) return false;\n";
        f << "        class UClass* ClassClass = GetClassClass();\n";
        f << "        if (!ClassClass) return false;\n";
        f << "        for (class UClass* Current = Obj->Class; Current; ) {\n";
        f << "            if (Current == ClassClass) return true;\n";
        f << "            Current = *reinterpret_cast<class UClass**>(reinterpret_cast<std::uintptr_t>(Current) + SDKOffsets::UStructSuperStruct);\n";
        f << "        }\n";
        f << "        return false;\n";
        f << "    }\n";
        f << "    static bool MatchesFindFlags(const UObject* Obj, Flag Flags) {\n";
        f << "        if (!Obj) return false;\n";
        f << "        if (HasFlag(Flags, Flag::DefaultObject) && !Obj->IsDefaultObject()) return false;\n";
        f << "        if (HasFlag(Flags, Flag::Class) && !IsClassObject(Obj)) return false;\n";
        f << "        return true;\n";
        f << "    }\n";
        f << "    static UObject* FindObject(const char* FullName, Flag Flags = Flag::None) {\n";
        f << "        if (!FullName || !SDK::GObjects) return nullptr;\n";
        f << "        for (UObject* Obj : Objects()) {\n";
        f << "            if (!Obj) continue;\n";
        f << "            if (!MatchesFindFlags(Obj, Flags)) continue;\n";
        f << "            if (Obj->GetFullName() == FullName) return Obj;\n";
        f << "        }\n";
        f << "        return nullptr;\n";
        f << "    }\n";
        f << "    static UObject* FindObjectFast(const char* Name, Flag Flags = Flag::None) {\n";
        f << "        if (!Name || !SDK::GObjects) return nullptr;\n";
        f << "        FName TargetName(Name);\n";
        f << "        if (TargetName.IsNone() && std::strcmp(Name, \"None\") != 0) return nullptr;\n";
        f << "        for (UObject* Obj : Objects()) {\n";
        f << "            if (!Obj) continue;\n";
        f << "            if (Obj->Name != TargetName) continue;\n";
        f << "            if (!MatchesFindFlags(Obj, Flags)) continue;\n";
        f << "            return Obj;\n";
        f << "        }\n";
        f << "        return nullptr;\n";
        f << "    }\n";
        f << "    static class UClass* FindClassFast(const char* ClassName) {\n";
        f << "        return reinterpret_cast<class UClass*>(FindObjectFast(ClassName, Flag::Class));\n";
        f << "    }\n";
        f << "    static class UClass* FindClass(const char* ClassFullName) {\n";
        f << "        return reinterpret_cast<class UClass*>(FindObject(ClassFullName, Flag::Class));\n";
        f << "    }\n";
        f << "    template<class T> static T* FindObject(const char* FullName, Flag Flags = Flag::None) {\n";
        f << "        return reinterpret_cast<T*>(FindObject(FullName, Flags));\n";
        f << "    }\n";
        f << "    template<class T> static T* FindObjectFast(const char* Name, Flag Flags = Flag::None) {\n";
        f << "        return reinterpret_cast<T*>(FindObjectFast(Name, Flags));\n";
        f << "    }\n";
        f << "    template<class T> static T* FindObject() {\n";
        f << "        class UClass* TargetClass = T::StaticClass();\n";
        f << "        if (!TargetClass || !SDK::GObjects) return nullptr;\n";
        f << "        for (UObject* Obj : Objects()) {\n";
        f << "            if (Obj && Obj->IsA(TargetClass)) return reinterpret_cast<T*>(Obj);\n";
        f << "        }\n";
        f << "        return nullptr;\n";
        f << "    }\n";
        // FindObject by explicit UClass* — handy when the caller already has a
        // class pointer (e.g. from another lookup) or wants to avoid typing the
        // class name twice. Skips CDOs by default so callers get a live instance.
        f << "    static UObject* FindObjectOfClass(class UClass* TargetClass, bool bIncludeDefaults = false) {\n";
        f << "        if (!TargetClass || !SDK::GObjects) return nullptr;\n";
        f << "        for (UObject* Obj : Objects()) {\n";
        f << "            if (!Obj) continue;\n";
        f << "            if (!bIncludeDefaults && Obj->IsDefaultObject()) continue;\n";
        f << "            if (Obj->IsA(TargetClass)) return Obj;\n";
        f << "        }\n";
        f << "        return nullptr;\n";
        f << "    }\n";
        f << "    template<class T> static T* FindObject(class UClass* TargetClass, bool bIncludeDefaults = false) {\n";
        f << "        return reinterpret_cast<T*>(FindObjectOfClass(TargetClass, bIncludeDefaults));\n";
        f << "    }\n";
        // Returns every live instance matching TargetClass. Use when the game
        // can legitimately have multiple worlds/levels/etc. and the caller
        // wants to pick among them rather than grab the first match.
        f << "    static std::vector<UObject*> FindObjectsOfClass(class UClass* TargetClass, bool bIncludeDefaults = false) {\n";
        f << "        std::vector<UObject*> Results;\n";
        f << "        if (!TargetClass || !SDK::GObjects) return Results;\n";
        f << "        for (UObject* Obj : Objects()) {\n";
        f << "            if (!Obj) continue;\n";
        f << "            if (!bIncludeDefaults && Obj->IsDefaultObject()) continue;\n";
        f << "            if (Obj->IsA(TargetClass)) Results.push_back(Obj);\n";
        f << "        }\n";
        f << "        return Results;\n";
        f << "    }\n";
        f << "    template<class T> static std::vector<T*> FindObjectsOfClass(class UClass* TargetClass, bool bIncludeDefaults = false) {\n";
        f << "        auto Base = FindObjectsOfClass(TargetClass, bIncludeDefaults);\n";
        f << "        std::vector<T*> Out;\n";
        f << "        Out.reserve(Base.size());\n";
        f << "        for (UObject* O : Base) Out.push_back(reinterpret_cast<T*>(O));\n";
        f << "        return Out;\n";
        f << "    }\n";
        f << "    template<class T> static std::vector<T*> FindObjectsOfClass(bool bIncludeDefaults = false) {\n";
        f << "        return FindObjectsOfClass<T>(T::StaticClass(), bIncludeDefaults);\n";
        f << "    }\n";
        // ForEachObjectOfClass — callback-based iteration, no allocation
        f << "    template<class T, class Fn> static void ForEachObjectOfClass(Fn&& Callback, bool bIncludeDefaults = false) {\n";
        f << "        class UClass* TargetClass = T::StaticClass();\n";
        f << "        if (!TargetClass || !SDK::GObjects) return;\n";
        f << "        for (UObject* Obj : Objects()) {\n";
        f << "            if (!Obj) continue;\n";
        f << "            if (!bIncludeDefaults && Obj->IsDefaultObject()) continue;\n";
        f << "            if (Obj->IsA(TargetClass)) Callback(reinterpret_cast<T*>(Obj));\n";
        f << "        }\n";
        f << "    }\n";
        f << "    bool HasAnyFlags(EObjectFlags InFlags) const { return (uint32(Flags) & uint32(InFlags)) != 0; }\n";
        f << "    bool IsDefaultObject() const { return HasAnyFlags(EObjectFlags::ClassDefaultObject); }\n";
        f << "    void ProcessEvent(class UFunction* Function, void* Params) {\n";
        f << "        if (SDK::ProcessEvent) { SDK::ProcessEvent(this, Function, Params); return; }\n";
        f << "        if (SDK::ProcessEventVTableIndex >= 0) {\n";
        f << "            using Fn = void(*)(class UObject*, class UFunction*, void*);\n";
        f << "            GetVFunction<Fn>(this, SDK::ProcessEventVTableIndex)(this, Function, Params);\n";
        f << "        }\n";
        f << "    }\n";
        // GetWorld — reads GWorld directly from the resolved offset
        f << "    static class UObject* GetWorld() {\n";
        f << "        if (!SDK::GWorld) return nullptr;\n";
        f << "        return *reinterpret_cast<class UObject**>(SDK::GWorld);\n";
        f << "    }\n";
        return true;
    }

    if (name == "Field") {
        EmitKnownCoreField(f, cursor, Off::UField::Next, sizeof(void*), "class UField*", "Next");
    } else if (name == "Struct") {
        EmitKnownCoreField(f, cursor, Off::UStruct::SuperStruct, sizeof(void*), "class UStruct*", "SuperStruct");
        EmitKnownCoreField(f, cursor, Off::UStruct::Children, sizeof(void*), "class UField*", "Children");
        EmitKnownCoreField(f, cursor, Off::UStruct::ChildProperties, sizeof(void*), "void*", "ChildProperties");
        EmitKnownCoreField(f, cursor, Off::UStruct::PropertiesSize, 4, "int32", "PropertiesSize");
        EmitKnownCoreField(f, cursor, Off::UStruct::MinAlignment, 4, "int32", "MinAlignment");
    } else if (name == "Class") {
        EmitKnownCoreField(f, cursor, Off::UClass::ClassFlags, 4, "uint32", "ClassFlags");
        EmitKnownCoreField(f, cursor, Off::UClass::ClassDefaultObject, sizeof(void*), "class UObject*", "ClassDefaultObject");
        f << "    class UObject* GetDefaultObject() const { return ClassDefaultObject; }\n";
        f << "    template<class T> T* GetDefaultObject() const { return reinterpret_cast<T*>(ClassDefaultObject); }\n";
        f << "    static class UClass* GetFunctionClass() {\n";
        f << "        static class UClass* Cached = nullptr;\n";
        f << "        if (!Cached) Cached = UObject::FindClassFast(\"Function\");\n";
        f << "        return Cached;\n";
        f << "    }\n";
        f << "    class UFunction* GetFunction(const char* InFuncName) const {\n";
        f << "        if (!InFuncName) return nullptr;\n";
        f << "        FName Target(InFuncName);\n";
        f << "        if (Target.IsNone() && std::strcmp(InFuncName, \"None\") != 0) return nullptr;\n";
        f << "        class UClass* FuncClass = GetFunctionClass();\n";
        f << "        class UStruct* Curr = const_cast<class UStruct*>(\n";
        f << "            reinterpret_cast<const class UStruct*>(this));\n";
        f << "        while (Curr) {\n";
        f << "            for (class UField* Child = Curr->Children;\n";
        f << "                 Child; Child = Child->Next) {\n";
        f << "                class UObject* Obj = reinterpret_cast<class UObject*>(Child);\n";
        f << "                if (Obj->Name == Target &&\n";
        f << "                    (FuncClass == nullptr || Obj->Class == FuncClass)) {\n";
        f << "                    return reinterpret_cast<class UFunction*>(Child);\n";
        f << "                }\n";
        f << "            }\n";
        f << "            Curr = Curr->SuperStruct;\n";
        f << "        }\n";
        f << "        return nullptr;\n";
        f << "    }\n";
        f << "    class UFunction* GetFunction(const char* /*InClassName*/, const char* InFuncName) const {\n";
        f << "        return GetFunction(InFuncName);\n";
        f << "    }\n";
    } else if (name == "Function") {
        EmitKnownCoreField(f, cursor, Off::UFunction::FunctionFlags, 4, "EFunctionFlags", "FunctionFlags");
        EmitKnownCoreField(f, cursor, Off::UFunction::NumParms, 1, "uint8", "NumParms");
        EmitKnownCoreField(f, cursor, Off::UFunction::Func, sizeof(void*), "void*", "Func");
    } else if (name == "Enum") {
        EmitKnownCoreField(f, cursor, Off::UEnum::Names, 0x10,
                           "TArray<TPair<FName, int64>>", "Names");
    }

    EmitCoreTail(f, cursor, totalSize);
    return true;
}

INTERNAL void EmitInheritance(
        std::ofstream& f,
        const ObjMeta* m,
        char prefix,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr) noexcept {
    if (m->superStruct == 0) return;
    auto sit = byAddr.find(m->superStruct);
    if (sit == byAddr.end()) return;
    f << " : public " << prefix << SanitizeIdent(sit->second->name);
}

INTERNAL int32_t BaseSizeOf(
        const ObjMeta* m,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr) noexcept {
    if (!m->superStruct) return 0;
    auto sit = byAddr.find(m->superStruct);
    if (sit == byAddr.end()) return 0;
    return sit->second->propertiesSize;
}

using PropertyCache = std::unordered_map<uintptr_t, std::vector<PropertyInfo>>;
using FunctionCache = std::unordered_map<uintptr_t, std::vector<FunctionInfo>>;

struct EmittedTypeRegistry {
    std::unordered_set<std::string> enums;
    std::unordered_set<std::string> structs;
    std::unordered_set<std::string> classes;
};

INTERNAL std::vector<PropertyInfo>& CachedProperties(
        uintptr_t structAddr,
        PropertyCache& cache,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::unordered_map<uintptr_t, std::string>& fieldClassCache) noexcept {
    auto it = cache.find(structAddr);
    if (it != cache.end()) return it->second;
    auto inserted = cache.emplace(
            structAddr,
            CollectStructProperties(structAddr, byAddr, fieldClassCache));
    return inserted.first->second;
}

INTERNAL std::vector<FunctionInfo>& CachedFunctions(
        uintptr_t structAddr,
        FunctionCache& cache,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::unordered_map<uintptr_t, std::string>& fieldClassCache) noexcept {
    auto it = cache.find(structAddr);
    if (it != cache.end()) return it->second;
    auto inserted = cache.emplace(
            structAddr,
            CollectStructFunctions(structAddr, byAddr, fieldClassCache));
    return inserted.first->second;
}

INTERNAL void AddPropertyIncludeDeps(
        const PackageInfo& pkg,
        const std::vector<PropertyInfo>& props,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::set<uintptr_t>& structDeps,
        std::set<uintptr_t>& enumDeps) noexcept {
    for (const auto& p : props) {
        for (uintptr_t target : p.typeTargets) {
            if (target == 0) continue;
            auto it = byAddr.find(target);
            if (it == byAddr.end()) continue;
            if (it->second->outerPackage == 0 ||
                it->second->outerPackage == pkg.address) {
                continue;
            }
            if (it->second->kind == Kind::Enum) {
                enumDeps.insert(it->second->outerPackage);
            } else if (it->second->kind == Kind::Struct &&
                       !IsPredefinedStructName(it->second->name)) {
                structDeps.insert(it->second->outerPackage);
            }
        }
    }
}

INTERNAL void AddFunctionIncludeDeps(
        const PackageInfo& pkg,
        const std::vector<FunctionInfo>& funcs,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::set<uintptr_t>& structDeps,
        std::set<uintptr_t>& enumDeps) noexcept {
    for (const auto& fn : funcs) {
        AddPropertyIncludeDeps(pkg, fn.params, byAddr, structDeps, enumDeps);
        if (fn.hasReturnParam) {
            std::vector<PropertyInfo> one;
            one.push_back(fn.returnParam);
            AddPropertyIncludeDeps(pkg, one, byAddr, structDeps, enumDeps);
        }
    }
}

INTERNAL std::vector<uintptr_t> OrderedPackageStructs(
        const PackageInfo& pkg,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        const PropertyCache& propsByStruct) noexcept {
    std::vector<uintptr_t> out;
    out.reserve(pkg.structs.size());

    std::unordered_set<uintptr_t> inPackage;
    inPackage.reserve(pkg.structs.size());
    for (uintptr_t a : pkg.structs) {
        auto it = byAddr.find(a);
        if (it == byAddr.end()) continue;
        if (IsPredefinedStructName(it->second->name)) continue;
        inPackage.insert(a);
    }

    std::unordered_map<uintptr_t, uint8_t> state;
    state.reserve(inPackage.size());

    std::function<void(uintptr_t)> visit = [&](uintptr_t a) {
        uint8_t& s = state[a];
        if (s == 2) return;
        if (s == 1) return;  // unexpected cycle; keep discovery order fallback
        s = 1;

        auto it = byAddr.find(a);
        if (it != byAddr.end() && inPackage.count(it->second->superStruct)) {
            visit(it->second->superStruct);
        }

        auto pit = propsByStruct.find(a);
        if (pit != propsByStruct.end()) {
            for (const auto& p : pit->second) {
                if (inPackage.count(p.structTarget)) visit(p.structTarget);
            }
        }

        s = 2;
        out.push_back(a);
    };

    for (uintptr_t a : pkg.structs) {
        if (inPackage.count(a)) visit(a);
    }
    return out;
}

INTERNAL std::vector<uintptr_t> OrderedPackageClasses(
        const PackageInfo& pkg,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr) noexcept {
    std::vector<uintptr_t> out;
    out.reserve(pkg.classes.size());

    std::unordered_set<uintptr_t> inPackage;
    inPackage.reserve(pkg.classes.size());
    for (uintptr_t a : pkg.classes) {
        if (byAddr.find(a) != byAddr.end()) inPackage.insert(a);
    }

    std::unordered_map<uintptr_t, uint8_t> state;
    state.reserve(inPackage.size());

    std::function<void(uintptr_t)> visit = [&](uintptr_t a) {
        uint8_t& s = state[a];
        if (s == 2) return;
        if (s == 1) return;
        s = 1;

        auto it = byAddr.find(a);
        if (it != byAddr.end() && inPackage.count(it->second->superStruct)) {
            visit(it->second->superStruct);
        }

        s = 2;
        out.push_back(a);
    };

    for (uintptr_t a : pkg.classes) {
        if (inPackage.count(a)) visit(a);
    }
    return out;
}

INTERNAL bool WritePackageEnumsHeader(
        const PackageInfo& pkg,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        EmittedTypeRegistry& emittedTypes,
        const std::string& outDir) noexcept {

    std::string fname = outDir + "/" + pkg.name + "_enums.hpp";
    std::ofstream f(fname);
    if (!f) {
        DLOGW("[sdk] failed to open %s", fname.c_str());
        return false;
    }

    f << "// " << pkg.name << "_enums.hpp - auto-generated.\n";
    f << "#pragma once\n";
    f << "#include \"Basic.hpp\"\n";
    f << "\nnamespace SDK {\n\n";

    for (auto a : pkg.enums) {
        auto it = byAddr.find(a);
        if (it == byAddr.end()) continue;
        std::string typeName = "E" + SanitizeIdent(it->second->name);
        if (!emittedTypes.enums.insert(typeName).second) {
            DLOGW("[sdk] skipped duplicate enum %s in package %s",
                  typeName.c_str(), pkg.name.c_str());
            continue;
        }
        EmitEnum(f, *it->second);
    }

    f << "}  // namespace SDK\n";
    return true;
}

INTERNAL bool WritePackageStructsHeader(
        const PackageInfo& pkg,
        const std::unordered_map<uintptr_t, PackageInfo>& packages,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::unordered_map<uintptr_t, std::string>& fieldClassCache,
        EmittedTypeRegistry& emittedTypes,
        const std::string& outDir) noexcept {

    std::string fname = outDir + "/" + pkg.name + "_structs.hpp";
    std::ofstream f(fname);
    if (!f) {
        DLOGW("[sdk] failed to open %s", fname.c_str());
        return false;
    }

    PropertyCache propsByStruct;
    propsByStruct.reserve(pkg.structs.size());
    std::set<uintptr_t> structIncludeDeps = pkg.dependsOn;
    std::set<uintptr_t> enumIncludeDeps;
    for (auto a : pkg.structs) {
        auto it = byAddr.find(a);
        if (it == byAddr.end()) continue;
        if (IsPredefinedStructName(it->second->name)) continue;
        auto& props = CachedProperties(a, propsByStruct, byAddr, fieldClassCache);
        AddPropertyIncludeDeps(pkg, props, byAddr,
                               structIncludeDeps, enumIncludeDeps);
    }

    f << "// " << pkg.name << "_structs.hpp - auto-generated.\n";
    f << "#pragma once\n";
    f << "#include \"Containers.hpp\"\n";
    f << "#include \"" << pkg.name << "_enums.hpp\"\n";
    for (auto dep : enumIncludeDeps) {
        auto it = packages.find(dep);
        if (it == packages.end()) continue;
        if (it->second.name == pkg.name) continue;
        f << "#include \"" << it->second.name << "_enums.hpp\"\n";
    }
    for (auto dep : structIncludeDeps) {
        auto it = packages.find(dep);
        if (it == packages.end()) continue;
        if (it->second.name == pkg.name) continue;
        f << "#include \"" << it->second.name << "_structs.hpp\"\n";
    }
    f << "\nnamespace SDK {\n\n";

    for (auto a : OrderedPackageStructs(pkg, byAddr, propsByStruct)) {
        auto it = byAddr.find(a);
        if (it == byAddr.end()) continue;
        std::string typeName = "F" + SanitizeIdent(it->second->name);
        if (!emittedTypes.structs.insert(typeName).second) {
            DLOGW("[sdk] skipped duplicate struct %s in package %s",
                  typeName.c_str(), pkg.name.c_str());
            continue;
        }
        // Size and alignment comment
        f << "// Size: 0x" << std::hex << std::uppercase
          << it->second->propertiesSize << std::dec;
        if (it->second->minAlignment > 1) {
            f << " (align=0x" << std::hex << it->second->minAlignment << std::dec << ")";
        }
        f << "\n";
        // Emit alignas if non-default alignment (> 8 on arm64)
        if (it->second->minAlignment > 8) {
            f << "struct alignas(" << it->second->minAlignment << ") " << typeName;
        } else {
            f << "struct " << typeName;
        }
        EmitInheritance(f, it->second, 'F', byAddr);
        f << " {\n";
        auto pit = propsByStruct.find(a);
        const std::vector<PropertyInfo>& props =
                pit != propsByStruct.end()
                        ? pit->second
                        : CachedProperties(a, propsByStruct, byAddr, fieldClassCache);
        EmitFields(f, props, BaseSizeOf(it->second, byAddr), it->second->propertiesSize);
        f << "};\n\n";
    }

    f << "}  // namespace SDK\n";
    return true;
}

INTERNAL bool WritePackageClassesHeader(
        const PackageInfo& pkg,
        const std::unordered_map<uintptr_t, PackageInfo>& packages,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::unordered_map<uintptr_t, std::string>& fieldClassCache,
        EmittedTypeRegistry& emittedTypes,
        const std::string& outDir) noexcept {

    std::string fname = outDir + "/" + pkg.name + "_classes.hpp";
    std::ofstream f(fname);
    if (!f) {
        DLOGW("[sdk] failed to open %s", fname.c_str());
        return false;
    }

    PropertyCache propsByClass;
    propsByClass.reserve(pkg.classes.size());
    FunctionCache funcsByClass;
    funcsByClass.reserve(pkg.classes.size());
    std::set<uintptr_t> structIncludeDeps;
    std::set<uintptr_t> enumIncludeDeps;
    for (auto a : pkg.classes) {
        auto it = byAddr.find(a);
        if (it == byAddr.end()) continue;
        auto& props = CachedProperties(a, propsByClass, byAddr, fieldClassCache);
        AddPropertyIncludeDeps(pkg, props, byAddr,
                               structIncludeDeps, enumIncludeDeps);
        auto& funcs = CachedFunctions(a, funcsByClass, byAddr, fieldClassCache);
        AddFunctionIncludeDeps(pkg, funcs, byAddr,
                               structIncludeDeps, enumIncludeDeps);
    }

    f << "// " << pkg.name << "_classes.hpp - auto-generated.\n";
    f << "#pragma once\n";
    f << "#include \"Containers.hpp\"\n";
    f << "#include \"" << pkg.name << "_structs.hpp\"\n";
    for (auto dep : enumIncludeDeps) {
        auto it = packages.find(dep);
        if (it == packages.end()) continue;
        if (it->second.name == pkg.name) continue;
        f << "#include \"" << it->second.name << "_enums.hpp\"\n";
    }
    for (auto dep : structIncludeDeps) {
        auto it = packages.find(dep);
        if (it == packages.end()) continue;
        if (it->second.name == pkg.name) continue;
        f << "#include \"" << it->second.name << "_structs.hpp\"\n";
    }
    for (auto dep : pkg.dependsOn) {
        auto it = packages.find(dep);
        if (it == packages.end()) continue;
        if (it->second.name == pkg.name) continue;
        f << "#include \"" << it->second.name << "_classes.hpp\"\n";
    }
    f << "\nnamespace SDK {\n\n";

    for (auto a : OrderedPackageClasses(pkg, byAddr)) {
        auto it = byAddr.find(a);
        if (it == byAddr.end()) continue;
        std::string typeName = "U" + SanitizeIdent(it->second->name);
        if (!emittedTypes.classes.insert(typeName).second) {
            DLOGW("[sdk] skipped duplicate class %s in package %s",
                  typeName.c_str(), pkg.name.c_str());
            continue;
        }
        std::string classFullName = pkg.displayName.empty()
                ? it->second->name
                : pkg.displayName + "." + it->second->name;
        // Class comment with flags
        std::string classTagStr;
        uint32_t cf = it->second->classFlags;
        if (cf & 0x00000001) classTagStr += "Abstract,";
        if (cf & 0x00004000) classTagStr += "Interface,";
        if (cf & 0x00000004) classTagStr += "Config,";
        if (cf & 0x00000008) classTagStr += "Transient,";
        if (cf & 0x00000080) classTagStr += "Native,";
        if (cf & 0x02000000) classTagStr += "Deprecated,";
        if (cf & 0x00040000) classTagStr += "BlueprintType,";
        if (!classTagStr.empty()) classTagStr.pop_back(); // trailing comma
        f << "// Class " << classFullName;
        if (!classTagStr.empty()) f << " [" << classTagStr << "]";
        f << "\n";
        // Size comment
        f << "// Size: 0x" << std::hex << std::uppercase
          << it->second->propertiesSize << std::dec;
        if (it->second->minAlignment > 1) {
            f << " (align=0x" << std::hex << it->second->minAlignment << std::dec << ")";
        }
        f << "\n";
        f << "class " << typeName;
        EmitInheritance(f, it->second, 'U', byAddr);
        f << " {\npublic:\n";
        f << "    static class UClass* StaticClass() {\n";
        f << "        static class UClass* Class = nullptr;\n";
        f << "        if (!Class) Class = UObject::FindClassFast("
          << CppStringLiteral(it->second->name) << ");\n";
        f << "        return Class;\n";
        f << "    }\n";
        f << "    static " << typeName << "* GetDefaultObj() {\n";
        f << "        class UClass* Class = StaticClass();\n";
        f << "        return Class ? reinterpret_cast<" << typeName
          << "*>(UObject::GetClassDefaultObject(Class)) : nullptr;\n";
        f << "    }\n";
        if (!EmitCoreUObjectFields(f, *it->second, BaseSizeOf(it->second, byAddr))) {
            auto pit = propsByClass.find(a);
            const std::vector<PropertyInfo>& props =
                    pit != propsByClass.end()
                            ? pit->second
                            : CachedProperties(a, propsByClass, byAddr, fieldClassCache);
            EmitFields(f, props, BaseSizeOf(it->second, byAddr), it->second->propertiesSize);
            auto fit = funcsByClass.find(a);
            const std::vector<FunctionInfo>& funcs =
                    fit != funcsByClass.end()
                            ? fit->second
                            : CachedFunctions(a, funcsByClass, byAddr, fieldClassCache);
            EmitFunctions(f, funcs);
        } else {
            // Predefined CoreUObject classes (UObject/UStruct/UClass/...)
            // still need reflection-discovered UFunction declarations
            // (e.g. ExecuteUbergraph) because `_functions.cpp` emits the
            // out-of-line definitions for them unconditionally.
            auto fit = funcsByClass.find(a);
            const std::vector<FunctionInfo>& funcs =
                    fit != funcsByClass.end()
                            ? fit->second
                            : CachedFunctions(a, funcsByClass, byAddr, fieldClassCache);
            EmitFunctions(f, funcs);
        }
        f << "};\n\n";
    }

    f << "}  // namespace SDK\n";
    return true;
}

INTERNAL void EmitParamStructBody(std::ofstream& f,
                                  const std::vector<PropertyInfo>& props,
                                  int32_t totalSize) noexcept {
    // Similar to EmitFields, but always starts at offset 0 for UFunction frames.
    int32_t cursor = 0;
    std::unordered_map<std::string, int32_t> totalByName;
    std::unordered_map<std::string, int32_t> nextByName;
    totalByName.reserve(props.size());
    nextByName.reserve(props.size());
    for (const auto& p : props) {
        if (!p.name.empty()) ++totalByName[p.name];
    }
    auto emitName = [&](const PropertyInfo& p) -> std::string {
        std::string base = p.name.empty() ? "Field" : p.name;
        auto totalIt = totalByName.find(base);
        if (totalIt == totalByName.end() || totalIt->second <= 1) return base;
        int32_t next = ++nextByName[base];
        return base + "_" + std::to_string(next);
    };

    for (size_t i = 0; i < props.size();) {
        const auto& p = props[i];
        if (p.offset < 0 || p.elementSize <= 0 || p.arrayDim <= 0) {
            ++i;
            continue;
        }

        if (p.isBitfield) {
            const int32_t byteOffset = p.offset;
            if (totalSize > 0 && byteOffset >= totalSize) {
                ++i;
                continue;
            }
            if (byteOffset > cursor) {
                f << "    uint8_t Pad_" << std::hex << std::uppercase << cursor
                  << "[0x" << (byteOffset - cursor) << "];" << std::dec << "\n";
                cursor = byteOffset;
            }
            while (i < props.size() &&
                   props[i].isBitfield &&
                   props[i].offset == byteOffset) {
                const auto& bp = props[i];
                std::string name = emitName(bp);
                f << "    uint8_t " << name
                  << " : 1;  // 0x" << std::hex << std::uppercase
                  << bp.offset << " (mask=0x" << (int)bp.boolMask
                  << ", size=0x1)" << std::dec << "\n";
                ++i;
            }
            cursor = byteOffset + 1;
            continue;
        }

        int64_t fieldSize64 = static_cast<int64_t>(p.elementSize) * p.arrayDim;
        if (fieldSize64 <= 0 || fieldSize64 > 0x100000) {
            ++i;
            continue;
        }
        if (totalSize > 0) {
            if (p.offset >= totalSize) {
                ++i;
                continue;
            }
            if (p.offset + fieldSize64 > static_cast<int64_t>(totalSize) + 0x100) {
                ++i;
                continue;
            }
        }
        int32_t fieldSize = static_cast<int32_t>(fieldSize64);

        // Emit explicit padding to align with Unreal Engine's struct layout.
        if (p.offset > cursor) {
            f << "    uint8_t Pad_" << std::hex << std::uppercase << cursor
              << "[0x" << (p.offset - cursor) << "];" << std::dec << "\n";
            cursor = p.offset;
        }

        std::string name = emitName(p);
        const bool sizeMismatch =
                p.arrayDim <= 1 && IsTypeSizeMismatch(p.typeName, p.elementSize);
        if (sizeMismatch) {
            f << "    uint8_t " << name << "[0x"
              << std::hex << std::uppercase << p.elementSize << "];"
              << std::dec << "  // 0x" << std::hex << std::uppercase << p.offset
              << " (size=0x" << fieldSize << ", was " << p.typeName << ")"
              << std::dec << "\n";
        } else if (p.arrayDim > 1) {
            f << "    " << p.typeName << " " << name << "[" << p.arrayDim
              << "];  // 0x" << std::hex << std::uppercase << p.offset
              << " (size=0x" << fieldSize << ")" << std::dec << "\n";
        } else {
            f << "    " << p.typeName << " " << name
              << ";  // 0x" << std::hex << std::uppercase << p.offset
              << " (size=0x" << fieldSize << ")" << std::dec << "\n";
        }
        cursor = p.offset + fieldSize;
        ++i;
    }

    if (totalSize > cursor) {
        f << "    uint8_t Pad_" << std::hex << std::uppercase << cursor
          << "[0x" << (totalSize - cursor) << "];  // tail" << std::dec << "\n";
    }
}

INTERNAL std::string ParamStructNameFromClassAndFunc(
        const std::string& className,
        const FunctionInfo& fn) noexcept {
    std::string stripped = className;
    if (!stripped.empty() && (stripped[0] == 'U' || stripped[0] == 'A')) {
        stripped.erase(0, 1);
    }
    return stripped + "_" + fn.name;
}

INTERNAL bool WritePackageParametersHeader(
        const PackageInfo& pkg,
        const std::unordered_map<uintptr_t, PackageInfo>& packages,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::unordered_map<uintptr_t, std::string>& fieldClassCache,
        const std::string& outDir) noexcept {
    FunctionCache funcsByClass;
    funcsByClass.reserve(pkg.classes.size());
    bool any = false;
    std::set<uintptr_t> structIncludeDeps;
    std::set<uintptr_t> enumIncludeDeps;
    for (auto a : pkg.classes) {
        auto it = byAddr.find(a);
        if (it == byAddr.end()) continue;
        auto& funcs = CachedFunctions(a, funcsByClass, byAddr, fieldClassCache);
        if (!funcs.empty()) any = true;
        AddFunctionIncludeDeps(pkg, funcs, byAddr,
                               structIncludeDeps, enumIncludeDeps);
    }
    std::string fname = outDir + "/" + pkg.name + "_parameters.hpp";
    std::ofstream f(fname);
    if (!f) {
        DLOGW("[sdk] failed to open %s", fname.c_str());
        return false;
    }

    f << "// " << pkg.name << "_parameters.hpp - auto-generated.\n";
    f << "#pragma once\n";
    f << "#include \"Containers.hpp\"\n";
    f << "#include \"" << pkg.name << "_structs.hpp\"\n";
    for (auto dep : enumIncludeDeps) {
        auto it = packages.find(dep);
        if (it == packages.end()) continue;
        if (it->second.name == pkg.name) continue;
        f << "#include \"" << it->second.name << "_enums.hpp\"\n";
    }
    for (auto dep : structIncludeDeps) {
        auto it = packages.find(dep);
        if (it == packages.end()) continue;
        if (it->second.name == pkg.name) continue;
        f << "#include \"" << it->second.name << "_structs.hpp\"\n";
    }

    f << "\nnamespace SDK {\n";
    f << "namespace Params {\n\n";

    if (!any) {
        f << "}  // namespace Params\n";
        f << "}  // namespace SDK\n";
        return true;
    }

    for (auto a : OrderedPackageClasses(pkg, byAddr)) {
        auto it = byAddr.find(a);
        if (it == byAddr.end()) continue;
        auto fit = funcsByClass.find(a);
        if (fit == funcsByClass.end() || fit->second.empty()) continue;
        std::string className = "U" + SanitizeIdent(it->second->name);
        for (const auto& fn : fit->second) {
            if (fn.name.empty()) continue;
            if (fn.params.empty() && !fn.hasReturnParam) continue;

            std::vector<PropertyInfo> frame = fn.params;
            if (fn.hasReturnParam) {
                PropertyInfo ret = fn.returnParam;
                ret.name = "ReturnValue";
                frame.push_back(ret);
            }
            std::sort(frame.begin(), frame.end(),
                      [](const PropertyInfo& a, const PropertyInfo& b) {
                          if (a.offset != b.offset) return a.offset < b.offset;
                          if (a.isBitfield != b.isBitfield) return a.isBitfield;
                          if (a.isBitfield && b.isBitfield) {
                              int32_t abit = BitIndex(a.boolMask);
                              int32_t bbit = BitIndex(b.boolMask);
                              if (abit != bbit) return abit < bbit;
                          }
                          return a.elementSize < b.elementSize;
                      });

            const std::string structName = ParamStructNameFromClassAndFunc(className, fn);
            // Calculate parameter frame size dynamically as UStruct::PropertiesSize
            // can be undersized in some engine versions.
            int32_t totalSize = fn.paramsSize;
            for (const auto& p : frame) {
                if (p.offset < 0) continue;
                if (p.isBitfield) {
                    totalSize = std::max(totalSize, p.offset + 1);
                    continue;
                }
                if (p.elementSize <= 0) continue;
                int32_t end = p.offset + p.elementSize * std::max(p.arrayDim, 1);
                totalSize = std::max(totalSize, end);
            }
            if (totalSize <= 0) totalSize = 1;

            f << "// " << fn.rawName << "\n";
            f << "struct " << structName << " {\n";
            EmitParamStructBody(f, frame, totalSize);
            f << "};\n";
            for (const auto& p : frame) {
                if (p.name.empty()) continue;
                if (p.isBitfield) continue; // offsetof on bitfields is not portable
                f << "static_assert(offsetof(" << structName << ", " << p.name
                  << ") == 0x" << std::hex << std::uppercase << p.offset
                  << std::dec << ", \"Member 'Params::" << structName << "::"
                  << p.name << "' has a wrong offset!\");\n";
            }
            f << "\n";
        }
    }

    f << "}  // namespace Params\n";
    f << "}  // namespace SDK\n";
    return true;
}

INTERNAL bool WritePackageFunctionsCpp(
        const PackageInfo& pkg,
        const std::unordered_map<uintptr_t, PackageInfo>& packages,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        std::unordered_map<uintptr_t, std::string>& fieldClassCache,
        const std::string& outDir) noexcept {

    FunctionCache funcsByClass;
    funcsByClass.reserve(pkg.classes.size());
    bool any = false;
    std::set<uintptr_t> structIncludeDeps;
    std::set<uintptr_t> enumIncludeDeps;
    for (auto a : pkg.classes) {
        auto it = byAddr.find(a);
        if (it == byAddr.end()) continue;
        auto& funcs = CachedFunctions(a, funcsByClass, byAddr, fieldClassCache);
        if (!funcs.empty()) any = true;
        AddFunctionIncludeDeps(pkg, funcs, byAddr,
                               structIncludeDeps, enumIncludeDeps);
    }
    if (!any) return true;  // nothing to emit; not an error

    std::string fname = outDir + "/" + pkg.name + "_functions.cpp";
    std::ofstream f(fname);
    if (!f) {
        DLOGW("[sdk] failed to open %s", fname.c_str());
        return false;
    }

    f << "// " << pkg.name << "_functions.cpp - auto-generated.\n";
    f << "// Function bodies dispatch through UObject::ProcessEvent (resolved\n";
    f << "// via vtable index at SDK init), looking up the UFunction* by name\n";
    f << "// using UClass::GetFunction (runtime FindFunction equivalent).\n";
    f << "#include <cstring>\n";
    // Include only required headers to speed up C++ compilation.
    //
    // Paths are relative to SDK/Private/ where this .cpp lives.
    f << "#include \"../Public/Basic.hpp\"\n";
    f << "#include \"../Public/Containers.hpp\"\n";
    f << "#include \"../Public/" << pkg.name << "_enums.hpp\"\n";
    f << "#include \"../Public/" << pkg.name << "_structs.hpp\"\n";
    f << "#include \"../Public/" << pkg.name << "_classes.hpp\"\n";
    f << "#include \"../Public/" << pkg.name << "_parameters.hpp\"\n";
    for (auto dep : enumIncludeDeps) {
        auto it = packages.find(dep);
        if (it == packages.end()) continue;
        if (it->second.name == pkg.name) continue;
        f << "#include \"../Public/" << it->second.name << "_enums.hpp\"\n";
    }
    for (auto dep : structIncludeDeps) {
        auto it = packages.find(dep);
        if (it == packages.end()) continue;
        if (it->second.name == pkg.name) continue;
        f << "#include \"../Public/" << it->second.name << "_structs.hpp\"\n";
        f << "#include \"../Public/" << it->second.name << "_classes.hpp\"\n";
    }
    f << "\nnamespace SDK {\n\n";

    for (auto a : OrderedPackageClasses(pkg, byAddr)) {
        auto it = byAddr.find(a);
        if (it == byAddr.end()) continue;
        auto fit = funcsByClass.find(a);
        if (fit == funcsByClass.end() || fit->second.empty()) continue;
        std::string className = "U" + SanitizeIdent(it->second->name);
        for (const auto& fn : fit->second) {
            EmitFunctionImpl(f, className, fn);
        }
    }

    f << "}  // namespace SDK\n";
    return true;
}

INTERNAL std::vector<uintptr_t> OrderedSdkIncludes(
        const std::vector<uintptr_t>& order,
        const std::unordered_map<uintptr_t, PackageInfo>& packages) noexcept {
    std::vector<uintptr_t> out;
    out.reserve(order.size());
    std::unordered_set<uintptr_t> used;
    used.reserve(order.size());

    auto addByName = [&](const char* name) {
        for (uintptr_t pkgAddr : order) {
            auto it = packages.find(pkgAddr);
            if (it == packages.end()) continue;
            if (it->second.name != name) continue;
            if (used.insert(pkgAddr).second) out.push_back(pkgAddr);
            return;
        }
    };

    addByName("Script_CoreUObject");
    addByName("CoreUObject");
    addByName("Script_Engine");
    addByName("Engine");

    for (uintptr_t pkgAddr : order) {
        if (used.insert(pkgAddr).second) out.push_back(pkgAddr);
    }
    return out;
}

INTERNAL bool PackageHasContent(const PackageInfo& pkg) noexcept {
    return !pkg.classes.empty() || !pkg.structs.empty() || !pkg.enums.empty();
}

INTERNAL bool WriteSdkUmbrellaHeader(
        const std::vector<uintptr_t>& order,
        const std::unordered_map<uintptr_t, PackageInfo>& packages,
        const std::string& rootDir) noexcept {
    std::string fname = JoinPath(rootDir, "SDK.hpp");
    std::ofstream f(fname);
    if (!f) {
        DLOGW("[sdk] failed to open %s", fname.c_str());
        return false;
    }

    f << "// SDK.hpp - auto-generated umbrella include.\n";
    f << "#pragma once\n\n";
    f << "#include \"Public/Basic.hpp\"\n";
    f << "#include \"Public/Containers.hpp\"\n";

    for (uintptr_t pkgAddr : OrderedSdkIncludes(order, packages)) {
        auto it = packages.find(pkgAddr);
        if (it == packages.end()) continue;
        if (!PackageHasContent(it->second)) continue;
        f << "#include \"" << "Public/" << it->second.name << "_classes.hpp\"\n";
        f << "#include \"" << "Public/" << it->second.name << "_parameters.hpp\"\n";
    }
    return true;
}

INTERNAL std::string GetFullObjectName(uintptr_t obj) noexcept {
    if (obj == 0) return "None";
    std::string path;
    uintptr_t cur = obj;
    SafeMemory::ScopedSigSegvGuard guard;
    guard.Try([&] {
        for (int hop = 0; hop < 64; ++hop) {
            std::string name = ReadObjectName(cur);
            if (name.empty()) name = "Unnamed";
            if (path.empty()) {
                path = name;
            } else {
                path = name + "." + path;
            }
            uintptr_t outer = ReadOuter(cur);
            if (outer == 0) break;
            cur = outer;
        }
    });
    return path;
}

INTERNAL std::string GetFullFunctionName(uintptr_t fnAddr) noexcept {
    SafeMemory::ScopedSigSegvGuard guard;
    std::string path;
    guard.Try([&] {
        std::string fnName = ReadObjectName(fnAddr);
        uintptr_t classAddr = ReadOuter(fnAddr);
        if (classAddr != 0) {
            std::string className = ReadObjectName(classAddr);
            uintptr_t pkgAddr = ReadOuter(classAddr);
            if (pkgAddr != 0) {
                std::string pkgName = ReadObjectName(pkgAddr);
                path = CleanUnrealDisplayPath(pkgName) + "." + className + "." + fnName;
            } else {
                path = className + "." + fnName;
            }
        } else {
            path = fnName;
        }
    });
    return path;
}

INTERNAL bool WriteNamesDump(const std::string& rootDir) noexcept {
    std::string fname = JoinPath(rootDir, "NamesDump.txt");
    std::ofstream f(fname);
    if (!f) {
        DLOGW("[sdk] failed to open NamesDump.txt");
        return false;
    }

    int32_t consecutiveEmpty = 0;
    for (int32_t blockIndex = 0; blockIndex < 512; ++blockIndex) {
        if (g_cancel.load()) break;
        auto entries = UE::NameArray::WalkBlock(blockIndex, 65536);
        if (entries.empty()) {
            consecutiveEmpty++;
            if (consecutiveEmpty > 4) break;
            continue;
        }
        consecutiveEmpty = 0;
        for (const auto& entry : entries) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "[%08d] ", entry.first);
            f << buf << entry.second << "\n";
        }
    }
    return true;
}

INTERNAL bool WriteObjectsDump(const std::string& rootDir) noexcept {
    std::string fname = JoinPath(rootDir, "ObjectsDump.txt");
    std::ofstream f(fname);
    if (!f) {
        DLOGW("[sdk] failed to open ObjectsDump.txt");
        return false;
    }

    f << "Address\t\tIndex\tClass\t\tPath\n";
    f << "--------------------------------------------------\n";

    SafeMemory::ScopedSigSegvGuard guard;
    std::unordered_map<uintptr_t, std::string> classNameCache;
    classNameCache.reserve(1024);

    auto classNameOf = [&](uintptr_t obj) -> std::string {
        uintptr_t cls = 0;
        guard.Try([&] { cls = ReadClassPtr(obj); });
        if (cls == 0) return {};
        auto it = classNameCache.find(cls);
        if (it != classNameCache.end()) return it->second;
        std::string n;
        guard.Try([&] { n = ReadObjectName(cls); });
        classNameCache.emplace(cls, n);
        return n;
    };

    UE::ObjectArray::ForEach([&](UE::UObject* obj) {
        if (g_cancel.load()) return;
        uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
        std::string clsName = classNameOf(addr);
        if (clsName.empty()) clsName = "UnknownClass";

        int32_t idx = -1;
        guard.Try([&] {
            auto pIdx = SafeMemory::SafeReadAny<int32_t>(addr + Off::UObject::InternalIndex);
            if (pIdx) idx = *pIdx;
        });

        std::string fullPath = GetFullObjectName(addr);
        
        char buf[64];
        std::snprintf(buf, sizeof(buf), "[0x%llX] (%d) ", (unsigned long long)addr, idx);
        f << buf << clsName << "\t" << fullPath << "\n";
    });

    return true;
}

INTERNAL bool WriteFunctionsLogAndScripts(
        const std::vector<uintptr_t>& order,
        const std::unordered_map<uintptr_t, PackageInfo>& packages,
        const std::unordered_map<uintptr_t, const ObjMeta*>& byAddr,
        const std::string& rootDir) noexcept {
    std::string logFname = JoinPath(rootDir, "Functions.log");
    std::string idapyFname = JoinPath(rootDir, "SDKRename_IDA.py");
    std::string ghidrapyFname = JoinPath(rootDir, "SDKRename_Ghidra.py");
    
    std::ofstream logF(logFname);
    std::ofstream idapyF(idapyFname);
    std::ofstream ghidrapyF(ghidrapyFname);

    if (!logF) {
        DLOGW("[sdk] failed to open Functions.log");
        return false;
    }

    logF << "// Universal Android Unreal Engine SDK Dumper — Native Function Offset Registry\n";
    logF << "// lib.base = " << HexPtr(CurrentLibBase()) << "\n\n";

    if (idapyF) {
        idapyF << "# Universal Android Unreal Engine SDK Dumper — IDA Pro Renaming Script\n";
        idapyF << "import idaapi\n\n";
        idapyF << "def rename(offset, name):\n";
        idapyF << "    addr = idaapi.get_imagebase() + offset\n";
        idapyF << "    if addr != idaapi.BADADDR:\n";
        idapyF << "        idaapi.set_name(addr, name, idaapi.SN_FORCE)\n\n";
        idapyF << "print(\"[sdk] Starting dynamic renaming...\")\n";
    }

    if (ghidrapyF) {
        ghidrapyF << "# Universal Android Unreal Engine SDK Dumper — Ghidra Renaming Script\n";
        ghidrapyF << "# @category UnrealEngine\n\n";
        ghidrapyF << "def rename(offset, name):\n";
        ghidrapyF << "    address = currentProgram.getImageBase().add(offset)\n";
        ghidrapyF << "    try:\n";
        ghidrapyF << "        createLabel(address, name, True)\n";
        ghidrapyF << "    except:\n";
        ghidrapyF << "        pass\n\n";
        ghidrapyF << "print \"[sdk] Starting dynamic renaming...\"\n";
    }

    std::unordered_map<uintptr_t, std::string> fieldClassCache;

    struct NativeFuncEntry {
        std::string fullPath;
        std::string sanitizedName;
        uintptr_t   offset;
        uint32_t    flags;
    };
    std::vector<NativeFuncEntry> nativeFuncs;

    for (uintptr_t pkgAddr : order) {
        auto pit = packages.find(pkgAddr);
        if (pit == packages.end()) continue;
        for (auto a : pit->second.functions) {
            FunctionInfo fn = BuildFunctionInfo(a, byAddr, fieldClassCache);
            if (fn.nativeFuncPtr != 0) {
                NativeFuncEntry entry;
                entry.fullPath = GetFullFunctionName(a);
                
                std::string sanName;
                for (char c : entry.fullPath) {
                    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                        sanName += c;
                    } else if (c == '.' || c == ':' || c == '/' || c == '\\') {
                        sanName += '_';
                    }
                }
                entry.sanitizedName = sanName;
                entry.offset = fn.nativeFuncPtr - Off::Resolved::LibBase;
                entry.flags = fn.functionFlags;
                nativeFuncs.push_back(std::move(entry));
            }
        }
    }

    std::sort(nativeFuncs.begin(), nativeFuncs.end(), [](const NativeFuncEntry& a, const NativeFuncEntry& b) {
        return a.offset < b.offset;
    });

    for (const auto& entry : nativeFuncs) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "[0x%08llX] ", (unsigned long long)entry.offset);
        logF << buf << "Function " << entry.fullPath 
             << " (Flags: 0x" << std::hex << std::uppercase << entry.flags << std::dec << ")\n";

        if (idapyF) {
            idapyF << "rename(0x" << std::hex << std::uppercase << entry.offset << std::dec 
                   << ", \"" << entry.sanitizedName << "\")\n";
        }
        if (ghidrapyF) {
            ghidrapyF << "rename(0x" << std::hex << std::uppercase << entry.offset << std::dec 
                   << ", \"" << entry.sanitizedName << "\")\n";
        }
    }

    if (idapyF) {
        idapyF << "print(\"[sdk] Renaming complete!\")\n";
    }
    if (ghidrapyF) {
        ghidrapyF << "print \"[sdk] Renaming complete!\"\n";
    }

    return true;
}

INTERNAL bool WriteOffsetsLog(const std::string& rootDir) noexcept {
    std::string fname = JoinPath(rootDir, "Offsets.log");
    std::ofstream f(fname);
    if (!f) {
        DLOGW("[sdk] failed to open %s", fname.c_str());
        return false;
    }

    auto writeAddr = [&](const char* label,
                         uintptr_t addr,
                         const char* method,
                         const char* detail) {
        f << label << ".method = " << (method && *method ? method : "unknown") << "\n";
        f << label << ".address = " << HexPtr(addr) << "\n";
        f << label << ".offset = " << HexPtr(OffsetFromLibBase(addr)) << "\n";
        if (detail && *detail) f << label << ".detail = " << detail << "\n";
        f << "\n";
    };

    f << "SDK discovery log\n";
    f << "lib.path = " << (Off::Discovery::LibPath[0] ? Off::Discovery::LibPath : "(unknown)") << "\n";
    f << "lib.base = " << HexPtr(CurrentLibBase()) << "\n\n";

    f << "engine.version = " << EngineVersionString() << "\n";
    f << "engine.major = " << Off::InSDK::EngineMajor << "\n";
    f << "engine.minor = " << Off::InSDK::EngineMinor << "\n";
    f << "engine.patch = " << Off::InSDK::EnginePatch << "\n";
    f << "engine.changelist = " << Off::InSDK::EngineChangelist << "\n";
    f << "engine.branch = "
      << (Off::Discovery::EngineBranch[0] ? Off::Discovery::EngineBranch : "(unknown)") << "\n";
    f << "engine.method = "
      << (Off::Discovery::EngineVersionMethod[0] ? Off::Discovery::EngineVersionMethod : "unknown") << "\n";
    if (Off::Discovery::EngineVersionDetail[0]) {
        f << "engine.detail = " << Off::Discovery::EngineVersionDetail << "\n";
    }
    f << "\n";

    writeAddr("GUObjectArray", Off::Resolved::GObjects,
              Off::Discovery::GObjectsMethod, Off::Discovery::GObjectsDetail);
    writeAddr("GNames", Off::Resolved::GNames,
              Off::Discovery::GNamesMethod, Off::Discovery::GNamesDetail);
    writeAddr("GWorld", Off::Resolved::GWorld,
              Off::Discovery::GWorldMethod, Off::Discovery::GWorldDetail);
    writeAddr("ProcessEvent", Off::Resolved::ProcessEvent,
              Off::Discovery::ProcessEventMethod, Off::Discovery::ProcessEventDetail);

    f << "ProcessEvent.vtable.index = "
      << Off::Resolved::ProcessEventVTableIndex << "\n";
    f << "ProcessEvent.vtable.offset = "
      << HexPtr(Off::Resolved::ProcessEventVTableIndex >= 0
              ? static_cast<uintptr_t>(Off::Resolved::ProcessEventVTableIndex) * sizeof(void*)
              : 0) << "\n";
    f << "ProcessEvent.vtable.slot_address = "
      << HexPtr(Off::Resolved::ProcessEventVTableSlot) << "\n\n";

    f << "UObject.VTable = " << Hex32(Off::UObject::VTable) << "\n";
    f << "UObject.ObjectFlags = " << Hex32(Off::UObject::ObjectFlags) << "\n";
    f << "UObject.InternalIndex = " << Hex32(Off::UObject::InternalIndex) << "\n";
    f << "UObject.ClassPrivate = " << Hex32(Off::UObject::ClassPrivate) << "\n";
    f << "UObject.NamePrivate = " << Hex32(Off::UObject::NamePrivate) << "\n";
    f << "UObject.OuterPrivate = " << Hex32(Off::UObject::OuterPrivate) << "\n";
    f << "UObject.Size = " << Hex32(Off::UObject::Size) << "\n\n";

    f << "FNamePool.BlocksOffset = " << Hex32(Off::FNamePool::BlocksOffset) << "\n";
    f << "FNamePool.Stride = " << Hex32(Off::FNamePool::Stride) << "\n";
    f << "FNamePool.BlockOffsetBits = " << Off::FNamePool::BlockOffsetBits << "\n";
    f << "FNamePool.CasePreserving = " << (Off::InSDK::bIsCasePreserving ? "true" : "false") << "\n\n";

    f << "UStruct.SuperStruct = " << Hex32(Off::UStruct::SuperStruct) << "\n";
    f << "UStruct.Children = " << Hex32(Off::UStruct::Children) << "\n";
    f << "UStruct.ChildProperties = " << Hex32(Off::UStruct::ChildProperties) << "\n";
    f << "UStruct.PropertiesSize = " << Hex32(Off::UStruct::PropertiesSize) << "\n";
    f << "UStruct.MinAlignment = " << Hex32(Off::UStruct::MinAlignment) << "\n\n";

    f << "UFunction.FunctionFlags = " << Hex32(Off::UFunction::FunctionFlags) << "\n";
    f << "UFunction.NumParms = " << Hex32(Off::UFunction::NumParms) << "\n";
    f << "UFunction.Func = " << Hex32(Off::UFunction::Func) << "\n\n";
    f << "# Auto-detected non-reflected offsets (-1 = not found)\n";
    f << "ULevel.Actors = " << Hex32(Off::AutoDetected::ULevelActors) << "\n";
    return true;
}

INTERNAL bool WriteHeaders(const std::vector<uintptr_t>& order,
                           const std::unordered_map<uintptr_t, PackageInfo>& packages,
                           const std::vector<ObjMeta>& metas,
                           const std::string& outDir) noexcept {
    const std::string rootDir = JoinPath(outDir, "SDK");
    const std::string sdkDir = JoinPath(rootDir, "Public");
    const std::string privDir = JoinPath(rootDir, "Private");

    if (!MakeDirRec(sdkDir)) {
        SetError("Failed to create output dir: " + sdkDir);
        return false;
    }
    // Private holds generated .cpp implementation units.
    if (!MakeDirRec(privDir)) {
        SetError("Failed to create output dir: " + privDir);
        return false;
    }
    ResolveSdkExtraOffsets();
    if (!WriteBasicHpp(sdkDir)) {
        SetError("Failed to write Basic.hpp");
        return false;
    }
    std::unordered_map<uintptr_t, const ObjMeta*> byAddr;
    byAddr.reserve(metas.size());
    for (const auto& m : metas) byAddr[m.address] = &m;

    // FFieldClass* → class name (e.g., "IntProperty"). Shared across all
    // packages so we decode each FFieldClass.Name once per dump.
    std::unordered_map<uintptr_t, std::string> fieldClassCache;
    fieldClassCache.reserve(64);

    // Pre-scan all properties to populate g_enumSizeHints before writing headers.
    {
        std::lock_guard<std::mutex> lock(g_enumSizeHintsMutex);
        g_enumSizeHints.clear();
    }
    ResetDetectedSdkSizes();
    for (const auto& m : metas) {
        if (g_cancel.load()) return false;
        if (m.kind != Kind::Struct && m.kind != Kind::Class &&
            m.kind != Kind::Function) continue;
        // Side effect: populates g_enumSizeHints and opaque support sizes via
        // trySanitizeAndPush before enums/Containers.hpp are emitted.
        (void)CollectStructProperties(m.address, byAddr, fieldClassCache);
    }

    CoreMathSizes mathSizes = DetectCoreMathSizes(metas);
    if (!WriteContainersHpp(sdkDir, mathSizes)) {
        SetError("Failed to write Containers.hpp");
        return false;
    }

    EmittedTypeRegistry emittedTypes;
    emittedTypes.enums.reserve(metas.size());
    emittedTypes.structs.reserve(metas.size());
    emittedTypes.classes.reserve(metas.size());

    for (uintptr_t pkgAddr : order) {
        if (g_cancel.load()) return false;
        auto it = packages.find(pkgAddr);
        if (it == packages.end()) continue;
        SetCurrentPackage(it->second.name);

        // Skip empty packages (no reflection content).
        if (!PackageHasContent(it->second)) {
            IncWritten();
            continue;
        }

        if (!WritePackageEnumsHeader(it->second, byAddr,
                                     emittedTypes, sdkDir)) {
            SetError("Failed writing " + it->second.name + "_enums.hpp");
            return false;
        }
        if (!WritePackageStructsHeader(it->second, packages, byAddr,
                                       fieldClassCache, emittedTypes, sdkDir)) {
            SetError("Failed writing " + it->second.name + "_structs.hpp");
            return false;
        }
        if (!WritePackageClassesHeader(it->second, packages, byAddr,
                                       fieldClassCache, emittedTypes, sdkDir)) {
            SetError("Failed writing " + it->second.name + "_classes.hpp");
            return false;
        }
        if (!WritePackageParametersHeader(it->second, packages, byAddr,
                                          fieldClassCache, sdkDir)) {
            SetError("Failed writing " + it->second.name + "_parameters.hpp");
            return false;
        }
        if (!WritePackageFunctionsCpp(it->second, packages, byAddr,
                                      fieldClassCache, privDir)) {
            SetError("Failed writing " + it->second.name + "_functions.cpp");
            return false;
        }
        IncWritten();
    }

    if (!WriteSdkUmbrellaHeader(order, packages, rootDir)) {
        SetError("Failed writing SDK.hpp");
        return false;
    }
    if (!WriteOffsetsLog(rootDir)) {
        SetError("Failed writing Offsets.log");
        return false;
    }
    if (!WriteNamesDump(rootDir)) {
        SetError("Failed writing NamesDump.txt");
        return false;
    }
    if (!WriteObjectsDump(rootDir)) {
        SetError("Failed writing ObjectsDump.txt");
        return false;
    }
    if (!WriteFunctionsLogAndScripts(order, packages, byAddr, rootDir)) {
        SetError("Failed writing Functions.log / SDKRename scripts");
        return false;
    }
    // Write a machine-readable JSON mapping of all classes/structs with their
    // sizes and member offsets, plus all native functions and their offsets.
    {
        std::string fname = JoinPath(rootDir, "Mappings.json");
        std::ofstream jf(fname);
        if (jf) {
            jf << "{\n";
            jf << "  \"engine\": \"" << EngineVersionString() << "\",\n";
            jf << "  \"libBase\": \"" << HexPtr(CurrentLibBase()) << "\",\n";
            jf << "  \"GObjects\": \"" << HexPtr(Off::Resolved::GObjects) << "\",\n";
            jf << "  \"GNames\": \"" << HexPtr(Off::Resolved::GNames) << "\",\n";
            jf << "  \"GWorld\": \"" << HexPtr(Off::Resolved::GWorld) << "\",\n";
            jf << "  \"ProcessEvent\": \"" << HexPtr(Off::Resolved::ProcessEvent) << "\",\n";
            jf << "  \"classes\": {\n";
            bool firstClass = true;
            for (uintptr_t pkgAddr : order) {
                auto pit = packages.find(pkgAddr);
                if (pit == packages.end()) continue;
                for (auto a : pit->second.classes) {
                    auto cit = byAddr.find(a);
                    if (cit == byAddr.end()) continue;
                    if (!firstClass) jf << ",\n";
                    firstClass = false;
                    // Collect members for JSON output
                    auto props = CollectStructProperties(a, byAddr, fieldClassCache);
                    jf << "    \"" << cit->second->name << "\": {"
                       << "\"size\": " << cit->second->propertiesSize
                       << ", \"package\": \"" << pit->second.name << "\"";
                    if (!props.empty()) {
                        jf << ", \"members\": {";
                        bool firstMem = true;
                        for (const auto& p : props) {
                            if (p.name.empty() || p.isBitfield) continue;
                            if (!firstMem) jf << ", ";
                            firstMem = false;
                            jf << "\"" << p.name << "\": " << p.offset;
                        }
                        jf << "}";
                    }
                    jf << "}";
                }
            }
            jf << "\n  },\n";
            jf << "  \"structs\": {\n";
            bool firstStruct = true;
            for (uintptr_t pkgAddr : order) {
                auto pit = packages.find(pkgAddr);
                if (pit == packages.end()) continue;
                for (auto a : pit->second.structs) {
                    auto sit = byAddr.find(a);
                    if (sit == byAddr.end()) continue;
                    if (!firstStruct) jf << ",\n";
                    firstStruct = false;
                    jf << "    \"" << sit->second->name << "\": {"
                       << "\"size\": " << sit->second->propertiesSize
                       << ", \"package\": \"" << pit->second.name << "\""
                       << "}";
                }
            }
            jf << "\n  },\n";
            jf << "  \"functions\": {\n";
            bool firstFunc = true;
            for (uintptr_t pkgAddr : order) {
                auto pit = packages.find(pkgAddr);
                if (pit == packages.end()) continue;
                for (auto a : pit->second.functions) {
                    FunctionInfo fn = BuildFunctionInfo(a, byAddr, fieldClassCache);
                    if (fn.nativeFuncPtr != 0) {
                        if (!firstFunc) jf << ",\n";
                        firstFunc = false;
                        std::string fullPath = GetFullFunctionName(a);
                        uintptr_t offset = fn.nativeFuncPtr - Off::Resolved::LibBase;
                        jf << "    \"" << fullPath << "\": {"
                           << "\"offset\": \"0x" << std::hex << std::uppercase << offset << std::dec << "\""
                           << ", \"flags\": \"0x" << std::hex << std::uppercase << fn.functionFlags << std::dec << "\""
                           << ", \"package\": \"" << pit->second.name << "\""
                           << "}";
                    }
                }
            }
            jf << "\n  }\n";
            jf << "}\n";
        }
    }
    return true;
}

// =============================================================================
// Worker thread entry.
// =============================================================================

INTERNAL void WorkerEntry(std::string outputDir) noexcept {
    auto t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_progress = Progress{};
        g_progress.outputDir = JoinPath(outputDir, "SDK");
        g_progress.phase     = Phase::Collecting;
    }
    g_cancel.store(false);

    DLOGI("[sdk] Start outputDir=%s root=%s",
          outputDir.c_str(), JoinPath(outputDir, "SDK").c_str());
    if (!EnsurePropertyLayoutReady()) {
        SetError("Advanced property layout scan failed; refusing to dump invalid fields");
        g_running.store(false);
        return;
    }

    std::vector<ObjMeta> metas;
    std::unordered_map<uintptr_t, PackageInfo> packages;
    if (!CollectObjects(metas, packages)) {
        SetError("Collection produced no reflection objects");
        g_running.store(false);
        return;
    }
    DLOGI("[sdk] Collected: %zu metas across %zu packages",
          metas.size(), packages.size());

    if (g_cancel.load()) {
        SetPhase(Phase::Cancelled);
        g_running.store(false);
        return;
    }

    SetPhase(Phase::Sorting);
    auto order = TopoSortPackages(packages);
    DLOGI("[sdk] Topo-sorted %zu packages", order.size());

    if (g_cancel.load()) {
        SetPhase(Phase::Cancelled);
        g_running.store(false);
        return;
    }

    SetPhase(Phase::Writing);
    bool ok = WriteHeaders(order, packages, metas, outputDir);

    auto t1 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_progress.elapsedMicros =
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (g_cancel.load())   g_progress.phase = Phase::Cancelled;
        else if (ok)           g_progress.phase = Phase::Done;
        // SetError already set Failed if !ok
    }
    DLOGI("[sdk] Finished phase=%s in %.1f ms",
          PhaseName(Snapshot().phase), Snapshot().elapsedMicros / 1000.0);
    g_running.store(false);
}

}  // namespace

// =============================================================================
// Public API.
// =============================================================================

bool Start(const std::string& outputDir) noexcept {
    if (g_running.load()) return false;
    if (!UE::ObjectArray::IsInitialized() || !UE::NameArray::IsInitialized()) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_progress       = Progress{};
        g_progress.error = "ObjectArray + NameArray must be initialized";
        g_progress.phase = Phase::Failed;
        return false;
    }
    g_running.store(true);
    if (g_worker.joinable()) g_worker.join();
    g_worker = std::thread(WorkerEntry, outputDir);
    g_worker.detach();
    return true;
}

void RequestCancel() noexcept { g_cancel.store(true); }

Progress Snapshot() noexcept {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_progress;
}

bool IsRunning() noexcept { return g_running.load(); }

const char* PhaseName(Phase p) noexcept {
    switch (p) {
        case Phase::Idle:       return "idle";
        case Phase::Collecting: return "collecting";
        case Phase::Sorting:    return "sorting";
        case Phase::Writing:    return "writing";
        case Phase::Done:       return "done";
        case Phase::Failed:     return "failed";
        case Phase::Cancelled:  return "cancelled";
    }
    return "?";
}

}  // namespace SDKDumper

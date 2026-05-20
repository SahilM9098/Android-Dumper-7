// =============================================================================
// Core/UEStructs.h
// -----------------------------------------------------------------------------
// In-memory layout descriptors for the UE4/UE5 reflection types we need to
// walk, plus the Off:: namespace — a runtime-populated table of byte offsets
// and engine flags. Defaults reflect a stock UE4.27 arm64 build; OffsetFinder
// overwrites them once it has identified the target engine version. No code
// outside this header should hardcode a UE offset.
//
// All variables in Off:: are inline (C++17) so multiple TUs can share them
// without an .o coordination file. They are mutable on purpose — there is
// exactly one population pass at startup and no concurrent mutation.
// =============================================================================

#pragma once

#include <cstdint>
#include <cstddef>

namespace UE {

// --- Forward decls (opaque pointer types — we never construct these) ---
struct UObject;
struct UField;
struct UStruct;
struct UClass;
struct UEnum;
struct UFunction;
struct UProperty;     // UE4 property base
struct FProperty;     // UE5 property base (lives in ChildProperties chain)

// FName as it appears packed inside UObject::NamePrivate. Two int32s on most
// builds; a third index on case-preserving builds (set by Off::InSDK).
struct FName {
    int32_t ComparisonIndex = 0;
    int32_t Number          = 0;
    // DisplayIndex (case-preserving): read manually using offsets when the
    // engine flag bIsCasePreserving is true.
};
static_assert(sizeof(FName) == 8, "FName base assumes 8 bytes");

// Header preceding each name in FNamePool (UE4.23+).
//   bit 0      = bIsWide
//   bits 1..15 = length (in code units)
struct FNameEntryHeader {
    uint16_t bIsWide : 1;
    uint16_t Length  : 15;
};
static_assert(sizeof(FNameEntryHeader) == 2, "FNameEntryHeader must be 2 bytes");

// FUObjectItem for chunked + flat array layouts. Fields after Object are
// flags/serial — we only need Object here.
struct FUObjectItem_18 {  // UE4.13–UE4.21 layout (0x18 bytes)
    UObject* Object;
    int32_t  Flags;
    int32_t  ClusterRootIndex;
    int32_t  SerialNumber;
};

struct FUObjectItem_20 {  // UE4.22+ / UE5 layout (0x20 bytes)
    UObject* Object;
    int32_t  Flags;
    int32_t  ClusterRootIndex;
    int32_t  SerialNumber;
    int32_t  Pad;
};

}  // namespace UE

// =============================================================================
// Off:: — runtime-detected offsets and engine flags.
// Defaults are UE4.27 arm64 (Dumper-7's reference layout); replaced wholesale
// by OffsetFinder. Inline variables: no .cpp coordination needed.
// =============================================================================

namespace Off {

namespace InSDK {
    inline bool    bIsUE5             = false;
    inline bool    bIsChunked         = true;   // FChunkedFixedUObjectArray
    inline bool    bUsesNamePool      = true;   // FNamePool vs TNameEntryArray
    inline bool    bUsesAppendString  = false;  // call FName::AppendString fn
    inline bool    bIsCasePreserving  = false;
    inline int32_t FUObjectItemSize   = 0x18;   // 0x18 (UE4.21-) or 0x20
    inline int32_t GObjectsObjObjectsOffset = 0x10; // offset from GObjects to ObjObjects (0x10 = outer, 0 = inner)
    inline int32_t EngineMajor        = 4;
    inline int32_t EngineMinor        = 27;
    inline int32_t EnginePatch        = -1;
    inline uint32_t EngineChangelist  = 0;
}

namespace Resolved {
    inline uintptr_t GObjects     = 0;   // address of GUObjectArray (in libUE4)
    inline uintptr_t GNames       = 0;   // address of NamePoolData / TNameEntryArray
    inline uintptr_t AppendString = 0;   // FName::AppendString function ptr
    inline uintptr_t LibBase      = 0;   // libUE4/libUnreal base used for offsets
    inline uintptr_t GWorld       = 0;   // address of GWorld global storage
    inline uintptr_t ProcessEvent = 0;   // UObject::ProcessEvent function ptr
    inline int32_t   ProcessEventVTableIndex = -1;
    inline uintptr_t ProcessEventVTableSlot  = 0; // one observed vtable slot VA
}

namespace Discovery {
    inline char LibPath[512] = {};
    inline char GObjectsMethod[32] = {};
    inline char GObjectsDetail[192] = {};
    inline char GNamesMethod[32] = {};
    inline char GNamesDetail[192] = {};
    inline char GWorldMethod[32] = {};
    inline char GWorldDetail[192] = {};
    inline char ProcessEventMethod[32] = {};
    inline char ProcessEventDetail[192] = {};
    inline char EngineVersion[32] = {};
    inline char EngineBranch[128] = {};
    inline char EngineVersionMethod[32] = {};
    inline char EngineVersionDetail[192] = {};
}

namespace UObject {
    inline int32_t VTable        = 0x00;
    inline int32_t ObjectFlags   = 0x08;
    inline int32_t InternalIndex = 0x0C;
    inline int32_t ClassPrivate  = 0x10;
    inline int32_t NamePrivate   = 0x18;
    inline int32_t OuterPrivate  = 0x20;
    inline int32_t Size          = 0x28;  // sizeof(UObject)
}

namespace UField {
    inline int32_t Next = 0x28;
    inline int32_t Size = 0x30;
}

namespace UStruct {
    inline int32_t SuperStruct      = 0x40;
    inline int32_t Children         = 0x48;
    inline int32_t ChildProperties  = 0x50;  // UE5 only; ignored on UE4
    inline int32_t PropertiesSize   = 0x58;
    inline int32_t MinAlignment     = 0x5C;
    inline int32_t Size             = 0x60;
}

namespace UClass {
    inline int32_t ClassFlags         = 0xB0;
    inline int32_t ClassDefaultObject = 0x118;
    inline int32_t Size               = 0x230;
}

namespace UEnum {
    inline int32_t Names = 0x40;
    inline int32_t Size  = 0x50;
}

namespace UFunction {
    inline int32_t FunctionFlags = 0xB0;
    inline int32_t NumParms      = 0xB4;
    inline int32_t Func          = 0xC8;
    inline int32_t Size          = 0xE0;
}

namespace UProperty {            // UE4 path (UProperty derives from UField)
    inline int32_t ArrayDim       = 0x30;
    inline int32_t ElementSize    = 0x34;
    inline int32_t PropertyFlags  = 0x38;
    inline int32_t Offset_Internal = 0x44;
    inline int32_t Size           = 0x70;
}

namespace FField {               // FField base (UE4.25+ FProperty rework)
    inline int32_t ClassPrivate   = 0x08;  // FFieldClass*
    inline int32_t Owner          = 0x10;  // FFieldVariant (16 bytes)
    inline int32_t Next           = 0x20;  // FField*
    inline int32_t NamePrivate    = 0x28;  // FName
    inline int32_t FlagsPrivate   = 0x30;  // EObjectFlags (uint32)
    // FField data ends at 0x34 — uint32 FlagsPrivate.
}

namespace FFieldClass {
    inline int32_t Name           = 0x00;  // FName (decodes to "IntProperty" etc.)
}

namespace FProperty {            // UE5 / UE4.25+ path (ChildProperties chain)
    // C++ derived-class layout reuses the base's trailing padding under the
    // Itanium ABI (Android NDK). FField data ends at 0x34, so FProperty's
    // ArrayDim packs into that gap rather than starting at sizeof(FField)=0x38.
    inline int32_t ArrayDim        = 0x34;  // int32 (in FField trailing padding)
    inline int32_t ElementSize     = 0x38;  // int32
    inline int32_t PropertyFlags   = 0x40;  // uint64 (after 4 bytes of pad)
    inline int32_t Offset_Internal = 0x4C;  // int32
    inline int32_t Next            = 0x20;  // alias for FField::Next
    inline int32_t Size            = 0x78;  // sizeof(FProperty); subclass extras follow
}

namespace FUObjectArray {
    inline int32_t ObjFirstGCIndex = 0x00;
    inline int32_t ObjLastNonGCIndex = 0x04;
    inline int32_t MaxObjectsNotConsideredByGC = 0x08;
    inline int32_t bOpenForDisregardForGC = 0x0C;
    inline int32_t ObjObjects   = 0x10;
}

namespace FChunkedFixedUObjectArray {
    inline int32_t Objects        = 0x00;  // FUObjectItem** chunks
    inline int32_t PreAllocatedObjects = 0x08;
    inline int32_t MaxElements    = 0x10;
    inline int32_t NumElements    = 0x14;
    inline int32_t MaxChunks      = 0x18;
    inline int32_t NumChunks      = 0x1C;
    inline int32_t ElementsPerChunk = 64 * 1024;  // 64K, UE default
}

namespace FFixedUObjectArray {
    inline int32_t Objects     = 0x00;  // FUObjectItem* base
    inline int32_t MaxElements = 0x08;
    inline int32_t NumElements = 0x0C;
}

namespace FNameEntry {
    inline int32_t HeaderOffset = 0x00;  // FNameEntryHeader (UE4.23+)
    inline int32_t NameOffset   = 0x02;  // ANSICHAR/WIDECHAR data immediately follows
    // Older TNameEntryArray layout — used only when InSDK::bUsesNamePool == false.
    inline int32_t LegacyIndex     = 0x00;
    inline int32_t LegacyHashNext  = 0x08;
    inline int32_t LegacyName      = 0x10;
    inline int32_t LegacyEntrySize = 0x60;  // worst-case
}

namespace FNamePool {
    // FNamePool layout (UE4.23+). Auto-detected at runtime by NameArray::Init
    // because the offset of the inner Blocks[] array drifts per engine version
    // and per platform (FRWLock size differs across libstdc++/bionic/Apple).
    //
    // Index decomposition:
    //   block  = ComparisonIndex >> BlockOffsetBits  (default 16 bits → 64K offsets/block)
    //   offset = ComparisonIndex &  BlockOffsetMask
    //   entry  = Blocks[block] + offset * Stride
    //
    // Stride is the alignment quantum within a block (FName entries are
    // 2-byte aligned by default).
    inline int32_t  BlocksOffset       = 0x40;     // void** Blocks (auto-detected)
    inline int32_t  Stride             = 2;        // entry alignment
    inline uint32_t BlockOffsetBits    = 16;       // → 0x10000 entries per block
    inline uint32_t BlockOffsetMask    = 0xFFFF;   // (1<<BlockOffsetBits)-1
}

// Auto-detected non-reflected offsets. These fields exist in UE source but
// are not exposed via UPROPERTY, so the dumper can't find them through
// reflection. Instead they're detected by scanning object memory at runtime.
namespace AutoDetected {
    // ULevel::Actors — TArray<AActor*> inside ULevel. Not reflected.
    // Detected by scanning ULevel memory for a TArray whose elements are
    // valid AActor pointers. -1 means not detected.
    inline int32_t ULevelActors = -1;
}

}  // namespace Off

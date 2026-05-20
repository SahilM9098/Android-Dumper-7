// =============================================================================
// Engine/StructOffsetFinder.h
// -----------------------------------------------------------------------------
// Locates byte offsets of fields inside the UE reflection types we need to
// walk: UObject, UField, UStruct, UClass, UFunction, UProperty / FProperty.
//
// Approach is pattern-scoring, not symbolic. For every candidate offset we
// score how often a field there satisfies the pattern expected for that role
// across a sample of live objects:
//
//   * UObject::NamePrivate   — read 8 bytes as FName, ComparisonIndex must
//                               decode to a non-empty FName via NameArray.
//   * UObject::ClassPrivate  — read 8-byte pointer; *ptr (vtable) must lie
//                               in an executable segment (real UObject).
//   * UObject::OuterPrivate  — pointer is null OR points to a UObject (same
//                               vtable test). Most game objects have an
//                               Outer; ~70% match rate is the sweet spot.
//   * UObject::InternalIndex — int32 equals the object's known GObjects
//                               index. Validation only — value already
//                               carried in FUObjectItem.
//
// Scoring lets the finder survive engine drift: as long as the layout
// preserves the *invariants*, the offset is found.
//
// All field reads go through SafeReadAny under ScopedSigSegvGuard, so
// reaching past the end of a small object never crashes the host.
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#define INTERNAL __attribute__((visibility("hidden")))

namespace StructOffsetFinder {

struct FieldHit {
    int32_t offset = -1;     // -1 = not found
    int32_t score  = 0;      // matches across the sample
    int32_t total  = 0;      // sample size that produced a definitive answer
};

struct UObjectLayout {
    bool      ok            = false;
    int32_t   sampleSize    = 0;     // live objects scanned
    FieldHit  vtable;                // always 0x00 — included for symmetry
    FieldHit  flags;                 // UObject::ObjectFlags (uint32)
    FieldHit  internalIndex;
    FieldHit  classPrivate;
    FieldHit  namePrivate;
    FieldHit  outerPrivate;
    int32_t   inferredSize  = 0;     // best guess at sizeof(UObject)
    uint64_t  elapsedMicros = 0;
    std::vector<std::string> traceLines;
};

// Walk the first `sampleSize` live UObjects and score every 4-byte offset
// 0x00..0x60 against each role's pattern. ObjectArray + NameArray must be
// initialized; otherwise the result reports `ok = false`. `sampleSize` 0 →
// pick a sensible default (256).
INTERNAL UObjectLayout FindUObjectLayout(int32_t sampleSize = 0) noexcept;

// Push a found layout into Off::UObject (overwrites prior values).
INTERNAL void Apply(const UObjectLayout& layout) noexcept;

// =============================================================================
// UField + UStruct layout
//
// Pattern strategy (uses already-resolved Off::UObject offsets as anchors):
//
//   * UField::Next            — chain pointer between sibling fields. For each
//                                candidate offset O after the UObject base,
//                                walk Children->Next chains via O and score
//                                % of chains that terminate cleanly at null.
//   * UStruct::SuperStruct    — pointer to parent UStruct. For UClass samples,
//                                the value at this offset is null (UObject's
//                                top of chain) OR points to another *UClass*
//                                (whose ClassPrivate->Name is "Class").
//   * UStruct::Children       — first member of the linked list. Pointer is
//                                null OR points to a UObject. Distinguishable
//                                from SuperStruct because Children's target
//                                is a UField subclass that is *not* a UClass
//                                (it's UFunction, UProperty, UScriptStruct…).
// =============================================================================

struct UStructLayout {
    bool      ok               = false;
    int32_t   sampleSize       = 0;     // UClass samples scanned
    FieldHit  fieldNext;                // UField::Next (offset within UField)
    FieldHit  superStruct;              // UStruct::SuperStruct
    FieldHit  children;                 // UStruct::Children
    int32_t   inferredFieldSize  = 0;   // sizeof(UField)
    int32_t   inferredStructMin  = 0;   // minimum sizeof(UStruct)
    uint64_t  elapsedMicros      = 0;
    std::vector<std::string> traceLines;
};

INTERNAL UStructLayout FindUStructLayout(int32_t sampleSize = 0) noexcept;
INTERNAL void Apply(const UStructLayout& layout) noexcept;

// =============================================================================
// UClass + UFunction + FProperty layout (the "advanced" pass)
//
// Uses UClass and UFunction samples (filtered by class name "Class" /
// "Function"), plus already-resolved Off::UStruct fields:
//
//   * UClass::ClassDefaultObject — UObject pointer whose name starts with
//                                   "Default__" (CDO naming convention).
//                                   Strong, near-unique signature.
//   * UStruct::ChildProperties   — pointer that's null OR a heap pointer
//                                   with vtable in exec; targets are not
//                                   UClass (FField, lives in libUE4).
//                                   Sits right after Children in canonical
//                                   layout (UE4.25+).
//   * UStruct::PropertiesSize    — int32 holding the per-instance size.
//                                   For the UObject UClass this equals
//                                   Off::UObject::Size — a strong anchor.
//   * UFunction::Func            — code pointer; value lies in an
//                                   executable segment.
//
// Other UClass / UFunction fields (ClassFlags, FunctionFlags, NumParms) keep
// canonical UE4.27 defaults. They're harder to pattern-test reliably and
// not load-bearing for SDK dumping.
// =============================================================================

struct AdvancedLayout {
    bool      ok                 = false;
    int32_t   uclassSamples      = 0;
    int32_t   uscriptStructSamples = 0;
    int32_t   ufuncSamples       = 0;
    int32_t   fpropSamples       = 0;
    int32_t   upropSamples       = 0;

    FieldHit  classDefaultObject;     // UClass::ClassDefaultObject
    FieldHit  childProperties;        // UStruct::ChildProperties
    FieldHit  propertiesSize;         // UStruct::PropertiesSize
    FieldHit  funcFunc;               // UFunction::Func

    FieldHit  fFieldClassPrivate;     // FField::ClassPrivate
    FieldHit  fFieldNext;             // FField::Next
    FieldHit  fFieldNamePrivate;      // FField::NamePrivate
    FieldHit  fFieldClassName;        // FFieldClass::Name
    FieldHit  fPropArrayDim;          // FProperty::ArrayDim
    FieldHit  fPropElementSize;       // FProperty::ElementSize
    FieldHit  fPropOffsetInternal;    // FProperty::Offset_Internal
    FieldHit  fPropSubtypeBase;       // sizeof(FProperty); subclass data starts here

    FieldHit  uPropArrayDim;          // UProperty::ArrayDim
    FieldHit  uPropElementSize;       // UProperty::ElementSize
    FieldHit  uPropOffsetInternal;    // UProperty::Offset_Internal
    FieldHit  uPropSubtypeBase;       // sizeof(UProperty); subclass data starts here

    uint64_t  elapsedMicros      = 0;
    std::vector<std::string> traceLines;
};

INTERNAL AdvancedLayout FindAdvancedLayout(int32_t sampleSize = 0) noexcept;
INTERNAL void Apply(const AdvancedLayout& layout) noexcept;

}  // namespace StructOffsetFinder

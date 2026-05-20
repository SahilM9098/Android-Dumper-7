// =============================================================================
// Engine/StructOffsetDiag.cpp
// =============================================================================

#include "StructOffsetDiag.h"

#include "NameArray.h"
#include "ObjectArray.h"
#include "StructOffsetFinder.h"
#include "UEStructs.h"
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace StructOffsetDiag {

namespace {

struct PanelState {
    int  sampleSize = 256;

    StructOffsetFinder::UObjectLayout uobjLayout;
    bool ranOnce = false;

    StructOffsetFinder::UStructLayout ustructLayout;
    bool ustructRan = false;

    StructOffsetFinder::AdvancedLayout advLayout;
    bool advRan = false;

    std::string status =
            "Run ObjectArray + NameArray first, then tap UObject to scan.";
};

INTERNAL PanelState& State() noexcept {
    static PanelState s;
    return s;
}

INTERNAL void DoUObject(PanelState& ps) noexcept {
    ps.uobjLayout = StructOffsetFinder::FindUObjectLayout(ps.sampleSize);
    ps.ranOnce = true;

    char buf[224];
    std::snprintf(buf, sizeof(buf),
                  "UObject scan: sample=%d  Class=0x%x  Name=0x%x  Outer=0x%x  size~=0x%x  ok=%s  (%.1f ms)",
                  ps.uobjLayout.sampleSize,
                  ps.uobjLayout.classPrivate.offset,
                  ps.uobjLayout.namePrivate.offset,
                  ps.uobjLayout.outerPrivate.offset,
                  ps.uobjLayout.inferredSize,
                  ps.uobjLayout.ok ? "yes" : "no",
                  ps.uobjLayout.elapsedMicros / 1000.0);
    ps.status = buf;
}

INTERNAL void DoApplyUObject(PanelState& ps) noexcept {
    StructOffsetFinder::Apply(ps.uobjLayout);
    ps.status = "Applied to Off::UObject — subsequent reads use the new offsets.";
}

INTERNAL void DoUStruct(PanelState& ps) noexcept {
    ps.ustructLayout = StructOffsetFinder::FindUStructLayout(ps.sampleSize);
    ps.ustructRan = true;

    char buf[224];
    std::snprintf(buf, sizeof(buf),
                  "UStruct scan: classes=%d  Super=0x%x  Children=0x%x  Next=0x%x  ok=%s  (%.1f ms)",
                  ps.ustructLayout.sampleSize,
                  ps.ustructLayout.superStruct.offset,
                  ps.ustructLayout.children.offset,
                  ps.ustructLayout.fieldNext.offset,
                  ps.ustructLayout.ok ? "yes" : "no",
                  ps.ustructLayout.elapsedMicros / 1000.0);
    ps.status = buf;
}

INTERNAL void DoApplyUStruct(PanelState& ps) noexcept {
    StructOffsetFinder::Apply(ps.ustructLayout);
    ps.status = "Applied to Off::UField + Off::UStruct.";
}

INTERNAL void DoAdvanced(PanelState& ps) noexcept {
    ps.advLayout = StructOffsetFinder::FindAdvancedLayout(ps.sampleSize);
    ps.advRan = true;

    char buf[224];
    std::snprintf(buf, sizeof(buf),
                  "Advanced: classes=%d structs=%d funcs=%d  FProps=%d UProps=%d  ok=%s  (%.1f ms)",
                  ps.advLayout.uclassSamples,
                  ps.advLayout.uscriptStructSamples,
                  ps.advLayout.ufuncSamples,
                  ps.advLayout.fpropSamples,
                  ps.advLayout.upropSamples,
                  ps.advLayout.ok ? "yes" : "no",
                  ps.advLayout.elapsedMicros / 1000.0);
    ps.status = buf;
}

INTERNAL void DoApplyAdvanced(PanelState& ps) noexcept {
    StructOffsetFinder::Apply(ps.advLayout);
    ps.status = "Applied UClass / UStruct extras / UFunction offsets.";
}

INTERNAL void RenderField(const char* label,
                          const StructOffsetFinder::FieldHit& h,
                          int32_t currentOff) noexcept {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    if (h.offset >= 0) ImGui::Text("0x%02x", h.offset);
    else               ImGui::TextDisabled("?");
    ImGui::TableSetColumnIndex(2); ImGui::Text("%d / %d", h.score, h.total);
    ImGui::TableSetColumnIndex(3);
    int32_t pct = h.total > 0 ? (h.score * 100 / h.total) : 0;
    ImVec4 c = pct >= 80 ? ImVec4(0.40f, 0.85f, 0.45f, 1.0f)
             : pct >= 50 ? ImVec4(0.85f, 0.75f, 0.40f, 1.0f)
                         : ImVec4(0.85f, 0.45f, 0.45f, 1.0f);
    ImGui::TextColored(c, "%d%%", pct);
    ImGui::TableSetColumnIndex(4); ImGui::Text("0x%02x", currentOff);
    ImGui::TableSetColumnIndex(5);
    if (h.offset == currentOff) ImGui::TextDisabled("=");
    else if (h.offset >= 0)     ImGui::TextColored(ImVec4(0.95f,0.65f,0.30f,1), "diff");
    else                        ImGui::TextDisabled("—");
}

}  // namespace

void RenderContent() noexcept {
    PanelState& ps = State();

    bool oaOk = UE::ObjectArray::IsInitialized();
    bool naOk = UE::NameArray::IsInitialized();

    ImGui::Text("ObjectArray: %s    NameArray: %s",
                oaOk ? "ready" : "NOT ready",
                naOk ? "ready" : "NOT ready");
    if (!oaOk || !naOk) {
        ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f),
                           "Both must be initialized before scanning struct offsets.");
    }

    ImGui::Separator();

    ImGui::TextDisabled("UObject layout:");
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderInt("sample size", &ps.sampleSize, 32, 1024);
    ImGui::SameLine();
    if (ImGui::Button("Scan UObject")) DoUObject(ps);
    ImGui::SameLine();
    if (ImGui::Button("Apply to Off::"))  DoApplyUObject(ps);

    ImGui::Separator();
    ImVec4 statColor = ps.ranOnce && ps.uobjLayout.ok
            ? ImVec4(0.40f, 0.85f, 0.45f, 1.0f)
            : ImVec4(0.85f, 0.75f, 0.40f, 1.0f);
    ImGui::TextColored(statColor, "%s", ps.status.c_str());

    if (ps.ranOnce) {
        if (ImGui::BeginTable("uobjLayout", 6,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX,
                ImVec2(0.0f, 0.0f))) {
            ImGui::TableSetupColumn("field",   ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("found@",  ImGuiTableColumnFlags_WidthFixed,  70.0f);
            ImGui::TableSetupColumn("score",   ImGuiTableColumnFlags_WidthFixed,  90.0f);
            ImGui::TableSetupColumn("%",       ImGuiTableColumnFlags_WidthFixed,  60.0f);
            ImGui::TableSetupColumn("Off::",   ImGuiTableColumnFlags_WidthFixed,  70.0f);
            ImGui::TableSetupColumn("status",  ImGuiTableColumnFlags_WidthFixed,  70.0f);
            ImGui::TableHeadersRow();
            const auto& l = ps.uobjLayout;
            RenderField("VTable",        l.vtable,        Off::UObject::VTable);
            RenderField("ObjectFlags",   l.flags,         Off::UObject::ObjectFlags);
            RenderField("InternalIndex", l.internalIndex, Off::UObject::InternalIndex);
            RenderField("ClassPrivate",  l.classPrivate,  Off::UObject::ClassPrivate);
            RenderField("NamePrivate",   l.namePrivate,   Off::UObject::NamePrivate);
            RenderField("OuterPrivate",  l.outerPrivate,  Off::UObject::OuterPrivate);
            ImGui::EndTable();
        }

        ImGui::Text("inferred sizeof(UObject) ~ 0x%02x  (currently 0x%02x)",
                    ps.uobjLayout.inferredSize, Off::UObject::Size);

        if (!ps.uobjLayout.traceLines.empty()) {
            if (ImGui::CollapsingHeader("UObject scan trace",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::BeginChild("uobjTrace", ImVec2(0, 140), true,
                                  ImGuiWindowFlags_HorizontalScrollbar);
                for (const auto& line : ps.uobjLayout.traceLines) {
                    ImGui::TextUnformatted(line.c_str());
                }
                ImGui::EndChild();
            }
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("UStruct / UField layout:");
    ImGui::TextUnformatted("Walks UClass instances + Children->Next chains.");
    if (ImGui::Button("Scan UStruct")) DoUStruct(ps);
    ImGui::SameLine();
    if (ImGui::Button("Apply Off:: UStruct")) DoApplyUStruct(ps);

    if (ps.ustructRan) {
        if (ImGui::BeginTable("ustructLayout", 6,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX,
                ImVec2(0.0f, 0.0f))) {
            ImGui::TableSetupColumn("field",   ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("found@",  ImGuiTableColumnFlags_WidthFixed,  70.0f);
            ImGui::TableSetupColumn("score",   ImGuiTableColumnFlags_WidthFixed,  90.0f);
            ImGui::TableSetupColumn("%",       ImGuiTableColumnFlags_WidthFixed,  60.0f);
            ImGui::TableSetupColumn("Off::",   ImGuiTableColumnFlags_WidthFixed,  70.0f);
            ImGui::TableSetupColumn("status",  ImGuiTableColumnFlags_WidthFixed,  70.0f);
            ImGui::TableHeadersRow();
            const auto& l = ps.ustructLayout;
            RenderField("UField::Next",    l.fieldNext,   Off::UField::Next);
            RenderField("Super",           l.superStruct, Off::UStruct::SuperStruct);
            RenderField("Children",        l.children,    Off::UStruct::Children);
            ImGui::EndTable();
        }

        ImGui::Text("inferred sizeof(UField) ~ 0x%02x  (currently 0x%02x)",
                    ps.ustructLayout.inferredFieldSize, Off::UField::Size);
        ImGui::Text("min sizeof(UStruct) ~ 0x%02x  (Children +8)",
                    ps.ustructLayout.inferredStructMin);

        if (!ps.ustructLayout.traceLines.empty()) {
            if (ImGui::CollapsingHeader("UStruct scan trace",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::BeginChild("ustructTrace", ImVec2(0, 140), true,
                                  ImGuiWindowFlags_HorizontalScrollbar);
                for (const auto& line : ps.ustructLayout.traceLines) {
                    ImGui::TextUnformatted(line.c_str());
                }
                ImGui::EndChild();
            }
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("UClass / UFunction / UStruct extras:");
    ImGui::TextUnformatted("Filters samples by class name. CDO scan is name-decode heavy.");
    if (ImGui::Button("Scan Advanced")) DoAdvanced(ps);
    ImGui::SameLine();
    if (ImGui::Button("Apply Off:: Advanced")) DoApplyAdvanced(ps);

    if (ps.advRan) {
        ImGui::Text("samples: UClass=%d  UScriptStruct=%d  UFunction=%d  FProp=%d  UProp=%d",
                    ps.advLayout.uclassSamples,
                    ps.advLayout.uscriptStructSamples,
                    ps.advLayout.ufuncSamples,
                    ps.advLayout.fpropSamples,
                    ps.advLayout.upropSamples);
        if (ImGui::BeginTable("advLayout", 6,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX,
                ImVec2(0.0f, 0.0f))) {
            ImGui::TableSetupColumn("field",   ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("found@",  ImGuiTableColumnFlags_WidthFixed,  70.0f);
            ImGui::TableSetupColumn("score",   ImGuiTableColumnFlags_WidthFixed,  90.0f);
            ImGui::TableSetupColumn("%",       ImGuiTableColumnFlags_WidthFixed,  60.0f);
            ImGui::TableSetupColumn("Off::",   ImGuiTableColumnFlags_WidthFixed,  70.0f);
            ImGui::TableSetupColumn("status",  ImGuiTableColumnFlags_WidthFixed,  70.0f);
            ImGui::TableHeadersRow();
            const auto& l = ps.advLayout;
            RenderField("UClass::CDO",      l.classDefaultObject, Off::UClass::ClassDefaultObject);
            RenderField("UStruct::ChildProps", l.childProperties, Off::UStruct::ChildProperties);
            RenderField("UStruct::PropsSize",  l.propertiesSize,  Off::UStruct::PropertiesSize);
            RenderField("UFunction::Func",     l.funcFunc,        Off::UFunction::Func);
            RenderField("FField::Class",       l.fFieldClassPrivate, Off::FField::ClassPrivate);
            RenderField("FField::Next",        l.fFieldNext,         Off::FField::Next);
            RenderField("FField::Name",        l.fFieldNamePrivate,  Off::FField::NamePrivate);
            RenderField("FFieldClass::Name",   l.fFieldClassName,    Off::FFieldClass::Name);
            RenderField("FProp::ArrayDim",     l.fPropArrayDim,      Off::FProperty::ArrayDim);
            RenderField("FProp::ElemSize",     l.fPropElementSize,   Off::FProperty::ElementSize);
            RenderField("FProp::Offset",       l.fPropOffsetInternal, Off::FProperty::Offset_Internal);
            RenderField("FProp::Subtype",      l.fPropSubtypeBase,    Off::FProperty::Size);
            RenderField("UProp::ArrayDim",     l.uPropArrayDim,      Off::UProperty::ArrayDim);
            RenderField("UProp::ElemSize",     l.uPropElementSize,   Off::UProperty::ElementSize);
            RenderField("UProp::Offset",       l.uPropOffsetInternal, Off::UProperty::Offset_Internal);
            RenderField("UProp::Subtype",      l.uPropSubtypeBase,    Off::UProperty::Size);
            ImGui::EndTable();
        }

        if (!ps.advLayout.traceLines.empty()) {
            if (ImGui::CollapsingHeader("Advanced scan trace",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::BeginChild("advTrace", ImVec2(0, 160), true,
                                  ImGuiWindowFlags_HorizontalScrollbar);
                for (const auto& line : ps.advLayout.traceLines) {
                    ImGui::TextUnformatted(line.c_str());
                }
                ImGui::EndChild();
            }
        }
    }
}

}  // namespace StructOffsetDiag

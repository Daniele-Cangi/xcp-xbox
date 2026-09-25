#include "pch.h"
#include "WorkerXvmVerifier.h"

#include "WorkerGraphResourceLedger.h"
#include "WorkerXvmFuelAnalysis.h"
#include "WorkerXvmIsa.h"

#include <functional>

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
        struct XvmLoopRange
        {
            uint32_t beginPc = 0;
            uint32_t endPc = 0;
            size_t regionIndex = 0;
        };

        struct XvmCodeRegion
        {
            uint32_t beginPc = 0;
            uint32_t endPc = 0;
            bool main = false;
        };

        struct XvmConditionalFrame
        {
            uint32_t ifPc = 0;
            uint32_t elsePc = 0;
            uint32_t endPc = 0;
            size_t loopDepth = 0;
            size_t regionIndex = 0;
        };

        struct XvmControlOwnership
        {
            size_t regionIndex = 0;
            std::vector<uint32_t> loopBeginPcs;
            std::vector<uint32_t> conditionalIfPcs;
        };

        struct XvmRecoveryBranch
        {
            uint32_t sourcePc = 0;
            uint32_t targetPc = 0;
        };

        bool SameControlOwnership(XvmControlOwnership const& left, XvmControlOwnership const& right)
        {
            return left.regionIndex == right.regionIndex &&
                left.loopBeginPcs == right.loopBeginPcs &&
                left.conditionalIfPcs == right.conditionalIfPcs;
        }

        bool IsSafeId(std::wstring const& value)
        {
            if (value.empty() || value.size() > 64)
            {
                return false;
            }
            return std::all_of(value.begin(), value.end(), [](wchar_t ch)
            {
                return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
                    (ch >= L'0' && ch <= L'9') || ch == L'_' || ch == L'-';
            });
        }

        std::wstring ReadRequiredString(
            JsonObject const& object,
            wchar_t const* name,
            char const* code)
        {
            if (!object.HasKey(name) || object.GetNamedValue(name).ValueType() != JsonValueType::String)
            {
                throw WorkerXvmError(code, "required typed-view string is missing or invalid");
            }
            return std::wstring(object.GetNamedString(name).c_str());
        }

        uint64_t ReadTypedViewInteger(
            JsonObject const& object,
            wchar_t const* name,
            uint64_t maximum)
        {
            if (!object.HasKey(name) || object.GetNamedValue(name).ValueType() != JsonValueType::Number)
            {
                throw WorkerXvmError("xvm.typed_view_shape_invalid", "typed-view offsets and lengths must be exact integers");
            }
            auto raw = object.GetNamedNumber(name);
            if (!std::isfinite(raw) || raw != std::floor(raw) || raw < 0 || raw > static_cast<double>(maximum))
            {
                throw WorkerXvmError("xvm.typed_view_bounds_invalid", "typed-view offset or length is outside its admitted address space");
            }
            return static_cast<uint64_t>(raw);
        }

        WorkerXvmViewAccess ParseViewAccess(
            std::wstring const& value,
            bool allowRead,
            bool allowWrite,
            bool allowReadWrite)
        {
            if (value == L"read" && allowRead)
            {
                return WorkerXvmViewAccess::Read;
            }
            if (value == L"write" && allowWrite)
            {
                return WorkerXvmViewAccess::Write;
            }
            if (value == L"read_write" && allowReadWrite)
            {
                return WorkerXvmViewAccess::ReadWrite;
            }
            throw WorkerXvmError("xvm.typed_view_access_invalid", "typed-view access is not admitted for its address space");
        }

        std::vector<WorkerXvmTypedView> ParseTypedViews(
            JsonObject const& object,
            wchar_t const* field,
            uint64_t addressSpaceBytes,
            bool allowRead,
            bool allowWrite,
            bool allowReadWrite,
            std::vector<std::wstring>& allViewIds)
        {
            if (!object.HasKey(field) || object.GetNamedValue(field).ValueType() != JsonValueType::Array)
            {
                throw WorkerXvmError("xvm.typed_views_required", "typed_memory_v1 requires input_views, memory_views and output_views arrays");
            }
            auto array = object.GetNamedArray(field);
            if (array.Size() == 0 || array.Size() > 64)
            {
                throw WorkerXvmError("xvm.typed_view_count_invalid", "each typed-view address space requires 1..64 views");
            }

            std::vector<WorkerXvmTypedView> views;
            views.reserve(array.Size());
            for (uint32_t index = 0; index < array.Size(); ++index)
            {
                auto value = array.GetAt(index);
                if (value.ValueType() != JsonValueType::Object)
                {
                    throw WorkerXvmError("xvm.typed_view_shape_invalid", "typed-view entries must be objects");
                }
                auto viewObject = value.GetObject();
                if (viewObject.Size() != 5 ||
                    !viewObject.HasKey(L"view_id") ||
                    !viewObject.HasKey(L"element_type") ||
                    !viewObject.HasKey(L"access") ||
                    !viewObject.HasKey(L"offset_bytes") ||
                    !viewObject.HasKey(L"length_bytes"))
                {
                    throw WorkerXvmError("xvm.typed_view_shape_invalid", "typed-view entries must use the exact v1 field set");
                }

                WorkerXvmTypedView view;
                view.viewId = ReadRequiredString(viewObject, L"view_id", "xvm.typed_view_id_invalid");
                view.elementType = ReadRequiredString(viewObject, L"element_type", "xvm.typed_view_element_invalid");
                auto access = ReadRequiredString(viewObject, L"access", "xvm.typed_view_access_invalid");
                if (!IsSafeId(view.viewId) ||
                    std::find(allViewIds.begin(), allViewIds.end(), view.viewId) != allViewIds.end())
                {
                    throw WorkerXvmError("xvm.typed_view_id_invalid", "typed-view ids must be safe and globally unique");
                }
                if (view.elementType != L"u32")
                {
                    throw WorkerXvmError("xvm.typed_view_element_invalid", "typed_memory_v1 admits only little-endian u32 elements");
                }
                view.access = ParseViewAccess(access, allowRead, allowWrite, allowReadWrite);
                view.offsetBytes = ReadTypedViewInteger(viewObject, L"offset_bytes", addressSpaceBytes);
                view.lengthBytes = ReadTypedViewInteger(viewObject, L"length_bytes", addressSpaceBytes);
                if (view.lengthBytes == 0 ||
                    (view.offsetBytes % 4) != 0 ||
                    (view.lengthBytes % 4) != 0 ||
                    view.offsetBytes > addressSpaceBytes - view.lengthBytes)
                {
                    throw WorkerXvmError("xvm.typed_view_bounds_invalid", "typed-view bounds must be non-empty, u32-aligned and inside their address space");
                }
                for (auto const& existing : views)
                {
                    if (view.offsetBytes < existing.offsetBytes + existing.lengthBytes &&
                        existing.offsetBytes < view.offsetBytes + view.lengthBytes)
                    {
                        throw WorkerXvmError("xvm.typed_view_overlap", "typed views in one address space may not overlap");
                    }
                }
                allViewIds.push_back(view.viewId);
                views.push_back(std::move(view));
            }
            return views;
        }

        bool ViewAdmitsAccess(
            std::vector<WorkerXvmTypedView> const& views,
            uint64_t offsetBytes,
            WorkerXvmViewAccess access)
        {
            return std::any_of(views.begin(), views.end(), [&](WorkerXvmTypedView const& view)
            {
                auto accessAllowed = view.access == WorkerXvmViewAccess::ReadWrite || view.access == access;
                return accessAllowed &&
                    offsetBytes >= view.offsetBytes &&
                    offsetBytes + 4 <= view.offsetBytes + view.lengthBytes;
            });
        }

        uint64_t ReadBoundedInteger(
            JsonObject const& object,
            wchar_t const* name,
            uint64_t minimum,
            uint64_t maximum,
            char const* code)
        {
            if (!object.HasKey(name))
            {
                throw WorkerXvmError(code, "required bounded integer is missing");
            }
            auto raw = object.GetNamedNumber(name);
            if (!std::isfinite(raw) || raw != std::floor(raw) || raw < static_cast<double>(minimum) || raw > static_cast<double>(maximum))
            {
                throw WorkerXvmError(code, "bounded integer is outside the admitted range");
            }
            return static_cast<uint64_t>(raw);
        }

        uint32_t ReadWord(IJsonValue const& value)
        {
            if (value.ValueType() != JsonValueType::Number)
            {
                throw WorkerXvmError("xvm.program_word_invalid", "bytecode_words entries must be unsigned 32-bit integers");
            }
            auto raw = value.GetNumber();
            if (!std::isfinite(raw) || raw != std::floor(raw) || raw < 0 || raw > static_cast<double>(UINT32_MAX))
            {
                throw WorkerXvmError("xvm.program_word_invalid", "bytecode word is outside the unsigned 32-bit range");
            }
            return static_cast<uint32_t>(raw);
        }

        uint32_t InstructionWord(WorkerXvmProgram const& program, uint32_t pc, uint32_t word)
        {
            return program.words[(static_cast<size_t>(pc) * WorkerXvmInstructionWords()) + word];
        }

        void RequireRegister(uint32_t value, std::wstring const& isaVersion)
        {
            if (value >= WorkerXvmRegisterCount())
            {
                throw WorkerXvmError("xvm.register_invalid", "register index exceeds the " + winrt::to_string(hstring(isaVersion)) + " register file");
            }
        }

        std::vector<XvmCodeRegion> BuildRegions(WorkerXvmProgram const& program, uint32_t instructionCount)
        {
            if (program.isaVersion == WorkerXvmV1Spec().isaVersion)
            {
                if (!program.functions.empty() || program.maxCallDepth != 0)
                {
                    throw WorkerXvmError("xvm.function_table_invalid", "xvm-v1 does not admit a function table or call depth");
                }
                return { { 0, instructionCount - 1, true } };
            }

            if (program.functions.empty())
            {
                throw WorkerXvmError("xvm.functions_required", "xvm-v2 requires at least one declared function region");
            }
            if (program.maxCallDepth == 0 || program.maxCallDepth > WorkerXvmMaxCallDepth())
            {
                throw WorkerXvmError("xvm.call_depth_invalid", "xvm-v2 max_call_depth is outside the admitted range");
            }

            std::vector<XvmCodeRegion> regions;
            uint32_t expectedEntry = 0;
            for (size_t index = 0; index < program.functions.size(); ++index)
            {
                auto const& function = program.functions[index];
                if (index == 0)
                {
                    if (function.entryPc == 0)
                    {
                        throw WorkerXvmError("xvm.function_region_invalid", "xvm-v2 main region must begin at pc 0 before function regions");
                    }
                    regions.push_back({ 0, function.entryPc - 1, true });
                    expectedEntry = function.entryPc;
                }
                if (function.entryPc != expectedEntry || function.entryPc > function.endPc || function.endPc >= instructionCount)
                {
                    throw WorkerXvmError("xvm.function_region_invalid", "xvm-v2 function regions must be ordered, contiguous and inside bytecode");
                }
                regions.push_back({ function.entryPc, function.endPc, false });
                expectedEntry = function.endPc + 1;
            }
            if (expectedEntry != instructionCount)
            {
                throw WorkerXvmError("xvm.function_region_invalid", "xvm-v2 function regions must cover all bytecode after main");
            }
            return regions;
        }

        size_t RegionForPc(std::vector<XvmCodeRegion> const& regions, uint32_t pc)
        {
            for (size_t index = 0; index < regions.size(); ++index)
            {
                if (pc >= regions[index].beginPc && pc <= regions[index].endPc)
                {
                    return index;
                }
            }
            throw WorkerXvmError("xvm.function_region_invalid", "instruction is not owned by an admitted code region");
        }

        size_t FunctionRegionForEntry(std::vector<XvmCodeRegion> const& regions, uint32_t entryPc)
        {
            for (size_t index = 1; index < regions.size(); ++index)
            {
                if (regions[index].beginPc == entryPc)
                {
                    return index;
                }
            }
            throw WorkerXvmError("xvm.call_target_invalid", "call target must be a declared function entry");
        }

        void VerifyCallGraph(
            WorkerXvmProgram const& program,
            std::vector<XvmCodeRegion> const& regions,
            std::vector<std::vector<size_t>> const& callEdges)
        {
            if (program.isaVersion == WorkerXvmV1Spec().isaVersion)
            {
                return;
            }

            std::vector<bool> visiting(regions.size(), false);
            std::vector<bool> reachable(regions.size(), false);
            uint64_t observedDepth = 0;
            std::function<void(size_t, uint64_t)> visit = [&](size_t regionIndex, uint64_t depth)
            {
                if (visiting[regionIndex])
                {
                    throw WorkerXvmError("xvm.call_cycle_invalid", "xvm-v2 call graph must be acyclic");
                }
                visiting[regionIndex] = true;
                reachable[regionIndex] = true;
                if (depth > observedDepth)
                {
                    observedDepth = depth;
                }
                for (auto target : callEdges[regionIndex])
                {
                    visit(target, depth + 1);
                }
                visiting[regionIndex] = false;
            };
            visit(0, 0);

            if (observedDepth > program.maxCallDepth)
            {
                throw WorkerXvmError("xvm.call_depth_exceeded", "declared max_call_depth is smaller than the verified acyclic call graph");
            }
            for (size_t index = 1; index < reachable.size(); ++index)
            {
                if (!reachable[index])
                {
                    throw WorkerXvmError("xvm.function_unreachable", "every xvm-v2 function must be reachable from the main region");
                }
            }
        }
    }

    std::vector<std::wstring> WorkerReadXvmCapabilities(JsonObject const& object, wchar_t const* field, bool required)
    {
        if (!object.HasKey(field))
        {
            if (required)
            {
                throw WorkerXvmError("xvm.capabilities_required", "capabilities array is required");
            }
            return {};
        }
        auto value = object.GetNamedValue(field);
        if (value.ValueType() != JsonValueType::Array)
        {
            throw WorkerXvmError("xvm.capabilities_invalid", "capabilities must be an array");
        }

        std::vector<std::wstring> capabilities;
        auto array = value.GetArray();
        for (uint32_t index = 0; index < array.Size(); ++index)
        {
            auto item = array.GetAt(index);
            if (item.ValueType() != JsonValueType::String)
            {
                throw WorkerXvmError("xvm.capability_invalid", "capability entries must be strings");
            }
            auto capability = std::wstring(item.GetString().c_str());
            if (capability != L"artifact_input" && capability != L"artifact_output" &&
                capability != L"control_output" && capability != L"typed_memory_v1" &&
                capability != L"structured_control_v2" && capability != L"structured_control_v2_phase_b" &&
                capability != L"spmd_lane_context_v1")
            {
                throw WorkerXvmError("xvm.capability_not_admitted", "XVM capability is not admitted by the sandboxed ISA");
            }
            if (std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end())
            {
                throw WorkerXvmError("xvm.capability_duplicate", "XVM capabilities must be unique");
            }
            capabilities.push_back(capability);
        }
        return capabilities;
    }

    bool WorkerXvmHasCapability(std::vector<std::wstring> const& capabilities, wchar_t const* capability)
    {
        return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
    }

    WorkerXvmProgram WorkerParseXvmProgram(std::string const& jsonUtf8)
    {
        JsonObject object;
        try
        {
            object = JsonObject::Parse(winrt::to_hstring(jsonUtf8));
        }
        catch (...)
        {
            throw WorkerXvmError("xvm.program_json_invalid", "program artifact is not valid JSON");
        }

        auto isaVersion = std::wstring(object.GetNamedString(L"isa_version", L"").c_str());
        auto spec = WorkerFindXvmIsaSpec(isaVersion);
        if (spec == nullptr)
        {
            throw WorkerXvmError("xvm.isa_version_invalid", "program artifact isa_version is not admitted");
        }

        auto schema = std::wstring(object.GetNamedString(L"schema_version", L"").c_str());
        auto profile = std::wstring(object.GetNamedString(L"profile", L"").c_str());
        if (schema != spec->programSchemaVersion)
        {
            throw WorkerXvmError("xvm.program_schema_invalid", "program artifact schema_version does not match isa_version");
        }
        if (profile != spec->profile)
        {
            throw WorkerXvmError("xvm.profile_invalid", "program profile does not match the admitted ISA profile");
        }

        WorkerXvmProgram program;
        program.schemaVersion = schema;
        program.isaVersion = isaVersion;
        program.artifactKind = spec->artifactKind;
        program.profile = profile;
        program.computeKind = spec->computeKind;
        program.programId = std::wstring(object.GetNamedString(L"program_id", L"").c_str());
        if (!IsSafeId(program.programId))
        {
            throw WorkerXvmError("xvm.program_id_invalid", "program_id must be a safe 1..64 character id");
        }
        program.memoryBytes = ReadBoundedInteger(object, L"memory_bytes", 4, WorkerGraphMaxXvmMemoryBytes(), "xvm.memory_invalid");
        program.outputBytes = ReadBoundedInteger(object, L"output_bytes", 4, WorkerGraphMaxXvmOutputBytes(), "xvm.output_invalid");
        program.maxFuel = ReadBoundedInteger(object, L"max_fuel", 1, WorkerGraphMaxFuel(), "xvm.fuel_invalid");
        if ((program.memoryBytes % 4) != 0 || (program.outputBytes % 4) != 0)
        {
            throw WorkerXvmError("xvm.alignment_invalid", "memory_bytes and output_bytes must be 4-byte aligned");
        }

        program.capabilities = WorkerReadXvmCapabilities(object, L"capabilities", true);
        for (auto required : { L"artifact_input", L"artifact_output", L"control_output" })
        {
            if (!WorkerXvmHasCapability(program.capabilities, required))
            {
                throw WorkerXvmError("xvm.capability_required", "program artifact must declare artifact_input, artifact_output and control_output");
            }
        }

        auto typedMemory = WorkerXvmHasCapability(program.capabilities, L"typed_memory_v1");
        auto structuredControl = WorkerXvmHasCapability(program.capabilities, L"structured_control_v2");
        auto structuredControlPhaseB = WorkerXvmHasCapability(program.capabilities, L"structured_control_v2_phase_b");
        auto spmdLaneContext = WorkerXvmHasCapability(program.capabilities, L"spmd_lane_context_v1");
        if (structuredControlPhaseB && !structuredControl)
        {
            throw WorkerXvmError("xvm.structured_control_phase_b_dependency_required", "structured_control_v2_phase_b requires structured_control_v2");
        }
        auto hasTypedViewFields = object.HasKey(L"input_views") ||
            object.HasKey(L"memory_views") ||
            object.HasKey(L"output_views");
        if (!typedMemory && hasTypedViewFields)
        {
            throw WorkerXvmError("xvm.typed_memory_capability_required", "typed-view declarations require the typed_memory_v1 capability");
        }
        if (typedMemory)
        {
            if (program.isaVersion != WorkerXvmV2Spec().isaVersion)
            {
                throw WorkerXvmError("xvm.typed_memory_isa_invalid", "typed_memory_v1 is admitted only by xvm-v2");
            }
            std::vector<std::wstring> viewIds;
            program.inputViews = ParseTypedViews(
                object,
                L"input_views",
                static_cast<uint64_t>(WorkerXvmInputWordCount()) * sizeof(uint32_t),
                true,
                false,
                false,
                viewIds);
            program.memoryViews = ParseTypedViews(
                object,
                L"memory_views",
                program.memoryBytes,
                true,
                true,
                true,
                viewIds);
            program.outputViews = ParseTypedViews(
                object,
                L"output_views",
                program.outputBytes,
                false,
                true,
                false,
                viewIds);
        }

        if (structuredControl &&
            program.isaVersion != WorkerXvmV2Spec().isaVersion)
        {
            throw WorkerXvmError("xvm.structured_control_isa_invalid", "structured_control_v2 is admitted only by xvm-v2");
        }
        if (structuredControlPhaseB &&
            program.isaVersion != WorkerXvmV2Spec().isaVersion)
        {
            throw WorkerXvmError("xvm.structured_control_phase_b_isa_invalid", "structured_control_v2_phase_b is admitted only by xvm-v2");
        }
        if (spmdLaneContext && program.isaVersion != WorkerXvmV2Spec().isaVersion)
        {
            throw WorkerXvmError("xvm.spmd_lane_context_isa_invalid", "spmd_lane_context_v1 is admitted only by xvm-v2");
        }

        if (!object.HasKey(L"bytecode_words"))
        {
            throw WorkerXvmError("xvm.bytecode_required", "bytecode_words array is required");
        }
        auto bytecodeValue = object.GetNamedValue(L"bytecode_words");
        if (bytecodeValue.ValueType() != JsonValueType::Array)
        {
            throw WorkerXvmError("xvm.bytecode_invalid", "bytecode_words must be an array");
        }
        auto bytecode = bytecodeValue.GetArray();
        if (bytecode.Size() == 0 || (bytecode.Size() % WorkerXvmInstructionWords()) != 0)
        {
            throw WorkerXvmError("xvm.bytecode_shape_invalid", "XVM instructions require exactly four words each");
        }
        auto instructionCount = bytecode.Size() / WorkerXvmInstructionWords();
        if (instructionCount > WorkerXvmInstructionLimit())
        {
            throw WorkerXvmError("xvm.instruction_limit_exceeded", "program exceeds the XVM instruction limit");
        }
        program.words.reserve(bytecode.Size());
        for (uint32_t index = 0; index < bytecode.Size(); ++index)
        {
            program.words.push_back(ReadWord(bytecode.GetAt(index)));
        }

        if (spec->structuredCalls)
        {
            program.maxCallDepth = ReadBoundedInteger(object, L"max_call_depth", 1, spec->maxCallDepth, "xvm.call_depth_invalid");
            if (!object.HasKey(L"functions") || object.GetNamedValue(L"functions").ValueType() != JsonValueType::Array)
            {
                throw WorkerXvmError("xvm.functions_required", "xvm-v2 functions array is required");
            }
            auto functions = object.GetNamedArray(L"functions");
            if (functions.Size() == 0 || functions.Size() > 64)
            {
                throw WorkerXvmError("xvm.function_count_invalid", "xvm-v2 requires 1..64 declared functions");
            }
            program.functions.reserve(functions.Size());
            for (uint32_t index = 0; index < functions.Size(); ++index)
            {
                auto value = functions.GetAt(index);
                if (value.ValueType() != JsonValueType::Object)
                {
                    throw WorkerXvmError("xvm.function_invalid", "function entries must be objects");
                }
                auto function = value.GetObject();
                program.functions.push_back({
                    static_cast<uint32_t>(ReadBoundedInteger(function, L"entry_pc", 1, instructionCount - 1, "xvm.function_entry_invalid")),
                    static_cast<uint32_t>(ReadBoundedInteger(function, L"end_pc", 1, instructionCount - 1, "xvm.function_end_invalid")),
                });
            }
        }
        else if (object.HasKey(L"functions") || object.HasKey(L"max_call_depth"))
        {
            throw WorkerXvmError("xvm.function_table_invalid", "xvm-v1 does not admit functions or max_call_depth");
        }

        return program;
    }

    WorkerXvmStaticFuelProof WorkerVerifyXvmProgram(WorkerXvmProgram const& program)
    {
        auto spec = WorkerFindXvmIsaSpec(program.isaVersion);
        if (spec == nullptr)
        {
            throw WorkerXvmError("xvm.isa_version_invalid", "verifier received an unknown ISA version");
        }
        auto instructionCount = static_cast<uint32_t>(program.words.size() / WorkerXvmInstructionWords());
        auto regions = BuildRegions(program, instructionCount);
        std::vector<std::vector<size_t>> callEdges(regions.size());
        std::vector<XvmLoopRange> loopRanges;
        std::vector<XvmLoopRange> loopStack;
        std::vector<std::pair<uint32_t, uint32_t>> forwardBranches;
        std::vector<XvmConditionalFrame> conditionalStack;
        std::vector<XvmControlOwnership> ownershipByPc(instructionCount);
        std::vector<XvmRecoveryBranch> recoveryBranches;
        bool hasOutput = false;
        bool hasControl = false;
        auto typedMemory = WorkerXvmHasCapability(program.capabilities, L"typed_memory_v1");
        auto structuredControl = WorkerXvmHasCapability(program.capabilities, L"structured_control_v2");
        auto structuredControlPhaseB = WorkerXvmHasCapability(program.capabilities, L"structured_control_v2_phase_b");

        for (uint32_t pc = 0; pc < instructionCount; ++pc)
        {
            auto regionIndex = RegionForPc(regions, pc);
            auto const& region = regions[regionIndex];
            XvmControlOwnership ownership;
            ownership.regionIndex = regionIndex;
            ownership.loopBeginPcs.reserve(loopStack.size());
            for (auto const& loop : loopStack)
            {
                ownership.loopBeginPcs.push_back(loop.beginPc);
            }
            ownership.conditionalIfPcs.reserve(conditionalStack.size());
            for (auto const& conditional : conditionalStack)
            {
                ownership.conditionalIfPcs.push_back(conditional.ifPc);
            }
            ownershipByPc[pc] = std::move(ownership);
            auto opcode = static_cast<WorkerXvmOpcode>(InstructionWord(program, pc, 0));
            auto a = InstructionWord(program, pc, 1);
            auto b = InstructionWord(program, pc, 2);
            auto c = InstructionWord(program, pc, 3);
            if (!WorkerXvmOpcodeIsAdmitted(*spec, opcode))
            {
                throw WorkerXvmError("xvm.opcode_not_admitted", "program contains an opcode not admitted by its ISA version");
            }

            switch (opcode)
            {
            case WorkerXvmOpcode::Halt:
                if (a != 0 || b != 0 || c != 0 || !region.main || pc != region.endPc)
                {
                    throw WorkerXvmError("xvm.halt_invalid", "halt must terminate the main region and have zero operands");
                }
                break;
            case WorkerXvmOpcode::MoveImmediate:
                RequireRegister(a, program.isaVersion);
                break;
            case WorkerXvmOpcode::LoadInputU32:
                RequireRegister(a, program.isaVersion);
                if (b >= WorkerXvmInputWordCount())
                {
                    throw WorkerXvmError("xvm.input_index_invalid", "input word index exceeds the typed XVM input view");
                }
                if (typedMemory && !ViewAdmitsAccess(program.inputViews, static_cast<uint64_t>(b) * 4, WorkerXvmViewAccess::Read))
                {
                    throw WorkerXvmError("xvm.typed_view_access_invalid", "input load is outside every declared readable u32 view");
                }
                break;
            case WorkerXvmOpcode::AddU32:
            case WorkerXvmOpcode::XorU32:
            case WorkerXvmOpcode::MultiplyU32:
            case WorkerXvmOpcode::EqualU32:
                RequireRegister(a, program.isaVersion);
                RequireRegister(b, program.isaVersion);
                RequireRegister(c, program.isaVersion);
                break;
            case WorkerXvmOpcode::LessThanU32:
            case WorkerXvmOpcode::LessThanS32:
            case WorkerXvmOpcode::SelectU32:
                if (!structuredControlPhaseB)
                {
                    throw WorkerXvmError("xvm.structured_control_phase_b_capability_required", "ordered comparisons and select require structured_control_v2_phase_b");
                }
                RequireRegister(a, program.isaVersion);
                RequireRegister(b, program.isaVersion);
                RequireRegister(c, program.isaVersion);
                break;
            case WorkerXvmOpcode::RotateLeftU32:
                RequireRegister(a, program.isaVersion);
                RequireRegister(b, program.isaVersion);
                if (c >= 32)
                {
                    throw WorkerXvmError("xvm.rotate_invalid", "rotate count must be 0..31");
                }
                break;
            case WorkerXvmOpcode::LoadMemoryU32:
            case WorkerXvmOpcode::StoreMemoryU32:
                RequireRegister(a, program.isaVersion);
                if ((b % 4) != 0 || static_cast<uint64_t>(b) + 4 > program.memoryBytes)
                {
                    throw WorkerXvmError("xvm.memory_access_invalid", "memory access is unaligned or outside declared linear memory");
                }
                if (typedMemory && !ViewAdmitsAccess(
                    program.memoryViews,
                    b,
                    opcode == WorkerXvmOpcode::LoadMemoryU32 ? WorkerXvmViewAccess::Read : WorkerXvmViewAccess::Write))
                {
                    throw WorkerXvmError("xvm.typed_view_access_invalid", "memory access is outside every declared view with matching access");
                }
                break;
            case WorkerXvmOpcode::BranchIfZero:
                RequireRegister(a, program.isaVersion);
                if (structuredControl)
                {
                    throw WorkerXvmError("xvm.structured_control_legacy_branch_invalid", "structured_control_v2 does not admit legacy forward branches");
                }
                if (!loopStack.empty())
                {
                    throw WorkerXvmError("xvm.branch_region_invalid", "forward branches may not cross an active loop region");
                }
                if (b <= pc || b > region.endPc)
                {
                    throw WorkerXvmError("xvm.branch_target_invalid", "branch target must be forward and remain inside its code region");
                }
                forwardBranches.push_back({ pc, b });
                break;
            case WorkerXvmOpcode::LoopBegin:
                if (a == 0 || a > WorkerXvmLoopIterationLimit() || b <= pc || b > region.endPc || c != 0)
                {
                    throw WorkerXvmError("xvm.loop_invalid", "loop bound or end target is outside the admitted range");
                }
                if (InstructionWord(program, b, 0) != static_cast<uint32_t>(WorkerXvmOpcode::LoopEnd) || InstructionWord(program, b, 1) != pc)
                {
                    throw WorkerXvmError("xvm.loop_pair_invalid", "loop begin/end instructions must reference each other");
                }
                loopStack.push_back({ pc, b, regionIndex });
                if (loopStack.size() > WorkerXvmLoopDepthLimit())
                {
                    throw WorkerXvmError("xvm.loop_depth_exceeded", "program exceeds the admitted loop nesting depth");
                }
                break;
            case WorkerXvmOpcode::LoopEnd:
                if (b != 0 || c != 0 || loopStack.empty() || loopStack.back().beginPc != a ||
                    loopStack.back().endPc != pc || loopStack.back().regionIndex != regionIndex)
                {
                    throw WorkerXvmError("xvm.loop_pair_invalid", "loop end does not close the active structured loop");
                }
                if (!conditionalStack.empty() && conditionalStack.back().loopDepth == loopStack.size())
                {
                    throw WorkerXvmError("xvm.structured_control_region_invalid", "structured conditional may not cross its containing loop boundary");
                }
                loopRanges.push_back(loopStack.back());
                loopStack.pop_back();
                break;
            case WorkerXvmOpcode::BreakIfZero:
            case WorkerXvmOpcode::ContinueIfZero:
                if (!structuredControlPhaseB)
                {
                    throw WorkerXvmError("xvm.structured_control_phase_b_capability_required", "loop control requires structured_control_v2_phase_b");
                }
                RequireRegister(a, program.isaVersion);
                if (loopStack.empty() || loopStack.back().beginPc != b || loopStack.back().endPc != c ||
                    loopStack.back().regionIndex != regionIndex)
                {
                    throw WorkerXvmError("xvm.structured_control_loop_owner_invalid", "loop control must name the innermost verified loop");
                }
                break;
            case WorkerXvmOpcode::OutputU32:
                RequireRegister(a, program.isaVersion);
                if ((b % 4) != 0 || static_cast<uint64_t>(b) + 4 > program.outputBytes)
                {
                    throw WorkerXvmError("xvm.output_access_invalid", "output access is unaligned or outside declared output");
                }
                if (typedMemory && !ViewAdmitsAccess(program.outputViews, b, WorkerXvmViewAccess::Write))
                {
                    throw WorkerXvmError("xvm.typed_view_access_invalid", "output store is outside every declared writable u32 view");
                }
                hasOutput = true;
                break;
            case WorkerXvmOpcode::SetControl:
                RequireRegister(a, program.isaVersion);
                hasControl = true;
                break;
            case WorkerXvmOpcode::Call:
            {
                if (!loopStack.empty())
                {
                    throw WorkerXvmError("xvm.call_region_invalid", "calls may not cross an active structured loop");
                }
                if (b != 0 || c != 0)
                {
                    throw WorkerXvmError("xvm.call_invalid", "call reserves only operand a for a function entry pc");
                }
                auto targetRegion = FunctionRegionForEntry(regions, a);
                if (std::find(callEdges[regionIndex].begin(), callEdges[regionIndex].end(), targetRegion) == callEdges[regionIndex].end())
                {
                    callEdges[regionIndex].push_back(targetRegion);
                }
                break;
            }
            case WorkerXvmOpcode::Return:
                if (a != 0 || b != 0 || c != 0 || region.main || pc != region.endPc)
                {
                    throw WorkerXvmError("xvm.return_invalid", "return must terminate a declared function region and have zero operands");
                }
                break;
            case WorkerXvmOpcode::IfZero:
                if (!structuredControl)
                {
                    throw WorkerXvmError("xvm.structured_control_capability_required", "if_zero requires structured_control_v2");
                }
                RequireRegister(a, program.isaVersion);
                if (c <= pc || c > region.endPc ||
                    InstructionWord(program, c, 0) != static_cast<uint32_t>(WorkerXvmOpcode::EndIf) ||
                    InstructionWord(program, c, 1) != 0 || InstructionWord(program, c, 2) != 0 || InstructionWord(program, c, 3) != 0 ||
                    (b != c && (b <= pc || b >= c ||
                        InstructionWord(program, b, 0) != static_cast<uint32_t>(WorkerXvmOpcode::Else) ||
                        InstructionWord(program, b, 1) != c || InstructionWord(program, b, 2) != 0 || InstructionWord(program, b, 3) != 0)))
                {
                    throw WorkerXvmError("xvm.structured_control_target_invalid", "if_zero must reference an exact else/end_if pair inside its code region");
                }
                conditionalStack.push_back({ pc, b, c, loopStack.size(), regionIndex });
                if (conditionalStack.size() > WorkerXvmConditionalDepthLimit())
                {
                    throw WorkerXvmError("xvm.structured_control_depth_exceeded", "structured conditional nesting exceeds admission");
                }
                break;
            case WorkerXvmOpcode::Else:
                if (!structuredControl)
                {
                    throw WorkerXvmError("xvm.structured_control_capability_required", "else requires structured_control_v2");
                }
                if (b != 0 || c != 0 || conditionalStack.empty() ||
                    conditionalStack.back().elsePc != pc || conditionalStack.back().endPc != a ||
                    conditionalStack.back().loopDepth != loopStack.size() || conditionalStack.back().regionIndex != regionIndex)
                {
                    throw WorkerXvmError("xvm.structured_control_pair_invalid", "else does not match the active structured conditional");
                }
                break;
            case WorkerXvmOpcode::EndIf:
                if (!structuredControl)
                {
                    throw WorkerXvmError("xvm.structured_control_capability_required", "end_if requires structured_control_v2");
                }
                if (a != 0 || b != 0 || c != 0 || conditionalStack.empty() ||
                    conditionalStack.back().endPc != pc || conditionalStack.back().loopDepth != loopStack.size() ||
                    conditionalStack.back().regionIndex != regionIndex)
                {
                    throw WorkerXvmError("xvm.structured_control_pair_invalid", "end_if does not close the active structured conditional");
                }
                conditionalStack.pop_back();
                break;
            case WorkerXvmOpcode::TrapIfZero:
                if (!structuredControlPhaseB)
                {
                    throw WorkerXvmError("xvm.structured_control_phase_b_capability_required", "deterministic recovered traps require structured_control_v2_phase_b");
                }
                RequireRegister(a, program.isaVersion);
                if (b == 0 || b > WorkerXvmTrapCodeLimit() || c <= pc || c > region.endPc)
                {
                    throw WorkerXvmError("xvm.structured_control_recovery_invalid", "trap recovery must use a nonzero bounded code and a forward target in its code region");
                }
                recoveryBranches.push_back({ pc, c });
                break;
            default:
                throw WorkerXvmError("xvm.opcode_not_admitted", "program contains an opcode not admitted by its ISA version");
            }
        }

        if (!loopStack.empty())
        {
            throw WorkerXvmError("xvm.loop_unclosed", "program contains an unclosed loop");
        }
        if (!conditionalStack.empty())
        {
            throw WorkerXvmError("xvm.structured_control_unclosed", "program contains an unclosed structured conditional");
        }
        for (auto const& branch : forwardBranches)
        {
            auto regionIndex = RegionForPc(regions, branch.first);
            for (auto const& loop : loopRanges)
            {
                if (loop.regionIndex == regionIndex && branch.second > loop.beginPc && branch.second <= loop.endPc)
                {
                    throw WorkerXvmError("xvm.branch_region_invalid", "forward branch may not enter a structured loop body");
                }
            }
        }
        for (auto const& recovery : recoveryBranches)
        {
            if (!SameControlOwnership(ownershipByPc[recovery.sourcePc], ownershipByPc[recovery.targetPc]))
            {
                throw WorkerXvmError("xvm.structured_control_recovery_invalid", "trap recovery may not cross a loop, conditional, or code-region ownership boundary");
            }
        }
        if (InstructionWord(program, regions[0].endPc, 0) != static_cast<uint32_t>(WorkerXvmOpcode::Halt))
        {
            throw WorkerXvmError("xvm.halt_required", "main program region must end with halt");
        }
        for (size_t index = 1; index < regions.size(); ++index)
        {
            if (InstructionWord(program, regions[index].endPc, 0) != static_cast<uint32_t>(WorkerXvmOpcode::Return))
            {
                throw WorkerXvmError("xvm.return_required", "every xvm-v2 function region must end with return");
            }
        }
        if (!hasOutput || !hasControl)
        {
            throw WorkerXvmError("xvm.result_contract_invalid", "program must write bounded output and a control token");
        }
        VerifyCallGraph(program, regions, callEdges);
        return WorkerAnalyzeXvmStaticFuel(program);
    }

    WorkerXvmProgram WorkerParseAndVerifyXvmProgram(std::string const& jsonUtf8)
    {
        auto program = WorkerParseXvmProgram(jsonUtf8);
        auto proof = WorkerVerifyXvmProgram(program);
        program.staticWorstCaseFuel = proof.worstCaseFuel;
        return program;
    }
}

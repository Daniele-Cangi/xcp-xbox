#include "pch.h"
#include "WorkerGpuXvmProvableWorlds.h"

#include "WorkerGpuXvmExecutor.h"
#include "WorkerGpuXvmProvableWorldsProfile.h"

using namespace winrt;
using namespace Windows::ApplicationModel;

namespace XComputeProbe
{
    namespace
    {
        std::wstring JsonStringLocal(std::wstring const& value)
        {
            return L"\"" + value + L"\"";
        }

        std::wstring DoubleJsonLocal(double value)
        {
            std::wostringstream out;
            out << std::fixed << std::setprecision(3) << value;
            return out.str();
        }

        std::wstring FeatureLevelText(D3D_FEATURE_LEVEL value)
        {
            switch (value)
            {
            case D3D_FEATURE_LEVEL_12_1: return L"12_1";
            case D3D_FEATURE_LEVEL_12_0: return L"12_0";
            case D3D_FEATURE_LEVEL_11_1: return L"11_1";
            case D3D_FEATURE_LEVEL_11_0: return L"11_0";
            case D3D_FEATURE_LEVEL_10_1: return L"10_1";
            default: return L"10_0";
            }
        }

        std::vector<uint8_t> ReadInstalledShader(std::wstring const& name)
        {
            auto root = std::filesystem::path(Package::Current().InstalledLocation().Path().c_str());
            std::ifstream input(root / name, std::ios::binary);
            if (!input)
            {
                throw WorkerXvmError("xvm.gpu_execution_failed", "could not open installed Provable Worlds shader");
            }
            return std::vector<uint8_t>(
                (std::istreambuf_iterator<char>(input)),
                std::istreambuf_iterator<char>());
        }

        void ThrowGpuFailure(char const* operation, HRESULT hr)
        {
            auto code = WorkerGpuXvmIsDeviceLoss(hr)
                ? "xvm.gpu_device_lost"
                : "xvm.gpu_execution_failed";
            std::ostringstream message;
            message << operation << " failed with HRESULT 0x" << std::hex << std::uppercase
                << static_cast<uint32_t>(hr);
            throw WorkerXvmError(code, message.str());
        }

        winrt::com_ptr<ID3D11ShaderResourceView> CreateU32Srv(
            ID3D11Device* device,
            std::vector<uint32_t> const& words)
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(words.size() * sizeof(uint32_t));
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = sizeof(uint32_t);
            D3D11_SUBRESOURCE_DATA initial{};
            initial.pSysMem = words.data();
            winrt::com_ptr<ID3D11Buffer> buffer;
            auto hr = device->CreateBuffer(&desc, &initial, buffer.put());
            if (FAILED(hr))
            {
                ThrowGpuFailure("CreateBuffer(SRV)", hr);
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
            viewDesc.Format = DXGI_FORMAT_UNKNOWN;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            viewDesc.Buffer.FirstElement = 0;
            viewDesc.Buffer.NumElements = static_cast<UINT>(words.size());
            winrt::com_ptr<ID3D11ShaderResourceView> view;
            hr = device->CreateShaderResourceView(buffer.get(), &viewDesc, view.put());
            if (FAILED(hr))
            {
                ThrowGpuFailure("CreateShaderResourceView", hr);
            }
            return view;
        }
    }

    WorkerGpuXvmExecutionResult WorkerExecuteGpuXvmProvableWorlds(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGpuXvmExecutionPlan const& executionPlan,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        std::function<bool()> const& cancelRequested)
    {
        if (profile.kind != WorkerGpuXvmProfileKind::ProvableWorlds1024x1024U32V1 ||
            executionPlan.schemaVersion != WorkerGpuXvmExecutionPlanSchemaVersionV11)
        {
            throw WorkerXvmError("xvm.gpu_profile_not_admitted", "Provable Worlds executor requires the exact v11 profile");
        }
        auto revalidated = WorkerBuildGpuXvmExecutionPlan(
            profile,
            program,
            executionPlan.programSha256,
            executionPlan.spmdExecutionPlanSha256,
            executionPlan.laneCount,
            executionPlan.gridShape,
            executionPlan.workgroupShape);
        if (!WorkerGpuXvmExecutionPlansEqual(executionPlan, revalidated))
        {
            throw WorkerXvmError(
                "xvm.provable_worlds_revalidation_failed",
                "worker revalidation did not reproduce the admitted Provable Worlds plan");
        }
        if (cancelRequested && cancelRequested())
        {
            throw WorkerXvmError("job.canceled", "Provable Worlds canceled before its logical dispatch");
        }

        auto shaderBytes = ReadInstalledShader(profile.shaderName);
        std::wstring shaderSha256;
        if (!WorkerGpuXvmShaderMatchesContract(profile, shaderBytes, shaderSha256))
        {
            throw WorkerXvmError(
                "xvm.gpu_shader_identity_mismatch",
                "installed Provable Worlds shader does not match the admitted profile");
        }

        auto started = std::chrono::steady_clock::now();
        D3D_FEATURE_LEVEL levels[] =
        {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_10_0;
        auto hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,
            ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            device.put(),
            &featureLevel,
            context.put());
        if (FAILED(hr))
        {
            ThrowGpuFailure("D3D11CreateDevice", hr);
        }

        winrt::com_ptr<ID3D11ComputeShader> shader;
        hr = device->CreateComputeShader(
            shaderBytes.data(),
            shaderBytes.size(),
            nullptr,
            shader.put());
        if (FAILED(hr))
        {
            ThrowGpuFailure("CreateComputeShader", hr);
        }

        std::vector<uint32_t> parameters
        {
            1024,
            1024,
            1048576,
            8,
            8,
            16,
            64,
            11,
        };
        std::vector<uint32_t> inputWords(inputs.begin(), inputs.end());
        std::vector<uint32_t> programWords = program.words;
        auto parameterSrv = CreateU32Srv(device.get(), parameters);
        auto inputSrv = CreateU32Srv(device.get(), inputWords);
        auto programSrv = CreateU32Srv(device.get(), programWords);

        D3D11_BUFFER_DESC fieldDesc{};
        fieldDesc.ByteWidth = 1048576u * sizeof(uint32_t);
        fieldDesc.Usage = D3D11_USAGE_DEFAULT;
        fieldDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        fieldDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        fieldDesc.StructureByteStride = sizeof(uint32_t);
        winrt::com_ptr<ID3D11Buffer> fieldBuffer;
        hr = device->CreateBuffer(&fieldDesc, nullptr, fieldBuffer.put());
        if (FAILED(hr))
        {
            ThrowGpuFailure("CreateBuffer(field)", hr);
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = 1048576;
        winrt::com_ptr<ID3D11UnorderedAccessView> fieldUav;
        hr = device->CreateUnorderedAccessView(fieldBuffer.get(), &uavDesc, fieldUav.put());
        if (FAILED(hr))
        {
            ThrowGpuFailure("CreateUnorderedAccessView(field)", hr);
        }

        D3D11_BUFFER_DESC stagingDesc = fieldDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        winrt::com_ptr<ID3D11Buffer> staging;
        hr = device->CreateBuffer(&stagingDesc, nullptr, staging.put());
        if (FAILED(hr))
        {
            ThrowGpuFailure("CreateBuffer(staging)", hr);
        }

        ID3D11ShaderResourceView* srvs[] =
        {
            parameterSrv.get(),
            inputSrv.get(),
            programSrv.get(),
        };
        ID3D11UnorderedAccessView* uavs[] = { fieldUav.get() };
        UINT initialCounts[] = { 0 };
        context->CSSetShader(shader.get(), nullptr, 0);
        context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, initialCounts);
        context->Dispatch(128, 128, 1);
        context->CSSetShaderResources(0, 3, std::array<ID3D11ShaderResourceView*, 3>{}.data());
        context->CSSetUnorderedAccessViews(0, 1, std::array<ID3D11UnorderedAccessView*, 1>{}.data(), nullptr);
        context->CopyResource(staging.get(), fieldBuffer.get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
        {
            ThrowGpuFailure("Map(field)", hr);
        }
        WorkerGpuXvmExecutionResult result;
        result.run.output.resize(4194304);
        std::memcpy(result.run.output.data(), mapped.pData, result.run.output.size());
        context->Unmap(staging.get(), 0);
        context->CSSetShader(nullptr, nullptr, 0);

        auto removed = device->GetDeviceRemovedReason();
        if (FAILED(removed))
        {
            ThrowGpuFailure("GetDeviceRemovedReason", removed);
        }
        if (cancelRequested && cancelRequested())
        {
            throw WorkerXvmError("job.canceled", "Provable Worlds canceled after its logical dispatch");
        }

        auto elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        result.run.controlToken = L"pass";
        result.run.fuelConsumed = 16777216ull;
        result.telemetryJson =
            std::wstring(L"{\"backend\":\"cpu_gpu_spmd_differential\"") +
            L",\"profile\":" + JsonStringLocal(profile.profileId) +
            L",\"profile_contract_id\":" + JsonStringLocal(profile.contractId) +
            L",\"profile_contract_sha256\":" + JsonStringLocal(profile.contractSha256) +
            L",\"shader\":" + JsonStringLocal(profile.shaderName) +
            L",\"shader_sha256\":" + JsonStringLocal(shaderSha256) +
            L",\"shader_identity_verified\":true" +
            L",\"feature_level\":" + JsonStringLocal(FeatureLevelText(featureLevel)) +
            L",\"dispatch_strategy\":\"single_logical_dispatch\"" +
            L",\"dispatch_count\":1" +
            L",\"grid_shape\":[1024,1024,1]" +
            L",\"workgroup_shape\":[8,8,1]" +
            L",\"dispatch_group_shape\":[128,128,1]" +
            L",\"logical_lane_count\":1048576" +
            L",\"field_format\":\"u32_le\"" +
            L",\"field_bytes\":4194304" +
            L",\"gpu_execution_plan_schema\":\"gpu-xvm-execution-plan-v11\"" +
            L",\"gpu_execution_plan_sha256\":" + JsonStringLocal(executionPlan.planSha256) +
            L",\"spmd_execution_plan_sha256\":" + JsonStringLocal(executionPlan.spmdExecutionPlanSha256) +
            L",\"worker_revalidated\":true" +
            L",\"gpu_elapsed_ms\":" + DoubleJsonLocal(elapsedMs) +
            L",\"fuel_consumed\":16777216" +
            L",\"runtime_shader_compilation_used\":false" +
            L",\"gpu_authority\":false" +
            L",\"arbitrary_native_or_host_code_executed\":false}";
        return result;
    }
}

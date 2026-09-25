#pragma once

#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe
{
    struct WorkerCreativeHostErrorDetails
    {
        std::string stage = "creative_lifecycle";
        std::string field;
        std::string path;
        std::string expected;
        std::string actual;
        std::string correction =
            "refresh describe_creative_host, correct the request or bundle, and retry";
        bool retryable = false;
    };

    struct WorkerCreativeHostError : std::exception
    {
        std::string code;
        std::string message;
        WorkerCreativeHostErrorDetails details;

        WorkerCreativeHostError(
            std::string codeValue,
            std::string messageValue,
            WorkerCreativeHostErrorDetails detailsValue = {});
        char const* what() const noexcept override;
    };

    struct WorkerCreativeInstalledFile
    {
        std::wstring role;
        std::wstring id;
        std::wstring kind;
        std::wstring runtimePath;
        std::wstring sha256;
        uint64_t bytes = 0;
        std::filesystem::path casPath;
    };

    struct WorkerCreativeActiveInstall
    {
        std::wstring installId;
        std::wstring projectId;
        std::wstring projectVersion;
        std::wstring entryModule;
        std::wstring bundleSha256;
        std::wstring contentSha256;
        std::wstring hostProfileCanonicalSha256;
        std::wstring activationRecordSha256;
        uint64_t activationSequence = 0;
        uint64_t totalBytes = 0;
        std::vector<WorkerCreativeInstalledFile> files;
    };

    WorkerCreativeActiveInstall WorkerLoadActiveCreativeInstall(
        std::filesystem::path const& root,
        std::wstring const& projectId,
        std::wstring const& installId);

    std::wstring WorkerPrepareCreativeInstall(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::wstring const& protocolVersion,
        std::filesystem::path const& root);

    std::wstring WorkerCommitCreativeInstall(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::wstring const& protocolVersion,
        std::filesystem::path const& root);

    std::wstring WorkerListCreativeInstalls(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::wstring const& protocolVersion,
        std::filesystem::path const& root);

    std::wstring WorkerActivateCreativeInstall(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::wstring const& protocolVersion,
        std::filesystem::path const& root);

    std::wstring WorkerRollbackCreativeActivation(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::wstring const& protocolVersion,
        std::filesystem::path const& root);

    std::wstring WorkerRemoveCreativeInstall(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::wstring const& protocolVersion,
        std::filesystem::path const& root);
}

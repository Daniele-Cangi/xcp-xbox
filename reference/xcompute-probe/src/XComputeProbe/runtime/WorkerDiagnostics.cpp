#include "pch.h"
#include "WorkerDiagnostics.h"

#include "../ProbeResult.h"

namespace XComputeProbe
{
    namespace
    {
        std::wstring JsonStringArray(std::vector<std::wstring> const& values)
        {
            std::wostringstream out;
            out << L"[";
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (i != 0)
                {
                    out << L",";
                }
                out << JsonString(values[i]);
            }
            out << L"]";
            return out.str();
        }

        wchar_t const* BoolJson(bool value)
        {
            return value ? L"true" : L"false";
        }

        std::wstring RedactSensitiveText(std::wstring const& value)
        {
            auto lower = value;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
                });
            for (auto const& marker : { L"pairing", L"pin", L"password", L"token", L"cookie", L"authorization", L"session_id", L"session id", L"session-id", L"credential", L"secret" })
            {
                if (lower.find(marker) != std::wstring::npos)
                {
                    return L"<redacted>";
                }
            }
            return value;
        }
    }

    std::wstring WorkerBuildDiagnosticsJson(WorkerDiagnosticsInput const& input)
    {
        auto const& state = input.state;
        auto const& workspace = input.workspace;
        std::vector<std::wstring> safeEvents;
        for (auto const& eventText : state.recentEvents)
        {
            safeEvents.push_back(RedactSensitiveText(eventText));
        }

        bool bridgeReady = state.running &&
            state.failedRequestCount == 0 &&
            !state.asyncJobRunning &&
            state.activeTaskPlanRunCount == 0;

        std::wostringstream json;
        json << L"{"
            << L"\"schema_version\":\"m5-worker-ui-diagnostics-0.1\","
            << L"\"diagnostics_capabilities\":[\"m5-worker-ui-diagnostics-0.1\",\"m62-xbox-ui-status-bridge-0.1\"],"
            << L"\"export_source\":\"worker_command\","
            << L"\"auth_redaction\":\"pairing codes, session ids, tokens, cookies, authorization headers, and account secrets are omitted\","
            << L"\"worker\":{"
            << L"\"running\":" << BoolJson(state.running) << L","
            << L"\"starting\":" << BoolJson(state.starting) << L","
            << L"\"port\":" << (state.port == 0 ? 8787 : state.port) << L","
            << L"\"accepted_connections\":" << state.acceptedConnectionCount << L","
            << L"\"completed_requests\":" << state.completedRequestCount << L","
            << L"\"failed_requests\":" << state.failedRequestCount << L","
            << L"\"active_sessions\":" << state.activeSessionCount << L","
            << L"\"active_async_jobs\":" << state.activeAsyncJobCount << L","
            << L"\"queued_async_jobs\":" << state.queuedAsyncJobCount << L","
            << L"\"async_job_running\":" << BoolJson(state.asyncJobRunning) << L","
            << L"\"latest_async_job_id\":" << JsonString(state.latestAsyncJobId) << L","
            << L"\"latest_async_job_status\":" << JsonString(state.latestAsyncJobStatus) << L","
            << L"\"latest_async_job_error\":" << JsonString(RedactSensitiveText(state.latestAsyncJobError)) << L","
            << L"\"active_task_plan_runs\":" << state.activeTaskPlanRunCount << L","
            << L"\"active_task_plan_id\":" << JsonString(state.activeTaskPlanId) << L","
            << L"\"active_task_plan_run_id\":" << JsonString(state.activeTaskPlanRunId) << L","
            << L"\"latest_task_plan_id\":" << JsonString(state.latestTaskPlanId) << L","
            << L"\"latest_task_plan_run_id\":" << JsonString(state.latestTaskPlanRunId) << L","
            << L"\"latest_task_plan_run_status\":" << JsonString(state.latestTaskPlanRunStatus) << L","
            << L"\"latest_task_plan_blocked_reason\":" << JsonString(state.latestTaskPlanBlockedReason) << L","
            << L"\"latest_task_plan_error\":" << JsonString(RedactSensitiveText(state.latestTaskPlanError)) << L","
            << L"\"last_error\":" << JsonString(RedactSensitiveText(state.lastError)) << L","
            << L"\"recent_events\":" << JsonStringArray(safeEvents)
            << L"},"
            << L"\"workspace\":{"
            << L"\"relative_path\":\"codex-worker-prototype\","
            << L"\"exists\":" << BoolJson(workspace.exists) << L","
            << L"\"file_count\":" << workspace.fileCount << L","
            << L"\"directory_count\":" << workspace.directoryCount << L","
            << L"\"bytes\":" << workspace.byteCount
            << L"},"
            << L"\"codex_auth_bridge\":{"
            << L"\"schema_version\":\"m62-xbox-ui-status-bridge-0.1\","
            << L"\"status\":" << JsonString(bridgeReady ? L"ready_for_pc_codex_tool_calls" : L"worker_not_ready") << L","
            << L"\"pc_codex_auth\":\"chatgpt_managed_app_server\","
            << L"\"xbox_stores_codex_tokens\":false,"
            << L"\"api_key_used_on_xbox\":false,"
            << L"\"copied_auth_cache_on_xbox\":false,"
            << L"\"adapter_policy\":\"allowlisted_xcompute_tools_only\","
            << L"\"bridge_path\":\"Codex PC app-server -> allowlisted adapter -> XCompute worker -> verdict/report on Xbox\","
            << L"\"verdict_report_on_xbox\":true,"
            << L"\"worker_ready\":" << BoolJson(bridgeReady)
            << L"},"
            << L"\"recovery_status\":\"diagnostics exported by worker command\""
            << L"}";
        return json.str();
    }
}

#include "pch.h"
#include "Direct3DApp.h"
#include "probes/ProbeRunner.h"
#include "runtime/WorkerCommandServer.h"

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Networking;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Storage;
using namespace Windows::UI::Core;

namespace fs = std::filesystem;

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t CompiledManifestFlavorMarker[] =
            L"XCOMPUTE_COMPILED_MANIFEST_FLAVOR=" XCOMPUTE_MANIFEST_FLAVOR;
    }

    struct App : implements<App, IFrameworkViewSource, IFrameworkView>
    {
        IFrameworkView CreateView()
        {
            return *this;
        }

        void Initialize(CoreApplicationView const& appView)
        {
            appView.Activated({ this, &App::OnActivated });
            CoreApplication::Suspending({ this, &App::OnSuspending });
            CoreApplication::Resuming({ this, &App::OnResuming });
        }

        void SetWindow(CoreWindow const& window)
        {
            m_window = window;
            window.Closed({ this, &App::OnWindowClosed });
            window.VisibilityChanged({ this, &App::OnVisibilityChanged });
            window.KeyDown({ this, &App::OnKeyDown });
            window.KeyUp({ this, &App::OnKeyUp });
            m_renderer.SetWindow(window);
            m_statusMessage = L"Starting probe suite...";
            RefreshUi();
        }

        void Load(hstring const&)
        {
            m_workerFlavor = std::wstring(XCOMPUTE_MANIFEST_FLAVOR) == L"worker-prototype";
            if (m_workerFlavor)
            {
                m_workerServer.CreativeForeground().Initialize(
                    WorkerWorkspacePath());
                auto restored =
                    m_workerServer.CreativeForeground().Snapshot();
                if (restored)
                {
                    m_lastCreativeLaunchSequence =
                        restored->launchSequence;
                }
            }
            m_commandServerStarted = m_workerFlavor && m_workerServer.Start(8787);
            auto probes = m_probeRunner.RunSafeSuite(m_renderer.MakeGraphicsProbes());
            auto report = m_probeRunner.BuildReportJson(probes);
            m_resultSaved = m_probeRunner.SaveReport(report);
            m_probeRunner.SendReportToLanReceiverAsync(report);
            m_probeComplete = true;
            m_statusMessage = L"Probe suite complete.";
            RefreshUi(true);
        }

        void Run()
        {
            while (!m_windowClosed)
            {
                if (m_windowVisible)
                {
                    m_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                    if (m_workerFlavor)
                    {
                        auto creativeSnapshot =
                            m_workerServer.CreativeForeground().Snapshot();
                        if (creativeSnapshot &&
                            creativeSnapshot->launchSequence >
                                m_lastCreativeLaunchSequence)
                        {
                            m_lastCreativeLaunchSequence =
                                creativeSnapshot->launchSequence;
                            m_operatorOverlayVisible = false;
                            m_statusMessage =
                                L"Studio launched a project; creative foreground active.";
                            RefreshUi(true);
                        }
                        if (!m_operatorOverlayVisible)
                        {
                            m_workerServer.CreativeForeground().Tick();
                            m_renderer.SetCreativeSnapshot(
                                m_workerServer.CreativeForeground().Snapshot());
                            auto capture =
                                m_workerServer.CreativeForeground()
                                    .TakeFrameCaptureRequest();
                            if (capture.has_value())
                            {
                                m_renderer.SetCreativeFrameCaptureRequest(
                                    std::move(*capture));
                            }
                        }
                        else
                        {
                            m_renderer.SetCreativeSnapshot(nullptr);
                        }
                    }
                    RefreshUiIfNeeded();
                    m_renderer.Render();
                    if (m_workerFlavor)
                    {
                        auto captured =
                            m_renderer
                                .TakeCreativeFrameCaptureResult();
                        if (captured.has_value())
                        {
                            m_workerServer.CreativeForeground()
                                .CompleteFrameCapture(
                                    std::move(*captured));
                        }
                    }
                }
                else
                {
                    m_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
                }
            }
        }

        void Uninitialize()
        {
            m_workerServer.RecordProcessTopologyLifecycleEvent(L"terminated");
        }

        void OnActivated(CoreApplicationView const&, IActivatedEventArgs const& args)
        {
            CoreWindow::GetForCurrentThread().Activate();
            std::wostringstream line;
            line << L"Activated. Previous state: " << static_cast<int>(args.PreviousExecutionState());
            m_lifecycleStatus = line.str();
            RefreshUi(true);
        }

        void OnSuspending(IInspectable const&, SuspendingEventArgs const& args)
        {
            auto deferral = args.SuspendingOperation().GetDeferral();
            m_workerServer.RecordProcessTopologyLifecycleEvent(L"suspending");
            m_workerServer.CreativeForeground().OnSuspending();
            m_lifecycleStatus = L"Suspending.";
            RefreshUi(true);
            deferral.Complete();
        }

        void OnResuming(IInspectable const&, IInspectable const&)
        {
            m_lifecycleStatus = L"Resumed; awaiting a phase-bound runtime witness.";
            m_workerServer.CreativeForeground().OnResuming();
            RefreshUi(true);
        }

        void OnWindowClosed(CoreWindow const&, CoreWindowEventArgs const&)
        {
            m_workerServer.RecordProcessTopologyLifecycleEvent(L"terminated");
            m_windowClosed = true;
        }

        void OnVisibilityChanged(CoreWindow const&, VisibilityChangedEventArgs const& args)
        {
            m_windowVisible = args.Visible();
        }

        void OnKeyDown(CoreWindow const&, KeyEventArgs const& args)
        {
            auto const key = args.VirtualKey();
            if (m_workerFlavor &&
                (key ==
                    Windows::System::VirtualKey::GamepadMenu ||
                 key ==
                    Windows::System::VirtualKey::F1))
            {
                m_operatorOverlayVisible = !m_operatorOverlayVisible;
                m_statusMessage = m_operatorOverlayVisible
                    ? L"Operator overlay visible; creative input is paused."
                    : L"Operator overlay hidden; creative foreground resumed.";
                if (m_operatorOverlayVisible)
                {
                    m_renderer.SetCreativeSnapshot(nullptr);
                }
                else
                {
                    m_renderer.SetCreativeSnapshot(
                        m_workerServer.CreativeForeground().Snapshot());
                }
                RefreshUi(true);
                return;
            }
            if (m_workerFlavor &&
                !m_operatorOverlayVisible &&
                m_workerServer.CreativeForeground().HasActiveProject() &&
                (key == Windows::System::VirtualKey::GamepadA ||
                 key == Windows::System::VirtualKey::GamepadB ||
                 key == Windows::System::VirtualKey::GamepadX ||
                 key == Windows::System::VirtualKey::GamepadY ||
                 key == Windows::System::VirtualKey::GamepadDPadUp ||
                 key == Windows::System::VirtualKey::GamepadDPadDown ||
                 key == Windows::System::VirtualKey::GamepadDPadLeft ||
                 key == Windows::System::VirtualKey::GamepadDPadRight))
            {
                // Physical creative gamepad input is sampled once by the
                // foreground runtime. Swallow the mirrored CoreWindow key
                // event so it cannot also trigger keyboard UI navigation or
                // worker-shell B/X/Y shortcuts.
                return;
            }
            if (m_workerFlavor &&
                !m_operatorOverlayVisible &&
                m_workerServer.CreativeForeground().HandleKeyDown(
                    key))
            {
                m_renderer.SetCreativeSnapshot(
                    m_workerServer.CreativeForeground().Snapshot());
                return;
            }
            if (key == Windows::System::VirtualKey::Escape ||
                key == Windows::System::VirtualKey::GamepadB)
            {
                m_windowClosed = true;
                return;
            }

            if (key == Windows::System::VirtualKey::GamepadY ||
                key == Windows::System::VirtualKey::F5 ||
                key == Windows::System::VirtualKey::R)
            {
                if (m_workerFlavor)
                {
                    m_commandServerStarted = m_workerServer.Restart();
                    m_statusMessage = m_commandServerStarted ? L"Worker listener restart requested." : L"Worker listener restart failed.";
                    RefreshUi(true);
                }
                return;
            }

            if (key == Windows::System::VirtualKey::GamepadX ||
                key == Windows::System::VirtualKey::X)
            {
                if (m_workerFlavor)
                {
                    ExportRedactedDiagnostics();
                    RefreshUi(true);
                }
                return;
            }

            if (key == Windows::System::VirtualKey::GamepadView ||
                key == Windows::System::VirtualKey::C)
            {
                if (m_workerFlavor)
                {
                    HandleClearWorkspaceRequest();
                    RefreshUi(true);
                }
                return;
            }

            if (key == Windows::System::VirtualKey::GamepadA ||
                key == Windows::System::VirtualKey::Enter)
            {
                if (m_workerFlavor &&
                    m_operatorOverlayVisible &&
                    m_workerServer.CreativeForeground().HasActiveProject())
                {
                    m_operatorOverlayVisible = false;
                    m_renderer.SetCreativeSnapshot(
                        m_workerServer.CreativeForeground().Snapshot());
                    m_statusMessage =
                        L"Saved creative project resumed.";
                    RefreshUi(true);
                    return;
                }
                m_statusMessage = L"UI refreshed.";
                RefreshUi(true);
            }
        }

        void OnKeyUp(CoreWindow const&, KeyEventArgs const& args)
        {
            if (m_workerFlavor)
            {
                m_workerServer.CreativeForeground().HandleKeyUp(
                    args.VirtualKey());
            }
        }

    private:
        static std::wstring BoolText(bool value)
        {
            return value ? L"true" : L"false";
        }

        static std::wstring JoinStrings(std::vector<std::wstring> const& values, std::wstring const& separator)
        {
            std::wostringstream out;
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (i != 0)
                {
                    out << separator;
                }
                out << values[i];
            }
            return out.str();
        }

        static std::wstring JsonEscape(std::wstring const& value)
        {
            std::wostringstream out;
            out << L"\"";
            for (auto ch : value)
            {
                switch (ch)
                {
                case L'\\':
                    out << L"\\\\";
                    break;
                case L'"':
                    out << L"\\\"";
                    break;
                case L'\b':
                    out << L"\\b";
                    break;
                case L'\f':
                    out << L"\\f";
                    break;
                case L'\n':
                    out << L"\\n";
                    break;
                case L'\r':
                    out << L"\\r";
                    break;
                case L'\t':
                    out << L"\\t";
                    break;
                default:
                    if (ch < 0x20)
                    {
                        out << L"\\u" << std::hex << std::setw(4) << std::setfill(L'0') << static_cast<int>(ch) << std::dec;
                    }
                    else
                    {
                        out << ch;
                    }
                    break;
                }
            }
            out << L"\"";
            return out.str();
        }

        static std::wstring JsonStringArray(std::vector<std::wstring> const& values)
        {
            std::wostringstream out;
            out << L"[";
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (i != 0)
                {
                    out << L",";
                }
                out << JsonEscape(values[i]);
            }
            out << L"]";
            return out.str();
        }

        static std::wstring RedactSensitiveText(std::wstring const& value)
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

        static std::wstring ExceptionMessage(std::exception const& ex)
        {
            return std::wstring(winrt::to_hstring(ex.what()).c_str());
        }

        static fs::path LocalFolderPath()
        {
            auto folder = ApplicationData::Current().LocalFolder();
            return fs::path(std::wstring(folder.Path().c_str()));
        }

        static fs::path WorkerWorkspacePath()
        {
            return LocalFolderPath() / L"codex-worker-prototype";
        }

        struct WorkspaceStats
        {
            bool exists = false;
            uint64_t fileCount = 0;
            uint64_t directoryCount = 0;
            uint64_t byteCount = 0;
        };

        static WorkspaceStats CollectWorkspaceStats(fs::path const& path)
        {
            WorkspaceStats stats;
            try
            {
                stats.exists = fs::exists(path);
                if (!stats.exists)
                {
                    return stats;
                }

                for (auto const& entry : fs::recursive_directory_iterator(path))
                {
                    if (entry.is_directory())
                    {
                        ++stats.directoryCount;
                    }
                    else if (entry.is_regular_file())
                    {
                        ++stats.fileCount;
                        stats.byteCount += static_cast<uint64_t>(entry.file_size());
                    }
                }
            }
            catch (...)
            {
            }
            return stats;
        }

        static std::vector<std::wstring> LocalIpv4Addresses()
        {
            std::vector<std::wstring> addresses;
            try
            {
                for (auto const& hostName : NetworkInformation::GetHostNames())
                {
                    if (hostName.Type() != HostNameType::Ipv4)
                    {
                        continue;
                    }
                    auto value = std::wstring(hostName.CanonicalName().c_str());
                    if (!value.empty() && std::find(addresses.begin(), addresses.end(), value) == addresses.end())
                    {
                        addresses.push_back(value);
                    }
                    if (addresses.size() >= 4)
                    {
                        break;
                    }
                }
            }
            catch (...)
            {
            }
            return addresses;
        }

        bool ClearWorkspaceArmed() const
        {
            return m_clearWorkspaceArmed &&
                std::chrono::steady_clock::now() <= m_clearWorkspaceArmedUntil;
        }

        void HandleClearWorkspaceRequest()
        {
            auto now = std::chrono::steady_clock::now();
            if (!m_clearWorkspaceArmed || now > m_clearWorkspaceArmedUntil)
            {
                m_clearWorkspaceArmed = true;
                m_clearWorkspaceArmedUntil = now + std::chrono::seconds(10);
                m_statusMessage = L"Clear workspace armed. Press View/C again within 10s to confirm.";
                m_recoveryStatus = L"clear armed";
                return;
            }

            m_clearWorkspaceArmed = false;
            m_clearWorkspaceArmedUntil = {};
            ClearWorkerWorkspace();
        }

        void ClearWorkerWorkspace()
        {
            try
            {
                auto workspace = WorkerWorkspacePath();
                auto before = CollectWorkspaceStats(workspace);
                uint64_t removed = 0;
                if (before.exists)
                {
                    removed = static_cast<uint64_t>(fs::remove_all(workspace));
                }
                fs::create_directories(workspace);
                m_statusMessage = L"Workspace cleared. Worker seed files regenerate on next command.";
                std::wostringstream status;
                status << L"cleared workspace entries=" << removed
                    << L" files_before=" << before.fileCount
                    << L" bytes_before=" << before.byteCount;
                m_recoveryStatus = status.str();
            }
            catch (std::exception const& ex)
            {
                m_statusMessage = L"Workspace clear failed.";
                m_recoveryStatus = L"clear failed: " + ExceptionMessage(ex);
            }
            catch (...)
            {
                m_statusMessage = L"Workspace clear failed.";
                m_recoveryStatus = L"clear failed";
            }
        }

        void ExportRedactedDiagnostics()
        {
            try
            {
                auto snapshot = m_workerServer.Snapshot();
                auto addresses = LocalIpv4Addresses();
                auto stats = CollectWorkspaceStats(WorkerWorkspacePath());
                std::wstring fileName = L"xcompute-worker-diagnostics.json";
                auto path = LocalFolderPath() / fileName;

                std::vector<std::wstring> safeEvents;
                for (auto const& eventText : snapshot.recentEvents)
                {
                    safeEvents.push_back(RedactSensitiveText(eventText));
                }
                bool bridgeReady = snapshot.running &&
                    snapshot.failedRequestCount == 0 &&
                    !snapshot.asyncJobRunning &&
                    snapshot.activeTaskPlanRunCount == 0;

                std::wostringstream json;
                json << L"{"
                    << L"\"schema_version\":\"m5-worker-ui-diagnostics-0.1\","
                    << L"\"diagnostics_capabilities\":[\"m5-worker-ui-diagnostics-0.1\",\"m62-xbox-ui-status-bridge-0.1\"],"
                    << L"\"auth_redaction\":\"pairing codes, session ids, tokens, cookies, authorization headers, and account secrets are omitted\","
                    << L"\"manifest_flavor\":" << JsonEscape(XCOMPUTE_MANIFEST_FLAVOR) << L","
                    << L"\"compiled_manifest_flavor_marker\":" << JsonEscape(CompiledManifestFlavorMarker) << L","
                    << L"\"probe_complete\":" << BoolText(m_probeComplete) << L","
                    << L"\"result_saved\":" << BoolText(m_resultSaved) << L","
                    << L"\"worker\":{"
                    << L"\"running\":" << BoolText(snapshot.running) << L","
                    << L"\"starting\":" << BoolText(snapshot.starting) << L","
                    << L"\"port\":" << (snapshot.port == 0 ? 8787 : snapshot.port) << L","
                    << L"\"accepted_connections\":" << snapshot.acceptedConnectionCount << L","
                    << L"\"completed_requests\":" << snapshot.completedRequestCount << L","
                    << L"\"failed_requests\":" << snapshot.failedRequestCount << L","
                    << L"\"active_sessions\":" << snapshot.activeSessionCount << L","
                    << L"\"active_async_jobs\":" << snapshot.activeAsyncJobCount << L","
                    << L"\"queued_async_jobs\":" << snapshot.queuedAsyncJobCount << L","
                    << L"\"async_job_running\":" << BoolText(snapshot.asyncJobRunning) << L","
                    << L"\"latest_async_job_id\":" << JsonEscape(snapshot.latestAsyncJobId) << L","
                    << L"\"latest_async_job_status\":" << JsonEscape(snapshot.latestAsyncJobStatus) << L","
                    << L"\"latest_async_job_error\":" << JsonEscape(RedactSensitiveText(snapshot.latestAsyncJobError)) << L","
                    << L"\"active_task_plan_runs\":" << snapshot.activeTaskPlanRunCount << L","
                    << L"\"active_task_plan_id\":" << JsonEscape(snapshot.activeTaskPlanId) << L","
                    << L"\"active_task_plan_run_id\":" << JsonEscape(snapshot.activeTaskPlanRunId) << L","
                    << L"\"latest_task_plan_id\":" << JsonEscape(snapshot.latestTaskPlanId) << L","
                    << L"\"latest_task_plan_run_id\":" << JsonEscape(snapshot.latestTaskPlanRunId) << L","
                    << L"\"latest_task_plan_run_status\":" << JsonEscape(snapshot.latestTaskPlanRunStatus) << L","
                    << L"\"latest_task_plan_blocked_reason\":" << JsonEscape(snapshot.latestTaskPlanBlockedReason) << L","
                    << L"\"latest_task_plan_error\":" << JsonEscape(RedactSensitiveText(snapshot.latestTaskPlanError)) << L","
                    << L"\"last_error\":" << JsonEscape(RedactSensitiveText(snapshot.lastError)) << L","
                    << L"\"recent_events\":" << JsonStringArray(safeEvents)
                    << L"},"
                    << L"\"network\":{\"ipv4\":" << JsonStringArray(addresses) << L"},"
                    << L"\"workspace\":{"
                    << L"\"relative_path\":\"codex-worker-prototype\","
                    << L"\"exists\":" << BoolText(stats.exists) << L","
                    << L"\"file_count\":" << stats.fileCount << L","
                    << L"\"directory_count\":" << stats.directoryCount << L","
                    << L"\"bytes\":" << stats.byteCount
                    << L"},"
                    << L"\"codex_auth_bridge\":{"
                    << L"\"schema_version\":\"m62-xbox-ui-status-bridge-0.1\","
                    << L"\"status\":" << JsonEscape(bridgeReady ? L"ready_for_pc_codex_tool_calls" : L"worker_not_ready") << L","
                    << L"\"pc_codex_auth\":\"chatgpt_managed_app_server\","
                    << L"\"xbox_stores_codex_tokens\":false,"
                    << L"\"api_key_used_on_xbox\":false,"
                    << L"\"copied_auth_cache_on_xbox\":false,"
                    << L"\"adapter_policy\":\"allowlisted_xcompute_tools_only\","
                    << L"\"bridge_path\":\"Codex PC app-server -> allowlisted adapter -> XCompute worker -> verdict/report on Xbox\","
                    << L"\"verdict_report_on_xbox\":true,"
                    << L"\"worker_ready\":" << BoolText(bridgeReady)
                    << L"},"
                    << L"\"recovery_status\":" << JsonEscape(RedactSensitiveText(m_recoveryStatus))
                    << L"}";

                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                auto utf8 = winrt::to_string(json.str());
                out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
                out.flush();
                if (!out.good())
                {
                    throw std::runtime_error("diagnostics write did not complete");
                }

                m_lastDiagnosticsFileName = fileName;
                m_statusMessage = L"Diagnostics exported to LocalFolder: " + fileName;
                m_recoveryStatus = L"diagnostics exported";
            }
            catch (std::exception const& ex)
            {
                m_statusMessage = L"Diagnostics export failed.";
                m_recoveryStatus = L"diagnostics failed: " + ExceptionMessage(ex);
            }
            catch (...)
            {
                m_statusMessage = L"Diagnostics export failed.";
                m_recoveryStatus = L"diagnostics failed";
            }
        }

        void RefreshUiIfNeeded()
        {
            auto now = std::chrono::steady_clock::now();
            if (now - m_lastUiRefresh >= std::chrono::seconds(1))
            {
                RefreshUi();
            }
        }

        void RefreshUi(bool force = false)
        {
            auto now = std::chrono::steady_clock::now();
            if (!force && now - m_lastUiRefresh < std::chrono::milliseconds(500))
            {
                return;
            }
            m_lastUiRefresh = now;
            if (m_clearWorkspaceArmed && !ClearWorkspaceArmed())
            {
                m_clearWorkspaceArmed = false;
                m_recoveryStatus = L"clear arm expired";
            }

            std::vector<std::wstring> lines;
            lines.push_back(L"XCP WORKER - CONNECT TO XCP STUDIO");
            lines.push_back(L"Probe: " + std::wstring(m_probeComplete ? L"complete" : L"running") +
                L" | result: " + std::wstring(m_probeComplete ? (m_resultSaved ? L"saved" : L"save failed") : L"pending"));
            lines.push_back(L"Status: " + m_statusMessage);

            if (m_workerFlavor)
            {
                auto snapshot = m_workerServer.Snapshot();
                auto addresses = LocalIpv4Addresses();
                std::wstring state = snapshot.running ? L"running" : (snapshot.starting ? L"starting" : L"stopped");
                lines.push_back(addresses.empty()
                    ? L"Xbox address: waiting for an IPv4 address"
                    : L"Xbox address: " + addresses.front());
                lines.push_back(
                    L"First-trust code: " +
                    m_workerServer.PairingCode());
                lines.push_back(
                    L"On the PC: XCP Studio > Devices > Trust this PC once");
                lines.push_back(
                    L"Trusted PCs: " +
                    std::to_wstring(snapshot.trustedControllerCount) +
                    L" | worker: " + state +
                    L" | port: " +
                    std::to_wstring(snapshot.port == 0 ? 8787 : snapshot.port));
                lines.push_back(
                    m_workerServer.CreativeForeground().HasActiveProject()
                        ? L"A resumes the saved project | Menu returns here"
                        : L"Keep this screen open while Studio completes first trust");
                lines.push_back(
                    L"Operator overlay: " +
                    std::wstring(
                        m_operatorOverlayVisible ? L"visible" : L"hidden") +
                    L" | Menu/F1 toggles without clearing workspace");
                lines.push_back(L"Connections: " + std::to_wstring(snapshot.acceptedConnectionCount) +
                    L" | requests ok/fail: " + std::to_wstring(snapshot.completedRequestCount) +
                    L"/" + std::to_wstring(snapshot.failedRequestCount));
                lines.push_back(L"Sessions: " + std::to_wstring(snapshot.activeSessionCount) +
                    L" | async jobs active/queued: " + std::to_wstring(snapshot.activeAsyncJobCount) +
                    L"/" + std::to_wstring(snapshot.queuedAsyncJobCount));
                lines.push_back(L"Job status: running=" + BoolText(snapshot.asyncJobRunning) +
                    L" | latest: " + (snapshot.latestAsyncJobId.empty() ? std::wstring(L"none") : snapshot.latestAsyncJobId) +
                    L" " + (snapshot.latestAsyncJobStatus.empty() ? std::wstring(L"idle") : snapshot.latestAsyncJobStatus));
                if (!snapshot.latestAsyncJobError.empty())
                {
                    lines.push_back(L"Job error: " + RedactSensitiveText(snapshot.latestAsyncJobError));
                }
                lines.push_back(L"Task plans: active runs " + std::to_wstring(snapshot.activeTaskPlanRunCount) +
                    L" | active: " + (snapshot.activeTaskPlanId.empty() ? std::wstring(L"none") : snapshot.activeTaskPlanId));
                if (!snapshot.latestTaskPlanRunId.empty())
                {
                    lines.push_back(L"Task status: " + snapshot.latestTaskPlanId +
                        L" / " + snapshot.latestTaskPlanRunId +
                        L" " + (snapshot.latestTaskPlanRunStatus.empty() ? std::wstring(L"unknown") : snapshot.latestTaskPlanRunStatus));
                }
                if (!snapshot.latestTaskPlanBlockedReason.empty())
                {
                    lines.push_back(L"Task blocked: " + snapshot.latestTaskPlanBlockedReason);
                }
                if (!snapshot.latestTaskPlanError.empty())
                {
                    lines.push_back(L"Task error: " + RedactSensitiveText(snapshot.latestTaskPlanError));
                }
                if (!snapshot.lastError.empty())
                {
                    lines.push_back(L"Last worker error: " + snapshot.lastError);
                }

                lines.push_back(addresses.empty()
                    ? L"Network: no IPv4 address visible yet"
                    : L"Network IPv4: " + JoinStrings(addresses, L", "));
                lines.push_back(L"Controls: Y/F5/R restart | X diagnostics | View/C clear arm/confirm");
                lines.push_back(L"Controls: A/Enter refresh | B/Esc close");
                lines.push_back(ClearWorkspaceArmed()
                    ? std::wstring(L"Recovery: CLEAR ARMED - press View/C again within 10s")
                    : (m_recoveryStatus.empty() ? std::wstring(L"Recovery: idle") : std::wstring(L"Recovery: ") + m_recoveryStatus));
                if (!m_lastDiagnosticsFileName.empty())
                {
                    lines.push_back(L"Diagnostics: LocalFolder\\" + m_lastDiagnosticsFileName);
                }
                bool bridgeReady = snapshot.running &&
                    snapshot.failedRequestCount == 0 &&
                    !snapshot.asyncJobRunning &&
                    snapshot.activeTaskPlanRunCount == 0;
                lines.push_back(L"Codex bridge: PC ChatGPT app-server -> allowlisted adapter -> XCompute worker.");
                lines.push_back(L"Bridge ready: " + BoolText(bridgeReady) +
                    L" | verdict/report visible on Xbox diagnostics.");
                lines.push_back(L"Codex auth: PC-managed; Xbox stores no OpenAI/Codex tokens.");

                if (!snapshot.recentEvents.empty())
                {
                    lines.push_back(L"Recent worker events:");
                    auto first = snapshot.recentEvents.size() > 4 ? snapshot.recentEvents.size() - 4 : 0;
                    for (size_t i = first; i < snapshot.recentEvents.size(); ++i)
                    {
                        lines.push_back(L"  " + snapshot.recentEvents[i]);
                    }
                }
            }
            else
            {
                lines.push_back(L"Worker: disabled for this manifest flavor");
                lines.push_back(L"LAN receiver is used only when ProbeConfig.json has an endpoint.");
            }

            if (!m_lifecycleStatus.empty())
            {
                lines.push_back(L"Lifecycle: " + m_lifecycleStatus);
            }

            m_renderer.SetStatusLines(std::move(lines));
        }

        CoreWindow m_window{ nullptr };
        Direct3DApp m_renderer;
        ProbeRunner m_probeRunner;
        WorkerCommandServer m_workerServer;
        bool m_windowClosed = false;
        bool m_windowVisible = true;
        bool m_workerFlavor = false;
        bool m_commandServerStarted = false;
        bool m_probeComplete = false;
        bool m_resultSaved = false;
        bool m_operatorOverlayVisible = true;
        bool m_clearWorkspaceArmed = false;
        std::wstring m_statusMessage;
        std::wstring m_lifecycleStatus;
        std::wstring m_recoveryStatus;
        std::wstring m_lastDiagnosticsFileName;
        std::chrono::steady_clock::time_point m_lastUiRefresh{};
        std::chrono::steady_clock::time_point m_clearWorkspaceArmedUntil{};
        uint64_t m_lastCreativeLaunchSequence = 0;
    };
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    winrt::init_apartment();
    winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<XComputeProbe::App>());
    return 0;
}

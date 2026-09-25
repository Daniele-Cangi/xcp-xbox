#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.System.h>

#include "WorkerCreativeWorld2DRuntime.h"
#include "WorkerXvmTypes.h"

namespace XComputeProbe
{
    using WorkerCreativeScalar =
        std::variant<bool, int64_t, double, std::wstring>;

    struct WorkerCreativeVec2
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct WorkerCreativeVec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct WorkerCreativeColor
    {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    struct WorkerCreativeTransform2D
    {
        WorkerCreativeVec2 position;
        float rotationDegrees = 0.0f;
        WorkerCreativeVec2 scale{ 1.0f, 1.0f };
    };

    struct WorkerCreativeTransform3D
    {
        WorkerCreativeVec3 position;
        WorkerCreativeVec3 rotationDegrees;
        WorkerCreativeVec3 scale{ 1.0f, 1.0f, 1.0f };
    };

    struct WorkerCreativeInputBinding
    {
        std::wstring action;
        std::wstring source;
        std::wstring target;
        float scale = 1.0f;
    };

    struct WorkerCreativeCanvasNode
    {
        std::wstring id;
        std::wstring type;
        WorkerCreativeTransform2D transform;
        WorkerCreativeVec2 size;
        WorkerCreativeVec2 lineEnd;
        std::wstring text;
        float fontSize = 24.0f;
        std::wstring assetId;
        WorkerCreativeColor fill;
        WorkerCreativeColor stroke;
        float strokeWidth = 0.0f;
        float opacity = 1.0f;
        int32_t zIndex = 0;
        bool visible = true;
    };

    struct WorkerCreativeCanvas
    {
        uint32_t width = 1280;
        uint32_t height = 720;
        WorkerCreativeColor background;
        std::vector<WorkerCreativeCanvasNode> nodes;
        std::vector<WorkerCreativeInputBinding> inputBindings;
    };

    struct WorkerCreativeUiElement
    {
        std::wstring id;
        std::wstring type;
        std::wstring parentId;
        std::wstring text;
        std::wstring direction = L"column";
        float width = 0.0f;
        float height = 0.0f;
        float gap = 0.0f;
        float padding = 0.0f;
        std::wstring align = L"start";
        bool focusable = false;
        std::wstring action;
        std::wstring valueState;
        double minimum = 0.0;
        double maximum = 1.0;
        std::wstring visibleState;
        std::wstring styleToken;
    };

    struct WorkerCreativeUi
    {
        std::wstring rootId;
        std::wstring initialFocusId;
        std::vector<WorkerCreativeUiElement> elements;
    };

    struct WorkerCreativeStateDefinition
    {
        std::wstring id;
        std::wstring type;
        WorkerCreativeScalar initial;
    };

    struct WorkerCreativeTimerDefinition
    {
        std::wstring id;
        uint32_t intervalMs = 0;
        bool repeating = false;
        std::wstring event;
    };

    struct WorkerCreativeOperand
    {
        bool fromState = false;
        std::wstring state;
        WorkerCreativeScalar value = false;
    };

    struct WorkerCreativeCondition
    {
        WorkerCreativeOperand left;
        std::wstring operation;
        WorkerCreativeOperand right;
    };

    struct WorkerCreativeAction
    {
        std::wstring type;
        std::wstring target;
        std::wstring source;
        std::optional<WorkerCreativeScalar> value;
        std::wstring event;
        std::vector<double> vector;
        std::wstring message;
    };

    struct WorkerCreativeRule
    {
        std::wstring id;
        std::wstring event;
        std::vector<WorkerCreativeCondition> conditions;
        std::vector<WorkerCreativeAction> actions;
    };

    struct WorkerCreativeBehavior
    {
        std::vector<WorkerCreativeStateDefinition> state;
        std::vector<WorkerCreativeTimerDefinition> timers;
        std::vector<WorkerCreativeRule> rules;
    };

    struct WorkerCreativeLocalStateDefinition
    {
        std::wstring id;
        std::wstring type;
        WorkerCreativeScalar defaultValue;
        uint32_t maxBytes = 0;
    };

    struct WorkerCreativeData
    {
        std::map<std::wstring, WorkerCreativeScalar> constants;
        std::vector<WorkerCreativeLocalStateDefinition> localState;
    };

    struct WorkerCreativeMaterial
    {
        std::wstring id;
        WorkerCreativeColor baseColor;
        WorkerCreativeColor emissive{ 0.0f, 0.0f, 0.0f, 1.0f };
        float roughness = 0.5f;
        float metallic = 0.0f;
    };

    struct WorkerCreativeMesh
    {
        std::wstring id;
        std::wstring primitive;
        std::wstring assetId;
        struct Triangle
        {
            WorkerCreativeVec3 a;
            WorkerCreativeVec3 b;
            WorkerCreativeVec3 c;
            WorkerCreativeVec3 normal;
        };
        std::vector<Triangle> triangles;
    };

    struct WorkerCreativeCollider
    {
        std::wstring shape;
        WorkerCreativeVec3 size{ 1.0f, 1.0f, 1.0f };
        float radius = 0.5f;
        float height = 1.0f;
        bool isTrigger = false;
    };

    struct WorkerCreativeSceneNode
    {
        std::wstring id;
        std::wstring parentId;
        std::wstring meshId;
        std::wstring materialId;
        WorkerCreativeTransform3D transform;
        std::optional<WorkerCreativeCollider> collider;
        bool visible = true;
    };

    struct WorkerCreativeCamera
    {
        std::wstring id;
        WorkerCreativeTransform3D transform;
        float fieldOfViewDegrees = 70.0f;
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
    };

    struct WorkerCreativeLight
    {
        std::wstring id;
        std::wstring type;
        WorkerCreativeColor color;
        float intensity = 1.0f;
        WorkerCreativeVec3 position;
        WorkerCreativeVec3 direction{ 0.0f, -1.0f, 0.0f };
        float range = 10.0f;
    };

    struct WorkerCreativeSceneAnimation
    {
        std::wstring id;
        std::wstring targetNode;
        WorkerCreativeVec3 from;
        WorkerCreativeVec3 to;
        uint32_t durationMs = 0;
        bool loop = false;
        bool autoplay = false;
        std::wstring startEvent;
        std::wstring completeEvent;
    };

    struct WorkerCreativeScene3D
    {
        std::vector<WorkerCreativeMaterial> materials;
        std::vector<WorkerCreativeMesh> meshes;
        std::vector<WorkerCreativeSceneNode> nodes;
        std::vector<WorkerCreativeCamera> cameras;
        std::wstring activeCamera;
        std::vector<WorkerCreativeLight> lights;
        std::vector<WorkerCreativeInputBinding> inputBindings;
        std::vector<WorkerCreativeSceneAnimation> animations;
    };

    struct WorkerCreativeAudioBus
    {
        std::wstring id;
        float gain = 1.0f;
    };

    struct WorkerCreativeAudioClip
    {
        std::wstring id;
        std::wstring assetId;
        std::wstring busId;
        float gain = 1.0f;
        bool loop = false;
    };

    struct WorkerCreativeAudioCue
    {
        std::wstring id;
        std::wstring event;
        std::wstring clipId;
        float gain = 1.0f;
    };

    struct WorkerCreativeAudio
    {
        std::vector<WorkerCreativeAudioBus> buses;
        std::vector<WorkerCreativeAudioClip> clips;
        std::vector<WorkerCreativeAudioCue> cues;
    };

    struct WorkerCreativeXvmInput
    {
        bool fromState = false;
        std::wstring stateId;
        uint32_t constant = 0;
    };

    struct WorkerCreativeXvmOutput
    {
        uint32_t offsetBytes = 0;
        std::wstring stateId;
    };

    struct WorkerCreativeXvmService
    {
        std::wstring id;
        std::wstring triggerEvent;
        std::wstring programAssetId;
        std::vector<WorkerCreativeXvmInput> inputs;
        std::vector<WorkerCreativeXvmOutput> outputs;
        std::wstring successEvent;
        std::wstring failureEvent;
        WorkerXvmProgram program;
    };

    struct WorkerCreativeXvm
    {
        std::vector<WorkerCreativeXvmService> services;
    };

    struct WorkerCreativeAsset
    {
        std::wstring id;
        std::wstring mediaType;
        std::wstring runtimePath;
        std::wstring sha256;
        uint64_t bytes = 0;
        std::filesystem::path casPath;
        uint32_t pixelWidth = 0;
        uint32_t pixelHeight = 0;
        uint32_t pixelStride = 0;
        std::vector<uint8_t> decodedPixels;
        uint16_t audioChannels = 0;
        uint16_t audioBitsPerSample = 0;
        uint16_t audioBlockAlign = 0;
        uint32_t audioSampleRate = 0;
        uint32_t audioAverageBytesPerSecond = 0;
        std::vector<uint8_t> decodedAudio;
    };

    struct WorkerCreativeExecutionPlan
    {
        std::wstring schemaVersion =
            L"xcp-creative-foreground-execution-plan-v1";
        std::wstring planSha256;
        std::wstring projectId;
        std::wstring projectVersion;
        std::wstring installId;
        std::wstring bundleSha256;
        std::wstring contentSha256;
        std::wstring hostProfileCanonicalSha256;
        std::wstring activationRecordSha256;
        uint64_t activationSequence = 0;
        std::wstring entryModule;
        std::optional<WorkerCreativeCanvas> canvas;
        std::optional<WorkerCreativeUi> ui;
        std::optional<WorkerCreativeBehavior> behavior;
        std::optional<WorkerCreativeData> data;
        std::wstring world2dCampaignModuleId;
        std::optional<WorkerCreativeWorld2DCampaign> world2dCampaign;
        std::optional<WorkerCreativeWorld2DMotion> world2dMotion;
        std::map<std::wstring, WorkerCreativeWorld2D> world2dModules;
        std::optional<WorkerCreativeWorld2D> world2d;
        std::optional<WorkerCreativeScene3D> scene3d;
        std::optional<WorkerCreativeAudio> audio;
        std::optional<WorkerCreativeXvm> xvm;
        std::map<std::wstring, WorkerCreativeAsset> assets;
    };

    struct WorkerCreativeSemanticEventObservation
    {
        uint64_t sequence = 0;
        std::wstring event;
        uint64_t stateRevision = 0;
    };

    struct WorkerCreativePhysicalInputObservation
    {
        uint64_t sequence = 0;
        std::wstring source;
        std::wstring focusedElementId;
        std::vector<std::wstring> semanticEvents;
        uint64_t stateRevision = 0;
    };

    struct WorkerCreativeForegroundSnapshot
    {
        std::wstring schemaVersion =
            L"xcp-creative-foreground-snapshot-v1";
        std::shared_ptr<WorkerCreativeExecutionPlan const> plan;
        std::wstring launchId;
        std::wstring launchRecordSha256;
        uint64_t launchSequence = 0;
        uint64_t stateRevision = 0;
        uint64_t frameCount = 0;
        std::map<std::wstring, WorkerCreativeScalar> state;
        std::map<std::wstring, WorkerCreativeScalar> localState;
        std::map<std::wstring, WorkerCreativeVec2> nodeTranslation2D;
        std::map<std::wstring, WorkerCreativeVec3> nodeTranslation3D;
        std::map<std::wstring, bool> nodeVisibility;
        std::wstring focusedElementId;
        std::map<std::wstring, WorkerCreativeTransform3D> cameraTransforms;
        WorkerCreativeWorld2DState world2dState;
        std::vector<std::wstring> logs;
        std::wstring lastEvent;
        uint64_t semanticEventSequence = 0;
        std::vector<WorkerCreativeSemanticEventObservation>
            recentSemanticEvents;
        uint64_t physicalInputSequence = 0;
        std::vector<WorkerCreativePhysicalInputObservation>
            recentPhysicalInputs;
        std::wstring lastErrorCode;
        uint64_t activeAudioVoices = 0;
        uint64_t audioCueCount = 0;
        uint64_t xvmInvocationCount = 0;
        bool loadedFromPersistentLaunch = false;
    };

    struct WorkerCreativeFrameCaptureRequest
    {
        std::wstring captureId;
        std::wstring projectId;
        std::wstring installId;
        std::wstring planSha256;
        uint64_t stateRevision = 0;
        uint64_t frameCount = 0;
    };

    struct WorkerCreativeFrameCaptureResult
    {
        WorkerCreativeFrameCaptureRequest request;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        std::vector<uint8_t> bgra;
        std::wstring errorCode;
    };

    class WorkerCreativeForegroundRuntime final
    {
    public:
        WorkerCreativeForegroundRuntime();
        ~WorkerCreativeForegroundRuntime();

        WorkerCreativeForegroundRuntime(
            WorkerCreativeForegroundRuntime const&) = delete;
        WorkerCreativeForegroundRuntime& operator=(
            WorkerCreativeForegroundRuntime const&) = delete;

        void Initialize(std::filesystem::path const& workerRoot);
        void Tick();
        bool HandleKeyDown(winrt::Windows::System::VirtualKey key);
        void HandleKeyUp(winrt::Windows::System::VirtualKey key);
        void OnSuspending();
        void OnResuming();

        bool HasActiveProject() const;
        std::shared_ptr<WorkerCreativeForegroundSnapshot const>
            Snapshot() const;
        std::optional<WorkerCreativeFrameCaptureRequest>
            TakeFrameCaptureRequest();
        void CompleteFrameCapture(
            WorkerCreativeFrameCaptureResult result);

        std::wstring Launch(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::wstring const& protocolVersion,
            std::filesystem::path const& workerRoot);
        std::wstring Reload(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::wstring const& protocolVersion,
            std::filesystem::path const& workerRoot);
        std::wstring Observe(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::wstring const& protocolVersion) const;
        std::wstring DispatchInput(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::wstring const& protocolVersion);
        std::wstring CaptureFrame(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::wstring const& protocolVersion);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}

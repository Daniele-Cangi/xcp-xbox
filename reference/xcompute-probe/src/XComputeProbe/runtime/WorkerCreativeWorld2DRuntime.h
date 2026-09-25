#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe
{
    struct WorkerCreativeWorld2DCell
    {
        int32_t x = 0;
        int32_t y = 0;

        bool operator==(WorkerCreativeWorld2DCell const& other) const
        {
            return x == other.x && y == other.y;
        }
    };

    struct WorkerCreativeWorld2DColor
    {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    struct WorkerCreativeWorld2DVec2
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct WorkerCreativeWorld2DRect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
    };

    struct WorkerCreativeWorld2DRender
    {
        std::wstring assetId;
        WorkerCreativeWorld2DVec2 offset;
        WorkerCreativeWorld2DVec2 size;
        WorkerCreativeWorld2DRect sourceRect;
        float opacity = 1.0f;
        bool hasSourceRect = false;
    };

    struct WorkerCreativeWorld2DTile
    {
        std::wstring id;
        WorkerCreativeWorld2DCell position;
        std::wstring kind;
        bool solid = false;
        WorkerCreativeWorld2DColor color;
        std::wstring assetId;
        WorkerCreativeWorld2DRender render;
        bool hasAuthoredRender = false;
        bool visible = true;
    };

    struct WorkerCreativeWorld2DActivation
    {
        uint32_t lifetimeTurns = 0;
        uint32_t blastRadius = 0;
        std::wstring inventoryKey;
        uint32_t inventoryCost = 0;
    };

    struct WorkerCreativeWorld2DEntity
    {
        std::wstring id;
        std::wstring archetype;
        WorkerCreativeWorld2DCell position;
        WorkerCreativeWorld2DColor color;
        std::wstring assetId;
        WorkerCreativeWorld2DRender render;
        bool hasAuthoredRender = false;
        bool solid = false;
        bool pushable = false;
        bool destructible = false;
        bool collectible = false;
        bool controllable = false;
        bool gravity = false;
        std::wstring movementMode = L"four_way";
        std::wstring inventoryKey;
        uint32_t inventoryAmount = 1;
        bool hasActivation = false;
        WorkerCreativeWorld2DActivation activation;
        bool initiallyVisible = true;
        int32_t zIndex = 0;
    };

    struct WorkerCreativeWorld2DInputBinding
    {
        std::wstring action;
        std::wstring source;
        std::wstring command;
        std::wstring targetEntity;
        std::wstring templateEntity;
    };

    struct WorkerCreativeWorld2DObjective
    {
        std::wstring id;
        std::wstring type;
        std::wstring target;
        uint32_t count = 1;
        std::wstring completionEvent;
    };

    struct WorkerCreativeWorld2D
    {
        bool authoredV2 = false;
        uint32_t width = 1280;
        uint32_t height = 720;
        WorkerCreativeWorld2DColor background;
        std::wstring backgroundAssetId;
        WorkerCreativeWorld2DVec2 contentOrigin;
        uint32_t columns = 1;
        uint32_t rows = 1;
        float cellSize = 32.0f;
        std::vector<WorkerCreativeWorld2DTile> tiles;
        std::vector<WorkerCreativeWorld2DEntity> entities;
        std::vector<WorkerCreativeWorld2DInputBinding> inputBindings;
        std::vector<WorkerCreativeWorld2DObjective> objectives;
    };

    struct WorkerCreativeWorld2DCampaignScene
    {
        std::wstring id;
        std::wstring moduleId;
        std::wstring packId;
        uint32_t ordinal = 1;
        std::wstring title;
    };

    struct WorkerCreativeWorld2DCampaignTransition
    {
        std::wstring id;
        std::wstring fromScene;
        std::wstring event;
        std::wstring toScene;
    };

    struct WorkerCreativeWorld2DCampaignInputBinding
    {
        std::wstring action;
        std::wstring source;
        std::wstring command;
    };

    struct WorkerCreativeWorld2DCampaign
    {
        std::wstring entryScene;
        std::vector<WorkerCreativeWorld2DCampaignScene> scenes;
        std::vector<WorkerCreativeWorld2DCampaignTransition> transitions;
        std::vector<WorkerCreativeWorld2DCampaignInputBinding> inputBindings;
    };

    struct WorkerCreativeWorld2DMotionBinding
    {
        std::wstring id;
        std::wstring targetEntity;
        WorkerCreativeWorld2DVec2 movementSpeed;
        WorkerCreativeWorld2DVec2 gridStep;
        WorkerCreativeWorld2DVec2 automaticStep;
    };

    struct WorkerCreativeWorld2DMotion
    {
        uint32_t fixedStepHz = 60;
        std::vector<WorkerCreativeWorld2DMotionBinding> bindings;
    };

    struct WorkerCreativeWorld2DState
    {
        std::map<std::wstring, WorkerCreativeWorld2DCell> entityCells;
        std::map<std::wstring, WorkerCreativeWorld2DVec2> entityPositions;
        std::map<std::wstring, bool> entityVisibility;
        std::map<std::wstring, uint32_t> explosiveTurns;
        std::map<std::wstring, uint32_t> inventory;
        std::set<std::wstring> completedObjectives;
        uint64_t turn = 0;
        bool completed = false;
    };

    struct WorkerCreativeWorld2DDispatchResult
    {
        bool handled = false;
        bool changed = false;
        std::vector<std::wstring> events;
    };

    WorkerCreativeWorld2D ParseWorkerCreativeWorld2D(
        winrt::Windows::Data::Json::JsonObject const& module,
        std::string const& path);
    WorkerCreativeWorld2DCampaign ParseWorkerCreativeWorld2DCampaign(
        winrt::Windows::Data::Json::JsonObject const& module,
        std::string const& path);
    WorkerCreativeWorld2DMotion ParseWorkerCreativeWorld2DMotion(
        winrt::Windows::Data::Json::JsonObject const& module,
        std::string const& path);
    void ValidateWorkerCreativeWorld2DMotion(
        WorkerCreativeWorld2DMotion const& motion,
        WorkerCreativeWorld2D const& world,
        std::string const& path);

    class WorkerCreativeWorld2DRuntime final
    {
    public:
        void Activate(
            WorkerCreativeWorld2D const* world,
            WorkerCreativeWorld2DMotion const* motion = nullptr);
        WorkerCreativeWorld2DDispatchResult Dispatch(
            std::wstring const& action);
        WorkerCreativeWorld2DDispatchResult Tick(float seconds);
        WorkerCreativeWorld2DState const& State() const;

    private:
        void Reset();
        bool CellOpen(
            WorkerCreativeWorld2DCell cell,
            std::wstring const& ignoredEntity = {}) const;
        WorkerCreativeWorld2DEntity const* Definition(
            std::wstring const& id) const;
        WorkerCreativeWorld2DTile const* TileAt(
            WorkerCreativeWorld2DCell cell) const;
        bool Move(
            WorkerCreativeWorld2DInputBinding const& binding,
            WorkerCreativeWorld2DCell delta,
            std::vector<std::wstring>& events);
        bool ActivateEntity(
            WorkerCreativeWorld2DInputBinding const& binding,
            std::vector<std::wstring>& events);
        void AdvanceTurn(std::vector<std::wstring>& events);
        void Collect(
            std::wstring const& controllerId,
            std::vector<std::wstring>& events);
        void ApplyGravity(std::vector<std::wstring>& events);
        void EvaluateObjectives(std::vector<std::wstring>& events);
        WorkerCreativeWorld2DMotionBinding const* MotionBinding(
            std::wstring const& entityId) const;
        bool BeginMotion(
            WorkerCreativeWorld2DMotionBinding const& binding,
            WorkerCreativeWorld2DCell target,
            bool automatic);
        void CompleteMotion(
            std::wstring const& entityId,
            bool automatic,
            std::vector<std::wstring>& events);
        void SyncSettledPositions();

        struct MotionProgress
        {
            WorkerCreativeWorld2DVec2 from;
            WorkerCreativeWorld2DVec2 to;
            float elapsed = 0.0f;
            float duration = 0.0f;
            bool automatic = false;
        };

        WorkerCreativeWorld2D const* world_ = nullptr;
        WorkerCreativeWorld2DMotion const* motion_ = nullptr;
        WorkerCreativeWorld2DState state_;
        std::map<std::wstring, MotionProgress> activeMotion_;
        float motionAccumulator_ = 0.0f;
    };
}

#include "pch.h"
#include "WorkerCreativeWorld2DRuntime.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "WorkerCreativeInstallRuntime.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
        [[noreturn]] void FailWorld(
            std::string code,
            std::string message,
            std::string field,
            std::string path,
            std::string expected,
            std::string actual,
            std::string correction)
        {
            WorkerCreativeHostErrorDetails details;
            details.stage = "foreground_plan_validation";
            details.field = std::move(field);
            details.path = std::move(path);
            details.expected = std::move(expected);
            details.actual = std::move(actual);
            details.correction = std::move(correction);
            throw WorkerCreativeHostError(
                std::move(code),
                std::move(message),
                std::move(details));
        }

        std::string Narrow(std::wstring const& value)
        {
            return winrt::to_string(winrt::hstring(value));
        }

        bool SafeIdentifier(
            std::wstring const& value,
            size_t maximum,
            bool contract)
        {
            if (value.empty() ||
                value.size() > maximum ||
                value.front() < L'a' ||
                value.front() > L'z')
            {
                return false;
            }
            bool separator = false;
            bool hasSeparator = false;
            for (auto ch : value)
            {
                auto alphaNumeric =
                    (ch >= L'a' && ch <= L'z') ||
                    (ch >= L'0' && ch <= L'9');
                auto currentSeparator =
                    ch == L'.' || ch == L'_' || ch == L'-';
                if (!alphaNumeric && !currentSeparator)
                    return false;
                if (separator && currentSeparator)
                    return false;
                separator = currentSeparator;
                hasSeparator = hasSeparator || currentSeparator;
            }
            return !separator && (!contract || hasSeparator);
        }

        void RequireFields(
            JsonObject const& object,
            std::set<std::wstring> const& allowed,
            std::string const& path)
        {
            for (auto const& field : object)
            {
                auto key = std::wstring(field.Key());
                if (allowed.find(key) == allowed.end())
                {
                    FailWorld(
                        "xcp.creative.module_schema_rejected",
                        "world2d document contains an unknown field",
                        Narrow(key),
                        path,
                        "only fields declared by xcp-world2d-module-v1",
                        Narrow(key),
                        "remove the unknown field and rebuild");
                }
            }
        }

        std::wstring RequiredString(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            size_t maximum = 4096)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() !=
                    JsonValueType::String)
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d string field is missing or invalid",
                    Narrow(name),
                    path,
                    "string",
                    "missing or non-string",
                    "correct the module using its published schema");
            }
            auto value = std::wstring(object.GetNamedString(name));
            if (value.size() > maximum)
            {
                FailWorld(
                    "xcp.creative.module_budget_exceeded",
                    "world2d string exceeds its bound",
                    Narrow(name),
                    path,
                    "at most " + std::to_string(maximum) + " characters",
                    std::to_string(value.size()),
                    "shorten the value");
            }
            return value;
        }

        std::wstring OptionalString(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            size_t maximum = 4096)
        {
            return object.HasKey(name)
                ? RequiredString(object, name, path, maximum)
                : std::wstring();
        }

        JsonObject RequiredObject(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() !=
                    JsonValueType::Object)
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d object field is missing or invalid",
                    Narrow(name),
                    path,
                    "object",
                    "missing or non-object",
                    "correct the module using its published schema");
            }
            return object.GetNamedObject(name);
        }

        JsonArray RequiredArray(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            uint32_t minimum,
            uint32_t maximum)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() !=
                    JsonValueType::Array)
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d array field is missing or invalid",
                    Narrow(name),
                    path,
                    "array",
                    "missing or non-array",
                    "correct the module using its published schema");
            }
            auto result = object.GetNamedArray(name);
            if (result.Size() < minimum || result.Size() > maximum)
            {
                FailWorld(
                    "xcp.creative.module_budget_exceeded",
                    "world2d array exceeds its bound",
                    Narrow(name),
                    path,
                    std::to_string(minimum) + ".." +
                        std::to_string(maximum) + " items",
                    std::to_string(result.Size()),
                    "reduce the module");
            }
            return result;
        }

        JsonObject ArrayObject(
            JsonArray const& array,
            uint32_t index,
            std::string const& path)
        {
            auto value = array.GetAt(index);
            if (value.ValueType() != JsonValueType::Object)
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d array item must be an object",
                    "item",
                    path + "[" + std::to_string(index) + "]",
                    "object",
                    "non-object",
                    "correct the module using its published schema");
            }
            return value.GetObject();
        }

        int64_t RequiredInteger(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            int64_t minimum,
            int64_t maximum)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() !=
                    JsonValueType::Number)
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d integer field is missing or invalid",
                    Narrow(name),
                    path,
                    "integer",
                    "missing or non-number",
                    "correct the module using its published schema");
            }
            auto value = object.GetNamedNumber(name);
            if (!std::isfinite(value) ||
                std::floor(value) != value ||
                value < static_cast<double>(minimum) ||
                value > static_cast<double>(maximum))
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d integer is outside its bound",
                    Narrow(name),
                    path,
                    std::to_string(minimum) + ".." +
                        std::to_string(maximum),
                    std::to_string(value),
                    "use an integer inside the published bound");
            }
            return static_cast<int64_t>(value);
        }

        double RequiredNumber(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            double minimum,
            double maximum,
            bool exclusiveMinimum = false)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() !=
                    JsonValueType::Number)
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d number field is missing or invalid",
                    Narrow(name),
                    path,
                    "finite number",
                    "missing or non-number",
                    "correct the module using its published schema");
            }
            auto value = object.GetNamedNumber(name);
            if (!std::isfinite(value) ||
                (exclusiveMinimum ? value <= minimum : value < minimum) ||
                value > maximum)
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d number is outside its bound",
                    Narrow(name),
                    path,
                    (exclusiveMinimum ? "greater than " : "at least ") +
                        std::to_string(minimum) + " and at most " +
                        std::to_string(maximum),
                    std::to_string(value),
                    "use a number inside the published bound");
            }
            return value;
        }

        bool RequiredBoolean(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path)
        {
            if (!object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() !=
                    JsonValueType::Boolean)
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d Boolean field is missing or invalid",
                    Narrow(name),
                    path,
                    "boolean",
                    "missing or non-boolean",
                    "correct the module using its published schema");
            }
            return object.GetNamedBoolean(name);
        }

        WorkerCreativeWorld2DCell RequiredCell(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            uint32_t columns,
            uint32_t rows)
        {
            auto array = RequiredArray(object, name, path, 2, 2);
            WorkerCreativeWorld2DCell result;
            for (uint32_t index = 0; index < 2; ++index)
            {
                auto value = array.GetAt(index);
                if (value.ValueType() != JsonValueType::Number ||
                    !std::isfinite(value.GetNumber()) ||
                    std::floor(value.GetNumber()) != value.GetNumber())
                {
                    FailWorld(
                        "xcp.creative.module_schema_rejected",
                        "world2d cell component is invalid",
                        Narrow(name),
                        path,
                        "two bounded integers",
                        "invalid component",
                        "correct the cell coordinate");
                }
                auto coordinate =
                    static_cast<int32_t>(value.GetNumber());
                auto maximum = index == 0
                    ? static_cast<int32_t>(columns)
                    : static_cast<int32_t>(rows);
                if (coordinate < 0 || coordinate >= maximum)
                {
                    FailWorld(
                        "xcp.creative.world_cell_out_of_bounds",
                        "world2d cell lies outside declared dimensions",
                        Narrow(name),
                        path,
                        "cell inside columns and rows",
                        std::to_string(coordinate),
                        "move the item inside the world");
                }
                (index == 0 ? result.x : result.y) = coordinate;
            }
            return result;
        }

        void RequireIdentifier(
            std::wstring const& value,
            std::string const& field,
            std::string const& path,
            bool contract = false);

        WorkerCreativeWorld2DVec2 RequiredVec2(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path,
            double minimum,
            double maximum,
            bool exclusiveMinimum = false)
        {
            auto array = RequiredArray(object, name, path, 2, 2);
            WorkerCreativeWorld2DVec2 result;
            for (uint32_t index = 0; index < 2; ++index)
            {
                auto value = array.GetAt(index);
                if (value.ValueType() != JsonValueType::Number ||
                    !std::isfinite(value.GetNumber()) ||
                    (exclusiveMinimum
                        ? value.GetNumber() <= minimum
                        : value.GetNumber() < minimum) ||
                    value.GetNumber() > maximum)
                {
                    FailWorld(
                        "xcp.creative.module_schema_rejected",
                        "world2d vector component is invalid",
                        Narrow(name),
                        path,
                        "two finite bounded numbers",
                        "invalid component",
                        "correct the authored vector");
                }
                (index == 0 ? result.x : result.y) =
                    static_cast<float>(value.GetNumber());
            }
            return result;
        }

        WorkerCreativeWorld2DRect RequiredRect(
            JsonObject const& object,
            wchar_t const* name,
            std::string const& path)
        {
            auto array = RequiredArray(object, name, path, 4, 4);
            WorkerCreativeWorld2DRect result;
            for (uint32_t index = 0; index < 4; ++index)
            {
                auto value = array.GetAt(index);
                auto minimum = 0.0;
                auto exclusive = index >= 2;
                if (value.ValueType() != JsonValueType::Number ||
                    !std::isfinite(value.GetNumber()) ||
                    (exclusive
                        ? value.GetNumber() <= minimum
                        : value.GetNumber() < minimum) ||
                    value.GetNumber() > 4096.0)
                {
                    FailWorld(
                        "xcp.creative.module_schema_rejected",
                        "world2d source rectangle is invalid",
                        Narrow(name),
                        path,
                        "x/y >= 0 and width/height in 0..4096",
                        "invalid component",
                        "correct the authored sprite region");
                }
                auto component = static_cast<float>(value.GetNumber());
                if (index == 0) result.x = component;
                else if (index == 1) result.y = component;
                else if (index == 2) result.width = component;
                else result.height = component;
            }
            return result;
        }

        WorkerCreativeWorld2DRender RequiredRender(
            JsonObject const& object,
            std::string const& path)
        {
            RequireFields(
                object,
                {
                    L"asset_id", L"source_rect", L"offset",
                    L"size", L"opacity"
                },
                path);
            WorkerCreativeWorld2DRender result;
            result.assetId = RequiredString(
                object, L"asset_id", path, 96);
            RequireIdentifier(result.assetId, "asset_id", path);
            result.offset = RequiredVec2(
                object, L"offset", path, -8192.0, 8192.0);
            result.size = RequiredVec2(
                object, L"size", path, 0.0, 8192.0, true);
            result.opacity = static_cast<float>(
                RequiredNumber(
                    object,
                    L"opacity",
                    path,
                    0.0,
                    1.0));
            if (object.HasKey(L"source_rect"))
            {
                result.sourceRect = RequiredRect(
                    object, L"source_rect", path);
                result.hasSourceRect = true;
            }
            return result;
        }

        uint8_t Nibble(wchar_t value)
        {
            if (value >= L'0' && value <= L'9')
                return static_cast<uint8_t>(value - L'0');
            if (value >= L'a' && value <= L'f')
                return static_cast<uint8_t>(10 + value - L'a');
            if (value >= L'A' && value <= L'F')
                return static_cast<uint8_t>(10 + value - L'A');
            return 0xff;
        }

        WorkerCreativeWorld2DColor ParseColor(
            std::wstring const& value,
            std::string const& path)
        {
            if ((value.size() == 7 || value.size() == 9) &&
                value.front() == L'#')
            {
                auto byte = [&](size_t offset)
                {
                    auto high = Nibble(value[offset]);
                    auto low = Nibble(value[offset + 1]);
                    if (high == 0xff || low == 0xff)
                    {
                        FailWorld(
                            "xcp.creative.module_schema_rejected",
                            "world2d color contains invalid digits",
                            "color",
                            path,
                            "#RRGGBB or #RRGGBBAA",
                            Narrow(value),
                            "correct the color");
                    }
                    return static_cast<uint8_t>(
                        (high << 4) | low);
                };
                return {
                    byte(1) / 255.0f,
                    byte(3) / 255.0f,
                    byte(5) / 255.0f,
                    value.size() == 9
                        ? byte(7) / 255.0f
                        : 1.0f
                };
            }
            if (SafeIdentifier(value, 64, false))
            {
                uint32_t hash = 2166136261u;
                for (auto ch : value)
                {
                    hash ^= static_cast<uint32_t>(ch);
                    hash *= 16777619u;
                }
                return {
                    0.25f + ((hash >> 0) & 0xff) / 510.0f,
                    0.25f + ((hash >> 8) & 0xff) / 510.0f,
                    0.25f + ((hash >> 16) & 0xff) / 510.0f,
                    1.0f
                };
            }
            FailWorld(
                "xcp.creative.module_schema_rejected",
                "world2d color is invalid",
                "color",
                path,
                "#RRGGBB, #RRGGBBAA or a style token",
                Narrow(value),
                "correct the color");
        }

        void RequireIdentifier(
            std::wstring const& value,
            std::string const& field,
            std::string const& path,
            bool contract)
        {
            if (!SafeIdentifier(
                    value,
                    contract ? 128 : 96,
                    contract))
            {
                FailWorld(
                    "xcp.creative.module_identity_invalid",
                    "world2d identifier is invalid",
                    field,
                    path,
                    contract
                        ? "bounded lowercase contract id"
                        : "bounded lowercase identifier",
                    Narrow(value),
                    "use the identifier syntax published by the schema");
            }
        }

        bool SameCell(
            WorkerCreativeWorld2DCell const& left,
            WorkerCreativeWorld2DCell const& right)
        {
            return left.x == right.x && left.y == right.y;
        }
    }

    WorkerCreativeWorld2D ParseWorkerCreativeWorld2D(
        JsonObject const& module,
        std::string const& path)
    {
        RequireFields(
            module,
            {
                L"schema_version", L"world", L"tiles", L"entities",
                L"input_bindings", L"objectives"
            },
            path);
        auto schemaVersion =
            RequiredString(module, L"schema_version", path);
        auto authoredV2 =
            schemaVersion == L"xcp-world2d-module-v2";
        if (schemaVersion != L"xcp-world2d-module-v1" &&
            !authoredV2)
        {
            FailWorld(
                "xcp.creative.module_schema_version_mismatch",
                "world2d module schema version is unsupported",
                "schema_version",
                path,
                "xcp-world2d-module-v1 or xcp-world2d-module-v2",
                "other",
                "use the schema published by describe_creative_host");
        }

        WorkerCreativeWorld2D result;
        result.authoredV2 = authoredV2;
        auto world = RequiredObject(module, L"world", path);
        RequireFields(
            world,
            authoredV2
                ? std::set<std::wstring>{
                    L"width", L"height", L"background",
                    L"background_asset_id", L"columns", L"rows",
                    L"cell_size", L"content_origin"
                }
                : std::set<std::wstring>{
                    L"width", L"height", L"background", L"columns",
                    L"rows", L"cell_size"
                },
            path + ".world");
        result.width = static_cast<uint32_t>(
            RequiredInteger(world, L"width", path + ".world", 1, 8192));
        result.height = static_cast<uint32_t>(
            RequiredInteger(world, L"height", path + ".world", 1, 8192));
        result.background = ParseColor(
            RequiredString(world, L"background", path + ".world", 64),
            path + ".world.background");
        if (authoredV2)
        {
            result.backgroundAssetId = OptionalString(
                world, L"background_asset_id", path + ".world", 96);
            if (!result.backgroundAssetId.empty())
            {
                RequireIdentifier(
                    result.backgroundAssetId,
                    "background_asset_id",
                    path + ".world");
            }
            result.contentOrigin = RequiredVec2(
                world,
                L"content_origin",
                path + ".world",
                -8192.0,
                8192.0);
        }
        result.columns = static_cast<uint32_t>(
            RequiredInteger(world, L"columns", path + ".world", 1, 512));
        result.rows = static_cast<uint32_t>(
            RequiredInteger(world, L"rows", path + ".world", 1, 512));
        result.cellSize = static_cast<float>(
            RequiredNumber(
                world,
                L"cell_size",
                path + ".world",
                0.0,
                512.0,
                true));

        std::set<std::wstring> tileIds;
        std::set<std::pair<int32_t, int32_t>> tileCells;
        auto tiles = RequiredArray(module, L"tiles", path, 0, 65536);
        for (uint32_t index = 0; index < tiles.Size(); ++index)
        {
            auto itemPath =
                path + ".tiles[" + std::to_string(index) + "]";
            auto object = ArrayObject(tiles, index, path + ".tiles");
            RequireFields(
                object,
                authoredV2
                    ? std::set<std::wstring>{
                        L"id", L"position", L"kind", L"solid",
                        L"color", L"render", L"visible"
                    }
                    : std::set<std::wstring>{
                        L"id", L"position", L"kind", L"solid",
                        L"color", L"asset_id", L"visible"
                    },
                itemPath);
            WorkerCreativeWorld2DTile tile;
            tile.id = RequiredString(object, L"id", itemPath, 96);
            RequireIdentifier(tile.id, "id", itemPath);
            if (!tileIds.insert(tile.id).second)
            {
                FailWorld(
                    "xcp.creative.module_identity_duplicate",
                    "world2d tile id is duplicated",
                    "id",
                    itemPath,
                    "unique tile id",
                    Narrow(tile.id),
                    "rename the duplicate tile");
            }
            tile.position = RequiredCell(
                object,
                L"position",
                itemPath,
                result.columns,
                result.rows);
            if (!tileCells.emplace(
                    tile.position.x,
                    tile.position.y).second)
            {
                FailWorld(
                    "xcp.creative.world_tile_overlap",
                    "world2d admits one tile per cell",
                    "position",
                    itemPath,
                    "unique tile cell",
                    std::to_string(tile.position.x) + "," +
                        std::to_string(tile.position.y),
                    "merge or move the overlapping tile");
            }
            tile.kind = RequiredString(object, L"kind", itemPath, 32);
            if (tile.kind != L"floor" &&
                tile.kind != L"solid" &&
                tile.kind != L"hazard" &&
                tile.kind != L"goal" &&
                tile.kind != L"climbable" &&
                tile.kind != L"decorative")
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d tile kind is unsupported",
                    "kind",
                    itemPath,
                    "published tile kind",
                    Narrow(tile.kind),
                    "select a kind from the schema");
            }
            tile.solid = RequiredBoolean(
                object, L"solid", itemPath);
            tile.color = ParseColor(
                RequiredString(object, L"color", itemPath, 64),
                itemPath + ".color");
            if (authoredV2 && object.HasKey(L"render"))
            {
                tile.render = RequiredRender(
                    RequiredObject(object, L"render", itemPath),
                    itemPath + ".render");
                tile.assetId = tile.render.assetId;
                tile.hasAuthoredRender = true;
            }
            else
            {
                tile.assetId = OptionalString(
                    object, L"asset_id", itemPath, 96);
                if (!tile.assetId.empty())
                    RequireIdentifier(tile.assetId, "asset_id", itemPath);
            }
            tile.visible = RequiredBoolean(
                object, L"visible", itemPath);
            result.tiles.push_back(std::move(tile));
        }

        std::set<std::wstring> entityIds;
        std::set<std::wstring> destructibleArchetypes;
        std::set<std::wstring> collectibleKeys;
        std::set<std::pair<int32_t, int32_t>> occupiedSolidCells;
        auto entities = RequiredArray(
            module, L"entities", path, 1, 4096);
        for (uint32_t index = 0; index < entities.Size(); ++index)
        {
            auto itemPath =
                path + ".entities[" + std::to_string(index) + "]";
            auto object = ArrayObject(
                entities, index, path + ".entities");
            RequireFields(
                object,
                authoredV2
                    ? std::set<std::wstring>{
                        L"id", L"archetype", L"position", L"color",
                        L"render", L"solid", L"pushable",
                        L"destructible", L"collectible",
                        L"controllable", L"gravity", L"movement_mode",
                        L"inventory_key", L"inventory_amount",
                        L"activation", L"initially_visible", L"z_index"
                    }
                    : std::set<std::wstring>{
                        L"id", L"archetype", L"position", L"color",
                        L"asset_id", L"solid", L"pushable",
                        L"destructible", L"collectible",
                        L"controllable", L"inventory_key",
                        L"inventory_amount", L"activation",
                        L"initially_visible", L"z_index"
                    },
                itemPath);
            WorkerCreativeWorld2DEntity entity;
            entity.id = RequiredString(object, L"id", itemPath, 96);
            entity.archetype = RequiredString(
                object, L"archetype", itemPath, 96);
            RequireIdentifier(entity.id, "id", itemPath);
            RequireIdentifier(entity.archetype, "archetype", itemPath);
            if (!entityIds.insert(entity.id).second)
            {
                FailWorld(
                    "xcp.creative.module_identity_duplicate",
                    "world2d entity id is duplicated",
                    "id",
                    itemPath,
                    "unique entity id",
                    Narrow(entity.id),
                    "rename the duplicate entity");
            }
            entity.position = RequiredCell(
                object,
                L"position",
                itemPath,
                result.columns,
                result.rows);
            entity.color = ParseColor(
                RequiredString(object, L"color", itemPath, 64),
                itemPath + ".color");
            if (authoredV2 && object.HasKey(L"render"))
            {
                entity.render = RequiredRender(
                    RequiredObject(object, L"render", itemPath),
                    itemPath + ".render");
                entity.assetId = entity.render.assetId;
                entity.hasAuthoredRender = true;
            }
            else
            {
                entity.assetId = OptionalString(
                    object, L"asset_id", itemPath, 96);
                if (!entity.assetId.empty())
                    RequireIdentifier(entity.assetId, "asset_id", itemPath);
            }
            entity.solid = RequiredBoolean(
                object, L"solid", itemPath);
            entity.pushable = RequiredBoolean(
                object, L"pushable", itemPath);
            entity.destructible = RequiredBoolean(
                object, L"destructible", itemPath);
            entity.collectible = RequiredBoolean(
                object, L"collectible", itemPath);
            entity.controllable = RequiredBoolean(
                object, L"controllable", itemPath);
            if (authoredV2)
            {
                entity.gravity = RequiredBoolean(
                    object, L"gravity", itemPath);
                entity.movementMode = RequiredString(
                    object, L"movement_mode", itemPath, 32);
                if (entity.movementMode != L"four_way" &&
                    entity.movementMode != L"platform_grid")
                {
                    FailWorld(
                        "xcp.creative.module_schema_rejected",
                        "world2d movement mode is unsupported",
                        "movement_mode",
                        itemPath,
                        "four_way or platform_grid",
                        Narrow(entity.movementMode),
                        "select a published movement mode");
                }
            }
            if (entity.pushable && !entity.solid)
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "pushable world2d entity must also be solid",
                    "pushable/solid",
                    itemPath,
                    "pushable implies solid",
                    "pushable non-solid entity",
                    "make the entity solid or remove pushable");
            }
            entity.inventoryKey = OptionalString(
                object, L"inventory_key", itemPath, 96);
            if (!entity.inventoryKey.empty())
            {
                RequireIdentifier(
                    entity.inventoryKey,
                    "inventory_key",
                    itemPath);
            }
            entity.inventoryAmount = static_cast<uint32_t>(
                object.HasKey(L"inventory_amount")
                    ? RequiredInteger(
                        object,
                        L"inventory_amount",
                        itemPath,
                        1,
                        1000000)
                    : 1);
            if (entity.collectible)
            {
                if (entity.inventoryKey.empty())
                {
                    FailWorld(
                        "xcp.creative.module_reference_invalid",
                        "collectible world2d entity has no inventory key",
                        "inventory_key",
                        itemPath,
                        "declared inventory key",
                        "missing",
                        "bind the collectible to bounded inventory");
                }
                collectibleKeys.insert(entity.inventoryKey);
            }
            if (entity.destructible)
                destructibleArchetypes.insert(entity.archetype);
            if (object.HasKey(L"activation"))
            {
                auto activation = RequiredObject(
                    object, L"activation", itemPath);
                RequireFields(
                    activation,
                    {
                        L"lifetime_turns", L"blast_radius",
                        L"inventory_key", L"inventory_cost"
                    },
                    itemPath + ".activation");
                entity.hasActivation = true;
                entity.activation.lifetimeTurns =
                    static_cast<uint32_t>(
                        RequiredInteger(
                            activation,
                            L"lifetime_turns",
                            itemPath + ".activation",
                            1,
                            1000));
                entity.activation.blastRadius =
                    static_cast<uint32_t>(
                        RequiredInteger(
                            activation,
                            L"blast_radius",
                            itemPath + ".activation",
                            0,
                            32));
                entity.activation.inventoryKey =
                    OptionalString(
                        activation,
                        L"inventory_key",
                        itemPath + ".activation",
                        96);
                if (!entity.activation.inventoryKey.empty())
                    RequireIdentifier(
                        entity.activation.inventoryKey,
                        "inventory_key",
                        itemPath + ".activation");
                entity.activation.inventoryCost =
                    static_cast<uint32_t>(
                        activation.HasKey(L"inventory_cost")
                            ? RequiredInteger(
                                activation,
                                L"inventory_cost",
                                itemPath + ".activation",
                                0,
                                1000000)
                            : 0);
            }
            entity.initiallyVisible = RequiredBoolean(
                object, L"initially_visible", itemPath);
            if (entity.solid && entity.initiallyVisible &&
                !occupiedSolidCells.emplace(
                    entity.position.x,
                    entity.position.y).second)
            {
                FailWorld(
                    "xcp.creative.world_entity_overlap",
                    "visible solid world2d entities overlap",
                    "position",
                    itemPath,
                    "unique cell for each visible solid entity",
                    std::to_string(entity.position.x) + "," +
                        std::to_string(entity.position.y),
                    "move one of the solid entities");
            }
            if (entity.initiallyVisible &&
                std::any_of(
                    result.tiles.begin(),
                    result.tiles.end(),
                    [&](auto const& tile)
                    {
                        return tile.visible && tile.solid &&
                            SameCell(tile.position, entity.position);
                    }))
            {
                FailWorld(
                    "xcp.creative.world_entity_tile_overlap",
                    "visible world2d entity starts inside a solid tile",
                    "position",
                    itemPath,
                    "non-solid starting cell",
                    std::to_string(entity.position.x) + "," +
                        std::to_string(entity.position.y),
                    "move the entity or make the tile non-solid");
            }
            entity.zIndex = static_cast<int32_t>(
                object.HasKey(L"z_index")
                    ? RequiredInteger(
                        object,
                        L"z_index",
                        itemPath,
                        -32768,
                        32767)
                    : 0);
            result.entities.push_back(std::move(entity));
        }
        for (auto const& entity : result.entities)
        {
            if (entity.hasActivation &&
                entity.activation.inventoryCost != 0 &&
                (entity.activation.inventoryKey.empty() ||
                    collectibleKeys.find(
                        entity.activation.inventoryKey) ==
                        collectibleKeys.end()))
            {
                FailWorld(
                    "xcp.creative.module_reference_invalid",
                    "world2d activation consumes an undeclared inventory key",
                    "activation.inventory_key",
                    path + ".entities",
                    "inventory key produced by a collectible entity",
                    Narrow(entity.activation.inventoryKey),
                    "correct the activation inventory binding");
            }
        }

        std::set<std::wstring> actions;
        auto bindings = RequiredArray(
            module, L"input_bindings", path, 1, 256);
        for (uint32_t index = 0; index < bindings.Size(); ++index)
        {
            auto itemPath =
                path + ".input_bindings[" +
                std::to_string(index) + "]";
            auto object = ArrayObject(
                bindings, index, path + ".input_bindings");
            RequireFields(
                object,
                {
                    L"action", L"source", L"command",
                    L"target_entity", L"template_entity"
                },
                itemPath);
            WorkerCreativeWorld2DInputBinding binding;
            binding.action = RequiredString(
                object, L"action", itemPath, 128);
            binding.source = RequiredString(
                object, L"source", itemPath, 64);
            binding.command = RequiredString(
                object, L"command", itemPath, 32);
            binding.targetEntity = RequiredString(
                object, L"target_entity", itemPath, 96);
            binding.templateEntity = OptionalString(
                object, L"template_entity", itemPath, 96);
            RequireIdentifier(binding.action, "action", itemPath, true);
            RequireIdentifier(
                binding.targetEntity, "target_entity", itemPath);
            if (!binding.templateEntity.empty())
                RequireIdentifier(
                    binding.templateEntity,
                    "template_entity",
                    itemPath);
            if (!actions.insert(binding.action).second)
            {
                FailWorld(
                    "xcp.creative.module_identity_duplicate",
                    "world2d action is duplicated",
                    "action",
                    itemPath,
                    "one binding per semantic action",
                    Narrow(binding.action),
                    "merge or rename the action");
            }
            if (binding.source != L"gamepad.dpad_up" &&
                binding.source != L"gamepad.dpad_down" &&
                binding.source != L"gamepad.dpad_left" &&
                binding.source != L"gamepad.dpad_right" &&
                binding.source != L"gamepad.a" &&
                binding.source != L"gamepad.b" &&
                binding.source != L"gamepad.x" &&
                binding.source != L"gamepad.y" &&
                binding.source != L"keyboard.up" &&
                binding.source != L"keyboard.down" &&
                binding.source != L"keyboard.left" &&
                binding.source != L"keyboard.right" &&
                binding.source != L"keyboard.space" &&
                binding.source != L"keyboard.r")
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d physical input source is unsupported",
                    "source",
                    itemPath,
                    "published bounded physical input source",
                    Narrow(binding.source),
                    "select a source from the module schema");
            }
            auto target = std::find_if(
                result.entities.begin(),
                result.entities.end(),
                [&](auto const& item)
                {
                    return item.id == binding.targetEntity;
                });
            if (target == result.entities.end() ||
                !target->controllable)
            {
                FailWorld(
                    "xcp.creative.module_reference_invalid",
                    "world2d input target is missing or not controllable",
                    "target_entity",
                    itemPath,
                    "declared controllable entity",
                    Narrow(binding.targetEntity),
                    "correct the input target");
            }
            if (binding.command != L"move_up" &&
                binding.command != L"move_down" &&
                binding.command != L"move_left" &&
                binding.command != L"move_right" &&
                binding.command != L"activate" &&
                binding.command != L"reset")
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d command is unsupported",
                    "command",
                    itemPath,
                    "published world2d command",
                    Narrow(binding.command),
                    "select a command from the schema");
            }
            if (binding.command == L"activate")
            {
                auto templateEntity = std::find_if(
                    result.entities.begin(),
                    result.entities.end(),
                    [&](auto const& item)
                    {
                        return item.id == binding.templateEntity;
                    });
                if (templateEntity == result.entities.end() ||
                    !templateEntity->hasActivation ||
                    templateEntity->initiallyVisible)
                {
                    FailWorld(
                        "xcp.creative.module_reference_invalid",
                        "world2d activation template is invalid",
                        "template_entity",
                        itemPath,
                        "hidden entity with activation policy",
                        Narrow(binding.templateEntity),
                        "correct the activation template");
                }
            }
            result.inputBindings.push_back(std::move(binding));
        }

        std::set<std::wstring> objectiveIds;
        auto objectives = RequiredArray(
            module, L"objectives", path, 0, 256);
        for (uint32_t index = 0; index < objectives.Size(); ++index)
        {
            auto itemPath =
                path + ".objectives[" +
                std::to_string(index) + "]";
            auto object = ArrayObject(
                objectives, index, path + ".objectives");
            RequireFields(
                object,
                {
                    L"id", L"type", L"target", L"count",
                    L"completion_event"
                },
                itemPath);
            WorkerCreativeWorld2DObjective objective;
            objective.id = RequiredString(
                object, L"id", itemPath, 96);
            objective.type = RequiredString(
                object, L"type", itemPath, 32);
            objective.target = RequiredString(
                object, L"target", itemPath, 96);
            objective.count = static_cast<uint32_t>(
                RequiredInteger(
                    object,
                    L"count",
                    itemPath,
                    1,
                    1000000));
            objective.completionEvent = RequiredString(
                object, L"completion_event", itemPath, 128);
            RequireIdentifier(objective.id, "id", itemPath);
            RequireIdentifier(
                objective.target, "target", itemPath);
            RequireIdentifier(
                objective.completionEvent,
                "completion_event",
                itemPath,
                true);
            if (!objectiveIds.insert(objective.id).second)
            {
                FailWorld(
                    "xcp.creative.module_identity_duplicate",
                    "world2d objective id is duplicated",
                    "id",
                    itemPath,
                    "unique objective id",
                    Narrow(objective.id),
                    "rename the duplicate objective");
            }
            bool targetValid = false;
            if (objective.type == L"reach_tile")
            {
                targetValid = tileIds.find(objective.target) !=
                    tileIds.end();
            }
            else if (objective.type == L"collect_count")
            {
                targetValid = collectibleKeys.find(objective.target) !=
                    collectibleKeys.end();
            }
            else if (objective.type == L"destroy_all")
            {
                targetValid =
                    destructibleArchetypes.find(objective.target) !=
                    destructibleArchetypes.end();
            }
            else
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d objective type is unsupported",
                    "type",
                    itemPath,
                    "reach_tile, collect_count or destroy_all",
                    Narrow(objective.type),
                    "select a type from the schema");
            }
            if (!targetValid)
            {
                FailWorld(
                    "xcp.creative.module_reference_invalid",
                    "world2d objective target is missing",
                    "target",
                    itemPath,
                    "declared tile, inventory key or archetype",
                    Narrow(objective.target),
                    "correct the objective target");
            }
            result.objectives.push_back(std::move(objective));
        }

        std::stable_sort(
            result.entities.begin(),
            result.entities.end(),
            [](auto const& left, auto const& right)
            {
                return left.zIndex < right.zIndex;
            });
        return result;
    }

    WorkerCreativeWorld2DCampaign ParseWorkerCreativeWorld2DCampaign(
        JsonObject const& module,
        std::string const& path)
    {
        RequireFields(
            module,
            {
                L"schema_version", L"entry_scene", L"scenes",
                L"transitions", L"input_bindings"
            },
            path);
        auto schemaVersion =
            RequiredString(module, L"schema_version", path);
        if (schemaVersion != L"xcp-world2d-campaign-module-v1")
        {
            FailWorld(
                "xcp.creative.module_schema_version_mismatch",
                "world2d campaign schema version is unsupported",
                "schema_version",
                path,
                "xcp-world2d-campaign-module-v1",
                Narrow(schemaVersion),
                "use the schema published by describe_creative_host");
        }

        WorkerCreativeWorld2DCampaign result;
        result.entryScene =
            RequiredString(module, L"entry_scene", path, 96);
        RequireIdentifier(result.entryScene, "entry_scene", path);

        std::set<std::wstring> sceneIds;
        std::set<std::wstring> moduleIds;
        auto scenes = RequiredArray(module, L"scenes", path, 2, 128);
        for (uint32_t index = 0; index < scenes.Size(); ++index)
        {
            auto itemPath =
                path + ".scenes[" + std::to_string(index) + "]";
            auto object = ArrayObject(scenes, index, path + ".scenes");
            RequireFields(
                object,
                {
                    L"id", L"module_id", L"pack_id", L"ordinal",
                    L"title"
                },
                itemPath);
            WorkerCreativeWorld2DCampaignScene scene;
            scene.id = RequiredString(object, L"id", itemPath, 96);
            scene.moduleId = RequiredString(
                object, L"module_id", itemPath, 96);
            scene.packId = RequiredString(
                object, L"pack_id", itemPath, 96);
            scene.ordinal = static_cast<uint32_t>(
                RequiredInteger(
                    object,
                    L"ordinal",
                    itemPath,
                    1,
                    1024));
            scene.title = RequiredString(
                object, L"title", itemPath, 256);
            RequireIdentifier(scene.id, "id", itemPath);
            RequireIdentifier(scene.moduleId, "module_id", itemPath);
            RequireIdentifier(scene.packId, "pack_id", itemPath);
            if (!sceneIds.insert(scene.id).second ||
                !moduleIds.insert(scene.moduleId).second)
            {
                FailWorld(
                    "xcp.creative.module_identity_duplicate",
                    "world2d campaign scene or module id is duplicated",
                    "id/module_id",
                    itemPath,
                    "unique scene and module ids",
                    Narrow(scene.id + L"/" + scene.moduleId),
                    "rename the duplicate scene or module");
            }
            result.scenes.push_back(std::move(scene));
        }
        if (sceneIds.find(result.entryScene) == sceneIds.end())
        {
            FailWorld(
                "xcp.creative.module_reference_invalid",
                "world2d campaign entry scene is missing",
                "entry_scene",
                path,
                "declared scene id",
                Narrow(result.entryScene),
                "select a declared entry scene");
        }

        std::set<std::wstring> transitionIds;
        std::set<std::wstring> transitionSources;
        auto transitions = RequiredArray(
            module, L"transitions", path, 1, 256);
        for (uint32_t index = 0; index < transitions.Size(); ++index)
        {
            auto itemPath =
                path + ".transitions[" + std::to_string(index) + "]";
            auto object = ArrayObject(
                transitions, index, path + ".transitions");
            RequireFields(
                object,
                { L"id", L"from_scene", L"event", L"to_scene" },
                itemPath);
            WorkerCreativeWorld2DCampaignTransition transition;
            transition.id = RequiredString(
                object, L"id", itemPath, 96);
            transition.fromScene = RequiredString(
                object, L"from_scene", itemPath, 96);
            transition.event = RequiredString(
                object, L"event", itemPath, 128);
            transition.toScene = RequiredString(
                object, L"to_scene", itemPath, 96);
            RequireIdentifier(transition.id, "id", itemPath);
            RequireIdentifier(
                transition.fromScene, "from_scene", itemPath);
            RequireIdentifier(
                transition.event, "event", itemPath, true);
            RequireIdentifier(
                transition.toScene, "to_scene", itemPath);
            if (transition.event != L"world.level-completed" ||
                sceneIds.find(transition.fromScene) == sceneIds.end() ||
                sceneIds.find(transition.toScene) == sceneIds.end() ||
                transition.fromScene == transition.toScene)
            {
                FailWorld(
                    "xcp.creative.module_reference_invalid",
                    "world2d campaign transition is invalid",
                    "from_scene/event/to_scene",
                    itemPath,
                    "two distinct declared scenes and world.level-completed",
                    Narrow(
                        transition.fromScene + L"/" +
                        transition.event + L"/" +
                        transition.toScene),
                    "correct the transition binding");
            }
            if (!transitionIds.insert(transition.id).second ||
                !transitionSources.insert(transition.fromScene).second)
            {
                FailWorld(
                    "xcp.creative.module_identity_duplicate",
                    "world2d campaign transition is duplicated or ambiguous",
                    "id/from_scene",
                    itemPath,
                    "unique transition and one successor per scene",
                    Narrow(transition.id),
                    "remove the duplicate or ambiguous transition");
            }
            result.transitions.push_back(std::move(transition));
        }

        std::set<std::wstring> actions;
        auto bindings = RequiredArray(
            module, L"input_bindings", path, 1, 16);
        for (uint32_t index = 0; index < bindings.Size(); ++index)
        {
            auto itemPath =
                path + ".input_bindings[" +
                std::to_string(index) + "]";
            auto object = ArrayObject(
                bindings, index, path + ".input_bindings");
            RequireFields(
                object,
                { L"action", L"source", L"command" },
                itemPath);
            WorkerCreativeWorld2DCampaignInputBinding binding;
            binding.action = RequiredString(
                object, L"action", itemPath, 128);
            binding.source = RequiredString(
                object, L"source", itemPath, 64);
            binding.command = RequiredString(
                object, L"command", itemPath, 32);
            RequireIdentifier(binding.action, "action", itemPath, true);
            if ((binding.source != L"gamepad.a" &&
                 binding.source != L"keyboard.space") ||
                binding.command != L"advance_scene")
            {
                FailWorld(
                    "xcp.creative.module_schema_rejected",
                    "world2d campaign input binding is unsupported",
                    "source/command",
                    itemPath,
                    "gamepad.a or keyboard.space with advance_scene",
                    Narrow(binding.source + L"/" + binding.command),
                    "use the published campaign navigation binding");
            }
            if (!actions.insert(binding.action).second)
            {
                FailWorld(
                    "xcp.creative.module_identity_duplicate",
                    "world2d campaign action is duplicated",
                    "action",
                    itemPath,
                    "unique campaign action",
                    Narrow(binding.action),
                    "remove the duplicate binding");
            }
            result.inputBindings.push_back(std::move(binding));
        }
        return result;
    }

    WorkerCreativeWorld2DMotion ParseWorkerCreativeWorld2DMotion(
        JsonObject const& module,
        std::string const& path)
    {
        RequireFields(
            module,
            { L"schema_version", L"fixed_step_hz", L"bindings" },
            path);
        auto schemaVersion = RequiredString(
            module, L"schema_version", path, 64);
        if (schemaVersion != L"xcp-world2d-motion-module-v1")
        {
            FailWorld(
                "xcp.creative.module_schema_rejected",
                "world2d motion schema version is unsupported",
                "schema_version",
                path,
                "xcp-world2d-motion-module-v1",
                Narrow(schemaVersion),
                "emit the published world2d motion schema");
        }

        WorkerCreativeWorld2DMotion result;
        result.fixedStepHz = static_cast<uint32_t>(
            RequiredInteger(
                module,
                L"fixed_step_hz",
                path,
                1,
                240));
        auto bindings = RequiredArray(
            module, L"bindings", path, 1, 4096);
        std::set<std::wstring> ids;
        std::set<std::wstring> targets;
        for (uint32_t index = 0; index < bindings.Size(); ++index)
        {
            auto itemPath =
                path + ".bindings[" + std::to_string(index) + "]";
            auto object = ArrayObject(bindings, index, path + ".bindings");
            RequireFields(
                object,
                {
                    L"id", L"target_entity", L"movement_speed",
                    L"grid_step", L"automatic_step"
                },
                itemPath);
            WorkerCreativeWorld2DMotionBinding binding;
            binding.id = RequiredString(object, L"id", itemPath, 96);
            binding.targetEntity = RequiredString(
                object, L"target_entity", itemPath, 96);
            RequireIdentifier(binding.id, "id", itemPath);
            RequireIdentifier(
                binding.targetEntity, "target_entity", itemPath);
            binding.movementSpeed = RequiredVec2(
                object,
                L"movement_speed",
                itemPath,
                0.0,
                8192.0,
                true);
            binding.gridStep = RequiredVec2(
                object,
                L"grid_step",
                itemPath,
                0.0,
                8192.0,
                true);
            binding.automaticStep = RequiredVec2(
                object,
                L"automatic_step",
                itemPath,
                -8192.0,
                8192.0);
            if (!ids.insert(binding.id).second ||
                !targets.insert(binding.targetEntity).second)
            {
                FailWorld(
                    "xcp.creative.module_identity_duplicate",
                    "world2d motion binding id or target is duplicated",
                    "id/target_entity",
                    itemPath,
                    "unique binding id and target entity",
                    Narrow(binding.id + L"/" + binding.targetEntity),
                    "remove the duplicate motion binding");
            }
            result.bindings.push_back(std::move(binding));
        }
        return result;
    }

    void ValidateWorkerCreativeWorld2DMotion(
        WorkerCreativeWorld2DMotion const& motion,
        WorkerCreativeWorld2D const& world,
        std::string const& path)
    {
        constexpr float tolerance = 0.0001f;
        for (auto const& binding : motion.bindings)
        {
            auto entity = std::find_if(
                world.entities.begin(),
                world.entities.end(),
                [&](auto const& item)
                {
                    return item.id == binding.targetEntity;
                });
            if (entity == world.entities.end())
            {
                FailWorld(
                    "xcp.creative.module_reference_invalid",
                    "world2d motion target entity is missing",
                    "target_entity",
                    path,
                    "entity present in every composed world2d module",
                    Narrow(binding.targetEntity),
                    "correct the motion binding or world composition");
            }
            if (std::fabs(binding.gridStep.x - world.cellSize) > tolerance ||
                std::fabs(binding.gridStep.y - world.cellSize) > tolerance)
            {
                FailWorld(
                    "xcp.creative.module_reference_invalid",
                    "world2d motion grid does not match the composed world",
                    "grid_step",
                    path,
                    "both components equal world.cell_size",
                    std::to_string(binding.gridStep.x) + "/" +
                        std::to_string(binding.gridStep.y),
                    "lower motion against the exact world grid");
            }
            auto exactCellDelta = [&](float value, float grid)
            {
                auto cells = value / grid;
                return std::fabs(cells - std::round(cells)) <= tolerance &&
                    std::fabs(cells) <= 128.0f;
            };
            if (!exactCellDelta(
                    binding.automaticStep.x,
                    binding.gridStep.x) ||
                !exactCellDelta(
                    binding.automaticStep.y,
                    binding.gridStep.y))
            {
                FailWorld(
                    "xcp.creative.module_reference_invalid",
                    "world2d automatic motion is not aligned to its grid",
                    "automatic_step",
                    path,
                    "bounded integral grid-cell delta",
                    std::to_string(binding.automaticStep.x) + "/" +
                        std::to_string(binding.automaticStep.y),
                    "lower an automatic step aligned to grid_step");
            }
            if ((binding.automaticStep.x != 0.0f ||
                 binding.automaticStep.y != 0.0f) &&
                !entity->gravity)
            {
                FailWorld(
                    "xcp.creative.module_reference_invalid",
                    "world2d automatic motion requires a gravity entity",
                    "target_entity",
                    path,
                    "entity with gravity=true",
                    Narrow(binding.targetEntity),
                    "mark the semantic entity as gravity-enabled or remove the automatic step");
            }
        }
    }

    void WorkerCreativeWorld2DRuntime::Activate(
        WorkerCreativeWorld2D const* world,
        WorkerCreativeWorld2DMotion const* motion)
    {
        world_ = world;
        motion_ = motion;
        Reset();
    }

    void WorkerCreativeWorld2DRuntime::Reset()
    {
        state_ = {};
        activeMotion_.clear();
        motionAccumulator_ = 0.0f;
        if (!world_)
            return;
        for (auto const& entity : world_->entities)
        {
            state_.entityCells.emplace(
                entity.id,
                entity.position);
            state_.entityPositions.emplace(
                entity.id,
                WorkerCreativeWorld2DVec2{
                    static_cast<float>(entity.position.x),
                    static_cast<float>(entity.position.y)
                });
            state_.entityVisibility.emplace(
                entity.id,
                entity.initiallyVisible);
        }
    }

    WorkerCreativeWorld2DState const&
        WorkerCreativeWorld2DRuntime::State() const
    {
        return state_;
    }

    WorkerCreativeWorld2DMotionBinding const*
        WorkerCreativeWorld2DRuntime::MotionBinding(
            std::wstring const& entityId) const
    {
        if (!motion_)
            return nullptr;
        auto found = std::find_if(
            motion_->bindings.begin(),
            motion_->bindings.end(),
            [&](auto const& item)
            {
                return item.targetEntity == entityId;
            });
        return found == motion_->bindings.end() ? nullptr : &*found;
    }

    void WorkerCreativeWorld2DRuntime::SyncSettledPositions()
    {
        if (!world_)
            return;
        for (auto const& entity : world_->entities)
        {
            if (activeMotion_.find(entity.id) != activeMotion_.end())
                continue;
            auto cell = state_.entityCells.find(entity.id);
            if (cell == state_.entityCells.end())
                continue;
            state_.entityPositions[entity.id] = {
                static_cast<float>(cell->second.x),
                static_cast<float>(cell->second.y)
            };
        }
    }

    bool WorkerCreativeWorld2DRuntime::BeginMotion(
        WorkerCreativeWorld2DMotionBinding const& binding,
        WorkerCreativeWorld2DCell target,
        bool automatic)
    {
        if (activeMotion_.find(binding.targetEntity) !=
                activeMotion_.end())
        {
            return false;
        }
        auto currentCell = state_.entityCells.at(binding.targetEntity);
        auto currentPosition =
            state_.entityPositions.at(binding.targetEntity);
        auto dx = target.x - currentCell.x;
        auto dy = target.y - currentCell.y;
        auto durationX = std::fabs(
            static_cast<float>(dx) * binding.gridStep.x) /
            binding.movementSpeed.x;
        auto durationY = std::fabs(
            static_cast<float>(dy) * binding.gridStep.y) /
            binding.movementSpeed.y;
        auto duration = (std::max)(durationX, durationY);
        if (!std::isfinite(duration) || duration <= 0.0f)
            return false;

        MotionProgress progress;
        progress.from = currentPosition;
        progress.to = {
            static_cast<float>(target.x),
            static_cast<float>(target.y)
        };
        progress.duration = duration;
        progress.automatic = automatic;
        state_.entityCells[binding.targetEntity] = target;
        activeMotion_.emplace(binding.targetEntity, progress);
        return true;
    }

    void WorkerCreativeWorld2DRuntime::CompleteMotion(
        std::wstring const& entityId,
        bool automatic,
        std::vector<std::wstring>& events)
    {
        auto definition = Definition(entityId);
        auto binding = MotionBinding(entityId);
        if (!definition || !binding)
            return;
        auto current = state_.entityCells.at(entityId);
        state_.entityPositions[entityId] = {
            static_cast<float>(current.x),
            static_cast<float>(current.y)
        };
        Collect(entityId, events);
        auto tile = TileAt(current);
        if (tile && tile->kind == L"hazard")
        {
            state_.entityCells[entityId] = definition->position;
            state_.entityPositions[entityId] = {
                static_cast<float>(definition->position.x),
                static_cast<float>(definition->position.y)
            };
            events.push_back(L"world.hazard-reset");
            EvaluateObjectives(events);
            return;
        }
        if (automatic)
            events.push_back(L"world.entity-fell");
        EvaluateObjectives(events);

        if (!definition->gravity ||
            (binding->automaticStep.x == 0.0f &&
             binding->automaticStep.y == 0.0f))
        {
            return;
        }
        WorkerCreativeWorld2DCell delta{
            static_cast<int32_t>(std::round(
                binding->automaticStep.x / binding->gridStep.x)),
            static_cast<int32_t>(std::round(
                binding->automaticStep.y / binding->gridStep.y))
        };
        WorkerCreativeWorld2DCell next{
            current.x + delta.x,
            current.y + delta.y
        };
        if (!world_ ||
            next.x < 0 ||
            next.y < 0 ||
            next.x >= static_cast<int32_t>(world_->columns) ||
            next.y >= static_cast<int32_t>(world_->rows))
        {
            if (definition->controllable)
            {
                state_.entityCells[entityId] = definition->position;
                state_.entityPositions[entityId] = {
                    static_cast<float>(definition->position.x),
                    static_cast<float>(definition->position.y)
                };
                events.push_back(L"world.fall-reset");
            }
            else if (definition->destructible)
            {
                state_.entityVisibility[entityId] = false;
                events.push_back(L"world.entity-destroyed");
            }
            return;
        }
        auto nextTile = TileAt(next);
        if ((nextTile && nextTile->solid) ||
            !CellOpen(next, entityId))
        {
            return;
        }
        BeginMotion(*binding, next, true);
    }

    WorkerCreativeWorld2DEntity const*
        WorkerCreativeWorld2DRuntime::Definition(
            std::wstring const& id) const
    {
        if (!world_)
            return nullptr;
        auto found = std::find_if(
            world_->entities.begin(),
            world_->entities.end(),
            [&](auto const& item)
            {
                return item.id == id;
            });
        return found == world_->entities.end() ? nullptr : &*found;
    }

    WorkerCreativeWorld2DTile const*
        WorkerCreativeWorld2DRuntime::TileAt(
            WorkerCreativeWorld2DCell cell) const
    {
        if (!world_)
            return nullptr;
        auto found = std::find_if(
            world_->tiles.begin(),
            world_->tiles.end(),
            [&](auto const& tile)
            {
                return tile.visible && SameCell(tile.position, cell);
            });
        return found == world_->tiles.end() ? nullptr : &*found;
    }

    bool WorkerCreativeWorld2DRuntime::CellOpen(
        WorkerCreativeWorld2DCell cell,
        std::wstring const& ignoredEntity) const
    {
        if (!world_ ||
            cell.x < 0 ||
            cell.y < 0 ||
            cell.x >= static_cast<int32_t>(world_->columns) ||
            cell.y >= static_cast<int32_t>(world_->rows))
        {
            return false;
        }
        auto tile = TileAt(cell);
        if (tile && tile->solid)
            return false;
        for (auto const& entity : world_->entities)
        {
            if (entity.id == ignoredEntity ||
                !entity.solid ||
                state_.entityVisibility.at(entity.id) == false)
            {
                continue;
            }
            if (SameCell(state_.entityCells.at(entity.id), cell))
                return false;
        }
        return true;
    }

    bool WorkerCreativeWorld2DRuntime::Move(
        WorkerCreativeWorld2DInputBinding const& binding,
        WorkerCreativeWorld2DCell delta,
        std::vector<std::wstring>& events)
    {
        auto definition = Definition(binding.targetEntity);
        auto motionBinding = MotionBinding(binding.targetEntity);
        if (!definition ||
            !state_.entityVisibility[binding.targetEntity])
        {
            return false;
        }
        if (motionBinding &&
            activeMotion_.find(binding.targetEntity) !=
                activeMotion_.end())
        {
            events.push_back(L"world.motion-busy");
            return false;
        }
        auto current = state_.entityCells[binding.targetEntity];
        WorkerCreativeWorld2DCell next{
            current.x + delta.x,
            current.y + delta.y
        };
        if (definition->movementMode == L"platform_grid" &&
            delta.y != 0)
        {
            auto currentTile = TileAt(current);
            auto nextTile = TileAt(next);
            auto onClimbable =
                currentTile &&
                currentTile->kind == L"climbable";
            auto enteringClimbable =
                nextTile &&
                nextTile->kind == L"climbable";
            if (!onClimbable && !enteringClimbable)
            {
                events.push_back(L"world.climb-blocked");
                return false;
            }
        }
        if (!world_ ||
            next.x < 0 ||
            next.y < 0 ||
            next.x >= static_cast<int32_t>(world_->columns) ||
            next.y >= static_cast<int32_t>(world_->rows))
        {
            events.push_back(L"world.collision-blocked");
            return false;
        }
        auto tile = TileAt(next);
        if (tile && tile->solid)
        {
            events.push_back(L"world.collision-blocked");
            return false;
        }
        for (auto const& entity : world_->entities)
        {
            if (entity.id == binding.targetEntity ||
                !entity.solid ||
                !state_.entityVisibility[entity.id] ||
                !SameCell(state_.entityCells[entity.id], next))
            {
                continue;
            }
            if (!entity.pushable)
            {
                events.push_back(L"world.collision-blocked");
                return false;
            }
            if (definition->movementMode == L"platform_grid" &&
                delta.y != 0)
            {
                events.push_back(L"world.push-blocked");
                return false;
            }
            WorkerCreativeWorld2DCell pushed{
                next.x + delta.x,
                next.y + delta.y
            };
            if (!CellOpen(pushed, entity.id))
            {
                events.push_back(L"world.push-blocked");
                return false;
            }
            state_.entityCells[entity.id] = pushed;
            events.push_back(L"world.entity-pushed");
            auto pushedTile = TileAt(pushed);
            if (pushedTile &&
                pushedTile->kind == L"hazard" &&
                entity.destructible)
            {
                state_.entityVisibility[entity.id] = false;
                events.push_back(L"world.entity-destroyed");
            }
        }
        if (motionBinding)
        {
            if (!BeginMotion(*motionBinding, next, false))
                return false;
            events.push_back(binding.action);
            AdvanceTurn(events);
            return true;
        }
        state_.entityCells[binding.targetEntity] = next;
        events.push_back(binding.action);
        Collect(binding.targetEntity, events);
        tile = TileAt(next);
        if (tile && tile->kind == L"hazard")
        {
            state_.entityCells[binding.targetEntity] =
                definition->position;
            events.push_back(L"world.hazard-reset");
        }
        AdvanceTurn(events);
        return true;
    }

    void WorkerCreativeWorld2DRuntime::Collect(
        std::wstring const& controllerId,
        std::vector<std::wstring>& events)
    {
        auto position = state_.entityCells[controllerId];
        for (auto const& entity : world_->entities)
        {
            if (!entity.collectible ||
                !state_.entityVisibility[entity.id] ||
                !SameCell(state_.entityCells[entity.id], position))
            {
                continue;
            }
            auto& value = state_.inventory[entity.inventoryKey];
            value = static_cast<uint32_t>(
                (std::min<uint64_t>)(
                    1000000ull,
                    static_cast<uint64_t>(value) +
                        entity.inventoryAmount));
            state_.entityVisibility[entity.id] = false;
            events.push_back(L"world.item-collected");
        }
    }

    bool WorkerCreativeWorld2DRuntime::ActivateEntity(
        WorkerCreativeWorld2DInputBinding const& binding,
        std::vector<std::wstring>& events)
    {
        auto definition = Definition(binding.templateEntity);
        if (!definition ||
            !definition->hasActivation ||
            state_.entityVisibility[binding.templateEntity])
        {
            events.push_back(L"world.activation-unavailable");
            return false;
        }
        auto const& activation = definition->activation;
        if (!activation.inventoryKey.empty())
        {
            auto& available =
                state_.inventory[activation.inventoryKey];
            if (available < activation.inventoryCost)
            {
                events.push_back(L"world.inventory-insufficient");
                return false;
            }
            available -= activation.inventoryCost;
        }
        state_.entityCells[binding.templateEntity] =
            state_.entityCells[binding.targetEntity];
        state_.entityVisibility[binding.templateEntity] = true;
        state_.explosiveTurns[binding.templateEntity] =
            activation.lifetimeTurns + 1;
        events.push_back(binding.action);
        events.push_back(L"world.entity-activated");
        AdvanceTurn(events);
        return true;
    }

    void WorkerCreativeWorld2DRuntime::AdvanceTurn(
        std::vector<std::wstring>& events)
    {
        ++state_.turn;
        std::vector<std::wstring> explode;
        for (auto& entry : state_.explosiveTurns)
        {
            if (entry.second > 0)
                --entry.second;
            if (entry.second == 0)
                explode.push_back(entry.first);
        }
        for (auto const& id : explode)
        {
            auto definition = Definition(id);
            auto center = state_.entityCells[id];
            auto radius = definition
                ? definition->activation.blastRadius
                : 0;
            bool destroyed = false;
            for (auto const& entity : world_->entities)
            {
                if (!entity.destructible ||
                    !state_.entityVisibility[entity.id])
                {
                    continue;
                }
                auto position = state_.entityCells[entity.id];
                auto distance =
                    std::abs(position.x - center.x) +
                    std::abs(position.y - center.y);
                if (distance <= static_cast<int32_t>(radius))
                {
                    state_.entityVisibility[entity.id] = false;
                    destroyed = true;
                }
            }
            if (destroyed)
                events.push_back(L"world.entity-destroyed");
            state_.entityVisibility[id] = false;
            state_.explosiveTurns.erase(id);
            events.push_back(L"world.explosion");
        }
        ApplyGravity(events);
        EvaluateObjectives(events);
    }

    void WorkerCreativeWorld2DRuntime::ApplyGravity(
        std::vector<std::wstring>& events)
    {
        if (!world_)
            return;
        auto maximumPasses =
            static_cast<uint64_t>(world_->rows) *
            static_cast<uint64_t>((std::max<size_t>)(
                1,
                world_->entities.size()));
        for (uint64_t pass = 0; pass < maximumPasses; ++pass)
        {
            bool changed = false;
            for (auto const& entity : world_->entities)
            {
                if (!entity.gravity ||
                    !state_.entityVisibility[entity.id] ||
                    MotionBinding(entity.id) != nullptr)
                {
                    continue;
                }
                auto current = state_.entityCells[entity.id];
                WorkerCreativeWorld2DCell below{
                    current.x,
                    current.y + 1
                };
                if (below.y >= static_cast<int32_t>(world_->rows))
                {
                    if (entity.controllable)
                    {
                        state_.entityCells[entity.id] =
                            entity.position;
                        events.push_back(L"world.fall-reset");
                    }
                    else if (entity.destructible)
                    {
                        state_.entityVisibility[entity.id] = false;
                        events.push_back(L"world.entity-destroyed");
                    }
                    changed = true;
                    continue;
                }
                auto tile = TileAt(below);
                if (tile && tile->solid)
                    continue;
                if (!CellOpen(below, entity.id))
                    continue;
                state_.entityCells[entity.id] = below;
                changed = true;
                if (tile && tile->kind == L"hazard")
                {
                    if (entity.controllable)
                    {
                        state_.entityCells[entity.id] =
                            entity.position;
                        events.push_back(L"world.hazard-reset");
                    }
                    else if (entity.destructible)
                    {
                        state_.entityVisibility[entity.id] = false;
                        events.push_back(L"world.entity-destroyed");
                    }
                }
                else
                {
                    events.push_back(L"world.entity-fell");
                }
            }
            if (!changed)
                break;
        }
    }

    void WorkerCreativeWorld2DRuntime::EvaluateObjectives(
        std::vector<std::wstring>& events)
    {
        for (auto const& objective : world_->objectives)
        {
            if (state_.completedObjectives.find(objective.id) !=
                state_.completedObjectives.end())
            {
                continue;
            }
            bool complete = false;
            if (objective.type == L"reach_tile")
            {
                auto tile = std::find_if(
                    world_->tiles.begin(),
                    world_->tiles.end(),
                    [&](auto const& item)
                    {
                        return item.id == objective.target;
                    });
                if (tile != world_->tiles.end())
                {
                    complete = std::any_of(
                        world_->entities.begin(),
                        world_->entities.end(),
                        [&](auto const& entity)
                        {
                            return entity.controllable &&
                                state_.entityVisibility[entity.id] &&
                                activeMotion_.find(entity.id) ==
                                    activeMotion_.end() &&
                                SameCell(
                                    state_.entityCells[entity.id],
                                    tile->position);
                        });
                }
            }
            else if (objective.type == L"collect_count")
            {
                complete =
                    state_.inventory[objective.target] >=
                    objective.count;
            }
            else if (objective.type == L"destroy_all")
            {
                complete = std::none_of(
                    world_->entities.begin(),
                    world_->entities.end(),
                    [&](auto const& entity)
                    {
                        return entity.archetype == objective.target &&
                            state_.entityVisibility[entity.id];
                    });
            }
            if (complete)
            {
                state_.completedObjectives.insert(objective.id);
                events.push_back(objective.completionEvent);
            }
        }
        auto completed =
            !world_->objectives.empty() &&
            state_.completedObjectives.size() ==
            world_->objectives.size();
        if (completed && !state_.completed)
        {
            state_.completed = true;
            events.push_back(L"world.completed");
        }
    }

    WorkerCreativeWorld2DDispatchResult
        WorkerCreativeWorld2DRuntime::Dispatch(
            std::wstring const& action)
    {
        WorkerCreativeWorld2DDispatchResult result;
        if (!world_)
            return result;
        auto binding = std::find_if(
            world_->inputBindings.begin(),
            world_->inputBindings.end(),
            [&](auto const& item)
            {
                return item.action == action;
            });
        if (binding == world_->inputBindings.end())
            return result;
        result.handled = true;
        if (binding->command == L"move_up")
            result.changed = Move(*binding, { 0, -1 }, result.events);
        else if (binding->command == L"move_down")
            result.changed = Move(*binding, { 0, 1 }, result.events);
        else if (binding->command == L"move_left")
            result.changed = Move(*binding, { -1, 0 }, result.events);
        else if (binding->command == L"move_right")
            result.changed = Move(*binding, { 1, 0 }, result.events);
        else if (binding->command == L"activate")
            result.changed = ActivateEntity(*binding, result.events);
        else if (binding->command == L"reset")
        {
            Reset();
            result.changed = true;
            result.events.push_back(binding->action);
            result.events.push_back(L"world.reset");
        }
        SyncSettledPositions();
        return result;
    }

    WorkerCreativeWorld2DDispatchResult
        WorkerCreativeWorld2DRuntime::Tick(float seconds)
    {
        WorkerCreativeWorld2DDispatchResult result;
        if (!world_ || !motion_ || activeMotion_.empty())
            return result;
        result.handled = true;
        auto fixedSeconds = 1.0f /
            static_cast<float>(motion_->fixedStepHz);
        motionAccumulator_ = (std::min)(
            motionAccumulator_ + (std::max)(0.0f, seconds),
            fixedSeconds * 32.0f);
        uint32_t steps = 0;
        while (motionAccumulator_ + 0.000001f >= fixedSeconds &&
               steps < 32)
        {
            motionAccumulator_ -= fixedSeconds;
            ++steps;
            std::vector<std::pair<std::wstring, bool>> completed;
            for (auto& [entityId, progress] : activeMotion_)
            {
                progress.elapsed = (std::min)(
                    progress.duration,
                    progress.elapsed + fixedSeconds);
                auto fraction = progress.duration > 0.0f
                    ? progress.elapsed / progress.duration
                    : 1.0f;
                state_.entityPositions[entityId] = {
                    progress.from.x +
                        (progress.to.x - progress.from.x) * fraction,
                    progress.from.y +
                        (progress.to.y - progress.from.y) * fraction
                };
                result.changed = true;
                if (progress.elapsed >= progress.duration)
                {
                    completed.emplace_back(
                        entityId,
                        progress.automatic);
                }
            }
            for (auto const& [entityId, automatic] : completed)
            {
                activeMotion_.erase(entityId);
                CompleteMotion(entityId, automatic, result.events);
            }
        }
        SyncSettledPositions();
        return result;
    }
}

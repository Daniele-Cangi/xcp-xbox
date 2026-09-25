namespace XComputeControlCenter.Core;

public enum StudioArea
{
    Home,
    Create,
    Projects,
    Adapt,
    Evolve,
    Playtest,
    Devices,
    Evidence,
}

public sealed record StudioNavigationItem(
    StudioArea Area,
    string Key,
    string Title,
    string Description,
    IReadOnlyList<string> Operations);

public static class StudioNavigation
{
    public static IReadOnlyList<StudioNavigationItem> Items { get; } =
    [
        new(
            StudioArea.Home,
            "home",
            "Home",
            "Start from an idea or an existing project, then follow one shared XCP lifecycle.",
            ["describe", "describe_creative_host"]),
        new(
            StudioArea.Create,
            "create",
            "Create from zero",
            "Turn a new idea into an ordinary XCP project and its C5 intent.",
            ["create", "init-intent", "init-ledger", "validate", "build"]),
        new(
            StudioArea.Projects,
            "projects",
            "Project workspace",
            "Guided and expert editors share one SHA-bound xcp-project.json.",
            [
                "open",
                "save_guided",
                "save_expert_source",
                "validate",
                "build",
                "verify",
            ]),
        new(
            StudioArea.Adapt,
            "adapt",
            "Adapt external source",
            "Inspect external source, emit Creative IR and generate an ordinary XCP project through C6.",
            [
                "xcp_source_adapt.py detect",
                "xcp_source_adapt.py adapt",
                "xcp_source_adapt.py finalize",
            ]),
        new(
            StudioArea.Evolve,
            "evolve",
            "Evolve an adaptation",
            "Reconcile a later source revision with a hash-bound XCP overlay through C7.",
            [
                "xcp_project_evolve.py evolve",
                "xcp_project_evolve.py finalize",
            ]),
        new(
            StudioArea.Playtest,
            "playtest",
            "Playtest",
            "Run install through cleanup once through the universal C5 lifecycle.",
            ["xcp_agent_lifecycle.py run-live"]),
        new(
            StudioArea.Devices,
            "devices",
            "Xbox device",
            "Connect with ephemeral authentication and discover the exact Creative Host profile.",
            ["describe_creative_host"]),
        new(
            StudioArea.Evidence,
            "evidence",
            "Evidence",
            "Inspect the latest structured result without collapsing project, bundle, capture or receipt identities.",
            ["observe_creative_foreground", "capture_creative_frame"]),
    ];

    public static StudioNavigationItem ByKey(string key) =>
        Items.FirstOrDefault(
            item => item.Key.Equals(key, StringComparison.OrdinalIgnoreCase))
        ?? throw new ArgumentOutOfRangeException(
            nameof(key),
            key,
            "Unknown Studio navigation key.");
}

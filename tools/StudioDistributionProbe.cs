using System.Text.Json;
using XComputeControlCenter.Core;

if (args.Length != 2)
{
    throw new ArgumentException("Expected distribution root and disposable work directory.");
}
var distribution = Path.GetFullPath(args[0]);
var work = Path.GetFullPath(args[1]);
Directory.CreateDirectory(work);
var tool = Path.Combine(distribution, "toolchain", "tools", "xcp_creative_project.py");
var python = Path.Combine(distribution, "runtime", "python", "python.exe");
var project = Path.Combine(work, "new project with spaces");
var bundle = Path.Combine(work, "bundle with spaces");
var recentPath = Path.Combine(work, "recent-projects-v1.json");
var client = new XcpCreativeCliClient(tool, python);
var created = await client.CreateAsync(project, "development-probe", "Development Probe");
Require(created.Ok, "create");
var workspace = new StudioProjectWorkspace();
var opened = await workspace.OpenAsync(project);
Require(opened.ProjectId == "development-probe", "open");
var edited = await workspace.SaveGuidedAsync(project, opened.FileSha256,
    "Development Probe Edited", "PC distribution workflow.", "1.0.1");
Require(edited.FileSha256 != opened.FileSha256, "save guided edit");
var store = new StudioRecentProjectStore(recentPath);
var recent = await store.RecordAsync(edited, "created");
Require(recent.Count == 1 && recent[0].Status == StudioRecentProjectStatus.Available,
    "record recent project");
Require((await client.ValidateAsync(project)).Ok, "validate");
Require((await client.BuildAsync(project, bundle)).Ok, "build");
Require((await client.VerifyAsync(bundle)).Ok, "verify bundle");

// Discard Studio's in-memory workspace and store, then reopen from disk.
workspace = new StudioProjectWorkspace();
store = new StudioRecentProjectStore(recentPath);
var loaded = await store.LoadAsync();
Require(loaded.Count == 1 && loaded[0].Status == StudioRecentProjectStatus.Available,
    "reopen recent projects");
var reopened = await workspace.OpenAsync(loaded[0].ProjectDirectory);
Require(reopened.FileSha256 == edited.FileSha256 && reopened.Title == "Development Probe Edited",
    "reopen project content");
Require((await client.ValidateAsync(reopened.ProjectDirectory)).Ok, "validate reopened project");

var example = Path.Combine(distribution, "examples", "hello-shapes");
Require((await client.ValidateAsync(example)).Ok, "validate shipped example");
var exampleBundle = Path.Combine(work, "example bundle");
Require((await client.BuildAsync(example, exampleBundle)).Ok, "build shipped example");
Require((await client.VerifyAsync(exampleBundle)).Ok, "verify shipped example");
Console.WriteLine(JsonSerializer.Serialize(new
{
    ok = true,
    hardware_validation = "NOT_TESTED_ON_XBOX",
    path = "extracted Studio Core + extracted pinned Python + extracted toolchain",
    operations = new[] { "create", "open", "save", "validate", "build", "verify",
        "close", "reopen", "validate_reopened", "validate_example", "build_example", "verify_example" }
}));

static void Require(bool condition, string operation)
{
    if (!condition) throw new InvalidOperationException($"Studio distribution operation failed: {operation}");
}

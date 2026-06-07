// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

using UnrealBuildTool;

[SupportedPlatforms(UnrealPlatformClass.Desktop)]
public class TraceDigestTarget : TargetRules
{
	public TraceDigestTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Program;
		// Monolithic: all engine modules statically linked into one executable.
		// We chose this over Modular for two reasons:
		//   1. Cleaner release artifact — a single ~30–50 MB binary instead of
		//      an exe plus ~10 libTraceDigest-<Module>.so files. The .so form
		//      ships compiled engine modules in a separately-distributable
		//      library shape, which is more uncomfortable under UE's EULA.
		//   2. Easier user experience — drop the binary anywhere and run it,
		//      no need to keep neighbouring libraries together.
		LinkType = TargetLinkType.Monolithic;
		LaunchModuleName = "TraceDigest";

		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion  = EngineIncludeOrderVersion.Latest;

		// TraceServices lives under Developer/, so we need the developer
		// modules in the build graph even though we don't pull in the editor.
		bBuildDeveloperTools = true;
		bBuildWithEditorOnlyData = false;
		bCompileAgainstEngine = false;
		bCompileAgainstCoreUObject = true;   // FName, UObject reflection that TraceServices needs internally
		bCompileAgainstApplicationCore = false;
		bIsBuildingConsoleApplication = true;
		bUseLoggingInShipping = true;

		SolutionDirectory = "Programs";
	}
}

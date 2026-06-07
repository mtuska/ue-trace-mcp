// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

using UnrealBuildTool;

[SupportedPlatforms(UnrealPlatformClass.Desktop)]
public class TraceDigestTarget : TargetRules
{
	public TraceDigestTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Program;
		LinkType = TargetLinkType.Modular;
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

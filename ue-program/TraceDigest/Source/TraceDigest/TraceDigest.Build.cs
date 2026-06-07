// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

using UnrealBuildTool;

public class TraceDigest : ModuleRules
{
	public TraceDigest(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.NoSharedPCHs;
		PrivatePCHHeaderFile = "Private/TraceDigestPCH.h";

		// Programs use the Launch module to get the FEngineLoop entry-point glue.
		PublicIncludePathModuleNames.Add("Launch");

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"TraceAnalysis",
			"TraceServices",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
		});
	}
}

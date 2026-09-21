// Copyright 2026 Holotype Pte Ltd. All Rights Reserved.

using UnrealBuildTool;

public class MHARKitExport : ModuleRules
{
	public MHARKitExport(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RenderCore",
			"Json",
			"JsonUtilities",
			"AssetRegistry",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"MeshUtilities",
			"Slate",
			"SlateCore",
			"InputCore",
			"ToolMenus",
			"DesktopPlatform",
		});
	}
}

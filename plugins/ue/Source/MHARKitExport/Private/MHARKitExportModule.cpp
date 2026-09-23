// Copyright 2026 Holotype Pte Ltd. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Framework/Application/SlateApplication.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DesktopPlatformModule.h"
#include "HAL/FileManager.h"
#include "Engine/SkeletalMesh.h"
#include "MHARKitExportBPLibrary.h"
#include "Async/Async.h"

#define LOCTEXT_NAMESPACE "MHARKitExport"

/**
 * Slate widget: ARKit Blend Shape Exporter window.
 * Character picker + output path + Export button + log output.
 */
class SMHARKitExportWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMHARKitExportWindow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		// Discover MetaHuman characters
		RefreshCharacterList();

		ChildSlot
		[
			SNew(SVerticalBox)

			// --- Character ---
			+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("CharLabel", "MetaHuman Character"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 8)
			[
				SAssignNew(CharacterCombo, SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&CharacterNames)
				.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Val, ESelectInfo::Type)
				{
					if (Val.IsValid()) SelectedCharacter = *Val;
				})
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
				{
					return SNew(STextBlock).Text(FText::FromString(*Item));
				})
				.Content()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FText::FromString(SelectedCharacter); })
				]
			]

			// --- Output Path ---
			+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("OutputLabel", "Output Path (.glb)"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SAssignNew(OutputPathBox, SEditableTextBox)
					.Text(FText::FromString(FPaths::ProjectSavedDir() / TEXT("ARKitExport") / TEXT("face_arkit.glb")))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Browse", "Browse..."))
					.OnClicked_Lambda([this]() -> FReply
					{
						IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
						if (Desktop)
						{
							TArray<FString> OutFiles;
							Desktop->SaveFileDialog(
								FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
								TEXT("Save GLB"),
								FPaths::GetPath(OutputPathBox->GetText().ToString()),
								TEXT("face_arkit.glb"),
								TEXT("GLB Files (*.glb)|*.glb"),
								0, OutFiles);
							if (OutFiles.Num() > 0)
							{
								OutputPathBox->SetText(FText::FromString(OutFiles[0]));
							}
						}
						return FReply::Handled();
					})
				]
			]

			// --- Export Button ---
			+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 8)
			[
				SAssignNew(ExportButton, SButton)
				.HAlign(HAlign_Center)
				.OnClicked_Lambda([this]() -> FReply
				{
					RunExport();
					return FReply::Handled();
				})
				.Content()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ExportBtn", "Export ARKit Blend Shapes"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
				]
			]

			// --- Log Output ---
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(8, 0, 8, 8)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				[
					SAssignNew(LogScroll, SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(LogText, STextBlock)
						.Text(LOCTEXT("Ready", "Ready. Select a character and click Export."))
						.AutoWrapText(true)
					]
				]
			]
		];
	}

private:
	TSharedPtr<SComboBox<TSharedPtr<FString>>> CharacterCombo;
	TSharedPtr<SEditableTextBox> OutputPathBox;
	TSharedPtr<SButton> ExportButton;
	TSharedPtr<SScrollBox> LogScroll;
	TSharedPtr<STextBlock> LogText;

	TArray<TSharedPtr<FString>> CharacterNames;
	FString SelectedCharacter;

	void RefreshCharacterList()
	{
		CharacterNames.Empty();
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();

		TArray<FAssetData> Assets;
		AR.GetAssetsByPath(FName(TEXT("/Game/MetaHumans")), Assets, false);

		for (const FAssetData& AD : Assets)
		{
			// MetaHuman character folders contain Blueprint assets named BP_<character>
			FString AssetName = AD.AssetName.ToString();
			if (AssetName.StartsWith(TEXT("BP_")))
			{
				FString CharName = AssetName.RightChop(3); // Remove "BP_"
				CharacterNames.Add(MakeShared<FString>(CharName));
			}
		}

		// Also scan for subfolders that contain Face/ with a SkeletalMesh
		TArray<FString> SubPaths;
		AR.GetSubPaths(TEXT("/Game/MetaHumans"), SubPaths, false);
		for (const FString& SubPath : SubPaths)
		{
			FString CharName = FPaths::GetCleanFilename(SubPath);
			if (CharName == TEXT("Common")) continue;

			// Check if we already have this character
			bool bFound = false;
			for (const TSharedPtr<FString>& Existing : CharacterNames)
			{
				if (*Existing == CharName) { bFound = true; break; }
			}
			if (!bFound)
			{
				// Verify it has a Face/ subfolder
				TArray<FAssetData> FaceAssets;
				AR.GetAssetsByPath(FName(*(SubPath / TEXT("Face"))), FaceAssets, true);
				for (const FAssetData& FA : FaceAssets)
				{
					if (FA.GetAsset() && FA.GetAsset()->IsA<USkeletalMesh>())
					{
						CharacterNames.Add(MakeShared<FString>(CharName));
						break;
					}
				}
			}
		}

		CharacterNames.Sort([](const TSharedPtr<FString>& A, const TSharedPtr<FString>& B)
		{
			return *A < *B;
		});

		if (CharacterNames.Num() > 0)
		{
			SelectedCharacter = *CharacterNames[0];
		}
	}

	void AppendLog(const FString& Msg)
	{
		FString Current = LogText->GetText().ToString();
		if (!Current.IsEmpty()) Current += TEXT("\n");
		Current += Msg;
		LogText->SetText(FText::FromString(Current));
		LogScroll->ScrollToEnd();
	}

	void RunExport()
	{
		if (SelectedCharacter.IsEmpty())
		{
			AppendLog(TEXT("ERROR: No character selected."));
			return;
		}

		FString OutPath = OutputPathBox->GetText().ToString();
		if (OutPath.IsEmpty())
		{
			AppendLog(TEXT("ERROR: No output path specified."));
			return;
		}

		// Replace filename with character-specific name if using default
		if (OutPath.EndsWith(TEXT("face_arkit.glb")))
		{
			OutPath = FPaths::GetPath(OutPath) / (SelectedCharacter + TEXT("_arkit.glb"));
			OutputPathBox->SetText(FText::FromString(OutPath));
		}

		ExportButton->SetEnabled(false);
		AppendLog(FString::Printf(TEXT("Exporting '%s' to %s..."), *SelectedCharacter, *OutPath));

		// Run on game thread (required for actor spawning / animation eval)
		bool bResult = UMHARKitExportBPLibrary::BakeARKitShapeKeys(SelectedCharacter, OutPath);

		if (bResult)
		{
			int64 FileSize = IFileManager::Get().FileSize(*OutPath);
			AppendLog(FString::Printf(TEXT("SUCCESS: %s (%.1f MB)"),
				*OutPath, FileSize / (1024.0 * 1024.0)));
		}
		else
		{
			AppendLog(TEXT("FAILED: Check Output Log for details."));
		}

		ExportButton->SetEnabled(true);
	}
};


/**
 * Module: registers Tools menu entry to open the exporter window.
 */
class FMHARKitExportModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(
			this, &FMHARKitExportModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

private:
	TWeakPtr<SWindow> ExportWindow;

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);

		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("MHARKitExport");

		Section.AddMenuEntry(
			"MHARKitExport_Open",
			LOCTEXT("MenuLabel", "Export ARKit Blend Shapes"),
			LOCTEXT("MenuTooltip", "Export MetaHuman face mesh with 52 ARKit morph targets as GLB"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FMHARKitExportModule::OpenWindow))
		);
	}

	void OpenWindow()
	{
		// Reuse existing window if still open
		if (ExportWindow.IsValid())
		{
			ExportWindow.Pin()->BringToFront();
			return;
		}

		TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(LOCTEXT("WindowTitle", "ARKit Blend Shape Exporter"))
			.ClientSize(FVector2D(500, 450))
			.SupportsMinimize(false)
			.SupportsMaximize(false)
			[
				SNew(SMHARKitExportWindow)
			];

		ExportWindow = Window;
		FSlateApplication::Get().AddWindow(Window);
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMHARKitExportModule, MHARKitExport)

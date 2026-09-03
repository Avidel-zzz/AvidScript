#include "Demos/AvidScriptUiSaveDemoAssets.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Factories/BlueprintFactory.h"
#include "Factories/WorldFactory.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/SaveGame.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptUiSaveDemoAssets, Log, All);

namespace AvidScript::UiSaveDemo
{
namespace
{
constexpr const TCHAR* AssetNames[] = {
	TEXT("WBP_UiSave"), TEXT("BP_UiSaveHost"), TEXT("BP_PlayerSave"), TEXT("L_UiSave")
};

bool Compile(UBlueprint* Blueprint, FString& Error)
{
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint,
		EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
	if (Blueprint->Status == BS_Error || !Blueprint->GeneratedClass)
	{
		Error = FString::Printf(TEXT("Blueprint compilation failed: %s"), *Blueprint->GetPathName());
		return false;
	}
	return true;
}

bool AddVariable(UBlueprint* Blueprint, FName Name, FName Category, UObject* Type, FString& Error)
{
	FEdGraphPinType PinType;
	PinType.PinCategory = Category;
	PinType.PinSubCategoryObject = Type;
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, Name, PinType))
	{
		Error = FString::Printf(TEXT("Cannot create property %s"), *Name.ToString());
		return false;
	}
	return true;
}

UBlueprint* MakeBlueprint(const FString& Path, UClass* Parent)
{
	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = Parent;
	return Cast<UBlueprint>(Factory->FactoryCreateNew(UBlueprint::StaticClass(),
		CreatePackage(*Path), FName(*FPackageName::GetLongPackageAssetName(Path)),
		RF_Public | RF_Standalone, nullptr, GWarn));
}

UTextBlock* AddText(UWidgetTree* Tree, UVerticalBox* Box, FName Name,
	const TCHAR* Text, int32 FontSize, bool bVariable)
{
	UTextBlock* Label = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
	Label->bIsVariable = bVariable;
	Label->SetText(FText::FromString(Text));
	FSlateFontInfo Font = Label->GetFont();
	Font.Size = FontSize;
	Label->SetFont(Font);
	Label->SetColorAndOpacity(FSlateColor(FLinearColor(0.92f, 0.94f, 0.96f)));
	Label->SetAutoWrapText(true);
	Box->AddChildToVerticalBox(Label)->SetPadding(FMargin(0, 0, 0, 16));
	return Label;
}

bool BuildWidget(UWidgetBlueprint* Blueprint, FString& Error)
{
	UWidgetTree* Tree = Blueprint->WidgetTree;
	UBorder* Background = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Background"));
	Background->SetBrushColor(FLinearColor(0.045f, 0.055f, 0.065f));
	Background->SetPadding(FMargin(24));
	Background->SetHorizontalAlignment(HAlign_Center);
	Background->SetVerticalAlignment(VAlign_Center);
	Tree->RootWidget = Background;
	USizeBox* Width = Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ContentWidth"));
	Width->SetMaxDesiredWidth(480);
	Background->SetContent(Width);
	UVerticalBox* Box = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Content"));
	Width->SetContent(Box);
	AddText(Tree, Box, TEXT("Title"), TEXT("UI / Save Demo"), 28, false);
	AddText(Tree, Box, TEXT("ScoreText"), TEXT("Score: 0"), 24, true);
	AddText(Tree, Box, TEXT("StatusText"), TEXT("Starting..."), 16, true);
	const TCHAR* Names[] = { TEXT("CollectButton"), TEXT("SaveButton"), TEXT("LoadButton"), TEXT("ResetButton") };
	const TCHAR* Labels[] = { TEXT("Collect"), TEXT("Save"), TEXT("Load"), TEXT("Reset") };
	for (int32 Index = 0; Index < 4; ++Index)
	{
		UButton* Button = Tree->ConstructWidget<UButton>(UButton::StaticClass(), FName(Names[Index]));
		Button->bIsVariable = true;
		Button->SetBackgroundColor(Index == 0 ? FLinearColor(0.08f, 0.48f, 0.32f) : FLinearColor(0.22f, 0.26f, 0.3f));
		USizeBox* Size = Tree->ConstructWidget<USizeBox>();
		Size->SetMinDesiredHeight(48);
		UTextBlock* Label = Tree->ConstructWidget<UTextBlock>();
		Label->SetText(FText::FromString(Labels[Index]));
		Label->SetJustification(ETextJustify::Center);
		Size->SetContent(Label);
		Button->SetContent(Size);
		Box->AddChildToVerticalBox(Button)->SetPadding(FMargin(0, 0, 0, 8));
	}
	if (!Compile(Blueprint, Error)) { return false; }
	CastChecked<UUserWidget>(Blueprint->GeneratedClass->GetDefaultObject())->SetIsFocusable(true);
	return true;
}

template<typename T>
T* AddNode(UEdGraph* Graph)
{
	T* Node = NewObject<T>(Graph);
	Graph->AddNode(Node, false, false);
	Node->CreateNewGuid();
	Node->NodePosX = (Graph->Nodes.Num() - 1) * 240;
	return Node;
}

UK2Node_CallFunction* Call(UEdGraph* Graph, UClass* Owner, FName Function)
{
	UFunction* Target = Owner->FindFunctionByName(Function);
	if (!Target) { return nullptr; }
	UK2Node_CallFunction* Node = AddNode<UK2Node_CallFunction>(Graph);
	Node->SetFromFunction(Target);
	Node->AllocateDefaultPins();
	return Node;
}

bool BuildHost(UBlueprint* Host, UClass* WidgetClass, UClass* SaveClass, FString& Error)
{
	if (!AddVariable(Host, TEXT("RootWidget"), UEdGraphSchema_K2::PC_Object, WidgetClass, Error)
		|| !AddVariable(Host, TEXT("SavedObject"), UEdGraphSchema_K2::PC_Object, SaveClass, Error)
		|| !Compile(Host, Error)) { return false; }
	// A Blueprint factory may seed a disabled BeginPlay event. Replace that event only.
	for (UEdGraph* ExistingGraph : Host->UbergraphPages)
	{
		const TArray<TObjectPtr<UEdGraphNode>> Nodes = ExistingGraph->Nodes;
		for (UEdGraphNode* Node : Nodes)
		{
			UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
			if (Event && Event->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay"))
			{
				FBlueprintEditorUtils::RemoveNode(Host, Event, true);
			}
		}
	}
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Host, TEXT("UiSavePresentation"),
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(Host, Graph);
	UK2Node_Event* Begin = AddNode<UK2Node_Event>(Graph);
	Begin->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"), AActor::StaticClass());
	Begin->bOverrideFunction = true;
	Begin->AllocateDefaultPins();
	UK2Node_CallFunction* Player = Call(Graph, UGameplayStatics::StaticClass(), TEXT("GetPlayerController"));
	UK2Node_CallFunction* Create = Call(Graph, UWidgetBlueprintLibrary::StaticClass(), TEXT("Create"));
	UK2Node_CallFunction* Viewport = Call(Graph, UUserWidget::StaticClass(), TEXT("AddToViewport"));
	UK2Node_CallFunction* Focus = Call(Graph, UWidgetBlueprintLibrary::StaticClass(), TEXT("SetInputMode_UIOnlyEx"));
	if (!Player || !Create || !Viewport || !Focus)
	{
		Error = TEXT("Required standard UE presentation functions are unavailable");
		return false;
	}
	UK2Node_DynamicCast* WidgetCast = AddNode<UK2Node_DynamicCast>(Graph);
	WidgetCast->TargetType = WidgetClass;
	WidgetCast->SetPurity(true);
	WidgetCast->AllocateDefaultPins();
	UK2Node_VariableSet* Root = AddNode<UK2Node_VariableSet>(Graph);
	Root->VariableReference.SetSelfMember(TEXT("RootWidget"));
	Root->AllocateDefaultPins();
	UK2Node_VariableSet* Cursor = AddNode<UK2Node_VariableSet>(Graph);
	Cursor->VariableReference.SetExternalMember(TEXT("bShowMouseCursor"), APlayerController::StaticClass());
	Cursor->AllocateDefaultPins();
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	auto Link = [&](UEdGraphNode* From, FName Out, UEdGraphNode* To, FName In)
	{
		UEdGraphPin* A = From->FindPin(Out, EGPD_Output);
		UEdGraphPin* B = To->FindPin(In, EGPD_Input);
		if (!A || !B || !Schema->TryCreateConnection(A, B))
		{
			Error = FString::Printf(TEXT("Cannot connect Blueprint pins %s -> %s"), *Out.ToString(), *In.ToString());
			return false;
		}
		return true;
	};
	UEdGraphPin* ClassPin = Create->FindPin(TEXT("WidgetType"));
	UEdGraphPin* CursorPin = Cursor->FindPin(TEXT("bShowMouseCursor"), EGPD_Input);
	if (!ClassPin || !CursorPin) { Error = TEXT("Missing WidgetType or cursor pin"); return false; }
	Schema->TrySetDefaultObject(*ClassPin, WidgetClass);
	Schema->TrySetDefaultValue(*CursorPin, TEXT("true"));
	if (!Link(Begin, UEdGraphSchema_K2::PN_Then, Create, UEdGraphSchema_K2::PN_Execute)
		|| !Link(Player, UEdGraphSchema_K2::PN_ReturnValue, Create, TEXT("OwningPlayer"))
		|| !Schema->TryCreateConnection(Create->GetReturnValuePin(), WidgetCast->GetCastSourcePin())
		|| !Schema->TryCreateConnection(WidgetCast->GetCastResultPin(), Root->FindPin(TEXT("RootWidget"), EGPD_Input))
		|| !Link(Create, UEdGraphSchema_K2::PN_Then, Root, UEdGraphSchema_K2::PN_Execute)
		|| !Link(Root, UEdGraphSchema_K2::PN_Then, Viewport, UEdGraphSchema_K2::PN_Execute)
		|| !Link(Create, UEdGraphSchema_K2::PN_ReturnValue, Viewport, UEdGraphSchema_K2::PN_Self)
		|| !Link(Viewport, UEdGraphSchema_K2::PN_Then, Cursor, UEdGraphSchema_K2::PN_Execute)
		|| !Link(Player, UEdGraphSchema_K2::PN_ReturnValue, Cursor, UEdGraphSchema_K2::PN_Self)
		|| !Link(Cursor, UEdGraphSchema_K2::PN_Then, Focus, UEdGraphSchema_K2::PN_Execute)
		|| !Link(Player, UEdGraphSchema_K2::PN_ReturnValue, Focus, TEXT("PlayerController"))
		|| !Link(Create, UEdGraphSchema_K2::PN_ReturnValue, Focus, TEXT("InWidgetToFocus")))
	{
		if (Error.IsEmpty()) { Error = TEXT("Widget cast connection failed"); }
		return false;
	}
	return Compile(Host, Error);
}

bool Validate(const TArray<UObject*>& Assets, FString& Error)
{
	UWidgetBlueprint* Widget = Cast<UWidgetBlueprint>(Assets[0]);
	UBlueprint* Host = Cast<UBlueprint>(Assets[1]);
	UBlueprint* Save = Cast<UBlueprint>(Assets[2]);
	UWorld* World = Cast<UWorld>(Assets[3]);
	if (!Widget || !Host || !Save || !World || !World->PersistentLevel || !Widget->GeneratedClass || !Host->GeneratedClass
		|| !Save->GeneratedClass || !Host->GeneratedClass->IsChildOf(AActor::StaticClass())
		|| !Save->GeneratedClass->IsChildOf(USaveGame::StaticClass())
		|| Widget->Status == BS_Error || Host->Status == BS_Error || Save->Status == BS_Error)
	{
		Error = TEXT("Existing assets have incompatible classes or Blueprint compilation errors; no assets overwritten");
		return false;
	}
	UEdGraph* Presentation = nullptr;
	for (UEdGraph* Graph : Host->UbergraphPages)
	{
		if (Graph->GetFName() == TEXT("UiSavePresentation")) { Presentation = Graph; }
	}
	int32 BeginCount = 0;
	int32 CreateCount = 0;
	int32 ViewportCount = 0;
	int32 FocusCount = 0;
	int32 RootSetCount = 0;
	int32 CursorSetCount = 0;
	auto HasInput = [](UEdGraphNode* Node, FName Name)
	{
		UEdGraphPin* Pin = Node->FindPin(Name, EGPD_Input);
		return Pin && Pin->LinkedTo.Num() == 1;
	};
	if (Presentation)
	{
		for (UEdGraphNode* Node : Presentation->Nodes)
		{
			if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
			{
				UEdGraphPin* Then = Event->FindPin(UEdGraphSchema_K2::PN_Then);
				if (Event->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay")
					&& Then && Then->LinkedTo.Num() == 1) { ++BeginCount; }
			}
			if (UK2Node_CallFunction* Function = Cast<UK2Node_CallFunction>(Node))
			{
				const FName Name = Function->FunctionReference.GetMemberName();
				if (Name == TEXT("Create"))
				{
					UEdGraphPin* ClassPin = Function->FindPin(TEXT("WidgetType"));
					if (ClassPin && ClassPin->DefaultObject == Widget->GeneratedClass
						&& HasInput(Function, TEXT("OwningPlayer"))
						&& HasInput(Function, UEdGraphSchema_K2::PN_Execute)) { ++CreateCount; }
				}
				if (Name == TEXT("AddToViewport") && HasInput(Function, UEdGraphSchema_K2::PN_Self)
					&& HasInput(Function, UEdGraphSchema_K2::PN_Execute)) { ++ViewportCount; }
				if (Name == TEXT("SetInputMode_UIOnlyEx") && HasInput(Function, TEXT("PlayerController"))
					&& HasInput(Function, TEXT("InWidgetToFocus"))
					&& HasInput(Function, UEdGraphSchema_K2::PN_Execute)) { ++FocusCount; }
			}
			if (UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node))
			{
				if (Set->VariableReference.GetMemberName() == TEXT("RootWidget"))
				{
					if (HasInput(Set, TEXT("RootWidget")) && HasInput(Set, UEdGraphSchema_K2::PN_Execute)) { ++RootSetCount; }
				}
				if (Set->VariableReference.GetMemberName() == TEXT("bShowMouseCursor"))
				{
					UEdGraphPin* Pin = Set->FindPin(TEXT("bShowMouseCursor"), EGPD_Input);
					if (Pin && Pin->DefaultValue == TEXT("true") && HasInput(Set, UEdGraphSchema_K2::PN_Self)
						&& HasInput(Set, UEdGraphSchema_K2::PN_Execute)) { ++CursorSetCount; }
				}
			}
		}
	}
	if (BeginCount != 1 || CreateCount != 1 || ViewportCount != 1 || FocusCount != 1 || RootSetCount != 1 || CursorSetCount != 1)
	{
		Error = TEXT("Host presentation graph contract mismatch; no assets overwritten");
		return false;
	}
	FObjectProperty* Root = FindFProperty<FObjectProperty>(Host->GeneratedClass, TEXT("RootWidget"));
	FObjectProperty* Saved = FindFProperty<FObjectProperty>(Host->GeneratedClass, TEXT("SavedObject"));
	FIntProperty* Score = FindFProperty<FIntProperty>(Save->GeneratedClass, TEXT("Score"));
	if (!Root || Root->PropertyClass != Widget->GeneratedClass || !Saved || Saved->PropertyClass != Save->GeneratedClass
		|| !Score || !Score->HasAnyPropertyFlags(CPF_SaveGame))
	{
		Error = TEXT("Host/SaveGame property contract mismatch; no assets overwritten");
		return false;
	}
	for (const TCHAR* Name : { TEXT("CollectButton"), TEXT("SaveButton"), TEXT("LoadButton"), TEXT("ResetButton"), TEXT("ScoreText"), TEXT("StatusText") })
	{
		FObjectProperty* Property = FindFProperty<FObjectProperty>(Widget->GeneratedClass, Name);
		UClass* Expected = FString(Name).EndsWith(TEXT("Button")) ? UButton::StaticClass() : UTextBlock::StaticClass();
		if (!Property || Property->PropertyClass != Expected || !Widget->WidgetTree
			|| !Widget->WidgetTree->FindWidget(FName(Name)))
		{
			Error = FString::Printf(TEXT("Widget contract mismatch: %s; no assets overwritten"), Name);
			return false;
		}
	}
	int32 HostCount = 0;
	int32 StartCount = 0;
	for (AActor* Actor : World->PersistentLevel->Actors)
	{
		if (Actor && Actor->IsA(Host->GeneratedClass)) { ++HostCount; }
		if (Actor && Actor->IsA<APlayerStart>()) { ++StartCount; }
	}
	if (HostCount != 1 || StartCount != 1 || World->GetWorldSettings()->DefaultGameMode != AGameModeBase::StaticClass())
	{
		Error = TEXT("Map must contain one UI host and the standard GameMode; no assets overwritten");
		return false;
	}
	return true;
}
}

bool Prepare(FString& OutError, bool& bOutReused, const FString& Root)
{
	OutError.Reset();
	bOutReused = false;
	if (!GEditor || !IsInGameThread() || !Root.StartsWith(TEXT("/AvidScript/"))
		|| !FPackageName::IsValidLongPackageName(Root) || Root.Contains(TEXT("..")))
	{
		OutError = TEXT("Asset preparation requires GameThread and a valid /AvidScript package root");
		return false;
	}
	TArray<UObject*> Assets;
	int32 Existing = 0;
	int32 ExistingOnDisk = 0;
	for (const TCHAR* Name : AssetNames)
	{
		const FString PackagePath = Root / Name;
		const bool bOnDisk = FPackageName::DoesPackageExist(PackagePath);
		const bool bExists = bOnDisk || FindPackage(nullptr, *PackagePath);
		Existing += bExists ? 1 : 0;
		ExistingOnDisk += bOnDisk ? 1 : 0;
	}
	if (Existing != 0)
	{
		if (Existing != 4 || ExistingOnDisk != 4)
		{
			OutError = TEXT("Partial asset set or unsaved assets already exist; refusing to overwrite or repair user assets");
			return false;
		}
		for (const TCHAR* Name : AssetNames)
		{
			Assets.Add(LoadObject<UObject>(nullptr, *((Root / Name) + TEXT(".") + Name)));
		}
		bOutReused = Validate(Assets, OutError);
		return bOutReused;
	}
	TStrongObjectPtr<UWidgetBlueprintFactory> WidgetFactory(NewObject<UWidgetBlueprintFactory>());
	WidgetFactory->ParentClass = UUserWidget::StaticClass();
	UWidgetBlueprint* Widget = Cast<UWidgetBlueprint>(WidgetFactory->FactoryCreateNew(UWidgetBlueprint::StaticClass(),
		CreatePackage(*(Root / AssetNames[0])), FName(AssetNames[0]), RF_Public | RF_Standalone, nullptr, GWarn));
	UBlueprint* Save = MakeBlueprint(Root / AssetNames[2], USaveGame::StaticClass());
	UBlueprint* Host = MakeBlueprint(Root / AssetNames[1], AActor::StaticClass());
	if (!Widget || !Save || !Host) { OutError = TEXT("UE Blueprint factory failed"); return false; }
	if (!BuildWidget(Widget, OutError)
		|| !AddVariable(Save, TEXT("Score"), UEdGraphSchema_K2::PC_Int, nullptr, OutError)) { return false; }
	for (FBPVariableDescription& Variable : Save->NewVariables)
	{
		if (Variable.VarName == TEXT("Score")) { Variable.PropertyFlags |= CPF_SaveGame; }
	}
	if (!Compile(Save, OutError) || !BuildHost(Host, Widget->GeneratedClass, Save->GeneratedClass, OutError)) { return false; }
	TStrongObjectPtr<UWorldFactory> WorldFactory(NewObject<UWorldFactory>());
	WorldFactory->WorldType = EWorldType::Editor;
	WorldFactory->bInformEngineOfWorld = false;
	WorldFactory->bCreateWorldPartition = false;
	UWorld* World = Cast<UWorld>(WorldFactory->FactoryCreateNew(UWorld::StaticClass(),
		CreatePackage(*(Root / AssetNames[3])), FName(AssetNames[3]), RF_Public | RF_Standalone, nullptr, GWarn));
	if (!World) { OutError = TEXT("UE World factory failed"); return false; }
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	FActorSpawnParameters Spawn;
	Spawn.Name = TEXT("UiSaveHost");
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* HostActor = World->SpawnActor<AActor>(Host->GeneratedClass, FTransform::Identity, Spawn);
	APlayerStart* Start = World->SpawnActor<APlayerStart>();
	if (!HostActor || !Start) { OutError = TEXT("Map host or PlayerStart creation failed"); return false; }
	HostActor->SetActorLabel(TEXT("UiSaveHost"));
	Assets = { Widget, Host, Save, World };
	if (!Validate(Assets, OutError)) { return false; }
	for (UObject* Asset : Assets)
	{
		UPackage* Package = Asset->GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(),
			Asset == World ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
		if (IFileManager::Get().FileExists(*Filename)) { OutError = TEXT("An asset appeared during preparation; refusing overwrite"); return false; }
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true))
		{
			OutError = TEXT("Cannot create the asset output directory");
			return false;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Asset, *Filename, Args))
		{
			OutError = FString::Printf(TEXT("Asset save failed: %s"), *Filename);
			return false;
		}
	}
	return true;
}

void ConsoleCommand(const TArray<FString>& Arguments)
{
	const bool bExit = Arguments.Num() == 1 && Arguments[0].Equals(TEXT("exit"), ESearchCase::IgnoreCase);
	FString Error;
	bool bReused = false;
	const bool bValidArgs = Arguments.IsEmpty() || bExit;
	const bool bSucceeded = bValidArgs && Prepare(Error, bReused);
	if (!bValidArgs) { Error = TEXT("Usage: AvidScript.PrepareUiSaveDemo [exit]"); }
	if (bSucceeded)
	{
		UE_LOG(LogAvidScriptUiSaveDemoAssets, Display, TEXT("AVIDSCRIPT_UI_SAVE_DEMO_ASSETS_PASSED root=/AvidScript/Demos/UiSave assets=4 mode=%s"), bReused ? TEXT("validated") : TEXT("created"));
	}
	else
	{
		UE_LOG(LogAvidScriptUiSaveDemoAssets, Error, TEXT("AVIDSCRIPT_UI_SAVE_DEMO_ASSETS_FAILED %s"), *Error);
	}
	if (bExit) { FPlatformMisc::RequestExitWithStatus(true, bSucceeded ? 0 : 1); }
}
}

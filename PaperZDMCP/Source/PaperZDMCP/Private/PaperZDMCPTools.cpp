#include "PaperZDMCPTools.h"
#include "PaperZDMCPGraphDocument.h"

#include "AnimSequences/PaperZDAnimSequence_Flipbook.h"
#include "AnimSequences/Sources/PaperZDAnimationSource_Flipbook.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/EngineVersion.h"
#include "Notifies/PaperZDAnimNotifyCustom.h"
#include "Notifies/PaperZDAnimNotifyState.h"
#include "PaperFlipbook.h"
#include "PaperZDAnimBP.h"
#include "PaperZDAnimBPGeneratedClass.h"
#include "PaperZDAnimInstance.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace
{
using FJson = TSharedPtr<FJsonObject>;

FString Encode(const FJson& Object)
{
    FString Result;
    FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Result));
    return Result;
}

FString Fail(const FString& Message)
{
    // Native MCP translates script exceptions into tool errors.
    UKismetSystemLibrary::RaiseScriptError(Message);
    FJson Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), false);
    Result->SetStringField(TEXT("error"), Message);
    return Encode(Result);
}

FJson Success()
{
    FJson Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), true);
    return Result;
}

bool CanMutate()
{
    return IsInGameThread() && GEditor && !GEditor->PlayWorld;
}

bool IsGamePath(const FString& Path)
{
    return Path.StartsWith(TEXT("/Game/")) && FPackageName::IsValidLongPackageName(Path);
}

UObject* LoadAsset(const FString& Path)
{
    FString Package = Path.Contains(TEXT(".")) ? FPackageName::ObjectPathToPackageName(Path) : Path;
    if (!IsGamePath(Package))
    {
        return nullptr;
    }
    const FString ObjectPath = Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package);
    if (Path.Contains(TEXT(".")) && Path != ObjectPath)
    {
        return nullptr; // No subobjects or generated classes.
    }
    return LoadObject<UObject>(nullptr, *ObjectPath);
}

bool IsPaperAsset(const UObject* Asset)
{
    return Asset && (Asset->IsA<UPaperZDAnimationSource>() || Asset->IsA<UPaperZDAnimSequence>() || Asset->IsA<UPaperZDAnimBP>());
}

bool ValidateDestination(const FString& Path, FString& Error)
{
    if (!IsGamePath(Path) || Path.Contains(TEXT(".")))
    {
        Error = TEXT("Use an unused /Game package path, e.g. /Game/Animations/AS_Hero, without an object suffix.");
        return false;
    }
    if (FindPackage(nullptr, *Path) || FPackageName::DoesPackageExist(Path))
    {
        Error = TEXT("Destination package already exists. No asset was overwritten.");
        return false;
    }
    return true;
}

FJson DescribeBase(UObject* Asset)
{
    FJson Result = Success();
    Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    Result->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
    Result->SetBoolField(TEXT("dirty"), Asset->GetOutermost()->IsDirty());
    return Result;
}

void Publish(UObject* Asset)
{
    Asset->PostEditChange();
    FAssetRegistryModule::AssetCreated(Asset);
    Asset->MarkPackageDirty();
}

bool ValidateFlipbooks(const TArray<FString>& Paths, float AngleOffset, TArray<UPaperFlipbook*>& Out, FString& Error)
{
    if (Paths.Num() < 1 || Paths.Num() > 16 || !FMath::IsFinite(AngleOffset) || FMath::Abs(AngleOffset) > 45.0f)
    {
        Error = TEXT("Provide 1..16 flipbooks and a finite AngleOffset between -45 and 45 degrees.");
        return false;
    }
    for (const FString& Path : Paths)
    {
        UPaperFlipbook* Flipbook = Cast<UPaperFlipbook>(LoadAsset(Path));
        if (!Flipbook || !FMath::IsFinite(Flipbook->GetTotalDuration()) || Flipbook->GetTotalDuration() <= 0.0f)
        {
            Error = FString::Printf(TEXT("Flipbook must exist under /Game and have nonzero duration: %s"), *Path);
            return false;
        }
        if (Out.Num() && (!FMath::IsNearlyEqual(Flipbook->GetTotalDuration(), Out[0]->GetTotalDuration()) ||
            !FMath::IsNearlyEqual(Flipbook->GetFramesPerSecond(), Out[0]->GetFramesPerSecond())))
        {
            Error = TEXT("Directional flipbooks must have matching durations and frame rates.");
            return false;
        }
        Out.Add(Flipbook);
    }
    return true;
}

// PaperZD exposes its data array through reflection. Validate its shape before any mutation.
bool GetDataLayout(UPaperZDAnimSequence_Flipbook* Sequence, FArrayProperty*& Array, FObjectPropertyBase*& Animation)
{
    Array = Sequence->GetAnimDataSourceProperty();
    if (!Array) Array = FindFProperty<FArrayProperty>(Sequence->GetClass(), TEXT("AnimData"));
    FStructProperty* Entry = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
    Animation = Entry ? FindFProperty<FObjectPropertyBase>(Entry->Struct, TEXT("Animation")) : nullptr;
    return Animation && Animation->PropertyClass == UPaperFlipbook::StaticClass();
}

void ApplyFlipbooks(UPaperZDAnimSequence_Flipbook* Sequence, FArrayProperty* Array,
    FObjectPropertyBase* Animation, const TArray<UPaperFlipbook*>& Flipbooks, float AngleOffset)
{
    Sequence->PreEditChange(Array);
    FScriptArrayHelper Entries(Array, Array->ContainerPtrToValuePtr<void>(Sequence));
    Entries.Resize(Flipbooks.Num());
    for (int32 Index = 0; Index < Flipbooks.Num(); ++Index)
    {
        Animation->SetObjectPropertyValue_InContainer(Entries.GetRawPtr(Index), Flipbooks[Index]);
    }
    Sequence->bDirectionalSequence = Flipbooks.Num() > 1;
    Sequence->DirectionalAngleOffset = AngleOffset;
    Sequence->DirectionalPreviewIndex = 0;
    FPropertyChangedEvent Event(Array, EPropertyChangeType::ValueSet);
    Sequence->PostEditChangeProperty(Event);
    Sequence->MarkPackageDirty();
}

FJson DescribeNotify(UPaperZDAnimNotify_Base* Notify)
{
    FJson Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("id"), Notify->GetName());
    Result->SetStringField(TEXT("name"), Notify->Name.ToString());
    Result->SetStringField(TEXT("class"), Notify->GetClass()->GetPathName());
    Result->SetNumberField(TEXT("time"), Notify->Time);
    Result->SetNumberField(TEXT("track"), Notify->TrackIndex);
    if (const UPaperZDAnimNotifyState* State = Cast<UPaperZDAnimNotifyState>(Notify))
    {
        Result->SetNumberField(TEXT("duration"), State->Duration);
    }
    return Result;
}

FJson DescribeGraph(UEdGraph* Graph)
{
    FJson Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Graph->GetPathName());
    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node) continue;
        FJson Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("id"), Node->NodeGuid.ToString());
        Item->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        Item->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin) continue;
            FJson PinInfo = MakeShared<FJsonObject>();
            PinInfo->SetStringField(TEXT("id"), Pin->PinId.ToString());
            PinInfo->SetStringField(TEXT("name"), Pin->PinName.ToString());
            PinInfo->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
            TArray<TSharedPtr<FJsonValue>> Links;
            for (const UEdGraphPin* Link : Pin->LinkedTo)
            {
                if (Link) Links.Add(MakeShared<FJsonValueString>(Link->PinId.ToString()));
            }
            PinInfo->SetArrayField(TEXT("links"), Links);
            Pins.Add(MakeShared<FJsonValueObject>(PinInfo));
        }
        Item->SetArrayField(TEXT("pins"), Pins);
        Nodes.Add(MakeShared<FJsonValueObject>(Item));
    }
    Result->SetArrayField(TEXT("nodes"), Nodes);
    TArray<TSharedPtr<FJsonValue>> Children;
    for (UEdGraph* Child : Graph->SubGraphs)
    {
        if (Child) Children.Add(MakeShared<FJsonValueObject>(DescribeGraph(Child)));
    }
    Result->SetArrayField(TEXT("subgraphs"), Children);
    return Result;
}
}

FString UPaperZDMCPTools::GetCapabilities()
{
    FJson Result = Success();
    Result->SetStringField(TEXT("extension_version"), TEXT("0.2.1"));
    Result->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PaperZD"));
    Result->SetStringField(TEXT("paperzd_version"), Plugin ? Plugin->GetDescriptor().VersionName : TEXT("unknown"));
    Result->SetStringField(TEXT("transport"), TEXT("Unreal Engine native ModelContextProtocol / ToolsetRegistry"));
    Result->SetBoolField(TEXT("mutations_available"), CanMutate());
    Result->SetBoolField(TEXT("automatic_save"), false);
    Result->SetStringField(TEXT("scope"), TEXT("/Game assets; flipbook sources/sequences, directional replacement, AnimBP graph round-trip, nested state machines, transitions and condition DSL, cached owner-variable updates, custom notifies, explicit saving. Call GetAnimationGraphSchema for the accepted document and consult docs for version limits."));
    Result->SetStringField(TEXT("graph_schema"), TEXT("paperzd-graph/1"));
    return Encode(Result);
}

FString UPaperZDMCPTools::ListAssets(const FString& Folder, int32 Offset, int32 Limit)
{
    if ((Folder != TEXT("/Game") && !IsGamePath(Folder)) || Offset < 0 || Limit < 1 || Limit > 500)
        return Fail(TEXT("Folder must be /Game or a valid child folder; Offset >= 0; Limit 1..500."));
    FARFilter Filter;
    Filter.PackagePaths.Add(*Folder);
    Filter.bRecursivePaths = true;
    Filter.bRecursiveClasses = true;
    Filter.ClassPaths = { UPaperZDAnimationSource::StaticClass()->GetClassPathName(),
        UPaperZDAnimSequence::StaticClass()->GetClassPathName(), UPaperZDAnimBP::StaticClass()->GetClassPathName() };
    TArray<FAssetData> Assets;
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Assets);
    Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString(); });
    TArray<TSharedPtr<FJsonValue>> Items;
    const int32 End = Offset >= Assets.Num() ? Assets.Num() : Offset + FMath::Min(Limit, Assets.Num() - Offset);
    for (int32 Index = Offset; Index < End; ++Index)
    {
        FJson Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("asset_path"), Assets[Index].GetSoftObjectPath().ToString());
        Item->SetStringField(TEXT("class"), Assets[Index].AssetClassPath.ToString());
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }
    FJson Result = Success();
    Result->SetArrayField(TEXT("assets"), Items);
    Result->SetNumberField(TEXT("total"), Assets.Num());
    Result->SetNumberField(TEXT("next_offset"), End < Assets.Num() ? End : -1);
    return Encode(Result);
}

FString UPaperZDMCPTools::InspectAsset(const FString& AssetPath)
{
    UObject* Asset = LoadAsset(AssetPath);
    if (!IsPaperAsset(Asset)) return Fail(TEXT("Expected a PaperZD source, sequence or animation blueprint under /Game."));
    FJson Result = DescribeBase(Asset);
    if (UPaperZDAnimBP* Blueprint = Cast<UPaperZDAnimBP>(Asset))
        return PaperZDMCPGraph::ExportDocument(Blueprint);
    if (UPaperZDAnimationSource* Source = Cast<UPaperZDAnimationSource>(Asset))
    {
        TArray<TSharedPtr<FJsonValue>> Names;
        for (FName Name : Source->GetRegisteredNotifyNames()) Names.Add(MakeShared<FJsonValueString>(Name.ToString()));
        Result->SetArrayField(TEXT("registered_notifies"), Names);
    }
    if (UPaperZDAnimSequence* Sequence = Cast<UPaperZDAnimSequence>(Asset))
    {
        Result->SetStringField(TEXT("source"), GetPathNameSafe(Sequence->GetAnimSource()));
        Result->SetStringField(TEXT("category"), Sequence->Category.ToString());
        Result->SetNumberField(TEXT("duration"), Sequence->GetTotalDuration());
        Result->SetNumberField(TEXT("fps"), Sequence->GetFramesPerSecond());
        Result->SetBoolField(TEXT("directional"), Sequence->bDirectionalSequence);
        Result->SetNumberField(TEXT("angle_offset"), Sequence->DirectionalAngleOffset);
        Result->SetNumberField(TEXT("track_count"), Sequence->GetNumTracks());
        TArray<TSharedPtr<FJsonValue>> Notifies;
        for (UPaperZDAnimNotify_Base* Notify : Sequence->GetAnimNotifies())
            if (Notify) Notifies.Add(MakeShared<FJsonValueObject>(DescribeNotify(Notify)));
        Result->SetArrayField(TEXT("notifies"), Notifies);
        if (UPaperZDAnimSequence_Flipbook* FlipbookSequence = Cast<UPaperZDAnimSequence_Flipbook>(Sequence))
        {
            TArray<TSharedPtr<FJsonValue>> Paths;
            for (const auto& Entry : FlipbookSequence->GetAnimDataSource())
                Paths.Add(MakeShared<FJsonValueString>(GetPathNameSafe(Entry.Animation)));
            Result->SetArrayField(TEXT("flipbooks"), Paths);
        }
    }
    if (UPaperZDAnimBP* Blueprint = Cast<UPaperZDAnimBP>(Asset))
    {
        Result->SetStringField(TEXT("source"), GetPathNameSafe(Blueprint->GetSupportedAnimationSource()));
        Result->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(Blueprint->Status));
        if (Blueprint->AnimationGraph) Result->SetObjectField(TEXT("animation_graph"), DescribeGraph(Blueprint->AnimationGraph));
    }
    return Encode(Result);
}

FString UPaperZDMCPTools::CreateAnimationSource(const FString& AssetPath)
{
    if (!CanMutate()) return Fail(TEXT("Requires the editor game thread with Play In Editor stopped."));
    FString Error;
    if (!ValidateDestination(AssetPath, Error)) return Fail(Error);
    const FScopedTransaction Transaction(NSLOCTEXT("PaperZDMCP", "CreateSource", "Create PaperZD animation source"));
    UPackage* Package = CreatePackage(*AssetPath);
    Package->Modify();
    auto* Source = NewObject<UPaperZDAnimationSource_Flipbook>(Package, *FPackageName::GetLongPackageAssetName(AssetPath), RF_Public | RF_Standalone | RF_Transactional);
    Publish(Source);
    return InspectAsset(Source->GetPathName());
}

FString UPaperZDMCPTools::CreateAnimationSequence(const FString& AssetPath, const FString& SourcePath,
    const TArray<FString>& FlipbookPaths, const FString& Category, float AngleOffset)
{
    if (!CanMutate()) return Fail(TEXT("Requires the editor game thread with Play In Editor stopped."));
    FString Error;
    if (!ValidateDestination(AssetPath, Error)) return Fail(Error);
    auto* Source = Cast<UPaperZDAnimationSource_Flipbook>(LoadAsset(SourcePath));
    if (!Source || Source->GetSupportedAnimSequenceClass() != UPaperZDAnimSequence_Flipbook::StaticClass())
        return Fail(TEXT("SourcePath must reference a standard PaperZD flipbook animation source."));
    TArray<UPaperFlipbook*> Flipbooks;
    if (!ValidateFlipbooks(FlipbookPaths, AngleOffset, Flipbooks, Error)) return Fail(Error);
    if (Category.Len() >= NAME_SIZE) return Fail(TEXT("Category is too long."));
    FArrayProperty* Array; FObjectPropertyBase* Animation;
    if (!GetDataLayout(GetMutableDefault<UPaperZDAnimSequence_Flipbook>(), Array, Animation))
        return Fail(TEXT("Unsupported PaperZD animation data layout. No asset was created."));
    const FScopedTransaction Transaction(NSLOCTEXT("PaperZDMCP", "CreateSequence", "Create PaperZD animation sequence"));
    UPackage* Package = CreatePackage(*AssetPath);
    Package->Modify();
    auto* Sequence = NewObject<UPaperZDAnimSequence_Flipbook>(Package, *FPackageName::GetLongPackageAssetName(AssetPath), RF_Public | RF_Standalone | RF_Transactional);
    Sequence->SetAnimSource(Source);
    Sequence->Category = FName(*Category);
    ApplyFlipbooks(Sequence, Array, Animation, Flipbooks, AngleOffset);
    Publish(Sequence);
    return InspectAsset(Sequence->GetPathName());
}

FString UPaperZDMCPTools::SetSequenceFlipbooks(const FString& AssetPath, const TArray<FString>& FlipbookPaths, float AngleOffset)
{
    if (!CanMutate()) return Fail(TEXT("Requires the editor game thread with Play In Editor stopped."));
    auto* Sequence = Cast<UPaperZDAnimSequence_Flipbook>(LoadAsset(AssetPath));
    if (!Sequence) return Fail(TEXT("Expected a PaperZD flipbook sequence."));
    TArray<UPaperFlipbook*> Flipbooks; FString Error;
    if (!ValidateFlipbooks(FlipbookPaths, AngleOffset, Flipbooks, Error)) return Fail(Error);
    if (Flipbooks.Num() != Sequence->GetAnimDataSource().Num()) return Fail(TEXT("Direction count must remain unchanged; no directional data was discarded."));
    for (const UPaperZDAnimNotify_Base* Notify : Sequence->GetAnimNotifies())
    {
        if (!Notify) continue;
        const auto* State = Cast<UPaperZDAnimNotifyState>(Notify);
        if (Notify->Time + (State ? State->Duration : 0.0f) > Flipbooks[0]->GetTotalDuration())
            return Fail(TEXT("Replacement duration would leave an existing notify outside the sequence."));
    }
    FArrayProperty* Array; FObjectPropertyBase* Animation;
    if (!GetDataLayout(Sequence, Array, Animation)) return Fail(TEXT("Unsupported PaperZD animation data layout."));
    const FScopedTransaction Transaction(NSLOCTEXT("PaperZDMCP", "SetFlipbooks", "Set PaperZD sequence flipbooks"));
    Sequence->Modify();
    ApplyFlipbooks(Sequence, Array, Animation, Flipbooks, AngleOffset);
    return InspectAsset(Sequence->GetPathName());
}

FString UPaperZDMCPTools::CreateAnimationBlueprint(const FString& AssetPath, const FString& SourcePath)
{
    if (!CanMutate()) return Fail(TEXT("Requires the editor game thread with Play In Editor stopped."));
    FString Error;
    if (!ValidateDestination(AssetPath, Error)) return Fail(Error);
    auto* Source = Cast<UPaperZDAnimationSource>(LoadAsset(SourcePath));
    if (!Source) return Fail(TEXT("SourcePath must reference a PaperZD animation source."));
    const FScopedTransaction Transaction(NSLOCTEXT("PaperZDMCP", "CreateBlueprint", "Create PaperZD animation blueprint"));
    UPackage* Package = CreatePackage(*AssetPath);
    Package->Modify();
    auto* Blueprint = Cast<UPaperZDAnimBP>(FKismetEditorUtilities::CreateBlueprint(UPaperZDAnimInstance::StaticClass(),
        Package, *FPackageName::GetLongPackageAssetName(AssetPath), BPTYPE_Normal,
        UPaperZDAnimBP::StaticClass(), UPaperZDAnimBPGeneratedClass::StaticClass()));
    if (!Blueprint) return Fail(TEXT("PaperZD animation blueprint creation failed."));
    Blueprint->SupportedAnimationSource = Source;
    Blueprint->Status = BS_Dirty;
    Publish(Blueprint);
    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), true);
    Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    return Encode(Result);
}

FString UPaperZDMCPTools::CompileAnimationBlueprint(const FString& AssetPath)
{
    if (!CanMutate()) return Fail(TEXT("Requires the editor game thread with Play In Editor stopped."));
    auto* Blueprint = Cast<UPaperZDAnimBP>(LoadAsset(AssetPath));
    if (!Blueprint || !Blueprint->GetSupportedAnimationSource()) return Fail(TEXT("Expected a PaperZD animation blueprint with a source."));
    FCompilerResultsLog Log;
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Log);
    FJson Result = DescribeBase(Blueprint);
    Result->SetBoolField(TEXT("ok"), Log.NumErrors == 0 && Blueprint->Status != BS_Error);
    Result->SetNumberField(TEXT("errors"), Log.NumErrors);
    Result->SetNumberField(TEXT("warnings"), Log.NumWarnings);
    TArray<TSharedPtr<FJsonValue>> Messages;
    for (const auto& Message : Log.Messages) Messages.Add(MakeShared<FJsonValueString>(Message->ToText().ToString()));
    Result->SetArrayField(TEXT("messages"), Messages);
    return Encode(Result);
}

FString UPaperZDMCPTools::AddCustomNotify(const FString& AssetPath, const FString& NotifyName, float Time, int32 TrackIndex)
{
    if (!CanMutate()) return Fail(TEXT("Requires the editor game thread with Play In Editor stopped."));
    auto* Sequence = Cast<UPaperZDAnimSequence>(LoadAsset(AssetPath));
    if (!Sequence || !Sequence->GetAnimSource()) return Fail(TEXT("Expected a PaperZD sequence with a source."));
    if (NotifyName.IsEmpty() || NotifyName.Len() >= NAME_SIZE || NotifyName.Equals(TEXT("None"), ESearchCase::IgnoreCase))
        return Fail(TEXT("NotifyName must be nonempty, shorter than NAME_SIZE and not None."));
    for (TCHAR Character : NotifyName)
        if (!FChar::IsAlnum(Character) && Character != TEXT('_')) return Fail(TEXT("Use letters, digits and underscores for notify names."));
    if (!FMath::IsFinite(Time) || Time < 0 || Time > Sequence->GetTotalDuration() || Sequence->GetTotalDuration() <= 0)
        return Fail(TEXT("Time must be finite and within a nonempty sequence."));
    if (TrackIndex < 0 || TrackIndex > Sequence->GetNumTracks()) return Fail(TEXT("TrackIndex must select an existing track or the next track."));
    UPaperZDAnimationSource* Source = Sequence->GetAnimSource();
    const FScopedTransaction Transaction(NSLOCTEXT("PaperZDMCP", "AddNotify", "Add PaperZD custom notify"));
    Sequence->Modify();
    Source->Modify();
    Source->RegisterCustomNotify(FName(*NotifyName));
    if (TrackIndex == Sequence->GetNumTracks()) Sequence->CreateTrack();
    Sequence->AddNotifyToTrack(UPaperZDAnimNotifyCustom::StaticClass(), TrackIndex, FName(*NotifyName), Time);
    Sequence->PostEditChange();
    Sequence->MarkPackageDirty();
    FJson Result = DescribeBase(Sequence);
    Result->SetObjectField(TEXT("notify"), DescribeNotify(Sequence->GetAnimNotifies().Last()));
    Result->SetStringField(TEXT("source_to_save"), Source->GetPathName());
    Result->SetStringField(TEXT("note"), TEXT("Compile consuming animation blueprints to refresh notify functions; save the sequence, source, and affected blueprints explicitly."));
    return Encode(Result);
}

FString UPaperZDMCPTools::RemoveNotify(const FString& AssetPath, const FString& NotifyId)
{
    if (!CanMutate()) return Fail(TEXT("Requires the editor game thread with Play In Editor stopped."));
    auto* Sequence = Cast<UPaperZDAnimSequence>(LoadAsset(AssetPath));
    if (!Sequence) return Fail(TEXT("Expected a PaperZD sequence."));
    UPaperZDAnimNotify_Base* Target = nullptr;
    for (UPaperZDAnimNotify_Base* Notify : Sequence->GetAnimNotifies())
        if (Notify && Notify->GetName() == NotifyId) { Target = Notify; break; }
    if (!Target) return Fail(TEXT("NotifyId not found; inspect the sequence for current notify IDs."));
    const FScopedTransaction Transaction(NSLOCTEXT("PaperZDMCP", "RemoveNotify", "Remove PaperZD notify"));
    Sequence->Modify();
    Target->Modify();
    Sequence->RemoveNotify(Target);
    Sequence->PostEditChange();
    Sequence->MarkPackageDirty();
    return InspectAsset(Sequence->GetPathName());
}

FString UPaperZDMCPTools::SaveAsset(const FString& AssetPath)
{
    if (!CanMutate()) return Fail(TEXT("Requires the editor game thread with Play In Editor stopped."));
    UObject* Asset = LoadAsset(AssetPath);
    if (!IsPaperAsset(Asset)) return Fail(TEXT("Expected a PaperZD asset under /Game."));
    if (const UPaperZDAnimBP* Blueprint = Cast<UPaperZDAnimBP>(Asset))
        if (Blueprint->Status != BS_UpToDate && Blueprint->Status != BS_UpToDateWithWarnings)
            return Fail(TEXT("Compile the animation blueprint successfully before saving."));
    UPackage* Package = Asset->GetOutermost();
    const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    if (!UPackage::SavePackage(Package, Asset, *Filename, Args))
        return Fail(TEXT("Package save failed. Check the editor log and source-control checkout/file permissions."));
    FJson Result = DescribeBase(Asset);
    Result->SetBoolField(TEXT("saved"), true);
    return Encode(Result);
}

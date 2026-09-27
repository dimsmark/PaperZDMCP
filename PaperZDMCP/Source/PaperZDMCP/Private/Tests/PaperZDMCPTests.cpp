#if WITH_DEV_AUTOMATION_TESTS

#include "PaperZDMCPTools.h"
#include "AnimSequences/PaperZDAnimSequence_Flipbook.h"
#include "AnimSequences/Sources/PaperZDAnimationSource.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "PaperFlipbook.h"
#include "PaperZDAnimBP.h"
#include "Serialization/JsonSerializer.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"
#include "UObject/Package.h"

namespace
{
TSharedPtr<FJsonObject> Parse(const FString& Text)
{
    TSharedPtr<FJsonObject> Object;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
    return Object;
}

FString ObjectPath(const FString& Package)
{
    FString Name;
    Package.Split(TEXT("/"), nullptr, &Name, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    return Package + TEXT(".") + Name;
}

UPaperFlipbook* MakeFlipbook(const FString& Path, int32 Frames)
{
    UPackage* Package = CreatePackage(*Path);
    FString Name;
    Path.Split(TEXT("/"), nullptr, &Name, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    auto* Flipbook = NewObject<UPaperFlipbook>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
    FScopedFlipbookMutator Mutator(Flipbook);
    Mutator.KeyFrames.AddDefaulted();
    Mutator.KeyFrames[0].FrameRun = Frames;
    Mutator.FramesPerSecond = 24.0f;
    FAssetRegistryModule::AssetCreated(Flipbook);
    return Flipbook;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPaperZDMCPSchemaTest, "PaperZDMCP.RegistrationAndSchema",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPaperZDMCPSchemaTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("Toolset registered"), UToolsetRegistry::IsToolsetClassRegistered(UPaperZDMCPTools::StaticClass()));
    const FString Schema = UToolsetRegistry::GetToolsetJsonSchema(UPaperZDMCPTools::StaticClass());
    TestTrue(TEXT("Creation tool exported"), Schema.Contains(TEXT("CreateAnimationSequence")));
    TestTrue(TEXT("Notify tool exported"), Schema.Contains(TEXT("AddCustomNotify")));
    TestTrue(TEXT("Save tool exported"), Schema.Contains(TEXT("SaveAsset")));
    TestTrue(TEXT("Array argument exported"), Schema.Contains(TEXT("FlipbookPaths")));
    TestTrue(TEXT("Capabilities return valid JSON"), Parse(UPaperZDMCPTools::GetCapabilities()).IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPaperZDMCPWorkflowTest, "PaperZDMCP.AssetWorkflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPaperZDMCPWorkflowTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("PaperZDMCPScratchProject")))
    {
        AddError(TEXT("Run asset tests in a disposable project with -PaperZDMCPScratchProject. These tests save a test source asset."));
        return false;
    }
    const FString Root = TEXT("/Game/PaperZDMCPTest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = Root + TEXT("/Source");
    const FString SequencePath = Root + TEXT("/Idle");
    const FString BlueprintPath = Root + TEXT("/ABP_Test");
    auto CheckOk = [this](const TCHAR* Label, const FString& Response)
    {
        auto Json = Parse(Response);
        return TestTrue(Label, Json.IsValid() && Json->GetBoolField(TEXT("ok")));
    };
    auto CheckError = [this](const TCHAR* Label, TFunction<FString()> Call)
    {
        UE::ToolsetRegistry::FToolCallExceptionHandler Errors;
        FString Response;
        Errors.CaptureErrorsIn([&]() { Response = Call(); });
        const auto Json = Parse(Response);
        TestTrue(Label, Json.IsValid() && !Json->GetBoolField(TEXT("ok")));
        TestFalse(TEXT("Failure emits a native MCP script error"), Errors.GetException().IsEmpty());
    };
    if (!CheckOk(TEXT("Create source"), UPaperZDMCPTools::CreateAnimationSource(SourcePath))) return false;
    CheckError(TEXT("Reject collision"), [&]() { return UPaperZDMCPTools::CreateAnimationSource(SourcePath); });
    CheckError(TEXT("Reject engine content"), [&]() { return UPaperZDMCPTools::CreateAnimationSource(TEXT("/Engine/TestPaperZD")); });
    auto* Flipbook = MakeFlipbook(Root + TEXT("/FB_Idle"), 24);
    auto* Other = MakeFlipbook(Root + TEXT("/FB_Other"), 24);
    auto* Short = MakeFlipbook(Root + TEXT("/FB_Short"), 6);
    const TArray<FString> Paths = { Flipbook->GetPathName() };
    CheckError(TEXT("Missing flipbook rejected"), [&]() { return UPaperZDMCPTools::CreateAnimationSequence(Root + TEXT("/Bad"), SourcePath, {Root + TEXT("/Missing")}); });
    TestNull(TEXT("No package created on validation failure"), FindPackage(nullptr, *(Root + TEXT("/Bad"))));
    if (!CheckOk(TEXT("Create sequence"), UPaperZDMCPTools::CreateAnimationSequence(SequencePath, SourcePath, Paths))) return false;
    auto* Sequence = LoadObject<UPaperZDAnimSequence_Flipbook>(nullptr, *ObjectPath(SequencePath));
    if (!TestNotNull(TEXT("Sequence loads"), Sequence)) return false;
    TestEqual(TEXT("Flipbook assigned"), Sequence->GetAnimDataSource()[0].Animation.Get(), Flipbook);
    TestEqual(TEXT("Duration"), Sequence->GetTotalDuration(), 1.0f);
    TestEqual(TEXT("Source assigned"), Sequence->GetAnimSource()->GetPathName(), ObjectPath(SourcePath));
    TestTrue(TEXT("Creation remains unsaved"), Sequence->GetOutermost()->IsDirty());
    CheckError(TEXT("Negative notify time rejected"), [&]() { return UPaperZDMCPTools::AddCustomNotify(SequencePath, TEXT("Step"), -1); });
    CheckError(TEXT("Out of bounds track rejected"), [&]() { return UPaperZDMCPTools::AddCustomNotify(SequencePath, TEXT("Step"), 0.5f, 500); });
    CheckOk(TEXT("Add notify"), UPaperZDMCPTools::AddCustomNotify(SequencePath, TEXT("Step"), 0.5f));
    TestEqual(TEXT("One notify"), Sequence->GetAnimNotifies().Num(), 1);
    TestTrue(TEXT("Source registers custom notify"), Sequence->GetAnimSource()->GetRegisteredNotifyNames().Contains(TEXT("Step")));
    CheckError(TEXT("Short replacement rejects orphaned notify"), [&]() { return UPaperZDMCPTools::SetSequenceFlipbooks(SequencePath, {Short->GetPathName()}); });
    TestEqual(TEXT("Failed replacement unchanged"), Sequence->GetAnimDataSource()[0].Animation.Get(), Flipbook);
    CheckOk(TEXT("Replace flipbook"), UPaperZDMCPTools::SetSequenceFlipbooks(SequencePath, {Other->GetPathName()}));
    TestEqual(TEXT("Replacement assigned"), Sequence->GetAnimDataSource()[0].Animation.Get(), Other);
    GEditor->UndoTransaction();
    TestEqual(TEXT("Replacement undo restored flipbook"), Sequence->GetAnimDataSource()[0].Animation.Get(), Flipbook);
    CheckError(TEXT("Reject direction count change"), [&]() { return UPaperZDMCPTools::SetSequenceFlipbooks(SequencePath, {Flipbook->GetPathName(), Other->GetPathName()}); });
    CheckOk(TEXT("Create directional sequence"), UPaperZDMCPTools::CreateAnimationSequence(Root + TEXT("/Directional"), SourcePath, {Flipbook->GetPathName(), Other->GetPathName()}));
    const FString NotifyId = Sequence->GetAnimNotifies()[0]->GetName();
    CheckError(TEXT("Unknown notify ID rejected"), [&]() { return UPaperZDMCPTools::RemoveNotify(SequencePath, TEXT("missing")); });
    CheckOk(TEXT("Remove notify"), UPaperZDMCPTools::RemoveNotify(SequencePath, NotifyId));
    TestEqual(TEXT("Notify removed"), Sequence->GetAnimNotifies().Num(), 0);
    GEditor->UndoTransaction();
    TestEqual(TEXT("Notify removal undo"), Sequence->GetAnimNotifies().Num(), 1);
    CheckOk(TEXT("Create blueprint"), UPaperZDMCPTools::CreateAnimationBlueprint(BlueprintPath, SourcePath));
    auto* Blueprint = LoadObject<UPaperZDAnimBP>(nullptr, *ObjectPath(BlueprintPath));
    if (!TestNotNull(TEXT("Blueprint loads"), Blueprint)) return false;
    TestNotNull(TEXT("Animation graph exists"), Blueprint->AnimationGraph.Get());
    CheckError(TEXT("Uncompiled blueprint cannot save"), [&]() { return UPaperZDMCPTools::SaveAsset(BlueprintPath); });
    CheckOk(TEXT("Compile blueprint"), UPaperZDMCPTools::CompileAnimationBlueprint(BlueprintPath));
    auto Listing = Parse(UPaperZDMCPTools::ListAssets(Root, 0, 1));
    TestEqual(TEXT("Page size"), Listing->GetArrayField(TEXT("assets")).Num(), 1);
    TestEqual(TEXT("PaperZD assets only"), static_cast<int32>(Listing->GetNumberField(TEXT("total"))), 4);
    TestTrue(TEXT("Beyond final page empty"), Parse(UPaperZDMCPTools::ListAssets(Root, MAX_int32, 1))->GetArrayField(TEXT("assets")).IsEmpty());
    CheckOk(TEXT("Save source"), UPaperZDMCPTools::SaveAsset(SourcePath));
    TestFalse(TEXT("Source clean after save"), Sequence->GetAnimSource()->GetOutermost()->IsDirty());
    TestTrue(TEXT("Saving source leaves sequence dirty"), Sequence->GetOutermost()->IsDirty());
    // Save only the self-contained source. Sequences use ephemeral flipbook fixtures.
    // Tests create isolated assets under a unique test prefix; run in a disposable project.
    return true;
}

#endif

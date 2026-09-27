#if WITH_DEV_AUTOMATION_TESTS
#include "UObject/SavePackage.h"
#include "PlayInEditorDataTypes.h"
#include "PackageTools.h"
#include "HAL/PlatformProcess.h"
#include "Algo/Reverse.h"
namespace PaperZDMCPGraph
{
FString PersistenceManifest() { return FPaths::ProjectSavedDir()/TEXT("PaperZDMCPOwnerPersistence.json"); }
bool SavePersistenceAsset(UObject* Asset)
{
    UPackage* Package=Asset->GetOutermost(); FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone; Args.SaveFlags=SAVE_NoError;
    return UPackage::SavePackage(Package,Asset,*FPackageName::LongPackageNameToFilename(Package->GetName(),FPackageName::GetAssetPackageExtension()),Args);
}
J PersistenceApply(UPaperZDAnimBP* BP,const J& Doc)
{
    return TestJson(UPaperZDMCPTools::ApplyAnimationGraph(BP->GetPathName(),GraphEncode(Doc),true));
}
FString PersistenceDefault(UPaperZDAnimBP* BP,const TCHAR* Name)
{
    J Doc=ExportObject(BP);
    for (const JV& V:Doc->GetArrayField(TEXT("variables"))) if (V->AsObject()->GetStringField(TEXT("name"))==Name) return V->AsObject()->GetStringField(TEXT("default"));
    return TEXT("MISSING");
}
void PersistenceSetDefault(const J& Doc,const TCHAR* Name,const TCHAR* Value)
{
    for (const JV& V:Doc->GetArrayField(TEXT("variables"))) if (V->AsObject()->GetStringField(TEXT("name"))==Name) V->AsObject()->SetStringField(TEXT("default"),Value);
}
bool PersistenceInspect(FAutomationTestBase* Test,UPaperZDAnimBP* BP)
{
    const int32 Undo=GEditor->Trans->GetQueueLength(); const bool Dirty=BP->GetOutermost()->IsDirty();
    TArray<UObject*> BeforeBlueprints; GetObjectsOfClass(UBlueprint::StaticClass(),BeforeBlueprints);
    bool Supported=!ContainsUnsupported(Val(ExportObject(BP)));
    TArray<UObject*> AfterBlueprints; GetObjectsOfClass(UBlueprint::StaticClass(),AfterBlueprints);
    Test->TestEqual(TEXT("Inspect creates no Blueprint objects"),AfterBlueprints.Num(),BeforeBlueprints.Num());
    Test->TestEqual(TEXT("Generated class still belongs to real Blueprint"),BP->GeneratedClass->ClassGeneratedBy.Get(),static_cast<UObject*>(BP));
    Test->TestTrue(TEXT("Owner recipe recognized after lifecycle change"),Supported);
    Test->TestEqual(TEXT("Inspect does not dirty package"),BP->GetOutermost()->IsDirty(),Dirty);
    Test->TestEqual(TEXT("Inspect creates no undo entry"),GEditor->Trans->GetQueueLength(),Undo);
    return Supported;
}
// Finish a real in-process PIE cycle on later editor ticks, then save the
// fixture for a different editor process. Never use another user's editor.
class FOwnerPersistencePIE final : public IAutomationLatentCommand
{
    FAutomationTestBase* Test; TStrongObjectPtr<UPaperZDAnimBP> BP; J Manifest;
    double Started=FPlatformTime::Seconds(); bool Stopping=false;
public:
    FOwnerPersistencePIE(FAutomationTestBase* InTest,UPaperZDAnimBP* InBP,const J& InManifest):Test(InTest),BP(InBP),Manifest(InManifest){}
    bool Update() override
    {
        if (FPlatformTime::Seconds()-Started>45)
        {
            Test->AddError(TEXT("PIE lifecycle timed out; persistence fixture is not certified.")); GEditor->RequestEndPlayMap(); return true;
        }
        if (!Stopping)
        {
            if (!GEditor->PlayWorld) return false;
            Test->AddInfo(TEXT("Real PIE world started.")); GEditor->RequestEndPlayMap(); Stopping=true; return false;
        }
        if (GEditor->PlayWorld) return false;
        Test->AddInfo(TEXT("Real PIE world stopped."));
        PersistenceInspect(Test,BP.Get());
        Test->TestEqual(TEXT("CDO default survives PIE"),PersistenceDefault(BP.Get(),TEXT("Speed")),FString(TEXT("37.25")));
        Test->TestTrue(TEXT("Save fixture after PIE"),SavePersistenceAsset(BP.Get()));
        Manifest->SetStringField(TEXT("document"),ExportDocument(BP.Get()));
        Test->TestTrue(TEXT("Save cold-start manifest"),FFileHelper::SaveStringToFile(GraphEncode(Manifest),*PersistenceManifest()));
        return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPaperZDOwnerPersistenceTest,"PaperZDMCP.OwnerPersistence",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FPaperZDOwnerPersistenceTest::RunTest(const FString& Parameters)
{
    using namespace PaperZDMCPGraph;
    if (!FParse::Param(FCommandLine::Get(),TEXT("PaperZDMCPScratchProject"))) { AddError(TEXT("Run in a disposable project.")); return false; }
    const bool Cold=FParse::Param(FCommandLine::Get(),TEXT("PaperZDMCPColdStart"));
    J Manifest; UPaperZDAnimBP* BP=nullptr;
    if (Cold)
    {
        FString Text;
        if (!TestTrue(TEXT("Seed manifest exists"),FFileHelper::LoadFileToString(Text,*PersistenceManifest()))) return false;
        Manifest=TestJson(Text); if (!TestTrue(TEXT("Valid seed manifest"),Manifest.IsValid())) return false;
        TestTrue(TEXT("Cold verification is in a different OS process"),Manifest->GetNumberField(TEXT("pid"))!=FPlatformProcess::GetCurrentProcessId());
        const FString Path=Manifest->GetStringField(TEXT("asset"));
        TestNull(TEXT("Fixture not already loaded in fresh editor"),FindObject<UPaperZDAnimBP>(nullptr,*Path));
        BP=LoadObject<UPaperZDAnimBP>(nullptr,*Path);
        if (!TestNotNull(TEXT("Saved fixture loads"),BP)||!PersistenceInspect(this,BP)) return false;
        TestEqual(TEXT("Cold-start export exactly matches saved export"),ExportDocument(BP),Manifest->GetStringField(TEXT("document")));
        TestEqual(TEXT("Nonzero float survives cold start"),PersistenceDefault(BP,TEXT("Speed")),FString(TEXT("37.25")));
        TestEqual(TEXT("True bool survives cold start"),PersistenceDefault(BP,TEXT("bTurning")),FString(TEXT("true")));
        const int32 ColdUndo=GEditor->Trans->GetQueueLength();
        TestFalse(TEXT("Cold-loaded package is clean"),BP->GetOutermost()->IsDirty());
        J NoOp=PersistenceApply(BP,ExportObject(BP));
        TestTrue(TEXT("Immediate cold-start apply is no-op"),NoOp->GetBoolField(TEXT("ok"))&&!NoOp->GetBoolField(TEXT("changed")));
        TestFalse(TEXT("Cold-start no-op stays clean"),BP->GetOutermost()->IsDirty());
        TestEqual(TEXT("Cold-start no-op adds no undo"),GEditor->Trans->GetQueueLength(),ColdUndo);
    }
    else
    {
        const FString Root=TEXT("/Game/PaperZDMCPOwner_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
        UPackage* OwnerPackage=CreatePackage(*(Root+TEXT("/BP_Owner")));
        UBlueprint* Owner=FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),OwnerPackage,TEXT("BP_Owner"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass());
        for (const TCHAR* Name:{TEXT("AnimSpeed"),TEXT("AnimVelZ"),TEXT("bWasFalling"),TEXT("bTurning")})
        {
            FEdGraphPinType Type; Type.PinCategory=Name[0]=='b'?UEdGraphSchema_K2::PC_Boolean:UEdGraphSchema_K2::PC_Real;
            if (Name[0]!='b') Type.PinSubCategory=FString(Name)==TEXT("AnimSpeed")?UEdGraphSchema_K2::PC_Float:UEdGraphSchema_K2::PC_Double;
            TestTrue(TEXT("Create Blueprint owner property"),FBlueprintEditorUtils::AddMemberVariable(Owner,Name,Type));
        }
        FKismetEditorUtilities::CompileBlueprint(Owner); TestTrue(TEXT("Save owner Blueprint"),SavePersistenceAsset(Owner));
        const FString Source=Root+TEXT("/Source"),Path=Root+TEXT("/ABP_Test");
        UPaperZDMCPTools::CreateAnimationSource(Source); UPaperZDMCPTools::CreateAnimationBlueprint(Path,Source);
        BP=GraphBlueprint(Path); if (!TestNotNull(TEXT("Persistence AnimBP created"),BP)) return false;
        J Doc=ExportObject(BP);
        Doc->SetArrayField(TEXT("variables"),{
            Val(TestJson(TEXT("{\"name\":\"Speed\",\"type\":\"float\",\"default\":\"0.0\"}"))),
            Val(TestJson(TEXT("{\"name\":\"VelZ\",\"type\":\"float\",\"default\":\"\"}"))),
            Val(TestJson(TEXT("{\"name\":\"bFalling\",\"type\":\"bool\",\"default\":\"false\"}"))),
            Val(TestJson(TEXT("{\"name\":\"bTurning\",\"type\":\"bool\",\"default\":\"\"}")))});
        J Spec=TestJson(TEXT("{\"class\":\"\",\"bindings\":[{\"variable\":\"Speed\",\"property\":\"AnimSpeed\"},{\"variable\":\"VelZ\",\"property\":\"AnimVelZ\"},{\"variable\":\"bFalling\",\"property\":\"bWasFalling\"},{\"variable\":\"bTurning\",\"property\":\"bTurning\"}]}"));
        Spec->SetStringField(TEXT("class"),Owner->GeneratedClass->GetPathName()); Doc->SetObjectField(TEXT("owner_update"),Spec);
        J Applied=PersistenceApply(BP,Doc); if (!TestTrue(TEXT("Four owner bindings apply"),Applied->GetBoolField(TEXT("ok")))) return false;
        TestEqual(TEXT("Explicit float zero"),PersistenceDefault(BP,TEXT("Speed")),FString(TEXT("0.0")));
        TestEqual(TEXT("Implicit float zero"),PersistenceDefault(BP,TEXT("VelZ")),FString(TEXT("0.0")));
        TestEqual(TEXT("Explicit bool zero"),PersistenceDefault(BP,TEXT("bFalling")),FString(TEXT("false")));
        TestEqual(TEXT("Implicit bool zero"),PersistenceDefault(BP,TEXT("bTurning")),FString(TEXT("false")));
        TestTrue(TEXT("Save animation source"),SavePersistenceAsset(BP->GetSupportedAnimationSource()));
        Manifest=Obj(); Manifest->SetStringField(TEXT("asset"),BP->GetPathName()); Manifest->SetNumberField(TEXT("pid"),FPlatformProcess::GetCurrentProcessId());
    }
    TStrongObjectPtr<UPaperZDAnimBP> KeepAlive(BP);
    if (!PersistenceInspect(this,BP)) return false;
    // Old releases persisted this field. A stale hash must not require repair
    // or permit a semantic edit. New assets never generate the field.
    for (auto& V:BP->NewVariables) if (V.VarName==OwnerVariable)
    {
        if (!Cold) TestTrue(TEXT("New owner has no saved fingerprint"),VariableMeta(V,TEXT("PaperZDMCP.OwnerFingerprint")).IsEmpty());
        SetVariableMeta(V,TEXT("PaperZDMCP.OwnerFingerprint"),TEXT("legacy-stale-hash"));
    }
    TestTrue(TEXT("Legacy stale hash ignored"),PersistenceInspect(this,BP));
    for (UEdGraph* G:BP->UbergraphPages) for (UEdGraphNode* N:G->Nodes) if (auto* Call=Cast<UK2Node_CallFunction>(N))
        if (Call->FunctionReference.GetMemberName()==InitGraphName||Call->FunctionReference.GetMemberName()==UpdateGraphName) Call->NodeComment=TEXT("Cosmetic comment edit");
    // Reconstruct engine nodes, randomize presentation and array order, and
    // normalize an explicit false to empty. Meaning must remain identical.
    for (UEdGraph* G:BP->FunctionGraphs) if (G->GetFName()==InitGraphName||G->GetFName()==UpdateGraphName)
    {
        for (UEdGraphNode* N:G->Nodes)
        {
            if (Cast<UK2Node_DynamicCast>(N)||Cast<UK2Node_CallFunction>(N)) N->ReconstructNode();
            N->NodeGuid=FGuid::NewGuid(); N->NodePosX+=99; N->NodePosY-=41;
            for (auto* P:N->Pins) { P->PinId=FGuid::NewGuid(); if (P->PinName==TEXT("bPrintToScreen")) P->DefaultValue=TEXT(""); }
            Algo::Reverse(N->Pins);
        }
        Algo::Reverse(G->Nodes);
    }
    TestTrue(TEXT("Rebuilt node identity, pin order and defaults are semantic"),PersistenceInspect(this,BP));
    J Before=ExportObject(BP),Changed=TestJson(GraphEncode(Before));
    PersistenceSetDefault(Changed,TEXT("Speed"),Cold?TEXT("53.5"):TEXT("37.25"));
    PersistenceSetDefault(Changed,TEXT("VelZ"),TEXT("-12.5")); PersistenceSetDefault(Changed,TEXT("bTurning"),TEXT("true"));
    J Result=PersistenceApply(BP,Changed); if (!TestTrue(TEXT("Update existing recognized owner"),Result->GetBoolField(TEXT("ok"))&&Result->GetBoolField(TEXT("changed")))) return false;
    TestTrue(TEXT("Undo defaults"),GEditor->UndoTransaction()); TestEqual(TEXT("Undo restores exact CDO document"),ExportDocument(BP),GraphEncode(Before));
    Result=PersistenceApply(BP,Changed); if (!TestTrue(TEXT("Reapply defaults"),Result->GetBoolField(TEXT("ok")))) return false;
    for (int32 Pass=0;Pass<2;++Pass)
    {
        UPaperZDMCPTools::CompileAnimationBlueprint(BP->GetPathName());
        TestEqual(TEXT("Nonzero CDO default survives subsequent compile"),PersistenceDefault(BP,TEXT("Speed")),FString(Cold?TEXT("53.5"):TEXT("37.25")));
        TestEqual(TEXT("Negative CDO default survives compile"),PersistenceDefault(BP,TEXT("VelZ")),FString(TEXT("-12.5")));
        TestEqual(TEXT("True CDO default survives compile"),PersistenceDefault(BP,TEXT("bTurning")),FString(TEXT("true")));
        PersistenceInspect(this,BP);
    }
    // A reset to the implicit zero must really change the CDO, not preserve its
    // previous nonzero value through the compiler's empty-default convention.
    J Nonzero=ExportObject(BP),Zero=TestJson(GraphEncode(Nonzero));
    PersistenceSetDefault(Zero,TEXT("Speed"),TEXT("")); PersistenceSetDefault(Zero,TEXT("bTurning"),TEXT(""));
    Result=PersistenceApply(BP,Zero); TestTrue(TEXT("Reset nonzero defaults"),Result->GetBoolField(TEXT("ok")));
    UPaperZDMCPTools::CompileAnimationBlueprint(BP->GetPathName());
    TestEqual(TEXT("Reset float CDO to implicit zero"),PersistenceDefault(BP,TEXT("Speed")),FString(TEXT("0.0")));
    TestEqual(TEXT("Reset bool CDO to implicit false"),PersistenceDefault(BP,TEXT("bTurning")),FString(TEXT("false")));
    Result=PersistenceApply(BP,Nonzero); TestTrue(TEXT("Restore nonzero defaults"),Result->GetBoolField(TEXT("ok")));
    BP->GetOutermost()->SetDirtyFlag(false); const int32 Undo=GEditor->Trans->GetQueueLength();
    Result=PersistenceApply(BP,ExportObject(BP)); TestTrue(TEXT("Repeated inspected document is no-op"),Result->GetBoolField(TEXT("ok"))&&!Result->GetBoolField(TEXT("changed")));
    TestFalse(TEXT("No-op stays clean"),BP->GetOutermost()->IsDirty()); TestEqual(TEXT("No-op adds no undo"),GEditor->Trans->GetQueueLength(),Undo);
    auto RejectEdit=[&](const TCHAR* Label)
    {
        TestTrue(Label,ContainsUnsupported(Val(ExportObject(BP))));
        const FString Text=GraphText(UpdateGraph(BP)); const int32 Count=GEditor->Trans->GetQueueLength(); const bool Dirty=BP->GetOutermost()->IsDirty();
        UE::ToolsetRegistry::FToolCallExceptionHandler Errors; J Failure;
        Errors.CaptureErrorsIn([&](){Failure=PersistenceApply(BP,Nonzero);});
        TestTrue(TEXT("Manual edit refuses apply"),Failure&&!Failure->GetBoolField(TEXT("ok")));
        TestEqual(TEXT("Manual graph left byte identical"),GraphText(UpdateGraph(BP)),Text);
        TestEqual(TEXT("Rejected edit keeps undo"),GEditor->Trans->GetQueueLength(),Count); TestEqual(TEXT("Rejected edit keeps dirty"),BP->GetOutermost()->IsDirty(),Dirty);
    };
    for (UEdGraph* G:BP->FunctionGraphs) if (G->GetFName()==InitGraphName||G->GetFName()==UpdateGraphName)
        for (UEdGraphNode* N:G->Nodes)
        {
            if (auto* Call=Cast<UK2Node_CallFunction>(N)) if (Call->FunctionReference.GetMemberName()==TEXT("PrintString"))
            { auto* Pin=Call->FindPin(TEXT("InString")); const FString Saved=Pin->DefaultValue; Pin->DefaultValue+=TEXT("edited"); RejectEdit(TEXT("Changed literal detected")); Pin->DefaultValue=Saved; }
            if (auto* Call=Cast<UK2Node_CallFunction>(N)) if (Call->FunctionReference.GetMemberName()==TEXT("IsValid"))
            { const FMemberReference Saved=Call->FunctionReference; Call->FunctionReference.SetExternalMember(TEXT("MissingFunction"),UKismetSystemLibrary::StaticClass()); RejectEdit(TEXT("Changed called function detected")); Call->FunctionReference=Saved; }
            if (auto* Get=Cast<UK2Node_VariableGet>(N)) if (!Get->VariableReference.IsSelfContext())
            { const FMemberReference Saved=Get->VariableReference; Get->VariableReference.SetExternalMember(TEXT("MissingProperty"),Saved.GetMemberParentClass()); RejectEdit(TEXT("Changed getter detected")); Get->VariableReference=Saved; }
            if (auto* Set=Cast<UK2Node_VariableSet>(N))
            { const FMemberReference Saved=Set->VariableReference; Set->VariableReference.SetSelfMember(TEXT("MissingTarget")); RejectEdit(TEXT("Changed setter detected")); Set->VariableReference=Saved; }
            if (auto* CastNode=Cast<UK2Node_DynamicCast>(N))
            { UClass* Saved=CastNode->TargetType; CastNode->TargetType=ACharacter::StaticClass(); RejectEdit(TEXT("Changed cast detected")); CastNode->TargetType=Saved; }
            if (auto* Branch=Cast<UK2Node_IfThenElse>(N))
            { auto Links=Branch->GetThenPin()->LinkedTo; Branch->GetThenPin()->BreakAllPinLinks(); RejectEdit(TEXT("Changed execution connection detected")); for (auto* P:Links) Branch->GetThenPin()->MakeLinkTo(P); }
        }
    if (!PersistenceInspect(this,BP)) return false;
    TestTrue(TEXT("Save compiled fixture"),SavePersistenceAsset(BP));
    // Package reload is a separate gate from the process restart below.
    const FString AssetPath=BP->GetPathName(); UPackage* Package=BP->GetOutermost(); KeepAlive.Reset(); BP=nullptr;
    FText ReloadError; TestTrue(TEXT("Reload saved package"),UPackageTools::ReloadPackages({Package},ReloadError,EReloadPackagesInteractionMode::AssumePositive));
    BP=LoadObject<UPaperZDAnimBP>(nullptr,*AssetPath); if (!TestNotNull(TEXT("Reloaded AnimBP"),BP)||!PersistenceInspect(this,BP)) return false;
    if (Cold)
    {
        TestEqual(TEXT("Cold-start edit and reload retains changed default"),PersistenceDefault(BP,TEXT("Speed")),FString(TEXT("53.5")));
        AddInfo(TEXT("Verified saved assets in a new editor process, edited, compiled and reloaded successfully.")); return true;
    }
    FRequestPlaySessionParams Params; Params.WorldType=EPlaySessionWorldType::SimulateInEditor;
    GEditor->RequestPlaySession(Params);
    ADD_LATENT_AUTOMATION_COMMAND(FOwnerPersistencePIE(this,BP,Manifest));
    return true;
}
#endif

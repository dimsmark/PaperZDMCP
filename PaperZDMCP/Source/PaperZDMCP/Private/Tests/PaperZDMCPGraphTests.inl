#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PaperFlipbook.h"
#include "PaperZDAnimBPGeneratedClass.h"
#include "Editor/TransBuffer.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"
namespace PaperZDMCPGraph
{
J TestJson(const FString& S) { J O; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(S),O); return O; }
J TestId(const FString& Type=TEXT(""))
{
    J O=Obj(); O->SetStringField(TEXT("id"),FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower)); O->SetNumberField(TEXT("x"),0); O->SetNumberField(TEXT("y"),0);
    if (!Type.IsEmpty()) O->SetStringField(TEXT("type"),Type); return O;
}
J TestPlay(const FString& Seq)
{
    J O=TestId(TEXT("play_sequence")); O->SetStringField(TEXT("sequence"),Seq); O->SetBoolField(TEXT("loop"),false); O->SetNumberField(TEXT("play_rate"),1); O->SetNumberField(TEXT("start_position"),0); return O;
}
J TestAnim(const J& Node)
{
    J O=Obj(); O->SetObjectField(TEXT("root"),TestId()); O->SetStringField(TEXT("output"),Node->GetStringField(TEXT("id"))); O->SetArrayField(TEXT("nodes"),{Val(Node)}); return O;
}
J TestState(const FString& Name,const J& Graph)
{
    J O=TestId(TEXT("state")); O->SetStringField(TEXT("name"),Name); O->SetObjectField(TEXT("graph"),Graph);
    O->SetStringField(TEXT("OnStateEnterEventName"),TEXT("None")); O->SetStringField(TEXT("OnStateExitEventName"),TEXT("None")); return O;
}
J TestJump(const J& Target)
{
    J O=TestId(TEXT("jump")); O->SetStringField(TEXT("name"),TEXT("Reset")); O->SetStringField(TEXT("target"),Target->GetStringField(TEXT("id"))); return O;
}
J TestTransition(const J& From,const J& To,const J& Rule,int32 Priority=0)
{
    J O=TestId(); O->SetStringField(TEXT("from"),From->GetStringField(TEXT("id"))); O->SetStringField(TEXT("to"),To->GetStringField(TEXT("id")));
    O->SetNumberField(TEXT("priority"),Priority); O->SetField(TEXT("blend"),MakeShared<FJsonValueNull>()); O->SetObjectField(TEXT("rule"),Rule);
    O->SetBoolField(TEXT("transitional_enabled"),false); O->SetField(TEXT("transitional_animation"),MakeShared<FJsonValueNull>()); O->SetStringField(TEXT("color"),TEXT("FFFFFFFF")); return O;
}
J TestMachine(const FString& Name,const J& Initial,const TArray<JV>& Nodes,const TArray<JV>& Transitions)
{
    J O=TestId(TEXT("state_machine")),M=Obj(); O->SetStringField(TEXT("name"),Name); M->SetObjectField(TEXT("entry"),TestId()); M->SetStringField(TEXT("entry_state"),Initial->GetStringField(TEXT("id")));
    M->SetArrayField(TEXT("nodes"),Nodes); M->SetArrayField(TEXT("transitions"),Transitions); O->SetObjectField(TEXT("machine"),M); return O;
}
void TestPureFunction(UPaperZDAnimBP* BP)
{
    auto* G=FBlueprintEditorUtils::CreateNewGraph(BP,TEXT("CanMove"),UEdGraph::StaticClass(),UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP,G,true,nullptr); UK2Node_FunctionEntry* Entry=nullptr;
    for (UEdGraphNode* N:G->Nodes) if (auto* E=Cast<UK2Node_FunctionEntry>(N)) Entry=E;
    Entry->AddExtraFlags(FUNC_BlueprintPure|FUNC_BlueprintCallable);
    FGraphNodeCreator<UK2Node_FunctionResult> C(*G); auto* Result=C.CreateNode(false); Result->FunctionReference.SetSelfMember(TEXT("CanMove")); C.Finalize();
    FEdGraphPinType T; T.PinCategory=UEdGraphSchema_K2::PC_Boolean; auto* Pin=Result->CreateUserDefinedPin(TEXT("ReturnValue"),T,EGPD_Input);
    G->GetSchema()->TrySetDefaultValue(*Pin,TEXT("true"));
    G->GetSchema()->TryCreateConnection(Entry->FindPin(UEdGraphSchema_K2::PN_Then),Result->FindPin(UEdGraphSchema_K2::PN_Execute));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPaperZDGraphRoundTripTest,"PaperZDMCP.GraphRoundTrip",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FPaperZDGraphRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace PaperZDMCPGraph;
    if (!FParse::Param(FCommandLine::Get(),TEXT("PaperZDMCPScratchProject"))) { AddError(TEXT("Run in a disposable project.")); return false; }
    FString Root=TEXT("/Game/PaperZDGraphTest_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Source=Root+TEXT("/Source"),Seq=Root+TEXT("/Idle"),Path=Root+TEXT("/ABP_Test");
    UPaperZDMCPTools::CreateAnimationSource(Source);
    UPackage* FBPackage=CreatePackage(*(Root+TEXT("/FB"))); auto* FB=NewObject<UPaperFlipbook>(FBPackage,TEXT("FB"),RF_Public|RF_Standalone|RF_Transactional);
    {FScopedFlipbookMutator M(FB); M.KeyFrames.AddDefaulted(); M.KeyFrames[0].FrameRun=24; M.FramesPerSecond=24;}
    UPaperZDMCPTools::CreateAnimationSequence(Seq,Source,{FB->GetPathName()}); UPaperZDMCPTools::CreateAnimationBlueprint(Path,Source);
    auto* BP=GraphBlueprint(Path); if (!TestNotNull(TEXT("Blueprint created"),BP)) return false;
    TestPureFunction(BP);
    TArray<UK2Node_CallFunction*> UserCalls;
    UEdGraph* Events=nullptr; for (UEdGraph* G:BP->UbergraphPages) if (G!=BP->AnimationGraph) { Events=G; break; }
    if (!Events) { Events=FBlueprintEditorUtils::CreateNewGraph(BP,TEXT("EventGraph"),UEdGraph::StaticClass(),UEdGraphSchema_K2::StaticClass()); FBlueprintEditorUtils::AddUbergraphPage(BP,Events); }
    for (FName Event:{FName(TEXT("OnInit")),FName(TEXT("OnTick"))})
    {
        UK2Node_Event* E=nullptr; for (UEdGraphNode* N:Events->Nodes) if (auto* Candidate=Cast<UK2Node_Event>(N)) if (Candidate->EventReference.GetMemberName()==Event) E=Candidate;
        if (!E) { FGraphNodeCreator<UK2Node_Event> C(*Events); E=C.CreateNode(false); E->EventReference.SetExternalMember(Event,UPaperZDAnimInstance::StaticClass()); E->bOverrideFunction=true; C.Finalize(); }
        FGraphNodeCreator<UK2Node_CallFunction> C(*Events); auto* Call=C.CreateNode(false); Call->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString"))); C.Finalize(); Call->NodeComment=TEXT("Hand-authored logic fixture");
        Events->GetSchema()->TryCreateConnection(E->FindPin(UEdGraphSchema_K2::PN_Then),Call->FindPin(UEdGraphSchema_K2::PN_Execute)); UserCalls.Add(Call);
    }
    J Before=ExportObject(BP),Doc=TestJson(GraphEncode(Before));
    J Player=TestPlay(Seq+TEXT(".Idle")),Idle=TestState(TEXT("Idle"),TestAnim(Player)),Run=TestState(TEXT("Run"),TestAnim(TestPlay(Seq+TEXT(".Idle"))));
    J NestedIdle=TestState(TEXT("Idle"),TestAnim(TestPlay(Seq+TEXT(".Idle"))));
    J Inner=TestMachine(TEXT("Secondary"),NestedIdle,{Val(NestedIdle),Val(TestJump(NestedIdle))},{});
    J NestedState=TestState(TEXT("Nested"),TestAnim(Inner));
    J Conduit=TestId(TEXT("conduit")); Conduit->SetStringField(TEXT("name"),TEXT("Gate")); Conduit->SetObjectField(TEXT("rule"),TestJson(TEXT("{\"constant\":true}")));
    J Rule=TestJson(TEXT("{\"all\":[{\"variable\":\"Moving\"},{\"not\":{\"variable\":\"Crouched\"}},{\"variable\":\"Speed\",\"op\":\">\",\"value\":10}]}"));
    J Transition=TestTransition(Idle,Run,Rule,2),Finish=Obj(); Finish->SetStringField(TEXT("animation_finished"),Run->GetObjectField(TEXT("graph"))->GetStringField(TEXT("output")));
    J Back=TestTransition(Run,Idle,Finish);
    J Ratio=Obj(); Ratio->SetBoolField(TEXT("time_remaining"),true); Ratio->SetBoolField(TEXT("ratio"),true); Ratio->SetStringField(TEXT("player"),Player->GetStringField(TEXT("id"))); Ratio->SetStringField(TEXT("op"),TEXT("<=")); Ratio->SetNumberField(TEXT("value"),0.2);
    J ToGate=TestTransition(Idle,Conduit,Ratio,1),FromGate=TestTransition(Conduit,NestedState,TestJson(TEXT("{\"constant\":true}")));
    Conduit->SetObjectField(TEXT("rule"),TestJson(TEXT("{\"any\":[{\"function\":\"CanMove\",\"arguments\":{}},{\"constant\":false}]}")));
    Transition->SetBoolField(TEXT("transitional_enabled"),true); Transition->SetObjectField(TEXT("transitional_animation"),TestAnim(TestPlay(Seq+TEXT(".Idle"))));
    J Machine=TestMachine(TEXT("Locomotion"),Idle,{Val(Idle),Val(Run),Val(Conduit),Val(NestedState),Val(TestJump(Idle))},{Val(Transition),Val(Back),Val(ToGate),Val(FromGate)});
    J Slot=TestId(TEXT("override_slot")); Slot->SetStringField(TEXT("slot"),TEXT("FullBody")); Slot->SetStringField(TEXT("group"),TEXT("DefaultGroup")); Slot->SetStringField(TEXT("input"),Machine->GetStringField(TEXT("id")));
    J Anim=TestAnim(Slot); Anim->SetArrayField(TEXT("nodes"),{Val(Machine),Val(Slot)}); Doc->SetObjectField(TEXT("graph"),Anim);
    Doc->SetArrayField(TEXT("variables"),{Val(TestJson(TEXT("{\"name\":\"Moving\",\"type\":\"bool\",\"default\":\"false\"}"))),Val(TestJson(TEXT("{\"name\":\"Crouched\",\"type\":\"bool\",\"default\":\"false\"}"))),Val(TestJson(TEXT("{\"name\":\"Speed\",\"type\":\"float\",\"default\":\"0\"}")))});
    Doc->SetObjectField(TEXT("owner_update"),TestJson(TEXT("{\"class\":\"/Script/Engine.Character\",\"bindings\":[{\"variable\":\"Crouched\",\"property\":\"bIsCrouched\"}]}")));
    auto Apply=[&](const J& D,bool Replace=true){return TestJson(UPaperZDMCPTools::ApplyAnimationGraph(Path,GraphEncode(D),Replace));};
    auto Reject=[&](const TCHAR* Label,const J& D)
    {
        J Original=ExportObject(BP); const int32 Undo=GEditor->Trans->GetQueueLength(); const bool Dirty=BP->GetOutermost()->IsDirty();
        UE::ToolsetRegistry::FToolCallExceptionHandler Errors; J R; Errors.CaptureErrorsIn([&]()
        {
            // UE's test-only wrapper is a raw, unrooted NewObject. Skeleton
            // compilation can collect it while its TFunction is on the stack.
            TArray<UObject*> Scopes; GetObjectsOfClass(FindObject<UClass>(nullptr,TEXT("/Script/ToolsetRegistry.ToolCallExceptionHandlerTestScope")),Scopes);
            TArray<TStrongObjectPtr<UObject>> KeepAlive; for (auto* Scope:Scopes) KeepAlive.Emplace(Scope);
            R=Apply(D);
        });
        TestTrue(Label,R&&!R->GetBoolField(TEXT("ok"))); TestTrue(TEXT("Failure leaves export unchanged"),Equal(Original,ExportObject(BP)));
        TestEqual(TEXT("Failure leaves undo unchanged"),GEditor->Trans->GetQueueLength(),Undo); TestEqual(TEXT("Failure leaves dirty unchanged"),BP->GetOutermost()->IsDirty(),Dirty); return R;
    };
    J Result=Apply(Doc,false); if (!TestTrue(TEXT("Rich graph compiles and applies"),Result&&Result->GetBoolField(TEXT("ok")))) { AddError(GraphEncode(Result)); return false; }
    TestTrue(TEXT("Initial apply changes asset"),Result->GetBoolField(TEXT("changed"))); J Exported=ExportObject(BP);
    TestFalse(TEXT("No unsupported export"),ContainsUnsupported(Val(Exported)));
    auto* Generated=Cast<UPaperZDAnimBPGeneratedClass>(BP->GeneratedClass); TestTrue(TEXT("Nested machines compiled"),Generated&&Generated->GetStateMachines().Num()==2);
    for (auto* Call:UserCalls) TestTrue(TEXT("Hand-authored event chain retained after managed hook"),InputNode(Call->FindPin(UEdGraphSchema_K2::PN_Execute))&&InputNode(Call->FindPin(UEdGraphSchema_K2::PN_Execute))->NodeComment==TEXT("PaperZDMCP owner hook"));
    {
        auto* Instance=NewObject<UPaperZDAnimInstance>(GetTransientPackage(),Generated);
        auto* Moving=FindFProperty<FBoolProperty>(Generated,TEXT("Moving")); auto* Crouched=FindFProperty<FBoolProperty>(Generated,TEXT("Crouched")); auto* Speed=FindFProperty<FDoubleProperty>(Generated,TEXT("Speed"));
        const FPaperZDAnimStateMachine* MachineData=Generated->GetStateMachines().FindByPredicate([](const auto& M){return M.MachineName==TEXT("Locomotion");});
        const auto& Link=MachineData->Nodes[MachineData->InitialState].OutwardLinks[0];
        UFunction* Predicate=Instance->FindFunction(MachineData->TransitionRules[Link.TransitionRuleIndex].RuleFunctionName);
        TestNotNull(TEXT("Compiled transition predicate exists"),Predicate);
        for (int32 Mask=0;Mask<8;++Mask)
        {
            Moving->SetPropertyValue_InContainer(Instance,(Mask&1)!=0); Crouched->SetPropertyValue_InContainer(Instance,(Mask&2)!=0); Speed->SetPropertyValue_InContainer(Instance,(Mask&4)?20:0);
            TArray<uint8> Buffer; Buffer.SetNumZeroed(Predicate->ParmsSize); Instance->ProcessEvent(Predicate,Buffer.GetData()); bool Value=false;
            for (TFieldIterator<FBoolProperty> P(Predicate);P;++P) if (P->HasAnyPropertyFlags(CPF_OutParm)) Value=P->GetPropertyValue_InContainer(Buffer.GetData());
            TestEqual(FString::Printf(TEXT("Compiled transition truth table %d"),Mask),Value,(Mask&1)&&!(Mask&2)&&(Mask&4));
        }
        auto* Character=GEditor->GetEditorWorldContext().World()->SpawnActor<ACharacter>();
        FindFProperty<FBoolProperty>(ACharacter::StaticClass(),TEXT("bIsCrouched"))->SetPropertyValue_InContainer(Character,true);
        FindFProperty<FObjectPropertyBase>(Generated,OwnerVariable)->SetObjectPropertyValue_InContainer(Instance,Character);
        Crouched->SetPropertyValue_InContainer(Instance,false); Instance->ProcessEvent(Instance->FindFunction(UpdateGraphName),nullptr);
        TestTrue(TEXT("Generated update reads the cached owner's property at runtime"),Crouched->GetPropertyValue_InContainer(Instance)); Character->Destroy();
    }
    BP->GetOutermost()->SetDirtyFlag(false); const int32 Undo=GEditor->Trans->GetQueueLength();
    J Repeat=Apply(Doc); TestTrue(TEXT("Original request repeated is no-op"),Repeat&&Repeat->GetBoolField(TEXT("ok"))&&!Repeat->GetBoolField(TEXT("changed")));
    TestFalse(TEXT("No-op leaves package clean"),BP->GetOutermost()->IsDirty()); TestEqual(TEXT("No-op creates no undo"),GEditor->Trans->GetQueueLength(),Undo);
    J Exact=Apply(Exported); TestTrue(TEXT("Export applies unchanged"),Exact&&Exact->GetBoolField(TEXT("ok"))&&!Exact->GetBoolField(TEXT("changed")));
    TestEqual(TEXT("Export no-op creates no undo"),GEditor->Trans->GetQueueLength(),Undo);
    TestTrue(TEXT("Single undo works"),GEditor->UndoTransaction()); TestEqual(TEXT("Undo restores exact original document"),ExportDocument(BP),GraphEncode(Before));
    Result=Apply(Doc); if (!Result->GetBoolField(TEXT("ok"))) { AddError(GraphEncode(Result)); return false; }
    Exported=ExportObject(BP);
    const FString ClonePath=Root+TEXT("/ABP_Clone"); UPaperZDMCPTools::CreateAnimationBlueprint(ClonePath,Source); auto* Clone=GraphBlueprint(ClonePath); TestPureFunction(Clone);
    J Cloned=TestJson(UPaperZDMCPTools::ApplyAnimationGraph(ClonePath,GraphEncode(Exported),false));
    TestTrue(TEXT("Clone into another character AnimBP compiles"),Cloned&&Cloned->GetBoolField(TEXT("ok")));
    TestTrue(TEXT("Clone exports equivalent document"),Equal(Exported,ExportObject(Clone)));
    FFileHelper::SaveStringToFile(ExportDocument(Clone),*(FPaths::ProjectSavedDir()/TEXT("PaperZDGraphClone.json")));
    FFileHelper::SaveStringToFile(GraphEncode(Exported),*(FPaths::ProjectSavedDir()/TEXT("BeforeUndo.json")));
    FFileHelper::SaveStringToFile(GraphText(UpdateGraph(BP)),*(FPaths::ProjectSavedDir()/TEXT("BeforeUndo-owner.txt")));
    for (UEdGraph* G:BP->FunctionGraphs) if (G->GetFName()==InitGraphName) FFileHelper::SaveStringToFile(GraphText(G),*(FPaths::ProjectSavedDir()/TEXT("BeforeUndo-init.txt")));
    // A rename retains ID-bound transitions. No string substitution of references.
    Idle->SetStringField(TEXT("name"),TEXT("IdleRenamed")); Result=Apply(Doc); TestTrue(TEXT("Rename with existing transitions compiles"),Result&&Result->GetBoolField(TEXT("ok")));
    TestTrue(TEXT("Undo rename"),GEditor->UndoTransaction()); TestEqual(TEXT("Rename undo exact"),ExportDocument(BP),GraphEncode(Exported)); Idle->SetStringField(TEXT("name"),TEXT("Idle"));
    FFileHelper::SaveStringToFile(ExportDocument(BP),*(FPaths::ProjectSavedDir()/TEXT("AfterUndo.json")));
    FFileHelper::SaveStringToFile(GraphText(UpdateGraph(BP)),*(FPaths::ProjectSavedDir()/TEXT("AfterUndo-owner.txt")));
    for (UEdGraph* G:BP->FunctionGraphs) if (G->GetFName()==InitGraphName) FFileHelper::SaveStringToFile(GraphText(G),*(FPaths::ProjectSavedDir()/TEXT("AfterUndo-init.txt")));
    // Ambiguous and wrong-state getters must never bind by guesswork.
    J Extra=TestPlay(Seq+TEXT(".Idle")); Idle->GetObjectField(TEXT("graph"))->SetArrayField(TEXT("nodes"),{Val(Player),Val(Extra)}); Reject(TEXT("Two players rejected"),Doc);
    Idle->GetObjectField(TEXT("graph"))->SetArrayField(TEXT("nodes"),{Val(Player)});
    Ratio->SetStringField(TEXT("player"),Run->GetObjectField(TEXT("graph"))->GetStringField(TEXT("output"))); Reject(TEXT("Wrong-state player rejected"),Doc); Ratio->SetStringField(TEXT("player"),Player->GetStringField(TEXT("id")));
    J Owner=Doc->GetObjectField(TEXT("owner_update")); Owner->SetStringField(TEXT("class"),TEXT("/Script/Engine.DoesNotExist")); Reject(TEXT("Missing cast class rejected"),Doc); Owner->SetStringField(TEXT("class"),TEXT("/Script/Engine.Character"));
    Owner->GetArrayField(TEXT("bindings"))[0]->AsObject()->SetStringField(TEXT("property"),TEXT("MissingProperty")); Reject(TEXT("Missing owner property rejected"),Doc); Owner->GetArrayField(TEXT("bindings"))[0]->AsObject()->SetStringField(TEXT("property"),TEXT("bIsCrouched"));
    // PaperZD does not diagnose nonexistent state callbacks; reject in preflight.
    Idle->SetStringField(TEXT("OnStateEnterEventName"),TEXT("MissingCallback"));
    Reject(TEXT("Missing state callback rejected"),Doc);
    Idle->SetStringField(TEXT("OnStateEnterEventName"),TEXT("None"));
    UEdGraph* UserGraph=nullptr; UK2Node_CallFunction* TickHook=nullptr;
    for (UEdGraph* G:BP->UbergraphPages) for (UEdGraphNode* N:G->Nodes) if (auto* Call=Cast<UK2Node_CallFunction>(N)) if (Call->FunctionReference.GetMemberName()==UpdateGraphName) { UserGraph=G; TickHook=Call; }
    FGraphNodeCreator<UK2Node_CallFunction> BrokenCreator(*UserGraph); auto* Broken=BrokenCreator.CreateNode(false); Broken->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString"))); BrokenCreator.Finalize();
    UserGraph->GetSchema()->TryCreateConnection(TickHook->FindPin(UEdGraphSchema_K2::PN_Then),Broken->FindPin(UEdGraphSchema_K2::PN_Execute));
    Broken->FunctionReference.SetSelfMember(TEXT("MissingFunction"));
    BP->Status=BS_Dirty;
    AddExpectedError(TEXT("MissingFunction"),EAutomationExpectedErrorFlags::Contains,0);
    AddExpectedError(TEXT("no longer exists on node  Missing Function"),EAutomationExpectedErrorFlags::Contains,0);
    J FailedCompile=Reject(TEXT("Actual Blueprint compiler failure rejects apply"),Doc);
    TestTrue(TEXT("Compile diagnostics included"),FailedCompile&&FailedCompile->GetArrayField(TEXT("diagnostics")).Num()>0);
    bool Located=false; for (const JV& V:FailedCompile->GetArrayField(TEXT("diagnostics"))) if (V->AsObject()->HasField(TEXT("node_id"))) Located=true;
    TestTrue(TEXT("Compile error has source node ID"),Located);
    Broken->DestroyNode(); UPaperZDMCPTools::CompileAnimationBlueprint(Path);
    // Author an unsupported custom graph, then modify a different state. The
    // original rule UObject and exported node text must both survive unchanged.
    auto* T=FindGraphNode(BP->AnimationGraph,Transition->GetStringField(TEXT("id"))); auto* CustomGraph=ChildGraph(T,TEXT("BoundGraph"));
    FGraphNodeCreator<UK2Node_CallFunction> C(*CustomGraph); auto* Manual=C.CreateNode(false); Manual->SetFromFunction(UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("RandomBool"))); C.Finalize();
    const FString Hash=GraphFingerprint(CustomGraph),CustomText=GraphText(CustomGraph); J Custom=ExportObject(BP);
    TMap<FString,J> Stubs; CollectCustom(Val(Custom),Stubs); TestEqual(TEXT("One opaque rule stub"),Stubs.Num(),1);
    J Edited=TestJson(GraphEncode(Custom)); Edited->GetObjectField(TEXT("graph"))->GetObjectField(TEXT("root"))->SetNumberField(TEXT("x"),100);
    Result=Apply(Edited); TestTrue(TEXT("Unchanged custom stub accepts unrelated edit"),Result&&Result->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Custom rule text byte-identical"),GraphFingerprint(CustomGraph),Hash); TestEqual(TEXT("Original custom graph retained"),ChildGraph(T,TEXT("BoundGraph")),CustomGraph);
    TestEqual(TEXT("Custom serialized rule byte-identical"),GraphText(CustomGraph),CustomText);
    BP->GetOutermost()->SetDirtyFlag(false); const int32 CustomUndo=GEditor->Trans->GetQueueLength();
    J CustomRepeat=Apply(Edited); TestTrue(TEXT("Custom document repeat no-op"),CustomRepeat->GetBoolField(TEXT("ok"))&&!CustomRepeat->GetBoolField(TEXT("changed")));
    TestFalse(TEXT("Custom repeat leaves package clean"),BP->GetOutermost()->IsDirty()); TestEqual(TEXT("Custom repeat leaves undo unchanged"),GEditor->Trans->GetQueueLength(),CustomUndo);
    TMap<FString,J> ChangedStubs; CollectCustom(Val(Edited),ChangedStubs); for (auto& Pair:ChangedStubs) Pair.Value->SetStringField(TEXT("hash"),TEXT("tampered")); Reject(TEXT("Edited custom stub rejected"),Edited);
    // Owner cast only appears in the init function, never the update function.
    int32 TickCasts=0,InitCasts=0;
    for (UEdGraph* G:BP->FunctionGraphs) for (UEdGraphNode* N:G->Nodes) if (Cast<UK2Node_DynamicCast>(N)) { if (G->GetFName()==InitGraphName) ++InitCasts; if (G->GetFName()==UpdateGraphName) ++TickCasts; }
    TestEqual(TEXT("Owner cast once at init"),InitCasts,1); TestEqual(TEXT("No per-tick cast"),TickCasts,0);
    FFileHelper::SaveStringToFile(GraphEncode(Exported),*(FPaths::ProjectSavedDir()/TEXT("PaperZDGraphRoundTrip.json")));
    return true;
}
#endif

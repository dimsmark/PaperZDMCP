namespace PaperZDMCPGraph
{
void CollectCustom(const JV& V,TMap<FString,J>& Rules)
{
    if (V->Type==EJson::Object)
    {
        J O=V->AsObject(); const J* Rule; FString ID;
        if (O->TryGetStringField(TEXT("id"),ID)&&O->TryGetObjectField(TEXT("rule"),Rule)&&(*Rule)->HasField(TEXT("kind"))) Rules.Add(ID,*Rule);
        for (const auto& Pair:O->Values) CollectCustom(Pair.Value,Rules);
    }
    else if (V->Type==EJson::Array) for (const auto& E:V->AsArray()) CollectCustom(E,Rules);
}
void RestoreCustomStubs(const JV& V,const TMap<FString,J>& Rules)
{
    if (V->Type==EJson::Object)
    {
        J O=V->AsObject(); FString ID;
        if (O->TryGetStringField(TEXT("id"),ID)) if (const J* Original=Rules.Find(ID)) O->SetObjectField(TEXT("rule"),*Original);
        for (const auto& Pair:O->Values) RestoreCustomStubs(Pair.Value,Rules);
    }
    else if (V->Type==EJson::Array) for (const JV& E:V->AsArray()) RestoreCustomStubs(E,Rules);
}
void SnapshotBlueprint(UPaperZDAnimBP* BP)
{
    BP->Modify(); TArray<UEdGraph*> Graphs; BP->GetAllGraphs(Graphs);
    if (BP->GeneratedClass) { UObject* CDO=BP->GeneratedClass->GetDefaultObject(); CDO->SetFlags(RF_Transactional); CDO->Modify(); }
    for (UEdGraph* G:Graphs) SnapshotGraph(G);
}
bool BuildDocument(FDocumentBuilder& B,const J& Doc)
{
    if (!B.Keys(Doc,{TEXT("schema"),TEXT("animation_source"),TEXT("variables"),TEXT("owner_update"),TEXT("graph")})) return false;
    FString Schema,Source;
    if (!B.Text(Doc,TEXT("schema"),Schema)||Schema!=TEXT("paperzd-graph/1")||!B.Text(Doc,TEXT("animation_source"),Source)) return B.Bad(TEXT("Unsupported document schema."));
    auto* SourceAsset=LoadObject<UPaperZDAnimationSource>(nullptr,*Source);
    if (!SourceAsset||!Source.StartsWith(TEXT("/Game/"))) return B.Bad(TEXT("animation_source must be an existing /Game PaperZD source."));
    B.BP->SupportedAnimationSource=SourceAsset;
    if (!Variables(B,Doc)) return false;
    B.Location=TEXT("$.graph");
    if (!B.Anim(B.BP->AnimationGraph,B.Object(Doc,TEXT("graph")))||!B.Rules()) return false;
    if (!FOwnerBuilder(B).Build(Doc->Values[TEXT("owner_update")])) return false;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(B.BP); return true;
}
bool CompileDocument(FDocumentBuilder& B,TArray<JV>& Diagnostics)
{
    FCompilerResultsLog Log; Log.bSilentMode=true;
    FKismetEditorUtilities::CompileBlueprint(B.BP,EBlueprintCompileOptions::SkipGarbageCollection,&Log);
    for (const auto& WeakNode:Log.AnnotatedNodes)
    {
        UEdGraphNode* Node=WeakNode.Get();
        if (!Node||Node->ErrorMsg.IsEmpty()) continue;
        J D=Obj(); D->SetStringField(TEXT("message"),Node->ErrorMsg); D->SetStringField(TEXT("node_id"),Id(Node));
        D->SetStringField(TEXT("severity"),Node->ErrorType==EMessageSeverity::Error?TEXT("error"):TEXT("warning"));
        FString Path=TEXT("$");
        for (UObject* Owner=Node;Owner;Owner=Owner->GetOuter()) if (auto* N=Cast<UEdGraphNode>(Owner)) if (const FString* P=B.Paths.Find(N->NodeGuid)) { Path=*P; break; }
        D->SetStringField(TEXT("document_path"),Path); Diagnostics.Add(Val(D));
    }
    for (const auto& Message:Log.Messages)
    {
        J D=Obj(); D->SetStringField(TEXT("message"),Message->ToText().ToString());
        D->SetStringField(TEXT("severity"),Message->GetSeverity()==EMessageSeverity::Error?TEXT("error"):TEXT("warning")); Diagnostics.Add(Val(D));
    }
    if (Log.NumErrors!=0 || B.BP->Status==BS_Error) return false;
    UObject* CDO=B.BP->GeneratedClass->GetDefaultObject();
    CDO->SetFlags(RF_Transactional); CDO->Modify();
    for (const auto& Pair:B.Defaults)
    {
        FProperty* P=FindFProperty<FProperty>(B.BP->GeneratedClass,Pair.Key);
        if (!P || !FBlueprintEditorUtils::PropertyValueFromString(P,Pair.Value,reinterpret_cast<uint8*>(CDO),CDO))
            return B.Bad(TEXT("Could not set compiled default: ")+Pair.Key.ToString());
        for (auto& V:B.BP->NewVariables) if (V.VarName==Pair.Key) V.DefaultValue=Pair.Value;
    }
    return true;
}
FString ApplyResult(UPaperZDAnimBP* BP,bool Changed,const TArray<JV>& Diagnostics)
{
    J R=Obj(); R->SetBoolField(TEXT("ok"),true); R->SetBoolField(TEXT("changed"),Changed);
    R->SetBoolField(TEXT("dirty"),BP->GetOutermost()->IsDirty()); R->SetObjectField(TEXT("document"),ExportObject(BP));
    R->SetArrayField(TEXT("diagnostics"),Diagnostics); return GraphEncode(R);
}
FString ApplyError(const FString& Message,const TArray<JV>& Diagnostics={})
{
    J R=Obj(); R->SetBoolField(TEXT("ok"),false); R->SetStringField(TEXT("error"),Message); R->SetArrayField(TEXT("diagnostics"),Diagnostics);
    const FString Result=GraphEncode(R); UKismetSystemLibrary::RaiseScriptError(Result); return Result;
}
}

FString UPaperZDMCPTools::ApplyAnimationGraph(const FString& AssetPath,const FString& GraphJson,bool Replace)
{
    using namespace PaperZDMCPGraph;
    auto* BP=GraphBlueprint(AssetPath); if (!BP) return ApplyError(TEXT("Requires a /Game PaperZD AnimBP with source, on the editor thread with PIE stopped."));
    J Doc; if (GraphJson.Len()>1000000||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(GraphJson),Doc)) return ApplyError(TEXT("GraphJson must be a JSON document under 1 MB."));
    const J Before=ExportObject(BP);
    if (ContainsUnsupported(Val(Doc))||ContainsUnsupported(Val(Before))) return ApplyError(TEXT("Unsupported graph/variable or manually edited owner function found. Existing asset was left untouched."));
    if (Equal(Before,Doc)&&(BP->Status==BS_UpToDate||BP->Status==BS_UpToDateWithWarnings)) return ApplyResult(BP,false,{});
    if (!Replace&&(BP->AnimationGraph->Nodes.Num()>1||!BP->NewVariables.IsEmpty())) return ApplyError(TEXT("Asset already has graph content. Inspect it and pass Replace=true explicitly."));
    TMap<FString,J> Custom; CollectCustom(Val(Before),Custom);
    TArray<JV> Diagnostics;
    // Duplicate all owned graph objects. Validation, node creation and compilation
    // happen here before any transaction or mutation on the destination asset.
    TStrongObjectPtr<UPaperZDAnimBP> Stage(DuplicateObject<UPaperZDAnimBP>(BP,GetTransientPackage(),MakeUniqueObjectName(GetTransientPackage(),BP->GetClass(),TEXT("PaperZDMCP_Preflight"))));
    Stage->SetFlags(RF_Transient); Stage->ClearFlags(RF_Public|RF_Standalone);
    ON_SCOPE_EXIT
    {
        // Finish compilation/reinstancing while the private copy is alive,
        // then exclude it from future editor/PIE Blueprint sweeps on all exits.
        FBlueprintCompilationManager::FlushCompilationQueueAndReinstance();
        Stage->Status=BS_UpToDate; Stage->MarkAsGarbage();
    };
    FDocumentBuilder Preview{Stage.Get()}; Preview.CustomRules=Custom;
    {
        FScopedTransaction Preflight(NSLOCTEXT("PaperZDMCP","Preflight","Validate PaperZD document"));
        const bool Built=BuildDocument(Preview,Doc);
        const bool Compiled=Built&&CompileDocument(Preview,Diagnostics);
        Preflight.Cancel();
        if (!Built) return ApplyError(Preview.Error,Diagnostics);
        if (!Compiled) return ApplyError(TEXT("Preflight compile failed; asset unchanged."),Diagnostics);
    }
    // Handles equivalent accepted syntax (e.g. boolean == true), as well as
    // applying the original request twice after the compiler normalized it.
    J Normalized=ExportObject(Stage.Get()); RestoreCustomStubs(Val(Normalized),Custom);
    if (Equal(Before,Normalized)) return ApplyResult(BP,false,Diagnostics);
    bool Success=false; FDocumentBuilder Actual{BP}; Actual.CustomRules=Custom;
    {
        FScopedTransaction Transaction(NSLOCTEXT("PaperZDMCP","ApplyGraph","Apply PaperZD animation graph"));
        SnapshotBlueprint(BP);
        Success=BuildDocument(Actual,Doc);
        if (Success) { Diagnostics.Reset(); Success=CompileDocument(Actual,Diagnostics); }
        if (Success) BP->MarkPackageDirty();
    }
    if (!Success)
    {
        const bool Restored=GEditor->UndoTransaction(false);
        return ApplyError((Restored?TEXT("Apply failed and was undone: "):TEXT("Apply failed; automatic undo also failed: "))+Actual.Error,Diagnostics);
    }
    return ApplyResult(BP,true,Diagnostics);
}

FString UPaperZDMCPTools::GetAnimationGraphSchema()
{
#include "PaperZDMCPGraphSchema.inl"
}

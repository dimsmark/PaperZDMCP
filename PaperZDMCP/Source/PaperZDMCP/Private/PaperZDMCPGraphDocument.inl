// Document codec shares the graph adapters in PaperZDMCPGraphTools.cpp.
namespace PaperZDMCPGraph
{
using J = TSharedPtr<FJsonObject>;
using JV = TSharedPtr<FJsonValue>;
static const FName OwnerDocKey(TEXT("PaperZDMCP.OwnerDocument"));
static const FName OwnerVariable(TEXT("PaperZDMCP_Owner"));
static const FName UpdateGraphName(TEXT("PaperZDMCP_OwnerUpdate"));
static const FName InitGraphName(TEXT("PaperZDMCP_OwnerInit"));
J Obj() { return MakeShared<FJsonObject>(); }
JV Val(const J& O) { return MakeShared<FJsonValueObject>(O); }
J Unsupported(const FString& Reason) { J O=Obj(); O->SetStringField(TEXT("unsupported"),Reason); return O; }
FString Id(UEdGraphNode* N) { return N ? N->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString(); }
J Identity(UEdGraphNode* N)
{
    J O=Obj(); O->SetStringField(TEXT("id"),Id(N)); O->SetNumberField(TEXT("x"),N->NodePosX); O->SetNumberField(TEXT("y"),N->NodePosY); return O;
}
UEdGraphNode* InputNode(UEdGraphPin* P) { return P && P->LinkedTo.Num()==1 ? P->LinkedTo[0]->GetOwningNode() : nullptr; }
UObject* ObjectField(UObject* O,const TCHAR* Name)
{
    auto* P=O ? FindFProperty<FObjectPropertyBase>(O->GetClass(),Name) : nullptr; return P ? P->GetObjectPropertyValue_InContainer(O) : nullptr;
}
FString GraphText(UEdGraph* G)
{
    TSet<UObject*> Nodes; for (UEdGraphNode* N:G->Nodes) Nodes.Add(N);
    FString Text; FEdGraphUtilities::ExportNodesToText(Nodes,Text); return Text;
}
FString GraphFingerprint(UEdGraph* G) { return FMD5::HashAnsiString(*GraphText(G)); }
FString VariableMeta(const FBPVariableDescription& V,FName Key)
{
    for (const FBPVariableMetaDataEntry& E:V.MetaDataArray) if (E.DataKey==Key) return E.DataValue; return FString();
}
void SetVariableMeta(FBPVariableDescription& V,FName Key,const FString& Value)
{
    for (auto& E:V.MetaDataArray) if (E.DataKey==Key) { E.DataValue=Value; return; }
    FBPVariableMetaDataEntry E; E.DataKey=Key; E.DataValue=Value; V.MetaDataArray.Add(E);
}
UEdGraph* UpdateGraph(UPaperZDAnimBP* BP)
{
    for (UEdGraph* G:BP->FunctionGraphs) if (G && G->GetFName()==UpdateGraphName) return G; return nullptr;
}
// The stored document is the recipe, never a saved fingerprint of editor objects.
bool OwnerMatchesDocument(UPaperZDAnimBP* BP,const J& Spec);
FString NormalizeDefault(const FString& Type,const FString& Text)
{
    if (Type==TEXT("bool")) return Text.Equals(TEXT("true"),ESearchCase::IgnoreCase)||Text==TEXT("1")?TEXT("true"):TEXT("false");
    if (Type==TEXT("float"))
    {
        double Number=0; if (!Text.IsEmpty()&&!LexTryParseString(Number,*Text)) return Text;
        return Number==0?TEXT("0.0"):FString::Printf(TEXT("%.17g"),Number);
    }
    return Text;
}
FString VariableDefault(UPaperZDAnimBP* BP,const FBPVariableDescription& V,const FString& Type)
{
    FString Text;
    if (BP->GeneratedClass)
    {
        UObject* CDO=BP->GeneratedClass->GetDefaultObject();
        if (FProperty* P=FindFProperty<FProperty>(BP->GeneratedClass,V.VarName))
            FBlueprintEditorUtils::PropertyValueToString(P,reinterpret_cast<const uint8*>(CDO),Text,CDO);
    }
    return NormalizeDefault(Type,Text);
}
J ReadExpression(UEdGraphPin* Pin,TSet<UEdGraphNode*>& Visited,int32 Depth=0);
J ReadValue(UEdGraphPin* Pin,TSet<UEdGraphNode*>& Visited,int32 Depth)
{
    UEdGraphNode* Node=InputNode(Pin); if (!Node || Depth>32) return Unsupported(TEXT("Missing or cyclic rule value."));
    Visited.Add(Node);
    if (auto* V=Cast<UK2Node_VariableGet>(Node))
    {
        auto* Self=V->FindPin(UEdGraphSchema_K2::PN_Self);
        if (!V->VariableReference.IsSelfContext() || (Self && Self->LinkedTo.Num())) return Unsupported(TEXT("Only AnimInstance member variables are supported in rules."));
        J O=Obj(); O->SetStringField(TEXT("variable"),V->VariableReference.GetMemberName().ToString()); return O;
    }
    if (auto* C=Cast<UK2Node_CallFunction>(Node))
    {
        const FString Name=C->FunctionReference.GetMemberName().ToString();
        if (Name==TEXT("Conv_FloatToDouble")&&C->GetTargetFunction()&&C->GetTargetFunction()->GetOuterUClass()==UKismetMathLibrary::StaticClass()) return ReadValue(C->FindPin(TEXT("InFloat")),Visited,Depth+1);
        if (HasNodeClass(Node,TEXT("PaperZDK2Node_AnimGetter")) && (Name==TEXT("GetInstanceAssetPlayerTimeFromEnd") || Name==TEXT("GetInstanceAssetPlayerTimeFromEndFraction") || Name==TEXT("GetInstanceAssetPlayerTime")))
        {
            auto* P=Cast<UEdGraphNode>(ObjectField(Node,TEXT("SourceNode"))); if (!Playback(P)) return Unsupported(TEXT("Animation getter has no supported player."));
            J O=Obj(); O->SetBoolField(TEXT("time_remaining"),true); O->SetStringField(TEXT("player"),Id(P));
            if (Name==TEXT("GetInstanceAssetPlayerTimeFromEndFraction")) O->SetBoolField(TEXT("ratio"),true);
            if (Name==TEXT("GetInstanceAssetPlayerTime")) O->SetBoolField(TEXT("reverse_finished"),true); return O;
        }
        UFunction* F=C->GetTargetFunction(); auto* Self=C->FindPin(UEdGraphSchema_K2::PN_Self);
        if (F && F->HasAllFunctionFlags(FUNC_BlueprintPure|FUNC_BlueprintCallable) && !F->HasAnyFunctionFlags(FUNC_Static|FUNC_Delegate) && !F->HasMetaData(TEXT("AnimGetter")) &&
            (!Self || Self->LinkedTo.IsEmpty()) && F->GetOuterUClass()->IsChildOf(UPaperZDAnimInstance::StaticClass()))
        {
            J O=Obj(), Args=Obj();
            for (UEdGraphPin* P:C->Pins) if (P->Direction==EGPD_Input && P!=Self)
            {
                if (P->LinkedTo.Num() || P->DefaultObject || P->PinType.IsContainer()) return Unsupported(TEXT("Pure function inputs must be scalar literals."));
                Args->SetStringField(P->PinName.ToString(),P->DefaultValue);
            }
            O->SetStringField(TEXT("function"),Name); O->SetObjectField(TEXT("arguments"),Args); return O;
        }
    }
    return Unsupported(TEXT("Rule value is outside the supported variable/getter/pure-function language."));
}
J ReadExpression(UEdGraphPin* Pin,TSet<UEdGraphNode*>& Visited,int32 Depth)
{
    if (!Pin || Depth>32) return Unsupported(TEXT("Missing pin or cyclic rule graph."));
    if (Pin->LinkedTo.IsEmpty()) { J O=Obj(); O->SetBoolField(TEXT("constant"),Pin->DefaultValue.Equals(TEXT("true"),ESearchCase::IgnoreCase)||Pin->DefaultValue==TEXT("1")); return O; }
    auto* C=Cast<UK2Node_CallFunction>(InputNode(Pin)); if (!C) return ReadValue(Pin,Visited,Depth+1);
    Visited.Add(C); const FString Name=C->FunctionReference.GetMemberName().ToString();
    if (!C->GetTargetFunction()||C->GetTargetFunction()->GetOuterUClass()!=UKismetMathLibrary::StaticClass()) return ReadValue(Pin,Visited,Depth+1);
    if (Name==TEXT("Not_PreBool"))
    {
        J Child=ReadExpression(C->FindPin(TEXT("A")),Visited,Depth+1); bool B; J O=Obj();
        if (Child->TryGetBoolField(TEXT("constant"),B)) O->SetBoolField(TEXT("constant"),!B); else O->SetObjectField(TEXT("not"),Child); return O;
    }
    if (Name==TEXT("BooleanAND") || Name==TEXT("BooleanOR"))
    {
        FString Key=Name==TEXT("BooleanAND")?TEXT("all"):TEXT("any"); TArray<JV> Items;
        for (const TCHAR* P:{TEXT("A"),TEXT("B")})
        {
            J Child=ReadExpression(C->FindPin(P),Visited,Depth+1); const TArray<JV>* Nested;
            if (Child->TryGetArrayField(Key,Nested)) Items.Append(*Nested); else Items.Add(Val(Child));
        }
        J O=Obj(); O->SetArrayField(Key,Items); return O;
    }
    static const TMap<FString,FString> Comparisons={
        {TEXT("Greater_DoubleDouble"),TEXT(">")},{TEXT("GreaterEqual_DoubleDouble"),TEXT(">=")},
        {TEXT("Less_DoubleDouble"),TEXT("<")},{TEXT("LessEqual_DoubleDouble"),TEXT("<=")},
        {TEXT("EqualEqual_DoubleDouble"),TEXT("==")},{TEXT("NotEqual_DoubleDouble"),TEXT("!=")}};
    if (const FString* Op=Comparisons.Find(Name))
    {
        auto* B=C->FindPin(TEXT("B")); double Value;
        if (!B || B->LinkedTo.Num() || !LexTryParseString(Value,*B->DefaultValue) || !FMath::IsFinite(Value)) return Unsupported(TEXT("Comparison RHS must be a numeric literal."));
        J O=ReadValue(C->FindPin(TEXT("A")),Visited,Depth+1);
        if (O->HasField(TEXT("time_remaining")) && !O->HasField(TEXT("ratio")) && *Op==TEXT("<=") && FMath::IsNearlyEqual(Value,0.0001,1e-12))
        {
            auto* BP=Cast<UPaperZDAnimBP>(FBlueprintEditorUtils::FindBlueprintForGraph(C->GetGraph()));
            auto* P=BP?FindGraphNode(BP->AnimationGraph,O->GetStringField(TEXT("player"))):nullptr; auto* Data=Playback(P);
            if (Data && !Data->bLoopAnimation && ((Data->PlayRate<0)==O->HasField(TEXT("reverse_finished"))))
            { J F=Obj(); F->SetStringField(TEXT("animation_finished"),Id(P)); return F; }
        }
        if (O->HasField(TEXT("reverse_finished"))) return Unsupported(TEXT("Elapsed time is supported only as reverse animation_finished."));
        O->SetStringField(TEXT("op"),*Op); O->SetNumberField(TEXT("value"),Value); return O;
    }
    return ReadValue(Pin,Visited,Depth+1);
}
J ExportRule(UEdGraph* Graph)
{
    auto* Result=FirstNode(Graph,ResultClass);
    TSet<UEdGraphNode*> Visited; Visited.Add(Result);
    J O=Result?ReadExpression(Result->FindPin(TEXT("bCanEnterTransition")),Visited):Unsupported(TEXT("Missing rule result."));
    bool Custom=GraphEncode(O).Contains(TEXT("\"unsupported\""));
    if (Graph) for (UEdGraphNode* N:Graph->Nodes) if (!Visited.Contains(N)) Custom=true;
    if (!Custom && Graph)
    {
        auto* BP=Cast<UPaperZDAnimBP>(FBlueprintEditorUtils::FindBlueprintForGraph(Graph));
        auto* Owner=Cast<UEdGraphNode>(Graph->GetOuter()); FString Why;
        auto* From=HasNodeClass(Owner,TransitionClass)?TransitionEndpoint(Owner,EGPD_Input):nullptr;
        if (!BP||!ReadRule(GraphEncode(O),BP,From,Why)) Custom=true;
    }
    if (Custom)
    {
        O=Obj(); O->SetStringField(TEXT("kind"),TEXT("custom"));
        O->SetStringField(TEXT("hash"),Graph?GraphFingerprint(Graph):TEXT("missing"));
        O->SetStringField(TEXT("reason"),TEXT("Rule is outside the supported condition language. Preserve this stub unchanged; edit the custom rule in Unreal."));
    }
    return O;
}
J ExportAnimGraph(UEdGraph* Graph,int32 Depth=0);
J ExportMachine(UEdGraph* Graph,int32 Depth)
{
    if (!Graph || Depth>16) return Unsupported(TEXT("Missing or too deeply nested state machine."));
    J O=Obj(); TArray<JV> Nodes,Transitions; auto* Root=FirstNode(Graph,TEXT("PaperZDStateGraphNode_Root"));
    if (!Root) return Unsupported(TEXT("State machine has no entry node."));
    O->SetObjectField(TEXT("entry"),Identity(Root)); O->SetStringField(TEXT("entry_state"),Id(InputNode(DirectionPin(Root,EGPD_Output))));
    for (UEdGraphNode* Node:Graph->Nodes)
    {
        if (Node==Root) continue; J N=Identity(Node);
        if (HasNodeClass(Node,StateClass))
        {
            N->SetStringField(TEXT("type"),TEXT("state")); auto* G=ChildGraph(Node,TEXT("BoundGraph"));
            N->SetStringField(TEXT("name"),G?G->GetName():TEXT("")); N->SetObjectField(TEXT("graph"),ExportAnimGraph(G,Depth+1));
            for (const TCHAR* F:{TEXT("OnStateEnterEventName"),TEXT("OnStateExitEventName")})
                if (auto* P=FindFProperty<FNameProperty>(Node->GetClass(),F)) N->SetStringField(F,P->GetPropertyValue_InContainer(Node).ToString());
        }
        else if (HasNodeClass(Node,TEXT("PaperZDStateGraphNode_Conduit")))
        {
            N->SetStringField(TEXT("type"),TEXT("conduit")); auto* G=ChildGraph(Node,TEXT("BoundGraph"));
            N->SetStringField(TEXT("name"),G?G->GetName():TEXT("")); N->SetObjectField(TEXT("rule"),ExportRule(G));
        }
        else if (HasNodeClass(Node,TEXT("PaperZDStateGraphNode_Jump")))
        {
            N->SetStringField(TEXT("type"),TEXT("jump"));
            auto* P=FindFProperty<FNameProperty>(Node->GetClass(),TEXT("Name"));
            N->SetStringField(TEXT("name"),P?P->GetPropertyValue_InContainer(Node).ToString():TEXT(""));
            N->SetStringField(TEXT("target"),Id(InputNode(DirectionPin(Node,EGPD_Output))));
        }
        else if (HasNodeClass(Node,TransitionClass))
        {
            N->SetStringField(TEXT("from"),Id(TransitionEndpoint(Node,EGPD_Input))); N->SetStringField(TEXT("to"),Id(TransitionEndpoint(Node,EGPD_Output)));
            auto* P=FindFProperty<FIntProperty>(Node->GetClass(),TEXT("Priority")); N->SetNumberField(TEXT("priority"),P?P->GetPropertyValue_InContainer(Node):0);
            N->SetField(TEXT("blend"),MakeShared<FJsonValueNull>()); N->SetObjectField(TEXT("rule"),ExportRule(ChildGraph(Node,TEXT("BoundGraph"))));
            auto* E=FindFProperty<FBoolProperty>(Node->GetClass(),TEXT("bPlayTransitionalAnimations")); auto* G=ChildGraph(Node,TEXT("AnimTransitionalGraph"));
            N->SetBoolField(TEXT("transitional_enabled"),E&&E->GetPropertyValue_InContainer(Node));
            N->SetField(TEXT("transitional_animation"),G?Val(ExportAnimGraph(G,Depth+1)):MakeShared<FJsonValueNull>());
            auto* Color=FindFProperty<FStructProperty>(Node->GetClass(),TEXT("Color")); N->SetStringField(TEXT("color"),Color?Color->ContainerPtrToValuePtr<FColor>(Node)->ToHex():TEXT("FFFFFFFF"));
            Transitions.Add(Val(N)); continue;
        }
        else return Unsupported(TEXT("Unsupported state-machine node: ")+Node->GetClass()->GetName());
        Nodes.Add(Val(N));
    }
    Nodes.Sort([](const JV& A,const JV& B){return A->AsObject()->GetStringField(TEXT("name"))<B->AsObject()->GetStringField(TEXT("name"));});
    TMap<FString,FString> Names; for (const JV& N:Nodes) Names.Add(N->AsObject()->GetStringField(TEXT("id")),N->AsObject()->GetStringField(TEXT("name")));
    Transitions.Sort([&](const JV& A,const JV& B)
    {
        J X=A->AsObject(),Y=B->AsObject();
        FString XF=Names.FindRef(X->GetStringField(TEXT("from"))),YF=Names.FindRef(Y->GetStringField(TEXT("from")));
        if (XF!=YF) return XF<YF;
        FString XT=Names.FindRef(X->GetStringField(TEXT("to"))),YT=Names.FindRef(Y->GetStringField(TEXT("to")));
        if (XT!=YT) return XT<YT; return X->GetNumberField(TEXT("priority"))<Y->GetNumberField(TEXT("priority"));
    });
    O->SetArrayField(TEXT("nodes"),Nodes); O->SetArrayField(TEXT("transitions"),Transitions); return O;
}
J ExportAnimGraph(UEdGraph* Graph,int32 Depth)
{
    if (!Graph || Depth>16) return Unsupported(TEXT("Missing or too deeply nested animation graph."));
    auto* Root=FirstNode(Graph,SinkClass); if (!Root) return Unsupported(TEXT("Animation graph has no output."));
    J O=Obj(); O->SetObjectField(TEXT("root"),Identity(Root)); O->SetStringField(TEXT("output"),Id(InputNode(DirectionPin(Root,EGPD_Input)))); TArray<JV> Nodes;
    for (UEdGraphNode* Node:Graph->Nodes)
    {
        if (Node==Root) continue; J N=Identity(Node);
        if (auto* P=Playback(Node))
        {
            for (UEdGraphPin* Pin:Node->Pins) if (Pin->Direction==EGPD_Input && Pin->LinkedTo.Num()) return Unsupported(TEXT("Externally driven Play Sequence pins are unsupported."));
            N->SetStringField(TEXT("type"),TEXT("play_sequence")); N->SetStringField(TEXT("sequence"),GetPathNameSafe(P->AnimSequence));
            N->SetBoolField(TEXT("loop"),P->bLoopAnimation); N->SetNumberField(TEXT("play_rate"),P->PlayRate); N->SetNumberField(TEXT("start_position"),P->StartPosition);
            for (UEdGraphPin* Pin:Node->Pins) if (Pin->Direction==EGPD_Input)
            {
                if (Pin->PinName==TEXT("PlayRate")||Pin->PinName==TEXT("StartPosition"))
                {
                    double Value; if (!LexTryParseString(Value,*Pin->DefaultValue)||!FMath::IsFinite(Value)) return Unsupported(TEXT("Invalid playback pin literal."));
                    N->SetNumberField(Pin->PinName==TEXT("PlayRate")?TEXT("play_rate"):TEXT("start_position"),Value);
                }
                else if (Pin->PinName==TEXT("bLoopAnimation")) N->SetBoolField(TEXT("loop"),Pin->DefaultValue.Equals(TEXT("true"),ESearchCase::IgnoreCase)||Pin->DefaultValue==TEXT("1"));
                else if (Pin->PinName==TEXT("AnimSequence")) N->SetStringField(TEXT("sequence"),GetPathNameSafe(Pin->DefaultObject));
                else return Unsupported(TEXT("Unsupported exposed playback pin."));
            }
        }
        else if (HasNodeClass(Node,MachineClass))
        {
            N->SetStringField(TEXT("type"),TEXT("state_machine")); auto* G=ChildGraph(Node,TEXT("StateMachineGraph"));
            N->SetStringField(TEXT("name"),G?G->GetName():TEXT("")); N->SetObjectField(TEXT("machine"),ExportMachine(G,Depth+1));
        }
        else if (HasNodeClass(Node,TEXT("PaperZDAnimGraphNode_OverrideSlot")))
        {
            auto* SlotProperty=FindFProperty<FStructProperty>(Node->GetClass(),TEXT("AnimNode"));
            if (!SlotProperty || SlotProperty->Struct!=FPaperZDAnimNode_OverrideSlot::StaticStruct()) return Unsupported(TEXT("Unsupported override slot layout."));
            auto* D=SlotProperty->ContainerPtrToValuePtr<FPaperZDAnimNode_OverrideSlot>(Node); N->SetStringField(TEXT("type"),TEXT("override_slot"));
            N->SetStringField(TEXT("slot"),D->SlotName.ToString()); N->SetStringField(TEXT("group"),D->GroupName.ToString());
            N->SetStringField(TEXT("input"),Id(InputNode(DirectionPin(Node,EGPD_Input))));
        }
        else return Unsupported(TEXT("Unsupported animation node: ")+Node->GetClass()->GetName()); Nodes.Add(Val(N));
    }
    Nodes.Sort([](const JV& A,const JV& B){return A->AsObject()->GetStringField(TEXT("id"))<B->AsObject()->GetStringField(TEXT("id"));});
    O->SetArrayField(TEXT("nodes"),Nodes); return O;
}
J ExportObject(UPaperZDAnimBP* BP)
{
    J D=Obj(); D->SetStringField(TEXT("schema"),TEXT("paperzd-graph/1")); D->SetStringField(TEXT("animation_source"),GetPathNameSafe(BP->GetSupportedAnimationSource()));
    TArray<JV> Vars; JV Owner=MakeShared<FJsonValueNull>();
    for (const auto& V:BP->NewVariables)
    {
        if (V.VarName==OwnerVariable)
        {
            J Spec; FString Text=VariableMeta(V,OwnerDocKey); UEdGraph* G=UpdateGraph(BP);
            Owner=G&&FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Spec)&&OwnerMatchesDocument(BP,Spec)
                ?Val(Spec):Val(Unsupported(TEXT("Owner update graph changed manually; it will not be overwritten."))); continue;
        }
        J N=Obj(); N->SetStringField(TEXT("name"),V.VarName.ToString());
        FString Type=V.VarType.PinCategory==UEdGraphSchema_K2::PC_Boolean?TEXT("bool"):V.VarType.PinCategory==UEdGraphSchema_K2::PC_Real?TEXT("float"):TEXT("unsupported");
        if (V.VarType.IsContainer()) Type=TEXT("unsupported"); N->SetStringField(TEXT("type"),Type); N->SetStringField(TEXT("default"),VariableDefault(BP,V,Type)); Vars.Add(Val(N));
    }
    Vars.Sort([](const JV& A,const JV& B){return A->AsObject()->GetStringField(TEXT("name"))<B->AsObject()->GetStringField(TEXT("name"));});
    D->SetArrayField(TEXT("variables"),Vars); D->SetField(TEXT("owner_update"),Owner); D->SetObjectField(TEXT("graph"),ExportAnimGraph(BP->AnimationGraph)); return D;
}
FString ExportDocument(UPaperZDAnimBP* BP) {return GraphEncode(ExportObject(BP));}
bool ContainsUnsupported(const JV& V)
{
    if (!V) return true;
    if (V->Type==EJson::Object)
    {
        auto O=V->AsObject(); if (O->HasField(TEXT("unsupported"))||(O->HasTypedField<EJson::String>(TEXT("type"))&&O->GetStringField(TEXT("type"))==TEXT("unsupported"))) return true;
        for (const auto& P:O->Values) if (ContainsUnsupported(P.Value)) return true;
    }
    if (V->Type==EJson::Array) for (const auto& E:V->AsArray()) if (ContainsUnsupported(E)) return true; return false;
}
}

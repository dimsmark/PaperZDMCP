namespace PaperZDMCPGraph
{
FString OwnerClassIdentity(UClass* Class)
{
    // Skeleton and reinstanced Blueprint classes represent the same asset.
    return Class && Class->ClassGeneratedBy ? Class->ClassGeneratedBy->GetPathName() : GetPathNameSafe(Class);
}
FString OwnerMemberIdentity(const FMemberReference& Member)
{
    return (Member.IsSelfContext()?TEXT("self"):OwnerClassIdentity(Member.GetMemberParentClass()))+TEXT(":")+Member.GetMemberName().ToString().ToLower();
}
FString OwnerPinName(UEdGraphPin* Pin)
{
    if (auto* CastNode=Cast<UK2Node_DynamicCast>(Pin->GetOwningNode()))
        if (Pin==CastNode->GetCastResultPin()) return TEXT("cast_result");
    return Pin->PinName.ToString().ToLower();
}
bool OwnerPinRelevant(UEdGraphPin* Pin)
{
    if (!Pin->LinkedTo.IsEmpty()) return true;
    // These are the only advanced inputs explicitly written by our recipe.
    if (auto* Call=Cast<UK2Node_CallFunction>(Pin->GetOwningNode()))
        if (Call->FunctionReference.GetMemberName()==TEXT("PrintString") &&
            (Pin->PinName==TEXT("InString")||Pin->PinName==TEXT("bPrintToScreen"))) return true;
    if (Pin->bHidden||Pin->bAdvancedView) return false;
    return Pin->PinName!=UEdGraphSchema_K2::PN_Self || Pin->DefaultObject!=nullptr || !Pin->DefaultValue.IsEmpty();
}
FString OwnerLiteral(UEdGraphPin* Pin)
{
    if (Pin->DefaultObject) return Pin->DefaultObject->GetPathName();
    const FName Type=Pin->PinType.PinCategory;
    if (Type==UEdGraphSchema_K2::PC_Boolean) return NormalizeDefault(TEXT("bool"),Pin->DefaultValue);
    if (Type==UEdGraphSchema_K2::PC_Real||Type==UEdGraphSchema_K2::PC_Int||Type==UEdGraphSchema_K2::PC_Int64)
        return NormalizeDefault(TEXT("float"),Pin->DefaultValue);
    if (Type==UEdGraphSchema_K2::PC_Name) return Pin->DefaultValue.IsEmpty()?TEXT("none"):Pin->DefaultValue.ToLower();
    return Pin->DefaultValue;
}

// Assign identities by following execution and input dependencies, never by
// UObject names, array order or GUIDs. Extra/disconnected nodes fail closed.
struct FOwnerDescription
{
    TMap<UEdGraphNode*,int32> Indices;
    TArray<UEdGraphNode*> Order;
    bool Valid=true;
    FString Operation(UEdGraphNode* Node)
    {
        if (!Node->IsNodeEnabled()) Valid=false;
        FString Op=Node->GetClass()->GetPathName();
        if (auto* Call=Cast<UK2Node_CallFunction>(Node)) return Op+TEXT(":")+OwnerMemberIdentity(Call->FunctionReference);
        if (auto* Get=Cast<UK2Node_VariableGet>(Node)) return Op+TEXT(":")+OwnerMemberIdentity(Get->VariableReference);
        if (auto* Set=Cast<UK2Node_VariableSet>(Node)) return Op+TEXT(":")+OwnerMemberIdentity(Set->VariableReference);
        if (auto* CastNode=Cast<UK2Node_DynamicCast>(Node)) return Op+TEXT(":")+OwnerClassIdentity(CastNode->TargetType);
        if (auto* Entry=Cast<UK2Node_FunctionEntry>(Node))
        {
            if (Entry->GetExtraFlags()&(FUNC_BlueprintPure|FUNC_Const)) Valid=false;
            return Op;
        }
        if (!Cast<UK2Node_IfThenElse>(Node)) Valid=false;
        return Op;
    }
    TArray<UEdGraphPin*> Pins(UEdGraphNode* Node)
    {
        TArray<UEdGraphPin*> Result;
        for (auto* Pin:Node->Pins) if (OwnerPinRelevant(Pin)) Result.Add(Pin);
        Result.Sort([](const UEdGraphPin& A,const UEdGraphPin& B){return OwnerPinName(const_cast<UEdGraphPin*>(&A))<OwnerPinName(const_cast<UEdGraphPin*>(&B));});
        return Result;
    }
    void Visit(UEdGraphNode* Node)
    {
        if (Indices.Contains(Node)||!Valid) return;
        if (Order.Num()>512) { Valid=false; return; }
        Indices.Add(Node,Order.Num()); Order.Add(Node);
        auto List=Pins(Node);
        // Execution order first; pure inputs are then visited by pin name.
        for (auto* P:List) if (P->Direction==EGPD_Output&&P->PinType.PinCategory==UEdGraphSchema_K2::PC_Exec)
        {
            if (P->LinkedTo.Num()>1) { Valid=false; return; }
            for (auto* Other:P->LinkedTo) Visit(Other->GetOwningNode());
        }
        for (auto* P:List) if (P->Direction==EGPD_Input&&P->PinType.PinCategory!=UEdGraphSchema_K2::PC_Exec)
        {
            if (P->LinkedTo.Num()>1) { Valid=false; return; }
            for (auto* Other:P->LinkedTo) Visit(Other->GetOwningNode());
        }
    }
    FString Describe(UEdGraph* Graph)
    {
        UK2Node_FunctionEntry* Entry=nullptr;
        for (UEdGraphNode* N:Graph->Nodes) if (auto* E=Cast<UK2Node_FunctionEntry>(N)) { if (Entry) return {}; Entry=E; }
        if (!Entry) return {};
        Visit(Entry); if (!Valid||Order.Num()!=Graph->Nodes.Num()) return {};
        J Description=Obj(); TArray<JV> Nodes,Edges;
        for (auto* N:Order)
        {
            J Item=Obj(); Item->SetNumberField(TEXT("index"),Indices[N]); Item->SetStringField(TEXT("operation"),Operation(N));
            TArray<JV> Literals;
            for (auto* P:Pins(N))
            {
                if (P->bOrphanedPin) Valid=false;
                if (P->Direction==EGPD_Input&&P->PinType.PinCategory!=UEdGraphSchema_K2::PC_Exec&&P->LinkedTo.IsEmpty())
                {
                    J L=Obj(); L->SetStringField(TEXT("pin"),OwnerPinName(P)); L->SetStringField(TEXT("type"),P->PinType.PinCategory.ToString().ToLower());
                    L->SetStringField(TEXT("value"),OwnerLiteral(P)); Literals.Add(Val(L));
                }
                if (P->Direction==EGPD_Output) for (auto* Other:P->LinkedTo)
                {
                    if (!Indices.Contains(Other->GetOwningNode())||Other->Direction!=EGPD_Input) { Valid=false; continue; }
                    J E=Obj(); E->SetNumberField(TEXT("from"),Indices[N]); E->SetStringField(TEXT("output"),OwnerPinName(P));
                    E->SetNumberField(TEXT("to"),Indices[Other->GetOwningNode()]); E->SetStringField(TEXT("input"),OwnerPinName(Other)); Edges.Add(Val(E));
                }
            }
            Item->SetArrayField(TEXT("literals"),Literals); Nodes.Add(Val(Item));
        }
        Description->SetArrayField(TEXT("nodes"),Nodes); Description->SetArrayField(TEXT("connections"),Edges);
        return Valid?Canonical(Val(Description)):FString();
    }
};

// The expected side is plain data. Inspection creates no Blueprint, graph,
// node, or compiler work, even transiently.
struct FOwnerRecipe
{
    struct FNode { FString Operation; TArray<JV> Literals; };
    struct FEdge { int32 From,To; FString Output,Input; bool Exec; };
    TArray<FNode> Nodes;
    TArray<FEdge> Edges;
    TMap<int32,int32> Indices;
    TArray<int32> Order;
    int32 Add(UClass* Type,const FString& Member=FString())
    { return Nodes.Add({Type->GetPathName()+(Member.IsEmpty()?TEXT(""):TEXT(":")+Member),{}}); }
    void Link(int32 From,const TCHAR* Output,int32 To,const FString& Input,bool Exec=false)
    { Edges.Add({From,To,FString(Output).ToLower(),Input.ToLower(),Exec}); }
    void Literal(int32 Node,const TCHAR* Pin,const TCHAR* Type,const FString& Value)
    {
        J L=Obj(); L->SetStringField(TEXT("pin"),FString(Pin).ToLower()); L->SetStringField(TEXT("type"),Type); L->SetStringField(TEXT("value"),Value); Nodes[Node].Literals.Add(Val(L));
    }
    void Visit(int32 Index)
    {
        if (Indices.Contains(Index)) return;
        Indices.Add(Index,Order.Num()); Order.Add(Index);
        TArray<FEdge> Out,In;
        for (const auto& E:Edges) { if (E.Exec&&E.From==Index) Out.Add(E); if (!E.Exec&&E.To==Index) In.Add(E); }
        Out.Sort([](const auto& A,const auto& B){return A.Output<B.Output;});
        In.Sort([](const auto& A,const auto& B){return A.Input<B.Input;});
        for (const auto& E:Out) Visit(E.To);
        for (const auto& E:In) Visit(E.From);
    }
    FString Describe()
    {
        Visit(0); if (Order.Num()!=Nodes.Num()) return {};
        J D=Obj(); TArray<JV> N,E;
        for (int32 Index:Order)
        {
            J Item=Obj(); Item->SetNumberField(TEXT("index"),Indices[Index]); Item->SetStringField(TEXT("operation"),Nodes[Index].Operation);
            Item->SetArrayField(TEXT("literals"),Nodes[Index].Literals); N.Add(Val(Item));
        }
        for (const auto& Edge:Edges)
        {
            J Item=Obj(); Item->SetNumberField(TEXT("from"),Indices[Edge.From]); Item->SetNumberField(TEXT("to"),Indices[Edge.To]);
            Item->SetStringField(TEXT("output"),Edge.Output); Item->SetStringField(TEXT("input"),Edge.Input); E.Add(Val(Item));
        }
        D->SetArrayField(TEXT("nodes"),N); D->SetArrayField(TEXT("connections"),E); return Canonical(Val(D));
    }
};
bool OwnerMatchesDocument(UPaperZDAnimBP* BP,const J& Spec)
{
    FString Path; const TArray<JV>* Bindings=nullptr;
    if (!Spec||!Spec->TryGetStringField(TEXT("class"),Path)||!Spec->TryGetArrayField(TEXT("bindings"),Bindings)||Bindings->Num()>128) return false;
    UClass* OwnerClass=LoadClass<AActor>(nullptr,*Path);
    if (!OwnerClass||!OwnerClass->IsChildOf(AActor::StaticClass())) return false;
    const auto* Cache=BP->NewVariables.FindByPredicate([](const auto& V){return V.VarName==OwnerVariable;});
    if (!Cache||Cache->VarType.PinSubCategoryObject.Get()!=OwnerClass) return false;
    FOwnerRecipe Init,Update;
    const FString Self=TEXT("self:"),Library=UKismetSystemLibrary::StaticClass()->GetPathName()+TEXT(":");
    const FString Owner=OwnerClassIdentity(OwnerClass)+TEXT(":");
    const FString CacheName=OwnerVariable.ToString().ToLower();
    const int32 Entry=Init.Add(UK2Node_FunctionEntry::StaticClass());
    const int32 GetActor=Init.Add(UK2Node_CallFunction::StaticClass(),Self+TEXT("getowningactor"));
    const int32 CastNode=Init.Add(UK2Node_DynamicCast::StaticClass(),OwnerClassIdentity(OwnerClass));
    const int32 SetCache=Init.Add(UK2Node_VariableSet::StaticClass(),Self+CacheName);
    const int32 Print=Init.Add(UK2Node_CallFunction::StaticClass(),Library+TEXT("printstring"));
    Init.Link(Entry,TEXT("then"),CastNode,TEXT("execute"),true);
    Init.Link(GetActor,TEXT("returnvalue"),CastNode,TEXT("object"));
    Init.Link(CastNode,TEXT("then"),SetCache,TEXT("execute"),true);
    Init.Link(CastNode,TEXT("cast_result"),SetCache,CacheName);
    Init.Link(CastNode,TEXT("castfailed"),Print,TEXT("execute"),true);
    Init.Literal(Print,TEXT("instring"),TEXT("string"),TEXT("PaperZDMCP: owning actor is not ")+Path+TEXT(". Owner-variable updates disabled."));
    Init.Literal(Print,TEXT("bprinttoscreen"),TEXT("bool"),TEXT("false"));
    const int32 UpdateEntry=Update.Add(UK2Node_FunctionEntry::StaticClass());
    const int32 GetCache=Update.Add(UK2Node_VariableGet::StaticClass(),Self+CacheName);
    const int32 Valid=Update.Add(UK2Node_CallFunction::StaticClass(),Library+TEXT("isvalid"));
    const int32 Branch=Update.Add(UK2Node_IfThenElse::StaticClass());
    Update.Link(UpdateEntry,TEXT("then"),Branch,TEXT("execute"),true);
    Update.Link(GetCache,*CacheName,Valid,TEXT("object"));
    Update.Link(Valid,TEXT("returnvalue"),Branch,TEXT("condition"));
    int32 Previous=Branch; TSet<FName> Seen;
    for (const JV& Binding:*Bindings)
    {
        FString Variable,Property;
        if (Binding->Type!=EJson::Object||!Binding->AsObject()->TryGetStringField(TEXT("variable"),Variable)||!Binding->AsObject()->TryGetStringField(TEXT("property"),Property)) return false;
        FEdGraphPinType Source,Target; FProperty* P=FindFProperty<FProperty>(OwnerClass,*Property);
        if (!P||!P->HasAnyPropertyFlags(CPF_BlueprintVisible)||Seen.Contains(FName(*Variable))||FName(*Variable)==OwnerVariable||!VariableType(BP,Variable,Target)||
            !GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(P,Source)||Source.IsContainer()||Target.IsContainer()||Source.PinCategory!=Target.PinCategory||
            (Target.PinCategory!=UEdGraphSchema_K2::PC_Boolean&&Target.PinCategory!=UEdGraphSchema_K2::PC_Real)) return false;
        Seen.Add(FName(*Variable));
        const int32 Get=Update.Add(UK2Node_VariableGet::StaticClass(),Owner+Property.ToLower());
        const int32 Set=Update.Add(UK2Node_VariableSet::StaticClass(),Self+Variable.ToLower());
        Update.Link(GetCache,*CacheName,Get,TEXT("self"));
        Update.Link(Get,*Property,Set,Variable);
        Update.Link(Previous,TEXT("then"),Set,TEXT("execute"),true); Previous=Set;
    }
    for (FName Name:{InitGraphName,UpdateGraphName})
    {
        UEdGraph* Actual=nullptr;
        for (UEdGraph* G:BP->FunctionGraphs) if (G->GetFName()==Name) { if (Actual) return false; Actual=G; }
        if (!Actual) return false;
        FString A=FOwnerDescription().Describe(Actual),B=Name==InitGraphName?Init.Describe():Update.Describe();
        if (A.IsEmpty()||B.IsEmpty()||FMD5::HashAnsiString(*A)!=FMD5::HashAnsiString(*B)) return false;
    }
    for (FName Event:{FName(TEXT("OnInit")),FName(TEXT("OnTick"))})
    {
        const FName Function=Event==TEXT("OnInit")?InitGraphName:UpdateGraphName; int32 Hooks=0,Events=0;
        for (UEdGraph* G:BP->UbergraphPages) for (UEdGraphNode* N:G->Nodes)
        {
            if (auto* E=Cast<UK2Node_Event>(N)) if (E->EventReference.GetMemberName()==Event) ++Events;
            if (auto* C=Cast<UK2Node_CallFunction>(N)) if (C->FunctionReference.GetMemberName()==Function)
            {
                ++Hooks; auto* E=Cast<UK2Node_Event>(InputNode(C->FindPin(UEdGraphSchema_K2::PN_Execute)));
                auto* SelfPin=C->FindPin(UEdGraphSchema_K2::PN_Self);
                if (!C->IsNodeEnabled()||!C->FunctionReference.IsSelfContext()||!E||!E->IsNodeEnabled()||E->EventReference.GetMemberName()!=Event||
                    (SelfPin&&(!SelfPin->LinkedTo.IsEmpty()||SelfPin->DefaultObject))) return false;
            }
        }
        if (Hooks!=1||Events!=1) return false;
    }
    return true;
}
}

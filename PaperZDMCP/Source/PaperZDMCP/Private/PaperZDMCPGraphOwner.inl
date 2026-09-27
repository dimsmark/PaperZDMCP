namespace PaperZDMCPGraph
{
// Canonical JSON equality is independent of object member and document list order.
// IDs and layout remain significant for idempotence; semantic test equality is separate.
FString Canonical(const JV& V)
{
    if (V->Type==EJson::Object)
    {
        TArray<FString> Keys; for (const auto& P:V->AsObject()->Values) Keys.Add(FString(*P.Key)); Keys.Sort(); FString S=TEXT("{");
        for (const auto& K:Keys) { J Key=Obj(); Key->SetStringField(TEXT("k"),K); S+=GraphEncode(Key)+TEXT(":")+Canonical(V->AsObject()->Values[*K])+TEXT(","); } return S+TEXT("}");
    }
    if (V->Type==EJson::Array)
    {
        TArray<FString> Items; for (const JV& E:V->AsArray()) Items.Add(Canonical(E)); Items.Sort();
        return TEXT("[")+FString::Join(Items,TEXT(","))+TEXT("]");
    }
    J O=Obj(); O->SetField(TEXT("v"),V); return GraphEncode(O);
}
bool Equal(const J& A,const J& B) { return Canonical(Val(A))==Canonical(Val(B)); }

struct FOwnerBuilder
{
    FDocumentBuilder& D;
    UPaperZDAnimBP* BP;
    UEdGraph* Graph=nullptr;
    FString Tag=TEXT("PaperZDMCP owner hook");
    explicit FOwnerBuilder(FDocumentBuilder& In):D(In),BP(In.BP){}
    UK2Node_CallFunction* Call(UFunction* F)
    {
        if (!F) { D.Bad(TEXT("Required owner function unavailable.")); return nullptr; }
        FGraphNodeCreator<UK2Node_CallFunction> C(*Graph); auto* N=C.CreateNode(false); N->SetFromFunction(F); C.Finalize(); return N;
    }
    UEdGraphPin* Get(FName Name,UClass* Class=nullptr,UEdGraphPin* Owner=nullptr)
    {
        FGraphNodeCreator<UK2Node_VariableGet> C(*Graph); auto* N=C.CreateNode(false);
        if (Class) N->VariableReference.SetExternalMember(Name,Class); else N->VariableReference.SetSelfMember(Name);
        C.Finalize(); if (Owner&&!D.Wire(Graph,Owner,N->FindPin(UEdGraphSchema_K2::PN_Self))) return nullptr; return N->GetValuePin();
    }
    UEdGraphPin* Set(FName Name,UEdGraphPin* Exec,UEdGraphPin* Value)
    {
        FGraphNodeCreator<UK2Node_VariableSet> C(*Graph); auto* N=C.CreateNode(false); N->VariableReference.SetSelfMember(Name); C.Finalize();
        if (!D.Wire(Graph,Exec,N->FindPin(UEdGraphSchema_K2::PN_Execute))||!D.Wire(Graph,Value,N->FindPin(Name))) return nullptr;
        return N->FindPin(UEdGraphSchema_K2::PN_Then);
    }
    UEdGraphPin* Function(FName Name)
    {
        for (UEdGraph* G:BP->FunctionGraphs) if (G->GetFName()==Name) { Graph=G; break; }
        if (!Graph)
        {
            Graph=FBlueprintEditorUtils::CreateNewGraph(BP,Name,UEdGraph::StaticClass(),UEdGraphSchema_K2::StaticClass());
            FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP,Graph,false,nullptr);
        }
        SnapshotGraph(Graph); UK2Node_FunctionEntry* Entry=nullptr; TArray<UEdGraphNode*> Old;
        for (UEdGraphNode* N:Graph->Nodes) { if (auto* E=Cast<UK2Node_FunctionEntry>(N)) Entry=E; else Old.Add(N); }
        for (auto* N:Old) N->DestroyNode();
        return Entry?Entry->FindPin(UEdGraphSchema_K2::PN_Then):nullptr;
    }
    bool Hook(FName Event,FName Function)
    {
        UK2Node_Event* Existing=nullptr; UK2Node_CallFunction* HookNode=nullptr; Graph=nullptr;
        for (UEdGraph* G:BP->UbergraphPages) for (UEdGraphNode* N:G->Nodes)
        {
            if (auto* E=Cast<UK2Node_Event>(N)) if (E->EventReference.GetMemberName()==Event) { if (Existing) return D.Bad(TEXT("Duplicate owner lifecycle event.")); Existing=E; }
            if (auto* C=Cast<UK2Node_CallFunction>(N)) if (C->FunctionReference.GetMemberName()==Function&&C->FunctionReference.IsSelfContext()) HookNode=C;
        }
        if (HookNode)
        {
            // Never silently move a hook which the user has rewired.
            if (!Existing||InputNode(HookNode->FindPin(UEdGraphSchema_K2::PN_Execute))!=Existing) return D.Bad(TEXT("Managed lifecycle hook was edited; restore it before applying."));
            return true;
        }
        if (Existing) Graph=Existing->GetGraph();
        else
        {
            for (UEdGraph* G:BP->UbergraphPages) if (G!=BP->AnimationGraph && G->GetSchema()->IsA<UEdGraphSchema_K2>()) { Graph=G; break; }
            if (!Graph)
            {
                Graph=FBlueprintEditorUtils::CreateNewGraph(BP,TEXT("EventGraph"),UEdGraph::StaticClass(),UEdGraphSchema_K2::StaticClass());
                FBlueprintEditorUtils::AddUbergraphPage(BP,Graph);
            }
            FGraphNodeCreator<UK2Node_Event> C(*Graph); Existing=C.CreateNode(false);
            Existing->EventReference.SetExternalMember(Event,UPaperZDAnimInstance::StaticClass()); Existing->bOverrideFunction=true; C.Finalize();
        }
        SnapshotGraph(Graph); auto* Then=Existing->FindPin(UEdGraphSchema_K2::PN_Then); if (!Then) return D.Bad(TEXT("Lifecycle event has no execution output."));
        const auto Old=Then->LinkedTo; Then->BreakAllPinLinks(false);
        FGraphNodeCreator<UK2Node_CallFunction> C(*Graph); auto* N=C.CreateNode(false); N->FunctionReference.SetSelfMember(Function); C.Finalize(); N->NodeComment=Tag;
        if (!D.Wire(Graph,Then,N->FindPin(UEdGraphSchema_K2::PN_Execute))) return false;
        for (auto* P:Old) if (!D.Wire(Graph,N->FindPin(UEdGraphSchema_K2::PN_Then),P)) return false;
        return true;
    }
    bool Build(const JV& Value)
    {
        D.Location=TEXT("$.owner_update");
        bool HasOwner=false; for (const auto& V:BP->NewVariables) if (V.VarName==OwnerVariable) HasOwner=true;
        if (Value->Type==EJson::Null) return !HasOwner||D.Bad(TEXT("Removing managed owner hooks is unsupported; retain owner_update."));
        if (Value->Type!=EJson::Object) return D.Bad(TEXT("Expected null or owner update object."));
        J O=Value->AsObject(); if (!D.Keys(O,{TEXT("class"),TEXT("bindings")})) return false;
        FString Path; const TArray<JV>* Bindings;
        if (!D.Text(O,TEXT("class"),Path)||!D.Array(O,TEXT("bindings"),Bindings)||Bindings->Num()>128) return D.Bad(TEXT("Invalid owner specification."));
        UClass* OwnerClass=LoadClass<AActor>(nullptr,*Path);
        if (!OwnerClass||!OwnerClass->IsChildOf(AActor::StaticClass())) return D.Bad(TEXT("Owner cast target class does not exist or is not an Actor class."));
        if (HasOwner)
        {
            for (auto& V:BP->NewVariables) if (V.VarName==OwnerVariable&&V.VarType.PinSubCategoryObject.Get()!=OwnerClass) return D.Bad(TEXT("Changing the cached owner class is unsupported; use a fresh AnimBP."));
        }
        else
        {
            for (UEdGraph* G:BP->FunctionGraphs) if (G->GetFName()==InitGraphName||G->GetFName()==UpdateGraphName) return D.Bad(TEXT("Managed function name already belongs to a user function."));
            FEdGraphPinType T; T.PinCategory=UEdGraphSchema_K2::PC_Object; T.PinSubCategoryObject=OwnerClass;
            if (!FBlueprintEditorUtils::AddMemberVariable(BP,OwnerVariable,T)) return D.Bad(TEXT("Cannot add owner cache variable."));
        }
        TSet<FName> Seen;
        for (const JV& V:*Bindings)
        {
            if (V->Type!=EJson::Object) return D.Bad(TEXT("Bindings must be objects.")); J B=V->AsObject();
            if (!D.Keys(B,{TEXT("variable"),TEXT("property")})) return false;
            FString Var,Property; if (!D.Text(B,TEXT("variable"),Var)||!D.Text(B,TEXT("property"),Property)) return false;
            FEdGraphPinType Target,Source; FProperty* P=FindFProperty<FProperty>(OwnerClass,*Property);
            if (Seen.Contains(FName(*Var))||FName(*Var)==OwnerVariable||!VariableType(BP,Var,Target)) return D.Bad(TEXT("Binding target is missing, reserved, or duplicated.")); Seen.Add(FName(*Var));
            if (!P||!P->HasAnyPropertyFlags(CPF_BlueprintVisible)||!GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(P,Source)||Source.IsContainer()||Target.IsContainer()||
                Source.PinCategory!=Target.PinCategory||(Source.PinCategory!=UEdGraphSchema_K2::PC_Real&&Source.PinCategory!=UEdGraphSchema_K2::PC_Boolean)) return D.Bad(TEXT("Owner property must exist, be Blueprint-visible, and match a bool or real target variable."));
        }
        // OnInit performs one cast and caches the owner. Failed cast reports once.
        Graph=nullptr; auto* Start=Function(InitGraphName); if (!Start) return D.Bad(TEXT("Could not create owner initializer."));
        auto* Owner=Call(UPaperZDAnimInstance::StaticClass()->FindFunctionByName(TEXT("GetOwningActor"))); if (!Owner) return false;
        FGraphNodeCreator<UK2Node_DynamicCast> CastCreator(*Graph); auto* CastNode=CastCreator.CreateNode(false); CastNode->TargetType=OwnerClass; CastNode->SetPurity(false); CastCreator.Finalize();
        if (!D.Wire(Graph,Start,CastNode->GetExecPin())||!D.Wire(Graph,Owner->GetReturnValuePin(),CastNode->GetCastSourcePin())||!Set(OwnerVariable,CastNode->GetValidCastPin(),CastNode->GetCastResultPin())) return false;
        auto* Print=Call(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString"))); if (!Print) return false;
        Graph->GetSchema()->TrySetDefaultValue(*Print->FindPinChecked(TEXT("InString")),TEXT("PaperZDMCP: owning actor is not ")+Path+TEXT(". Owner-variable updates disabled."));
        Graph->GetSchema()->TrySetDefaultValue(*Print->FindPinChecked(TEXT("bPrintToScreen")),TEXT("false"));
        if (!D.Wire(Graph,CastNode->GetInvalidCastPin(),Print->FindPin(UEdGraphSchema_K2::PN_Execute))) return false;
        Graph=nullptr; Start=Function(UpdateGraphName); if (!Start) return D.Bad(TEXT("Could not create update function."));
        auto* Cache=Get(OwnerVariable); auto* Valid=Call(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("IsValid"))); if (!Cache||!Valid) return false;
        if (!D.Wire(Graph,Cache,Valid->FindPin(TEXT("Object")))) return false;
        FGraphNodeCreator<UK2Node_IfThenElse> BranchCreator(*Graph); auto* Branch=BranchCreator.CreateNode(false); BranchCreator.Finalize();
        if (!D.Wire(Graph,Start,Branch->GetExecPin())||!D.Wire(Graph,Valid->GetReturnValuePin(),Branch->GetConditionPin())) return false;
        auto* Exec=Branch->GetThenPin();
        for (const JV& V:*Bindings)
        {
            J B=V->AsObject(); auto* P=Get(*B->GetStringField(TEXT("property")),OwnerClass,Cache);
            if (!P) return false; Exec=Set(*B->GetStringField(TEXT("variable")),Exec,P); if (!Exec) return false;
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        if (!Hook(TEXT("OnInit"),InitGraphName)||!Hook(TEXT("OnTick"),UpdateGraphName)) return false;
        for (auto& V:BP->NewVariables) if (V.VarName==OwnerVariable) SetVariableMeta(V,OwnerDocKey,GraphEncode(O));
        return true;
    }
};

bool Variables(FDocumentBuilder& D,const J& O)
{
    D.Location=TEXT("$.variables"); const TArray<JV>* Vars; if (!D.Array(O,TEXT("variables"),Vars)||Vars->Num()>128) return D.Bad(TEXT("Invalid variables list."));
    TSet<FName> Seen;
    for (const JV& V:*Vars)
    {
        if (V->Type!=EJson::Object) return D.Bad(TEXT("Expected variable object.")); J N=V->AsObject();
        FString Name,Kind,Default;
        if (!D.Keys(N,{TEXT("name"),TEXT("type"),TEXT("default")})||!D.Text(N,TEXT("name"),Name)||!D.Text(N,TEXT("type"),Kind)||!D.Text(N,TEXT("default"),Default)) return false;
        if (!SimpleName(Name)||FName(*Name)==OwnerVariable||Seen.Contains(FName(*Name))||(Kind!=TEXT("bool")&&Kind!=TEXT("float"))) return D.Bad(TEXT("Invalid or duplicate variable name/type.")); Seen.Add(FName(*Name));
        double Number;
        if (Kind==TEXT("bool")&&Default!=TEXT("")&&Default!=TEXT("true")&&Default!=TEXT("false")) return D.Bad(TEXT("Bool default must be true, false, or empty."));
        if (Kind==TEXT("float")&&!Default.IsEmpty()&&(!LexTryParseString(Number,*Default)||!FMath::IsFinite(Number))) return D.Bad(TEXT("Float default must be finite."));
        Default=NormalizeDefault(Kind,Default);
        D.Defaults.Add(FName(*Name),Default);
        FEdGraphPinType T; T.PinCategory=Kind==TEXT("bool")?UEdGraphSchema_K2::PC_Boolean:UEdGraphSchema_K2::PC_Real; if (Kind==TEXT("float")) T.PinSubCategory=UEdGraphSchema_K2::PC_Double;
        FBPVariableDescription* Existing=D.BP->NewVariables.FindByPredicate([&](const auto& E){return E.VarName==FName(*Name);});
        if (Existing)
        {
            if (Existing->VarType.IsContainer()||Existing->VarType.PinCategory!=T.PinCategory) return D.Bad(TEXT("Existing variable type cannot change.")); Existing->DefaultValue=Default;
        }
        else if (!FBlueprintEditorUtils::AddMemberVariable(D.BP,*Name,T,Default)) return D.Bad(TEXT("Cannot create variable; name may collide with an inherited member."));
    }
    for (const auto& V:D.BP->NewVariables) if (V.VarName!=OwnerVariable&&!Seen.Contains(V.VarName)) return D.Bad(TEXT("Omitting existing variables is unsupported because user graphs may reference them."));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(D.BP); return true;
}
}

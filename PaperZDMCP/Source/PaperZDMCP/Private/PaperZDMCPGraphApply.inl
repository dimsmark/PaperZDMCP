namespace PaperZDMCPGraph
{
bool Equal(const J& A,const J& B);
struct FDocumentBuilder
{
    UPaperZDAnimBP* BP;
    FString Error, Location=TEXT("$");
    TSet<FGuid> Ids;
    TMap<FGuid,FString> Paths;
    TMap<FString,J> CustomRules;
    TMap<FName,FString> Defaults;
    TSet<FString> KeptCustom;
    TSet<FName> Slots;
    int32 Count=0;
    struct FPending { UEdGraphNode* Owner; UEdGraphNode* From; J Rule; FString Path; };
    TArray<FPending> Pending;
    bool Bad(const FString& Message) { Error=Location+TEXT(": ")+Message; return false; }
    bool Keys(const J& O,std::initializer_list<const TCHAR*> Names)
    {
        if (!O) return Bad(TEXT("Expected an object."));
        TSet<FString> Expected; for (auto N:Names) Expected.Add(N);
        for (const auto& P:O->Values) if (!Expected.Contains(FString(*P.Key))) return Bad(FString(TEXT("Unknown field: "))+*P.Key);
        for (auto N:Names) if (!O->HasField(N)) return Bad(FString(TEXT("Missing field: "))+N); return true;
    }
    bool Text(const J& O,const TCHAR* K,FString& V) { return O->TryGetStringField(K,V)||Bad(FString(K)+TEXT(" must be a string.")); }
    bool Number(const J& O,const TCHAR* K,double& V) { return (O->TryGetNumberField(K,V)&&FMath::IsFinite(V))||Bad(FString(K)+TEXT(" must be finite.")); }
    bool Boolean(const J& O,const TCHAR* K,bool& V) { return O->TryGetBoolField(K,V)||Bad(FString(K)+TEXT(" must be boolean.")); }
    J Object(const J& O,const TCHAR* K) { const J* P; if (!O || !O->TryGetObjectField(K,P)) { Bad(FString(K)+TEXT(" must be an object.")); return nullptr; } return *P; }
    bool Array(const J& O,const TCHAR* K,const TArray<JV>*& V) { return O->TryGetArrayField(K,V)||Bad(FString(K)+TEXT(" must be an array.")); }
    bool SetIdentity(UEdGraphNode* N,const J& O)
    {
        FString S; FGuid G; double X,Y;
        if (!Text(O,TEXT("id"),S)||!FGuid::Parse(S,G)||!G.IsValid()||Ids.Contains(G)) return Bad(TEXT("IDs must be unique nonzero UUIDs."));
        if (!Number(O,TEXT("x"),X)||!Number(O,TEXT("y"),Y)||FMath::Abs(X)>1000000||FMath::Abs(Y)>1000000||X!=FMath::TruncToDouble(X)||Y!=FMath::TruncToDouble(Y)) return Bad(TEXT("Layout coordinates must be bounded integers."));
        if (++Count>512) return Bad(TEXT("Document exceeds 512 nodes."));
        N->Modify(); Ids.Add(G); Paths.Add(G,Location); N->NodeGuid=G; N->NodePosX=int32(X); N->NodePosY=int32(Y); return true;
    }
    bool Wire(UEdGraph* G,UEdGraphPin* A,UEdGraphPin* B)
    { return (A&&B&&G->GetSchema()->TryCreateConnection(A,B))||Bad(TEXT("Graph schema rejected a connection.")); }
    UEdGraphNode* Node(UEdGraph* G,const J& O,const TCHAR* Class)
    {
        FString S; if (!Text(O,TEXT("id"),S)) return nullptr;
        for (UEdGraphNode* N:G->Nodes) if (Id(N)==S)
        {
            if (!HasNodeClass(N,Class)) { Bad(TEXT("Existing ID has a different node type.")); return nullptr; }
            N->Modify(); return N;
        }
        return SpawnGraphNode(G,NodeClass(Class));
    }
    void UnlinkGraph(UEdGraph* G)
    {
        SnapshotGraph(G);
        for (UEdGraphNode* N:G->Nodes) for (UEdGraphPin* P:N->Pins) P->BreakAllPinLinks(false);
    }
    bool RemoveUnlisted(UEdGraph* G,const TSet<UEdGraphNode*>& Keep)
    {
        TArray<UEdGraphNode*> Old; for (UEdGraphNode* N:G->Nodes) if (!Keep.Contains(N)) Old.Add(N);
        for (auto* N:Old)
        {
            if (CustomRules.Contains(Id(N))) return Bad(TEXT("Cannot remove an existing custom rule; keep its opaque stub."));
            N->DestroyNode();
        }
        return true;
    }
    bool Rename(UEdGraphNode* N,const FString& Name)
    {
        FText Reason;
        if (Name.IsEmpty()||Name.Len()>100||FName(*Name).IsNone()||!FName(*Name).IsValidObjectName(Reason)) return Bad(TEXT("Invalid graph node name: ")+Reason.ToString());
        // Names are scoped to each graph. IDs retain references through a rename.
        if (N->GetNodeTitle(ENodeTitleType::ListView).ToString()!=Name) N->OnRenameNode(Name);
        return true;
    }
    bool Anim(UEdGraph* G,const J& O,int32 Depth=0,bool Transitional=false)
    {
        if (!G||Depth>16) return Bad(TEXT("Missing graph or nesting exceeds 16."));
        if (!Keys(O,{TEXT("root"),TEXT("output"),TEXT("nodes")})) return false;
        J RootSpec=Object(O,TEXT("root")); auto* Root=FirstNode(G,SinkClass);
        if (!Root||!Keys(RootSpec,{TEXT("id"),TEXT("x"),TEXT("y")})||!SetIdentity(Root,RootSpec)) return false;
        const TArray<JV>* Nodes; FString Output;
        if (!Array(O,TEXT("nodes"),Nodes)||!Text(O,TEXT("output"),Output)) return false;
        UnlinkGraph(G); TSet<UEdGraphNode*> Keep{Root}; TMap<FString,UEdGraphNode*> Map;
        TArray<TPair<UEdGraphNode*,FString>> Links; TSet<FName> MachineNames;
        const FString Base=Location;
        for (int32 I=0;I<Nodes->Num();++I)
        {
            Location=Base+FString::Printf(TEXT(".nodes[%d]"),I); if ((*Nodes)[I]->Type!=EJson::Object) return Bad(TEXT("Expected a node object."));
            J N=(*Nodes)[I]->AsObject(); FString Type; if (!Text(N,TEXT("type"),Type)) return false;
            const TCHAR* Class=Type==TEXT("play_sequence")?PlayClass:Type==TEXT("state_machine")?MachineClass:Type==TEXT("override_slot")?TEXT("PaperZDAnimGraphNode_OverrideSlot"):nullptr;
            if (!Class||(Transitional&&Type==TEXT("state_machine"))) return Bad(TEXT("Unsupported animation node for this graph."));
            auto* New=Node(G,N,Class); if (!New||!SetIdentity(New,N)) return false; Keep.Add(New); Map.Add(N->GetStringField(TEXT("id")),New);
            if (Type==TEXT("play_sequence"))
            {
                if (!Keys(N,{TEXT("id"),TEXT("x"),TEXT("y"),TEXT("type"),TEXT("sequence"),TEXT("loop"),TEXT("play_rate"),TEXT("start_position")})) return false;
                FString Path; bool Loop; double Rate,Start;
                if (!Text(N,TEXT("sequence"),Path)||!Boolean(N,TEXT("loop"),Loop)||!Number(N,TEXT("play_rate"),Rate)||!Number(N,TEXT("start_position"),Start)) return false;
                auto* Seq=StateSequence(BP,Path); auto* P=Playback(New);
                if (!Seq||!P||Start<0||Start>Seq->GetTotalDuration()||FMath::Abs(Rate)>10000) return Bad(TEXT("Invalid sequence/source, start position, or play rate."));
                P->AnimSequence=Seq; P->bLoopAnimation=Loop; P->PlayRate=Rate; P->StartPosition=Start; New->ReconstructNode();
                for (UEdGraphPin* Pin:New->Pins) if (Pin->Direction==EGPD_Input)
                {
                    if (Pin->PinName==TEXT("PlayRate")) G->GetSchema()->TrySetDefaultValue(*Pin,FString::Printf(TEXT("%.9g"),float(Rate)));
                    else if (Pin->PinName==TEXT("StartPosition")) G->GetSchema()->TrySetDefaultValue(*Pin,FString::Printf(TEXT("%.9g"),float(Start)));
                    else if (Pin->PinName==TEXT("bLoopAnimation")) G->GetSchema()->TrySetDefaultValue(*Pin,Loop?TEXT("true"):TEXT("false"));
                    else if (Pin->PinName==TEXT("AnimSequence")) G->GetSchema()->TrySetDefaultObject(*Pin,Seq);
                    else return Bad(TEXT("Unsupported exposed Play Sequence pin."));
                }
            }
            else if (Type==TEXT("state_machine"))
            {
                if (!Keys(N,{TEXT("id"),TEXT("x"),TEXT("y"),TEXT("type"),TEXT("name"),TEXT("machine")})) return false;
                FString Name; if (!Text(N,TEXT("name"),Name)||MachineNames.Contains(FName(*Name))||!Rename(New,Name)) return Bad(TEXT("Machine names must be unique in their animation graph."));
                MachineNames.Add(FName(*Name)); Location+=TEXT(".machine");
                if (!Machine(ChildGraph(New,TEXT("StateMachineGraph")),Object(N,TEXT("machine")),Depth+1)) return false;
            }
            else
            {
                if (!Keys(N,{TEXT("id"),TEXT("x"),TEXT("y"),TEXT("type"),TEXT("slot"),TEXT("group"),TEXT("input")})) return false;
                FString Slot,Group,Input;
                if (!Text(N,TEXT("slot"),Slot)||!Text(N,TEXT("group"),Group)||!Text(N,TEXT("input"),Input)||!SimpleName(Slot)||!SimpleName(Group)||Slots.Contains(FName(*Slot))) return Bad(TEXT("Invalid/duplicate slot or group."));
                Slots.Add(FName(*Slot)); auto* P=FindFProperty<FStructProperty>(New->GetClass(),TEXT("AnimNode"));
                auto* D=P->ContainerPtrToValuePtr<FPaperZDAnimNode_OverrideSlot>(New); D->SlotName=FName(*Slot); D->GroupName=FName(*Group); Links.Add({New,Input});
            }
        }
        Location=Base; TMap<UEdGraphNode*,UEdGraphNode*> Dependencies;
        for (const auto& L:Links) { auto* N=Map.Find(L.Value); if (!N) return Bad(TEXT("Override input must refer to this graph.")); Dependencies.Add(L.Key,*N); }
        for (const auto& L:Links)
        {
            TSet<UEdGraphNode*> Seen; auto* N=L.Key;
            while (N) { if (Seen.Contains(N)) return Bad(TEXT("Animation links contain a cycle.")); Seen.Add(N); auto* Next=Dependencies.Find(N); N=Next?*Next:nullptr; }
            if (!Wire(G,DirectionPin(Map[L.Value],EGPD_Output),DirectionPin(L.Key,EGPD_Input))) return false;
        }
        if (!Output.IsEmpty()) { auto* N=Map.Find(Output); if (!N) return Bad(TEXT("output must refer to this graph.")); if (!Wire(G,DirectionPin(*N,EGPD_Output),DirectionPin(Root,EGPD_Input))) return false; }
        return RemoveUnlisted(G,Keep);
    }
    bool Machine(UEdGraph* G,const J& O,int32 Depth)
    {
        if (!G||!Keys(O,{TEXT("entry"),TEXT("entry_state"),TEXT("nodes"),TEXT("transitions")})) return false;
        auto* Entry=FirstNode(G,TEXT("PaperZDStateGraphNode_Root")); J E=Object(O,TEXT("entry"));
        if (!Entry||!Keys(E,{TEXT("id"),TEXT("x"),TEXT("y")})||!SetIdentity(Entry,E)) return false;
        const TArray<JV>* Nodes; const TArray<JV>* Transitions; FString Initial;
        if (!Array(O,TEXT("nodes"),Nodes)||!Array(O,TEXT("transitions"),Transitions)||!Text(O,TEXT("entry_state"),Initial)) return false;
        UnlinkGraph(G); TSet<UEdGraphNode*> Keep{Entry}; TMap<FString,UEdGraphNode*> Map; TSet<FName> Names;
        TArray<TPair<UEdGraphNode*,FString>> Jumps; const FString Base=Location;
        for (int32 I=0;I<Nodes->Num();++I)
        {
            Location=Base+FString::Printf(TEXT(".nodes[%d]"),I); if ((*Nodes)[I]->Type!=EJson::Object) return Bad(TEXT("Expected a machine node."));
            J N=(*Nodes)[I]->AsObject(); FString Type,Name;
            if (!Text(N,TEXT("type"),Type)||!Text(N,TEXT("name"),Name)||Names.Contains(FName(*Name))) return Bad(TEXT("Names must be unique within each machine.")); Names.Add(FName(*Name));
            const TCHAR* Class=Type==TEXT("state")?StateClass:Type==TEXT("conduit")?TEXT("PaperZDStateGraphNode_Conduit"):Type==TEXT("jump")?TEXT("PaperZDStateGraphNode_Jump"):nullptr;
            if (!Class) return Bad(TEXT("Unsupported machine node type.")); auto* New=Node(G,N,Class);
            if (!New||!SetIdentity(New,N)||!Rename(New,Name)) return false; Keep.Add(New); Map.Add(N->GetStringField(TEXT("id")),New);
            if (Type==TEXT("state"))
            {
                if (!Keys(N,{TEXT("id"),TEXT("x"),TEXT("y"),TEXT("type"),TEXT("name"),TEXT("graph"),TEXT("OnStateEnterEventName"),TEXT("OnStateExitEventName")})) return false;
                for (const TCHAR* F:{TEXT("OnStateEnterEventName"),TEXT("OnStateExitEventName")})
                {
                    FString Callback; if (!Text(N,F,Callback)||Callback.Len()>=NAME_SIZE) return Bad(TEXT("Invalid state callback."));
                    if (!FName(*Callback).IsNone())
                    {
                        auto* Fn=BP->SkeletonGeneratedClass?BP->SkeletonGeneratedClass->FindFunctionByName(*Callback):nullptr;
                        if (!Fn||Fn->NumParms!=0) return Bad(TEXT("State callback must name an existing no-argument AnimInstance function/event."));
                    }
                    FindFProperty<FNameProperty>(New->GetClass(),F)->SetPropertyValue_InContainer(New,FName(*Callback));
                }
                Location+=TEXT(".graph"); if (!Anim(ChildGraph(New,TEXT("BoundGraph")),Object(N,TEXT("graph")),Depth+1)) return false;
            }
            else if (Type==TEXT("conduit"))
            {
                if (!Keys(N,{TEXT("id"),TEXT("x"),TEXT("y"),TEXT("type"),TEXT("name"),TEXT("rule")})) return false;
                J Rule=Object(N,TEXT("rule")); if (!Rule) return false; Pending.Add({New,nullptr,Rule,Location+TEXT(".rule")});
            }
            else
            { if (!Keys(N,{TEXT("id"),TEXT("x"),TEXT("y"),TEXT("type"),TEXT("name"),TEXT("target")})) return false; FString Target; if (!Text(N,TEXT("target"),Target)) return false; Jumps.Add({New,Target}); }
        }
        Location=Base;
        if (!Initial.IsEmpty())
        { auto* T=Map.Find(Initial); if (!T||!HasNodeClass(*T,StateClass)) return Bad(TEXT("entry_state must identify a state in this machine.")); if (!Wire(G,DirectionPin(Entry,EGPD_Output),DirectionPin(*T,EGPD_Input))) return false; }
        for (const auto& Jump:Jumps)
        { auto* T=Map.Find(Jump.Value); if (!T||!HasNodeClass(*T,StateClass)) return Bad(TEXT("Jump target must be a state in this machine.")); if (!Wire(G,DirectionPin(Jump.Key,EGPD_Output),DirectionPin(*T,EGPD_Input))) return false; }
        TSet<FString> TransitionKeys;
        for (int32 I=0;I<Transitions->Num();++I)
        {
            Location=Base+FString::Printf(TEXT(".transitions[%d]"),I); if ((*Transitions)[I]->Type!=EJson::Object) return Bad(TEXT("Expected a transition object."));
            J T=(*Transitions)[I]->AsObject();
            if (!Keys(T,{TEXT("id"),TEXT("x"),TEXT("y"),TEXT("from"),TEXT("to"),TEXT("priority"),TEXT("blend"),TEXT("rule"),TEXT("transitional_enabled"),TEXT("transitional_animation"),TEXT("color")})) return false;
            FString From,To,Color; double Priority; bool Enabled;
            if (!Text(T,TEXT("from"),From)||!Text(T,TEXT("to"),To)||!Number(T,TEXT("priority"),Priority)||!Boolean(T,TEXT("transitional_enabled"),Enabled)||!Text(T,TEXT("color"),Color)) return false;
            if (T->Values[TEXT("blend")]->Type!=EJson::Null) return Bad(TEXT("PaperZD 2.2.4 has no per-transition blend settings; blend must be null."));
            auto* A=Map.Find(From); auto* B=Map.Find(To);
            if (!A||!B||A==B||HasNodeClass(*A,TEXT("PaperZDStateGraphNode_Jump"))||HasNodeClass(*B,TEXT("PaperZDStateGraphNode_Jump"))) return Bad(TEXT("Transition endpoints must be distinct states/conduits in this machine."));
            if (FMath::Abs(Priority)>MAX_int32||Priority!=FMath::TruncToDouble(Priority)||Color.Len()!=8) return Bad(TEXT("Invalid priority or RGBA color."));
            FString Key=From+TEXT("/")+To+FString::Printf(TEXT("/%d"),int32(Priority));
            if (TransitionKeys.Contains(Key)) return Bad(TEXT("Transitions must be unique by (from, to, priority).")); TransitionKeys.Add(Key);
            for (TCHAR C:Color) if (!FChar::IsHexDigit(C)) return Bad(TEXT("Invalid hexadecimal color."));
            auto* New=Node(G,T,TransitionClass); if (!New||!SetIdentity(New,T)) return false; Keep.Add(New);
            // This is PaperZD's CreateConnections operation through public pins, after endpoint validation.
            DirectionPin(New,EGPD_Input)->MakeLinkTo(DirectionPin(*A,EGPD_Output));
            DirectionPin(New,EGPD_Output)->MakeLinkTo(DirectionPin(*B,EGPD_Input));
            FindFProperty<FIntProperty>(New->GetClass(),TEXT("Priority"))->SetPropertyValue_InContainer(New,int32(Priority));
            *FindFProperty<FStructProperty>(New->GetClass(),TEXT("Color"))->ContainerPtrToValuePtr<FColor>(New)=FColor::FromHex(Color);
            J Rule=Object(T,TEXT("rule")); if (!Rule) return false; Pending.Add({New,*A,Rule,Location+TEXT(".rule")});
            JV Animation=T->Values[TEXT("transitional_animation")]; auto* P=FindFProperty<FBoolProperty>(New->GetClass(),TEXT("bPlayTransitionalAnimations"));
            if (Animation->Type!=EJson::Null)
            {
                if (Animation->Type!=EJson::Object) return Bad(TEXT("transitional_animation must be an animation graph or null."));
                P->SetPropertyValue_InContainer(New,true); FPropertyChangedEvent Change(P); New->PostEditChangeProperty(Change);
                if (!Anim(ChildGraph(New,TEXT("AnimTransitionalGraph")),Animation->AsObject(),Depth+1,true)) return false;
                P->SetPropertyValue_InContainer(New,Enabled); New->PostEditChangeProperty(Change);
            }
            else if (Enabled||ChildGraph(New,TEXT("AnimTransitionalGraph"))) return Bad(TEXT("Retain the existing transitional graph, or supply one when enabled."));
        }
        Location=Base; return RemoveUnlisted(G,Keep);
    }
    bool Rules()
    {
        for (const auto& P:Pending)
        {
            Location=P.Path;
            if (P.Rule->HasField(TEXT("kind")))
            {
                const J* Original=CustomRules.Find(Id(P.Owner));
                if (!Original||!Keys(P.Rule,{TEXT("kind"),TEXT("hash"),TEXT("reason")})||!Equal(*Original,P.Rule)) return Bad(TEXT("Custom stub is new or changed. Preserve the original stub exactly; custom rules cannot be cloned through this language."));
                KeptCustom.Add(Id(P.Owner)); continue;
            }
            if (CustomRules.Contains(Id(P.Owner))) return Bad(TEXT("Cannot replace an existing custom rule; preserve its stub."));
            FString Why; auto Parsed=ReadRule(GraphEncode(P.Rule),BP,P.From,Why);
            if (!Parsed) return Bad(Why);
            // Supported manual rule graphs may be regenerated after full expression validation.
            auto* RuleGraph=ChildGraph(P.Owner,TEXT("BoundGraph"));
            for (UEdGraphNode* N:RuleGraph->Nodes) if (!HasNodeClass(N,ResultClass)) { N->Modify(); N->NodeComment=RuleTag; }
            if (!WriteRule(BP,P.Owner,P.From,Parsed,Why)) return Bad(Why);
        }
        if (KeptCustom.Num()!=CustomRules.Num()) return Bad(TEXT("A custom rule was omitted; it must remain in the document."));
        return true;
    }
};
}

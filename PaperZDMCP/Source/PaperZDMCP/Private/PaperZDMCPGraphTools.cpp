#include "PaperZDMCPTools.h"
#include "AnimNodes/PaperZDAnimNode_PlaySequence.h"
#include "AnimSequences/PaperZDAnimSequence.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "PaperZDAnimBP.h"
#include "PaperZDAnimInstance.h"
#include "ScopedTransaction.h"
#include "PaperZDMCPGraphDocument.h"
#include "AnimNodes/PaperZDAnimNode_OverrideSlot.h"
#include "EdGraphUtilities.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "K2Node_Event.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableSet.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "AnimSequences/Sources/PaperZDAnimationSource.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopeExit.h"
#include "BlueprintCompilationManager.h"
#include "Internationalization/Regex.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/StrongObjectPtr.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

namespace PaperZDMCPGraph
{
static const TCHAR* MachineClass = TEXT("PaperZDAnimGraphNode_StateMachine");
static const TCHAR* StateClass = TEXT("PaperZDStateGraphNode_State");
static const TCHAR* TransitionClass = TEXT("PaperZDStateGraphNode_Transition");
static const TCHAR* PlayClass = TEXT("PaperZDAnimGraphNode_PlaySequence");
static const TCHAR* SinkClass = TEXT("PaperZDAnimGraphNode_Sink");
static const TCHAR* ResultClass = TEXT("PaperZDTransitionGraphNode_Result");
static const FString RuleTag = TEXT("PaperZDMCP generated rule");

FString GraphEncode(const TSharedPtr<FJsonObject>& Json)
{
    FString Text;
    FJsonSerializer::Serialize(Json.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
    return Text;
}

FString GraphError(const FString& Message)
{
    UKismetSystemLibrary::RaiseScriptError(Message);
    auto Json = MakeShared<FJsonObject>();
    Json->SetBoolField(TEXT("ok"), false);
    Json->SetStringField(TEXT("error"), Message);
    return GraphEncode(Json);
}

UClass* NodeClass(const TCHAR* Name)
{
    // PaperZD graph classes are reflected but not exported as public C++ APIs.
    // Use the engine's graph lifecycle and schema through their public base classes.
    return LoadClass<UEdGraphNode>(nullptr, *(FString(TEXT("/Script/PaperZDEditor.")) + Name));
}

bool HasNodeClass(const UEdGraphNode* Node, const TCHAR* Name)
{
    return Node && Node->GetClass()->GetPathName() == FString(TEXT("/Script/PaperZDEditor.")) + Name;
}

UEdGraph* ChildGraph(UObject* Owner, const TCHAR* Property)
{
    FObjectPropertyBase* Field = Owner ? FindFProperty<FObjectPropertyBase>(Owner->GetClass(), Property) : nullptr;
    return Field ? Cast<UEdGraph>(Field->GetObjectPropertyValue_InContainer(Owner)) : nullptr;
}

UEdGraphNode* FindGraphNode(UEdGraph* Graph, const FString& Id)
{
    FGuid Guid;
    if (!Graph || !FGuid::Parse(Id, Guid)) return nullptr;
    for (UEdGraphNode* Node : Graph->Nodes) if (Node && Node->NodeGuid == Guid) return Node;
    for (UEdGraph* Child : Graph->SubGraphs)
        if (UEdGraphNode* Found = FindGraphNode(Child, Id)) return Found;
    return nullptr;
}

UEdGraphNode* FirstNode(UEdGraph* Graph, const TCHAR* ClassName)
{
    if (Graph) for (UEdGraphNode* Node : Graph->Nodes) if (HasNodeClass(Node, ClassName)) return Node;
    return nullptr;
}

UEdGraphPin* DirectionPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
    if (Node) for (UEdGraphPin* Pin : Node->Pins) if (Pin && Pin->Direction == Direction) return Pin;
    return nullptr;
}

UPaperZDAnimBP* GraphBlueprint(const FString& Path, bool Mutate = true)
{
    if (!IsInGameThread() || !GEditor || (Mutate && GEditor->PlayWorld)) return nullptr;
    const FString Package = FPackageName::ObjectPathToPackageName(Path);
    if (!Package.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Package)) return nullptr;
    const FString Full = Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package);
    if (Path.Contains(TEXT(".")) && Path != Full) return nullptr;
    auto* BP = LoadObject<UPaperZDAnimBP>(nullptr, *Full);
    return BP && BP->AnimationGraph && BP->GetSupportedAnimationSource() ? BP : nullptr;
}

UPaperZDAnimSequence* StateSequence(UPaperZDAnimBP* BP, const FString& Path)
{
    const FString Package = FPackageName::ObjectPathToPackageName(Path);
    if (!Package.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Package)) return nullptr;
    const FString Full = Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package);
    if (Path.Contains(TEXT(".")) && Path != Full) return nullptr;
    auto* Sequence = LoadObject<UPaperZDAnimSequence>(nullptr, *Full);
    return Sequence && Sequence->GetAnimSource() == BP->GetSupportedAnimationSource() ? Sequence : nullptr;
}

bool SimpleName(const FString& Name)
{
    if (Name.IsEmpty() || Name.Len() > 100 || Name.Equals(TEXT("None"), ESearchCase::IgnoreCase) || FChar::IsDigit(Name[0])) return false;
    for (TCHAR C : Name) if (!FChar::IsAlnum(C) && C != TEXT('_')) return false;
    return true;
}

bool NameUsed(UEdGraph* Graph, const FString& Name)
{
    for (UEdGraph* Child : Graph->SubGraphs) if (Child && Child->GetFName() == FName(*Name)) return true;
    return false;
}

UEdGraphNode* SpawnGraphNode(UEdGraph* Graph, UClass* Class, int32 X = 0, int32 Y = 0)
{
    FGraphNodeCreator<UEdGraphNode> Creator(*Graph);
    UEdGraphNode* Node = Creator.CreateNode(false, Class);
    Node->NodePosX = X;
    Node->NodePosY = Y;
    Creator.Finalize();
    return Node;
}

FPaperZDAnimNode_PlaySequence* Playback(UEdGraphNode* Node)
{
    auto* Field = Node ? FindFProperty<FStructProperty>(Node->GetClass(), TEXT("AnimNode")) : nullptr;
    return Field && Field->Struct == FPaperZDAnimNode_PlaySequence::StaticStruct()
        ? Field->ContainerPtrToValuePtr<FPaperZDAnimNode_PlaySequence>(Node) : nullptr;
}

UEdGraphNode* SimplePlayer(UEdGraphNode* State)
{
    UEdGraph* Graph = ChildGraph(State, TEXT("BoundGraph"));
    if (!Graph || Graph->Nodes.Num() != 2) return nullptr;
    UEdGraphNode* Player = FirstNode(Graph, PlayClass);
    UEdGraphPin* Input = DirectionPin(FirstNode(Graph, SinkClass), EGPD_Input);
    UEdGraphPin* Output = DirectionPin(Player, EGPD_Output);
    if (!Player || !Playback(Player) || !Input || Input->LinkedTo.Num() != 1 || Input->LinkedTo[0] != Output) return nullptr;
    for (UEdGraphPin* Pin : Player->Pins)
        if (Pin && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num()) return nullptr;
    return Player;
}

void SnapshotGraph(UEdGraph* Graph)
{
    Graph->Modify();
    for (UEdGraphNode* Node : Graph->Nodes) if (Node) Node->Modify();
}

void GraphChanged(UPaperZDAnimBP* BP, UEdGraph* Graph)
{
    Graph->NotifyGraphChanged();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    BP->MarkPackageDirty();
}

FString NodeResponse(UPaperZDAnimBP* BP, UEdGraphNode* Node, const TCHAR* Key)
{
    auto Json = MakeShared<FJsonObject>();
    Json->SetBoolField(TEXT("ok"), true);
    Json->SetStringField(TEXT("asset_path"), BP->GetPathName());
    Json->SetStringField(Key, Node->NodeGuid.ToString());
    Json->SetBoolField(TEXT("dirty"), BP->GetOutermost()->IsDirty());
    return GraphEncode(Json);
}

UEdGraphNode* TransitionEndpoint(UEdGraphNode* Transition, EEdGraphPinDirection Direction)
{
    UEdGraphPin* Pin = DirectionPin(Transition, Direction);
    return Pin && Pin->LinkedTo.Num() == 1 ? Pin->LinkedTo[0]->GetOwningNode() : nullptr;
}

// Conditions are parsed and type-checked in full before touching any graph.
struct FRule
{
    enum EKind { Constant, Variable, Remaining, Pure, Compare, Not, All, Any } Kind = Constant;
    bool BoolValue = false;
    FString VariableName;
    FString PlayerId;
    TMap<FString, FString> Arguments;
    bool bFinished = false;
    bool bRatio = false;
    FString Function;
    double Number = 0;
    bool bFloat32 = false;
    TArray<TSharedPtr<FRule>> Children;
};

bool VariableType(UPaperZDAnimBP* BP, const FString& Name, FEdGraphPinType& Type)
{
    for (const FBPVariableDescription& Var : BP->NewVariables)
        if (Var.VarName == FName(*Name)) { Type = Var.VarType; return true; }
    FProperty* Property = BP->SkeletonGeneratedClass ? FindFProperty<FProperty>(BP->SkeletonGeneratedClass, *Name) : nullptr;
    return Property && GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, Type);
}

TSharedPtr<FRule> ParseRule(const TSharedPtr<FJsonObject>& Json, UPaperZDAnimBP* BP, UEdGraphNode* From,
    FString& Error, int32& Count, int32 Depth = 0)
{
    if (!Json || ++Count > 64 || Depth > 8) { Error = TEXT("Rule exceeds 64 expressions or depth 8."); return nullptr; }
    auto Rule = MakeShared<FRule>();
    bool BoolValue;
    if (Json->Values.Num() == 1 && Json->TryGetBoolField(TEXT("constant"), BoolValue))
    { Rule->BoolValue = BoolValue; return Rule; }
    for (const TCHAR* Group : {TEXT("all"), TEXT("any"), TEXT("not")})
    {
        if (!Json->HasField(Group)) continue;
        if (Json->Values.Num() != 1) { Error = TEXT("Group expressions accept exactly one key."); return nullptr; }
        Rule->Kind = FString(Group) == TEXT("all") ? FRule::All : FString(Group) == TEXT("any") ? FRule::Any : FRule::Not;
        TArray<TSharedPtr<FJsonValue>> Items;
        if (Rule->Kind == FRule::Not) Items.Add(Json->Values[Group]);
        else
        {
            const TArray<TSharedPtr<FJsonValue>>* Array;
            if (!Json->TryGetArrayField(Group, Array) || Array->Num() < 2 || Array->Num() > 8)
            { Error = TEXT("all/any require 2..8 child expressions."); return nullptr; }
            Items = *Array;
        }
        for (const auto& Item : Items)
        {
            if (Item->Type != EJson::Object) { Error = TEXT("Each rule must be an object."); return nullptr; }
            auto Child = ParseRule(Item->AsObject(), BP, From, Error, Count, Depth + 1);
            if (!Child) return nullptr;
            Rule->Children.Add(Child);
        }
        return Rule;
    }
    FEdGraphPinType Type;
    FString FinishedPlayer;
    if (Json->Values.Num() == 1 && Json->TryGetStringField(TEXT("animation_finished"), FinishedPlayer))
    {
        UEdGraphNode* Player = FindGraphNode(BP->AnimationGraph, FinishedPlayer);
        auto* Data = Playback(Player);
        if (!Data || Data->bLoopAnimation || SimplePlayer(From)!=Player) { Error = TEXT("animation_finished requires the unambiguous nonlooping player in this transition's source state."); return nullptr; }
        Rule->Kind = FRule::Remaining; Rule->PlayerId = FinishedPlayer; Rule->bFloat32 = true; Rule->bFinished = true;
        auto Comparison = MakeShared<FRule>(); Comparison->Kind = FRule::Compare;
        Comparison->Number = 0.0001; Comparison->Function = TEXT("LessEqual_DoubleDouble"); Comparison->Children.Add(Rule);
        return Comparison;
    }
    if (Json->TryGetStringField(TEXT("function"), Rule->Function))
    {
        UFunction* Function = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass->FindFunctionByName(*Rule->Function) : nullptr;
        const TSharedPtr<FJsonObject>* Arguments;
        if (!Function || !Function->HasAllFunctionFlags(FUNC_BlueprintPure | FUNC_BlueprintCallable) ||
            Function->HasAnyFunctionFlags(FUNC_Static | FUNC_Delegate) || Function->HasMetaData(TEXT("AnimGetter")) || !Json->TryGetObjectField(TEXT("arguments"), Arguments))
        { Error = TEXT("function requires a callable pure instance function on the AnimInstance and arguments object."); return nullptr; }
        FProperty* Return = Function->GetReturnProperty();
        if (!Return || !GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Return, Type) || Type.IsContainer())
        { Error = TEXT("Pure condition function must return bool or float/double."); return nullptr; }
        for (const auto& Pair : (*Arguments)->Values)
        {
            FString Value;
            FProperty* Parameter = FindFProperty<FProperty>(Function, *Pair.Key);
            if (!Pair.Value->TryGetString(Value) || !Parameter || !Parameter->HasAnyPropertyFlags(CPF_Parm) || Parameter->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm))
            { Error = TEXT("Pure function arguments must name input parameters with string literal values."); return nullptr; }
            Rule->Arguments.Add(FString(*Pair.Key), Value);
        }
        Rule->Kind = FRule::Pure;
        if (Type.PinCategory == UEdGraphSchema_K2::PC_Boolean && Json->Values.Num() == 2) return Rule;
        if (Type.PinCategory != UEdGraphSchema_K2::PC_Real) { Error = TEXT("Pure condition return must be bool or real."); return nullptr; }
        Rule->bFloat32 = Type.PinSubCategory == UEdGraphSchema_K2::PC_Float;
    }
    else if (Json->TryGetStringField(TEXT("variable"), Rule->VariableName))
    {
        if (!SimpleName(Rule->VariableName) || !VariableType(BP, Rule->VariableName, Type) || Type.IsContainer())
        { Error = TEXT("Rule variable is missing or not a supported scalar."); return nullptr; }
        if (Type.PinCategory == UEdGraphSchema_K2::PC_Boolean)
        {
            Rule->Kind = FRule::Variable;
            if (Json->Values.Num() == 1) return Rule;
            FString Op;
            if (Json->Values.Num() != 3 || !Json->TryGetStringField(TEXT("op"), Op) ||
                (Op != TEXT("==") && Op != TEXT("!=")) || !Json->TryGetBoolField(TEXT("value"), BoolValue))
            { Error = TEXT("Boolean comparisons require == or != and a boolean value."); return nullptr; }
            if ((Op == TEXT("==")) == BoolValue) return Rule;
            auto Negated = MakeShared<FRule>(); Negated->Kind = FRule::Not; Negated->Children.Add(Rule); return Negated;
        }
        if (Type.PinCategory != UEdGraphSchema_K2::PC_Real)
        { Error = TEXT("Numeric rules require a float/double variable."); return nullptr; }
        Rule->bFloat32 = Type.PinSubCategory == UEdGraphSchema_K2::PC_Float;
        Rule->Kind = FRule::Variable;
    }
    else if (Json->TryGetBoolField(TEXT("time_remaining"), BoolValue) && BoolValue)
    {
        if (!Json->TryGetStringField(TEXT("player"), Rule->PlayerId) || !SimplePlayer(From) || SimplePlayer(From)!=FindGraphNode(BP->AnimationGraph, Rule->PlayerId))
        { Error = TEXT("time_remaining requires the sole directly connected player in the transition's source state; multiple players, SelectBy, and conduit sources are ambiguous."); return nullptr; }
        if (Json->HasField(TEXT("ratio")) && !Json->TryGetBoolField(TEXT("ratio"),Rule->bRatio)) { Error=TEXT("ratio must be boolean."); return nullptr; }
        Rule->Kind = FRule::Remaining;
        Rule->bFloat32 = true;
    }
    else { Error = TEXT("Unknown rule. Use constant, variable, time_remaining, all, any, or not."); return nullptr; }
    FString Op;
    double Number;
    if (Json->Values.Num() != (Rule->Kind == FRule::Variable ? 3 : 4)+(Rule->Kind==FRule::Remaining && Json->HasField(TEXT("ratio"))?1:0) || !Json->TryGetStringField(TEXT("op"), Op) ||
        !Json->TryGetNumberField(TEXT("value"), Number) || !FMath::IsFinite(Number))
    { Error = TEXT("Numeric comparisons require op and a finite numeric value."); return nullptr; }
    static const TMap<FString, FString> Operators = {
        {TEXT(">"), TEXT("Greater")}, {TEXT(">="), TEXT("GreaterEqual")},
        {TEXT("<"), TEXT("Less")}, {TEXT("<="), TEXT("LessEqual")},
        {TEXT("=="), TEXT("EqualEqual")}, {TEXT("!="), TEXT("NotEqual")}
    };
    const FString* Function = Operators.Find(Op);
    if (!Function) { Error = TEXT("Unsupported comparison operator."); return nullptr; }
    auto Comparison = MakeShared<FRule>();
    Comparison->Kind = FRule::Compare;
    Comparison->Number = Number;
    // UE5 math comparisons operate on doubles; explicitly convert float getters.
    Comparison->Function = *Function + TEXT("_DoubleDouble");
    Comparison->Children.Add(Rule);
    return Comparison;
}

TSharedPtr<FRule> ReadRule(const FString& Text, UPaperZDAnimBP* BP, UEdGraphNode* From, FString& Error)
{
    TSharedPtr<FJsonObject> Json;
    if (Text.Len() > 16000 || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json))
    { Error = TEXT("RuleJson must be a JSON object of at most 16000 characters."); return nullptr; }
    int32 Count = 0;
    return ParseRule(Json, BP, From, Error, Count);
}

struct FRuleBuilder
{
    UPaperZDAnimBP* BP;
    UEdGraph* Graph;
    UEdGraphNode* From;
    TArray<UEdGraphNode*> Added;
    FString Error;

    bool Wire(UEdGraphPin* A, UEdGraphPin* B)
    {
        if (A && B && Graph->GetSchema()->TryCreateConnection(A, B)) return true;
        Error = TEXT("PaperZD rejected a rule connection; existing rule was retained."); return false;
    }
    void Track(UEdGraphNode* Node)
    {
        Node->NodeComment = RuleTag;
        Node->NodePosX = -600 - Added.Num() * 180;
        Node->NodePosY = Added.Num() * 100;
        Added.Add(Node);
    }
    UK2Node_CallFunction* Function(const FString& Name)
    {
        UFunction* Fn = UKismetMathLibrary::StaticClass()->FindFunctionByName(*Name);
        if (!Fn) { Error = TEXT("Required Kismet math function is unavailable: ") + Name; return nullptr; }
        FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
        auto* Node = Creator.CreateNode(false);
        Node->SetFromFunction(Fn);
        Creator.Finalize(); Track(Node); return Node;
    }
    UEdGraphPin* Build(const TSharedPtr<FRule>& Rule)
    {
        if (Rule->Kind == FRule::Variable)
        {
            FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
            auto* Node = Creator.CreateNode(false);
            Node->VariableReference.SetSelfMember(*Rule->VariableName);
            Creator.Finalize(); Track(Node);
            return Node->GetValuePin();
        }
        if (Rule->Kind == FRule::Remaining)
        {
            UClass* Class = NodeClass(TEXT("PaperZDK2Node_AnimGetter"));
            if (!Class || !Class->IsChildOf(UK2Node_CallFunction::StaticClass())) { Error = TEXT("PaperZD animation getter unavailable."); return nullptr; }
            for (const TCHAR* Field : {TEXT("SourceNode"), TEXT("GetterClass"), TEXT("SourceAnimBlueprint")})
                if (!FindFProperty<FObjectPropertyBase>(Class, Field)) { Error = TEXT("Unsupported animation getter layout."); return nullptr; }
            FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
            auto* Node = Creator.CreateNode(false, Class);
            FindFProperty<FObjectPropertyBase>(Class, TEXT("SourceNode"))->SetObjectPropertyValue_InContainer(Node, FindGraphNode(BP->AnimationGraph, Rule->PlayerId));
            FindFProperty<FObjectPropertyBase>(Class, TEXT("GetterClass"))->SetObjectPropertyValue_InContainer(Node, BP->ParentClass);
            FindFProperty<FObjectPropertyBase>(Class, TEXT("SourceAnimBlueprint"))->SetObjectPropertyValue_InContainer(Node, BP);
            const auto* Data = Playback(FindGraphNode(BP->AnimationGraph, Rule->PlayerId));
            Node->SetFromFunction(UPaperZDAnimInstance::StaticClass()->FindFunctionByName(
                Rule->bFinished && Data && Data->PlayRate < 0 ? TEXT("GetInstanceAssetPlayerTime") : Rule->bRatio ? TEXT("GetInstanceAssetPlayerTimeFromEndFraction") : TEXT("GetInstanceAssetPlayerTimeFromEnd")));
            if (auto* Title = FindFProperty<FTextProperty>(Class, TEXT("CachedTitle"))) Title->SetPropertyValue_InContainer(Node, FText::FromString(TEXT("Time Remaining")));
            Creator.Finalize(); Track(Node); return Node->GetReturnValuePin();
        }
        if (Rule->Kind == FRule::Pure)
        {
            FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
            auto* Node = Creator.CreateNode(false);
            Node->FunctionReference.SetSelfMember(*Rule->Function);
            Creator.Finalize(); Track(Node);
            for (const auto& Pair : Rule->Arguments)
            {
                auto* Pin = Node->FindPin(*Pair.Key);
                if (!Pin || Pin->Direction != EGPD_Input) { Error = TEXT("Invalid pure function input."); return nullptr; }
                const FString Invalid = Graph->GetSchema()->IsPinDefaultValid(Pin, Pair.Value, nullptr, FText::GetEmpty());
                if (!Invalid.IsEmpty()) { Error = Invalid; return nullptr; }
                Graph->GetSchema()->TrySetDefaultValue(*Pin, Pair.Value);
            }
            return Node->GetReturnValuePin();
        }
        if (Rule->Kind == FRule::Compare)
        {
            UEdGraphPin* Value = Build(Rule->Children[0]);
            if (!Value) return nullptr;
            auto* Node = Function(Rule->Function);
            if (!Node || !Wire(Value, Node->FindPin(TEXT("A")))) return nullptr;
            Graph->GetSchema()->TrySetDefaultValue(*Node->FindPinChecked(TEXT("B")), FString::Printf(TEXT("%.17g"), Rule->Number));
            return Node->GetReturnValuePin();
        }
        if (Rule->Kind == FRule::Not)
        {
            auto* Input = Build(Rule->Children[0]);
            auto* Node = Function(TEXT("Not_PreBool"));
            return Node && Wire(Input, Node->FindPin(TEXT("A"))) ? Node->GetReturnValuePin() : nullptr;
        }
        if (Rule->Kind == FRule::All || Rule->Kind == FRule::Any)
        {
            UEdGraphPin* Current = Build(Rule->Children[0]);
            for (int32 Index = 1; Current && Index < Rule->Children.Num(); ++Index)
            {
                auto* Next = Build(Rule->Children[Index]);
                auto* Node = Function(Rule->Kind == FRule::All ? TEXT("BooleanAND") : TEXT("BooleanOR"));
                if (!Node || !Wire(Current, Node->FindPin(TEXT("A"))) || !Wire(Next, Node->FindPin(TEXT("B")))) return nullptr;
                Current = Node->GetReturnValuePin();
            }
            return Current;
        }
        // A boolean identity node permits constants inside compound expressions.
        auto* Node = Function(TEXT("Not_PreBool"));
        if (!Node) return nullptr;
        Graph->GetSchema()->TrySetDefaultValue(*Node->FindPinChecked(TEXT("A")), Rule->BoolValue ? TEXT("false") : TEXT("true"));
        return Node->GetReturnValuePin();
    }
};

bool WriteRule(UPaperZDAnimBP* BP, UEdGraphNode* Transition, UEdGraphNode* From, const TSharedPtr<FRule>& Rule, FString& Error)
{
    UEdGraph* Graph = ChildGraph(Transition, TEXT("BoundGraph"));
    auto* Result = FirstNode(Graph, ResultClass);
    UEdGraphPin* Input = Result ? Result->FindPin(TEXT("bCanEnterTransition")) : nullptr;
    if (!Input) { Error = TEXT("Transition result pin is missing."); return false; }
    TArray<UEdGraphNode*> OldNodes;
    for (UEdGraphNode* Node : Graph->Nodes)
        if (Node != Result)
        {
            if (!Node || Node->NodeComment != RuleTag) { Error = TEXT("Rule contains manually authored nodes; edit it in Unreal instead."); return false; }
            OldNodes.Add(Node);
        }
    SnapshotGraph(Graph);
    FRuleBuilder Builder{BP, Graph, From, {}, {}};
    UEdGraphPin* Output = Rule->Kind == FRule::Constant ? nullptr : Builder.Build(Rule);
    if (Rule->Kind != FRule::Constant && (!Output || !Builder.Wire(Output, Input)))
    {
        for (UEdGraphNode* Node : Builder.Added) Node->DestroyNode();
        Error = Builder.Error.IsEmpty() ? TEXT("Could not build rule graph.") : Builder.Error;
        return false;
    }
    if (Rule->Kind == FRule::Constant)
    {
        Graph->GetSchema()->BreakPinLinks(*Input, true);
        Graph->GetSchema()->TrySetDefaultValue(*Input, Rule->BoolValue ? TEXT("true") : TEXT("false"));
    }
    for (UEdGraphNode* Node : OldNodes) Node->DestroyNode();
    Graph->NotifyGraphChanged();
    return true;
}
}
#include "PaperZDMCPGraphDocument.inl"
#include "PaperZDMCPGraphApply.inl"
#include "PaperZDMCPGraphOwner.inl"
#include "PaperZDMCPGraphOwnerSemantics.inl"
#include "PaperZDMCPGraphEntry.inl"
#include "Tests/PaperZDMCPGraphTests.inl"
#include "Tests/PaperZDMCPPersistenceTests.inl"

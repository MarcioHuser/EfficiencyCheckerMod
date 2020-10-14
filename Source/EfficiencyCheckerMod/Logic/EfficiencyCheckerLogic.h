#pragma once

#include <map>

#include "EfficiencyCheckerBuilding.h"
#include "FGItemDescriptor.h"
#include "EfficiencyCheckerLogic.generated.h"

UCLASS()
class AEfficiencyCheckerLogic : public AActor
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="EfficiencyCheckerLogic")
    virtual void Initialize
    (
        UPARAM(DisplayName = "None Item Descriptor") const TSet<TSubclassOf<UFGItemDescriptor>>& in_noneItemDescriptors,
        UPARAM(DisplayName = "Wildcard Item Descriptor") const TSet<TSubclassOf<UFGItemDescriptor>>& in_wildcardItemDescriptors,
        UPARAM(DisplayName = "Any Undefined Item Descriptor") const TSet<TSubclassOf<UFGItemDescriptor>>& in_anyUndefinedItemDescriptors,
        UPARAM(DisplayName = "Overflow Item Descriptor") const TSet<TSubclassOf<UFGItemDescriptor>>& in_overflowItemDescriptors,
        UPARAM(DisplayName = "Nuclear Waste Item Descriptor") const TSet<TSubclassOf<UFGItemDescriptor>>& in_nuclearWasteItemDescriptors
    );

    UFUNCTION(BlueprintCallable, Category="EfficiencyCheckerLogic")
    virtual void Terminate();

    UFUNCTION(BlueprintCallable, Category="EfficiencyCheckerLogic")
    static void DumpInformation(TSubclassOf<UFGItemDescriptor> equipmentDescriptor);

    UFUNCTION(BlueprintCallable)
    virtual bool IsValidBuildable(class AFGBuildable* newBuildable);

    static void collectInput
    (
        EResourceForm resourceForm,
        bool customInjectedInput,
        class UFGConnectionComponent* connector,
        float& out_injectedInput,
        float& out_limitedThroughput,
        TSet<AActor*>& seenActors,
        TSet<class AFGBuildable*>& connected,
        TSet<TSubclassOf<UFGItemDescriptor>>& out_injectedItems,
        const TSet<TSubclassOf<UFGItemDescriptor>>& restrictItems,
        class AFGBuildableSubsystem* buildableSubsystem,
        int level,
        const FString& indent
    );

    static void collectOutput
    (
        EResourceForm resourceForm,
        class UFGConnectionComponent* connector,
        float& out_requiredOutput,
        float& out_limitedThroughput,
        std::map<AActor*, TSet<TSubclassOf<UFGItemDescriptor>>>& seenActors,
        TSet<AFGBuildable*>& connected,
        const TSet<TSubclassOf<UFGItemDescriptor>>& in_injectedItems,
        class AFGBuildableSubsystem* buildableSubsystem,
        int level,
        const FString& indent
    );

    static bool containsActor(const std::map<AActor*, TSet<TSubclassOf<UFGItemDescriptor>>>& seenActors, AActor* actor);
    static bool actorContainsItem(const std::map<AActor*, TSet<TSubclassOf<UFGItemDescriptor>>>& seenActors, AActor* actor, const TSubclassOf<UFGItemDescriptor>& item);
    static void addAllItemsToActor(std::map<AActor*, TSet<TSubclassOf<UFGItemDescriptor>>>& seenActors, AActor* actor, const TSet<TSubclassOf<UFGItemDescriptor>>& items);

    static void dumpUnknownClass(const FString& indent, AActor* owner);

    inline static FString
    getTimeStamp()
    {
        const auto now = FDateTime::Now();

        return FString::Printf(TEXT("%02d:%02d:%02d"), now.GetHour(), now.GetMinute(), now.GetSecond());
    }

    TSet<TSubclassOf<UFGItemDescriptor>> nuclearWasteItemDescriptors;
    TSet<TSubclassOf<UFGItemDescriptor>> noneItemDescriptors;
    TSet<TSubclassOf<UFGItemDescriptor>> wildCardItemDescriptors;
    TSet<TSubclassOf<UFGItemDescriptor>> anyUndefinedItemDescriptors;
    TSet<TSubclassOf<UFGItemDescriptor>> overflowItemDescriptors;

    FCriticalSection eclCritical;

    static AEfficiencyCheckerLogic* singleton;

    TSet<class AEfficiencyCheckerBuilding*> allEfficiencyBuildings;
    TSet<class AFGBuildableConveyorBelt*> allBelts;
    TSet<class AFGBuildablePipeline*> allPipes;
    TSet<class AFGBuildable*> allTeleporters;

    FActorEndPlaySignature::FDelegate removeEffiencyBuildingDelegate;
    FActorEndPlaySignature::FDelegate removeBeltDelegate;
    FActorEndPlaySignature::FDelegate removePipeDelegate;
    FActorEndPlaySignature::FDelegate removeTeleporterDelegate;

    virtual void addEfficiencyBuilding(class AEfficiencyCheckerBuilding* actor);
    virtual void addBelt(AFGBuildableConveyorBelt* actor);
    virtual void addPipe(AFGBuildablePipeline* actor);
    virtual void addTeleporter(AFGBuildable* actor);

    UFUNCTION()
    virtual void removeEfficiencyBuilding(AActor* actor, EEndPlayReason::Type reason);
    UFUNCTION()
    virtual void removeBelt(AActor* actor, EEndPlayReason::Type reason);
    UFUNCTION()
    virtual void removePipe(AActor* actor, EEndPlayReason::Type reason);
    UFUNCTION()
    virtual void removeTeleporter(AActor* actor, EEndPlayReason::Type reason);
};

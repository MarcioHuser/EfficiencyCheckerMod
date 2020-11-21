#pragma once

#include "FGBuildableConveyorBase.h"
#include "FGEquipment.h"
#include "EfficiencyCheckerEquipment.generated.h"

UCLASS()
class AEfficiencyCheckerEquipment : public AFGEquipment
{
    GENERATED_BODY()
public:
    AEfficiencyCheckerEquipment();

    UFUNCTION(BlueprintCallable)
    virtual void PrimaryFirePressed(class AFGBuildable* targetBuildable);

    UFUNCTION(BlueprintImplementableEvent, Category = "EfficiencyChecker")
    void ShowStatsWidget
    (
        UPARAM(DisplayName = "Injected Input") float in_injectedInput,
        UPARAM(DisplayName = "Limited Throughput") float in_limitedThroughput,
        UPARAM(DisplayName = "Required Output") float in_requiredOutput,
        UPARAM(DisplayName = "Items") const TArray<TSubclassOf<UFGItemDescriptor>>& in_injectedItems,
        UPARAM(DisplayName = "Overflow") bool in_overflow
    );

public:
    FORCEINLINE ~AEfficiencyCheckerEquipment() = default;
};

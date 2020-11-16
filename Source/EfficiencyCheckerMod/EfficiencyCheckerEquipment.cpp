#include "EfficiencyCheckerEquipment.h"
#include "Logic/EfficiencyCheckerLogic.h"

#include "FGBuildablePipeline.h"
#include "FGItemDescriptor.h"
#include "FGPipeConnectionComponent.h"
#include "FGPlayerController.h"

#include "Util/Optimize.h"
#include "Util/Util.h"

#ifndef OPTIMIZE
#pragma optimize( "", off )
#endif

AEfficiencyCheckerEquipment::AEfficiencyCheckerEquipment()
{
}

void AEfficiencyCheckerEquipment::PrimaryFirePressed(AFGBuildable* targetBuildable)
{
    float injectedInput = 0;
    float requiredOutput = 0;

    class UFGConnectionComponent* inputConnector = nullptr;
    class UFGConnectionComponent* outputConnector = nullptr;

    TSet<TSubclassOf<UFGItemDescriptor>> restrictedItems;

    auto resourceForm = EResourceForm::RF_INVALID;

    TSet<AFGBuildable*> connected;
    const auto buildableSubsystem = AFGBuildableSubsystem::Get(GetWorld());
    const FString indent(TEXT("    "));
    TSet<TSubclassOf<UFGItemDescriptor>> injectedItemsSet;

    if (targetBuildable)
    {
        auto conveyor = Cast<AFGBuildableConveyorBase>(targetBuildable);
        if (conveyor)
        {
            inputConnector = conveyor->GetConnection0();
            outputConnector = conveyor->GetConnection1();

            resourceForm = EResourceForm::RF_SOLID;

            TArray<TSubclassOf<UFGItemDescriptor>> allItems;
            UFGBlueprintFunctionLibrary::Cheat_GetAllDescriptors(allItems);

            for (auto item : allItems)
            {
                if (!item ||
                    !UFGBlueprintFunctionLibrary::CanBeOnConveyor(item) ||
                    UFGItemDescriptor::GetForm(item) != EResourceForm::RF_SOLID ||
                    AEfficiencyCheckerLogic::singleton->wildCardItemDescriptors.Contains(item) ||
                    AEfficiencyCheckerLogic::singleton->overflowItemDescriptors.Contains(item) ||
                    AEfficiencyCheckerLogic::singleton->noneItemDescriptors.Contains(item) ||
                    AEfficiencyCheckerLogic::singleton->anyUndefinedItemDescriptors.Contains(item)
                    )
                {
                    continue;;
                }

                restrictedItems.Add(item);
            }
        }
        else
        {
            auto pipe = Cast<AFGBuildablePipeline>(targetBuildable);
            if (pipe)
            {
                inputConnector = pipe->GetPipeConnection0();
                outputConnector = pipe->GetPipeConnection1();

                TSubclassOf<UFGItemDescriptor> fluidItem = pipe->GetPipeConnection0()->GetFluidDescriptor();
                if (! fluidItem)
                {
                    fluidItem = pipe->GetPipeConnection1()->GetFluidDescriptor();
                }

                if (fluidItem)
                {
                    restrictedItems.Add(fluidItem);
                    injectedItemsSet.Add(fluidItem);

                    resourceForm = UFGItemDescriptor::GetForm(fluidItem);
                }
            }
            else
            {
                return;
            }
        }
    }

    float limitedThroughputIn = 0;

    if (inputConnector)
    {
        TSet<AActor*> seenActors;

        AEfficiencyCheckerLogic::collectInput(
            resourceForm,
            injectedInput,
            inputConnector,
            injectedInput,
            limitedThroughputIn,
            seenActors,
            connected,
            injectedItemsSet,
            restrictedItems,
            buildableSubsystem,
            0,
            indent
            );
    }

    float limitedThroughputOut = 0;

    if (outputConnector)
    {
        std::map<AActor*, TSet<TSubclassOf<UFGItemDescriptor>>> seenActors;

        AEfficiencyCheckerLogic::collectOutput(
            resourceForm,
            outputConnector,
            requiredOutput,
            limitedThroughputOut,
            seenActors,
            connected,
            injectedItemsSet,
            buildableSubsystem,
            0,
            indent
            );
    }

    if (inputConnector || outputConnector)
    {
        ShowStatsWidget(injectedInput, min(limitedThroughputIn, limitedThroughputOut), requiredOutput, injectedItemsSet.Array());
    }
}

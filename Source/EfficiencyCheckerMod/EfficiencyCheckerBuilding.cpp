// ReSharper disable CppUE4CodingStandardNamingViolationWarning
// ReSharper disable CommentTypo

#include "EfficiencyCheckerBuilding.h"
#include "EfficiencyCheckerRCO.h"
#include "Logic/EfficiencyCheckerLogic.h"

#include "EfficiencyCheckerModModule.h"
#include "FGBuildableConveyorBelt.h"
#include "FGBuildableDockingStation.h"
#include "FGBuildableManufacturer.h"
#include "FGBuildablePipeline.h"
#include "FGBuildablePipelineAttachment.h"
#include "FGBuildableRailroadStation.h"
#include "FGBuildableSplitterSmart.h"
#include "FGBuildableSubSystem.h"
#include "FGFactoryConnectionComponent.h"
#include "FGPipeConnectionComponent.h"
#include "FGPipeSubsystem.h"
#include "FGPowerInfoComponent.h"
#include "FGRailroadSubsystem.h"
#include "FGResourceNode.h"
#include "FGTrain.h"

#include "UnrealNetwork.h"

#include "SML/util/Logging.h"
#include "SML/util/Utility.h"
#include "SML/util/ReflectionHelper.h"

#include <map>

#include "Util/Optimize.h"
#include "Util/Util.h"

#ifndef OPTIMIZE
#pragma optimize( "", off )
#endif

// Sets default values
AEfficiencyCheckerBuilding::AEfficiencyCheckerBuilding()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bAllowTickOnDedicatedServer = true;
    PrimaryActorTick.bStartWithTickEnabled = false;

    PrimaryActorTick.SetTickFunctionEnable(false);

    // //// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
    // mFactoryTickFunction.bCanEverTick = true;
    // mFactoryTickFunction.bAllowTickOnDedicatedServer = true;
    // mFactoryTickFunction.bStartWithTickEnabled = false;
    //
    // mFactoryTickFunction.SetTickFunctionEnable(false);
}

// Called when the game starts or when spawned
void AEfficiencyCheckerBuilding::BeginPlay()
{
    _TAG_NAME = GetName() + TEXT(": ");

    SML::Logging::info(*getTagName(), TEXT("BeginPlay"));

    Super::BeginPlay();

    // {
    //     SML::Logging::info(*getTagName(),TEXT("Adding to list!"));
    //
    //     FScopeLock ScopeLock(&AEfficiencyCheckerLogic::eclCritical);
    //     AEfficiencyCheckerLogic::allEfficiencyBuildings.Add(this);
    // }

    if (HasAuthority())
    {
        auto arrows = GetComponentsByTag(UStaticMeshComponent::StaticClass(), TEXT("DirectionArrow"));
        for (auto arrow : arrows)
        {
            // Cast<UStaticMeshComponent>(arrow)->SetVisibilitySML(false);

            arrow->DestroyComponent();
        }

        TInlineComponentArray<UWidgetComponent*> widgets(this, true);
        for (auto widget : widgets)
        {
            widget->SetVisibilitySML(true);
        }

        if (innerPipelineAttachment && pipelineToSplit)
        {
            auto fluidType = pipelineToSplit->GetFluidDescriptor();

            if (150 < pipelineSplitOffset && pipelineSplitOffset < pipelineToSplit->GetLength() - 150)
            {
                // Split into two pipes
                auto newPipes = AFGBuildablePipeline::Split(pipelineToSplit, pipelineSplitOffset, false, innerPipelineAttachment);

                if (newPipes.Num() > 1)
                {
                    auto attachmentPipeConnections = innerPipelineAttachment->GetPipeConnections();

                    const auto pipe0OutputCoincident = (
                        newPipes[0]->GetPipeConnection1()->GetComponentRotation().Vector() | attachmentPipeConnections[0]->GetComponentRotation().Vector()
                    ) >= 0;

                    // Must connect to the one point the other way around
                    newPipes[0]->GetPipeConnection1()->SetConnection(
                        pipe0OutputCoincident
                            ? attachmentPipeConnections[1]
                            : attachmentPipeConnections[0]
                        );

                    // Must connect to the one point the other way around
                    newPipes[1]->GetPipeConnection0()->SetConnection(
                        pipe0OutputCoincident
                            ? attachmentPipeConnections[0]
                            : attachmentPipeConnections[1]
                        );
                }
            }
            else
            {
                // Attach to the nearest edge of the pipe
                const auto closestPipeConnection = pipelineSplitOffset <= 150 ? pipelineToSplit->GetPipeConnection0() : pipelineToSplit->GetPipeConnection1();

                for (auto attachmentConnection : innerPipelineAttachment->GetPipeConnections())
                {
                    if (FVector::Dist(attachmentConnection->GetConnectorLocation(), closestPipeConnection->GetConnectorLocation()) < 1)
                    {
                        closestPipeConnection->SetConnection(attachmentConnection);

                        break;
                    }
                }
            }

            AFGPipeSubsystem::Get(GetWorld())->TrySetNetworkFluidDescriptor(innerPipelineAttachment->GetPipeConnections()[0]->GetPipeNetworkID(), fluidType);
        }

        pipelineToSplit = nullptr;

        if (FEfficiencyCheckerModModule::autoUpdate && autoUpdateMode == EAutoUpdateType::AUT_USE_DEFAULT ||
            autoUpdateMode == EAutoUpdateType::AUT_ENABLED)
        {
            lastUpdated = GetWorld()->GetTimeSeconds();
            updateRequested = lastUpdated + FEfficiencyCheckerModModule::autoUpdateTimeout; // Give a timeout
            SetActorTickEnabled(true);
            SetActorTickInterval(FEfficiencyCheckerModModule::autoUpdateTimeout);
            // mFactoryTickFunction.SetTickFunctionEnable(true);
            // mFactoryTickFunction.TickInterval = autoUpdateTimeout;

            addOnDestroyBindings(pendingBuildables);
            addOnDestroyBindings(connectedBuildables);
            addOnRecipeChangedBindings(connectedBuildables);
            addOnSortRulesChangedDelegateBindings(connectedBuildables);

            checkTick_ = true;
            //checkFactoryTick_ = true;
        }
        else if (GetBuildTime() < GetWorld()->GetTimeSeconds())
        {
            SetActorTickEnabled(true);
            SetActorTickInterval(0);

            doUpdateItem = true;
        }
        else
        {
            updateRequested = GetWorld()->GetTimeSeconds();
            SetActorTickEnabled(true);
            SetActorTickInterval(0);
            mustUpdate_ = true;
        }
    }
}

void AEfficiencyCheckerBuilding::EndPlay(const EEndPlayReason::Type endPlayReason)
{
    Super::EndPlay(endPlayReason);

    // {
    //     SML::Logging::info(*getTagName(), TEXT("Removing from list!"));
    //
    //     FScopeLock ScopeLock(&AEfficiencyCheckerLogic::eclCritical);
    //     AEfficiencyCheckerLogic::allEfficiencyBuildings.Remove(this);
    // }

    if (HasAuthority())
    {
        if (innerPipelineAttachment && endPlayReason == EEndPlayReason::Destroyed)
        {
            auto attachmentPipeConnections = innerPipelineAttachment->GetPipeConnections();

            const auto pipeConnection1 = attachmentPipeConnections.Num() > 0 && attachmentPipeConnections[0]->IsConnected()
                                             ? attachmentPipeConnections[0]->GetPipeConnection()
                                             : nullptr;
            const auto pipeConnection2 = attachmentPipeConnections.Num() > 1 && attachmentPipeConnections[1]->IsConnected()
                                             ? attachmentPipeConnections[1]->GetPipeConnection()
                                             : nullptr;

            auto fluidType = pipeConnection1 ? pipeConnection1->GetFluidDescriptor() : nullptr;
            if (!fluidType)
            {
                fluidType = pipeConnection2 ? pipeConnection2->GetFluidDescriptor() : nullptr;
            }

            // Remove the connections
            for (auto connection : attachmentPipeConnections)
            {
                connection->ClearConnection();
            }

            if (pipeConnection1 && pipeConnection2 &&
                FVector::Dist(pipeConnection1->GetConnectorLocation(), pipeConnection2->GetConnectorLocation()) <= 1)
            {
                // Merge together

                TArray<AFGBuildablePipeline*> pipelines;
                pipelines.Add(Cast<AFGBuildablePipeline>(pipeConnection1->GetOwner()));
                pipelines.Add(Cast<AFGBuildablePipeline>(pipeConnection2->GetOwner()));

                // Merge the pipes
                AFGBuildablePipeline::Merge(pipelines);

                AFGPipeSubsystem::Get(GetWorld())->TrySetNetworkFluidDescriptor(innerPipelineAttachment->GetPipeConnections()[0]->GetPipeNetworkID(), fluidType);
            }

            innerPipelineAttachment->Destroy();
            innerPipelineAttachment = nullptr;
        }

        removeOnDestroyBindings(pendingBuildables);
        removeOnDestroyBindings(connectedBuildables);
        removeOnRecipeChangedBindings(connectedBuildables);
        removeOnSortRulesChangedDelegateBindings(connectedBuildables);

        pendingBuildables.Empty();
        connectedBuildables.Empty();
    }
}

void AEfficiencyCheckerBuilding::Tick(float dt)
{
    Super::Tick(dt);

    if (HasAuthority())
    {
        if (checkTick_)
        {
            SML::Logging::info(*getTagName(), TEXT("Ticking"));
        }

        if (doUpdateItem)
        {
            UpdateItem(injectedInput, limitedThroughput, requiredOutput, injectedItems);
            SetActorTickEnabled(false);
        }
        else if (lastUpdated < updateRequested && updateRequested <= GetWorld()->GetTimeSeconds())
        {
            auto playerIt = GetWorld()->GetPlayerControllerIterator();
            for (; playerIt; ++playerIt)
            {
                if (checkTick_)
                {
                    SML::Logging::info(*getTagName(), TEXT("Player Controller"));
                }

                const auto pawn = (*playerIt)->GetPawn();
                if (pawn)
                {
                    auto playerTranslation = pawn->GetActorLocation();

                    if (!(FEfficiencyCheckerModModule::autoUpdate && autoUpdateMode == EAutoUpdateType::AUT_USE_DEFAULT ||
                            autoUpdateMode == EAutoUpdateType::AUT_ENABLED) ||
                        FVector::Dist(playerTranslation, GetActorLocation()) <= FEfficiencyCheckerModModule::autoUpdateDistance)
                    {
                        SML::Logging::info(*getTagName(), TEXT("Last Tick"));
                        // SML::Logging::info(*getTagName(), TEXT("Player Pawn"));
                        // SML::Logging::info(*getTagName(), TEXT("Translation X = "), playerTranslation.X, TEXT(" / Y = "), playerTranslation.Y,TEXT( " / Z = "), playerTranslation.Z);

                        // Check if has pending buildings
                        if (!mustUpdate_)
                        {
                            if (connectedBuildables.Num())
                            {
                                for (auto pending : pendingBuildables)
                                {
                                    // If building connects to any of connections, it must be updated
                                    TInlineComponentArray<UFGFactoryConnectionComponent*> components;
                                    pending->GetComponents(components);

                                    for (auto component : components)
                                    {
                                        const auto connectionComponent = Cast<UFGFactoryConnectionComponent>(component);
                                        if (!connectionComponent->IsConnected())
                                        {
                                            continue;
                                        }

                                        if (connectedBuildables.Contains(Cast<AFGBuildable>(connectionComponent->GetConnection()->GetOwner())))
                                        {
                                            SML::Logging::info(
                                                *getTagName(),
                                                TEXT("New building "),
                                                playerTranslation.X,
                                                TEXT(" connected to known building "),
                                                *connectionComponent->GetConnection()->GetOwner()->GetPathName()
                                                );

                                            mustUpdate_ = true;
                                            break;
                                        }
                                    }

                                    if (mustUpdate_)
                                    {
                                        break;
                                    }
                                }
                            }
                            else
                            {
                                mustUpdate_ = true;
                            }
                        }

                        removeOnDestroyBindings(pendingBuildables);

                        pendingBuildables.Empty();

                        if (mustUpdate_)
                        {
                            // Recalculate connections
                            Server_UpdateConnectedProduction(true, false, 0, true, false, 0);
                        }

                        SetActorTickEnabled(false);
                        // mFactoryTickFunction.SetTickFunctionEnable(false);

                        break;
                    }
                }
            }
        }

        checkTick_ = false;
    }
}

// // Called every frame
// void AEfficiencyCheckerBuilding::Factory_Tick(float dt)
// {
//     Super::Factory_Tick(dt);
//
//     if (HasAuthority())
//     {
//         if (checkFactoryTick_)
//         {
//             SML::Logging::info(*getTagName(), TEXT("Factory Ticking"));
//             checkFactoryTick_ = false;
//         }
//
//         if (lastUpdated < updateRequested && updateRequested <= GetWorld()->GetTimeSeconds())
//         {
//             SML::Logging::info(*getTagName(), TEXT("Last Factory Tick"));
//
//             //Server_UpdateConnectedProduction(injectedInput, limitedThroughput, requiredOutput, injectedItem, dumpConnections);
//
//             //SetActorTickEnabled(false);
//             //mFactoryTickFunction.SetTickFunctionEnable(false);
//         }
//     }
// }

void AEfficiencyCheckerBuilding::SetCustomInjectedInput(bool enabled, float value)
{
    if (HasAuthority())
    {
        Server_SetCustomInjectedInput(enabled, value);
    }
    else
    {
        auto rco = UEfficiencyCheckerRCO::getRCO(GetWorld());
        if (rco)
        {
            if (FEfficiencyCheckerModModule::dumpConnections)
            {
                SML::Logging::info(*getTagName(), TEXT("Calling SetCustomInjectedInput at server"));
            }

            rco->SetCustomInjectedInputRPC(this, enabled, value);
        }
    }
}

void AEfficiencyCheckerBuilding::Server_SetCustomInjectedInput(bool enabled, float value)
{
    if (HasAuthority())
    {
        customInjectedInput = enabled;
        injectedInput = customInjectedInput ? value : 0;
    }
}

void AEfficiencyCheckerBuilding::SetCustomRequiredOutput(bool enabled, float value)
{
    if (HasAuthority())
    {
        Server_SetCustomRequiredOutput(enabled, value);
    }
    else
    {
        auto rco = UEfficiencyCheckerRCO::getRCO(GetWorld());
        if (rco)
        {
            if (FEfficiencyCheckerModModule::dumpConnections)
            {
                SML::Logging::info(*getTagName(), TEXT("Calling SetCustomRequiredOutput at server"));
            }

            rco->SetCustomRequiredOutputRPC(this, enabled, value);
        }
    }
}

void AEfficiencyCheckerBuilding::Server_SetCustomRequiredOutput(bool enabled, float value)
{
    if (HasAuthority())
    {
        customRequiredOutput = enabled;
        requiredOutput = customRequiredOutput ? value : 0;
    }
}

void AEfficiencyCheckerBuilding::SetAutoUpdateMode(EAutoUpdateType autoUpdateMode)
{
    if (HasAuthority())
    {
        Server_SetAutoUpdateMode(autoUpdateMode);
    }
    else
    {
        auto rco = UEfficiencyCheckerRCO::getRCO(GetWorld());
        if (rco)
        {
            if (FEfficiencyCheckerModModule::dumpConnections)
            {
                SML::Logging::info(*getTagName(), TEXT("Calling SetAutoUpdateMode at server"));
            }

            rco->SetAutoUpdateModeRPC(this, autoUpdateMode);
        }
    }
}

void AEfficiencyCheckerBuilding::Server_SetAutoUpdateMode(EAutoUpdateType autoUpdateMode)
{
    if (HasAuthority())
    {
        this->autoUpdateMode = autoUpdateMode;
    }
}

void AEfficiencyCheckerBuilding::UpdateBuilding(AFGBuildable* newBuildable)
{
    if (HasAuthority())
    {
        Server_UpdateBuilding(newBuildable);
    }
    else
    {
        auto rco = UEfficiencyCheckerRCO::getRCO(GetWorld());
        if (rco)
        {
            if (FEfficiencyCheckerModModule::dumpConnections)
            {
                SML::Logging::info(*getTagName(), TEXT("Calling UpdateBuilding at server"));
            }

            rco->UpdateBuildingRPC(this, newBuildable);
        }
    }
}

void AEfficiencyCheckerBuilding::Server_UpdateBuilding(AFGBuildable* newBuildable)
{
    if (HasAuthority())
    {
        if (FEfficiencyCheckerModModule::dumpConnections)
        {
            SML::Logging::info(*getTagName(),TEXT(" OnUpdateBuilding"));
        }

        if (newBuildable)
        {
            Server_AddPendingBuilding(newBuildable);
            // Trigger event to start listening for possible dismantle before checking the usage
            AddOnDestroyBinding(newBuildable);
        }

        // Trigger specific building
        updateRequested = GetWorld()->GetTimeSeconds() + FEfficiencyCheckerModModule::autoUpdateTimeout;
        // Give a 5 seconds timeout

        SML::Logging::info(*getTimeStamp(), TEXT("    Updating "), *GetName());

        SetActorTickEnabled(true);
        SetActorTickInterval(FEfficiencyCheckerModModule::autoUpdateTimeout);
        // mFactoryTickFunction.SetTickFunctionEnable(true);
        // mFactoryTickFunction.TickInterval = autoUpdateTimeout;

        checkTick_ = true;
        //checkFactoryTick_ = true;
        mustUpdate_ = true;
    }
    else
    {
        if (FEfficiencyCheckerModModule::dumpConnections)
        {
            SML::Logging::info(*getTagName(),TEXT(" OnUpdateBuilding - no authority"));
        }
    }

    if (FEfficiencyCheckerModModule::dumpConnections)
    {
        SML::Logging::info(TEXT("===="));
    }
}

void AEfficiencyCheckerBuilding::UpdateBuildings(AFGBuildable* newBuildable)
{
    SML::Logging::info(*getTimeStamp(),TEXT(" EfficiencyCheckerBuilding: UpdateBuildings"));

    if (newBuildable)
    {
        SML::Logging::info(*getTimeStamp(),TEXT("    New buildable: "), *newBuildable->GetName());

        //TArray<UActorComponent*> components = newBuildable->GetComponentsByClass(UFGFactoryConnectionComponent::StaticClass());
        //for (auto component : components) {
        //	UFGFactoryConnectionComponent* connectionComponent = Cast<UFGFactoryConnectionComponent>(component);

        //	if (connectionComponent->IsConnected()) {
        //		SML::Logging::info(*getTimeStamp(), TEXT("        - "),  *component->GetName(), TEXT(" is connected to"), * connectionComponent->GetConnection()->GetOwner()->GetName());
        //	}
        //	else {
        //		SML::Logging::info(*getTimeStamp(),TEXT( "        - "),  *component->GetName(), TEXT(" is not connected"));
        //	}
        //}
    }

    // Update all EfficiencyCheckerBuildings
    FScopeLock ScopeLock(&AEfficiencyCheckerLogic::singleton->eclCritical);

    // Trigger all buildings
    for (auto efficiencyBuilding : AEfficiencyCheckerLogic::singleton->allEfficiencyBuildings)
    {
        efficiencyBuilding->UpdateBuilding(newBuildable);
    }

    SML::Logging::info(TEXT("===="));
}

// UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "EfficiencyChecker")
void AEfficiencyCheckerBuilding::GetConnectedProduction
(
    float& out_injectedInput,
    float& out_limitedThroughput,
    float& out_requiredOutput,
    TSet<TSubclassOf<UFGItemDescriptor>>& out_injectedItems,
    TSet<AFGBuildable*>& connected
)
{
    if (FEfficiencyCheckerModModule::dumpConnections)
    {
        SML::Logging::info(*getTagName(), TEXT("GetConnectedProduction"));
    }

    const FString indent(TEXT("    "));

    const auto buildableSubsystem = AFGBuildableSubsystem::Get(GetWorld());

    class UFGConnectionComponent* inputConnector = nullptr;
    class UFGConnectionComponent* outputConnector = nullptr;
    //AFGBuildable* inputConveyor = nullptr;
    //AFGBuildable* outputConveyor = nullptr;

    TSet<TSubclassOf<UFGItemDescriptor>> restrictedItems;

    if (innerPipelineAttachment)
    {
        auto attachmentPipeConnections = innerPipelineAttachment->GetPipeConnections();

        inputConnector = attachmentPipeConnections.Num() > 0 ? attachmentPipeConnections[0] : nullptr;
        outputConnector = attachmentPipeConnections.Num() > 1 ? attachmentPipeConnections[1] : nullptr;

        TSubclassOf<UFGItemDescriptor> fluidItem;

        for (auto pipeConnection : attachmentPipeConnections)
        {
            fluidItem = pipeConnection->GetFluidDescriptor();

            if (fluidItem)
            {
                break;
            }
        }

        if (fluidItem)
        {
            restrictedItems.Add(fluidItem);
            out_injectedItems.Add(fluidItem);
        }
    }
    else if (resourceForm == EResourceForm::RF_SOLID)
    {
        auto anchorPoint = GetActorLocation() + FVector(0, 0, 100);

        if (FEfficiencyCheckerModModule::dumpConnections)
        {
            SML::Logging::info(*getTagName(), TEXT("Anchor point: X = "), anchorPoint.X,TEXT(" / Y = "), anchorPoint.Y, TEXT(" / Z = "), anchorPoint.Z);
        }

        AFGBuildableConveyorBelt* currentConveyor = nullptr;

        // TArray<AActor*> allBelts;
        //
        // if (IsInGameThread())
        // {
        //     UGameplayStatics::GetAllActorsOfClass(GetWorld(), AFGBuildableConveyorBelt::StaticClass(), allBelts);
        // }

        FScopeLock ScopeLock(&AEfficiencyCheckerLogic::singleton->eclCritical);

        for (auto conveyorActor : AEfficiencyCheckerLogic::singleton->allBelts)
        {
            auto conveyor = Cast<AFGBuildableConveyorBelt>(conveyorActor);

            if (conveyor->IsPendingKill() || currentConveyor && conveyor->GetBuildTime() < currentConveyor->GetBuildTime())
            {
                SML::Logging::info(*getTagName(), TEXT("Conveyor "), *conveyor->GetName(), anchorPoint.X,TEXT(" was skipped"));

                continue;
            }

            //FVector connection0Location = conveyor->GetConnection0()->GetConnectorLocation();
            //FVector connection1Location = conveyor->GetConnection1()->GetConnectorLocation();

            //SML::Logging::info(*getTagName(), TEXT("    connection 0: X = "), connection0Location.X,TEXT( " / Y = "), connection0Location.Y,TEXT( " / Z = "), connection0Location.Z);
            //SML::Logging::info(*getTagName(), TEXT("    connection 1: X = "), connection1Location.X, TEXT(" / Y = "), connection1Location.Y,TEXT( " / Z = "), connection1Location.Z);

            //if (!inputConveyor && FVector::PointsAreNear(connection0Location, anchorPoint, 1)) {
            //	SML::Logging::info(*getTagName(), TEXT("Found input "),  *conveyor->GetName());
            //	SML::Logging::info(*getTagName(),TEXT( "    connection 0: X = "), connection0Location.X,TEXT( " / Y = "), connection0Location.Y, TEXT(" / Z = "), connection0Location.Z);
            //	SML::Logging::info(*getTagName(),TEXT( "    distance = "), FVector::Dist(connection0Location, anchorPoint));

            //	inputConveyor = conveyor;
            //} else if (!outputConveyor && FVector::PointsAreNear(connection0Location, anchorPoint, 1)) {
            //	SML::Logging::info(*getTagName(), TEXT("Found output "),  *conveyor->GetName());
            //	SML::Logging::info(*getTagName(), TEXT("    connection 1: X = "), connection1Location.X, TEXT(" / Y = "), connection1Location.Y, TEXT(" / Z = "), connection1Location.Z);
            //	SML::Logging::info(*getTagName(), TEXT("    distance = "), FVector::Dist(connection1Location, anchorPoint));

            //	outputConveyor = conveyor;
            //}
            //else
            //{
            FVector nearestCoord;
            FVector direction;
            conveyor->GetLocationAndDirectionAtOffset(conveyor->FindOffsetClosestToLocation(anchorPoint), nearestCoord, direction);

            if (FVector::PointsAreNear(nearestCoord, anchorPoint, 50))
            {
                auto connection0Location = conveyor->GetConnection0()->GetConnectorLocation();
                auto connection1Location = conveyor->GetConnection1()->GetConnectorLocation();

                if (FEfficiencyCheckerModModule::dumpConnections)
                {
                    SML::Logging::info(*getTagName(), TEXT("Found intersecting conveyor "), *conveyor->GetName());
                    SML::Logging::info(
                        *getTagName(),
                        TEXT("    connection 0: X = "),
                        connection0Location.X,
                        TEXT(" / Y = "),
                        connection0Location.Y,
                        TEXT(" / Z = "),
                        connection0Location.Z
                        );
                    SML::Logging::info(
                        *getTagName(),
                        TEXT("    connection 1: X = "),
                        connection1Location.X,
                        TEXT(" / Y = "),
                        connection1Location.Y,
                        TEXT(" / Z = "),
                        connection1Location.Z
                        );
                    SML::Logging::info(
                        *getTagName(),
                        TEXT("    nearest location: X = "),
                        nearestCoord.X,
                        TEXT(" / Y = "),
                        nearestCoord.Y,
                        TEXT(" / Z = "),
                        nearestCoord.Z
                        );
                }

                currentConveyor = conveyor;
                inputConnector = conveyor->GetConnection0();
                outputConnector = conveyor->GetConnection1();
            }
            //}
            //
            //if (!inputConveyor && !outputConveyor &&
            //	FVector::Coincident(connection1Location - connection0Location, anchorPoint - connection0Location) &&
            //	FVector::Dist(anchorPoint, connection0Location) <= FVector::Dist(connection1Location, connection0Location)) {
            //	SML::Logging::info(*getTagName(), TEXT("Found input and output "),  *conveyor->GetName());
            //	SML::Logging::info(*getTagName(), TEXT("    connection 0: X = "), connection0Location.X, TEXT(" / Y = "), connection0Location.Y, TEXT(" / Z = "), connection0Location.Z);
            //	SML::Logging::info(*getTagName(), TEXT("    connection 1: X = "), connection1Location.X, TEXT(" / Y = "), connection1Location.Y, TEXT(" / Z = "), connection1Location.Z);
            //	SML::Logging::info(*getTagName(), TEXT("    coincident = "), (connection1Location - connection0Location) | (anchorPoint - connection0Location));
            //	SML::Logging::info(*getTagName(), TEXT("    connectors distance = "), FVector::Dist(connection1Location, connection0Location));
            //	SML::Logging::info(*getTagName(), TEXT("    anchor distance = "), FVector::Dist(anchorPoint, connection0Location));

            //	inputConveyor = conveyor;
            //	outputConveyor = conveyor;
            //}

            //if (inputConveyor && outputConveyor) {
            //	break;
            //}
        }

        if (inputConnector || outputConnector)
        {
            TArray<TSubclassOf<UFGItemDescriptor>> allItems;
            UFGBlueprintFunctionLibrary::Cheat_GetAllDescriptors(allItems);

            for (auto item : allItems)
            {
                if (!UFGBlueprintFunctionLibrary::CanBeOnConveyor(item) ||
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
    }
    else if ((resourceForm == EResourceForm::RF_LIQUID || resourceForm == EResourceForm::RF_GAS) && placementType == EPlacementType::PT_WALL)
    {
        auto anchorPoint = GetActorLocation();

        if (FEfficiencyCheckerModModule::dumpConnections)
        {
            SML::Logging::info(*getTagName(), TEXT("Anchor point: X = "), anchorPoint.X,TEXT(" / Y = "), anchorPoint.Y, TEXT(" / Z = "), anchorPoint.Z);
        }

        AFGBuildablePipeline* currentPipe = nullptr;

        // TArray<AActor*> allPipes;
        // if (IsInGameThread())
        // {
        //     UGameplayStatics::GetAllActorsOfClass(GetWorld(), AFGBuildablePipeline::StaticClass(), allPipes);
        // }

        FScopeLock ScopeLock(&AEfficiencyCheckerLogic::singleton->eclCritical);

        for (auto pipeActor : AEfficiencyCheckerLogic::singleton->allPipes)
        {
            auto pipe = Cast<AFGBuildablePipeline>(pipeActor);

            if (pipe->IsPendingKill() || currentPipe && pipe->GetBuildTime() < currentPipe->GetBuildTime())
            {
                SML::Logging::info(*getTagName(), TEXT("Conveyor "), *pipe->GetName(), anchorPoint.X,TEXT(" was skipped"));

                continue;
            }

            auto connection0Location = pipe->GetPipeConnection0()->GetConnectorLocation();
            auto connection1Location = pipe->GetPipeConnection1()->GetConnectorLocation();

            if (FVector::PointsAreNear(connection0Location, anchorPoint, 1) ||
                FVector::PointsAreNear(connection1Location, anchorPoint, 1))
            {
                if (FEfficiencyCheckerModModule::dumpConnections)
                {
                    SML::Logging::info(*getTagName(), TEXT("Found connected pipe "), *pipe->GetName());
                    SML::Logging::info(
                        *getTagName(),
                        TEXT("    connection 0: X = "),
                        connection0Location.X,
                        TEXT(" / Y = "),
                        connection0Location.Y,
                        TEXT(" / Z = "),
                        connection0Location.Z
                        );
                    SML::Logging::info(
                        *getTagName(),
                        TEXT("    connection 1: X = "),
                        connection1Location.X,
                        TEXT(" / Y = "),
                        connection1Location.Y,
                        TEXT(" / Z = "),
                        connection1Location.Z
                        );
                }

                currentPipe = pipe;
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
                    out_injectedItems.Add(fluidItem);
                }

                break;
            }
        }
    }

    float limitedThroughputIn = customInjectedInput ? injectedInput : 0;

    if (inputConnector)
    {
        TSet<AActor*> seenActors;

        AEfficiencyCheckerLogic::collectInput(
            resourceForm,
            injectedInput,
            inputConnector,
            out_injectedInput,
            limitedThroughputIn,
            seenActors,
            connected,
            out_injectedItems,
            restrictedItems,
            buildableSubsystem,
            0,
            indent
            );
    }

    float limitedThroughputOut = 0;

    if (outputConnector && !customRequiredOutput)
    {
        std::map<AActor*, TSet<TSubclassOf<UFGItemDescriptor>>> seenActors;

        AEfficiencyCheckerLogic::collectOutput(
            resourceForm,
            outputConnector,
            out_requiredOutput,
            limitedThroughputOut,
            seenActors,
            connected,
            out_injectedItems,
            buildableSubsystem,
            0,
            indent
            );
    }
    else
    {
        limitedThroughputOut = requiredOutput;
    }

    if (!inputConnector && !outputConnector && FEfficiencyCheckerModModule::dumpConnections)
    {
        if (resourceForm == EResourceForm::RF_SOLID)
        {
            SML::Logging::info(*getTagName(), TEXT("GetConnectedProduction: no intersecting belt"));
        }
        else
        {
            SML::Logging::info(*getTagName(), TEXT("GetConnectedProduction: no intersecting pipe"));
        }
    }

    out_limitedThroughput = min(limitedThroughputIn, limitedThroughputOut);

    if (FEfficiencyCheckerModModule::dumpConnections)
    {
        SML::Logging::info(TEXT("===="));
    }
}

void AEfficiencyCheckerBuilding::UpdateConnectedProduction
(
    const bool keepCustomInput,
    const bool hasCustomInjectedInput,
    float in_customInjectedInput,
    const bool keepCustomOutput,
    const bool hasCustomRequiredOutput,
    float in_customRequiredOutput
)
{
    if (HasAuthority())
    {
        Server_UpdateConnectedProduction(
            keepCustomInput,
            hasCustomInjectedInput,
            in_customInjectedInput,
            keepCustomOutput,
            hasCustomRequiredOutput,
            in_customRequiredOutput
            );
    }
    else
    {
        auto rco = UEfficiencyCheckerRCO::getRCO(GetWorld());
        if (rco)
        {
            if (FEfficiencyCheckerModModule::dumpConnections)
            {
                SML::Logging::info(*getTagName(), TEXT("Calling UpdateConnectedProduction at server"));
            }

            rco->UpdateConnectedProductionRPC(
                this,
                keepCustomInput,
                hasCustomInjectedInput,
                in_customInjectedInput,
                keepCustomOutput,
                hasCustomRequiredOutput,
                in_customRequiredOutput
                );
        }
    }
}

void AEfficiencyCheckerBuilding::Server_UpdateConnectedProduction
(
    const bool keepCustomInput,
    const bool hasCustomInjectedInput,
    float in_customInjectedInput,
    const bool keepCustomOutput,
    const bool hasCustomRequiredOutput,
    float in_customRequiredOutput
)
{
    if (HasAuthority())
    {
        if (FEfficiencyCheckerModModule::dumpConnections)
        {
            SML::Logging::info(*getTagName(), TEXT("Server_UpdateConnectedProduction"));
        }

        limitedThroughput = 0;

        const TSet<AFGBuildable*> connectionsToUnbind(connectedBuildables);

        connectedBuildables.Empty();

        if (!keepCustomInput)
        {
            customInjectedInput = hasCustomInjectedInput;

            if (customInjectedInput)
            {
                injectedInput = in_customInjectedInput;
            }
        }

        if (! customInjectedInput)
        {
            injectedInput = 0;
        }

        if (!keepCustomOutput)
        {
            customRequiredOutput = hasCustomRequiredOutput;

            if (customRequiredOutput)
            {
                requiredOutput = in_customRequiredOutput;
            }
        }

        if (!customRequiredOutput)
        {
            requiredOutput = 0;
        }

        TSet<TSubclassOf<UFGItemDescriptor>> injectedItemsSet;

        GetConnectedProduction(injectedInput, limitedThroughput, requiredOutput, injectedItemsSet, connectedBuildables);

        injectedItems = injectedItemsSet.Array();

        lastUpdated = GetWorld()->GetTimeSeconds();
        updateRequested = 0;

        SetActorTickEnabled(false);
        //mFactoryTickFunction.SetTickFunctionEnable(false);

        if (FEfficiencyCheckerModModule::autoUpdate && autoUpdateMode == EAutoUpdateType::AUT_USE_DEFAULT ||
            autoUpdateMode == EAutoUpdateType::AUT_ENABLED)
        {
            // Remove bindings for all that are on connectionsToUnbind but not on connectedBuildables
            const auto bindingsToRemove = connectionsToUnbind.Difference(connectedBuildables);
            removeOnDestroyBindings(bindingsToRemove);
            removeOnRecipeChangedBindings(bindingsToRemove);
            removeOnSortRulesChangedDelegateBindings(bindingsToRemove);

            // Add bindings for all that are on connectedBuildables but not on connectionsToUnbind
            const auto bindingsToAdd = connectedBuildables.Difference(connectionsToUnbind);
            addOnDestroyBindings(bindingsToAdd);
            addOnRecipeChangedBindings(bindingsToAdd);
            addOnSortRulesChangedDelegateBindings(bindingsToAdd);
        }

        UpdateItem(injectedInput, limitedThroughput, requiredOutput, injectedItems);
    }
    else
    {
        if (FEfficiencyCheckerModModule::dumpConnections)
        {
            SML::Logging::info(*getTagName(), TEXT("Server_UpdateConnectedProduction - no authority"));
        }
    }

    if (FEfficiencyCheckerModModule::dumpConnections)
    {
        SML::Logging::info(TEXT("===="));
    }
}

void AEfficiencyCheckerBuilding::addOnDestroyBindings(const TSet<AFGBuildable*>& buildings)
{
    for (auto building : buildings)
    {
        AddOnDestroyBinding(building);
    }
}

void AEfficiencyCheckerBuilding::removeOnDestroyBindings(const TSet<AFGBuildable*>& buildings)
{
    for (auto building : buildings)
    {
        RemoveOnDestroyBinding(building);
    }
}

void AEfficiencyCheckerBuilding::addOnSortRulesChangedDelegateBindings(const TSet<AFGBuildable*>& buildings)
{
    for (auto building : buildings)
    {
        const auto smart = Cast<AFGBuildableSplitterSmart>(building);
        if (smart)
        {
            AddOnSortRulesChangedDelegateBinding(smart);
        }
    }
}

void AEfficiencyCheckerBuilding::removeOnSortRulesChangedDelegateBindings(const TSet<AFGBuildable*>& buildings)
{
    for (auto building : buildings)
    {
        const auto smart = Cast<AFGBuildableSplitterSmart>(building);
        if (smart)
        {
            RemoveOnSortRulesChangedDelegateBinding(smart);
        }
    }
}

void AEfficiencyCheckerBuilding::addOnRecipeChangedBindings(const TSet<AFGBuildable*>& buildings)
{
    for (auto building : buildings)
    {
        const auto manufacturer = Cast<AFGBuildableManufacturer>(building);
        if (manufacturer)
        {
            AddOnRecipeChangedBinding(manufacturer);
        }
    }
}

void AEfficiencyCheckerBuilding::removeOnRecipeChangedBindings(const TSet<AFGBuildable*>& buildings)
{
    for (auto building : buildings)
    {
        const auto manufacturer = Cast<AFGBuildableManufacturer>(building);
        if (manufacturer)
        {
            RemoveOnRecipeChangedBinding(manufacturer);
        }
    }
}

void AEfficiencyCheckerBuilding::RemoveBuilding(AFGBuildable* buildable)
{
    if (HasAuthority())
    {
        Server_RemoveBuilding(buildable);
    }
    else
    {
        auto rco = UEfficiencyCheckerRCO::getRCO(GetWorld());
        if (rco)
        {
            if (FEfficiencyCheckerModModule::dumpConnections)
            {
                SML::Logging::info(*getTagName(), TEXT("Calling RemoveBuilding at server"));
            }

            rco->RemoveBuildingRPC(this, buildable);
        }
    }
}

void AEfficiencyCheckerBuilding::Server_RemoveBuilding(AFGBuildable* buildable)
{
    if (HasAuthority())
    {
        pendingBuildables.Remove(buildable);
        connectedBuildables.Remove(buildable);
    }
}

void AEfficiencyCheckerBuilding::GetEfficiencyCheckerSettings(bool& out_autoUpdate, bool& out_dumpConnections, float& out_autoUpdateTimeout, float& out_autoUpdateDistance)
{
    SML::Logging::info(*getTimeStamp(), TEXT(" EfficiencyCheckerBuilding: GetEfficiencyCheckerSettings"));

    out_autoUpdate = FEfficiencyCheckerModModule::autoUpdate;
    out_dumpConnections = FEfficiencyCheckerModModule::dumpConnections;
    out_autoUpdateTimeout = FEfficiencyCheckerModModule::autoUpdateTimeout;
    out_autoUpdateDistance = FEfficiencyCheckerModModule::autoUpdateDistance;
}

void AEfficiencyCheckerBuilding::setPendingPotentialCallback(class AFGBuildableFactory* buildable, float potential)
{
    SML::Logging::info(
        *getTimeStamp(),
        TEXT(" EfficiencyCheckerBuilding: SetPendingPotential of building "),
        *buildable->GetPathName(),
        TEXT(" to "),
        potential
        );

    // Update all EfficiencyCheckerBuildings that connects to this building
    FScopeLock ScopeLock(&AEfficiencyCheckerLogic::singleton->eclCritical);

    for (auto efficiencyBuilding : AEfficiencyCheckerLogic::singleton->allEfficiencyBuildings)
    {
        if (efficiencyBuilding->HasAuthority() && efficiencyBuilding->connectedBuildables.Contains(buildable))
        {
            efficiencyBuilding->Server_UpdateConnectedProduction(true, false, 0, true, false, 0);
        }
    }
}

void AEfficiencyCheckerBuilding::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME(AEfficiencyCheckerBuilding, injectedItems);

    DOREPLIFETIME(AEfficiencyCheckerBuilding, injectedInput);
    DOREPLIFETIME(AEfficiencyCheckerBuilding, customInjectedInput);

    DOREPLIFETIME(AEfficiencyCheckerBuilding, limitedThroughput);

    DOREPLIFETIME(AEfficiencyCheckerBuilding, requiredOutput);
    DOREPLIFETIME(AEfficiencyCheckerBuilding, customRequiredOutput);

    // DOREPLIFETIME(AEfficiencyCheckerBuilding, connectedBuildables);

    // DOREPLIFETIME(AEfficiencyCheckerBuilding, pendingBuildables);
}

void AEfficiencyCheckerBuilding::AddPendingBuilding(AFGBuildable* buildable)
{
    if (HasAuthority())
    {
        Server_AddPendingBuilding(buildable);
    }
    else
    {
        auto rco = UEfficiencyCheckerRCO::getRCO(GetWorld());
        if (rco)
        {
            if (FEfficiencyCheckerModModule::dumpConnections)
            {
                SML::Logging::info(*getTagName(), TEXT("Calling AddPendingBuilding at server"));
            }

            rco->AddPendingBuildingRPC(this, buildable);
        }
    }
}

void AEfficiencyCheckerBuilding::Server_AddPendingBuilding(AFGBuildable* buildable)
{
    if (HasAuthority())
    {
        pendingBuildables.Add(buildable);
    }
}

bool AEfficiencyCheckerBuilding::IsAutoUpdateEnabled()
{
    return FEfficiencyCheckerModModule::autoUpdate;
}

bool AEfficiencyCheckerBuilding::IsDumpConnectionsEnabled()
{
    return FEfficiencyCheckerModModule::dumpConnections;
}

float AEfficiencyCheckerBuilding::GetAutoUpdateTimeout()
{
    return FEfficiencyCheckerModModule::autoUpdateTimeout;
}

float AEfficiencyCheckerBuilding::GetAutoUpdateDistance()
{
    return FEfficiencyCheckerModModule::autoUpdateDistance;
}

void AEfficiencyCheckerBuilding::UpdateItem_Implementation
(
    float in_injectedInput,
    float in_limitedThroughput,
    float in_requiredOutput,
    const TArray<TSubclassOf<UFGItemDescriptor>>& in_injectedItems
)
{
    OnUpdateItem.Broadcast(in_injectedInput, in_limitedThroughput, in_requiredOutput, in_injectedItems);
}

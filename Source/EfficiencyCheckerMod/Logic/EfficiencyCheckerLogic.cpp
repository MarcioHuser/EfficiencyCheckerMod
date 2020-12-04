﻿// ReSharper disable CppUE4CodingStandardNamingViolationWarning
// ReSharper disable CommentTypo
// ReSharper disable IdentifierTypo

#include "EfficiencyCheckerLogic.h"
#include "EfficiencyCheckerModModule.h"

#include "FGBuildableConveyorAttachment.h"
#include "FGBuildableConveyorBase.h"
#include "FGBuildableConveyorBelt.h"
#include "FGBuildableDockingStation.h"
#include "FGBuildableFactory.h"
#include "FGBuildableGeneratorNuclear.h"
#include "FGBuildableManufacturer.h"
#include "FGBuildablePipeline.h"
#include "FGBuildableRailroadStation.h"
#include "FGBuildableResourceExtractor.h"
#include "FGBuildableSplitterSmart.h"
#include "FGBuildableStorage.h"
#include "FGBuildableTrainPlatformCargo.h"
#include "FGConnectionComponent.h"
#include "FGEquipmentAttachment.h"
#include "FGEquipmentDescriptor.h"
#include "FGGameMode.h"
#include "FGItemCategory.h"
#include "FGItemDescriptor.h"
#include "FGPipeConnectionComponent.h"
#include "FGRailroadSubsystem.h"
#include "FGRailroadTimeTable.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"

#include "SML/util/Logging.h"
#include "SML/util/ReflectionHelper.h"

#include "Util/Optimize.h"
#include "Util/Util.h"

#include <set>
#include <map>


#include "EfficiencyCheckerRCO.h"
#include "FGBuildablePipelinePump.h"

#ifndef OPTIMIZE
#pragma optimize( "", off )
#endif

// TSet<TSubclassOf<UFGItemDescriptor>> AEfficiencyCheckerLogic::nuclearWasteItemDescriptors;
// TSet<TSubclassOf<UFGItemDescriptor>> AEfficiencyCheckerLogic::noneItemDescriptors;
// TSet<TSubclassOf<UFGItemDescriptor>> AEfficiencyCheckerLogic::wildCardItemDescriptors;
// TSet<TSubclassOf<UFGItemDescriptor>> AEfficiencyCheckerLogic::anyUndefinedItemDescriptors;
// TSet<TSubclassOf<UFGItemDescriptor>> AEfficiencyCheckerLogic::overflowItemDescriptors;
//
// FCriticalSection AEfficiencyCheckerLogic::eclCritical;

AEfficiencyCheckerLogic* AEfficiencyCheckerLogic::singleton = nullptr;

// TSet<class AEfficiencyCheckerBuilding*> AEfficiencyCheckerLogic::allEfficiencyBuildings;

inline FString getEnumItemName(TCHAR* name, int value)
{
	FString valueStr;

	auto MyEnum = FindObject<UEnum>(ANY_PACKAGE, name);
	if (MyEnum)
	{
		MyEnum->AddToRoot();

		valueStr = MyEnum->GetDisplayNameTextByValue(value).ToString();
	}
	else
	{
		valueStr = TEXT("(Unknown)");
	}

	return FString::Printf(TEXT("%s (%d)"), *valueStr, value);
}

void AEfficiencyCheckerLogic::Initialize
(
	const TSet<TSubclassOf<UFGItemDescriptor>>& in_noneItemDescriptors,
	const TSet<TSubclassOf<UFGItemDescriptor>>& in_wildcardItemDescriptors,
	const TSet<TSubclassOf<UFGItemDescriptor>>& in_anyUndefinedItemDescriptors,
	const TSet<TSubclassOf<UFGItemDescriptor>>& in_overflowItemDescriptors,
	const TSet<TSubclassOf<UFGItemDescriptor>>& in_nuclearWasteItemDescriptors
)
{
	singleton = this;

	noneItemDescriptors = in_noneItemDescriptors;
	wildCardItemDescriptors = in_wildcardItemDescriptors;
	anyUndefinedItemDescriptors = in_anyUndefinedItemDescriptors;
	overflowItemDescriptors = in_overflowItemDescriptors;
	nuclearWasteItemDescriptors = in_nuclearWasteItemDescriptors;

	removeEffiencyBuildingDelegate.BindDynamic(this, &AEfficiencyCheckerLogic::removeEfficiencyBuilding);
	removeBeltDelegate.BindDynamic(this, &AEfficiencyCheckerLogic::removeBelt);
	removePipeDelegate.BindDynamic(this, &AEfficiencyCheckerLogic::removePipe);
	removeTeleporterDelegate.BindDynamic(this, &AEfficiencyCheckerLogic::removeTeleporter);

	auto subsystem = AFGBuildableSubsystem::Get(this);

	if (subsystem)
	{
		FScopeLock ScopeLock(&eclCritical);

		TArray<AActor*> allBuildables;
		UGameplayStatics::GetAllActorsOfClass(subsystem->GetWorld(), AFGBuildable::StaticClass(), allBuildables);

		for (auto buildableActor : allBuildables)
		{
			IsValidBuildable(Cast<AFGBuildable>(buildableActor));
		}

		auto gameMode = Cast<AFGGameMode>(UGameplayStatics::GetGameMode(subsystem->GetWorld()));
		if (gameMode)
		{
			gameMode->RegisterRemoteCallObjectClass(UEfficiencyCheckerRCO::StaticClass());
		}
	}
}

void AEfficiencyCheckerLogic::Terminate()
{
	FScopeLock ScopeLock(&eclCritical);
	allEfficiencyBuildings.Empty();
	allBelts.Empty();
	allPipes.Empty();
	allTeleporters.Empty();

	singleton = nullptr;
}

bool AEfficiencyCheckerLogic::containsActor(const std::map<AActor*, TSet<TSubclassOf<UFGItemDescriptor>>>& seenActors, AActor* actor)
{
	return seenActors.find(actor) != seenActors.end();
}

bool AEfficiencyCheckerLogic::actorContainsItem
(
	const std::map<AActor*, TSet<TSubclassOf<UFGItemDescriptor>>>& seenActors,
	AActor* actor,
	const TSubclassOf<UFGItemDescriptor>& item
)
{
	auto it = seenActors.find(actor);
	if (it == seenActors.end())
	{
		return false;
	}

	return it->second.Contains(item);
}

void AEfficiencyCheckerLogic::addAllItemsToActor
(
	std::map<AActor*, TSet<TSubclassOf<UFGItemDescriptor>>>& seenActors,
	AActor* actor,
	const TSet<TSubclassOf<UFGItemDescriptor>>& items
)
{
	// Ensure the actor exists, even with an empty list
	seenActors[actor];

	for (auto item : items)
	{
		seenActors[actor].Add(item);
	}
}

void AEfficiencyCheckerLogic::collectInput
(
	EResourceForm resourceForm,
	bool customInjectedInput,
	class UFGConnectionComponent* connector,
	float& out_injectedInput,
	float& out_limitedThroughput,
	TSet<AActor*>& seenActors,
	TSet<class AFGBuildable*>& connected,
	TSet<TSubclassOf<UFGItemDescriptor>>& out_injectedItems,
	const TSet<TSubclassOf<UFGItemDescriptor>>& in_restrictItems,
	class AFGBuildableSubsystem* buildableSubsystem,
	int level,
	bool& overflow,
	const FString& indent
)
{
	TSet<TSubclassOf<UFGItemDescriptor>> restrictItems = in_restrictItems;

	for (;;)
	{
		if (!connector)
		{
			return;
		}

		auto owner = connector->GetOwner();

		if (!owner || seenActors.Contains(owner))
		{
			return;
		}

		const auto fullClassName = owner->GetClass()->GetPathName();

		if (level > 100)
		{
			SML::Logging::error(
				*getTimeStamp(),
				*indent,
				TEXT("collectInput: level is too deep: "),
				level,
				TEXT("; "),
				*owner->GetName(),
				TEXT(" / "),
				*fullClassName
				);

			overflow = true;

			return;
		}

		if (FEfficiencyCheckerModModule::dumpConnections)
		{
			SML::Logging::info(
				*getTimeStamp(),
				*indent,
				TEXT("collectInput at level "),
				level,
				TEXT(": "),
				*owner->GetName(),
				TEXT(" / "),
				*fullClassName
				);
		}

		seenActors.Add(owner);

		{
			const auto manufacturer = Cast<AFGBuildableManufacturer>(owner);
			if (manufacturer)
			{
				const auto recipeClass = manufacturer->GetCurrentRecipe();

				if (recipeClass)
				{
					auto products = UFGRecipe::GetProducts(recipeClass);

					for (auto item : products)
					{
						auto itemForm = UFGItemDescriptor::GetForm(item.ItemClass);

						if (itemForm == EResourceForm::RF_SOLID && resourceForm != EResourceForm::RF_SOLID ||
							(itemForm == EResourceForm::RF_LIQUID || itemForm == EResourceForm::RF_GAS) &&
							resourceForm != EResourceForm::RF_LIQUID && resourceForm != EResourceForm::RF_GAS ||
							!restrictItems.Contains(item.ItemClass))
						{
							continue;
						}

						out_injectedItems.Add(item.ItemClass);

						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							SML::Logging::info(*getTimeStamp(), *indent, TEXT("Item amount = "), item.Amount);
							SML::Logging::info(*getTimeStamp(), *indent, TEXT("Current potential = "), manufacturer->GetCurrentPotential());
							SML::Logging::info(*getTimeStamp(), *indent, TEXT("Pending potential = "), manufacturer->GetPendingPotential());
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								TEXT("Production cycle time = "),
								manufacturer->CalcProductionCycleTimeForPotential(manufacturer->GetPendingPotential())
								);
							SML::Logging::info(*getTimeStamp(), *indent, TEXT("Recipe duration = "), UFGRecipe::GetManufacturingDuration(recipeClass));
						}

						float itemAmountPerMinute = item.Amount * (60.0 / manufacturer->CalcProductionCycleTimeForPotential(manufacturer->GetPendingPotential()));

						if (resourceForm == EResourceForm::RF_LIQUID || resourceForm == EResourceForm::RF_GAS)
						{
							itemAmountPerMinute /= 1000;
						}

						// if (fullClassName.StartsWith(TEXT("/Game/MK22k20/Buildable")))
						// {
						//     if (fullClassName.EndsWith(TEXT("Mk2_C")))
						//     {
						//         itemAmountPerMinute *= 1.5;
						//     }
						//     else if (fullClassName.EndsWith(TEXT("Mk3_C")))
						//     {
						//         itemAmountPerMinute *= 2;
						//     }
						//     else if (fullClassName.EndsWith(TEXT("Mk4_C")))
						//     {
						//         itemAmountPerMinute *= 2.5;
						//     }
						// }
						// else if (fullClassName.StartsWith(TEXT("/Game/FarmingMod/Buildable")))
						// {
						//     if (fullClassName.EndsWith(TEXT("Mk2_C")))
						//     {
						//         itemAmountPerMinute *= 2;
						//     }
						//     else if (fullClassName.EndsWith(TEXT("Mk3_C")))
						//     {
						//         itemAmountPerMinute *= 3;
						//     }
						// }


						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								*manufacturer->GetName(),
								TEXT(" produces "),
								itemAmountPerMinute,
								TEXT(" "),
								*UFGItemDescriptor::GetItemName(item.ItemClass).ToString(),
								TEXT("/minute")
								);
						}

						if (!customInjectedInput)
						{
							out_injectedInput += itemAmountPerMinute;
						}

						break;
					}
				}

				connected.Add(manufacturer);

				return;
			}
		}

		{
			const auto extractor = Cast<AFGBuildableResourceExtractor>(owner);
			if (extractor)
			{
				TSubclassOf<UFGItemDescriptor> item;

				if (!item)
				{
					const auto resource = extractor->GetExtractableResource();
					if (resource)
					{
						const auto resourceObj = resource.GetObject();

						item = IFGExtractableResourceInterface::Execute_GetResourceClass(resourceObj);

						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								TEXT("Extraction Speed Multiplier = "),
								IFGExtractableResourceInterface::Execute_GetExtractionSpeedMultiplier(resourceObj)
								);
						}
					}
					else
					{
						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							SML::Logging::info(*getTimeStamp(), *indent, TEXT("Extractable resource is null"));
						}
					}
				}

				if (!item)
				{
					item = extractor->GetOutputInventory()->GetAllowedItemOnIndex(0);
				}

				if (!item || !restrictItems.Contains(item))
				{
					return;
				}

				if (FEfficiencyCheckerModModule::dumpConnections)
				{
					SML::Logging::info(*getTimeStamp(), *indent, TEXT("Resource name = "), *UFGItemDescriptor::GetItemName(item).ToString());
				}

				out_injectedItems.Add(item);

				if (FEfficiencyCheckerModModule::dumpConnections)
				{
					SML::Logging::info(*getTimeStamp(), *indent, TEXT("Current potential = "), extractor->GetCurrentPotential());
					SML::Logging::info(*getTimeStamp(), *indent, TEXT("Pending potential = "), extractor->GetPendingPotential());
					SML::Logging::info(
						*getTimeStamp(),
						*indent,
						TEXT("Production cycle time = "),
						extractor->CalcProductionCycleTimeForPotential(extractor->GetPendingPotential())
						);
					SML::Logging::info(*getTimeStamp(), *indent, TEXT("Items per cycle converted = "), extractor->GetNumExtractedItemsPerCycleConverted());
					SML::Logging::info(*getTimeStamp(), *indent, TEXT("Items per cycle = "), extractor->GetNumExtractedItemsPerCycle());
				}

				float itemAmountPerMinute = extractor->GetNumExtractedItemsPerCycle() * (60.0 / extractor->CalcProductionCycleTimeForPotential(extractor->GetPendingPotential()));

				if (resourceForm == EResourceForm::RF_LIQUID || resourceForm == EResourceForm::RF_GAS)
				{
					itemAmountPerMinute /= 1000;
				}

				if (fullClassName == TEXT("/Game/Miner_Mk4/Build_MinerMk4.Build_MinerMk4_C"))
				{
					itemAmountPerMinute = 2000;
				}

				// if (fullClassName.StartsWith(TEXT("/Game/MK22k20/Buildable")))
				// {
				//     if (fullClassName.EndsWith(TEXT("MK2_C")))
				//     {
				//         itemAmountPerMinute *= 1.5;
				//     }
				//     else if (fullClassName.EndsWith(TEXT("MK3_C")))
				//     {
				//         itemAmountPerMinute *= 2;
				//     }
				//     else if (fullClassName.EndsWith(TEXT("MK4_C")))
				//     {
				//         itemAmountPerMinute *= 2.5;
				//     }
				// }

				if (FEfficiencyCheckerModModule::dumpConnections)
				{
					SML::Logging::info(
						*getTimeStamp(),
						*indent,
						*extractor->GetName(),
						TEXT(" extracts "),
						itemAmountPerMinute,
						TEXT(" "),
						*UFGItemDescriptor::GetItemName(item).ToString(),
						TEXT("/minute")
						);
				}

				if (!customInjectedInput)
				{
					out_injectedInput += itemAmountPerMinute;
				}

				connected.Add(extractor);

				return;
			}
		}

		if (resourceForm == EResourceForm::RF_SOLID)
		{
			const auto conveyor = Cast<AFGBuildableConveyorBase>(owner);
			if (conveyor)
			{
				// The innitial limit for a belt is its own speed
				// out_limitedThroughput = conveyor->GetSpeed() / 2;

				// const auto conveyorInput = conveyor->GetConnection0();
				// if (conveyorInput && conveyorInput->IsConnected())
				// {
				//     float previousLimit = out_limitedThroughput;
				//     collectInput(
				//         resourceForm,
				//         customInjectedInput,
				//         conveyorInput->GetConnection(),
				//         out_injectedInput,
				//         previousLimit,
				//         seenActors,
				//         connected,
				//         out_injectedItems,
				//         restrictItems,
				//         buildableSubsystem,
				//         level + 1,
				//         indent + TEXT("    ")
				//         );
				//
				//     out_limitedThroughput = FMath::Min(out_limitedThroughput, previousLimit);
				// }

				connected.Add(conveyor);

				connector = conveyor->GetConnection0()->GetConnection();

				out_limitedThroughput = FMath::Min(out_limitedThroughput, conveyor->GetSpeed() / 2);

				if (FEfficiencyCheckerModModule::dumpConnections)
				{
					SML::Logging::info(*getTimeStamp(), *indent, *conveyor->GetName(), TEXT(" limited at "), out_limitedThroughput, TEXT(" items/minute"));
				}

				continue;
			}

			AFGBuildableStorage* storageContainer = nullptr;
			AFGBuildableTrainPlatformCargo* cargoPlatform = nullptr;
			AFGBuildableConveyorAttachment* conveyorAttachment = nullptr;
			AFGBuildableDockingStation* dockingStation = nullptr;
			AFGBuildableFactory* storageTeleporter = nullptr;

			AFGBuildableFactory* buildable = conveyorAttachment = Cast<AFGBuildableConveyorAttachment>(owner);
			if (!buildable)
			{
				buildable = storageContainer = Cast<AFGBuildableStorage>(owner);
			}

			if (!buildable)
			{
				cargoPlatform = Cast<AFGBuildableTrainPlatformCargo>(owner);
				if (cargoPlatform)
				{
					buildable = cargoPlatform;

					TArray<FInventoryStack> stacks;

					cargoPlatform->GetInventory()->GetInventoryStacks(stacks);

					for (auto stack : stacks)
					{
						if (!restrictItems.Contains(stack.Item.ItemClass))
						{
							continue;
						}

						out_injectedItems.Add(stack.Item.ItemClass);
					}
				}
			}

			if (!buildable)
			{
				dockingStation = Cast<AFGBuildableDockingStation>(owner);
				if (dockingStation)
				{
					buildable = dockingStation;

					TArray<FInventoryStack> stacks;

					dockingStation->GetInventory()->GetInventoryStacks(stacks);

					for (auto stack : stacks)
					{
						if (!restrictItems.Contains(stack.Item.ItemClass))
						{
							continue;
						}

						out_injectedItems.Add(stack.Item.ItemClass);
					}
				}
			}

			if (!FEfficiencyCheckerModModule::ignoreStorageTeleporter &&
				!buildable && fullClassName == TEXT("/Game/StorageTeleporter/Buildables/ItemTeleporter/ItemTeleporter_Build.ItemTeleporter_Build_C"))
			{
				buildable = storageTeleporter = Cast<AFGBuildableFactory>(owner);
			}

			if (buildable)
			{
				auto components = buildable->GetConnectionComponents();

				if (cargoPlatform)
				{
					auto trackId = cargoPlatform->GetTrackGraphID();

					auto railroadSubsystem = AFGRailroadSubsystem::Get(owner->GetWorld());

					// Determine offsets from all the connected stations
					std::set<int> stationOffsets;
					TSet<AFGBuildableRailroadStation*> destinationStations;

					for (auto i = 0; i <= 1; i++)
					{
						auto offsetDistance = 1;

						for (auto connectedPlatform = cargoPlatform->GetConnectedPlatformInDirectionOf(i);
						     connectedPlatform;
						     connectedPlatform = connectedPlatform->GetConnectedPlatformInDirectionOf(i),
						     ++offsetDistance)
						{
							if (FEfficiencyCheckerModModule::dumpConnections)
							{
								SML::Logging::info(
									*getTimeStamp(),
									*indent,
									*connectedPlatform->GetName(),
									TEXT(" direction = "),
									i,
									TEXT(" / orientation reversed = "),
									connectedPlatform->IsOrientationReversed() ? TEXT("true") : TEXT("false")
									);
							}

							auto station = Cast<AFGBuildableRailroadStation>(connectedPlatform);
							if (station)
							{
								destinationStations.Add(station);

								if (FEfficiencyCheckerModModule::dumpConnections)
								{
									SML::Logging::info(
										*getTimeStamp(),
										*indent,
										TEXT("    Station = "),
										*station->GetStationIdentifier()->GetStationName().ToString()
										);
								}

								if (i == 0 && connectedPlatform->IsOrientationReversed() ||
									i == 1 && !connectedPlatform->IsOrientationReversed())
								{
									stationOffsets.insert(offsetDistance);
									if (FEfficiencyCheckerModModule::dumpConnections)
									{
										SML::Logging::info(*getTimeStamp(), *indent, TEXT("        offset distance = "), offsetDistance);
									}
								}
								else
								{
									stationOffsets.insert(-offsetDistance);
									if (FEfficiencyCheckerModModule::dumpConnections)
									{
										SML::Logging::info(*getTimeStamp(), *indent, TEXT("        offset distance = "), -offsetDistance);
									}
								}
							}

							if (FEfficiencyCheckerModModule::dumpConnections)
							{
								auto cargo = Cast<AFGBuildableTrainPlatformCargo>(connectedPlatform);
								if (cargo)
								{
									SML::Logging::info(
										*getTimeStamp(),
										*indent,
										TEXT("    Load mode = "),
										cargo->GetIsInLoadMode() ? TEXT("true") : TEXT("false")
										);
								}
							}
						}
					}

					TArray<AFGTrain*> trains;
					railroadSubsystem->GetTrains(trackId, trains);

					for (auto train : trains)
					{
						if (!train->HasTimeTable())
						{
							continue;
						}

						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							if (!train->GetTrainName().IsEmpty())
							{
								SML::Logging::info(
									*getTimeStamp(),
									*indent,
									TEXT("Train = "),
									*train->GetTrainName().ToString()
									);
							}
							else
							{
								SML::Logging::info(
									*getTimeStamp(),
									*indent,
									TEXT("Anonymous Train")
									);
							}
						}

						// Get train stations
						auto timeTable = train->GetTimeTable();

						TArray<FTimeTableStop> stops;
						timeTable->GetStops(stops);

						bool stopAtStations = false;

						for (auto stop : stops)
						{
							if (!stop.Station || !stop.Station->GetStation() || !destinationStations.Contains(stop.Station->GetStation()))
							{
								continue;
							}

							stopAtStations = true;

							break;
						}

						if (!stopAtStations)
						{
							continue;
						}

						for (auto stop : stops)
						{
							if (!stop.Station || !stop.Station->GetStation())
							{
								continue;
							}

							if (FEfficiencyCheckerModModule::dumpConnections)
							{
								SML::Logging::info(
									*getTimeStamp(),
									*indent,
									TEXT("    Stop = "),
									*stop.Station->GetStationName().ToString()
									);
							}

							for (auto i = 0; i <= 1; i++)
							{
								auto offsetDistance = 1;

								for (auto connectedPlatform = stop.Station->GetStation()->GetConnectedPlatformInDirectionOf(i);
								     connectedPlatform;
								     connectedPlatform = connectedPlatform->GetConnectedPlatformInDirectionOf(i),
								     ++offsetDistance)
								{
									auto stopCargo = Cast<AFGBuildableTrainPlatformCargo>(connectedPlatform);
									if (!stopCargo || stopCargo == cargoPlatform)
									{
										// Not a cargo or the same as the current one. Skip
										continue;
									}

									auto adjustedOffsetDistance = i == 0 && !stop.Station->GetStation()->IsOrientationReversed()
									                              || i == 1 && stop.Station->GetStation()->IsOrientationReversed()
										                              ? offsetDistance
										                              : -offsetDistance;

									if (stationOffsets.find(adjustedOffsetDistance) == stationOffsets.end())
									{
										// Not on a valid offset. Skip
										continue;
									}

									seenActors.Add(stopCargo);

									components.Append(
										stopCargo->GetConnectionComponents().FilterByPredicate(
											[&components, stopCargo](UFGFactoryConnectionComponent* connection)
											{
												return !components.Contains(connection) &&
													(stopCargo->GetIsInLoadMode() || connection->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT);
											}
											)
										);
								}
							}
						}
					}
				}

				if (storageTeleporter)
				{
					// Find all others of the same type
					auto currentStorageID = FReflectionHelper::GetPropertyValue<UStrProperty>(storageTeleporter, TEXT("StorageID"));

					// TArray<AActor*> allTeleporters;
					// if (IsInGameThread())
					// {
					//     UGameplayStatics::GetAllActorsOfClass(storageTeleporter->GetWorld(), storageTeleporter->GetClass(), allTeleporters);
					// }

					FScopeLock ScopeLock(&singleton->eclCritical);

					for (auto testTeleporter : AEfficiencyCheckerLogic::singleton->allTeleporters)
					{
						if (testTeleporter->IsPendingKill() || testTeleporter == storageTeleporter)
						{
							continue;
						}

						auto storageID = FReflectionHelper::GetPropertyValue<UStrProperty>(testTeleporter, TEXT("StorageID"));
						if (storageID == currentStorageID)
						{
							seenActors.Add(testTeleporter);

							auto factory = Cast<AFGBuildableFactory>(testTeleporter);
							if (factory)
							{
								components.Append(
									factory->GetConnectionComponents().FilterByPredicate(
										[&components](UFGFactoryConnectionComponent* connection)
										{
											return !components.Contains(connection); // Not in use already
										}
										)
									);
							}
						}
					}
				}

				int currentOutputIndex = -1;
				std::map<int, TSet<TSubclassOf<UFGItemDescriptor>>> restrictedItemsByOutput;

				// Filter items
				auto smartSplitter = Cast<AFGBuildableSplitterSmart>(buildable);
				if (smartSplitter)
				{
					for (int connectorIndex = 0; connectorIndex < components.Num(); connectorIndex++)
					{
						auto connection = components[connectorIndex];

						if (connection->GetConnector() != EFactoryConnectionConnector::FCC_CONVEYOR)
						{
							continue;
						}

						if (connection->GetDirection() != EFactoryConnectionDirection::FCD_OUTPUT)
						{
							continue;
						}

						auto outputIndex = connection->GetName()[connection->GetName().Len() - 1] - '1';

						if (connection == connector)
						{
							currentOutputIndex = outputIndex;
						}
					}

					// Already restricted. Restrict further
					for (int x = 0; x < smartSplitter->GetNumSortRules(); ++x)
					{
						auto rule = smartSplitter->GetSortRuleAt(x);

						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								TEXT("Rule "),
								x,
								TEXT(" / output index = "),
								rule.OutputIndex,
								TEXT(" / item = "),
								*UFGItemDescriptor::GetItemName(rule.ItemClass).ToString(),
								TEXT(" / class = "),
								*rule.ItemClass->GetPathName()
								);
						}

						restrictedItemsByOutput[rule.OutputIndex].Add(rule.ItemClass);
					}

					TSet<TSubclassOf<UFGItemDescriptor>> definedItems;

					// First pass
					for (auto it = restrictedItemsByOutput.begin(); it != restrictedItemsByOutput.end(); ++it)
					{
						if (singleton->noneItemDescriptors.Intersect(it->second).Num())
						{
							// No item is valid. Empty it all
							it->second.Empty();
						}
						else if (singleton->wildCardItemDescriptors.Intersect(it->second).Num() || singleton->overflowItemDescriptors.Intersect(it->second).Num())
						{
							// Add all current restrictItems as valid items
							it->second = restrictItems;
						}

						for (auto item : it->second)
						{
							definedItems.Add(item);
						}
					}

					for (auto it = restrictedItemsByOutput.begin(); it != restrictedItemsByOutput.end(); ++it)
					{
						if (singleton->anyUndefinedItemDescriptors.Intersect(it->second).Num())
						{
							it->second = it->second.Union(restrictItems.Difference(definedItems));
						}

						if (it->first == currentOutputIndex && !it->second.Num())
						{
							// Can't go further. Return
							return;
						}
					}
				}

				TArray<UFGFactoryConnectionComponent*> connectedInputs, connectedOutputs;
				for (auto connection : components)
				{
					if (!connection->IsConnected())
					{
						continue;
					}

					if (connection->GetConnector() != EFactoryConnectionConnector::FCC_CONVEYOR)
					{
						continue;
					}

					if (connection->GetDirection() == EFactoryConnectionDirection::FCD_INPUT)
					{
						connectedInputs.Add(connection);
					}
					else if (connection->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT)
					{
						connectedOutputs.Add(connection);
					}
				}

				if (connectedInputs.Num() == 1 && connectedOutputs.Num() == 1)
				{
					connected.Add(buildable);

					connector = connectedInputs[0]->GetConnection();

					if (FEfficiencyCheckerModModule::dumpConnections)
					{
						SML::Logging::info(*getTimeStamp(), *indent, *buildable->GetName(), TEXT(" skipped"));
					}

					if (smartSplitter)
					{
						auto outputIndex = connectedOutputs[0]->GetName()[connectedOutputs[0]->GetName().Len() - 1] - '1';

						restrictItems = restrictedItemsByOutput[outputIndex];
					}

					continue;
				}

				if (connectedInputs.Num() == 0)
				{
					// Nothing is being inputed. Bail
					SML::Logging::info(*getTimeStamp(), *indent, *buildable->GetName(), TEXT(" has no input"));
				}
				else
				{
					bool firstConnection = true;
					float limitedThroughput = 0;

					for (auto connection : components)
					{
						if (!connection->IsConnected())
						{
							continue;
						}

						if (connection->GetConnector() != EFactoryConnectionConnector::FCC_CONVEYOR)
						{
							continue;
						}

						if (connection->GetDirection() != EFactoryConnectionDirection::FCD_INPUT)
						{
							continue;
						}

						if (dockingStation && connection->GetName().Equals(TEXT("Input0"), ESearchCase::IgnoreCase))
						{
							continue;
						}

						float previousLimit = out_limitedThroughput;
						collectInput(
							resourceForm,
							customInjectedInput,
							connection->GetConnection(),
							out_injectedInput,
							previousLimit,
							seenActors,
							connected,
							out_injectedItems,
							currentOutputIndex < 0 ? restrictItems : restrictedItemsByOutput[currentOutputIndex],
							buildableSubsystem,
							level + 1,
							overflow,
							indent + TEXT("    ")
							);

						if (firstConnection)
						{
							limitedThroughput = previousLimit;
							firstConnection = false;
						}
						else
						{
							limitedThroughput += previousLimit;
						}
					}

					out_limitedThroughput = FMath::Min(out_limitedThroughput, limitedThroughput);

					for (int connectorIndex = 0; connectorIndex < components.Num(); connectorIndex++)
					{
						auto connection = components[connectorIndex];

						if (connection == connector)
						{
							continue;
						}

						if (!connection->IsConnected())
						{
							continue;
						}

						if (connection->GetDirection() != EFactoryConnectionDirection::FCD_OUTPUT)
						{
							continue;
						}

						auto outputIndex = connection->GetName()[connection->GetName().Len() - 1] - '1';

						float previousLimit = 0;
						float discountedInput = 0;
						std::map<AActor*, TSet<TSubclassOf<UFGItemDescriptor>>> seenActorsCopy;

						auto tempInjectedItems = out_injectedItems;

						if (currentOutputIndex >= 0)
						{
							tempInjectedItems = tempInjectedItems.Intersect(restrictedItemsByOutput[outputIndex]);
						}

						for (auto actor : seenActors)
						{
							seenActorsCopy[actor] = out_injectedItems;
						}

						collectOutput(
							resourceForm,
							connection->GetConnection(),
							discountedInput,
							previousLimit,
							seenActorsCopy,
							connected,
							tempInjectedItems,
							buildableSubsystem,
							level + 1,
							overflow,
							indent + TEXT("    ")
							);

						if (discountedInput > 0)
						{
							if (FEfficiencyCheckerModModule::dumpConnections)
							{
								SML::Logging::info(*getTimeStamp(), *indent, TEXT("Discounting "), discountedInput, TEXT(" items/minute"));
							}

							if (!customInjectedInput)
							{
								out_injectedInput -= discountedInput;
							}
						}
					}

					if (dockingStation /*|| cargoPlatform*/)
					{
						out_injectedInput += out_limitedThroughput;
					}

					if (FEfficiencyCheckerModModule::dumpConnections)
					{
						SML::Logging::info(*getTimeStamp(), *indent, *buildable->GetName(), TEXT(" limited at "), out_limitedThroughput, TEXT(" items/minute"));
					}
				}

				connected.Add(buildable);

				return;
			}
		}

		if (resourceForm == EResourceForm::RF_LIQUID || resourceForm == EResourceForm::RF_GAS)
		{
			auto pipeline = Cast<AFGBuildablePipeline>(owner);
			// if (pipeline)
			// {
			//     // out_limitedThroughput = UFGBlueprintFunctionLibrary::RoundFloatWithPrecision(pipeline->GetFlowLimit() * 60, 4);
			//     // // out_limitedThroughput = pipeline->mFlowLimit * 60;
			//     //
			//     // //out_limitedThroughput = 300;
			//     //
			//     // auto components = pipeline->GetPipeConnections();
			//     //
			//     // for (auto connection : components)
			//     // {
			//     //     if (!connection->IsConnected() ||
			//     //         connection->GetPipeConnection()->GetPipeConnectionType() == EPipeConnectionType::PCT_CONSUMER /*||
			//     //     connection == connector*/)
			//     //     {
			//     //         continue;
			//     //     }
			//     //
			//     //     float previousLimit = out_limitedThroughput;
			//     //     collectInput(
			//     //         resourceForm,
			//     //         customInjectedInput,
			//     //         connection->GetConnection(),
			//     //         out_injectedInput,
			//     //         previousLimit,
			//     //         seenActors,
			//     //         connected,
			//     //         out_injectedItems,
			//     //         restrictItems,
			//     //         buildableSubsystem,
			//     //         level + 1,
			//     //         indent + TEXT("    ")
			//     //         );
			//     //
			//     //     out_limitedThroughput = FMath::Min(out_limitedThroughput, previousLimit);
			//     // }
			//
			//     auto flowLimit = getPipeSpeed(pipeline);
			//
			//     out_limitedThroughput = FMath::Min(out_limitedThroughput, flowLimit);
			//
			//     if (FEfficiencyCheckerModModule::dumpConnections)
			//     {
			//         SML::Logging::info(*getTimeStamp(), *indent, *pipeline->GetName(), TEXT(" flow limit = "), flowLimit, TEXT(" m³/minute"));
			//         SML::Logging::info(*getTimeStamp(), *indent, *pipeline->GetName(), TEXT(" limited at "), out_limitedThroughput, TEXT(" m³/minute"));
			//     }
			//
			//     connected.Add(pipeline);
			//
			//     // Get the opposing connector
			//     connector = connector == pipeline->GetPipeConnection0()
			//                     ? pipeline->GetPipeConnection1()->GetConnection()
			//                     : pipeline->GetPipeConnection0()->GetConnection();
			//
			//     continue;
			// }

			auto fluidIntegrant = Cast<IFGFluidIntegrantInterface>(owner);
			if (fluidIntegrant)
			{
				auto buildable = Cast<AFGBuildable>(owner);

				auto components = fluidIntegrant->GetPipeConnections();

				if (pipeline)
				{
					out_limitedThroughput = FMath::Min(out_limitedThroughput, getPipeSpeed(pipeline));
				}

				auto otherConnections = components.FilterByPredicate(
					[connector, seenActors](UFGPipeConnectionComponent* pipeConnection)
					{
						return seenActors.Num() == 1 ||
							pipeConnection != connector && pipeConnection->IsConnected();
					}
					);

				auto pipePump = Cast<AFGBuildablePipelinePump>(fluidIntegrant);

				if (otherConnections.Num() == 0)
				{
					// No more connections. Bail
					SML::Logging::info(*getTimeStamp(), *indent, *owner->GetName(), TEXT(" has no other connection"));
				}
				else if (otherConnections.Num() == 1 &&
					otherConnections[0]->GetPipeConnectionType() != EPipeConnectionType::PCT_CONSUMER &&
					otherConnections[0]->GetPipeConnectionType() != EPipeConnectionType::PCT_PRODUCER)
				{
					connected.Add(buildable);

					connector = otherConnections[0]->GetConnection();

					if (FEfficiencyCheckerModModule::dumpConnections)
					{
						SML::Logging::info(*getTimeStamp(), *indent, *buildable->GetName(), TEXT(" skipped"));
					}

					continue;
				}
				else
				{
					bool firstConnection = true;
					float limitedThroughput = 0;

					bool firstActor = seenActors.Num() == 1;

					for (auto connection : components)
					{
						if (connection == connector && !firstActor)
						{
							continue;
						}

						if (!connection->IsConnected())
						{
							continue;
						}

						float previousLimit = out_limitedThroughput;
						collectInput(
							resourceForm,
							customInjectedInput,
							connection->GetConnection(),
							out_injectedInput,
							previousLimit,
							seenActors,
							connected,
							out_injectedItems,
							restrictItems,
							buildableSubsystem,
							level + 1,
							overflow,
							indent + TEXT("    ")
							);

						if (pipeline)
						{
							out_limitedThroughput = FMath::Min(out_limitedThroughput, previousLimit);
						}
						else if (firstConnection)
						{
							limitedThroughput = previousLimit;
							firstConnection = false;
						}
						else
						{
							limitedThroughput += limitedThroughput;
						}
					}

					if (!pipeline && !firstConnection)
					{
						out_limitedThroughput = FMath::Min(out_limitedThroughput, limitedThroughput);
					}

					auto pipeConnector = Cast<UFGPipeConnectionComponent>(connector);

					if (pipePump && pipeConnector->GetPipeConnectionType() == EPipeConnectionType::PCT_PRODUCER)
					{
						for (auto connection : components)
						{
							if (connection == connector)
							{
								continue;
							}

							if (!connection->IsConnected() || connection->GetPipeConnectionType() != EPipeConnectionType::PCT_CONSUMER)
							{
								continue;
							}

							float previousLimit = 0;
							float discountedInput = 0;

							std::map<AActor*, TSet<TSubclassOf<UFGItemDescriptor>>> seenActorsCopy;

							for (auto actor : seenActors)
							{
								seenActorsCopy[actor] = out_injectedItems;
							}

							collectOutput(
								resourceForm,
								connection->GetConnection(),
								discountedInput,
								previousLimit,
								seenActorsCopy,
								connected,
								out_injectedItems,
								buildableSubsystem,
								level + 1,
								overflow,
								indent + TEXT("    ")
								);

							if (discountedInput > 0)
							{
								if (FEfficiencyCheckerModModule::dumpConnections)
								{
									SML::Logging::info(*getTimeStamp(), *indent, TEXT("Discounting "), discountedInput, TEXT(" m³/minute"));
								}

								if (!customInjectedInput)
								{
									out_injectedInput -= discountedInput;
								}
							}
						}
					}

					if (FEfficiencyCheckerModModule::dumpConnections)
					{
						SML::Logging::info(*getTimeStamp(), *indent, *owner->GetName(), TEXT(" limited at "), out_limitedThroughput, TEXT(" m³/minute"));
					}
				}

				connected.Add(buildable);

				return;
			}

			auto cargoPlatform = Cast<AFGBuildableTrainPlatformCargo>(owner);
			if (cargoPlatform)
			{
				TArray<UFGPipeConnectionComponent*> pipeConnections;
				cargoPlatform->GetComponents(pipeConnections);

				auto trackId = cargoPlatform->GetTrackGraphID();

				auto railroadSubsystem = AFGRailroadSubsystem::Get(owner->GetWorld());

				// Determine offsets from all the connected stations
				std::set<int> stationOffsets;
				TSet<AFGBuildableRailroadStation*> destinationStations;

				for (auto i = 0; i <= 1; i++)
				{
					auto offsetDistance = 1;

					for (auto connectedPlatform = cargoPlatform->GetConnectedPlatformInDirectionOf(i);
					     connectedPlatform;
					     connectedPlatform = connectedPlatform->GetConnectedPlatformInDirectionOf(i),
					     ++offsetDistance)
					{
						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								*connectedPlatform->GetName(),
								TEXT(" direction = "),
								i,
								TEXT(" / orientation reversed = "),
								connectedPlatform->IsOrientationReversed() ? TEXT("true") : TEXT("false")
								);
						}

						auto station = Cast<AFGBuildableRailroadStation>(connectedPlatform);
						if (station)
						{
							destinationStations.Add(station);

							if (FEfficiencyCheckerModModule::dumpConnections)
							{
								SML::Logging::info(
									*getTimeStamp(),
									*indent,
									TEXT("    Station = "),
									*station->GetStationIdentifier()->GetStationName().ToString()
									);
							}

							if (i == 0 && connectedPlatform->IsOrientationReversed() ||
								i == 1 && !connectedPlatform->IsOrientationReversed())
							{
								stationOffsets.insert(offsetDistance);
								if (FEfficiencyCheckerModModule::dumpConnections)
								{
									SML::Logging::info(*getTimeStamp(), *indent, TEXT("        offset distance = "), offsetDistance);
								}
							}
							else
							{
								stationOffsets.insert(-offsetDistance);
								if (FEfficiencyCheckerModModule::dumpConnections)
								{
									SML::Logging::info(*getTimeStamp(), *indent, TEXT("        offset distance = "), -offsetDistance);
								}
							}
						}

						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							auto cargo = Cast<AFGBuildableTrainPlatformCargo>(connectedPlatform);
							if (cargo)
							{
								SML::Logging::info(
									*getTimeStamp(),
									*indent,
									TEXT("    Load mode = "),
									cargo->GetIsInLoadMode() ? TEXT("true") : TEXT("false")
									);
							}
						}
					}
				}

				TArray<AFGTrain*> trains;
				railroadSubsystem->GetTrains(trackId, trains);

				for (auto train : trains)
				{
					if (!train->HasTimeTable())
					{
						continue;
					}

					if (FEfficiencyCheckerModModule::dumpConnections)
					{
						if (!train->GetTrainName().IsEmpty())
						{
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								TEXT("Train = "),
								*train->GetTrainName().ToString()
								);
						}
						else
						{
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								TEXT("Anonymous Train")
								);
						}
					}

					// Get train stations
					auto timeTable = train->GetTimeTable();

					TArray<FTimeTableStop> stops;
					timeTable->GetStops(stops);

					bool stopAtStations = false;

					for (auto stop : stops)
					{
						if (!stop.Station || !stop.Station->GetStation() || !destinationStations.Contains(stop.Station->GetStation()))
						{
							continue;
						}

						stopAtStations = true;

						break;
					}

					if (!stopAtStations)
					{
						continue;
					}

					for (auto stop : stops)
					{
						if (!stop.Station || !stop.Station->GetStation())
						{
							continue;
						}

						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								TEXT("    Stop = "),
								*stop.Station->GetStationName().ToString()
								);
						}

						for (auto i = 0; i <= 1; i++)
						{
							auto offsetDistance = 1;

							for (auto connectedPlatform = stop.Station->GetStation()->GetConnectedPlatformInDirectionOf(i);
							     connectedPlatform;
							     connectedPlatform = connectedPlatform->GetConnectedPlatformInDirectionOf(i),
							     ++offsetDistance)
							{
								auto stopCargo = Cast<AFGBuildableTrainPlatformCargo>(connectedPlatform);
								if (!stopCargo || stopCargo == cargoPlatform)
								{
									// Not a cargo or the same as the current one. Skip
									continue;
								}

								auto adjustedOffsetDistance = i == 0 && !stop.Station->GetStation()->IsOrientationReversed()
								                              || i == 1 && stop.Station->GetStation()->IsOrientationReversed()
									                              ? offsetDistance
									                              : -offsetDistance;

								if (stationOffsets.find(adjustedOffsetDistance) == stationOffsets.end())
								{
									// Not on a valid offset. Skip
									continue;
								}

								seenActors.Add(stopCargo);

								TArray<UFGPipeConnectionComponent*> cargoPipeConnections;
								stopCargo->GetComponents(cargoPipeConnections);

								pipeConnections.Append(
									cargoPipeConnections.FilterByPredicate(
										[&pipeConnections, stopCargo](UFGPipeConnectionComponent* connection)
										{
											if (pipeConnections.Contains(connection))
											{
												// Already in use
												return false;
											}

											if (stopCargo->GetIsInLoadMode())
											{
												// Loading
												return true;
											}

											if (connection->GetPipeConnectionType() == EPipeConnectionType::PCT_PRODUCER)
											{
												// Is not a consumer connection
												return true;
											}

											return false;
										}
										)
									);
							}
						}
					}
				}

				bool firstConnection = true;
				float limitedThroughput = out_limitedThroughput;

				for (auto connection : pipeConnections)
				{
					if (!connection->IsConnected() ||
						connection->GetPipeConnectionType() != EPipeConnectionType::PCT_CONSUMER ||
						connection == connector)
					{
						continue;
					}

					float previousLimit = out_limitedThroughput;
					collectInput(
						resourceForm,
						customInjectedInput,
						connection->GetConnection(),
						out_injectedInput,
						previousLimit,
						seenActors,
						connected,
						out_injectedItems,
						restrictItems,
						buildableSubsystem,
						level + 1,
						overflow,
						indent + TEXT("    ")
						);

					if (firstConnection)
					{
						limitedThroughput = previousLimit;
						firstConnection = false;
					}
					else
					{
						limitedThroughput += previousLimit;
					}
				}

				out_limitedThroughput = FMath::Min(out_limitedThroughput, limitedThroughput);

				for (auto connection : pipeConnections)
				{
					if (connection == connector ||
						!connection->IsConnected() ||
						connection->GetPipeConnectionType() != EPipeConnectionType::PCT_PRODUCER)
					{
						continue;
					}

					float previousLimit = 0;
					float discountedInput = 0;

					std::map<AActor*, TSet<TSubclassOf<UFGItemDescriptor>>> seenActorsCopy;

					for (auto actor : seenActors)
					{
						seenActorsCopy[actor] = out_injectedItems;
					}

					collectOutput(
						resourceForm,
						connection->GetConnection(),
						discountedInput,
						previousLimit,
						seenActorsCopy,
						connected,
						out_injectedItems,
						buildableSubsystem,
						level + 1,
						overflow,
						indent + TEXT("    ")
						);

					if (discountedInput > 0)
					{
						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							SML::Logging::info(*getTimeStamp(), *indent, TEXT("Discounting "), discountedInput, TEXT(" m³/minute"));
						}

						if (!customInjectedInput)
						{
							out_injectedInput -= discountedInput;
						}
					}
				}

				if (FEfficiencyCheckerModModule::dumpConnections)
				{
					SML::Logging::info(*getTimeStamp(), *indent, *owner->GetName(), TEXT(" limited at "), out_limitedThroughput, TEXT(" m³/minute"));
				}

				connected.Add(Cast<AFGBuildable>(fluidIntegrant));

				return;
			}
		}

		{
			const auto nuclearGenerator = Cast<AFGBuildableGeneratorNuclear>(owner);
			if (nuclearGenerator)
			{
				for (auto item : singleton->nuclearWasteItemDescriptors)
				{
					out_injectedItems.Add(item);
				}

				connected.Add(nuclearGenerator);

				out_injectedInput += 0.2;

				return;
			}
		}

		if (inheritsFrom(owner, TEXT("/Script/FactoryGame.FGBuildableFactorySimpleProducer")))
		{
			TSubclassOf<UFGItemDescriptor> itemType = FReflectionHelper::GetObjectPropertyValue<UClass>(owner, TEXT("mItemType"));
			auto timeToProduceItem = FReflectionHelper::GetPropertyValue<UFloatProperty>(owner, TEXT("mTimeToProduceItem"));

			if (timeToProduceItem && itemType)
			{
				out_injectedItems.Add(itemType);

				out_injectedInput += 60 / timeToProduceItem;
			}

			connected.Add(Cast<AFGBuildable>(owner));

			return;
		}

		// out_limitedThroughput = 0;

		if (FEfficiencyCheckerModModule::dumpConnections)
		{
			dumpUnknownClass(indent, owner);
		}

		return;
	}
}

void AEfficiencyCheckerLogic::collectOutput
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
	bool& overflow,
	const FString& indent
)
{
	TSet<TSubclassOf<UFGItemDescriptor>> injectedItems = in_injectedItems;

	for (;;)
	{
		if (!connector)
		{
			return;
		}

		auto owner = connector->GetOwner();

		if (!owner)
		{
			return;
		}

		auto fullClassName = owner->GetClass()->GetPathName();

		if (level > 100)
		{
			SML::Logging::error(
				*getTimeStamp(),
				*indent,
				TEXT("collectOutput: level is too deep: "),
				level,
				TEXT("; "),
				*owner->GetName(),
				TEXT(" / "),
				*fullClassName
				);

			overflow = true;

			return;
		}

		if (injectedItems.Num())
		{
			bool unusedItems = false;

			for (auto item : injectedItems)
			{
				if (!actorContainsItem(seenActors, owner, item))
				{
					unusedItems = true;
					break;
				}
			}

			if (!unusedItems)
			{
				return;
			}
		}
		else
		{
			if (containsActor(seenActors, owner))
			{
				return;
			}
		}

		if (FEfficiencyCheckerModModule::dumpConnections)
		{
			SML::Logging::info(
				*getTimeStamp(),
				*indent,
				TEXT("collectOutput at level "),
				level,
				TEXT(": "),
				*owner->GetName(),
				TEXT(" / "),
				*fullClassName
				);
		}

		{
			const auto manufacturer = Cast<AFGBuildableManufacturer>(owner);
			if (manufacturer)
			{
				const auto recipeClass = manufacturer->GetCurrentRecipe();

				if (recipeClass)
				{
					auto ingredients = UFGRecipe::GetIngredients(recipeClass);

					for (auto item : ingredients)
					{
						auto itemForm = UFGItemDescriptor::GetForm(item.ItemClass);

						if (itemForm == EResourceForm::RF_SOLID && resourceForm != EResourceForm::RF_SOLID ||
							(itemForm == EResourceForm::RF_LIQUID || itemForm == EResourceForm::RF_GAS) &&
							resourceForm != EResourceForm::RF_LIQUID && resourceForm != EResourceForm::RF_GAS)
						{
							continue;
						}

						if (!injectedItems.Contains(item.ItemClass) || seenActors[manufacturer].Contains(item.ItemClass))
						{
							continue;
						}

						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							SML::Logging::info(*getTimeStamp(), *indent, TEXT("Item amount = "), item.Amount);
							SML::Logging::info(*getTimeStamp(), *indent, TEXT("Current potential = "), manufacturer->GetCurrentPotential());
							SML::Logging::info(*getTimeStamp(), *indent, TEXT("Pending potential = "), manufacturer->GetPendingPotential());
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								TEXT("Production cycle time = "),
								manufacturer->CalcProductionCycleTimeForPotential(manufacturer->GetPendingPotential())
								);
							SML::Logging::info(*getTimeStamp(), *indent, TEXT("Recipe duration = "), UFGRecipe::GetManufacturingDuration(recipeClass));
						}

						float itemAmountPerMinute = item.Amount * (60.0 / manufacturer->CalcProductionCycleTimeForPotential(manufacturer->GetPendingPotential()));

						if (resourceForm == EResourceForm::RF_LIQUID || resourceForm == EResourceForm::RF_GAS)
						{
							itemAmountPerMinute /= 1000;
						}

						// if (fullClassName.StartsWith(TEXT("/Game/MK22k20/Buildable")))
						// {
						//     if (fullClassName.EndsWith(TEXT("MK2_C")))
						//     {
						//         itemAmountPerMinute *= 1.5;
						//     }
						//     else if (fullClassName.EndsWith(TEXT("MK3_C")))
						//     {
						//         itemAmountPerMinute *= 2;
						//     }
						//     else if (fullClassName.EndsWith(TEXT("MK4_C")))
						//     {
						//         itemAmountPerMinute *= 2.5;
						//     }
						// }
						// else if (fullClassName.StartsWith(TEXT("/Game/FarmingMod/Buildable")))
						// {
						//     if (fullClassName.EndsWith(TEXT("Mk2_C")))
						//     {
						//         itemAmountPerMinute *= 2;
						//     }
						//     else if (fullClassName.EndsWith(TEXT("Mk3_C")))
						//     {
						//         itemAmountPerMinute *= 3;
						//     }
						// }

						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								*manufacturer->GetName(),
								TEXT(" consumes "),
								itemAmountPerMinute,
								TEXT(" "),
								*UFGItemDescriptor::GetItemName(item.ItemClass).ToString(),
								TEXT("/minute")
								);
						}

						out_requiredOutput += itemAmountPerMinute;

						seenActors[manufacturer].Add(item.ItemClass);
					}
				}

				connected.Add(manufacturer);

				return;
			}
		}

		if (resourceForm == EResourceForm::RF_SOLID)
		{
			const auto conveyor = Cast<AFGBuildableConveyorBase>(owner);
			if (conveyor)
			{
				addAllItemsToActor(seenActors, conveyor, injectedItems);

				// // The innitial limit for a belt is its own speed
				// out_limitedThroughput = conveyor->GetSpeed() / 2;
				//
				// const auto conveyorInput = conveyor->GetConnection1();
				// if (conveyorInput && conveyorInput->IsConnected())
				// {
				//     float previousLimit = out_limitedThroughput;
				//     collectOutput(
				//         resourceForm,
				//         conveyorInput->GetConnection(),
				//         out_requiredOutput,
				//         previousLimit,
				//         seenActors,
				//         connected,
				//         injectedItems,
				//         buildableSubsystem,
				//         level + 1,
				//         indent + TEXT("    ")
				//         );
				//
				//     out_limitedThroughput = FMath::Min(out_limitedThroughput, previousLimit);
				// }

				connected.Add(conveyor);

				connector = conveyor->GetConnection1()->GetConnection();

				out_limitedThroughput = FMath::Min(out_limitedThroughput, conveyor->GetSpeed() / 2);

				if (FEfficiencyCheckerModModule::dumpConnections)
				{
					SML::Logging::info(*getTimeStamp(), *indent, *conveyor->GetName(), TEXT(" limited at "), out_limitedThroughput, TEXT(" items/minute"));
				}

				continue;
			}

			AFGBuildableStorage* storageContainer = nullptr;
			AFGBuildableTrainPlatformCargo* cargoPlatform = nullptr;
			AFGBuildableConveyorAttachment* conveyorAttachment = nullptr;
			AFGBuildableDockingStation* dockingStation = nullptr;
			AFGBuildableFactory* storageTeleporter = nullptr;

			AFGBuildableFactory* buildable = Cast<AFGBuildableConveyorAttachment>(owner);
			if (!buildable)
			{
				buildable = storageContainer = Cast<AFGBuildableStorage>(owner);
			}

			if (!buildable)
			{
				buildable = cargoPlatform = Cast<AFGBuildableTrainPlatformCargo>(owner);
			}

			if (!buildable)
			{
				buildable = dockingStation = Cast<AFGBuildableDockingStation>(owner);
			}

			if (!FEfficiencyCheckerModModule::ignoreStorageTeleporter &&
				!buildable && fullClassName == TEXT("/Game/StorageTeleporter/Buildables/ItemTeleporter/ItemTeleporter_Build.ItemTeleporter_Build_C"))
			{
				buildable = storageTeleporter = Cast<AFGBuildableFactory>(owner);
			}

			if (buildable)
			{
				addAllItemsToActor(seenActors, buildable, injectedItems);

				auto components = buildable->GetConnectionComponents();

				if (cargoPlatform)
				{
					auto trackId = cargoPlatform->GetTrackGraphID();

					auto railroadSubsystem = AFGRailroadSubsystem::Get(owner->GetWorld());

					// Determine offsets from all the connected stations
					std::set<int> stationOffsets;
					TSet<AFGBuildableRailroadStation*> destinationStations;

					for (auto i = 0; i <= 1; i++)
					{
						auto offsetDistance = 1;

						for (auto connectedPlatform = cargoPlatform->GetConnectedPlatformInDirectionOf(i);
						     connectedPlatform;
						     connectedPlatform = connectedPlatform->GetConnectedPlatformInDirectionOf(i),
						     ++offsetDistance)
						{
							if (FEfficiencyCheckerModModule::dumpConnections)
							{
								SML::Logging::info(
									*getTimeStamp(),
									*indent,
									*connectedPlatform->GetName(),
									TEXT(" direction = "),
									i,
									TEXT(" / orientation reversed = "),
									connectedPlatform->IsOrientationReversed() ? TEXT("true") : TEXT("false")
									);
							}

							auto station = Cast<AFGBuildableRailroadStation>(connectedPlatform);
							if (station)
							{
								destinationStations.Add(station);

								if (FEfficiencyCheckerModModule::dumpConnections)
								{
									SML::Logging::info(
										*getTimeStamp(),
										*indent,
										TEXT("    Station = "),
										*station->GetStationIdentifier()->GetStationName().ToString()
										);
								}

								if (i == 0 && connectedPlatform->IsOrientationReversed() ||
									i == 1 && !connectedPlatform->IsOrientationReversed())
								{
									stationOffsets.insert(offsetDistance);
									if (FEfficiencyCheckerModModule::dumpConnections)
									{
										SML::Logging::info(*getTimeStamp(), *indent, TEXT("        offset distance = "), offsetDistance);
									}
								}
								else
								{
									stationOffsets.insert(-offsetDistance);
									if (FEfficiencyCheckerModModule::dumpConnections)
									{
										SML::Logging::info(*getTimeStamp(), *indent, TEXT("        offset distance = "), -offsetDistance);
									}
								}
							}

							auto cargo = Cast<AFGBuildableTrainPlatformCargo>(connectedPlatform);
							if (cargo)
							{
								if (FEfficiencyCheckerModModule::dumpConnections)
								{
									SML::Logging::info(
										*getTimeStamp(),
										*indent,
										TEXT("    Load mode = "),
										cargo->GetIsInLoadMode() ? TEXT("true") : TEXT("false")
										);
								}
							}
						}
					}

					TArray<AFGTrain*> trains;
					railroadSubsystem->GetTrains(trackId, trains);

					for (auto train : trains)
					{
						if (!train->HasTimeTable())
						{
							continue;
						}

						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							if (!train->GetTrainName().IsEmpty())
							{
								SML::Logging::info(
									*getTimeStamp(),
									*indent,
									TEXT("Train = "),
									*train->GetTrainName().ToString()
									);
							}
							else
							{
								SML::Logging::info(
									*getTimeStamp(),
									*indent,
									TEXT("Anonymous Train")
									);
							}
						}

						// Get train stations
						auto timeTable = train->GetTimeTable();

						TArray<FTimeTableStop> stops;
						timeTable->GetStops(stops);

						bool stopAtStations = false;

						for (auto stop : stops)
						{
							if (!stop.Station || !stop.Station->GetStation() || !destinationStations.Contains(stop.Station->GetStation()))
							{
								continue;
							}

							stopAtStations = true;

							break;
						}

						if (!stopAtStations)
						{
							continue;
						}

						for (auto stop : stops)
						{
							if (!stop.Station || !stop.Station->GetStation())
							{
								continue;
							}

							if (FEfficiencyCheckerModModule::dumpConnections)
							{
								SML::Logging::info(
									*getTimeStamp(),
									*indent,
									TEXT("    Stop = "),
									*stop.Station->GetStationName().ToString()
									);
							}

							for (auto i = 0; i <= 1; i++)
							{
								auto offsetDistance = 1;

								for (auto connectedPlatform = stop.Station->GetStation()->GetConnectedPlatformInDirectionOf(i);
								     connectedPlatform;
								     connectedPlatform = connectedPlatform->GetConnectedPlatformInDirectionOf(i),
								     ++offsetDistance)
								{
									auto stopCargo = Cast<AFGBuildableTrainPlatformCargo>(connectedPlatform);
									if (!stopCargo || stopCargo == cargoPlatform)
									{
										// Not a cargo or the same as the current one. Skip
										continue;
									}

									auto adjustedOffsetDistance = i == 0 && !stop.Station->GetStation()->IsOrientationReversed()
									                              || i == 1 && stop.Station->GetStation()->IsOrientationReversed()
										                              ? offsetDistance
										                              : -offsetDistance;

									if (stationOffsets.find(adjustedOffsetDistance) == stationOffsets.end())
									{
										// Not on a valid offset. Skip
										continue;
									}

									addAllItemsToActor(seenActors, stopCargo, injectedItems);

									components.Append(
										stopCargo->GetConnectionComponents().FilterByPredicate(
											[&components, stopCargo](UFGFactoryConnectionComponent* connection)
											{
												return !components.Contains(connection) && // Not in use already
													!stopCargo->GetIsInLoadMode() && // Unload mode
													connection->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT; // Is output connection
											}
											)
										);
								}
							}
						}
					}
				}

				if (storageTeleporter)
				{
					auto currentStorageID = FReflectionHelper::GetPropertyValue<UStrProperty>(storageTeleporter, TEXT("StorageID"));

					// TArray<AActor*> allTeleporters;
					// if (IsInGameThread())
					// {
					//     UGameplayStatics::GetAllActorsOfClass(storageTeleporter->GetWorld(), storageTeleporter->GetClass(), allTeleporters);
					// }

					FScopeLock ScopeLock(&singleton->eclCritical);

					for (auto testTeleporter : AEfficiencyCheckerLogic::singleton->allTeleporters)
					{
						if (testTeleporter == storageTeleporter)
						{
							continue;
						}

						auto storageID = FReflectionHelper::GetPropertyValue<UStrProperty>(testTeleporter, TEXT("StorageID"));
						if (storageID == currentStorageID)
						{
							addAllItemsToActor(seenActors, testTeleporter, injectedItems);

							auto factory = Cast<AFGBuildableFactory>(testTeleporter);
							if (factory)
							{
								components.Append(
									factory->GetConnectionComponents().FilterByPredicate(
										[&components](UFGFactoryConnectionComponent* connection)
										{
											return !components.Contains(connection) && // Not in use already
												connection->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT; // Is output connection
										}
										)
									);
							}
						}
					}
				}

				std::map<int, TSet<TSubclassOf<UFGItemDescriptor>>> restrictedItemsByOutput;

				// Filter items
				auto smartSplitter = Cast<AFGBuildableSplitterSmart>(buildable);
				if (smartSplitter)
				{
					for (int x = 0; x < smartSplitter->GetNumSortRules(); ++x)
					{
						auto rule = smartSplitter->GetSortRuleAt(x);

						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								TEXT("Rule "),
								x,
								TEXT(" / output index = "),
								rule.OutputIndex,
								TEXT(" / item = "),
								*UFGItemDescriptor::GetItemName(rule.ItemClass).ToString(),
								TEXT(" / class = "),
								*rule.ItemClass->GetPathName()
								);
						}

						restrictedItemsByOutput[rule.OutputIndex].Add(rule.ItemClass);
					}

					TSet<TSubclassOf<UFGItemDescriptor>> definedItems;

					for (auto it = restrictedItemsByOutput.begin(); it != restrictedItemsByOutput.end(); ++it)
					{
						if (singleton->noneItemDescriptors.Intersect(it->second).Num())
						{
							// No item is valid. Empty it all
							it->second.Empty();
						}
						else if (singleton->wildCardItemDescriptors.Intersect(it->second).Num() || singleton->overflowItemDescriptors.Intersect(it->second).Num())
						{
							// Add all current restrictItems as valid items
							it->second = injectedItems;
						}

						for (auto item : it->second)
						{
							definedItems.Add(item);
						}
					}

					for (auto it = restrictedItemsByOutput.begin(); it != restrictedItemsByOutput.end(); ++it)
					{
						if (singleton->anyUndefinedItemDescriptors.Intersect(it->second).Num())
						{
							it->second = it->second.Union(injectedItems.Difference(definedItems));
						}

						it->second = it->second.Intersect(injectedItems);
					}
				}


				TArray<UFGFactoryConnectionComponent*> connectedInputs, connectedOutputs;
				for (auto connection : components)
				{
					if (!connection->IsConnected())
					{
						continue;
					}

					if (connection->GetConnector() != EFactoryConnectionConnector::FCC_CONVEYOR)
					{
						continue;
					}

					if (connection->GetDirection() == EFactoryConnectionDirection::FCD_INPUT)
					{
						connectedInputs.Add(connection);
					}
					else if (connection->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT)
					{
						connectedOutputs.Add(connection);
					}
				}

				if (connectedInputs.Num() == 1 && connectedOutputs.Num() == 1)
				{
					connected.Add(buildable);

					connector = connectedOutputs[0]->GetConnection();

					if (FEfficiencyCheckerModModule::dumpConnections)
					{
						SML::Logging::info(*getTimeStamp(), *indent, *buildable->GetName(), TEXT(" skipped"));
					}

					if (smartSplitter)
					{
						auto outputIndex = connectedOutputs[0]->GetName()[connectedOutputs[0]->GetName().Len() - 1] - '1';

						injectedItems = restrictedItemsByOutput[outputIndex];
					}

					continue;
				}

				if (connectedOutputs.Num() == 0)
				{
					// Nothing is being outputed. Bail
					SML::Logging::info(*getTimeStamp(), *indent, *buildable->GetName(), TEXT(" has no input"));
				}
				else
				{
					if (!dockingStation || !connector->GetName().Equals(TEXT("Input0"), ESearchCase::IgnoreCase))
					{
						bool firstConnection = true;
						float limitedThroughput = 0;

						for (int connectorIndex = 0; connectorIndex < components.Num(); connectorIndex++)
						{
							auto connection = components[connectorIndex];

							if (!connection->IsConnected())
							{
								continue;
							}

							if (connection->GetConnector() != EFactoryConnectionConnector::FCC_CONVEYOR)
							{
								continue;
							}

							if (connection->GetDirection() != EFactoryConnectionDirection::FCD_OUTPUT)
							{
								continue;
							}

							auto outputIndex = connection->GetName()[connection->GetName().Len() - 1] - '1';

							float previousLimit = out_limitedThroughput;
							collectOutput(
								resourceForm,
								connection->GetConnection(),
								out_requiredOutput,
								previousLimit,
								seenActors,
								connected,
								smartSplitter ? restrictedItemsByOutput[outputIndex] : injectedItems,
								buildableSubsystem,
								level + 1,
								overflow,
								indent + TEXT("    ")
								);

							if (firstConnection)
							{
								limitedThroughput = previousLimit;
								firstConnection = false;
							}
							else
							{
								limitedThroughput += previousLimit;
							}
						}

						out_limitedThroughput = FMath::Min(out_limitedThroughput, limitedThroughput);

						for (int connectorIndex = 0; connectorIndex < components.Num(); connectorIndex++)
						{
							auto connection = components[connectorIndex];

							if (connection == connector)
							{
								continue;
							}

							if (!connection->IsConnected())
							{
								continue;
							}

							if (connection->GetDirection() != EFactoryConnectionDirection::FCD_INPUT)
							{
								continue;
							}

							auto inputIndex = connection->GetName()[connection->GetName().Len() - 1] - '1';

							float previousLimit = 0;
							float discountedOutput = 0;
							TSet<AActor*> seenActorsCopy;

							auto tempInjectedItems = injectedItems;

							for (auto actor : seenActors)
							{
								seenActorsCopy.Add(actor.first);
							}

							collectInput(
								resourceForm,
								false,
								connection->GetConnection(),
								discountedOutput,
								previousLimit,
								seenActorsCopy,
								connected,
								tempInjectedItems,
								tempInjectedItems,
								buildableSubsystem,
								level + 1,
								overflow,
								indent + TEXT("    ")
								);

							if (discountedOutput > 0)
							{
								if (FEfficiencyCheckerModModule::dumpConnections)
								{
									SML::Logging::info(*getTimeStamp(), *indent, TEXT("Discounting "), discountedOutput, TEXT(" items/minute"));
								}

								out_requiredOutput -= discountedOutput;
							}
						}
					}

					if (FEfficiencyCheckerModModule::dumpConnections)
					{
						SML::Logging::info(*getTimeStamp(), *indent, *buildable->GetName(), TEXT(" limited at "), out_limitedThroughput, TEXT(" items/minute"));
					}
				}

				connected.Add(buildable);

				return;
			}
		}

		if (resourceForm == EResourceForm::RF_LIQUID || resourceForm == EResourceForm::RF_GAS)
		{
			auto pipeline = Cast<AFGBuildablePipeline>(owner);
			// if (pipeline)
			// {
			//     addAllItemsToActor(seenActors, pipeline, injectedItems);
			//
			//     // out_limitedThroughput = UFGBlueprintFunctionLibrary::RoundFloatWithPrecision(pipeline->GetFlowLimit() * 60, 4);
			//     // // out_limitedThroughput = pipeline->mFlowLimit * 60;
			//     //
			//     // auto components = pipeline->GetPipeConnections();
			//     //
			//     // for (auto connection : components)
			//     // {
			//     //     if (!connection->IsConnected() || connection->GetPipeConnection()->GetPipeConnectionType() == EPipeConnectionType::PCT_PRODUCER)
			//     //     {
			//     //         continue;
			//     //     }
			//     //
			//     //     float previousLimit = out_limitedThroughput;
			//     //     collectOutput(
			//     //         resourceForm,
			//     //         connection->GetConnection(),
			//     //         out_requiredOutput,
			//     //         previousLimit,
			//     //         seenActors,
			//     //         connected,
			//     //         injectedItems,
			//     //         buildableSubsystem,
			//     //         level + 1,
			//     //         indent + TEXT("    ")
			//     //         );
			//     //
			//     //     out_limitedThroughput = FMath::Min(out_limitedThroughput, previousLimit);
			//     // }
			//
			//     auto flowLimit = getPipeSpeed(pipeline);
			//
			//     out_limitedThroughput = FMath::Min(out_limitedThroughput, flowLimit);
			//
			//     if (FEfficiencyCheckerModModule::dumpConnections)
			//     {
			//         SML::Logging::info(*getTimeStamp(), *indent, *pipeline->GetName(), TEXT(" flow limit = "), flowLimit, TEXT(" m³/minute"));
			//         SML::Logging::info(*getTimeStamp(), *indent, *pipeline->GetName(), TEXT(" limited at "), out_limitedThroughput, TEXT(" m³/minute"));
			//     }
			//
			//     connected.Add(pipeline);
			//
			//     // Get the opposing connector
			//     connector = connector == pipeline->GetPipeConnection0()
			//                     ? pipeline->GetPipeConnection1()->GetConnection()
			//                     : pipeline->GetPipeConnection0()->GetConnection();
			//
			//     continue;
			// }

			auto fluidIntegrant = Cast<IFGFluidIntegrantInterface>(owner);
			if (fluidIntegrant)
			{
				auto buildable = Cast<AFGBuildable>(fluidIntegrant);

				addAllItemsToActor(seenActors, Cast<AFGBuildable>(fluidIntegrant), injectedItems);

				auto components = fluidIntegrant->GetPipeConnections();

				if (pipeline)
				{
					out_limitedThroughput = FMath::Min(out_limitedThroughput, getPipeSpeed(pipeline));
				}

				auto otherConnections = components.FilterByPredicate(
					[connector, seenActors](UFGPipeConnectionComponent* pipeConnection)
					{
						return seenActors.size() == 1 ||
							pipeConnection != connector && pipeConnection->IsConnected();
					}
					);

				auto pipePump = Cast<AFGBuildablePipelinePump>(fluidIntegrant);

				if (otherConnections.Num() == 0)
				{
					// No more connections. Bail
					SML::Logging::info(*getTimeStamp(), *indent, *owner->GetName(), TEXT(" has no other connection"));
				}
				else if (otherConnections.Num() == 1 &&
					otherConnections[0]->GetPipeConnectionType() != EPipeConnectionType::PCT_PRODUCER &&
					otherConnections[0]->GetPipeConnectionType() != EPipeConnectionType::PCT_CONSUMER)
				{
					connected.Add(buildable);

					connector = otherConnections[0]->GetConnection();

					if (FEfficiencyCheckerModModule::dumpConnections)
					{
						SML::Logging::info(*getTimeStamp(), *indent, *buildable->GetName(), TEXT(" skipped"));
					}

					continue;
				}
				else
				{
					// out_limitedThroughput = 0;
					bool firstConnection = true;
					float limitedThroughput = out_limitedThroughput;

					bool firstActor = seenActors.size() == 1;

					for (auto connection : components)
					{
						if (connection == connector && !firstActor)
						{
							continue;
						}

						if (!connection->IsConnected())
						{
							continue;
						}

						float previousLimit = out_limitedThroughput;
						collectOutput(
							resourceForm,
							connection->GetConnection(),
							out_requiredOutput,
							previousLimit,
							seenActors,
							connected,
							injectedItems,
							buildableSubsystem,
							level + 1,
							overflow,
							indent + TEXT("    ")
							);

						if (pipeline)
						{
							out_limitedThroughput = FMath::Min(out_limitedThroughput, previousLimit);
						}
						else if (firstConnection)
						{
							limitedThroughput = previousLimit;
							firstConnection = false;
						}
						else
						{
							limitedThroughput += previousLimit;
						}
					}

					if (!pipeline && !firstConnection)
					{
						out_limitedThroughput = FMath::Min(out_limitedThroughput, limitedThroughput);
					}

					auto pipeConnector = Cast<UFGPipeConnectionComponent>(connector);

					if (pipePump && pipeConnector->GetPipeConnectionType() == EPipeConnectionType::PCT_CONSUMER)
					{
						for (auto connection : components)
						{
							if (connection == connector)
							{
								continue;
							}

							if (!connection->IsConnected() || connection->GetPipeConnectionType() != EPipeConnectionType::PCT_PRODUCER)
							{
								continue;
							}

							float previousLimit = 0;
							float discountedOutput = 0;

							TSet<AActor*> seenActorsCopy;

							auto tempInjectedItems = injectedItems;

							for (auto actor : seenActors)
							{
								seenActorsCopy.Add(actor.first);
							}

							collectInput(
								resourceForm,
								false,
								connection->GetConnection(),
								discountedOutput,
								previousLimit,
								seenActorsCopy,
								connected,
								tempInjectedItems,
								tempInjectedItems,
								buildableSubsystem,
								level + 1,
								overflow,
								indent + TEXT("    ")
								);

							if (discountedOutput > 0)
							{
								if (FEfficiencyCheckerModModule::dumpConnections)
								{
									SML::Logging::info(*getTimeStamp(), *indent, TEXT("Discounting "), discountedOutput, TEXT(" m³/minute"));
								}

								out_requiredOutput -= discountedOutput;
							}
						}
					}

					if (FEfficiencyCheckerModModule::dumpConnections)
					{
						SML::Logging::info(*getTimeStamp(), *indent, *owner->GetName(), TEXT(" limited at "), out_limitedThroughput, TEXT(" m³/minute"));
					}
				}

				connected.Add(buildable);

				return;
			}

			auto cargoPlatform = Cast<AFGBuildableTrainPlatformCargo>(owner);
			if (cargoPlatform)
			{
				addAllItemsToActor(seenActors, Cast<AFGBuildable>(cargoPlatform), injectedItems);

				TArray<UFGPipeConnectionComponent*> pipeConnections;
				cargoPlatform->GetComponents(pipeConnections);

				auto trackId = cargoPlatform->GetTrackGraphID();

				auto railroadSubsystem = AFGRailroadSubsystem::Get(owner->GetWorld());

				// Determine offsets from all the connected stations
				std::set<int> stationOffsets;
				TSet<AFGBuildableRailroadStation*> destinationStations;

				for (auto i = 0; i <= 1; i++)
				{
					auto offsetDistance = 1;

					for (auto connectedPlatform = cargoPlatform->GetConnectedPlatformInDirectionOf(i);
					     connectedPlatform;
					     connectedPlatform = connectedPlatform->GetConnectedPlatformInDirectionOf(i),
					     ++offsetDistance)
					{
						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								*connectedPlatform->GetName(),
								TEXT(" direction = "),
								i,
								TEXT(" / orientation reversed = "),
								connectedPlatform->IsOrientationReversed() ? TEXT("true") : TEXT("false")
								);
						}

						auto station = Cast<AFGBuildableRailroadStation>(connectedPlatform);
						if (station)
						{
							destinationStations.Add(station);

							if (FEfficiencyCheckerModModule::dumpConnections)
							{
								SML::Logging::info(
									*getTimeStamp(),
									*indent,
									TEXT("    Station = "),
									*station->GetStationIdentifier()->GetStationName().ToString()
									);
							}

							if (i == 0 && connectedPlatform->IsOrientationReversed() ||
								i == 1 && !connectedPlatform->IsOrientationReversed())
							{
								stationOffsets.insert(offsetDistance);
								if (FEfficiencyCheckerModModule::dumpConnections)
								{
									SML::Logging::info(*getTimeStamp(), *indent, TEXT("        offset distance = "), offsetDistance);
								}
							}
							else
							{
								stationOffsets.insert(-offsetDistance);
								if (FEfficiencyCheckerModModule::dumpConnections)
								{
									SML::Logging::info(*getTimeStamp(), *indent, TEXT("        offset distance = "), -offsetDistance);
								}
							}
						}

						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							auto cargo = Cast<AFGBuildableTrainPlatformCargo>(connectedPlatform);
							if (cargo)
							{
								SML::Logging::info(
									*getTimeStamp(),
									*indent,
									TEXT("    Load mode = "),
									cargo->GetIsInLoadMode() ? TEXT("true") : TEXT("false")
									);
							}
						}
					}
				}

				TArray<AFGTrain*> trains;
				railroadSubsystem->GetTrains(trackId, trains);

				for (auto train : trains)
				{
					if (!train->HasTimeTable())
					{
						continue;
					}

					if (FEfficiencyCheckerModModule::dumpConnections)
					{
						if (!train->GetTrainName().IsEmpty())
						{
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								TEXT("Train = "),
								*train->GetTrainName().ToString()
								);
						}
						else
						{
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								TEXT("Anonymous Train")
								);
						}
					}

					// Get train stations
					auto timeTable = train->GetTimeTable();

					TArray<FTimeTableStop> stops;
					timeTable->GetStops(stops);

					bool stopAtStations = false;

					for (auto stop : stops)
					{
						if (!stop.Station || !stop.Station->GetStation() || !destinationStations.Contains(stop.Station->GetStation()))
						{
							continue;
						}

						stopAtStations = true;

						break;
					}

					if (!stopAtStations)
					{
						continue;
					}

					for (auto stop : stops)
					{
						if (!stop.Station || !stop.Station->GetStation())
						{
							continue;
						}

						if (FEfficiencyCheckerModModule::dumpConnections)
						{
							SML::Logging::info(
								*getTimeStamp(),
								*indent,
								TEXT("    Stop = "),
								*stop.Station->GetStationName().ToString()
								);
						}

						for (auto i = 0; i <= 1; i++)
						{
							auto offsetDistance = 1;

							for (auto connectedPlatform = stop.Station->GetStation()->GetConnectedPlatformInDirectionOf(i);
							     connectedPlatform;
							     connectedPlatform = connectedPlatform->GetConnectedPlatformInDirectionOf(i),
							     ++offsetDistance)
							{
								auto stopCargo = Cast<AFGBuildableTrainPlatformCargo>(connectedPlatform);
								if (!stopCargo || stopCargo == cargoPlatform)
								{
									// Not a cargo or the same as the current one. Skip
									continue;
								}

								auto adjustedOffsetDistance = i == 0 && !stop.Station->GetStation()->IsOrientationReversed()
								                              || i == 1 && stop.Station->GetStation()->IsOrientationReversed()
									                              ? offsetDistance
									                              : -offsetDistance;

								if (stationOffsets.find(adjustedOffsetDistance) == stationOffsets.end())
								{
									// Not on a valid offset. Skip
									continue;
								}

								addAllItemsToActor(seenActors, stopCargo, injectedItems);

								TArray<UFGPipeConnectionComponent*> cargoPipeConnections;
								stopCargo->GetComponents(cargoPipeConnections);

								pipeConnections.Append(
									cargoPipeConnections.FilterByPredicate(
										[&pipeConnections, stopCargo](UFGPipeConnectionComponent* connection)
										{
											if (pipeConnections.Contains(connection))
											{
												// Already in use
												return false;
											}

											if (stopCargo->GetIsInLoadMode())
											{
												// Loading
												return false;
											}

											if (connection->GetPipeConnectionType() == EPipeConnectionType::PCT_PRODUCER)
											{
												// It is a producer connection
												return true;
											}

											return false;
										}
										)
									);
							}
						}
					}
				}

				bool firstConnection = true;
				float limitedThroughput = 0;

				for (auto connection : pipeConnections)
				{
					if (!connection->IsConnected() ||
						connection->GetPipeConnectionType() != EPipeConnectionType::PCT_PRODUCER)
					{
						continue;
					}

					float previousLimit = out_limitedThroughput;
					collectOutput(
						resourceForm,
						connection->GetConnection(),
						out_requiredOutput,
						previousLimit,
						seenActors,
						connected,
						injectedItems,
						buildableSubsystem,
						level + 1,
						overflow,
						indent + TEXT("    ")
						);

					if (firstConnection)
					{
						limitedThroughput = previousLimit;
						firstConnection = false;
					}
					else
					{
						limitedThroughput += previousLimit;
					}
				}

				out_limitedThroughput = FMath::Min(out_limitedThroughput, limitedThroughput);

				if (FEfficiencyCheckerModModule::dumpConnections)
				{
					SML::Logging::info(*getTimeStamp(), *indent, *owner->GetName(), TEXT(" limited at "), out_limitedThroughput, TEXT(" m³/minute"));
				}

				connected.Add(Cast<AFGBuildable>(fluidIntegrant));

				return;
			}
		}

		{
			const auto generator = Cast<AFGBuildableGeneratorFuel>(owner);
			if (generator)
			{
				if (injectedItems.Contains(generator->GetSupplementalResourceClass()) && !seenActors[generator].Contains(generator->GetSupplementalResourceClass()))
				{
					if (FEfficiencyCheckerModModule::dumpConnections)
					{
						SML::Logging::info(
							*getTimeStamp(),
							*indent,
							TEXT("Supplemental item = "),
							*UFGItemDescriptor::GetItemName(generator->GetSupplementalResourceClass()).ToString()
							);
						SML::Logging::info(*getTimeStamp(), *indent, TEXT("Supplemental amount = "), generator->GetSupplementalConsumptionRateMaximum());
					}

					out_requiredOutput += generator->GetSupplementalConsumptionRateMaximum() * (
						(UFGItemDescriptor::GetForm(generator->GetSupplementalResourceClass()) == EResourceForm::RF_LIQUID ||
							UFGItemDescriptor::GetForm(generator->GetSupplementalResourceClass()) == EResourceForm::RF_GAS)
							? 60
							: 1);

					seenActors[generator].Add(generator->GetSupplementalResourceClass());
				}
				else
				{
					for (auto item : injectedItems)
					{
						if (generator->IsValidFuel(item) && !seenActors[generator].Contains(item))
						{
							if (FEfficiencyCheckerModModule::dumpConnections)
							{
								SML::Logging::info(*getTimeStamp(), *indent, TEXT("Energy item = "), *UFGItemDescriptor::GetItemName(item).ToString());
							}

							float energy = UFGItemDescriptor::GetEnergyValue(item);

							// if (UFGItemDescriptor::GetForm(out_injectedItem) == EResourceForm::RF_LIQUID)
							// {
							//     energy *= 1000;
							// }

							if (FEfficiencyCheckerModModule::dumpConnections)
							{
								SML::Logging::info(*getTimeStamp(), *indent, TEXT("Energy = "), energy);
								SML::Logging::info(*getTimeStamp(), *indent, TEXT("Current potential = "), generator->GetCurrentPotential());
								SML::Logging::info(*getTimeStamp(), *indent, TEXT("Pending potential = "), generator->GetPendingPotential());
								SML::Logging::info(*getTimeStamp(), *indent, TEXT("Power production capacity = "), generator->GetPowerProductionCapacity());
								SML::Logging::info(*getTimeStamp(), *indent, TEXT("Default power production capacity = "), generator->GetDefaultPowerProductionCapacity());
							}

							float itemAmountPerMinute = 60 / (energy / generator->GetPowerProductionCapacity());

							if (resourceForm == EResourceForm::RF_LIQUID || resourceForm == EResourceForm::RF_GAS)
							{
								itemAmountPerMinute /= 1000;
							}

							// if (fullClassName.StartsWith(TEXT("/Game/MK22k20/Buildable")))
							// {
							//     if (fullClassName.EndsWith(TEXT("MK2_C")))
							//     {
							//         itemAmountPerMinute *= 1.5;
							//     }
							//     else if (fullClassName.EndsWith(TEXT("MK3_C")))
							//     {
							//         itemAmountPerMinute *= 2;
							//     }
							//     else if (fullClassName.EndsWith(TEXT("MK4_C")))
							//     {
							//         itemAmountPerMinute *= 2.5;
							//     }
							// }

							if (FEfficiencyCheckerModModule::dumpConnections)
							{
								SML::Logging::info(
									*getTimeStamp(),
									*indent,
									*generator->GetName(),
									TEXT(" consumes "),
									itemAmountPerMinute,
									TEXT(" "),
									*UFGItemDescriptor::GetItemName(item).ToString(),
									TEXT("/minute")
									);
							}

							seenActors[generator].Add(item);
							out_requiredOutput += itemAmountPerMinute;

							break;
						}
					}
				}

				connected.Add(generator);

				return;
			}
		}

		addAllItemsToActor(seenActors, owner, injectedItems);

		// out_limitedThroughput = 0;

		if (FEfficiencyCheckerModModule::dumpConnections)
		{
			dumpUnknownClass(indent, owner);
		}

		return;
	}
}

bool AEfficiencyCheckerLogic::inheritsFrom(AActor* owner, const FString& className)
{
	for (auto cls = owner->GetClass(); cls && cls != AActor::StaticClass(); cls = cls->GetSuperClass())
	{
		if (cls->GetPathName() == className)
		{
			return true;
		}
	}

	return false;
}

void AEfficiencyCheckerLogic::dumpUnknownClass(const FString& indent, AActor* owner)
{
	if (FEfficiencyCheckerModModule::dumpConnections)
	{
		SML::Logging::info(*getTimeStamp(), *indent, TEXT("Unknown Class "), *owner->GetClass()->GetPathName());

		for (auto cls = owner->GetClass()->GetSuperClass(); cls && cls != AActor::StaticClass(); cls = cls->GetSuperClass())
		{
			SML::Logging::info(*getTimeStamp(), *indent, TEXT("    - Super: "), *cls->GetPathName());
		}

		for (TFieldIterator<UProperty> property(owner->GetClass()); property; ++property)
		{
			SML::Logging::info(
				*getTimeStamp(),
				*indent,
				TEXT("    - "),
				*property->GetName(),
				TEXT(" ("),
				*property->GetCPPType(),
				TEXT(" / "),
				*property->GetClass()->GetPathName(),
				TEXT(")")
				);

			auto floatProperty = Cast<UFloatProperty>(*property);
			if (floatProperty)
			{
				SML::Logging::info(*getTimeStamp(), *indent, TEXT("        = "), floatProperty->GetPropertyValue_InContainer(owner));
			}

			auto intProperty = Cast<UIntProperty>(*property);
			if (intProperty)
			{
				SML::Logging::info(*getTimeStamp(), *indent, TEXT("        = "), intProperty->GetPropertyValue_InContainer(owner));
			}

			auto boolProperty = Cast<UBoolProperty>(*property);
			if (boolProperty)
			{
				SML::Logging::info(*getTimeStamp(), *indent, TEXT("        = "), boolProperty->GetPropertyValue_InContainer(owner) ? TEXT("true") : TEXT("false"));
			}

			auto structProperty = Cast<UStructProperty>(*property);
			if (structProperty && property->GetName() == TEXT("mFactoryTickFunction"))
			{
				auto factoryTick = structProperty->ContainerPtrToValuePtr<FFactoryTickFunction>(owner);
				if (factoryTick)
				{
					SML::Logging::info(*getTimeStamp(), *indent, TEXT("    - Tick Interval = "), factoryTick->TickInterval);
				}
			}

			auto strProperty = Cast<UStrProperty>(*property);
			if (strProperty)
			{
				SML::Logging::info(*getTimeStamp(), *indent, TEXT("        = "), *strProperty->GetPropertyValue_InContainer(owner));
			}
		}
	}
}

void AEfficiencyCheckerLogic::DumpInformation(TSubclassOf<UFGItemDescriptor> itemDescriptorClass)
{
	if (FEfficiencyCheckerModModule::dumpConnections)
	{
		if (!itemDescriptorClass)
		{
			return;
		}

		auto className = GetFullNameSafe(itemDescriptorClass);

		SML::Logging::info(*getTimeStamp(), TEXT(" Dumping "), *className);

		auto itemDescriptor = Cast<UFGItemDescriptor>(itemDescriptorClass->GetDefaultObject());
		if (!itemDescriptor)
		{
			SML::Logging::info(*getTimeStamp(), TEXT(" Equipment small icon = "), *GetPathNameSafe(itemDescriptor->mSmallIcon));
			SML::Logging::info(*getTimeStamp(), TEXT(" Equipment big icon = "), *GetPathNameSafe(itemDescriptor->mPersistentBigIcon));
			SML::Logging::info(*getTimeStamp(), TEXT(" Equipment conveyor mesh = "), *GetPathNameSafe(itemDescriptor->mConveyorMesh));
			SML::Logging::info(*getTimeStamp(), TEXT(" Equipment item category = "), *GetPathNameSafe(itemDescriptor->mItemCategory));
		}

		auto equipmentDescriptor = Cast<UFGEquipmentDescriptor>(itemDescriptor);
		if (!equipmentDescriptor)
		{
			SML::Logging::info(*getTimeStamp(), TEXT(" Class "), *className, TEXT(" is not an Equipment Descriptor"));

			return;
		}

		SML::Logging::info(*getTimeStamp(), TEXT(" Equipment stack size = "), *getEnumItemName(TEXT("EStackSize"), (int)equipmentDescriptor->mStackSize));

		SML::Logging::info(*getTimeStamp(), TEXT(" Equipment class = "), *GetPathNameSafe(equipmentDescriptor->mEquipmentClass));

		if (!equipmentDescriptor->mEquipmentClass)
		{
			return;
		}

		auto equipment = Cast<AFGEquipment>(equipmentDescriptor->mEquipmentClass->GetDefaultObject());

		SML::Logging::info(*getTimeStamp(), TEXT(" Equipment attachment = "), *GetPathNameSafe(equipment->mAttachmentClass));
		SML::Logging::info(*getTimeStamp(), TEXT(" Equipment secondary attachment = "), *GetPathNameSafe(equipment->mSecondaryAttachmentClass));
		SML::Logging::info(*getTimeStamp(), TEXT(" Equipment slot = "), *getEnumItemName(TEXT("EEquipmentSlot"), (int)equipment->mEquipmentSlot));
		SML::Logging::info(*getTimeStamp(), TEXT(" Equipment attachment socket = "), *equipment->mAttachSocket.ToString());
		SML::Logging::info(*getTimeStamp(), TEXT(" Equipment arm animation = "), *getEnumItemName(TEXT("EArmEquipment"), (int)equipment->mArmAnimation));
		SML::Logging::info(*getTimeStamp(), TEXT(" Equipment back animation = "), *getEnumItemName(TEXT("EBackEquipment"), (int)equipment->mBackAnimation));
		SML::Logging::info(*getTimeStamp(), TEXT(" Equipment equip sound = "), *GetPathNameSafe(equipment->mEquipSound));
		SML::Logging::info(*getTimeStamp(), TEXT(" Equipment unequip sound = "), *GetPathNameSafe(equipment->mUnequipSound));
		SML::Logging::info(*getTimeStamp(), TEXT(" Equipment widget = "), *GetPathNameSafe(equipment->mEquipmentWidget));
		SML::Logging::info(*getTimeStamp(), TEXT(" Equipment use default primary fire = "), equipment->mUseDefaultPrimaryFire ? TEXT("true") : TEXT("false"));

		SML::Logging::info(*getTimeStamp(), TEXT(" Dump done"));
	}
}

bool AEfficiencyCheckerLogic::IsValidBuildable(AFGBuildable* newBuildable)
{
	if (!newBuildable)
	{
		return false;
	}

	if (auto checker = Cast<AEfficiencyCheckerBuilding>(newBuildable))
	{
		addEfficiencyBuilding(checker);

		return true;
	}
	if (Cast<AFGBuildableManufacturer>(newBuildable))
	{
		return true;
	}
	if (Cast<AFGBuildableResourceExtractor>(newBuildable))
	{
		return true;
	}
	if (Cast<AFGBuildableStorage>(newBuildable))
	{
		return true;
	}
	if (auto belt = Cast<AFGBuildableConveyorBelt>(newBuildable))
	{
		addBelt(belt);

		return true;
	}
	if (Cast<AFGBuildableConveyorBase>(newBuildable))
	{
		return true;
	}
	if (Cast<AFGBuildableConveyorAttachment>(newBuildable))
	{
		return true;
	}
	if (auto pipe = Cast<AFGBuildablePipeline>(newBuildable))
	{
		addPipe(pipe);

		return true;
	}
	if (Cast<AFGBuildablePipelineAttachment>(newBuildable))
	{
		return true;
	}
	if (Cast<AFGBuildableTrainPlatform>(newBuildable))
	{
		return true;
	}
	if (Cast<AFGBuildableRailroadStation>(newBuildable))
	{
		return true;
	}
	if (Cast<AFGBuildableDockingStation>(newBuildable))
	{
		return true;
	}
	if (Cast<AFGBuildableGeneratorFuel>(newBuildable))
	{
		return true;
	}
	if (Cast<AFGBuildableGeneratorNuclear>(newBuildable))
	{
		return true;
	}
	if (!FEfficiencyCheckerModModule::ignoreStorageTeleporter &&
		newBuildable->GetClass()->GetPathName() == TEXT("/Game/StorageTeleporter/Buildables/ItemTeleporter/ItemTeleporter_Build.ItemTeleporter_Build_C"))
	{
		addTeleporter(newBuildable);

		return true;
	}

	return false;
}

void AEfficiencyCheckerLogic::addEfficiencyBuilding(class AEfficiencyCheckerBuilding* checker)
{
	FScopeLock ScopeLock(&eclCritical);
	allEfficiencyBuildings.Add(checker);

	checker->OnEndPlay.Add(removeEffiencyBuildingDelegate);
}

void AEfficiencyCheckerLogic::removeEfficiencyBuilding(AActor* actor, EEndPlayReason::Type reason)
{
	FScopeLock ScopeLock(&eclCritical);
	allEfficiencyBuildings.Remove(Cast<AEfficiencyCheckerBuilding>(actor));

	actor->OnEndPlay.Remove(removeEffiencyBuildingDelegate);
}

void AEfficiencyCheckerLogic::addBelt(AFGBuildableConveyorBelt* belt)
{
	FScopeLock ScopeLock(&eclCritical);
	allBelts.Add(belt);

	belt->OnEndPlay.Add(removeBeltDelegate);
}

void AEfficiencyCheckerLogic::removeBelt(AActor* actor, EEndPlayReason::Type reason)
{
	FScopeLock ScopeLock(&eclCritical);
	allBelts.Remove(Cast<AFGBuildableConveyorBelt>(actor));

	actor->OnEndPlay.Remove(removeBeltDelegate);
}

void AEfficiencyCheckerLogic::addPipe(AFGBuildablePipeline* pipe)
{
	FScopeLock ScopeLock(&eclCritical);
	allPipes.Add(pipe);

	pipe->OnEndPlay.Add(removePipeDelegate);
}

void AEfficiencyCheckerLogic::removePipe(AActor* actor, EEndPlayReason::Type reason)
{
	FScopeLock ScopeLock(&eclCritical);
	allPipes.Remove(Cast<AFGBuildablePipeline>(actor));

	actor->OnEndPlay.Remove(removePipeDelegate);
}

void AEfficiencyCheckerLogic::addTeleporter(AFGBuildable* teleporter)
{
	FScopeLock ScopeLock(&eclCritical);
	allTeleporters.Add(teleporter);

	teleporter->OnEndPlay.Add(removeTeleporterDelegate);
}

void AEfficiencyCheckerLogic::removeTeleporter(AActor* actor, EEndPlayReason::Type reason)
{
	FScopeLock ScopeLock(&eclCritical);
	allTeleporters.Remove(Cast<AFGBuildable>(actor));

	actor->OnEndPlay.Remove(removeTeleporterDelegate);
}

float AEfficiencyCheckerLogic::getPipeSpeed(AFGBuildablePipeline* pipe)
{
	if (!pipe)
	{
		return 0;
	}

	return UFGBlueprintFunctionLibrary::RoundFloatWithPrecision(pipe->GetFlowLimit() * 60, 4);
}

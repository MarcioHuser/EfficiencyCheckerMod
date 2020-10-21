// ReSharper disable CppUE4CodingStandardNamingViolationWarning
// ReSharper disable CommentTypo

#include "EfficiencyCheckerHologramWall.h"
#include "EfficiencyCheckerBuilding.h"

#include "FGConstructDisqualifier.h"
#include "FGFactoryConnectionComponent.h"

#include "SML/util/Logging.h"

#include "Util/Optimize.h"

#ifndef OPTIMIZE
#pragma optimize( "", off )
#endif

AEfficiencyCheckerHologramWall::AEfficiencyCheckerHologramWall()
    : Super()
{
    this->mValidHitClasses.Add(AFGBuildableWall::StaticClass());
    this->mScrollMode = EHologramScrollMode::HSM_ROTATE;
}

// Called when the game starts or when spawned
void AEfficiencyCheckerHologramWall::BeginPlay()
{
    Super::BeginPlay();

    _TAG_NAME = GetName() + TEXT(": ");

    if (HasAuthority())
    {
        TInlineComponentArray<UWidgetComponent*> widgets;

        GetDefaultBuildable<AEfficiencyCheckerBuilding>()->GetComponents(widgets);

        for (auto widget : widgets)
        {
            widget->SetVisibilitySML(false);
        }
    }
}

bool AEfficiencyCheckerHologramWall::IsValidHitResult(const FHitResult& hitResult) const
{
    const auto defaultBuildable = GetDefaultBuildable<AEfficiencyCheckerBuilding>();

    bool ret = false;

    auto wallCheck = Cast<AFGBuildableWall>(hitResult.GetActor());

    if (defaultBuildable->resourceForm == EResourceForm::RF_SOLID &&
        wallCheck &&
        wallCheck->GetComponentByClass(UFGFactoryConnectionComponent::StaticClass()))
    {
        ret = true;
    }

    static float lastCheck = 0;

    if (GetWorld()->TimeSince(lastCheck) > 10)
    {
        lastCheck = GetWorld()->GetTimeSeconds();

        SML::Logging::info(*getTagName(), TEXT("IsValidHitResult = "), ret);

        dumpDisqualifiers();

        if (hitResult.GetActor())
        {
            SML::Logging::info(*getTagName(), TEXT("Actor = "), *hitResult.GetActor()->GetName());
            //
            // if (wall)
            // {
            //     SML::Logging::info(*getTagName(), TEXT("Is Wall"));
            //
            //     for (const auto component : wall->GetComponents())
            //     {
            //         SML::Logging::info(*getTagName(), TEXT("    "), *component->GetName(), TEXT(" / "), *component->GetClass()->GetPathName());
            //     }
            // }
        }

        SML::Logging::info(TEXT("===="));
    }

    return ret;
}

void AEfficiencyCheckerHologramWall::AdjustForGround(const FHitResult& hitResult, FVector& out_adjustedLocation, FRotator& out_adjustedRotation)
{
    static float lastCheck = 0;

    if (GetWorld()->TimeSince(lastCheck) > 10)
    {
        SML::Logging::info(*getTagName(), TEXT("Before AdjustForGround"));

        SML::Logging::info(*getTagName(), TEXT("Hologram = "), *GetName());

        FVector location = GetActorLocation();
        FRotator rotator = GetActorRotation();

        SML::Logging::info(*getTagName(), TEXT("    X = "), location.X, TEXT(" / Y = "), location.Y, TEXT(" / Z = "), location.Z);
        SML::Logging::info(*getTagName(), TEXT("    Pitch = "), rotator.Pitch, TEXT(" / Roll = "), rotator.Roll, TEXT(" / Yaw = "), rotator.Yaw);

        SML::Logging::info(*getTagName(), TEXT("Actor = "), *hitResult.GetActor()->GetName());

        location = hitResult.GetActor()->GetActorLocation();
        rotator = hitResult.GetActor()->GetActorRotation();

        SML::Logging::info(*getTagName(), TEXT("    X = "), location.X, TEXT(" / Y = "), location.Y, TEXT(" / Z = "), location.Z);
        SML::Logging::info(*getTagName(), TEXT("    Pitch = "), rotator.Pitch, TEXT(" / Roll = "), rotator.Roll, TEXT(" / Yaw = "), rotator.Yaw);
    }

    bool isSnapped = false;
    wall = nullptr;

    FVector nearestCoord;
    FVector direction;

    const auto defaultBuildable = GetDefaultBuildable<AEfficiencyCheckerBuilding>();

    if (defaultBuildable->resourceForm == EResourceForm::RF_SOLID)
    {
        wall = Cast<AFGBuildableWall>(hitResult.GetActor());

        if (wall)
        {
            nearestCoord = wall->GetActorLocation();
            direction = wall->GetActorRotation().Vector();

            TInlineComponentArray<UFGFactoryConnectionComponent*> components;
            wall->GetComponents(components);

            UFGFactoryConnectionComponent* nearestConnection = nullptr;

            for (const auto attachment : components)
            {
                if (FVector::Dist(attachment->GetComponentLocation(), hitResult.Location) <= 300 &&
                    (!nearestConnection || FVector::Dist(attachment->GetComponentLocation(), hitResult.Location) < FVector::Dist(
                        nearestConnection->GetComponentLocation(),
                        hitResult.Location
                        )))
                {
                    nearestConnection = attachment;
                }
            }

            if (nearestConnection)
            {
                out_adjustedRotation = nearestConnection->GetComponentRotation().Add(0, rotationDelta * 180, 0);
                out_adjustedLocation = FVector(nearestConnection->GetComponentLocation().X, nearestConnection->GetComponentLocation().Y, wall->GetActorLocation().Z);
                isSnapped = true;
            }
        }
    }

    if (!isSnapped)
    {
        Super::AdjustForGround(hitResult, out_adjustedLocation, out_adjustedRotation);

        wall = nullptr;
    }

    if (GetWorld()->TimeSince(lastCheck) > 10)
    {
        SML::Logging::info(*getTagName(), TEXT("After AdjustForGround"));

        dumpDisqualifiers();

        //SML::Logging::info(*getTagName(), TEXT("    After Adjusted location:  X = "), out_adjustedLocation.X, TEXT(" / Y = "), out_adjustedLocation.Y, TEXT(" / Z = "), out_adjustedLocation.Z);
        //SML::Logging::info(*getTagName(), TEXT("    After Adjusted rotation: Pitch = "), out_adjustedRotation.Pitch, TEXT(" / Roll = "), out_adjustedRotation.Roll, TEXT(" / Yaw = "), out_adjustedRotation.Yaw);

        if (hitResult.GetActor())
        {
            SML::Logging::info(*getTagName(), TEXT("Actor = "), *hitResult.GetActor()->GetName());

            //SML::Logging::info(*getTagName(), TEXT("    X = "), location.X, TEXT(" / Y = "), location.Y, TEXT(" / Z = "), location.Z);
            //SML::Logging::info(*getTagName(), TEXT("    Pitch = "), rotator.Pitch, TEXT(" / Roll = "), rotator.Roll, TEXT(" / Yaw = "), rotator.Yaw);

            FRotator rotation = direction.Rotation();

            SML::Logging::info(*getTagName(), TEXT("    Nearest location:  X = "), nearestCoord.X, TEXT(" / Y = "), nearestCoord.Y, TEXT(" / Z = "), nearestCoord.Z);
            SML::Logging::info(*getTagName(), TEXT("    Rotation: Pitch = "), rotation.Pitch, TEXT(" / Roll = "), rotation.Roll, TEXT(" / Yaw = "), rotation.Yaw);
        }

        SML::Logging::info(
            *getTagName(),
            TEXT("    Adjusted location:  X = "),
            out_adjustedLocation.X,
            TEXT(" / Y = "),
            out_adjustedLocation.Y,
            TEXT(" / Z = "),
            out_adjustedLocation.Z
            );
        SML::Logging::info(
            *getTagName(),
            TEXT("    Adjusted rotation: Pitch = "),
            out_adjustedRotation.Pitch,
            TEXT(" / Roll = "),
            out_adjustedRotation.Roll,
            TEXT(" / Yaw = "),
            out_adjustedRotation.Yaw
            );

        lastCheck = GetWorld()->GetTimeSeconds();

        SML::Logging::info(TEXT("===="));
    }
}

// bool AEfficiencyCheckerHologramWall::TrySnapToActor(const FHitResult& hitResult)
// {
//     static float lastCheck = 0;
//
//     bool ret = Super::TrySnapToActor(hitResult);
//
//     if (GetWorld()->TimeSince(lastCheck) > 10)
//     {
//         SML::Logging::info(*getTagName(), TEXT("TrySnapToActor = "), ret);
//
//         dumpDisqualifiers();
//
//         {
//             SML::Logging::info(*getTagName(), TEXT("Hologram = "), *GetName());
//
//             FVector location = GetActorLocation();
//             FRotator rotator = GetActorRotation();
//
//             SML::Logging::info(*getTagName(), TEXT("    X = "), location.X, TEXT(" / Y = "), location.Y, TEXT(" / Z = "), location.Z);
//             SML::Logging::info(*getTagName(), TEXT("    Pitch = "), rotator.Pitch, TEXT(" / Roll = "), rotator.Roll, TEXT(" / Yaw = "), rotator.Yaw);
//         }
//
//         if (hitResult.GetActor())
//         {
//             SML::Logging::info(*getTagName(), TEXT("Actor = "), *hitResult.GetActor()->GetName());
//
//             FVector location = hitResult.GetActor()->GetActorLocation();
//             FRotator rotator = hitResult.GetActor()->GetActorRotation();
//
//             SML::Logging::info(*getTagName(), TEXT("    X = "), location.X, TEXT(" / Y = "), location.Y, TEXT(" / Z = "), location.Z);
//             SML::Logging::info(*getTagName(), TEXT("    Pitch = "), rotator.Pitch, TEXT(" / Roll = "), rotator.Roll, TEXT(" / Yaw = "), rotator.Yaw);
//
//             if (ret)
//             {
//                 SML::Logging::info(*getTagName(), TEXT("    Snapping"));
//             }
//         }
//
//         lastCheck = GetWorld()->GetTimeSeconds();
//
//         SML::Logging::info(TEXT("===="));
//     }
//
//     return ret;
// }

void AEfficiencyCheckerHologramWall::SetHologramLocationAndRotation(const FHitResult& hitResult)
{
    Super::SetHologramLocationAndRotation(hitResult);

    static float lastCheck = 0;

    if (GetWorld()->TimeSince(lastCheck) > 10)
    {
        SML::Logging::info(*getTagName(), TEXT("SetHologramLocationAndRotation"));

        dumpDisqualifiers();

        lastCheck = GetWorld()->GetTimeSeconds();

        SML::Logging::info(TEXT("===="));
    }
}

void AEfficiencyCheckerHologramWall::CheckValidPlacement()
{
    static float lastCheck = 0;
    if (GetWorld()->TimeSince(lastCheck) > 10)
    {
        SML::Logging::info(*getTagName(), TEXT("Before CheckValidPlacement"));

        dumpDisqualifiers();
    }

    if (!wall)
    {
        Super::CheckValidPlacement();
    }

    if (GetWorld()->TimeSince(lastCheck) > 10)
    {
        SML::Logging::info(*getTagName(), TEXT("After CheckValidPlacement"));

        dumpDisqualifiers();

        lastCheck = GetWorld()->GetTimeSeconds();

        SML::Logging::info(TEXT("===="));
    }
}

// ReSharper disable once IdentifierTypo
void AEfficiencyCheckerHologramWall::dumpDisqualifiers() const
{
    // ReSharper disable once IdentifierTypo
    for (const auto disqualifier : mConstructDisqualifiers)
    {
        SML::Logging::info(*getTagName(), TEXT("Disqualifier "), *UFGConstructDisqualifier::GetDisqualifyingText(disqualifier).ToString());
    }
}

void AEfficiencyCheckerHologramWall::ScrollRotate(int32 delta, int32 step)
{
    static float lastCheck = 0;

    rotationDelta += delta;

    Super::ScrollRotate(delta, step);

    if (GetWorld()->TimeSince(lastCheck) > 10)
    {
        SML::Logging::info(*getTagName(), TEXT("Scroll rotate delta = "), delta, TEXT(" / step = "), step);
    }
}

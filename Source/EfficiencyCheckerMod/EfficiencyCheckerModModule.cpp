// ReSharper disable IdentifierTypo
#include "EfficiencyCheckerModModule.h"
#include "EfficiencyCheckerBuilding.h"

#include "FGBuildableFactory.h"
#include "FGGameMode.h"
#include "FGVersionFunctionLibrary.h"

#include "SML/util/Logging.h"
#include "SML/mod/hooking.h"

#include "Util/Optimize.h"

#ifndef OPTIMIZE
#pragma optimize( "", off )
#endif

bool FEfficiencyCheckerModModule::autoUpdate = true;
bool FEfficiencyCheckerModModule::dumpConnections = false;
float FEfficiencyCheckerModModule::autoUpdateTimeout = 10;
float FEfficiencyCheckerModModule::autoUpdateDistance = 5 * 800;
bool FEfficiencyCheckerModModule::ignoreStorageTeleporter = false;
bool FEfficiencyCheckerModModule::compatibleVersion = true;
int32 FEfficiencyCheckerModModule::currentGameVersion = 0;
int32 FEfficiencyCheckerModModule::compatibleGameVersion = 138229;

void FEfficiencyCheckerModModule::StartupModule()
{
    // // This is EXP branch. Check if game is EXP too
    // if (UFGVersionFunctionLibrary::GetGameVersion() != EGameVersion::GV_Experimental)
    // {
    //     // Show popup warning
    //
    //     FPopupClosed ClosedDelagate;
    //     UFGBlueprintFunctionLibrary::AddPopupWithCloseDelegate(
    //         GetLocalController(),
    //         FText::FromString(TEXT("")),
    //         FText::FromString(TEXT("")),
    //         ClosedDelagate,
    //         EPopupId::PID_OK
    //         );
    // }

    SML::Logging::info(*getTimeStamp(), TEXT(" EfficiencyChecker: StartupModule"));

    TSharedRef<FJsonObject> defaultValues(new FJsonObject());

    defaultValues->SetBoolField(TEXT("autoUpdate"), autoUpdate);
    defaultValues->SetNumberField(TEXT("autoUpdateTimeout"), autoUpdateTimeout);
    defaultValues->SetNumberField(TEXT("autoUpdateDistance"), autoUpdateDistance);
    defaultValues->SetBoolField(TEXT("dumpConnections"), dumpConnections);
    defaultValues->SetBoolField(TEXT("ignoreStorageTeleporter"), ignoreStorageTeleporter);

    defaultValues = SML::ReadModConfig(TEXT("EfficiencyChecker"), defaultValues);

    autoUpdate = defaultValues->GetBoolField(TEXT("autoUpdate"));
    autoUpdateTimeout = defaultValues->GetNumberField(TEXT("autoUpdateTimeout"));
    autoUpdateDistance = defaultValues->GetNumberField(TEXT("autoUpdateDistance"));
    dumpConnections = defaultValues->GetBoolField(TEXT("dumpConnections"));
    ignoreStorageTeleporter = defaultValues->GetBoolField(TEXT("ignoreStorageTeleporter"));

    SML::Logging::info(*getTimeStamp(), TEXT(" EfficiencyChecker: autoUpdate = "), autoUpdate ? TEXT("true") : TEXT("false"));
    SML::Logging::info(*getTimeStamp(), TEXT(" EfficiencyChecker: autoUpdateTimeout = "), autoUpdateTimeout);
    SML::Logging::info(*getTimeStamp(), TEXT(" EfficiencyChecker: autoUpdateDistance = "), autoUpdateDistance);
    SML::Logging::info(*getTimeStamp(), TEXT(" EfficiencyChecker: dumpConnections = "), dumpConnections ? TEXT("true") : TEXT("false"));
    SML::Logging::info(*getTimeStamp(), TEXT(" EfficiencyChecker: ignoreStorageTeleporter = "), ignoreStorageTeleporter ? TEXT("true") : TEXT("false"));

    // FString PatternString(TEXT("CL#(\\d+)$"));
    // FRegexPattern Pattern(PatternString);
    // FRegexMatcher Matcher(Pattern, UFGVersionFunctionLibrary::GetVersionString());
    // if (Matcher.FindNext())
    // {
    //     auto versionNumberStr = Matcher.GetCaptureGroup(1);
    // }

    currentGameVersion = FEngineVersion::Current().GetChangelist();

#if true
    // Toggle version testing:
    // true = any version will work
    // false = will lock the specific compatible version

    // Overwrite version number to the current one
    compatibleGameVersion = currentGameVersion;
#endif

    compatibleVersion = compatibleGameVersion == currentGameVersion;

    SML::Logging::info(*getTimeStamp(), TEXT(" EfficiencyChecker: "), compatibleVersion ? TEXT("is compatible game version") : TEXT("not a compatible game version"));

    if (autoUpdate)
    {
        SML::Logging::info(*getTimeStamp(), TEXT(" EfficiencyChecker: Hooking AFGBuildableFactory::SetPendingPotential"));

        SUBSCRIBE_METHOD_AFTER(
            AFGBuildableFactory::SetPendingPotential,
            [](AFGBuildableFactory * factory, float potential)
            {
            AEfficiencyCheckerBuilding::setPendingPotentialCallback(factory, potential);
            }
            );

        // SUBSCRIBE_VIRTUAL_FUNCTION_AFTER(
        //     AFGBuildableFactory,
        //     AFGBuildableFactory::SetPendingPotential,
        //     [](AFGBuildableFactory * factory, float potential)
        //     {
        //     setPendingPotentialCallback(factory, potential);
        //     }
        //     );

        // SML::Logging::info(*getTimeStamp(), TEXT(" EfficiencyCheckerBuilding: Hooking AFGBuildableGeneratorFuel::SetPendingPotential"));
        //
        // SUBSCRIBE_VIRTUAL_FUNCTION_AFTER(
        //     AFGBuildableGeneratorFuel,
        //     AFGBuildableGeneratorFuel::SetPendingPotential,
        //     [](AFGBuildableGeneratorFuel * factory, float potential)
        //     {
        //     setPendingPotentialCallback(factory, potential);
        //     }
        //     );
    }

    // SUBSCRIBE_VIRTUAL_FUNCTION(
    //     AFGGameMode,
    //     AFGGameMode::PostLogin,
    //     [](auto& scope, AFGGameMode* gm, APlayerController* pc) {
    //     if (gm && gm->HasAuthority() && !gm->IsMainMenuGameMode()) {
    //     gm->RegisterRemoteCallObjectClass(UEfficiencyCheckerRCO::StaticClass());
    //     }
    //     }
    //     );

    SML::Logging::info(*getTimeStamp(), TEXT(" ==="));
}

IMPLEMENT_GAME_MODULE(FEfficiencyCheckerModModule, EfficiencyCheckerMod);

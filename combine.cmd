pushd "%~dp0\Mods\EfficiencyCheckerMod"

copy /y ..\..\Content\EfficiencyCheckerMod\Icons\effcheck.png effcheck.png

echo {"resources"^:{"icon"^:"effcheck.png"}} > "resources.json"

jq --tab -s ".[0] * .[1]" "data.json" "resources.json" > "data-fixed.json"

move /y "data-fixed.json" "data.json"

del /q "resources.json"

popd

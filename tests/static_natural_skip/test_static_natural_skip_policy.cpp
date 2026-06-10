#include "static_natural_skip_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

bool expectTrue(const char* name, bool value)
{
    if (!value) {
        std::cerr << "expected true: " << name << "\n";
        return false;
    }
    return true;
}

bool expectFalse(const char* name, bool value)
{
    if (value) {
        std::cerr << "expected false: " << name << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    bool ok = true;

    StaticNaturalSkipFacts pineStatic;
    pineStatic.naturalRepresentation = true;
    pineStatic.registeredStatic = true;
    ok &= expectTrue("Pine3 registered static -> skip",
                     isPureStaticNaturalSkipCandidate(pineStatic));

    StaticNaturalSkipFacts pineUnregistered = pineStatic;
    pineUnregistered.registeredStatic = false;
    ok &= expectFalse("Pine3 unregistered -> do not skip",
                      isPureStaticNaturalSkipCandidate(pineUnregistered));

    StaticNaturalSkipFacts pineFalling = pineStatic;
    pineFalling.falling = true;
    ok &= expectFalse("Pine3 falling -> do not skip",
                      isPureStaticNaturalSkipCandidate(pineFalling));

    StaticNaturalSkipFacts pineJustCreated = pineStatic;
    pineJustCreated.justCreated = true;
    ok &= expectFalse("Pine3 justCreated -> do not skip",
                      isPureStaticNaturalSkipCandidate(pineJustCreated));

    StaticNaturalSkipFacts sensorTower = pineStatic;
    sensorTower.sensor = true;
    ok &= expectFalse("SensorTower -> do not skip",
                      isPureStaticNaturalSkipCandidate(sensorTower));

    StaticNaturalSkipFacts perimeterAlarm = pineStatic;
    perimeterAlarm.perimeterAlarm = true;
    ok &= expectFalse("PerimeterAlarm -> do not skip",
                      isPureStaticNaturalSkipCandidate(perimeterAlarm));

    StaticNaturalSkipFacts rockStatic = pineStatic;
    ok &= expectTrue("rock_gclump registered static -> skip",
                     isPureStaticNaturalSkipCandidate(rockStatic));

    StaticNaturalSkipFacts fullBake = pineStatic;
    fullBake.fullBakeNextFrame = true;
    ok &= expectFalse("fullBakeNextFrame -> do not skip",
                      isPureStaticNaturalSkipCandidate(fullBake));

    StaticNaturalSkipFacts special = pineStatic;
    special.specialBuilding = true;
    ok &= expectFalse("special -> do not skip",
                      isPureStaticNaturalSkipCandidate(special));

    StaticNaturalSkipFacts lookout = pineStatic;
    lookout.lookout = true;
    ok &= expectFalse("lookout -> do not skip",
                      isPureStaticNaturalSkipCandidate(lookout));

    StaticNaturalSkipFacts control = pineStatic;
    control.controlBuilding = true;
    ok &= expectFalse("control -> do not skip",
                      isPureStaticNaturalSkipCandidate(control));

    StaticNaturalSkipFacts mechBay = pineStatic;
    mechBay.mechBay = true;
    ok &= expectFalse("mechBay -> do not skip",
                      isPureStaticNaturalSkipCandidate(mechBay));

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

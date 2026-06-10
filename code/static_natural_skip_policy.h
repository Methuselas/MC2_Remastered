#pragma once

// Static-natural skip policy for BUILDING-backed render props.
//
// Some natural props in MC2 are authored as BUILDING objects even though they
// are pure render-static set dressing at runtime. They are safe to skip only
// when they are static/registered and do not carry service roles such as
// alarms, sensor coverage, lookout vision, power, control, or mech-bay logic.
// Falling and just-created objects always stay in the update path so run-over
// trees and first-frame initialization continue to wake correctly.
struct StaticNaturalSkipFacts {
    bool naturalRepresentation = false;
    bool registeredStatic = false;
    bool shapeMismatch = false;
    bool fullBakeNextFrame = false;
    bool falling = false;
    bool justCreated = false;
    bool specialBuilding = false;
    bool perimeterAlarm = false;
    bool lookout = false;
    bool sensor = false;
    bool powerSource = false;
    bool controlBuilding = false;
    bool mechBay = false;
};

inline bool isPureStaticNaturalServiceFree(const StaticNaturalSkipFacts& f)
{
    return !f.specialBuilding &&
           !f.perimeterAlarm &&
           !f.lookout &&
           !f.sensor &&
           !f.powerSource &&
           !f.controlBuilding &&
           !f.mechBay;
}

inline bool isPureStaticNaturalSkipCandidate(const StaticNaturalSkipFacts& f)
{
    return f.naturalRepresentation &&
           f.registeredStatic &&
           !f.shapeMismatch &&
           !f.fullBakeNextFrame &&
           !f.falling &&
           !f.justCreated &&
           isPureStaticNaturalServiceFree(f);
}

//==========================================================================//
// File:    spawn.cpp                                                        //
// Contents: mc2::particles::Spawn polymorphic dispatcher implementation.    //
//           Plan v6 §5.4 B1 Stage 2' C7-revised.                            //
//===========================================================================//

#include "gosfx/gosfxheaders.hpp"
#include "gosfx/card.hpp"
#include "gosfx/pointcloud.hpp"
#include "gosfx/shardcloud.hpp"
#include "gosfx/tube.hpp"

#include "spawn.h"
#include "spawn_card.h"
#include "spawn_point.h"
#include "spawn_shard.h"
#include "spawn_tube.h"

namespace mc2 {
namespace particles {

bool Spawn(gosFX::Effect::Specification* spec,
           const Stuff::LinearMatrix4D*  parentToWorld,
           float                          spawnSeed)
{
    if (!spec) {
        return false;
    }

    // GetClassID() Check_Object's the spec internally (see effect.hpp:112).
    const Stuff::RegisteredClass::ClassID id = spec->GetClassID();

    switch (id) {
        case gosFX::CardClassID:
            SpawnCard(static_cast<const gosFX::Card__Specification*>(spec),
                      parentToWorld, spawnSeed);
            return true;
        case gosFX::PointCloudClassID:
            SpawnPoint(static_cast<const gosFX::PointCloud__Specification*>(spec),
                       parentToWorld, spawnSeed);
            return true;
        case gosFX::ShardCloudClassID:
            SpawnShard(static_cast<const gosFX::ShardCloud__Specification*>(spec),
                       parentToWorld, spawnSeed);
            return true;
        case gosFX::TubeClassID:
            SpawnTube(static_cast<const gosFX::Tube__Specification*>(spec),
                      parentToWorld, spawnSeed);
            return true;
        default:
            // Pert / Shape / Debris / EffectCloud / unknown - B2 deferred.
            return false;
    }
}

}  // namespace particles
}  // namespace mc2

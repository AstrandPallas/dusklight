#pragma once
#include "dusk/coop.h"

namespace dusk::coop {

// Called once per game-logic frame from duskExecute().
void tick();

}  // namespace dusk::coop

#include "dusk/coop/coop_overlay.hpp"

#include "dusk/coop.h"
#include "dusk/settings.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <chrono>

namespace dusk::coop {
namespace {

using clock = std::chrono::steady_clock;

// How long the tags stay visible after the split (re-)activates. The layout
// is fixed (P1 top / P2 bottom), so a short reminder beats a permanent HUD
// element — but every fresh activation (join, unstash, auto-rejoin) re-shows
// them, since stash restores teleport P2 to P1's side.
constexpr auto kTagShowDuration = std::chrono::seconds(5);

clock::time_point s_shownSince{};
bool s_wasSplitActive = false;

}  // namespace

void drawPlayerTags(Rml::ElementDocument* document) {
    if (document == nullptr) {
        return;
    }

    const bool active = isSplitActive();
    if (active && !s_wasSplitActive) {
        s_shownSince = clock::now();
    }
    s_wasSplitActive = active;

    const bool shown = active && clock::now() - s_shownSince < kTagShowDuration;

    // Element IDs declared in the overlay document source (overlay.cpp) and
    // styled/positioned in res/rml/overlay.rcss.
    if (auto* tag = document->GetElementById("coop-tag-p1")) {
        // The FPS counter occupies the window's top-left corner when its
        // corner setting is "tl" (index 0, see kFpsCorners in overlay.cpp) —
        // shift P1's tag below it so the two boxes don't stack.
        const bool belowFps = getSettings().video.enableFpsOverlay.getValue() &&
                              getSettings().video.fpsOverlayCorner.getValue() == 0;
        tag->SetClass("below-fps", belowFps);
        tag->SetClass("shown", shown);
    }
    if (auto* tag = document->GetElementById("coop-tag-p2")) {
        tag->SetClass("shown", shown);
    }
}

}  // namespace dusk::coop

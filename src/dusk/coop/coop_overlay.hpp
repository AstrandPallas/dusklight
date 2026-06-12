#pragma once

namespace Rml {
class ElementDocument;
}  // namespace Rml

namespace dusk::coop {

// Updates the per-viewport "P1"/"P2" labels living in the persistent UI
// overlay document (see dusk::ui::Overlay). Called once per frame from
// Overlay::update(); shows the tags while the split is active and fades
// them out after a few seconds.
void drawPlayerTags(Rml::ElementDocument* document);

}  // namespace dusk::coop

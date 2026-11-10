// SPDX-License-Identifier: GPL-3.0-or-later
// Transmog: an armor piece shows another piece's look (or none) while keeping its own stats.
// The look fields of the piece's EquipParamProtector row (model id / category / gender, the body
// parts it hides, optionally its sounds) are copied from the chosen piece's row in the live params;
// the choice applies to the item (every copy of it, anyone wearing it) and is kept in transmog.ini
// next to bbport.ini. Chosen in the guide's Transmog tab (Insert / L3+R3).
#pragma once

namespace BbEngine::Transmog {
/// Main thread (the frame tick): writes the saved looks into the live params, refreshes the
/// player's model when an equipped piece's look changed.
void Tick();
/// The guide's Transmog tab (render thread).
void DrawPage();
}  // namespace BbEngine::Transmog

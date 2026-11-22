/*
 * KiCadD: where a connector opens (USB, RJ45, a DC jack ...) and where its footprint wants the
 * board edge, for auto_place's edge rules: the connector's opening faces the edge and what is
 * beyond the edge line hangs over.
 *
 *   marked     the footprint draws the board edge: a "PCB Edge" (or "Board edge") text on
 *              Dwgs.User / Cmts.User next to a straight line on the same layer (KiCad's
 *              library does this for many connectors): that line is the edge
 *   by shape   else the side where the courtyard reaches well past the pads (1.5 mm, twice any
 *              other side): the body of a socket with its pins at the back; the courtyard's
 *              front goes on the edge (flush), or overhang mm past it
 *
 * All in the footprint's frame at rotation 0 relative to its anchor, as auto_place's model.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <optional>
#include <string>

class FOOTPRINT;

namespace kicadd::mouth
{

struct MOUTH
{
    int         side = -1;        ///< kicadd::autoplace::EDGE_LEFT .. EDGE_BOTTOM; -1: none found
    int         edgeLine = 0;     ///< nm from the anchor towards side: where the board edge goes
    int         front = 0;        ///< nm from the anchor towards side: the courtyard's far end
    bool        marked = false;   ///< from the footprint's own edge line
    bool        byPads = false;   ///< from its pads: signal pins at the back, mounting pads at the mouth
    std::string error;            ///< found but unusable (pads past the edge line)
};


/// aOverhang (mm, given by the agent): the courtyard's front this far past the edge, in place
/// of the footprint's own line or flush.
MOUTH Find( const FOOTPRINT& aFp, std::optional<double> aOverhang );

/// "left", "right", "top", "bottom"
const char* SideName( int aSide );

} // namespace kicadd::mouth

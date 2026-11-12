/*
 * KiCadD: the board's design rules as one value (net classes, net class assignments by
 * pattern, the board's minimum constraints), to read, change and put back (get_rules /
 * set_rules, with the agent's undo).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <map>
#include <optional>
#include <utility>
#include <vector>

#include <wx/string.h>

class BOARD;

namespace kicadd::rules
{

/// A net class's own values (nm); unset: taken from the Default class.
struct CLASS
{
    std::optional<int> clearance, trackWidth, viaDiameter, viaDrill, diffPairWidth, diffPairGap;

    bool operator==( const CLASS& ) const = default;
};


struct STATE
{
    std::map<wxString, CLASS>                   classes;    ///< "Default" included
    std::vector<std::pair<wxString, wxString>>  patterns;   ///< net name pattern -> class, in order

    int minClearance = 0, minTrackWidth = 0, minViaDiameter = 0, minThroughDrill = 0, minAnnularWidth = 0,
        holeClearance = 0, holeToHole = 0, copperEdgeClearance = 0, minSilkTextHeight = 0,
        minSilkTextThickness = 0;

    bool operator==( const STATE& ) const = default;
};


STATE Capture( BOARD& aBoard );

/// Make the board's rules aState, then re-resolve every net's class.
void Apply( BOARD& aBoard, const STATE& aState );

} // namespace kicadd::rules

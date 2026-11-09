/*
 * KiCadD M5c: automatic placement.
 *
 * A deterministic placer for the parts an agent has just brought onto a board (sync_pcb puts
 * them outside the outline) or for all unlocked parts:
 *
 *   1. global: every part is pulled to the weighted centre of the pins it connects to (clique
 *      weights 1/(k-1); nets touching a large share of the parts, typically GND, are left out
 *      so they don't collapse everything into one point), small passives additionally to the
 *      nearest pin of the biggest part they share a net with (decoupling caps go to their
 *      supply pin, pull-ups to their signal)
 *   2. legal: biggest parts first, each at the free spot nearest its target: courtyards apart
 *      by `spacing`, inside the board outline by `edge`, clear of the fixed parts
 *   3. refine: each part in turn tries the legal spot nearest its connection centre in all four
 *      rotations; kept when the total wire length (half-perimeter per net) goes down
 *
 * Works on a plain model (no BOARD) so it can be tested and reasoned about on its own; the
 * handler builds the model from the board and applies the result as one undoable edit.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <string>
#include <vector>

#include <math/box2.h>
#include <math/vector2d.h>
#include <geometry/shape_poly_set.h>

namespace kicadd::autoplace
{

struct PAD
{
    VECTOR2I offset;      ///< from the anchor, at rotation 0
    int      net = -1;    ///< index into the net list, -1 = no net
};

struct PART
{
    std::string      ref;
    bool             movable = true;
    VECTOR2I         pos;             ///< anchor
    int              rot = 0;         ///< quarter turns (counter-clockwise, KiCad's sense)
    std::vector<VECTOR2I> outline;    ///< courtyard (or body) points relative to the anchor, rotation 0
    std::vector<VECTOR2I> copper;     ///< pad extents, same frame: what must clear the obstacles
    std::vector<PAD> pads;
};

struct OPTIONS
{
    int  spacing = 250000;     ///< courtyard to courtyard, nm
    int  edge = 500000;        ///< courtyard to board edge, nm
    int  grid = 250000;        ///< placement grid, nm
    bool rotate = true;
    int  refinePasses = 6;
    int  starts = 4;           ///< independent starts (run in parallel), the best kept

    /// Areas no pad may touch: copper graphics and text on the board, tracks, vias, footprint
    /// keep-outs (already grown by the clearance they need). Silk may go over them.
    std::vector<BOX2I> obstacles;
};

struct RESULT
{
    double                   hpwlBefore = 0;   ///< mm, nets with movable parts, before (only parts inside)
    double                   hpwlAfter = 0;    ///< mm
    std::vector<std::string> unplaced;          ///< no room left
    int                      moved = 0;
};

/**
 * Place the movable parts of @a aParts (positions and rotations updated in place).
 * @param aNetCount  nets are 0 .. aNetCount-1
 * @param aOutline   the board outline (Edge.Cuts polygons)
 */
RESULT Place( std::vector<PART>& aParts, int aNetCount, const SHAPE_POLY_SET& aOutline, const OPTIONS& aOptions );

/// Total half-perimeter wire length, mm, over nets with at least two pads (all nets).
double Hpwl( const std::vector<PART>& aParts, int aNetCount );

} // namespace kicadd::autoplace

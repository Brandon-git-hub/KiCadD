/*
 * KiCadD: silkscreen tidying — references (and values) moved off pads, off each other and onto
 * the board, next to their part.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicadd/board.pb.h>

#include <geometry/eda_angle.h>
#include <math/vector2d.h>

#include <string>
#include <vector>

class BOARD;
class FOOTPRINT;
class PCB_FIELD;

namespace kicadd
{

struct SILK_MOVE
{
    FOOTPRINT*  fp;
    PCB_FIELD*  field;
    VECTOR2I    pos;
    EDA_ANGLE   angle;
    std::string text;   // "R5 reference (88.9, 112.3) -> (88.9, 109.9), above"
};

struct SILK_PLAN
{
    std::vector<SILK_MOVE>   moves;
    std::vector<std::string> stuck;   // no free place found
    int                      checked = 0;
    int                      problemsBefore = 0;
    int                      problemsAfter = 0;   // as planned
    int                      nearerBefore = 0;    // texts nearer another part's courtyard than their own
    int                      nearerAfter = 0;     // as planned
    std::vector<std::string> nearer;              // those, as planned: "C3 reference: 0.2 mm from C4, 0.6 mm from its own"
};

/// Where the texts @a aReq asks about should go. Nothing is changed.
SILK_PLAN PlanSilk( BOARD& aBoard, const kicadd::board::SilkTidy& aReq );

} // namespace kicadd

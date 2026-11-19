/*
 * KiCadD: reference-plane checks for the board audit (roadmap phase 2, item 1). Pure geometry on
 * the board's tracks, vias and filled zones; nothing is changed.
 *
 * - plane: a signal track's reference planes are the copper zones on the nearest copper layers
 *   above and below it (stack order). Along each track the check follows which plane nets are
 *   under it: a track that goes from one plane net to another (a split) has its return current
 *   cut; a stretch with no plane under it (a void or past the plane's edge) longer than plane_gap_mm
 *   is reported too.
 * - return_via: a signal via that moves a track to a layer with other reference planes needs a
 *   return path next to it: a via (or through-hole pad) of the plane net joining the two planes
 *   within return_via_mm, or, when the planes are different nets, a capacitor between them.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicadd/board.pb.h>

class BOARD;

namespace kicadd
{

/// The reference-plane findings of the audit ("plane", "return_via") into @a aOut.
void CheckReferencePlanes( const BOARD& aBoard, const kicadd::board::BoardAudit& aReq,
                           kicadd::board::BoardAuditResponse& aOut );

} // namespace kicadd

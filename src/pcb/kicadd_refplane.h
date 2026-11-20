/*
 * KiCadD: reference-plane checks for the board audit (roadmap phase 2, item 1). Pure geometry on
 * the board's tracks, vias and filled zones; nothing is changed.
 *
 * Made for an agent: without limits it measures and summarises (per net, worst first, plane_limit
 * rows) and judges nothing; the agent passes its own limits as net globs -> mm.
 *
 * - plane: a signal track's reference planes are its layer's planes as the impedance tool has
 *   them (impedance::Line: the nearest plane layer each way by its poured copper, signal layers
 *   between passed over; the nearest copper layers on a board with no plane yet), their fills.
 *   Along each track: where it goes from one plane net to another (a split), stretches with no
 *   plane under it (a gap in the fill, or past its edge), and stretches over a local pour on a
 *   signal layer between the track and its plane (the high-frequency return is drawn onto it:
 *   an impedance step; said, never judged). With plane_gap {glob: mm}: splits, and stretches
 *   longer than the mm, as warnings.
 * - return_via: a signal via that moves a track to a layer with other reference planes: the
 *   nearest via (or through-hole pad) of the plane net joining the two planes, the nearest on the
 *   other side of the via (or of the pair's two vias), whether it is in a BGA's field and how many
 *   plane vias are there; between different plane nets, the nearest capacitor between them. With
 *   return_via {glob: mm} (two_sided, bga_return_via_mm): the vias over the limit as warnings.
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

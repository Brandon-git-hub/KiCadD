/*
 * KiCadD: parallel-track crosstalk pre-screen for the board audit (roadmap phase 2, item 4).
 * Measures only: no routing.
 *
 * On each copper layer, signal tracks of two different nets that run side by side (within 10
 * degrees) with an edge-to-edge gap under 3 H, H being the dielectric to the nearest plane, as the
 * impedance tool takes it (planes by their poured copper; signal layers between passed over, and
 * signal layers next to each other named: broadside coupling is not screened). Per net pair: the length coupled, per
 * layer, and the closest gap (in mm and in H). Diff pair partners together are no aggressor and
 * victim (a pair is one name, "X_P/N (pair)"); supplies and grounds are left out.
 *
 * Made for an agent, like kicadd_refplane: without limits a header with the longest coupled pair,
 * no verdicts; xtalk_limit / xtalk_nets give rows. Limits from the agent: xtalk_length
 * {glob: mm}, the spacing as a multiple of H (xtalk_spacing, default 3).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicadd/board.pb.h>

class BOARD;

namespace kicadd
{

/// The crosstalk findings of the audit ("crosstalk") into @a aOut.
void CheckCrosstalk( BOARD& aBoard, const kicadd::board::BoardAudit& aReq, kicadd::board::BoardAuditResponse& aOut );

} // namespace kicadd

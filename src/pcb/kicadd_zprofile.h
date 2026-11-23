/*
 * KiCadD: impedance along the line for the board audit (roadmap phase 2, item 5). Measures only:
 * no routing.
 *
 * Each signal track, piece by piece: its width, its layer and the copper over and under that spot
 * give its cross-section. The planes are impedance::Planes() (by their poured copper); where a
 * pour on a signal layer between lies under the track, that pour is its plane there (the return
 * current takes the nearest copper), and where a plane has a gap the next copper past it is. With
 * no copper on either side the spot is off plane (no Z: the plane check says where). A diff pair
 * takes its Zdiff at the gap to its partner there (every 0.25 mm; uncoupled: twice Z0, said as
 * "uncoupled" with how far its partner is, and counted per pair). The header names each layer's
 * reference planes, the ones the impedance tool and set_rules use. The
 * arithmetic is kicadd_zcalc's (KiCad's transmission line calculations). Vias and pads are not in
 * it, nor coplanar ground on the track's own layer.
 *
 * Made for an agent, like the other audit checks: without targets one header line (the nets that
 * vary most from their own main Z), no verdicts; z_limit / z_nets give rows. Targets from the
 * agent: z_target {glob: ohm} (Z0), z_diff_target {glob: ohm} (Zdiff), z_tolerance (%), z_off_mm.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicadd/board.pb.h>

class BOARD;

namespace kicadd
{

/// The impedance-profile findings of the audit ("impedance") into @a aOut.
void CheckImpedance( BOARD& aBoard, const kicadd::board::BoardAudit& aReq, kicadd::board::BoardAuditResponse& aOut );

} // namespace kicadd

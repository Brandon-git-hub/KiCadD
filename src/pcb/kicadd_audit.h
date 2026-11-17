/*
 * KiCadD: the board half of the design review (audit) — current, voltage, silkscreen, test points.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicadd/board.pb.h>

class BOARD;

namespace kicadd
{

/// Copper thickness (um) of the board's outer and inner layers from its stackup (35 / 0 without one).
void BoardCopperUm( const BOARD& aBoard, double& aOuter, double& aInner );

/// Review @a aBoard as @a aReq asks; findings into @a aOut.
void AuditBoard( BOARD& aBoard, const kicadd::board::BoardAudit& aReq, kicadd::board::BoardAuditResponse& aOut );

} // namespace kicadd

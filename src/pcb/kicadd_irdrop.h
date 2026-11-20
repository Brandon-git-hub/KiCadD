/*
 * KiCadD: DC IR drop for the board audit (roadmap phase 2, item 3). Measures only.
 *
 * Each supply rail becomes a resistor network: the zone fills and pads on a grid of square cells
 * (each cell side to side is one square of that layer's copper), tracks as one resistor each
 * (length / width squares), vias and plated holes as their barrel (20 um plating) between the
 * layers they connect. The current enters at the source pads (one node at 0 V) and leaves at the
 * load pads; the network is solved for the drop at every node.
 *
 * Made for an agent, like kicadd_refplane: without limits a header with the worst rail (per A when
 * no current is given) and no verdicts; ir_limit / ir_nets give a row per rail (the drop at the
 * worst load, where the losses are, the busiest via and the most loaded track). The source is
 * guessed when not given (a power_out / VOUT pin, else an inductor, else a connector) and said so;
 * ir_source / ir_loads set it. Limits from the agent: ir_drop {glob: mV}, via_current {glob: A}.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicadd/board.pb.h>

class BOARD;

namespace kicadd
{

/// The IR drop findings of the audit ("ir_drop") into @a aOut.
void CheckIrDrop( BOARD& aBoard, const kicadd::board::BoardAudit& aReq, kicadd::board::BoardAuditResponse& aOut );

} // namespace kicadd

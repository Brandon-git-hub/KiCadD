/*
 * KiCadD - local (incremental) DRC around a set of footprints.
 *
 * Instead of running every DRC provider over the whole board, test only the pairs whose
 * result can have changed: items of the given footprints against copper and courtyards
 * within the board's worst-case clearance. Rules come from the board's DRC_ENGINE, so
 * custom rules / netclasses / net ties are honoured exactly as in full DRC.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_LOCAL_DRC_H
#define KICADD_LOCAL_DRC_H

#include <vector>

#include <kicadd/board.pb.h>

class BOARD;
class FOOTPRINT;

namespace kicadd
{

/// Test aFootprints against their neighbourhood (and each other). Zone fills are not
/// tested: they are stale until refilled and would report phantom shorts.
std::vector<board::DrcViolation> RunLocalDrc( BOARD* aBoard, const std::vector<FOOTPRINT*>& aFootprints );

} // namespace kicadd

#endif // KICADD_LOCAL_DRC_H

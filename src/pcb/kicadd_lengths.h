/*
 * KiCadD: diff pair and length checks for the board audit (roadmap phase 2, item 2). Measures
 * only: no routing, no meanders (the human tunes with KiCad's own tools on the desktop).
 *
 * Made for an agent, like kicadd_refplane: without limits it measures and summarises (worst
 * first, length_limit rows) and judges nothing; the agent passes its own limits as net globs -> mm.
 *
 * - diff_pair: each routed pair (X+ / X-, X_P / X_N with the partner on the board): both members'
 *   lengths (as KiCad's net inspector: tracks, via spans, pad-to-die) and the skew; along the
 *   tracks, the gap to the partner on the same layer (coupled where it is near the target: the net
 *   class's diff pair gap, else the pair's median), the uncoupled track and its longest stretch,
 *   widths, vias. With skew / uncoupled / pair_gap {glob: mm}: the pairs over them as warnings.
 * - length: length_nets [globs] list nets' lengths; length_match {glob: mm} makes each glob's nets
 *   a group (a pair counts as its mean) and judges longest minus shortest, naming what each short
 *   net lacks; max_length / min_length {glob: mm} judge each net.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicadd/board.pb.h>

#include <map>
#include <optional>
#include <set>
#include <string>

#include <math/vector2d.h>

class BOARD;
class FOOTPRINT;

namespace kicadd
{

/// The diff pair and length findings of the audit ("diff_pair", "length") into @a aOut.
void CheckLengths( BOARD& aBoard, const kicadd::board::BoardAudit& aReq, kicadd::board::BoardAuditResponse& aOut );

/// The board's diff pairs (X+ / X-, X_P / X_N, both members on the board): P net code -> N.
std::map<int, int> DiffPairNets( BOARD& aBoard );

struct PAIR_ORDER
{
    int                     checked = 0;   ///< pairs on two or more parts
    int                     crossed = 0;   ///< of them, P and N swapped between two parts
    std::optional<VECTOR2I> at;            ///< the first crossing
    std::string             line;          ///< "pair order: ..." ("" when nothing was checked)
};

/// Between two parts a pair's P and N must not swap sides (the lines P-P and N-N crossing).
/// aOnly: only the pairs and part pairs that involve one of these footprints.
PAIR_ORDER CheckPairOrder( BOARD& aBoard, const std::map<int, int>& aPairs,
                           const std::set<const FOOTPRINT*>* aOnly = nullptr );

} // namespace kicadd

/*
 * KiCadD: draw tracks the way the interactive router does (KiCad's push-and-shove router in
 * walkaround mode: 45-degree tracks around obstacles, the board's clearance rules, keepouts and
 * edge clearance), without a canvas. Not an autorouter: the caller says what to connect, and
 * through which points / vias; existing copper is never moved.
 *
 * A SESSION syncs the router's world from the board once and then routes any number of
 * connections; each new one sees the ones before. The tracks are not put on the board: the
 * caller takes them (one commit, one undo) or drops them (dry run).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <layer_ids.h>
#include <math/vector2d.h>

#include <kicadd/board.pb.h>

class BOARD;
class BOARD_CONNECTED_ITEM;
class PCB_TRACK;

namespace kicadd::route
{

struct STOP
{
    VECTOR2I     pos;
    PCB_LAYER_ID viaTo = UNDEFINED_LAYER;   ///< a via here, then on this layer; undefined: a waypoint
    bool         otherSide = false;         ///< a via here, then on the end's layer / the other outer layer
};


struct REQUEST
{
    BOARD_CONNECTED_ITEM* fromItem = nullptr;   ///< a pad, track or via
    VECTOR2I              from;
    BOARD_CONNECTED_ITEM* toItem = nullptr;     ///< null: route to the point `to` (a free end)
    VECTOR2I              to;
    std::vector<STOP>     stops;
    PCB_LAYER_ID          layer = UNDEFINED_LAYER;   ///< undefined: chosen from the ends
    int                   width = 0;                 ///< 0: the net class's / rules' width
    PCB_LAYER_ID          endViaTo = UNDEFINED_LAYER;   ///< end in a via down to this layer (fanout)
};


struct RESULT
{
    bool        ok = false;
    std::string error;          ///< why not (ok == false)
    std::string net;
    std::string layer;          ///< the layers in route order: "B.Cu", "B.Cu -> F.Cu"
    int         width = 0;
    long long   length = 0;     ///< nm, along the new tracks
    long long   direct = 0;     ///< nm, from -> stops -> to in straight lines
    int         segments = 0;
    int         vias = 0;
    int         minGap = -1;    ///< nm to the nearest other-net copper or the edge (-1: none within 5 mm)
    std::string gapTo;          ///< what that is
    std::optional<VECTOR2I> reached;     ///< failed: how far the route got
    std::string             blockedBy;   ///< failed: what is in the way there
    std::optional<VECTOR2I> blockedAt;
    std::vector<board::DrcViolation> drc;   ///< clearance / edge violations of the new tracks
};


class SESSION
{
public:
    explicit SESSION( BOARD* aBoard );
    ~SESSION();

    /// Route one connection. On success its tracks join the session (later routes go around
    /// them); on failure nothing is kept.
    RESULT Route( const REQUEST& aRequest );

    /// A through via of aNetCode at aPos, checked as a route's tracks are (clearance to other-net
    /// copper, hole to hole, the board edge and cut-outs, earlier vias of the session). Kept in
    /// the session when it breaks none of them.
    RESULT PlaceVia( const VECTOR2I& aPos, int aNetCode, int aDiameter, int aDrill );

    /// The new tracks / arcs / vias (not on the board; the caller owns them after this).
    std::vector<std::unique_ptr<PCB_TRACK>> TakeAdded();

    /// Board tracks the router replaced (e.g. merged into a new one): to remove with the add.
    std::vector<PCB_TRACK*> Replaced() const;

private:
    struct IMPL;
    std::unique_ptr<IMPL> m_impl;
};


/// "U1.5 [GND]", "track VCC", "via GND", "keepout", "board edge" ...
std::string Describe( const BOARD_CONNECTED_ITEM* aItem );

} // namespace kicadd::route

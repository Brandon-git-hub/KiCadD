/*
 * KiCadD: a rail's decoupling as the board has it (roadmap phase 3, item 3: PDN Z(f)). For each
 * capacitor between the rail and its ground: how it reaches the planes (tracks from its pads,
 * vias down to the rail's and the ground's plane layers) and how far it is from the load through
 * the plane pair; the plane pair's own capacitance; the load's vias. Inductances are estimates
 * from the geometry with textbook formulas (a track: Z0 x delay of its layer; a via pair: the
 * two-wire loop (mu0 h / 2 pi) ln(s / r) per via; the plane pair from the capacitor to the load:
 * (mu0 d / 2 pi) ln(D / r)), each said where it comes from. The pdn tool (MCP) adds the parts'
 * ESR / ESL / C and works out Z(f).
 *
 * Network and plane-pair geometry only: no field solver, no cavity resonances.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <string>

#include <kicadd/board.pb.h>

class BOARD;

namespace kicadd
{

/// aOut for aReq; "" or why not (an unknown net, no capacitors on it ...).
std::string PdnGeometry( BOARD& aBoard, const kicadd::board::PdnGeometry& aReq, kicadd::board::PdnGeometryResponse& aOut );

/// A net's copper as a graph of transmission lines (see NetTopology in board.proto).
std::string NetTopology( BOARD& aBoard, const kicadd::board::NetTopology& aReq, kicadd::board::NetTopologyResponse& aOut );

} // namespace kicadd

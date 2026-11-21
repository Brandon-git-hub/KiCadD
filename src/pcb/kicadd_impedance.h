/*
 * KiCadD: controlled-impedance track widths from the board's own stackup (the impedance tool,
 * set_rules impedance / diff_impedance; the crosstalk pre-screen takes its H from here). A
 * track's reference planes are the nearest plane layers above and below it (Planes: by their
 * poured copper, not by their name or type), the signal layers between passed over (their copper
 * thickness in H): an outer layer is a microstrip over the nearest plane in, an inner one a
 * stripline between its planes, or with a plane on one side only an embedded microstrip (the
 * board on the other side as its cover). With no plane on the board yet, the nearest copper
 * layers. The arithmetic is kicadd_zcalc's.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include <layer_ids.h>

#include "common/kicadd_zcalc.h"

class BOARD;

namespace kicadd::impedance
{

struct LAYER_LINE
{
    PCB_LAYER_ID              layer = UNDEFINED_LAYER;
    std::vector<PCB_LAYER_ID> planes;   ///< the reference layers: one (microstrip) or two
    kicadd::zcalc::LINE       line;
    std::vector<PCB_LAYER_ID> crossed;          ///< signal layers between it and its planes
    bool                      embedded = false; ///< inner, a plane on one side: line.mask* is the cover
    bool                      guessed = false;  ///< no plane on the board: the nearest copper layers
};


/// A layer is a plane with its zones' copper (fills; outlines while not filled) over this much of
/// the board outline; with no zone yet, when Board Setup types it power or mixed or it has no
/// tracks (a plane not poured yet).
constexpr double PLANE_COVER = 0.4;

/// The copper layers taken as reference planes (see PLANE_COVER). aWhy: each layer, plane or
/// not, and why ("In1.Cu 93%"), for a note.
std::set<PCB_LAYER_ID> Planes( const BOARD& aBoard, std::string* aWhy = nullptr );


/// A track's cross-section on aLayer, from the board's stackup (its default one when none is
/// defined). nullopt: aError says why (not a copper layer of the board, no dielectric ...).
std::optional<LAYER_LINE> Line( const BOARD& aBoard, PCB_LAYER_ID aLayer, std::string& aError );

/// aLayer's cross-section over the planes given, aAbove towards F.Cu and aBelow towards B.Cu
/// (UNDEFINED_LAYER: none that way): what a track sees where the copper under it is not its
/// planes' (a pour on a signal layer between, a gap in a plane). nullopt with neither.
std::optional<LAYER_LINE> LineOver( const BOARD& aBoard, PCB_LAYER_ID aLayer, PCB_LAYER_ID aAbove, PCB_LAYER_ID aBelow,
                                    std::string& aError );

/// "microstrip over In1.Cu, 0.2104 mm Er 4.4, copper 0.035 mm, mask 0.01 mm Er 3.8"
std::string Describe( const LAYER_LINE& aLine );

/// The reference layers among aPlanes with no copper zone on them (so not planes yet).
std::vector<PCB_LAYER_ID> WithoutZone( const BOARD& aBoard, const std::vector<PCB_LAYER_ID>& aPlanes );

/// Where the dielectric constants come from: a fab's stackup (board_setup stackup=...) or
/// KiCad's default FR4 numbers, with the advice to check with the fab before ordering.
std::string Source( const BOARD& aBoard );

/// The dielectric between two copper layers from the stackup (its default one when none is
/// defined): its thickness (m, the copper of layers between included) and its thickness-weighted
/// Er. false: not both copper layers of the board.
bool Between( const BOARD& aBoard, PCB_LAYER_ID aA, PCB_LAYER_ID aB, double& aH, double& aEr );

/// mm, rounded to the micrometre.
double Mm( double aMetres );

} // namespace kicadd::impedance

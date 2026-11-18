/*
 * KiCadD: controlled-impedance track widths from the board's own stackup (the impedance tool,
 * set_rules impedance / diff_impedance). A track's reference planes are the nearest copper
 * layers above and below it: an outer layer is a microstrip over the next layer in, an inner
 * one a stripline between its two neighbours. The arithmetic is kicadd_zcalc's.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <optional>
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
};


/// A track's cross-section on aLayer, from the board's stackup (its default one when none is
/// defined). nullopt: aError says why (not a copper layer of the board, no dielectric ...).
std::optional<LAYER_LINE> Line( const BOARD& aBoard, PCB_LAYER_ID aLayer, std::string& aError );

/// "microstrip over In1.Cu, 0.2104 mm Er 4.4, copper 0.035 mm, mask 0.01 mm Er 3.8"
std::string Describe( const LAYER_LINE& aLine );

/// The reference layers among aPlanes with no copper zone on them (so not planes yet).
std::vector<PCB_LAYER_ID> WithoutZone( const BOARD& aBoard, const std::vector<PCB_LAYER_ID>& aPlanes );

/// Where the dielectric constants come from: a fab's stackup (board_setup stackup=...) or
/// KiCad's default FR4 numbers, with the advice to check with the fab before ordering.
std::string Source( const BOARD& aBoard );

/// mm, rounded to the micrometre.
double Mm( double aMetres );

} // namespace kicadd::impedance

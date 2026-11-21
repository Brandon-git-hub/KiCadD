/*
 * KiCadD: controlled-impedance track widths, the arithmetic only: one line's cross-section
 * (microstrip on an outer layer, stripline between two planes) handed to KiCad's own
 * transmission line calculations (common/transline_calculations, the ones pcb_calculator and
 * Board Setup's tuning profiles use), set up the way Board Setup's tuning profiles set them up.
 * Nothing here knows of a board: kicadd_impedance reads the cross-section from the stackup.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <string>

namespace kicadd::zcalc
{

/// The frequency the lines are calculated at (Hz): the microstrip's dispersion, the losses.
constexpr double FREQUENCY = 1e9;

/// A line's cross-section, metres.
struct LINE
{
    bool   microstrip = true;   ///< an outer layer over one plane; else a stripline between two
    double t = 35e-6;           ///< the copper
    double h = 0;               ///< microstrip: the dielectric to the plane; stripline: to the plane above
    double hBelow = 0;          ///< stripline: the dielectric to the plane below
    double er = 4.5;            ///< the dielectric between (thickness-weighted)
    double tand = 0.02;
    double maskT = 0;           ///< microstrip: solder mask over it (0: none)
    double maskEr = 3.8;
    double maskTand = 0.025;
};


struct RESULT
{
    bool        ok = false;
    std::string error;
    double      width = 0;   ///< m
    double      gap = 0;     ///< m (pairs)
    double      z = 0;       ///< ohm: Z0, or the pair's differential impedance
    double      delay = 0;   ///< ps/cm (a pair: odd mode)
};


/// The width for a single-ended Z0.
RESULT Width( const LINE& aLine, double aZ0 );

/// A pair's width for a differential impedance at a given gap.
RESULT PairWidth( const LINE& aLine, double aZdiff, double aGap );

/// A pair's gap for a differential impedance at a given width.
RESULT PairGap( const LINE& aLine, double aZdiff, double aWidth );

/// The width for a Z0 with ground copper beside the track aGround away on its layer (coplanar
/// waveguide with ground: an outer layer over its plane); gap in the result is aGround.
RESULT CpwgWidth( const LINE& aLine, double aZ0, double aGround );

/// Z0 of a track this wide with ground copper aGround beside it (CPWG).
RESULT Cpwg( const LINE& aLine, double aWidth, double aGround );

/// Z0 of a track this wide.
RESULT Single( const LINE& aLine, double aWidth );

/// The differential impedance of a pair this wide, this far apart.
RESULT Pair( const LINE& aLine, double aWidth, double aGap );

} // namespace kicadd::zcalc

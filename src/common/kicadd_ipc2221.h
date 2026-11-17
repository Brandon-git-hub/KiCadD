/*
 * KiCadD: IPC-2221 numbers for the design review (audit) and net class widths (set_rules).
 *
 * Current: IPC-2221 (section 6.2, the chart's fitted formula) I = k * dT^0.44 * A^0.725, A in mil^2,
 * k = 0.048 for outer layers and 0.024 for inner ones. Valid up to 35 A, tracks up to 400 mil,
 * temperature rise 10..100 C. IPC-2152 (2009) measured inner tracks to run cooler than this says:
 * the inner numbers here are conservative.
 *
 * Voltage: IPC-2221B table 6-1, B1 (inner conductors) and B2 (outer, uncoated, sea level to
 * 3050 m), mm between conductors for the voltage between them (DC or AC peak). Solder mask is not
 * counted as a coating (B4), which keeps the outer numbers on the safe side.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <cmath>

namespace kicadd::ipc2221
{

/// Track width (mm) to carry @a aAmps with a rise of @a aRiseC over ambient, in copper
/// @a aCopperUm thick (35 um = 1 oz), on an outer layer or an inner one.
inline double TrackWidthMm( double aAmps, double aRiseC, double aCopperUm, bool aOuter )
{
    if( aAmps <= 0 || aRiseC <= 0 || aCopperUm <= 0 )
        return 0;

    const double k = aOuter ? 0.048 : 0.024;
    const double areaMil2 = std::pow( aAmps / ( k * std::pow( aRiseC, 0.44 ) ), 1.0 / 0.725 );
    const double thickMil = aCopperUm / 25.4;
    return areaMil2 / thickMil * 0.0254;
}

/// The current (A) a track @a aWidthMm wide carries at that rise (the formula the other way round).
inline double CurrentA( double aWidthMm, double aRiseC, double aCopperUm, bool aOuter )
{
    if( aWidthMm <= 0 || aRiseC <= 0 || aCopperUm <= 0 )
        return 0;

    const double k = aOuter ? 0.048 : 0.024;
    const double areaMil2 = ( aWidthMm / 0.0254 ) * ( aCopperUm / 25.4 );
    return k * std::pow( aRiseC, 0.44 ) * std::pow( areaMil2, 0.725 );
}

/// Minimum spacing (mm) between conductors @a aVolts apart (table 6-1: B2 outer, B1 inner).
inline double ClearanceMm( double aVolts, bool aOuter )
{
    const double v = std::fabs( aVolts );

    struct ROW
    {
        double upTo, inner, outer;
    };

    static const ROW rows[] = { { 15, 0.05, 0.1 },   { 30, 0.05, 0.1 },   { 50, 0.1, 0.6 },
                                { 100, 0.1, 0.6 },   { 150, 0.2, 0.6 },   { 170, 0.2, 1.25 },
                                { 250, 0.2, 1.25 },  { 300, 0.2, 1.25 },  { 500, 0.25, 2.5 } };

    for( const ROW& r : rows )
    {
        if( v <= r.upTo )
            return aOuter ? r.outer : r.inner;
    }

    return v * ( aOuter ? 0.005 : 0.0025 );
}

} // namespace kicadd::ipc2221

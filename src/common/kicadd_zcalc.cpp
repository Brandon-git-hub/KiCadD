/*
 * KiCadD: controlled-impedance track widths, the arithmetic (see kicadd_zcalc.h). The
 * parameters are set as pcbnew/dialogs/panel_setup_tuning_profile_info.cpp sets them.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "common/kicadd_zcalc.h"

#include <cmath>

#include <transline_calculations/coplanar.h>
#include <transline_calculations/coupled_microstrip.h>
#include <transline_calculations/coupled_stripline.h>
#include <transline_calculations/microstrip.h>
#include <transline_calculations/stripline.h>
#include <transline_calculations/units.h>

namespace kicadd::zcalc
{

namespace
{

using TCP = TRANSLINE_PARAMETERS;
using RESULTS = std::unordered_map<TRANSLINE_PARAMETERS, std::pair<double, TRANSLINE_STATUS>>;

constexpr double RHO = 1.72e-8;   // copper, ohm m

double skinDepth( double aFreq )
{
    return 1.0 / std::sqrt( 3.14159265358979323846 * aFreq * TRANSLINE_CALCULATIONS::MU0 * ( 1.0 / RHO ) );
}


bool good( RESULTS& aR, TCP aKey )
{
    auto it = aR.find( aKey );
    return it != aR.end() && it->second.second == TRANSLINE_STATUS::OK && std::isfinite( it->second.first )
           && it->second.first > 0;
}


std::string checkLine( const LINE& aLine )
{
    if( aLine.t <= 0 )
        return "the copper has no thickness";

    if( aLine.er < 1 )
        return "the dielectric has no dielectric constant";

    if( aLine.microstrip ? aLine.h <= 0 : aLine.h <= 0 && aLine.hBelow <= 0 )
        return "no dielectric between the layer and its reference plane";

    return "";
}


void setMask( TRANSLINE_CALCULATION_BASE& aCalc, const LINE& aLine )
{
    if( aLine.maskT <= 0 )
        return;

    aCalc.SetParameter( TCP::SOLDERMASK_PRESENT, 1.0 );
    aCalc.SetParameter( TCP::SOLDERMASK_THICKNESS, aLine.maskT );
    aCalc.SetParameter( TCP::SOLDERMASK_EPSILONR, aLine.maskEr );
    aCalc.SetParameter( TCP::SOLDERMASK_TAND, aLine.maskTand );
}


// one track: aZ0 > 0 asks for the width, else aWidth is analysed
RESULT single( const LINE& aLine, double aZ0, double aWidth )
{
    RESULT out;
    out.error = checkLine( aLine );

    if( !out.error.empty() )
        return out;

    const bool synth = aZ0 > 0;
    RESULTS*   r;

    MICROSTRIP ms;
    STRIPLINE  sl;

    if( aLine.microstrip )
    {
        ms.SetParameter( TCP::PHYS_WIDTH, synth ? 0.001 : aWidth );
        ms.SetParameter( TCP::SIGMA, 1.0 / RHO );
        ms.SetParameter( TCP::EPSILON_EFF, 1.0 );
        ms.SetParameter( TCP::SKIN_DEPTH, skinDepth( FREQUENCY ) );
        ms.SetParameter( TCP::EPSILONR, aLine.er );
        ms.SetParameter( TCP::H_T, 1e+20 );
        ms.SetParameter( TCP::H, aLine.h );
        ms.SetParameter( TCP::T, aLine.t );
        ms.SetParameter( TCP::Z0, synth ? aZ0 : 50 );
        ms.SetParameter( TCP::FREQUENCY, FREQUENCY );
        ms.SetParameter( TCP::ROUGH, 0 );
        ms.SetParameter( TCP::TAND, aLine.tand );
        ms.SetParameter( TCP::PHYS_LEN, 10 );
        ms.SetParameter( TCP::MUR, 1 );
        ms.SetParameter( TCP::MURC, 1 );
        ms.SetParameter( TCP::ANG_L, 1 );
        setMask( ms, aLine );

        if( synth )
            ms.Synthesize( SYNTHESIZE_OPTS::DEFAULT );
        else
            ms.Analyse();

        r = synth ? &ms.GetSynthesisResults() : &ms.GetAnalysisResults();
    }
    else
    {
        sl.SetParameter( TCP::PHYS_WIDTH, synth ? 0.001 : aWidth );
        sl.SetParameter( TCP::SKIN_DEPTH, skinDepth( FREQUENCY ) );
        sl.SetParameter( TCP::EPSILONR, aLine.er );
        sl.SetParameter( TCP::T, aLine.t );
        sl.SetParameter( TCP::STRIPLINE_A, aLine.h );
        sl.SetParameter( TCP::H, aLine.h + aLine.t + aLine.hBelow );
        sl.SetParameter( TCP::Z0, synth ? aZ0 : 50 );
        sl.SetParameter( TCP::PHYS_LEN, 1.0 );
        sl.SetParameter( TCP::FREQUENCY, FREQUENCY );
        sl.SetParameter( TCP::TAND, aLine.tand );
        sl.SetParameter( TCP::ANG_L, 1.0 );
        sl.SetParameter( TCP::SIGMA, 1.0 / RHO );
        sl.SetParameter( TCP::MURC, 1 );

        if( synth )
            sl.Synthesize( SYNTHESIZE_OPTS::DEFAULT );
        else
            sl.Analyse();

        r = synth ? &sl.GetSynthesisResults() : &sl.GetAnalysisResults();
    }

    if( !good( *r, TCP::PHYS_WIDTH ) || !good( *r, TCP::Z0 ) )
    {
        out.error = synth ? "no width gives that impedance on this layer" : "the impedance cannot be worked out for it";
        return out;
    }

    out.ok = true;
    out.width = ( *r )[TCP::PHYS_WIDTH].first;
    out.z = ( *r )[TCP::Z0].first;
    out.delay = good( *r, TCP::UNIT_PROP_DELAY ) ? ( *r )[TCP::UNIT_PROP_DELAY].first : 0;
    return out;
}


// a track with ground copper beside it on its own layer, aGround apart (CPWG: the plane below
// too, as on an outer layer over its plane): aZ0 > 0 asks for the width, else aWidth is analysed
RESULT coplanar( const LINE& aLine, double aZ0, double aWidth, double aGround )
{
    RESULT out;
    out.error = checkLine( aLine );

    if( !out.error.empty() )
        return out;

    if( !aLine.microstrip )
    {
        out.error = "coplanar with ground is worked out for an outer layer over its plane only";
        return out;
    }

    if( aGround <= 0 )
    {
        out.error = "the gap to the ground beside the track must be above 0";
        return out;
    }

    const bool synth = aZ0 > 0;
    COPLANAR   cp;
    cp.SetParameter( TCP::PHYS_WIDTH, synth ? 0.0005 : aWidth );
    cp.SetParameter( TCP::PHYS_S, aGround );
    cp.SetParameter( TCP::H, aLine.h );
    cp.SetParameter( TCP::T, aLine.t );
    cp.SetParameter( TCP::EPSILONR, aLine.er );
    cp.SetParameter( TCP::TAND, aLine.tand );
    cp.SetParameter( TCP::SIGMA, 1.0 / RHO );
    cp.SetParameter( TCP::MURC, 1 );
    cp.SetParameter( TCP::SKIN_DEPTH, skinDepth( FREQUENCY ) );
    cp.SetParameter( TCP::EPSILON_EFF, 1.0 );
    cp.SetParameter( TCP::Z0, synth ? aZ0 : 50 );
    cp.SetParameter( TCP::FREQUENCY, FREQUENCY );
    cp.SetParameter( TCP::PHYS_LEN, 10 );
    cp.SetParameter( TCP::ANG_L, 1 );
    cp.SetParameter( TCP::CPW_BACKMETAL, 1.0 );   // the plane below: grounded coplanar
    setMask( cp, aLine );

    if( synth )
    {
        cp.SetSynthesizeTarget( TCP::PHYS_WIDTH );
        cp.Synthesize( SYNTHESIZE_OPTS::DEFAULT );
    }
    else
    {
        cp.Analyse();
    }

    RESULTS* r = synth ? &cp.GetSynthesisResults() : &cp.GetAnalysisResults();

    if( !good( *r, TCP::PHYS_WIDTH ) || !good( *r, TCP::Z0 ) )
    {
        out.error = synth ? "no width gives that impedance with that gap to ground" : "the impedance cannot be worked out for it";
        return out;
    }

    out.ok = true;
    out.width = ( *r )[TCP::PHYS_WIDTH].first;
    out.gap = aGround;
    out.z = ( *r )[TCP::Z0].first;
    out.delay = good( *r, TCP::UNIT_PROP_DELAY ) ? ( *r )[TCP::UNIT_PROP_DELAY].first : 0;
    return out;
}


enum class SOLVE { WIDTH, GAP, NONE };

RESULT pair( const LINE& aLine, double aZdiff, double aWidth, double aGap, SOLVE aSolve )
{
    RESULT out;
    out.error = checkLine( aLine );

    if( !out.error.empty() )
        return out;

    const double z = aSolve == SOLVE::NONE ? 100 : aZdiff;
    const double width = aSolve == SOLVE::WIDTH ? 0 : aWidth;
    const double gap = aSolve == SOLVE::GAP ? 0 : aGap;
    RESULTS*     r;

    COUPLED_MICROSTRIP ms;
    COUPLED_STRIPLINE  sl;
    TRANSLINE_CALCULATION_BASE& calc = aLine.microstrip ? static_cast<TRANSLINE_CALCULATION_BASE&>( ms ) : sl;

    calc.SetParameter( TCP::Z0_E, z / 2.0 );
    calc.SetParameter( TCP::Z0_O, z / 2.0 );
    calc.SetParameter( TCP::Z_DIFF, z );
    calc.SetParameter( TCP::PHYS_WIDTH, width );
    calc.SetParameter( TCP::PHYS_S, gap );
    calc.SetParameter( TCP::T, aLine.t );
    calc.SetParameter( TCP::EPSILONR, aLine.er );
    calc.SetParameter( TCP::FREQUENCY, FREQUENCY );
    calc.SetParameter( TCP::MURC, 1 );
    calc.SetParameter( TCP::SKIN_DEPTH, skinDepth( FREQUENCY ) );
    calc.SetParameter( TCP::SIGMA, 1.0 / RHO );
    calc.SetParameter( TCP::ANG_L, 1 );

    if( aLine.microstrip )
    {
        ms.SetParameter( TCP::PHYS_LEN, 10 );
        ms.SetParameter( TCP::H, aLine.h );
        ms.SetParameter( TCP::H_T, 1e+20 );
        ms.SetParameter( TCP::ROUGH, 0 );
        ms.SetParameter( TCP::TAND, aLine.tand );
        setMask( ms, aLine );
    }
    else
    {
        // COUPLED_STRIPLINE reads STRIPLINE_A as the strip's midplane above the bottom plane
        // (0: centred, its symmetric path), as Board Setup's tuning profiles do
        const double totalH = aLine.h + aLine.t + aLine.hBelow;
        const double a = aLine.hBelow + 0.5 * aLine.t;

        sl.SetParameter( TCP::H, totalH );
        sl.SetParameter( TCP::STRIPLINE_A, COUPLED_STRIPLINE::IsCenteredOffset( a, totalH ) ? 0.0 : a );
        sl.SetParameter( TCP::PHYS_LEN, 1.0 );
    }

    switch( aSolve )
    {
    case SOLVE::WIDTH: calc.Synthesize( SYNTHESIZE_OPTS::FIX_SPACING ); break;
    case SOLVE::GAP:   calc.Synthesize( SYNTHESIZE_OPTS::FIX_WIDTH ); break;
    case SOLVE::NONE:  calc.Analyse(); break;
    }

    r = aSolve == SOLVE::NONE ? &calc.GetAnalysisResults() : &calc.GetSynthesisResults();

    if( !good( *r, TCP::PHYS_WIDTH ) || !good( *r, TCP::PHYS_S ) || !good( *r, TCP::Z_DIFF ) )
    {
        out.error = aSolve == SOLVE::WIDTH ? "no width gives that impedance at that gap on this layer"
                    : aSolve == SOLVE::GAP ? "no gap gives that impedance at that width on this layer"
                                           : "the impedance cannot be worked out";
        return out;
    }

    out.ok = true;
    out.width = ( *r )[TCP::PHYS_WIDTH].first;
    out.gap = ( *r )[TCP::PHYS_S].first;
    out.z = ( *r )[TCP::Z_DIFF].first;
    out.delay = good( *r, TCP::UNIT_PROP_DELAY_ODD ) ? ( *r )[TCP::UNIT_PROP_DELAY_ODD].first : 0;
    return out;
}

} // namespace


namespace
{

// a synthesis answer analysed again: the solvers stop at the edge of their search range and
// report the target as met; an answer that does not give the target (0.5 %) is no answer
RESULT confirmed( const LINE& aLine, RESULT aR, double aTarget, bool aPair, const char* aWhat )
{
    if( !aR.ok )
        return aR;

    constexpr double MIN_LEN = 1e-6;   // 1 um: below it nothing is made

    if( aR.width < MIN_LEN || ( aPair && aR.gap < MIN_LEN ) )
        return { false, std::string( "no " ) + aWhat + " gives that impedance on this layer (it would be under 1 um)" };

    RESULT back = aPair ? pair( aLine, 0, aR.width, aR.gap, SOLVE::NONE ) : single( aLine, 0, aR.width );

    if( !back.ok || std::fabs( back.z - aTarget ) > 0.005 * aTarget )
        return { false, std::string( "no " ) + aWhat + " gives that impedance on this layer (out of the calculation's range)" };

    aR.z = back.z;
    aR.delay = back.delay > 0 ? back.delay : aR.delay;
    return aR;
}

} // namespace


RESULT Width( const LINE& aLine, double aZ0 )
{
    if( !( aZ0 > 0 ) )
        return { false, "the impedance must be above 0" };

    return confirmed( aLine, single( aLine, aZ0, 0 ), aZ0, false, "width" );
}


RESULT PairWidth( const LINE& aLine, double aZdiff, double aGap )
{
    if( !( aZdiff > 0 ) || !( aGap > 0 ) )
        return { false, "the impedance and the gap must be above 0" };

    return confirmed( aLine, pair( aLine, aZdiff, 0, aGap, SOLVE::WIDTH ), aZdiff, true, "width at that gap" );
}


RESULT PairGap( const LINE& aLine, double aZdiff, double aWidth )
{
    if( !( aZdiff > 0 ) || !( aWidth > 0 ) )
        return { false, "the impedance and the width must be above 0" };

    return confirmed( aLine, pair( aLine, aZdiff, aWidth, 0, SOLVE::GAP ), aZdiff, true, "gap at that width" );
}


RESULT Single( const LINE& aLine, double aWidth )
{
    if( !( aWidth > 0 ) )
        return { false, "the width must be above 0" };

    return single( aLine, 0, aWidth );
}


RESULT Pair( const LINE& aLine, double aWidth, double aGap )
{
    if( !( aWidth > 0 ) || !( aGap > 0 ) )
        return { false, "the width and the gap must be above 0" };

    return pair( aLine, 0, aWidth, aGap, SOLVE::NONE );
}

RESULT CpwgWidth( const LINE& aLine, double aZ0, double aGround )
{
    return coplanar( aLine, aZ0, 0, aGround );
}


RESULT Cpwg( const LINE& aLine, double aWidth, double aGround )
{
    return coplanar( aLine, 0, aWidth, aGround );
}

} // namespace kicadd::zcalc

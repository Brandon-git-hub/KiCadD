/*
 * KiCadD: the board's design rules as one value (see kicadd_rules.h).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_rules.h"

#include <board.h>
#include <board_design_settings.h>
#include <netclass.h>
#include <project/net_settings.h>

namespace kicadd::rules
{

namespace
{

CLASS classOf( const NETCLASS& aClass )
{
    CLASS c;
    c.clearance = aClass.GetClearanceOpt();
    c.trackWidth = aClass.GetTrackWidthOpt();
    c.viaDiameter = aClass.GetViaDiameterOpt();
    c.viaDrill = aClass.GetViaDrillOpt();
    c.diffPairWidth = aClass.GetDiffPairWidthOpt();
    c.diffPairGap = aClass.GetDiffPairGapOpt();
    return c;
}


void setClass( NETCLASS& aClass, const CLASS& aValues )
{
    aClass.SetClearance( aValues.clearance );
    aClass.SetTrackWidth( aValues.trackWidth );
    aClass.SetViaDiameter( aValues.viaDiameter );
    aClass.SetViaDrill( aValues.viaDrill );
    aClass.SetDiffPairWidth( aValues.diffPairWidth );
    aClass.SetDiffPairGap( aValues.diffPairGap );
}

} // namespace


STATE Capture( BOARD& aBoard )
{
    BOARD_DESIGN_SETTINGS& bds = aBoard.GetDesignSettings();
    NET_SETTINGS&          ns = *bds.m_NetSettings;
    STATE                  st;

    st.classes[NETCLASS::Default] = classOf( *ns.GetDefaultNetclass() );

    for( const auto& [name, nc] : ns.GetNetclasses() )
        st.classes[name] = classOf( *nc );

    for( const auto& [matcher, nc] : ns.GetNetclassPatternAssignments() )
        st.patterns.emplace_back( matcher->GetPattern(), nc );

    st.minClearance = bds.m_MinClearance;
    st.minTrackWidth = bds.m_TrackMinWidth;
    st.minViaDiameter = bds.m_ViasMinSize;
    st.minThroughDrill = bds.m_MinThroughDrill;
    st.minAnnularWidth = bds.m_ViasMinAnnularWidth;
    st.holeClearance = bds.m_HoleClearance;
    st.holeToHole = bds.m_HoleToHoleMin;
    st.copperEdgeClearance = bds.m_CopperEdgeClearance;
    st.minSilkTextHeight = bds.m_MinSilkTextHeight;
    st.minSilkTextThickness = bds.m_MinSilkTextThickness;
    return st;
}


void Apply( BOARD& aBoard, const STATE& aState )
{
    BOARD_DESIGN_SETTINGS& bds = aBoard.GetDesignSettings();
    NET_SETTINGS&          ns = *bds.m_NetSettings;

    std::map<wxString, std::shared_ptr<NETCLASS>> classes;

    for( const auto& [name, values] : aState.classes )
    {
        if( name == NETCLASS::Default )
        {
            setClass( *ns.GetDefaultNetclass(), values );
            continue;
        }

        // the same object where there is one: tracks and nets hold on to it
        auto it = ns.GetNetclasses().find( name );
        std::shared_ptr<NETCLASS> nc = it != ns.GetNetclasses().end() ? it->second
                                                                      : std::make_shared<NETCLASS>( name, false );
        setClass( *nc, values );
        classes[name] = nc;
    }

    ns.SetNetclasses( classes );
    ns.ClearNetclassPatternAssignments();

    for( const auto& [pattern, nc] : aState.patterns )
        ns.SetNetclassPatternAssignment( pattern, nc );

    bds.m_MinClearance = aState.minClearance;
    bds.m_TrackMinWidth = aState.minTrackWidth;
    bds.m_ViasMinSize = aState.minViaDiameter;
    bds.m_MinThroughDrill = aState.minThroughDrill;
    bds.m_ViasMinAnnularWidth = aState.minAnnularWidth;
    bds.m_HoleClearance = aState.holeClearance;
    bds.m_HoleToHoleMin = aState.holeToHole;
    bds.m_CopperEdgeClearance = aState.copperEdgeClearance;
    bds.m_MinSilkTextHeight = aState.minSilkTextHeight;
    bds.m_MinSilkTextThickness = aState.minSilkTextThickness;

    ns.ClearAllCaches();
    aBoard.SynchronizeNetsAndNetClasses( false );
}

} // namespace kicadd::rules

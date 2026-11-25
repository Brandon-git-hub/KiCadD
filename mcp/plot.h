/*
 * KiCadD MCP - charts as PNG for the agent (simulate, pdn, reflection): lines over linear or
 * log axes, one or more panels sharing the x axis, a legend, an optional limit line. Drawn here
 * (an antialiased raster, KiCad's stroke font for the lettering, zlib for the PNG): no SVG
 * library, no fonts on the system, no new dependency.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_MCP_PLOT_H
#define KICADD_MCP_PLOT_H

#include <optional>
#include <string>
#include <functional>
#include <vector>

namespace kicadd::mcp::plot
{

struct SERIES
{
    std::string         label;
    std::vector<double> x, y;
    int                 color = -1;      ///< palette index; -1: the series' place
    bool                faint = false;   ///< thinner, lighter (corners)
    bool                dashed = false;
};


struct PANEL
{
    std::string           yUnit;            ///< "V", "dB", "°", "Ω", "A"
    bool                  logY = false;
    std::vector<SERIES>   series;
    std::optional<double> limit;            ///< a horizontal line (a target)
    std::string           limitLabel;
};


struct CHART
{
    std::string        title;
    std::string        xUnit;               ///< "s", "Hz", "V"
    bool               logX = false;
    std::vector<PANEL> panels;
    int                width = 960;
    int                panelHeight = 300;
};


/// The chart as PNG bytes ("" when there is nothing to draw).
std::string Png( const CHART& aChart );

/// A value with its SI prefix for an axis ("2.5 µs", "100 kHz", "-20 dB").
/// aStep: the axis's tick step, for enough digits that neighbouring ticks differ (0: 3 digits)
std::string Tick( double aValue, const std::string& aUnit, double aStep = 0 );

/// The web view's Chart tab (phase 4 item 2): with a view running (kicadd-mcp --http), every chart
/// a tool makes (simulate, pdn, reflection) is also handed to it, plot=true or not; the PNG is
/// still made only when asked. aTool names the tool that made it.
void SetSink( std::function<void( const CHART&, const std::string& aTool )> aSink );
bool HasSink();
void Publish( const CHART& aChart, const std::string& aTool );

} // namespace kicadd::mcp::plot

#endif

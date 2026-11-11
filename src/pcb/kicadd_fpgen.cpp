/*
 * KiCadD: parametric footprints. See kicadd_fpgen.h.
 *
 * Pad sizes follow the IPC-7351 idea (a nominal lead plus toe, heel and side margins) with
 * margins of our own choosing, not KiCad's library (whose footprints are CC-BY-SA). Origin at
 * the package centre (headers: pin 1, as KiCad does). Pin 1 top left, counter-clockwise.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_fpgen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

namespace kicadd
{

namespace
{

std::string f( double v )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.4f", std::abs( v ) < 5e-5 ? 0.0 : v );
    std::string s = buf;

    while( s.back() == '0' )
        s.pop_back();

    if( s.back() == '.' )
        s.pop_back();

    return s;
}

double r2( double v ) { return std::round( v * 100 ) / 100; }     // 0.01 mm

struct PAD
{
    std::string number;
    double      x, y, w, h;     // centre, size (w along X)
    bool        tht = false;
    bool        square = false;
    double      drill = 0;
};

struct BUILD
{
    std::vector<PAD> pads;
    double           bodyW = 0, bodyH = 0;     // fab outline, centred on the body
    double           bodyX = 0, bodyY = 0;
    bool             tht = false;
    std::string      descr;
};

/// Two rows of gull-wing (or flat) leads, pins 1..n/2 down the left, then up the right
void dualRow( BUILD& b, int aPins, double aPitch, double aSpan, double aFoot, double aToe, double aHeel,
              double aWidth )
{
    const int    per = aPins / 2;
    const double outer = aSpan / 2 + aToe, inner = aSpan / 2 - aFoot - aHeel;
    const double len = r2( outer - inner ), cx = r2( ( outer + inner ) / 2 );

    for( int i = 0; i < per; ++i )
    {
        double y = ( i - ( per - 1 ) / 2.0 ) * aPitch;
        b.pads.push_back( { std::to_string( i + 1 ), -cx, y, len, aWidth } );
        b.pads.push_back( { std::to_string( aPins - i ), cx, y, len, aWidth } );
    }
}

/// Four rows: left top->bottom, bottom left->right, right bottom->top, top right->left
void quadRow( BUILD& b, int aPins, double aPitch, double aCentre, double aLen, double aWidth )
{
    const int per = aPins / 4;
    int       n = 1;

    for( int side = 0; side < 4; ++side )
    {
        for( int i = 0; i < per; ++i, ++n )
        {
            double t = ( i - ( per - 1 ) / 2.0 ) * aPitch;

            switch( side )
            {
            case 0: b.pads.push_back( { std::to_string( n ), -aCentre, t, aLen, aWidth } ); break;
            case 1: b.pads.push_back( { std::to_string( n ), t, aCentre, aWidth, aLen } ); break;
            case 2: b.pads.push_back( { std::to_string( n ), aCentre, -t, aLen, aWidth } ); break;
            default: b.pads.push_back( { std::to_string( n ), -t, -aCentre, aWidth, aLen } ); break;
            }
        }
    }
}

double padWidth( double aPitch ) { return r2( std::min( { 0.6, aPitch * 0.62, aPitch - 0.2 } ) ); }

} // namespace


bool MakeFootprint( const kicadd::board::FpCreate& aReq, std::string& aName, std::string& aText, int& aPads,
                    std::string& aError )
{
    std::string kind = aReq.kind();
    std::transform( kind.begin(), kind.end(), kind.begin(), ::tolower );
    BUILD b;
    char  nm[128];

    if( kind == "chip" )
    {
        // metric-size bodies: length, width, terminal length (inch codes)
        static const std::map<std::string, std::tuple<double, double, double>> sizes = {
            { "0201", { 0.6, 0.3, 0.15 } },  { "0402", { 1.0, 0.5, 0.25 } }, { "0603", { 1.6, 0.8, 0.35 } },
            { "0805", { 2.0, 1.25, 0.4 } },  { "1206", { 3.2, 1.6, 0.5 } },  { "1210", { 3.2, 2.5, 0.5 } },
            { "2010", { 5.0, 2.5, 0.6 } },   { "2512", { 6.3, 3.2, 0.6 } } };
        auto it = sizes.find( aReq.size() );

        if( it == sizes.end() )
        {
            aError = "chip size is one of 0201 0402 0603 0805 1206 1210 2010 2512";
            return false;
        }

        auto [L, W, T] = it->second;
        // toe grows with the part (the solder fillet beyond the terminal), almost no heel
        static const std::map<std::string, double> toes = { { "0201", 0.12 }, { "0402", 0.28 }, { "0603", 0.42 },
                                                            { "0805", 0.45 }, { "1206", 0.5 },  { "1210", 0.5 },
                                                            { "2010", 0.55 }, { "2512", 0.6 } };
        const double toe = toes.at( aReq.size() ), heel = 0.02, side = L < 1.5 ? 0.07 : 0.08;
        const double outer = L / 2 + toe, inner = L / 2 - T - heel;
        const double len = r2( outer - inner ), cx = r2( ( outer + inner ) / 2 ), w = r2( W + 2 * side );
        b.pads.push_back( { "1", -cx, 0, len, w } );
        b.pads.push_back( { "2", cx, 0, len, w } );
        b.bodyW = L;
        b.bodyH = W;
        std::string prefix = aReq.prefix().empty() ? "R" : aReq.prefix();
        std::snprintf( nm, sizeof( nm ), "%s_%s", prefix.c_str(), aReq.size().c_str() );
        b.descr = prefix + " " + aReq.size() + " (" + f( L ) + " x " + f( W ) + " mm), made by KiCadD";
    }
    else if( kind == "sot23" )
    {
        int n = aReq.pins() ? aReq.pins() : 3;

        if( n != 3 && n != 5 && n != 6 )
        {
            aError = "sot23 pins: 3, 5 or 6";
            return false;
        }

        const double pitch = 0.95, span = 2.4, foot = 0.4, toe = 0.6, heel = 0.33;
        const double outer = span / 2 + toe, inner = span / 2 - foot - heel;
        const double len = r2( outer - inner ), cx = r2( ( outer + inner ) / 2 ), w = 0.6;

        if( n == 3 )
        {
            b.pads.push_back( { "1", -cx, -pitch, len, w } );
            b.pads.push_back( { "2", -cx, pitch, len, w } );
            b.pads.push_back( { "3", cx, 0, len, w } );
        }
        else
        {
            for( int i = 0; i < 3; ++i )
                b.pads.push_back( { std::to_string( i + 1 ), -cx, ( i - 1 ) * pitch, len, w } );

            if( n == 5 )
            {
                b.pads.push_back( { "4", cx, pitch, len, w } );
                b.pads.push_back( { "5", cx, -pitch, len, w } );
            }
            else
            {
                for( int i = 0; i < 3; ++i )
                    b.pads.push_back( { std::to_string( i + 4 ), cx, ( 1 - i ) * pitch, len, w } );
            }
        }

        b.bodyW = 1.3;
        b.bodyH = 2.9;
        std::snprintf( nm, sizeof( nm ), "SOT-23-%d", n );
        b.descr = std::string( nm ) + ", made by KiCadD";
    }
    else if( kind == "soic" || kind == "so" )
    {
        int n = aReq.pins();

        if( n < 4 || n % 2 )
        {
            aError = kind + " pins: an even number, 4 or more";
            return false;
        }

        const double pitch = aReq.pitch() > 0 ? aReq.pitch() : 1.27;
        const double bodyW = aReq.body() > 0 ? aReq.body() : ( pitch >= 1.27 ? 3.9 : 4.4 );
        const double span = aReq.span() > 0 ? aReq.span() : ( pitch >= 1.27 ? bodyW + 2.1 : bodyW + 2.0 );
        const double foot = pitch >= 1.0 ? 0.84 : 0.6;
        dualRow( b, n, pitch, span, foot, 0.45, 0.65, padWidth( pitch ) );
        b.bodyW = bodyW;
        b.bodyH = r2( ( n / 2 - 1 ) * pitch + ( pitch >= 1.27 ? 1.09 : 0.9 ) );
        std::snprintf( nm, sizeof( nm ), "%s-%d_%smm_P%smm", pitch >= 1.27 ? "SOIC" : "SO", n, f( bodyW ).c_str(),
                       f( pitch ).c_str() );
        b.descr = std::string( nm ) + " (lead span " + f( span ) + " mm), made by KiCadD";
    }
    else if( kind == "qfn" || kind == "qfp" )
    {
        int n = aReq.pins();

        if( n < 8 || n % 4 )
        {
            aError = kind + " pins: a multiple of 4, 8 or more";
            return false;
        }

        const double pitch = aReq.pitch() > 0 ? aReq.pitch() : 0.5;
        const double body = aReq.body() > 0 ? aReq.body() : r2( ( n / 4 + 1 ) * pitch + ( kind == "qfn" ? 0.5 : 1.0 ) );
        const double w = padWidth( pitch );

        if( kind == "qfn" )
        {
            // flat leads under the body edge: 0.4 long, the pad 0.3 out (toe) and 0.05 in
            const double outer = body / 2 + 0.3, inner = body / 2 - 0.4 - 0.05;
            quadRow( b, n, pitch, r2( ( outer + inner ) / 2 ), r2( outer - inner ), w );

            if( aReq.ep() > 0 )
                b.pads.push_back( { std::to_string( n + 1 ), 0, 0, aReq.ep(), aReq.ep() } );

            std::snprintf( nm, sizeof( nm ), "QFN-%d_%sx%smm_P%smm%s", n, f( body ).c_str(), f( body ).c_str(),
                           f( pitch ).c_str(), aReq.ep() > 0 ? ( "_EP" + f( aReq.ep() ) + "mm" ).c_str() : "" );
        }
        else
        {
            const double span = aReq.span() > 0 ? aReq.span() : body + 2.0;
            const double outer = span / 2 + 0.4, inner = span / 2 - 0.6 - 0.35;
            quadRow( b, n, pitch, r2( ( outer + inner ) / 2 ), r2( outer - inner ), w );
            std::snprintf( nm, sizeof( nm ), "QFP-%d_%sx%smm_P%smm", n, f( body ).c_str(), f( body ).c_str(),
                           f( pitch ).c_str() );
        }

        b.bodyW = b.bodyH = body;
        b.descr = std::string( nm ) + ", made by KiCadD";
    }
    else if( kind == "header" )
    {
        const int    rows = aReq.rows() == 2 ? 2 : 1;
        const int    n = aReq.pins();
        const double pitch = aReq.pitch() > 0 ? aReq.pitch() : 2.54;

        if( n < 1 || n % rows )
        {
            aError = "header pins: a multiple of rows";
            return false;
        }

        const double drill = pitch >= 2.5 ? 1.0 : pitch >= 2.0 ? 0.8 : 0.65;
        const double pad = pitch >= 2.5 ? 1.7 : pitch >= 2.0 ? 1.35 : 1.0;
        const int    cols = n / rows;

        for( int i = 0; i < n; ++i )
        {
            int  col = rows == 2 ? i / 2 : i, row = rows == 2 ? i % 2 : 0;
            PAD  p{ std::to_string( i + 1 ), row * pitch, col * pitch, pad, pad, true, i == 0, drill };
            b.pads.push_back( p );
        }

        b.tht = true;
        b.bodyW = rows * pitch;
        b.bodyH = cols * pitch;
        b.bodyX = ( rows - 1 ) * pitch / 2;
        b.bodyY = ( cols - 1 ) * pitch / 2;
        std::snprintf( nm, sizeof( nm ), "Header_%dx%02d_P%smm", rows, cols, f( pitch ).c_str() );
        b.descr = std::string( nm ) + " through-hole, made by KiCadD";
    }
    else
    {
        aError = "kind is chip, sot23, soic, so, qfn, qfp or header";
        return false;
    }

    aName = aReq.name().empty() ? std::string( nm ) : aReq.name();

    // --- extents: pads and body; courtyard around them
    double x0 = b.bodyX - b.bodyW / 2, x1 = b.bodyX + b.bodyW / 2, y0 = b.bodyY - b.bodyH / 2, y1 = b.bodyY + b.bodyH / 2;
    double px0 = 1e9, px1 = -1e9, py0 = 1e9, py1 = -1e9;

    for( const PAD& p : b.pads )
    {
        px0 = std::min( px0, p.x - p.w / 2 );
        px1 = std::max( px1, p.x + p.w / 2 );
        py0 = std::min( py0, p.y - p.h / 2 );
        py1 = std::max( py1, p.y + p.h / 2 );
    }

    const double margin = b.tht ? 0.5 : 0.25;
    double       cx0 = r2( std::min( x0, px0 ) - margin ), cx1 = r2( std::max( x1, px1 ) + margin );
    double       cy0 = r2( std::min( y0, py0 ) - margin ), cy1 = r2( std::max( y1, py1 ) + margin );

    const std::string stroke = [&]( double w ) { return "(stroke (width " + f( w ) + ") (type solid))"; }( 0.12 );
    const std::string fab = "(stroke (width 0.1) (type solid))";
    const std::string crt = "(stroke (width 0.05) (type solid))";

    std::string t = "(footprint \"" + aName + "\"\n  (version 20240108)\n  (generator \"kicadd\")\n  (layer \"F.Cu\")\n";
    t += "  (descr \"" + b.descr + "\")\n";
    t += "  (property \"Reference\" \"REF**\" (at " + f( b.bodyX ) + " " + f( cy0 - 0.8 ) + " 0) (layer \"F.SilkS\") "
         "(effects (font (size 1 1) (thickness 0.15))))\n";
    t += "  (property \"Value\" \"" + aName + "\" (at " + f( b.bodyX ) + " " + f( cy1 + 0.8 ) + " 0) (layer \"F.Fab\") "
         "(effects (font (size 1 1) (thickness 0.15))))\n";
    t += std::string( "  (attr " ) + ( b.tht ? "through_hole" : "smd" ) + ")\n";

    // fab: the body, its pin-1 corner cut
    {
        double c = std::min( { 1.0, b.bodyW / 4, b.bodyH / 4 } );
        t += "  (fp_poly (pts (xy " + f( x0 + c ) + " " + f( y0 ) + ") (xy " + f( x1 ) + " " + f( y0 ) + ") (xy " + f( x1 )
             + " " + f( y1 ) + ") (xy " + f( x0 ) + " " + f( y1 ) + ") (xy " + f( x0 ) + " " + f( y0 + c ) + ")) " + fab
             + " (fill no) (layer \"F.Fab\"))\n";
        t += "  (fp_text user \"${REFERENCE}\" (at " + f( b.bodyX ) + " " + f( b.bodyY ) + " 0) (layer \"F.Fab\") "
             "(effects (font (size 0.5 0.5) (thickness 0.08))))\n";
    }

    // silk: the body's edges that are clear of the pads (0.2 mm), and a pin-1 dot
    auto silkLine = [&]( double ax, double ay, double bx, double by )
    {
        t += "  (fp_line (start " + f( ax ) + " " + f( ay ) + ") (end " + f( bx ) + " " + f( by ) + ") " + stroke
             + " (layer \"F.SilkS\"))\n";
    };

    const double gap = 0.2 + 0.06;
    const double sy0 = std::min( y0, py0 - gap ) - 0.0, sy1 = std::max( y1, py1 + gap );

    if( kind == "chip" )
    {
        // between the pads, above and below the body
        double inner = b.pads[1].x - b.pads[1].w / 2 - gap;

        if( inner > 0.1 )
        {
            silkLine( -inner, y0 - 0.1, inner, y0 - 0.1 );
            silkLine( -inner, y1 + 0.1, inner, y1 + 0.1 );
        }
    }
    else if( kind == "soic" || kind == "so" || kind == "sot23" )
    {
        silkLine( x0, sy0, x1, sy0 );
        silkLine( x0, sy1, x1, sy1 );
    }
    else if( kind == "qfn" || kind == "qfp" )
    {
        // corner marks, up to the first pads
        double edge = b.bodyW / 2 + ( kind == "qfn" ? 0.11 : 0.0 );
        double reach = 0;

        for( const PAD& p : b.pads )
        {
            if( std::abs( p.x ) < b.bodyW / 2 && p.y < 0 )
                reach = std::max( reach, std::abs( p.x ) + p.w / 2 + gap );
        }

        reach = std::min( reach, edge );

        for( int sx : { -1, 1 } )
        {
            for( int sy : { -1, 1 } )
            {
                silkLine( sx * edge, sy * edge, sx * reach, sy * edge );

                if( !( sx < 0 && sy < 0 ) )
                    silkLine( sx * edge, sy * edge, sx * edge, sy * reach );
            }
        }
    }
    else
    {
        t += "  (fp_rect (start " + f( x0 - 0.1 ) + " " + f( y0 - 0.1 ) + ") (end " + f( x1 + 0.1 ) + " " + f( y1 + 0.1 )
             + ") " + stroke + " (fill no) (layer \"F.SilkS\"))\n";
    }

    if( kind != "chip" && kind != "header" )
    {
        // dot outside pad 1, to its left
        const PAD& p1 = b.pads[0];
        t += "  (fp_circle (center " + f( p1.x - p1.w / 2 - 0.4 ) + " " + f( p1.y - p1.h / 2 - 0.3 ) + ") (end "
             + f( p1.x - p1.w / 2 - 0.4 + 0.15 ) + " " + f( p1.y - p1.h / 2 - 0.3 ) + ") " + stroke
             + " (fill yes) (layer \"F.SilkS\"))\n";
    }

    t += "  (fp_rect (start " + f( cx0 ) + " " + f( cy0 ) + ") (end " + f( cx1 ) + " " + f( cy1 ) + ") " + crt
         + " (fill no) (layer \"F.CrtYd\"))\n";

    for( const PAD& p : b.pads )
    {
        if( p.tht )
        {
            t += "  (pad \"" + p.number + "\" thru_hole " + ( p.square ? "rect" : "circle" ) + " (at " + f( p.x ) + " "
                 + f( p.y ) + ") (size " + f( p.w ) + " " + f( p.h ) + ") (drill " + f( p.drill )
                 + ") (layers \"*.Cu\" \"*.Mask\"))\n";
        }
        else
        {
            const bool ep = p.w == p.h && p.w > 1.0 && kind == "qfn";
            t += "  (pad \"" + p.number + "\" smd " + ( ep ? "rect" : "roundrect" ) + " (at " + f( p.x ) + " " + f( p.y )
                 + ") (size " + f( p.w ) + " " + f( p.h ) + ") (layers \"F.Cu\" \"F.Mask\" \"F.Paste\")"
                 + ( ep ? "" : " (roundrect_rratio 0.25)" ) + ")\n";
        }
    }

    t += ")\n";
    aText = t;
    aPads = (int) b.pads.size();
    return true;
}

} // namespace kicadd

/*
 * KiCadD MCP - charts as PNG (see plot.h).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "plot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>

#include <zlib.h>

namespace kicadd::mcp::plot
{

namespace
{

#include "plot_font.inc"

struct RGB
{
    uint8_t r, g, b;
};

// a colour-blind safe palette (Okabe-Ito without the yellow, which is lost on white)
const RGB PALETTE[] = { { 0, 114, 178 },  { 213, 94, 0 },   { 0, 158, 115 }, { 204, 121, 167 },
                        { 86, 180, 233 }, { 230, 159, 0 }, { 60, 60, 60 } };
const RGB INK = { 40, 40, 40 }, GRID = { 228, 228, 228 }, FRAME = { 150, 150, 150 }, LIMIT = { 200, 30, 30 };


struct GLYPH
{
    double                                          advance = 0;
    std::vector<std::vector<std::pair<double, double>>> strokes;
};


const std::map<char32_t, GLYPH>& glyphs()
{
    static std::map<char32_t, GLYPH> g = []
    {
        std::map<char32_t, GLYPH> out;

        for( const auto& [cp, def] : PLOT_FONT )
        {
            GLYPH       gl;
            std::string s = def;

            if( s.size() < 2 )
                continue;

            const double start = ( s[0] - 'R' ) / 21.0, end = ( s[1] - 'R' ) / 21.0;
            gl.advance = end - start;
            gl.strokes.emplace_back();

            for( size_t i = 2; i + 1 < s.size(); i += 2 )
            {
                if( s[i] == ' ' && s[i + 1] == 'R' )
                {
                    gl.strokes.emplace_back();
                    continue;
                }

                gl.strokes.back().emplace_back( ( s[i] - 'R' ) / 21.0 - start, ( s[i + 1] - 'R' - 8 ) / 21.0 );
            }

            out[cp] = std::move( gl );
        }

        return out;
    }();

    return g;
}


std::u32string decode( const std::string& aText )
{
    std::u32string out;

    for( size_t i = 0; i < aText.size(); )
    {
        unsigned char c = aText[i];

        if( c < 0x80 )
        {
            out += c;
            i += 1;
        }
        else if( ( c & 0xE0 ) == 0xC0 && i + 1 < aText.size() )
        {
            out += ( ( c & 0x1F ) << 6 ) | ( aText[i + 1] & 0x3F );
            i += 2;
        }
        else if( ( c & 0xF0 ) == 0xE0 && i + 2 < aText.size() )
        {
            out += ( ( c & 0x0F ) << 12 ) | ( ( aText[i + 1] & 0x3F ) << 6 ) | ( aText[i + 2] & 0x3F );
            i += 3;
        }
        else
        {
            out += U'?';
            i += 1;
        }
    }

    return out;
}


class RASTER
{
public:
    RASTER( int aW, int aH ) : w( aW ), h( aH ), rgb( size_t( aW ) * aH * 3, 255 ), cov( size_t( aW ) * aH, 0.f ) { unclip(); }

    void fill( int x0, int y0, int x1, int y1, RGB c )
    {
        for( int y = std::max( 0, y0 ); y < std::min( h, y1 ); ++y )
            for( int x = std::max( 0, x0 ); x < std::min( w, x1 ); ++x )
                put( x, y, c, 1 );
    }

    /// a polyline, antialiased, aWidth px, inside the clip
    void stroke( const std::vector<std::pair<double, double>>& aPts, double aWidth, RGB aColor, double aAlpha, bool aDashed = false )
    {
        if( aPts.size() < 2 )
            return;

        if( aDashed )
        {
            // 7 px on, 5 off, along the line
            double                                     along = 0;
            std::vector<std::pair<double, double>> run;

            for( size_t i = 1; i < aPts.size(); ++i )
            {
                auto [x0, y0] = aPts[i - 1];
                auto [x1, y1] = aPts[i];
                double len = std::hypot( x1 - x0, y1 - y0 );
                double t = 0;

                while( t < len )
                {
                    double phase = std::fmod( along, 12.0 );
                    double step = std::min( len - t, phase < 7 ? 7 - phase : 12 - phase );
                    std::pair<double, double> a{ x0 + ( x1 - x0 ) * t / len, y0 + ( y1 - y0 ) * t / len };
                    std::pair<double, double> b{ x0 + ( x1 - x0 ) * ( t + step ) / len, y0 + ( y1 - y0 ) * ( t + step ) / len };

                    if( phase < 7 )
                    {
                        if( run.empty() )
                            run.push_back( a );

                        run.push_back( b );
                    }
                    else if( !run.empty() )
                    {
                        stroke( run, aWidth, aColor, aAlpha );
                        run.clear();
                    }

                    t += std::max( step, 1e-9 );
                    along += std::max( step, 1e-9 );
                }
            }

            stroke( run, aWidth, aColor, aAlpha );
            return;
        }

        const double r = aWidth / 2;
        int          bx0 = w, by0 = h, bx1 = -1, by1 = -1;

        for( size_t i = 1; i < aPts.size(); ++i )
        {
            auto [x0, y0] = aPts[i - 1];
            auto [x1, y1] = aPts[i];

            if( !std::isfinite( x0 ) || !std::isfinite( y0 ) || !std::isfinite( x1 ) || !std::isfinite( y1 ) )
                continue;

            int ix0 = std::max( clipX0, (int) std::floor( std::min( x0, x1 ) - r - 1 ) );
            int ix1 = std::min( clipX1, (int) std::ceil( std::max( x0, x1 ) + r + 1 ) );
            int iy0 = std::max( clipY0, (int) std::floor( std::min( y0, y1 ) - r - 1 ) );
            int iy1 = std::min( clipY1, (int) std::ceil( std::max( y0, y1 ) + r + 1 ) );

            if( ix0 >= ix1 || iy0 >= iy1 )
                continue;

            const double dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;

            for( int y = iy0; y < iy1; ++y )
            {
                for( int x = ix0; x < ix1; ++x )
                {
                    double px = x + 0.5 - x0, py = y + 0.5 - y0;
                    double t = l2 > 0 ? std::clamp( ( px * dx + py * dy ) / l2, 0.0, 1.0 ) : 0;
                    double d = std::hypot( px - t * dx, py - t * dy );
                    float  a = (float) std::clamp( r + 0.5 - d, 0.0, 1.0 );

                    if( a > 0 )
                    {
                        float& c = cov[size_t( y ) * w + x];
                        c = std::max( c, a );
                    }
                }
            }

            bx0 = std::min( bx0, ix0 );
            by0 = std::min( by0, iy0 );
            bx1 = std::max( bx1, ix1 );
            by1 = std::max( by1, iy1 );
        }

        for( int y = by0; y < by1; ++y )
        {
            for( int x = bx0; x < bx1; ++x )
            {
                float& c = cov[size_t( y ) * w + x];

                if( c > 0 )
                {
                    put( x, y, aColor, c * aAlpha );
                    c = 0;
                }
            }
        }
    }

    double measure( const std::string& aText, double aSize ) const
    {
        const auto& g = glyphs();
        double      width = 0;

        for( char32_t c : decode( aText ) )
        {
            auto it = g.find( c );
            width += ( it == g.end() ? g.at( U'?' ).advance : it->second.advance ) * aSize;
        }

        return width;
    }

    /// text with its baseline at y; anchor 0 left, 0.5 centre, 1 right
    double text( const std::string& aText, double x, double y, double aSize, RGB aColor, double aAnchor = 0 )
    {
        const auto&    g = glyphs();
        std::u32string s = decode( aText );
        double         width = 0;

        for( char32_t c : s )
        {
            auto it = g.find( c );
            width += ( it == g.end() ? g.at( U'?' ).advance : it->second.advance ) * aSize;
        }

        double pen = x - width * aAnchor;
        int    sx0 = clipX0, sy0 = clipY0, sx1 = clipX1, sy1 = clipY1;
        clipX0 = clipY0 = 0;
        clipX1 = w;
        clipY1 = h;

        for( char32_t c : s )
        {
            auto         it = g.find( c );
            const GLYPH& gl = it == g.end() ? g.at( U'?' ) : it->second;

            for( const auto& st : gl.strokes )
            {
                std::vector<std::pair<double, double>> pts;

                for( auto [gx, gy] : st )
                    pts.emplace_back( pen + gx * aSize, y + gy * aSize );

                stroke( pts, std::max( 1.0, aSize / 9 ), aColor, 1 );
            }

            pen += gl.advance * aSize;
        }

        clipX0 = sx0;
        clipY0 = sy0;
        clipX1 = sx1;
        clipY1 = sy1;
        return width;
    }

    void clip( int x0, int y0, int x1, int y1 )
    {
        clipX0 = std::max( 0, x0 );
        clipY0 = std::max( 0, y0 );
        clipX1 = std::min( w, x1 );
        clipY1 = std::min( h, y1 );
    }

    void unclip() { clip( 0, 0, w, h ); }

    std::string png() const
    {
        std::string raw;
        raw.reserve( size_t( h ) * ( w * 3 + 1 ) );

        for( int y = 0; y < h; ++y )
        {
            raw += '\0';
            raw.append( reinterpret_cast<const char*>( &rgb[size_t( y ) * w * 3] ), size_t( w ) * 3 );
        }

        uLongf      zlen = compressBound( raw.size() );
        std::string z( zlen, '\0' );

        if( compress2( reinterpret_cast<Bytef*>( z.data() ), &zlen, reinterpret_cast<const Bytef*>( raw.data() ), raw.size(), 6 ) != Z_OK )
            return {};

        z.resize( zlen );

        std::string out = "\x89PNG\r\n\x1a\n";

        auto be32 = []( uint32_t v )
        {
            std::string s( 4, '\0' );
            s[0] = char( v >> 24 );
            s[1] = char( v >> 16 );
            s[2] = char( v >> 8 );
            s[3] = char( v );
            return s;
        };

        auto chunk = [&]( const char* aType, const std::string& aData )
        {
            std::string body = std::string( aType, 4 ) + aData;
            out += be32( (uint32_t) aData.size() ) + body
                   + be32( (uint32_t) crc32( 0, reinterpret_cast<const Bytef*>( body.data() ), (uInt) body.size() ) );
        };

        std::string ihdr = be32( w ) + be32( h ) + std::string( "\x08\x02\x00\x00\x00", 5 );
        chunk( "IHDR", ihdr );
        chunk( "IDAT", z );
        chunk( "IEND", "" );
        return out;
    }

    const int w, h;

private:
    void put( int x, int y, RGB c, double a )
    {
        if( x < 0 || y < 0 || x >= w || y >= h || a <= 0 )
            return;

        uint8_t* p = &rgb[( size_t( y ) * w + x ) * 3];
        p[0] = uint8_t( p[0] + ( c.r - p[0] ) * a + 0.5 );
        p[1] = uint8_t( p[1] + ( c.g - p[1] ) * a + 0.5 );
        p[2] = uint8_t( p[2] + ( c.b - p[2] ) * a + 0.5 );
    }

    std::vector<uint8_t> rgb;
    std::vector<float>   cov;
    int                  clipX0 = 0, clipY0 = 0, clipX1 = 1 << 30, clipY1 = 1 << 30;
};


RGB lighter( RGB c, double k )
{
    return { uint8_t( c.r + ( 255 - c.r ) * k ), uint8_t( c.g + ( 255 - c.g ) * k ), uint8_t( c.b + ( 255 - c.b ) * k ) };
}


struct AXIS
{
    double lo = 0, hi = 1;
    bool   log = false;

    double map( double v, double a0, double a1 ) const
    {
        double t = log ? ( std::log10( v ) - std::log10( lo ) ) / ( std::log10( hi ) - std::log10( lo ) ) : ( v - lo ) / ( hi - lo );
        return a0 + t * ( a1 - a0 );
    }

    /// (value, labelled)
    std::vector<std::pair<double, bool>> ticks() const
    {
        std::vector<std::pair<double, bool>> out;

        if( log )
        {
            int d0 = (int) std::floor( std::log10( lo ) + 1e-9 ), d1 = (int) std::ceil( std::log10( hi ) - 1e-9 );
            int every = std::max( 1, ( d1 - d0 + 7 ) / 8 );

            for( int d = d0; d <= d1; ++d )
            {
                for( int m : { 1, 2, 5 } )
                {
                    if( m != 1 && d1 - d0 > 3 )
                        continue;

                    double v = m * std::pow( 10.0, d );

                    if( v >= lo * ( 1 - 1e-9 ) && v <= hi * ( 1 + 1e-9 ) )
                        out.emplace_back( v, m == 1 ? ( d - d0 ) % every == 0 : d1 - d0 <= 1 );
                }
            }

            return out;
        }

        double raw = ( hi - lo ) / 6, mag = std::pow( 10.0, std::floor( std::log10( raw ) ) ), step = mag;

        for( double k : { 1.0, 2.0, 5.0, 10.0 } )
        {
            if( k * mag >= raw )
            {
                step = k * mag;
                break;
            }
        }

        for( double v = std::ceil( lo / step - 1e-9 ) * step; v <= hi + step * 1e-9; v += step )
            out.emplace_back( std::fabs( v ) < step * 1e-9 ? 0.0 : v, true );

        return out;
    }
};


AXIS range( std::vector<double> aValues, bool aLog, double aPad )
{
    AXIS a;
    a.log = aLog;
    double lo = 1e300, hi = -1e300;

    for( double v : aValues )
    {
        if( !std::isfinite( v ) || ( aLog && v <= 0 ) )
            continue;

        lo = std::min( lo, v );
        hi = std::max( hi, v );
    }

    if( lo > hi )
    {
        lo = aLog ? 1 : 0;
        hi = aLog ? 10 : 1;
    }

    if( aLog )
    {
        if( hi / lo < 1.5 )
        {
            lo /= 1.25;
            hi *= 1.25;
        }

        double k = std::pow( hi / lo, aPad );
        a.lo = lo / k;
        a.hi = hi * k;
    }
    else
    {
        if( hi - lo < 1e-12 * std::max( 1.0, std::fabs( hi ) ) )
        {
            double d = std::fabs( hi ) > 0 ? std::fabs( hi ) * 0.1 : 1;
            lo -= d;
            hi += d;
        }

        double d = ( hi - lo ) * aPad;
        a.lo = lo - d;
        a.hi = hi + d;
    }

    return a;
}


std::string number( double v, int aSig = 3 )
{
    char buf[48];
    std::snprintf( buf, sizeof buf, "%.*g", aSig, v );
    std::string s = buf;

    if( s.find( 'e' ) == std::string::npos && s.find( '.' ) != std::string::npos )
    {
        while( s.back() == '0' )
            s.pop_back();

        if( s.back() == '.' )
            s.pop_back();
    }

    return s == "-0" ? "0" : s;
}

} // namespace


std::string Tick( double aValue, const std::string& aUnit )
{
    if( aUnit == "dB" || aUnit == "°" || aUnit == "%" || aUnit.empty() )
        return number( aValue ) + ( aUnit.empty() ? "" : aUnit == "°" || aUnit == "%" ? aUnit : " " + aUnit );

    if( aValue == 0 )
        return "0";

    static const char* PREFIX[] = { "f", "p", "n", "µ", "m", "", "k", "M", "G", "T" };
    int                e = (int) std::floor( std::log10( std::fabs( aValue ) ) / 3 + 1e-9 );
    e = std::clamp( e, -5, 4 );
    return number( aValue / std::pow( 1000.0, e ) ) + " " + PREFIX[e + 5] + aUnit;
}


std::string Png( const CHART& aChart )
{
    if( aChart.panels.empty() )
        return {};

    const int left = 86, right = 36, top = aChart.title.empty() ? 14 : 40, gap = 22, bottom = 46;
    const int W = std::clamp( aChart.width, 320, 2400 );
    const int H = top + (int) aChart.panels.size() * aChart.panelHeight + ( (int) aChart.panels.size() - 1 ) * gap + bottom;
    RASTER    img( W, H );
    const int x0 = left, x1 = W - right;

    // the shared x axis
    std::vector<double> xs;

    for( const PANEL& p : aChart.panels )
        for( const SERIES& s : p.series )
            xs.insert( xs.end(), s.x.begin(), s.x.end() );

    AXIS xa = range( xs, aChart.logX, 0 );

    if( !aChart.title.empty() )
        img.text( aChart.title, x0, 26, 15, INK );

    for( size_t pi = 0; pi < aChart.panels.size(); ++pi )
    {
        const PANEL& p = aChart.panels[pi];
        const int    y0 = top + (int) pi * ( aChart.panelHeight + gap ), y1 = y0 + aChart.panelHeight;
        std::vector<double> ys;

        for( const SERIES& s : p.series )
        {
            for( size_t i = 0; i < s.y.size() && i < s.x.size(); ++i )
            {
                if( s.x[i] >= xa.lo && s.x[i] <= xa.hi )
                    ys.push_back( s.y[i] );
            }
        }

        if( p.limit )
            ys.push_back( *p.limit );

        AXIS ya = range( ys, p.logY, 0.05 );

        // grid and ticks
        for( auto [v, lab] : xa.ticks() )
        {
            double x = xa.map( v, x0, x1 );
            img.fill( (int) std::lround( x ), y0, (int) std::lround( x ) + 1, y1, GRID );

            if( lab && pi + 1 == aChart.panels.size() )
                img.text( Tick( v, aChart.xUnit ), x, y1 + 18, 11, INK, 0.5 );
        }

        for( auto [v, lab] : ya.ticks() )
        {
            double y = ya.map( v, y1, y0 );
            img.fill( x0, (int) std::lround( y ), x1, (int) std::lround( y ) + 1, GRID );

            if( lab )
                img.text( Tick( v, p.yUnit ), x0 - 6, y + 4, 11, INK, 1 );
        }

        img.fill( x0, y0, x1, y0 + 1, FRAME );
        img.fill( x0, y1, x1, y1 + 1, FRAME );
        img.fill( x0, y0, x0 + 1, y1, FRAME );
        img.fill( x1, y0, x1 + 1, y1 + 1, FRAME );

        img.clip( x0 + 1, y0 + 1, x1, y1 );
        std::vector<std::pair<double, double>> drawn;   // every point drawn (where the legend goes)

        if( p.limit )
        {
            double y = ya.map( *p.limit, y1, y0 );
            img.stroke( { { x0, y }, { x1, y } }, 1.5, LIMIT, 0.9, true );
        }

        // faint ones under the others
        for( int pass : { 0, 1 } )
        {
            for( size_t si = 0; si < p.series.size(); ++si )
            {
                const SERIES& s = p.series[si];

                if( s.faint != ( pass == 0 ) )
                    continue;

                const RGB col = PALETTE[( s.color >= 0 ? s.color : (int) si ) % std::size( PALETTE )];
                std::vector<std::pair<double, double>> pts;
                const size_t n = std::min( s.x.size(), s.y.size() );

                // many more points than pixels: each pixel column's first, min, max and last
                if( n > size_t( 4 * ( x1 - x0 ) ) )
                {
                    int    col0 = -1;
                    double fy = 0, mn = 0, mx = 0, ly = 0, cx = 0;
                    size_t imn = 0, imx = 0;

                    auto flush = [&]()
                    {
                        if( col0 < 0 )
                            return;

                        pts.emplace_back( cx, fy );
                        pts.emplace_back( cx, imn < imx ? mn : mx );
                        pts.emplace_back( cx, imn < imx ? mx : mn );
                        pts.emplace_back( cx, ly );
                    };

                    for( size_t i = 0; i < n; ++i )
                    {
                        if( ( xa.log && s.x[i] <= 0 ) || ( ya.log && s.y[i] <= 0 ) || !std::isfinite( s.y[i] ) )
                            continue;

                        double px = xa.map( s.x[i], x0, x1 ), py = ya.map( s.y[i], y1, y0 );
                        int    c = (int) px;

                        if( c != col0 )
                        {
                            flush();
                            col0 = c;
                            cx = px;
                            fy = mn = mx = ly = py;
                            imn = imx = i;
                        }

                        if( py < mn )
                        {
                            mn = py;
                            imn = i;
                        }

                        if( py > mx )
                        {
                            mx = py;
                            imx = i;
                        }

                        ly = py;
                    }

                    flush();
                }
                else
                {
                    for( size_t i = 0; i < n; ++i )
                    {
                        if( ( xa.log && s.x[i] <= 0 ) || ( ya.log && s.y[i] <= 0 ) || !std::isfinite( s.y[i] ) )
                            continue;

                        pts.emplace_back( xa.map( s.x[i], x0, x1 ), ya.map( s.y[i], y1, y0 ) );
                    }
                }

                // a lone point: a short tick, still seen
                if( pts.size() == 1 )
                    pts.push_back( { pts[0].first + 2, pts[0].second } );

                img.stroke( pts, s.faint ? 1.2 : 2.0, s.faint ? lighter( col, 0.35 ) : col, s.faint ? 0.8 : 1, s.dashed );
                drawn.insert( drawn.end(), pts.begin(), pts.end() );
            }
        }

        img.unclip();

        // legend: in the corner with the least of the lines under it, one row per labelled series
        // (and the limit)
        std::vector<std::tuple<std::string, RGB, bool, bool>> rows;

        for( size_t si = 0; si < p.series.size(); ++si )
        {
            const SERIES& s = p.series[si];

            if( !s.label.empty() )
                rows.emplace_back( s.label, PALETTE[( s.color >= 0 ? s.color : (int) si ) % std::size( PALETTE )], s.faint, s.dashed );
        }

        if( p.limit && !p.limitLabel.empty() )
            rows.emplace_back( p.limitLabel, LIMIT, false, true );

        if( !rows.empty() )
        {
            double widest = 0;

            for( const auto& row : rows )
                widest = std::max( widest, img.measure( std::get<0>( row ), 11 ) );

            const int boxH = std::min( (int) rows.size() * 16 + 8, y1 - y0 - 8 );
            const int boxW = (int) widest + 44;
            int       bx1 = x1 - 4, by0 = y0 + 4;
            size_t    least = SIZE_MAX;

            // top right, top left, bottom right, bottom left: the first with the fewest points
            for( auto [rx, ty] : { std::pair{ true, true }, { false, true }, { true, false }, { false, false } } )
            {
                const int cx1 = rx ? x1 - 4 : x0 + 4 + boxW, cy0 = ty ? y0 + 4 : y1 - 4 - boxH;
                size_t    n = 0;

                for( auto [px, py] : drawn )
                {
                    if( px >= cx1 - boxW - 4 && px <= cx1 + 4 && py >= cy0 - 4 && py <= cy0 + boxH + 4 )
                        n++;
                }

                if( n < least )
                {
                    least = n;
                    bx1 = cx1;
                    by0 = cy0;
                }
            }

            img.fill( bx1 - boxW, by0, bx1, by0 + boxH, FRAME );
            img.fill( bx1 - boxW + 1, by0 + 1, bx1 - 1, by0 + boxH - 1, { 255, 255, 255 } );
            int ly = by0 + 14;

            for( const auto& [label, col, faint, dashed] : rows )
            {
                if( ly > by0 + boxH - 2 )
                    break;

                double tw = img.text( label, bx1 - 6, ly, 11, INK, 1 );
                img.stroke( { { bx1 - 36 - tw, ly - 4.0 }, { bx1 - 12 - tw, ly - 4.0 } }, faint ? 1.2 : 2.0, faint ? lighter( col, 0.35 ) : col,
                            1, dashed );
                ly += 16;
            }
        }

    }

    return img.png();
}


namespace
{
std::mutex                                                     g_sinkMutex;
std::function<void( const CHART&, const std::string& )>        g_sink;
} // namespace


void SetSink( std::function<void( const CHART&, const std::string& )> aSink )
{
    std::lock_guard<std::mutex> lock( g_sinkMutex );
    g_sink = std::move( aSink );
}


bool HasSink()
{
    std::lock_guard<std::mutex> lock( g_sinkMutex );
    return (bool) g_sink;
}


void Publish( const CHART& aChart, const std::string& aTool )
{
    std::function<void( const CHART&, const std::string& )> sink;

    {
        std::lock_guard<std::mutex> lock( g_sinkMutex );
        sink = g_sink;
    }

    if( sink && !aChart.panels.empty() )
        sink( aChart, aTool );
}

} // namespace kicadd::mcp::plot

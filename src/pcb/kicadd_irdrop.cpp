/*
 * KiCadD: DC IR drop for the board audit (see kicadd_irdrop.h).
 *
 * The network, per rail:
 * - zone fills (and the pads and via rings on those layers) on a grid of square cells, the cell
 *   size picked so a rail has at most MAX_CELLS cells (0.1 mm at least); neighbouring cells are
 *   one square of that layer's copper apart (sheet conductance sigma * t);
 * - a track is one resistor, sigma * t * width / length, from its start to its end; an end
 *   joins the via or pad it lies on, else the zone cell under it, else the other tracks ending
 *   at the same point;
 * - a pad is one node per copper layer it is flashed on (its cells joined into it); vias and
 *   plated holes join their layers through the barrel: sigma * pi ((r + 20 um)^2 - r^2) / the
 *   layer distance of the stackup.
 * Copper thickness per layer from the stackup (35 um outer, 17.5 um inner when it has none),
 * copper at 20 C. The source pads are one node at 0 V; each load pad draws its share; the system
 * (the source's island only) is solved with a sparse LDLT. Linear, so per A scales.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_irdrop.h"
#include "common/kicadd_ipc2221.h"
#include "pcb/kicadd_netnames.h"

#include <board.h>
#include <board_design_settings.h>
#include <board_stackup_manager/board_stackup.h>
#include <footprint.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_track.h>
#include <zone.h>
#include <geometry/approximation.h>
#include <geometry/shape_poly_set.h>

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <unordered_map>

namespace kicadd
{

using namespace kicadd::board;
using netnames::GLOBS;

namespace
{

constexpr double RHO = 1.72e-8;        // ohm m, copper at 20 C
constexpr double PLATING = 20e-6;      // m, the copper on a hole's wall
constexpr double MAX_CELLS = 600000;   // copper cells per rail, all layers together (default)
constexpr double MAX_ASKED = 2000000;  // with ir_mesh_mm
constexpr double MAX_GRID = 40e6;      // grid cells (copper or not) over the layers with fills: memory
constexpr int    COARSE = 150000;      // nm: a coarser mesh blurs antipads (a BGA's "Swiss cheese")
constexpr int    H_MIN = 100000;       // nm, the smallest cell
constexpr int    BUCKET = 2000000;     // nm, the pad / via lookup grid

std::string str( const wxString& s )
{
    return s.ToStdString( wxConvUTF8 );
}

std::string mm( double aMm, int aDigits = 2 )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.*f", aDigits, aMm );
    std::string s( buf );

    if( s.find( '.' ) != std::string::npos )
    {
        while( s.back() == '0' )
            s.pop_back();

        if( s.back() == '.' )
            s.pop_back();
    }

    return s == "-0" ? "0" : s;
}

/// Two or three significant digits: 0.0123 -> "0.012", 1.234 -> "1.23", 41.7 -> "41.7", 312 -> "312".
std::string sig( double v )
{
    const double a = std::fabs( v );
    return mm( v, a >= 100 ? 0 : a >= 10 ? 1 : a >= 1 ? 2 : a >= 0.1 ? 3 : 4 );
}

std::string at( const VECTOR2I& p )
{
    return "(" + mm( p.x / 1e6 ) + ", " + mm( p.y / 1e6 ) + ")";
}

void line( BoardAuditResponse& aOut, const char* aSeverity, const std::string& aMsg,
           std::optional<VECTOR2I> aAt = std::nullopt )
{
    BoardAuditFinding* f = aOut.add_findings();
    f->set_category( "ir_drop" );
    f->set_severity( aSeverity );
    f->set_message( aMsg );

    if( aAt )
    {
        f->mutable_at()->set_x( aAt->x );
        f->mutable_at()->set_y( aAt->y );
    }
}

/// "VCC" answers to "VCC", "/VCC" and "/Sheet/VCC" (as the audit's currents)
bool sameNet( const std::string& aNet, const std::string& aWanted )
{
    if( aNet == aWanted || aNet == "/" + aWanted )
        return true;

    return aNet.size() > aWanted.size()
           && aNet.compare( aNet.size() - aWanted.size() - 1, aWanted.size() + 1, "/" + aWanted ) == 0;
}

std::string refOf( const PAD* aPad )
{
    const FOOTPRINT* fp = aPad->GetParentFootprint();
    return fp ? str( fp->GetReference() ) : std::string( "?" );
}

std::string padName( const PAD* aPad )
{
    return refOf( aPad ) + ( aPad->GetNumber().IsEmpty() ? std::string() : "." + str( aPad->GetNumber() ) );
}

/// The letters a reference starts with, upper case: "U12" -> "U", "TP3" -> "TP".
std::string prefix( const std::string& aRef )
{
    std::string p;

    for( char c : aRef )
    {
        if( !std::isalpha( (unsigned char) c ) )
            break;

        p += (char) std::toupper( (unsigned char) c );
    }

    return p;
}

/// A supply by name (netnames::SupplyName), or a voltage with one word at most around it: MGT_0V85,
/// 5VREG, 3V3_A (not I2C5_SCL_1V8 or SFP_LEDY_1V8: signals in a 1.8 V domain).
bool railName( const std::string& aNet )
{
    static const std::regex re( R"(^([A-Za-z0-9]+[_.-])?\d+V\d*[A-Za-z]*([_.-][A-Za-z0-9]+)?$)" );
    const size_t            slash = aNet.rfind( '/' );
    return netnames::SupplyName( aNet ) || std::regex_match( slash == std::string::npos ? aNet : aNet.substr( slash + 1 ), re );
}

/// Named like a control signal of a supply: PWR_EN, VBUS_EN, PI_nLED_PWR, PWR_BUT, CARRIER_PWR_ON ...
bool controlName( const std::string& aNet )
{
    static const std::regex re( R"((^|.*[_.\-/])N?(EN|ENABLE|ON|OFF|BUT|BTN|LED|PG|PGOOD|GOOD|OK|FB|SNS|SENSE|ALERT|FAULT|FLT|BAD|DET|INT|CTRL|SW|LX)($|[_.\-].*))",
                                std::regex::icase );
    std::string name = aNet;   // "/DCDC/~{VDDIN_PWR_BAD}": the overbar's braces out
    name.erase( std::remove_if( name.begin(), name.end(), []( char c ) { return c == '~' || c == '{' || c == '}'; } ), name.end() );
    return std::regex_match( name, re );
}

bool groundName( const std::string& aNet )
{
    static const std::regex re( R"(^(.*GND.*|VSS.*|0V)$)", std::regex::icase );
    const size_t            slash = aNet.rfind( '/' );
    return std::regex_match( slash == std::string::npos ? aNet : aNet.substr( slash + 1 ), re );
}

struct DSU
{
    std::vector<int> p;

    int add()
    {
        p.push_back( (int) p.size() );
        return (int) p.size() - 1;
    }

    int find( int x )
    {
        while( p[x] != x )
        {
            p[x] = p[p[x]];
            x = p[x];
        }

        return x;
    }

    void join( int a, int b )
    {
        a = find( a );
        b = find( b );

        if( a != b )
            p[b] = a;
    }
};

enum KIND { K_TRACK = 0, K_PLANE = 1, K_BARREL = 2 };

struct EDGE
{
    int    a, b;
    double g;      // S
    KIND   kind;
    int    ref;    // K_TRACK: the track, K_BARREL: the barrel, K_PLANE: the layer
};

struct BARREL
{
    const BOARD_CONNECTED_ITEM* item;
    VECTOR2I                    pos;
    int                         drill;
    bool                        via;
};

struct GRID
{
    int64_t                                  x0 = 0, y0 = 0;
    int                                      h = 0, nx = 0, ny = 0;
    std::map<PCB_LAYER_ID, std::vector<int>> cells;   // a node per cell, -1: no copper

    std::vector<int>* on( PCB_LAYER_ID aLayer )
    {
        auto it = cells.find( aLayer );
        return it == cells.end() ? nullptr : &it->second;
    }

    bool cellOf( const VECTOR2I& p, int& i, int& j ) const
    {
        i = (int) std::floor( ( p.x - x0 ) / (double) h );
        j = (int) std::floor( ( p.y - y0 ) / (double) h );
        return i >= 0 && j >= 0 && i < nx && j < ny;
    }

    VECTOR2I centre( int i, int j ) const
    {
        return VECTOR2I( (int) ( x0 + ( i + 0.5 ) * h ), (int) ( y0 + ( j + 0.5 ) * h ) );
    }
};

/// Calls f(i, j) for every grid cell whose centre is inside the polygon set (even-odd over its
/// outlines and holes; the bridges of a fractured set cancel out).
template <class F>
void raster( const SHAPE_POLY_SET& aPoly, const GRID& g, F&& f )
{
    const BOX2I bb = aPoly.BBox();
    const int   j0 = std::max( 0, (int) std::ceil( ( bb.GetTop() - g.y0 ) / (double) g.h - 0.5 ) );
    const int   j1 = std::min( g.ny - 1, (int) std::floor( ( bb.GetBottom() - g.y0 ) / (double) g.h - 0.5 ) );

    if( j1 < j0 )
        return;

    std::vector<std::vector<double>> xs( j1 - j0 + 1 );

    for( auto it = aPoly.CIterateSegmentsWithHoles(); it; it++ )
    {
        const SEG    s = *it;
        const double ya = s.A.y, yb = s.B.y;

        if( ya == yb )
            continue;

        const double lo = std::min( ya, yb ), hi = std::max( ya, yb );
        const int    ja = std::max( j0, (int) std::ceil( ( lo - g.y0 ) / g.h - 0.5 ) );
        const int    jb = std::min( j1, (int) std::ceil( ( hi - g.y0 ) / g.h - 0.5 ) - 1 );

        for( int j = ja; j <= jb; j++ )
        {
            const double yc = g.y0 + ( j + 0.5 ) * g.h;
            xs[j - j0].push_back( s.A.x + ( yc - ya ) * ( s.B.x - s.A.x ) / ( yb - ya ) );
        }
    }

    for( int r = 0; r < (int) xs.size(); r++ )
    {
        std::vector<double>& row = xs[r];
        std::sort( row.begin(), row.end() );

        for( size_t k = 0; k + 1 < row.size(); k += 2 )
        {
            const int ia = std::max( 0, (int) std::ceil( ( row[k] - g.x0 ) / g.h - 0.5 ) );
            const int ib = std::min( g.nx - 1, (int) std::floor( ( row[k + 1] - g.x0 ) / g.h - 0.5 ) );

            for( int i = ia; i <= ib; i++ )
                f( i, j0 + r );
        }
    }
}

struct LOAD
{
    const PAD* pad;
    double     amps;
};

struct VIAI
{
    VECTOR2I pos;
    double   amps;
    int      drill;
};

struct RAIL
{
    int         code = 0;
    std::string name;

    std::vector<const PAD*> source;
    std::string             sourceSaid;   // "U1.5 (VOUT)"
    std::string             guessed;      // why, when guessed: "a power_out pin"
    std::vector<LOAD>       loads;
    std::string             loadsSaid;    // "U5 (6 pads), U7"
    double                  amps = 1;
    bool                    perAmp = true;
    std::string             problem;      // not solved: why
    bool                    noSource = false;

    // results (V, A, W)
    double                  worst = 0, least = HUGE_VAL;
    const PAD*              worstPad = nullptr;
    std::vector<const PAD*> islands;      // loads with no copper to the source
    double                  loss[3] = { 0, 0, 0 };
    std::vector<VIAI>       vias;         // the busiest first
    const PCB_TRACK*        hotTrack = nullptr;
    double                  hotAmps = 0;
    double                  hMm = 0;
    std::string             meshWhy;      // why the mesh is not the one asked for
    int                     unfilled = 0; // its zones with no fill (nothing to solve on)
    int                     nodes = 0;
};

/// The copper layers of the board, top to bottom.
std::vector<PCB_LAYER_ID> copperLayers( const BOARD& aBoard )
{
    std::vector<PCB_LAYER_ID> out;

    for( PCB_LAYER_ID l : aBoard.GetEnabledLayers().CuStack() )
        out.push_back( l );

    return out;
}

/// Builds and solves the network of one rail (source, loads and amps set).
void solve( BOARD& aBoard, RAIL& r, const std::map<PCB_LAYER_ID, double>& aCopperM, double aMeshMm )
{
    const double              sigma = 1.0 / RHO;
    const std::vector<PCB_LAYER_ID> cu = copperLayers( aBoard );
    BOARD_STACKUP&            stackup = aBoard.GetDesignSettings().GetStackupDescriptor();
    const int                 boardT = aBoard.GetDesignSettings().GetBoardThickness();

    auto thick = [&]( PCB_LAYER_ID l ) -> double
    {
        auto it = aCopperM.find( l );
        return it != aCopperM.end() ? it->second : IsExternalCopperLayer( l ) ? 35e-6 : 17.5e-6;
    };

    auto depth = [&]( PCB_LAYER_ID a, PCB_LAYER_ID b ) -> double   // m
    {
        int d = stackup.GetCount() > 0 ? stackup.GetLayerDistance( a, b ) : 0;

        if( d <= 0 )
        {
            const auto ia = std::find( cu.begin(), cu.end(), a ) - cu.begin();
            const auto ib = std::find( cu.begin(), cu.end(), b ) - cu.begin();
            d = (int) ( (double) boardT / std::max<size_t>( 1, cu.size() - 1 ) * std::abs( ia - ib ) );
        }

        return std::max( d, 10000 ) * 1e-9;
    };

    // ---- the rail's copper
    std::vector<const PCB_TRACK*> tracks;
    std::vector<const PCB_VIA*>   viaList;
    std::vector<const PAD*>       pads;
    std::vector<std::pair<PCB_LAYER_ID, std::shared_ptr<SHAPE_POLY_SET>>> fills;

    for( PCB_TRACK* t : aBoard.Tracks() )
    {
        if( t->GetNetCode() != r.code )
            continue;

        if( t->Type() == PCB_VIA_T )
            viaList.push_back( static_cast<const PCB_VIA*>( t ) );
        else
            tracks.push_back( t );
    }

    for( FOOTPRINT* fp : aBoard.Footprints() )
    {
        for( PAD* p : fp->Pads() )
        {
            if( p->GetNetCode() == r.code )
                pads.push_back( p );
        }
    }

    double area = 0;
    BOX2I  box;
    bool   haveBox = false;

    for( ZONE* z : aBoard.Zones() )
    {
        if( z->GetIsRuleArea() || z->GetNetCode() != r.code )
            continue;

        r.unfilled += !z->IsFilled();

        // the fill, not the outline: other nets' clearances (antipads round vias and pads) are cut out
        for( PCB_LAYER_ID l : z->GetLayerSet().CuStack() )
        {
            if( !aBoard.GetEnabledLayers().Contains( l ) )
                continue;

            std::shared_ptr<SHAPE_POLY_SET> poly = z->GetFilledPolysList( l );

            if( !poly || poly->OutlineCount() == 0 )
                continue;

            fills.emplace_back( l, poly );
            area += poly->Area();
            const BOX2I bb = poly->BBox();
            box = haveBox ? box.Merge( bb ) : bb;
            haveBox = true;
        }
    }

    DSU               dsu;
    std::vector<EDGE> edges;
    GRID              grid;

    if( haveBox )
    {
        for( const PAD* p : pads )
            box.Merge( p->GetBoundingBox() );

        // the cell: asked for (ir_mesh_mm), else 0.1 mm; larger when the copper needs more cells than
        // the budget, or the grid over the layers more than memory allows
        std::set<PCB_LAYER_ID> fillLayers;

        for( const auto& f : fills )
            fillLayers.insert( f.first );

        const double want = aMeshMm > 0 ? std::max( aMeshMm * 1e6, 20000.0 ) : H_MIN;
        const double byCells = std::sqrt( area / ( aMeshMm > 0 ? MAX_ASKED : MAX_CELLS ) );
        const double byGrid = std::sqrt( (double) box.GetWidth() * box.GetHeight() * fillLayers.size() / MAX_GRID );
        grid.h = (int) std::max( { want, byCells, byGrid } );

        if( aMeshMm > 0 && grid.h > want * 1.01 )
            r.meshWhy = "asked " + mm( aMeshMm, 3 ) + ", the cell limit";

        box.Inflate( grid.h );
        grid.x0 = box.GetLeft();
        grid.y0 = box.GetTop();
        grid.nx = (int) ( box.GetWidth() / grid.h ) + 1;
        grid.ny = (int) ( box.GetHeight() / grid.h ) + 1;
        r.hMm = grid.h / 1e6;

        for( auto& [l, poly] : fills )
        {
            std::vector<int>& cells = grid.cells[l];

            if( cells.empty() )
                cells.assign( (size_t) grid.nx * grid.ny, -1 );

            raster( *poly, grid,
                    [&]( int i, int j )
                    {
                        int& c = cells[(size_t) j * grid.nx + i];

                        if( c < 0 )
                            c = dsu.add();
                    } );
        }
    }

    // a node per flashed copper layer of a pad or via, its cells joined in
    std::map<std::pair<const BOARD_ITEM*, PCB_LAYER_ID>, int> itemNode;
    std::vector<BARREL>                                     barrels;

    auto joinCells = [&]( int aNode, PCB_LAYER_ID l, const SHAPE_POLY_SET* aPoly, const VECTOR2I& aPos, int aRadius )
    {
        std::vector<int>* cells = grid.on( l );

        if( !cells )
            return;

        bool any = false;
        auto take = [&]( int i, int j )
        {
            int& c = ( *cells )[(size_t) j * grid.nx + i];

            if( c < 0 )
                c = dsu.add();

            dsu.join( aNode, c );
            any = true;
        };

        if( aPoly )
        {
            raster( *aPoly, grid, take );
        }
        else
        {
            int i0, j0, i1, j1;
            grid.cellOf( aPos - VECTOR2I( aRadius, aRadius ), i0, j0 );
            grid.cellOf( aPos + VECTOR2I( aRadius, aRadius ), i1, j1 );

            for( int j = std::max( 0, j0 ); j <= std::min( grid.ny - 1, j1 ); j++ )
            {
                for( int i = std::max( 0, i0 ); i <= std::min( grid.nx - 1, i1 ); i++ )
                {
                    if( ( grid.centre( i, j ) - aPos ).EuclideanNorm() <= aRadius )
                        take( i, j );
                }
            }
        }

        int i, j;

        if( !any && grid.cellOf( aPos, i, j ) && ( *cells )[(size_t) j * grid.nx + i] >= 0 )
            dsu.join( aNode, ( *cells )[(size_t) j * grid.nx + i] );
    };

    auto barrel = [&]( const BOARD_CONNECTED_ITEM* aItem, const std::vector<PCB_LAYER_ID>& aLayers, int aDrill, bool aVia )
    {
        const int    idx = (int) barrels.size();
        const double rad = aDrill / 2.0 * 1e-9;
        const double ring = 3.14159265358979 * ( ( rad + PLATING ) * ( rad + PLATING ) - rad * rad );
        barrels.push_back( { aItem, aItem->GetPosition(), aDrill, aVia } );

        for( size_t k = 1; k < aLayers.size(); k++ )
        {
            edges.push_back( { itemNode[{ aItem, aLayers[k - 1] }], itemNode[{ aItem, aLayers[k] }],
                               sigma * ring / depth( aLayers[k - 1], aLayers[k] ), K_BARREL, idx } );
        }
    };

    for( const PAD* p : pads )
    {
        std::vector<PCB_LAYER_ID> on;

        for( PCB_LAYER_ID l : cu )
        {
            if( p->IsOnLayer( l ) && p->FlashLayer( l ) )
                on.push_back( l );
        }

        for( PCB_LAYER_ID l : on )
        {
            const int n = dsu.add();
            itemNode[{ p, l }] = n;
            joinCells( n, l, p->GetEffectivePolygon( l, ERROR_INSIDE ).get(), p->GetPosition(), 0 );
        }

        if( on.size() > 1 && p->HasHole() && p->GetAttribute() == PAD_ATTRIB::PTH )
            barrel( p, on, std::min( p->GetDrillSize().x, p->GetDrillSize().y ), false );
    }

    for( const PCB_VIA* v : viaList )
    {
        PCB_LAYER_ID top, bot;
        v->LayerPair( &top, &bot );
        const auto it = std::find( cu.begin(), cu.end(), top ), ib = std::find( cu.begin(), cu.end(), bot );

        if( it == cu.end() || ib == cu.end() )
            continue;

        std::vector<PCB_LAYER_ID> on;

        for( auto l = std::min( it, ib ); l <= std::max( it, ib ); ++l )
        {
            if( v->FlashLayer( *l ) )
                on.push_back( *l );
        }

        for( PCB_LAYER_ID l : on )
        {
            const int n = dsu.add();
            itemNode[{ v, l }] = n;
            joinCells( n, l, nullptr, v->GetPosition(), v->GetWidth( l ) / 2 );
        }

        if( on.size() > 1 )
            barrel( v, on, v->GetPrimaryDrillSize().x, true );
    }

    // pads and vias near a point, per layer
    std::map<std::tuple<int, int, int>, std::vector<const BOARD_CONNECTED_ITEM*>> bucket;

    auto bucketAdd = [&]( const BOARD_CONNECTED_ITEM* aItem, PCB_LAYER_ID l, const BOX2I& bb )
    {
        for( int by = (int) std::floor( bb.GetTop() / (double) BUCKET ); by <= (int) std::floor( bb.GetBottom() / (double) BUCKET ); by++ )
        {
            for( int bx = (int) std::floor( bb.GetLeft() / (double) BUCKET ); bx <= (int) std::floor( bb.GetRight() / (double) BUCKET ); bx++ )
                bucket[{ (int) l, bx, by }].push_back( aItem );
        }
    };

    for( const auto& [key, n] : itemNode )
        bucketAdd( static_cast<const BOARD_CONNECTED_ITEM*>( key.first ), key.second, key.first->GetBoundingBox() );

    std::map<std::tuple<int, int, int>, int> freeEnd;

    auto endNode = [&]( const VECTOR2I& p, PCB_LAYER_ID l ) -> int
    {
        auto b = bucket.find( { (int) l, (int) std::floor( p.x / (double) BUCKET ), (int) std::floor( p.y / (double) BUCKET ) } );

        if( b != bucket.end() )
        {
            for( const BOARD_CONNECTED_ITEM* it : b->second )
            {
                if( it->Type() == PCB_VIA_T )
                {
                    const PCB_VIA* v = static_cast<const PCB_VIA*>( it );

                    if( ( p - v->GetPosition() ).EuclideanNorm() <= v->GetWidth( l ) / 2 )
                        return itemNode[{ it, l }];
                }
            }

            for( const BOARD_CONNECTED_ITEM* it : b->second )
            {
                if( it->Type() == PCB_PAD_T && static_cast<const PAD*>( it )->HitTest( p, 0, l ) )
                    return itemNode[{ it, l }];
            }
        }

        int i, j;

        if( std::vector<int>* cells = grid.on( l ); cells && grid.cellOf( p, i, j ) && ( *cells )[(size_t) j * grid.nx + i] >= 0 )
            return ( *cells )[(size_t) j * grid.nx + i];

        auto [f, added] = freeEnd.try_emplace( { (int) l, p.x, p.y }, 0 );

        if( added )
            f->second = dsu.add();

        return f->second;
    };

    // tracks: an end that lands along another track (a T) splits that one there
    std::vector<int> startNode( tracks.size() ), endNodeOf( tracks.size() );
    std::map<std::tuple<int, int, int>, std::vector<int>> trackBucket;

    for( size_t k = 0; k < tracks.size(); k++ )
    {
        const PCB_TRACK* t = tracks[k];
        startNode[k] = endNode( t->GetStart(), t->GetLayer() );
        endNodeOf[k] = endNode( t->GetEnd(), t->GetLayer() );

        if( t->Type() != PCB_TRACE_T )
            continue;

        const BOX2I bb = t->GetBoundingBox();

        for( int by = (int) std::floor( bb.GetTop() / (double) BUCKET ); by <= (int) std::floor( bb.GetBottom() / (double) BUCKET ); by++ )
        {
            for( int bx = (int) std::floor( bb.GetLeft() / (double) BUCKET ); bx <= (int) std::floor( bb.GetRight() / (double) BUCKET ); bx++ )
                trackBucket[{ (int) t->GetLayer(), bx, by }].push_back( (int) k );
        }
    }

    std::vector<std::vector<std::pair<double, int>>> splits( tracks.size() );   // (t along, node)

    for( size_t k = 0; k < tracks.size(); k++ )
    {
        const PCB_TRACK* t = tracks[k];

        for( int side : { 0, 1 } )
        {
            const VECTOR2I p = side ? t->GetEnd() : t->GetStart();
            const int      node = side ? endNodeOf[k] : startNode[k];
            auto           b = trackBucket.find( { (int) t->GetLayer(), (int) std::floor( p.x / (double) BUCKET ),
                                                   (int) std::floor( p.y / (double) BUCKET ) } );

            if( b == trackBucket.end() )
                continue;

            for( int j : b->second )
            {
                const PCB_TRACK* h = tracks[j];

                if( j == (int) k || h->GetLength() <= 0 )
                    continue;

                const SEG seg( h->GetStart(), h->GetEnd() );

                if( seg.Distance( p ) > h->GetWidth() / 2 )
                    continue;

                const VECTOR2D d = VECTOR2D( seg.B - seg.A );
                const double   along = VECTOR2D( p - seg.A ).Dot( d ) / d.SquaredEuclideanNorm();

                if( along * h->GetLength() <= h->GetWidth() / 2.0 )
                    dsu.join( node, startNode[j] );
                else if( ( 1 - along ) * h->GetLength() <= h->GetWidth() / 2.0 )
                    dsu.join( node, endNodeOf[j] );
                else
                    splits[j].emplace_back( along, node );
            }
        }
    }

    for( size_t k = 0; k < tracks.size(); k++ )
    {
        const PCB_TRACK*   t = tracks[k];
        const PCB_LAYER_ID l = t->GetLayer();
        const double       g1 = sigma * thick( l ) * t->GetWidth();   // S per unit length (nm)
        std::vector<std::pair<double, int>>& cuts = splits[k];
        cuts.emplace_back( 0.0, startNode[k] );
        cuts.emplace_back( 1.0, endNodeOf[k] );
        std::sort( cuts.begin(), cuts.end() );

        for( size_t m = 1; m < cuts.size(); m++ )
            edges.push_back( { cuts[m - 1].second, cuts[m].second,
                               g1 / std::max( ( cuts[m].first - cuts[m - 1].first ) * t->GetLength(), 1000.0 ), K_TRACK, (int) k } );
    }

    for( auto& [l, cells] : grid.cells )
    {
        const double g = sigma * thick( l );

        for( int j = 0; j < grid.ny; j++ )
        {
            for( int i = 0; i < grid.nx; i++ )
            {
                const int c = cells[(size_t) j * grid.nx + i];

                if( c < 0 )
                    continue;

                if( i + 1 < grid.nx && cells[(size_t) j * grid.nx + i + 1] >= 0 )
                    edges.push_back( { c, cells[(size_t) j * grid.nx + i + 1], g, K_PLANE, (int) l } );

                if( j + 1 < grid.ny && cells[(size_t) ( j + 1 ) * grid.nx + i] >= 0 )
                    edges.push_back( { c, cells[(size_t) ( j + 1 ) * grid.nx + i], g, K_PLANE, (int) l } );
            }
        }
    }

    // ---- the source: one node
    int src = -1;

    for( const PAD* p : r.source )
    {
        for( PCB_LAYER_ID l : cu )
        {
            auto it = itemNode.find( { p, l } );

            if( it == itemNode.end() )
                continue;

            if( src < 0 )
                src = it->second;
            else
                dsu.join( src, it->second );
        }
    }

    if( src < 0 )
    {
        r.problem = "the source has no copper";
        return;
    }

    src = dsu.find( src );

    // ---- the source's island
    const int        total = (int) dsu.p.size();
    std::vector<std::vector<std::pair<int, int>>> adj( total );   // node -> (node, edge)

    for( size_t k = 0; k < edges.size(); k++ )
    {
        EDGE& e = edges[k];
        e.a = dsu.find( e.a );
        e.b = dsu.find( e.b );

        if( e.a != e.b )
        {
            adj[e.a].emplace_back( e.b, (int) k );
            adj[e.b].emplace_back( e.a, (int) k );
        }
    }

    std::vector<int> idx( total, -2 );   // -2: not reached, -1: the source, else the unknown's index
    std::vector<int> stack = { src };
    int              n = 0;
    idx[src] = -1;

    while( !stack.empty() )
    {
        const int x = stack.back();
        stack.pop_back();

        for( const auto& [y, e] : adj[x] )
        {
            if( idx[y] == -2 )
            {
                idx[y] = n++;
                stack.push_back( y );
            }
        }
    }

    r.nodes = n + 1;

    // ---- loads
    auto loadNode = [&]( const PAD* p ) -> int
    {
        const FOOTPRINT* fp = p->GetParentFootprint();
        const PCB_LAYER_ID side = fp ? fp->GetLayer() : F_Cu;
        auto it = itemNode.find( { p, side } );

        if( it == itemNode.end() )
        {
            for( PCB_LAYER_ID l : cu )
            {
                if( ( it = itemNode.find( { p, l } ) ) != itemNode.end() )
                    break;
            }
        }

        return it == itemNode.end() ? -1 : dsu.find( it->second );
    };

    Eigen::VectorXd b = Eigen::VectorXd::Zero( n );
    std::vector<std::pair<const PAD*, int>> reached;

    for( const LOAD& ld : r.loads )
    {
        const int node = loadNode( ld.pad );

        if( node < 0 || idx[node] == -2 )
        {
            r.islands.push_back( ld.pad );
            continue;
        }

        reached.emplace_back( ld.pad, node );

        if( idx[node] >= 0 )
            b[idx[node]] += ld.amps;
    }

    if( reached.empty() )
    {
        r.problem = "no load has copper to the source";
        return;
    }

    std::vector<double> drop( total, 0.0 );

    if( n > 0 )
    {
        std::vector<Eigen::Triplet<double>> trip;
        trip.reserve( edges.size() * 4 );

        for( const EDGE& e : edges )
        {
            if( e.a == e.b || idx[e.a] == -2 )
                continue;

            const int ia = idx[e.a], ib = idx[e.b];

            if( ia >= 0 )
                trip.emplace_back( ia, ia, e.g );

            if( ib >= 0 )
                trip.emplace_back( ib, ib, e.g );

            if( ia >= 0 && ib >= 0 )
            {
                trip.emplace_back( ia, ib, -e.g );
                trip.emplace_back( ib, ia, -e.g );
            }
        }

        Eigen::SparseMatrix<double> G( n, n );
        G.setFromTriplets( trip.begin(), trip.end() );
        Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt( G );

        if( ldlt.info() != Eigen::Success )
        {
            r.problem = "the network could not be solved";
            return;
        }

        const Eigen::VectorXd d = ldlt.solve( b );

        for( int x = 0; x < total; x++ )
        {
            if( idx[x] >= 0 )
                drop[x] = d[idx[x]];
        }
    }

    for( const auto& [p, node] : reached )
    {
        if( drop[node] > r.worst || !r.worstPad )
        {
            r.worst = drop[node];
            r.worstPad = p;
        }

        r.least = std::min( r.least, drop[node] );
    }

    // ---- where the current goes
    std::vector<double> barrelAmps( barrels.size(), 0.0 );

    for( const EDGE& e : edges )
    {
        if( e.a == e.b || idx[e.a] == -2 )
            continue;

        const double dv = drop[e.a] - drop[e.b];
        const double amps = std::fabs( e.g * dv );
        r.loss[e.kind] += e.g * dv * dv;

        if( e.kind == K_BARREL )
        {
            barrelAmps[e.ref] = std::max( barrelAmps[e.ref], amps );
        }
        else if( e.kind == K_TRACK )
        {
            const PCB_TRACK* t = tracks[e.ref];

            if( !r.hotTrack || amps / t->GetWidth() > r.hotAmps / r.hotTrack->GetWidth() )
            {
                r.hotTrack = t;
                r.hotAmps = amps;
            }
        }
    }

    for( size_t k = 0; k < barrels.size(); k++ )
    {
        if( barrels[k].via && barrelAmps[k] > 0 )
            r.vias.push_back( { barrels[k].pos, barrelAmps[k], barrels[k].drill } );
    }

    std::sort( r.vias.begin(), r.vias.end(), []( const VIAI& a, const VIAI& c ) { return a.amps > c.amps; } );
}

/// Pads of a rail from "U1", "U1.5" or "U1.5,U1.6" ("" with the token that names none).
std::vector<const PAD*> padsFrom( BOARD& aBoard, int aCode, const std::string& aSpec, std::string& aBad )
{
    std::vector<const PAD*> out;
    size_t                  pos = 0;

    while( pos <= aSpec.size() )
    {
        size_t      comma = aSpec.find( ',', pos );
        std::string tok = aSpec.substr( pos, comma == std::string::npos ? std::string::npos : comma - pos );
        pos = comma == std::string::npos ? aSpec.size() + 1 : comma + 1;
        tok.erase( 0, tok.find_first_not_of( " " ) );
        tok.erase( tok.find_last_not_of( " " ) + 1 );

        if( tok.empty() )
            continue;

        const size_t      dot = tok.find( '.' );
        const std::string ref = tok.substr( 0, dot );
        const std::string num = dot == std::string::npos ? "" : tok.substr( dot + 1 );
        bool              any = false;

        for( FOOTPRINT* fp : aBoard.Footprints() )
        {
            if( str( fp->GetReference() ) != ref )
                continue;

            for( PAD* p : fp->Pads() )
            {
                if( p->GetNetCode() == aCode && ( num.empty() || str( p->GetNumber() ) == num ) )
                {
                    out.push_back( p );
                    any = true;
                }
            }
        }

        if( !any && aBad.empty() )
            aBad = tok;
    }

    return out;
}

/// "U5 (6 pads), U7, ... +3 more" from pads grouped by part, in order.
std::string partsSaid( const std::vector<std::pair<std::string, int>>& aParts )
{
    std::string s;

    for( size_t k = 0; k < aParts.size() && k < 4; k++ )
        s += ( k ? ", " : "" ) + aParts[k].first + ( aParts[k].second > 1 ? " (" + std::to_string( aParts[k].second ) + " pads)" : "" );

    if( aParts.size() > 4 )
        s += " +" + std::to_string( aParts.size() - 4 ) + " more";

    return s;
}

/// The source of a rail when not given: a power_out / output pin of an IC, else an inductor's
/// pad (a buck's output), else a connector's. All pads of the first such part on the rail.
std::vector<const PAD*> guessSource( const std::vector<const PAD*>& aPads, bool aGround, std::string& aWhy )
{
    static const std::regex out( R"(^(V_?OUT\d*|OUT\d*|VO\d*|VREG\d*|V?LDO_?OUT\d*|VCC_?OUT|VDD_?OUT)$)", std::regex::icase );
    static const std::regex word( R"(.*(^|[^a-z])out(put)?([^a-z].*|$))", std::regex::icase );   // "+3.3v_(Output)"

    auto firstPart = [&]( auto&& aTest ) -> std::vector<const PAD*>
    {
        std::string best;

        for( const PAD* p : aPads )
        {
            if( aTest( p ) && ( best.empty() || refOf( p ) < best ) )
                best = refOf( p );
        }

        std::vector<const PAD*> got;

        for( const PAD* p : aPads )
        {
            if( !best.empty() && refOf( p ) == best && aTest( p ) )
                got.push_back( p );
        }

        return got;
    };

    std::vector<const PAD*> got = firstPart(
            [&]( const PAD* p )
            {
                if( aGround )   // a ground's current comes back to the connector (GND pins typed power_out are no source)
                    return false;

                const std::string pre = prefix( refOf( p ) );
                const std::string fn = str( p->GetPinFunction() );
                return pre != "C" && pre != "R" && pre != "TP" && pre != "D"
                       && ( str( p->GetPinType() ) == "power_out" || std::regex_match( fn, out ) || std::regex_match( fn, word ) );
            } );

    if( !got.empty() )
    {
        aWhy = "an output pin";
        return got;
    }

    if( !( got = firstPart( [&]( const PAD* p ) { return !aGround && prefix( refOf( p ) ) == "L"; } ) ).empty() )
    {
        aWhy = "an inductor";
        return got;
    }

    if( !( got = firstPart( []( const PAD* p ) { const std::string pre = prefix( refOf( p ) );
                                                 return pre == "J" || pre == "P" || pre == "CN" || pre == "X"; } ) ).empty() )
    {
        aWhy = "a connector";
        return got;
    }

    if( !( got = firstPart( []( const PAD* p ) { const std::string pre = prefix( refOf( p ) );
                                                 return pre == "MODULE" || pre == "MOD" || pre == "M" || pre == "A"; } ) ).empty() )
    {
        aWhy = "a module";
        return got;
    }

    return got;
}

std::string sourceSaid( const std::vector<const PAD*>& aPads )
{
    std::string s;

    for( size_t k = 0; k < aPads.size() && k < 3; k++ )
    {
        const std::string fn = str( aPads[k]->GetPinFunction() );
        const bool        plain = fn.empty() || k || wxString( fn ).IsSameAs( aPads[k]->GetNumber(), false )
                           || fn.rfind( "Pin_", 0 ) == 0;
        s += ( k ? ", " : "" ) + padName( aPads[k] ) + ( plain ? "" : " (" + fn + ")" );
    }

    if( aPads.size() > 3 )
        s += " +" + std::to_string( aPads.size() - 3 ) + " more";

    return s;
}

/// mV (or mV per A) of a rail's worst load.
double mv( const RAIL& r )
{
    return r.worst * 1000;
}

std::string amps( const RAIL& r )
{
    return r.perAmp ? "per A" : "at " + sig( r.amps ) + " A";
}

/// One rail, for a row.
std::string row( const RAIL& r, double aRise, const std::map<PCB_LAYER_ID, double>& aCopperM, bool aName = true )
{
    std::string s = ( aName ? r.name + ": " : std::string() ) + r.sourceSaid + ( r.guessed.empty() ? "" : " (guessed: " + r.guessed + ")" ) + " -> "
                    + r.loadsSaid + ", " + ( r.perAmp ? "per A (no current given)" : sig( r.amps ) + " A" );

    if( !r.problem.empty() )
        return s + "; not solved: " + r.problem;

    s += "; drop " + ( r.least < r.worst - 1e-9 ? sig( r.least * 1000 ) + "-" : "" ) + sig( mv( r ) ) + " mV, worst "
         + padName( r.worstPad ) + " " + at( r.worstPad->GetPosition() );

    const double lossSum = r.loss[0] + r.loss[1] + r.loss[2];

    if( lossSum > 0 )
    {
        static const char* names[3] = { "tracks", "planes", "vias" };
        std::string        parts;

        for( int k : { 0, 1, 2 } )
        {
            const int pct = (int) std::lround( 100 * r.loss[k] / lossSum );

            if( pct > 0 )
                parts += ( parts.empty() ? "" : ", " ) + std::string( names[k] ) + " " + std::to_string( pct ) + "%";
        }

        s += "; loss in " + parts;
    }

    if( !r.vias.empty() )
        s += "; busiest via " + sig( r.vias[0].amps ) + " A at " + at( r.vias[0].pos ) + " (" + mm( r.vias[0].drill / 1e6 )
             + " mm drill" + ( r.vias.size() > 1 ? ", of " + std::to_string( r.vias.size() ) : "" ) + ")";

    if( r.hotTrack && r.hotAmps > 0.01 * r.amps )
    {
        const PCB_LAYER_ID l = r.hotTrack->GetLayer();
        const bool         outer = IsExternalCopperLayer( l );
        auto               it = aCopperM.find( l );
        const double       um = ( it != aCopperM.end() ? it->second : outer ? 35e-6 : 17.5e-6 ) * 1e6;
        s += "; most loaded track " + mm( r.hotTrack->GetWidth() / 1e6 ) + " mm " + str( LSET::Name( l ) ) + " at "
             + at( ( r.hotTrack->GetStart() + r.hotTrack->GetEnd() ) / 2 ) + ": " + sig( r.hotAmps ) + " A";

        if( !r.perAmp )
            s += " (IPC-2221 " + mm( aRise, 0 ) + " C: " + mm( ipc2221::TrackWidthMm( r.hotAmps, aRise, um, outer ) ) + " mm)";
    }

    if( !r.islands.empty() )
    {
        s += "; no copper to the source: " + padName( r.islands[0] );

        if( r.islands.size() > 1 )
            s += " +" + std::to_string( r.islands.size() - 1 ) + " more";
    }

    if( r.hMm > 0 )
    {
        s += "; mesh " + mm( r.hMm, 3 ) + " mm";

        if( !r.meshWhy.empty() )
            s += " (" + r.meshWhy + ")";
        else if( r.hMm * 1e6 > COARSE )
            s += " (coarse: antipads and necks under about " + mm( 2 * r.hMm, 1 )
                 + " mm blur, a few % off, up to 15% at 0.35 mm; ir_mesh_mm 0.1 for a finer one)";
    }

    if( r.unfilled )
        s += "; " + std::to_string( r.unfilled ) + " zone(s) not filled, left out: refill_zones first";

    return s;
}

} // namespace


void CheckIrDrop( BOARD& aBoard, const BoardAudit& aReq, BoardAuditResponse& aOut )
{
    const double rise = aReq.temp_rise() > 0 ? aReq.temp_rise() : 10.0;

    std::map<PCB_LAYER_ID, double> copperM;

    for( BOARD_STACKUP_ITEM* item : aBoard.GetDesignSettings().GetStackupDescriptor().GetList() )
    {
        if( item->GetType() == BS_ITEM_TYPE_COPPER && item->GetThickness() > 0 )
            copperM[item->GetBrdLayerId()] = item->GetThickness() * 1e-9;
    }

    // ---- the agent's globs (string values: their index)
    GLOBS                    picked, srcGlobs, loadGlobs, dropLimit, viaLimit;
    std::vector<std::string> srcSpecs, loadSpecs;

    for( const std::string& g : aReq.ir_nets() )
        picked.add( g, 0 );

    for( const auto& [g, spec] : aReq.ir_source() )
    {
        srcGlobs.add( g, (double) srcSpecs.size() );
        srcSpecs.push_back( spec );
    }

    for( const auto& [g, spec] : aReq.ir_loads() )
    {
        loadGlobs.add( g, (double) loadSpecs.size() );
        loadSpecs.push_back( spec );
    }

    for( const auto& [g, v] : aReq.ir_drop() )
        dropLimit.add( g, v );

    for( const auto& [g, v] : aReq.via_current() )
        viaLimit.add( g, v );

    // ---- the rails: ir_nets, else supply nets (not ground) with pads on two parts or more
    std::map<int, std::vector<const PAD*>> netPads;

    for( FOOTPRINT* fp : aBoard.Footprints() )
    {
        for( PAD* p : fp->Pads() )
        {
            if( p->GetNetCode() > 0 )
                netPads[p->GetNetCode()].push_back( p );
        }
    }

    std::vector<RAIL>     rails;
    std::set<std::string> used;

    for( NETINFO_ITEM* net : aBoard.GetNetInfo() )
    {
        const int code = net->GetNetCode();

        if( code <= 0 )
            continue;

        const std::string name = str( net->GetNetname() );

        if( !picked.empty() )
        {
            bool hit = false;

            for( const GLOBS::GLOB& g : picked.list )
            {
                if( GLOBS::matches( g, name ) )
                {
                    used.insert( g.pattern );
                    hit = true;
                }
            }

            if( !hit )
                continue;
        }
        else if( !railName( name ) || groundName( name ) || controlName( name ) )
        {
            continue;
        }

        std::set<std::string> parts;

        for( const PAD* p : netPads[code] )
            parts.insert( refOf( p ) );

        if( parts.size() < 2 )
            continue;

        RAIL r;
        r.code = code;
        r.name = name;
        rails.push_back( r );
    }

    for( const GLOBS::GLOB& g : picked.list )
    {
        if( !used.count( g.pattern ) )
            line( aOut, "warning", "no rail matches " + g.pattern + " (a net with pads on two parts or more)" );
    }

    if( rails.empty() )
        return;

    // ---- source, loads, current
    for( RAIL& r : rails )
    {
        const std::vector<const PAD*>& pads = netPads[r.code];
        std::string                    bad;

        if( const GLOBS::GLOB* g = srcGlobs.match( r.name ) )
        {
            r.source = padsFrom( aBoard, r.code, srcSpecs[(size_t) g->value], bad );

            if( !bad.empty() )
                r.problem = "ir_source " + bad + ": no pad on " + r.name;
        }
        else
        {
            r.source = guessSource( pads, groundName( r.name ), r.guessed );
        }

        if( r.source.empty() )
        {
            r.sourceSaid = "?";

            if( r.problem.empty() )
            {
                r.problem = "no source found (no output pin, inductor, connector or module): give ir_source {\"" + r.name
                            + "\": \"U1.5\"}";
                r.noSource = true;
            }
        }
        else
        {
            r.sourceSaid = sourceSaid( r.source );
        }

        std::set<const PAD*> isSource( r.source.begin(), r.source.end() );

        std::optional<double> given;

        for( const auto& [wanted, a] : aReq.currents() )
        {
            if( a > 0 && sameNet( r.name, wanted ) )
                given = a;
        }

        // the parts drawing current, in order, with their pads and amps (nullopt: a share)
        struct PART
        {
            std::string              name;
            std::vector<const PAD*>  pads;
            std::optional<double>    amps;
        };

        std::vector<PART> parts;

        auto addPart = [&]( const std::string& aName, const std::vector<const PAD*>& aPads, std::optional<double> aAmps )
        {
            std::vector<const PAD*> keep;

            for( const PAD* p : aPads )
            {
                if( !isSource.count( p ) )
                    keep.push_back( p );
            }

            if( !keep.empty() )
                parts.push_back( { aName, keep, aAmps } );
        };

        if( const GLOBS::GLOB* g = loadGlobs.match( r.name ) )
        {
            const std::string& spec = loadSpecs[(size_t) g->value];
            size_t             pos = 0;

            while( pos <= spec.size() && r.problem.empty() )
            {
                const size_t comma = spec.find( ',', pos );
                std::string  tok = spec.substr( pos, comma == std::string::npos ? std::string::npos : comma - pos );
                pos = comma == std::string::npos ? spec.size() + 1 : comma + 1;
                tok.erase( 0, tok.find_first_not_of( " " ) );
                tok.erase( tok.find_last_not_of( " " ) + 1 );

                if( tok.empty() )
                    continue;

                std::optional<double> a;
                const size_t          colon = tok.find( ':' );

                if( colon != std::string::npos )
                {
                    char*        end = nullptr;
                    const double v = std::strtod( tok.c_str() + colon + 1, &end );

                    if( end == tok.c_str() + colon + 1 || v < 0 )
                    {
                        r.problem = "ir_loads " + tok + ": not \"REF:A\"";
                        break;
                    }

                    a = v;
                    tok = tok.substr( 0, colon );
                }

                std::string                   nobad;
                const std::vector<const PAD*> got = padsFrom( aBoard, r.code, tok, nobad );

                if( got.empty() )
                    r.problem = "ir_loads " + tok + ": no pad on " + r.name;
                else
                    addPart( tok, got, a );
            }
        }
        else
        {
            std::map<std::string, std::vector<const PAD*>> byRef;

            for( const PAD* p : pads )
                byRef[refOf( p )].push_back( p );

            const std::string srcRef = r.source.empty() ? "" : refOf( r.source[0] );

            for( int tier : { 0, 1 } )
            {
                for( const auto& [ref, list] : byRef )
                {
                    const std::string pre = prefix( ref );
                    const bool        ic = pre == "U" || pre == "IC";

                    if( ref == srcRef || ( tier == 0 && !ic )
                        || ( tier == 1 && ( pre == "C" || pre == "TP" || pre == "FB" || pre == "L" ) ) )
                    {
                        continue;
                    }

                    addPart( ref, list, std::nullopt );
                }

                if( !parts.empty() )
                    break;
            }
        }

        if( parts.empty() && r.problem.empty() )
            r.problem = "no load (no other IC or part on it): give ir_loads {\"" + r.name + "\": \"U5:0.5,U7\"}";

        // parts with amps get them; the rest share what is left of the rail's current per pad
        // (a big consumer has many supply pins, a sense input one)
        double fixed = 0;
        size_t sharePads = 0;

        for( const PART& p : parts )
        {
            if( p.amps )
                fixed += *p.amps;
            else
                sharePads += p.pads.size();
        }

        r.perAmp = !given && fixed == 0;
        r.amps = given ? *given : fixed > 0 ? fixed : 1.0;
        const double perPad = sharePads ? std::max( 0.0, r.amps - fixed ) / sharePads : 0;

        if( given && fixed > *given + 1e-9 )
            r.amps = fixed;

        std::vector<std::pair<std::string, int>> said;

        for( const PART& p : parts )
        {
            for( const PAD* pad : p.pads )
                r.loads.push_back( { pad, p.amps ? *p.amps / p.pads.size() : perPad } );

            said.emplace_back( p.name + ( p.amps ? ":" + sig( *p.amps ) + "A" : "" ), (int) p.pads.size() );
        }

        r.loadsSaid = parts.empty() ? "?" : partsSaid( said );

        if( r.problem.empty() )
            solve( aBoard, r, copperM, aReq.ir_mesh_mm() );
    }

    aOut.set_ir_rails( (int) rails.size() );

    // ---- the header
    const bool   judging = !dropLimit.empty() || !viaLimit.empty();
    const bool   rows = aReq.ir_limit() > 0 || aReq.ir_nets_size() > 0;
    const size_t limit = aReq.ir_limit() > 0 ? aReq.ir_limit() : 10;

    std::vector<const RAIL*> solved, unsolved, sourceless;   // sourceless: supply-named nets by default only

    for( const RAIL& r : rails )
        ( r.problem.empty() ? solved : r.noSource && picked.empty() ? sourceless : unsolved ).push_back( &r );

    std::sort( solved.begin(), solved.end(), []( const RAIL* a, const RAIL* b ) { return mv( *a ) > mv( *b ); } );

    int guessed = 0, given = 0;

    for( const RAIL& r : rails )
    {
        guessed += !r.guessed.empty();
        given += !r.perAmp;
    }

    std::string head = std::to_string( solved.size() ) + " rail(s) solved"
                       + ( unsolved.empty() ? "" : ", " + std::to_string( unsolved.size() ) + " not" );

    if( !solved.empty() )
    {
        const RAIL& w = *solved[0];
        head += ": worst drop " + sig( mv( w ) ) + " mV " + amps( w ) + " on " + w.name + " at " + padName( w.worstPad )
                + " (from " + w.sourceSaid + ")";
    }

    head += ". Sources guessed for " + std::to_string( guessed ) + " (an output pin, else an inductor, a connector, a "
            "module; ir_source sets them), loads: the rail's ICs, the current split per pad (ir_loads sets them); currents "
            "given for " + std::to_string( given ) + " of " + std::to_string( solved.size() + unsolved.size() )
            + " (else drops are per A: currents {net: A})";

    int unfilled = 0;

    for( const RAIL& r : rails )
        unfilled += r.unfilled > 0;

    if( unfilled )
        head += ". " + std::to_string( unfilled ) + " rail(s) have zones with no fill (left out of the copper): refill_zones first";

    if( !sourceless.empty() )
    {
        head += ". " + std::to_string( sourceless.size() ) + " supply-named net(s) with no source part, left out: ";

        // the big ones first: the main supplies an agent should give a source to
        std::stable_sort( sourceless.begin(), sourceless.end(),
                          [&]( const RAIL* a, const RAIL* b ) { return netPads[a->code].size() > netPads[b->code].size(); } );

        for( size_t k = 0; k < sourceless.size() && k < 5; k++ )
            head += ( k ? ", " : "" ) + sourceless[k]->name + " (" + std::to_string( netPads[sourceless[k]->code].size() ) + " pads)";

        head += sourceless.size() > 5 ? " +" + std::to_string( sourceless.size() - 5 ) + " more" : "";
        head += " (ir_source to include)";
    }

    if( !judging )
    {
        head += ". No verdicts: judge them, or pass ir_drop {glob: mV} / via_current {glob: A}";

        if( !rows && !solved.empty() )
            head += ". The worst: " + row( *solved[0], rise, copperM ) + ". Per rail: ir_limit / ir_nets";
    }

    line( aOut, "info", head );

    if( !judging && rows )
    {
        size_t shown = 0;

        for( const RAIL* r : solved )
        {
            if( shown++ < limit )
                line( aOut, "info", row( *r, rise, copperM ), r->worstPad ? std::optional<VECTOR2I>( r->worstPad->GetPosition() ) : std::nullopt );
        }

        for( const RAIL* r : unsolved )
        {
            if( shown++ < limit )
                line( aOut, "info", row( *r, rise, copperM ) );
        }

        if( shown > limit )
            line( aOut, "info", std::to_string( shown - limit ) + " more rail(s) (ir_limit)" );
    }

    if( !judging )
        return;

    for( const RAIL* r : unsolved )
        line( aOut, "warning", row( *r, rise, copperM ) );

    // ---- the agent's limits, rule by rule (the tightest first)
    struct JUDGED
    {
        const RAIL* rail;
        double      over;   // how far over (mV, or A for the busiest via)
        int         count;  // vias over
    };

    auto judge = [&]( const GLOBS& aGlobs, const char* aWhat, const char* aUnit, auto&& aOver )
    {
        std::vector<const GLOBS::GLOB*> order;

        for( const GLOBS::GLOB& g : aGlobs.list )
            order.push_back( &g );

        std::sort( order.begin(), order.end(), []( auto* a, auto* b ) { return a->value < b->value; } );

        for( const GLOBS::GLOB* g : order )
        {
            std::vector<JUDGED> over;
            int                      judged = 0;
            std::vector<std::string> noCurrent;

            for( const RAIL* r : solved )
            {
                if( aGlobs.match( r->name ) != g )
                    continue;

                if( r->perAmp )
                {
                    noCurrent.push_back( r->name );
                    continue;
                }

                judged++;
                JUDGED j{ r, 0, 0 };

                if( aOver( *r, g->value, j ) )
                    over.push_back( j );
            }

            if( judged == 0 && noCurrent.empty() )
                continue;

            std::sort( over.begin(), over.end(), []( const JUDGED& a, const JUDGED& b ) { return a.over > b.over; } );
            std::string tally = std::string( aWhat ) + " " + g->pattern + " (max " + sig( g->value ) + " " + aUnit + "): "
                               + std::to_string( judged ) + " rail(s) judged, "
                               + ( over.empty() ? "all within" : std::to_string( over.size() ) + " over" );

            if( !noCurrent.empty() )
            {
                tally += "; " + std::to_string( noCurrent.size() ) + " not judged, no current given (";

                for( size_t k = 0; k < noCurrent.size() && k < 3; k++ )
                    tally += ( k ? ", " : "" ) + noCurrent[k];

                tally += std::string( noCurrent.size() > 3 ? " +" + std::to_string( noCurrent.size() - 3 ) + " more" : "" )
                         + "): currents {net: A}";
            }

            line( aOut, over.empty() ? "info" : "warning", tally );

            for( size_t k = 0; k < over.size() && k < limit; k++ )
            {
                const RAIL& r = *over[k].rail;
                std::string msg;

                if( over[k].count == 0 )
                {
                    msg = r.name + ": " + sig( mv( r ) ) + " mV at " + padName( r.worstPad ) + " " + at( r.worstPad->GetPosition() ) + ", " + sig( over[k].over )
                          + " over; " + row( r, rise, copperM, false );
                }
                else
                {
                    msg = r.name + ": " + std::to_string( over[k].count ) + " via(s) over " + sig( g->value ) + " A (" + amps( r )
                          + "):";

                    for( size_t v = 0; v < (size_t) over[k].count && v < 3; v++ )
                        msg += ( v ? "," : "" ) + std::string( " " ) + sig( r.vias[v].amps ) + " A at " + at( r.vias[v].pos ) + " ("
                               + mm( r.vias[v].drill / 1e6 ) + " mm drill)";

                    msg += " of " + std::to_string( r.vias.size() ) + " carrying current; add vias beside them";
                }

                line( aOut, "warning", msg,
                      over[k].count ? std::optional<VECTOR2I>( r.vias[0].pos )
                                    : std::optional<VECTOR2I>( r.worstPad->GetPosition() ) );
            }
        }
    };

    judge( dropLimit, "ir_drop", "mV",
           []( const RAIL& r, double aMax, JUDGED& j )
           {
               j.over = mv( r ) - aMax;
               return j.over > 0;
           } );

    judge( viaLimit, "via_current", "A",
           []( const RAIL& r, double aMax, JUDGED& j )
           {
               for( const VIAI& v : r.vias )
                   j.count += v.amps > aMax;

               j.over = r.vias.empty() ? 0 : r.vias[0].amps - aMax;
               return j.count > 0;
           } );
}

} // namespace kicadd

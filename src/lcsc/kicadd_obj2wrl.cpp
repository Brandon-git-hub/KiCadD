/*
 * KiCadD: an EasyEDA OBJ as VRML 2.0 (see kicadd_obj2wrl.h).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "lcsc/kicadd_obj2wrl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <vector>

namespace kicadd::obj2wrl
{

namespace
{

struct MATERIAL
{
    double kd[3] = { 0.7, 0.7, 0.7 };
    double ks[3] = { 0.2, 0.2, 0.2 };
    double transparency = 0;
};


struct GROUP
{
    std::string      material;
    std::vector<int> faces;   ///< vertex indices (0-based), -1 after each face
};


std::string num( double aV )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.5g", std::fabs( aV ) < 5e-7 ? 0.0 : aV );
    return buf;
}


// the vertex index of an OBJ face corner: "12", "12/3", "12//7", "-1"
int index( const std::string& aCorner, int aCount )
{
    const int i = std::atoi( aCorner.c_str() );
    return i > 0 ? i - 1 : i < 0 ? aCount + i : -1;
}

} // namespace


bool Convert( const std::string& aObj, const PLACE& aPlace, std::string& aWrl, INFO& aInfo, std::string& aError )
{
    std::vector<double>             v;      // x, y, z
    std::map<std::string, MATERIAL> materials;
    std::vector<GROUP>              groups( 1 );
    std::string                     defining;
    std::istringstream              in( aObj );
    std::string                     line;

    while( std::getline( in, line ) )
    {
        if( !line.empty() && line.back() == '\r' )
            line.pop_back();

        std::istringstream ls( line );
        std::string        key;
        ls >> key;

        if( key == "v" )
        {
            double x = 0, y = 0, z = 0;
            ls >> x >> y >> z;
            v.insert( v.end(), { x, y, z } );
        }
        else if( key == "f" )
        {
            std::string corner;
            std::vector<int> face;

            while( ls >> corner )
                face.push_back( index( corner, (int) v.size() / 3 ) );

            if( face.size() < 3 )
                continue;

            for( int i : face )
            {
                if( i < 0 || i >= (int) v.size() / 3 )
                {
                    aError = "the model has a face on a vertex it does not have";
                    return false;
                }
            }

            groups.back().faces.insert( groups.back().faces.end(), face.begin(), face.end() );
            groups.back().faces.push_back( -1 );
        }
        else if( key == "newmtl" )
        {
            ls >> defining;
            materials[defining];
        }
        else if( key == "Kd" || key == "Ks" )
        {
            double* c = key == "Kd" ? materials[defining].kd : materials[defining].ks;
            ls >> c[0] >> c[1] >> c[2];
        }
        else if( key == "d" )
        {
            // EasyEDA writes its transparency here (0: opaque), not OBJ's dissolve; a fully
            // see-through part is a mistake, keep something of it
            double d = 0;
            ls >> d;
            materials[defining].transparency = std::clamp( d, 0.0, 0.8 );
        }
        else if( key == "usemtl" )
        {
            GROUP g;
            ls >> g.material;
            groups.push_back( g );
        }
    }

    const int count = (int) v.size() / 3;

    if( count == 0 )
    {
        aError = "the model has no vertices";
        return false;
    }

    double lo[3], hi[3];

    for( int k = 0; k < 3; ++k )
    {
        lo[k] = std::numeric_limits<double>::max();
        hi[k] = std::numeric_limits<double>::lowest();
    }

    for( int i = 0; i < count; ++i )
    {
        for( int k = 0; k < 3; ++k )
        {
            lo[k] = std::min( lo[k], v[3 * i + k] );
            hi[k] = std::max( hi[k], v[3 * i + k] );
        }
    }

    // centred on x / y, bottom on 0, turned, moved to its place; y up; 2.54 mm units
    const double cx = ( lo[0] + hi[0] ) / 2, cy = ( lo[1] + hi[1] ) / 2;
    const double a = aPlace.rotation * 3.14159265358979323846 / 180.0, ca = std::cos( a ), sa = std::sin( a );
    std::vector<double> w( v.size() );

    for( int i = 0; i < count; ++i )
    {
        const double x = v[3 * i] - cx, y = v[3 * i + 1] - cy;
        w[3 * i] = ( x * ca - y * sa + aPlace.x ) / 2.54;
        w[3 * i + 1] = ( x * sa + y * ca - aPlace.y ) / 2.54;
        w[3 * i + 2] = ( v[3 * i + 2] - lo[2] + aPlace.z ) / 2.54;
    }

    std::string out = "#VRML V2.0 utf8\n"
                      "# EasyEDA 3D model, converted by KiCadD. Source: JLCEDA/EasyEDA Official Library\n";
    int faces = 0, used = 0;

    for( const GROUP& g : groups )
    {
        if( g.faces.empty() )
            continue;

        // this group's own vertices, renumbered
        std::map<int, int> local;
        std::string        coords, idx;

        for( int i : g.faces )
        {
            if( i < 0 )
            {
                idx += "-1,\n";
                faces++;
                continue;
            }

            auto [it, added] = local.emplace( i, (int) local.size() );

            if( added )
                coords += num( w[3 * i] ) + " " + num( w[3 * i + 1] ) + " " + num( w[3 * i + 2] ) + ",\n";

            idx += std::to_string( it->second ) + ",";
        }

        auto           mit = materials.find( g.material );
        const MATERIAL m = mit == materials.end() ? MATERIAL() : mit->second;
        used++;

        out += "Shape {\n appearance Appearance { material Material {\n  diffuseColor " + num( m.kd[0] ) + " "
               + num( m.kd[1] ) + " " + num( m.kd[2] ) + "\n  specularColor " + num( m.ks[0] ) + " " + num( m.ks[1] )
               + " " + num( m.ks[2] ) + "\n  ambientIntensity 0.3\n  shininess 0.4\n  transparency "
               + num( m.transparency ) + "\n } }\n geometry IndexedFaceSet {\n  creaseAngle 0.5\n  coord Coordinate { point [\n"
               + coords + "  ] }\n  coordIndex [\n" + idx + "  ]\n }\n}\n";
    }

    if( faces == 0 )
    {
        aError = "the model has no faces";
        return false;
    }

    aWrl = std::move( out );
    aInfo.vertices = count;
    aInfo.faces = faces;
    aInfo.materials = used;

    for( int k = 0; k < 3; ++k )
        aInfo.size[k] = hi[k] - lo[k];

    return true;
}

} // namespace kicadd::obj2wrl

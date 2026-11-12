/*
 * KiCadD: fabrication output. See kicadd_fab.h.
 *
 * Gerbers and drill files come from KiCad's own plotters (as `kicad-cli pcb export gerbers /
 * drill`), but run on the board in memory: the CLI jobs reload the file. BOM and CPL are
 * written here in the columns JLCPCB's upload asks for.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_fab.h"

#include <board.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <pcb_field.h>
#include <pcb_plot_params.h>
#include <pcb_plotter.h>
#include <pcbplot.h>
#include <plotters/plotter_gerber.h>
#include <gerber_jobfile_writer.h>
#include <gendrill_excellon_writer.h>
#include <wildcards_and_files_ext.h>

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

namespace kicadd
{

namespace
{

std::string str( const wxString& s ) { return s.ToStdString( wxConvUTF8 ); }

std::string csv( const std::string& aField )
{
    std::string s = "\"";

    for( char c : aField )
    {
        if( c == '"' )
            s += '"';

        s += c;
    }

    return s + "\"";
}

std::string mm( double aIU )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.4fmm", aIU / 1e6 );
    return buf;
}

/// "C25804" from the footprint's fields (sync_pcb copies the symbol's LCSC field)
std::string lcscOf( const FOOTPRINT* aFp )
{
    for( const char* name : { "LCSC", "LCSC Part #", "LCSC Part", "JLCPCB Part #", "JLC" } )
    {
        if( const PCB_FIELD* f = aFp->GetField( wxString::FromUTF8( name ) ) )
        {
            std::string v = str( f->GetText() );

            if( !v.empty() )
                return v;
        }
    }

    return {};
}

/// References in natural order: R2 before R10
bool refLess( const std::string& a, const std::string& b )
{
    auto split = []( const std::string& r )
    {
        size_t i = r.find_first_of( "0123456789" );
        std::string prefix = r.substr( 0, i );
        long        n = i == std::string::npos ? 0 : std::strtol( r.c_str() + i, nullptr, 10 );
        return std::make_tuple( prefix, n, r );
    };

    return split( a ) < split( b );
}

bool writeText( const wxString& aPath, const std::string& aText )
{
    wxFFileOutputStream out( aPath );
    return out.IsOk() && out.WriteAll( aText.data(), aText.size() ) && out.Close();
}

} // namespace


bool ExportFab( BOARD* aBoard, const wxString& aDir, bool aZip, kicadd::board::ExportFabResponse& aOut,
                std::string& aError )
{
    if( !wxFileName::DirExists( aDir ) && !wxFileName::Mkdir( aDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
    {
        aError = "cannot create " + str( aDir );
        return false;
    }

    aOut.set_dir( str( aDir ) );

    wxFileName base( aBoard->GetFileName() );

    if( base.GetName().IsEmpty() )
        base.SetName( wxS( "board" ) );

    std::vector<wxString> uploads;     // full paths, zipped

    // --- Gerbers: the copper layers, mask, silk, paste, outline
    PCB_PLOT_PARAMS opts;
    opts.SetFormat( PLOT_FORMAT::GERBER );
    opts.SetUseGerberProtelExtensions( true );
    opts.SetUseGerberX2format( true );
    opts.SetIncludeGerberNetlistInfo( true );
    opts.SetCreateGerberJobFile( true );
    opts.SetSubtractMaskFromSilk( true );
    opts.SetUseAuxOrigin( false );
    opts.SetDrillMarksType( DRILL_MARKS::NO_DRILL_SHAPE );

    LSET wanted = LSET::AllCuMask()
                  | LSET( { F_Mask, B_Mask, F_SilkS, B_SilkS, F_Paste, B_Paste, Edge_Cuts } );
    LSET layers = aBoard->GetEnabledLayers() & wanted;

    GERBER_JOBFILE_WRITER jobfile( aBoard );

    for( PCB_LAYER_ID layer : layers.SeqStackupForPlotting() )
    {
        wxFileName fn = base;
        wxString   layerName = aBoard->GetLayerName( layer );
        PCB_PLOTTER::BuildPlotFileName( &fn, aDir, layerName, GetGerberProtelExtension( layer ) );
        wxString name = fn.GetFullName();
        jobfile.AddGbrFile( layer, name );

        PLOTTER* plotter = StartPlotBoard( aBoard, &opts, layer, layerName, fn.GetFullPath(), wxEmptyString,
                                           wxEmptyString );

        if( !plotter )
        {
            aError = "cannot write " + str( fn.GetFullPath() );
            return false;
        }

        PlotBoardLayers( aBoard, plotter, LSEQ( { layer } ), opts );
        plotter->EndPlot();
        delete plotter;

        uploads.push_back( fn.GetFullPath() );
        aOut.add_files( str( name ) );
        aOut.set_layers( aOut.layers() + 1 );
    }

    {
        wxFileName fn = base;
        PCB_PLOTTER::BuildPlotFileName( &fn, aDir, wxS( "job" ), FILEEXT::GerberJobFileExtension );

        if( jobfile.CreateJobFile( fn.GetFullPath() ) )
        {
            uploads.push_back( fn.GetFullPath() );
            aOut.add_files( str( fn.GetFullName() ) );
        }
    }

    // --- Drill: Excellon, mm, decimal, PTH and NPTH apart, absolute (as the Gerbers)
    {
        EXCELLON_WRITER drill( aBoard );
        drill.SetFormat( true, EXCELLON_WRITER::DECIMAL_FORMAT, 3, 3 );
        drill.SetOptions( false, false, VECTOR2I( 0, 0 ), false );
        drill.SetRouteModeForOvalHoles( false );

        if( !drill.CreateDrillandMapFilesSet( aDir, true, false, nullptr ) )
        {
            aError = "cannot write the drill files in " + str( aDir );
            return false;
        }

        for( const wxString& f : drill.GetCreatedFiles() )
        {
            uploads.push_back( f );
            aOut.add_files( str( wxFileName( f ).GetFullName() ) );
        }
    }

    // --- BOM and CPL
    struct LINE
    {
        std::vector<std::string> refs;
        std::string              value, footprint, lcsc;
    };

    std::map<std::tuple<std::string, std::string, std::string>, LINE> bom;
    std::vector<std::pair<std::string, std::string>>                   cpl;   // ref, row
    int                                                                tht = 0, noBom = 0;

    for( FOOTPRINT* fp : aBoard->Footprints() )
    {
        std::string ref = str( fp->GetReference() );

        if( ref.empty() || ref[0] == '#' || fp->IsDNP() || fp->IsBoardOnly() )
            continue;

        std::string value = str( fp->GetValue() );
        std::string footprint = str( fp->GetFPID().GetLibItemName().wx_str() );
        std::string lcsc = lcscOf( fp );

        if( !fp->IsExcludedFromBOM() )
        {
            LINE& line = bom[{ value, footprint, lcsc }];
            line.refs.push_back( ref );
            line.value = value;
            line.footprint = footprint;
            line.lcsc = lcsc;
        }
        else
        {
            // not bought, not placed: a CPL row JLCPCB has no BOM line for is an error there
            noBom++;
            continue;
        }

        if( fp->IsExcludedFromPosFiles() )
            continue;

        if( fp->GetAttributes() & FP_THROUGH_HOLE )
            tht++;

        // the Gerbers' frame: absolute, Y up
        const VECTOR2I pos = fp->GetPosition();
        double         rot = fp->GetOrientation().Normalize().AsDegrees();
        char           r[16];
        std::snprintf( r, sizeof( r ), "%g", std::round( rot * 100 ) / 100 );

        cpl.emplace_back( ref, csv( ref ) + "," + csv( mm( pos.x ) ) + "," + csv( mm( -pos.y ) ) + ","
                                       + csv( fp->IsFlipped() ? "Bottom" : "Top" ) + "," + csv( r ) );
    }

    std::vector<LINE> lines;

    for( auto& [key, line] : bom )
    {
        std::sort( line.refs.begin(), line.refs.end(), refLess );
        lines.push_back( line );
    }

    std::sort( lines.begin(), lines.end(), []( const LINE& a, const LINE& b ) { return refLess( a.refs[0], b.refs[0] ); } );

    std::string bomText = "Comment,Designator,Footprint,LCSC Part #\n";
    int         parts = 0;

    for( const LINE& line : lines )
    {
        std::string refs;

        for( const std::string& ref : line.refs )
            refs += ( refs.empty() ? "" : "," ) + ref;

        bomText += csv( line.value ) + "," + csv( refs ) + "," + csv( line.footprint ) + "," + csv( line.lcsc ) + "\n";
        parts += (int) line.refs.size();

        if( line.lcsc.empty() )
        {
            std::string shortRefs;

            for( size_t i = 0; i < line.refs.size() && i < 6; ++i )
                shortRefs += ( i ? " " : "" ) + line.refs[i];

            if( line.refs.size() > 6 )
                shortRefs += " ...";

            aOut.add_no_lcsc( shortRefs + " (" + line.value + ", " + line.footprint + ")" );
        }
    }

    std::sort( cpl.begin(), cpl.end(), []( const auto& a, const auto& b ) { return refLess( a.first, b.first ); } );
    std::string cplText = "Designator,Mid X,Mid Y,Layer,Rotation\n";

    for( const auto& [ref, row] : cpl )
        cplText += row + "\n";

    wxFileName bomFile = base, cplFile = base;
    bomFile.SetPath( aDir );
    bomFile.SetName( base.GetName() + wxS( "-bom" ) );
    bomFile.SetExt( wxS( "csv" ) );
    cplFile.SetPath( aDir );
    cplFile.SetName( base.GetName() + wxS( "-cpl" ) );
    cplFile.SetExt( wxS( "csv" ) );

    if( !writeText( bomFile.GetFullPath(), bomText ) || !writeText( cplFile.GetFullPath(), cplText ) )
    {
        aError = "cannot write the BOM / CPL in " + str( aDir );
        return false;
    }

    aOut.set_bom( str( bomFile.GetFullName() ) );
    aOut.set_cpl( str( cplFile.GetFullName() ) );
    aOut.set_bom_lines( (int) lines.size() );
    aOut.set_bom_parts( parts );
    aOut.set_placed( (int) cpl.size() );

    if( tht )
        aOut.add_notes( std::to_string( tht ) + " through-hole part(s) in the CPL: JLCPCB places them only with "
                        "through-hole assembly; remove their rows for SMT only" );

    if( noBom )
        aOut.add_notes( std::to_string( noBom ) + " part(s) marked 'exclude from BOM' left out of the BOM and the CPL" );

    aOut.add_notes( "CPL: footprint origins and KiCad rotations; JLCPCB's preview may turn some packages "
                    "(check it before ordering)" );

    // --- the upload: Gerbers + job file + drill in one zip
    if( aZip )
    {
        wxFileName zipFile = base;
        zipFile.SetPath( aDir );
        zipFile.SetName( base.GetName() + wxS( "-gerbers" ) );
        zipFile.SetExt( wxS( "zip" ) );

        wxFFileOutputStream out( zipFile.GetFullPath() );
        wxZipOutputStream   zip( out );

        for( const wxString& f : uploads )
        {
            wxFFileInputStream in( f );

            if( !in.IsOk() || !zip.PutNextEntry( wxFileName( f ).GetFullName() ) )
            {
                aError = "cannot zip " + str( f );
                return false;
            }

            zip.Write( in );
        }

        if( !zip.Close() || !out.Close() )
        {
            aError = "cannot write " + str( zipFile.GetFullPath() );
            return false;
        }

        aOut.set_zip( str( zipFile.GetFullName() ) );
    }

    return true;
}

} // namespace kicadd

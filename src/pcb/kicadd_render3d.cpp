/*
 * KiCadD roadmap phase 5 item 1c: a 3D picture of the board in memory (board_render_3d).
 *
 * KiCad's own raytracer (RENDER_3D_RAYTRACE_RAM, the renderer of `kicad-cli pcb render`): no
 * OpenGL, no window. Set up as PCBNEW_JOBS_HANDLER::JobExportRender does, on the board in memory
 * (unsaved edits included), so nothing is written. Without OpenCASCADE only VRML models load:
 * the parts without one (none set, the file not found, a STEP / IGES file) are listed.
 *
 * It runs on KiCad's main thread: in the desktop editor the window waits for it (seconds; the
 * size and quality set how many).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "kicadd_handler_pcb.h"

#include <3d_rendering/raytracing/render_3d_raytrace_ram.h>
#include <3d_rendering/track_ball.h>
#include <3d_viewer/eda_3d_viewer_settings.h>
#include <3d_cache/3d_cache.h>
#include <board.h>
#include <filename_resolver.h>
#include <footprint.h>
#include <project_pcb.h>
#include <reporter.h>

#include <wx/filename.h>
#include <wx/image.h>
#include <wx/mstream.h>

#include <chrono>
#include <cmath>
#include <map>

using namespace kicadd::board;
using kiapi::common::ApiResponseStatus;
using kiapi::common::ApiStatusCode;

namespace
{

ApiResponseStatus error3d( ApiStatusCode aCode, const std::string& aMsg )
{
    ApiResponseStatus e;
    e.set_status( aCode );
    e.set_error_message( aMsg );
    return e;
}

} // namespace


HANDLER_RESULT<Render3DResponse> KICADD_HANDLER_PCB::handleRender3D( const HANDLER_CONTEXT<Render3D>& aCtx )
{
    BOARD* brd = board();

    if( !brd )
        return tl::unexpected( error3d( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    if( auto err = busy() )   // the picture holds KiCad's main thread: not while the user is in a tool
        return tl::unexpected( *err );

    const Render3D& req = aCtx.Request;
    const auto      t0 = std::chrono::steady_clock::now();

    static const std::map<std::string, VIEW3D_TYPE> sides = {
        { "top", VIEW3D_TYPE::VIEW3D_TOP },     { "bottom", VIEW3D_TYPE::VIEW3D_BOTTOM },
        { "front", VIEW3D_TYPE::VIEW3D_FRONT }, { "back", VIEW3D_TYPE::VIEW3D_BACK },
        { "left", VIEW3D_TYPE::VIEW3D_LEFT },   { "right", VIEW3D_TYPE::VIEW3D_RIGHT },
    };

    const std::string side = req.side().empty() ? "top" : req.side();
    auto              sideIt = sides.find( side );

    if( sideIt == sides.end() )
        return tl::unexpected( error3d( ApiStatusCode::AS_BAD_REQUEST, "side: top, bottom, front, back, left or right" ) );

    const int w = req.width() > 0 ? req.width() : 1200;
    const int h = req.height() > 0 ? req.height() : 900;

    if( w < 64 || h < 64 || w > 3000 || h > 3000 )
        return tl::unexpected( error3d( ApiStatusCode::AS_BAD_REQUEST, "width / height: 64 .. 3000 pixels" ) );

    Render3DResponse out;

    // --- the parts the picture cannot show: no model, the file not found, STEP / IGES (no OCC)
    S3D_CACHE*         cache = PROJECT_PCB::Get3DCacheManager( brd->GetProject() );
    FILENAME_RESOLVER* resolver = cache ? cache->GetResolver() : nullptr;
    int                withModel = 0;

    for( FOOTPRINT* fp : brd->Footprints() )
    {
        std::string why;
        bool        any = false;

        for( const FP_3DMODEL& m : fp->Models() )
        {
            if( !m.m_Show )
                continue;

            any = true;
            wxString ext = wxFileName( m.m_Filename ).GetExt().Lower();

            if( ext == wxS( "step" ) || ext == wxS( "stp" ) || ext == wxS( "stpz" ) || ext == wxS( "igs" ) || ext == wxS( "iges" ) )
            {
                why = "STEP / IGES model (this build loads VRML only)";
                continue;
            }

            std::vector<const EMBEDDED_FILES*> embedded = { brd->GetEmbeddedFiles(), fp->GetEmbeddedFiles() };
            wxString path = resolver ? resolver->ResolvePath( m.m_Filename, wxEmptyString, std::move( embedded ) ) : wxString();

            if( path.IsEmpty() || !wxFileName::FileExists( path ) )
            {
                why = "model file not found (" + m.m_Filename.ToStdString( wxConvUTF8 ) + ")";
                continue;
            }

            why.clear();
            break;
        }

        if( !any )
            why = "no 3D model";

        if( why.empty() )
        {
            withModel++;
        }
        else
        {
            Render3DMissing* miss = out.add_missing();
            miss->set_ref( fp->GetReference().ToStdString( wxConvUTF8 ) );
            miss->set_why( why );
        }
    }

    out.set_with_model( withModel );

    // --- the renderer, as JobExportRender sets it up
    BOARD_ADAPTER adapter;
    adapter.SetBoard( brd );
    adapter.m_IsBoardView = false;

    // the settings' members get their values from their parameters' defaults (a constructed
    // settings object alone shows nothing: no board body, no layers)
    EDA_3D_VIEWER_SETTINGS cfg;
    cfg.ResetToDefaults();
    cfg.m_Render.show_footprints_not_in_posfile = true;
    cfg.m_Render.show_footprints_dnp = true;
    cfg.m_Render.show_footprints_insert = true;
    cfg.m_Render.show_footprints_normal = true;
    cfg.m_Render.show_footprints_virtual = true;

    const bool high = req.quality() == "high";
    cfg.m_Render.raytrace_anti_aliasing = true;
    cfg.m_Render.raytrace_backfloor = high || req.floor();
    cfg.m_Render.raytrace_post_processing = high || req.floor();
    cfg.m_Render.raytrace_procedural_textures = high;
    cfg.m_Render.raytrace_reflections = high;
    cfg.m_Render.raytrace_shadows = high || req.floor();
    cfg.m_Render.differentiate_plated_copper = true;
    cfg.m_Render.raytrace_refractions = true;   // tracks under the soldermask
    cfg.m_Render.raytrace_recursivelevel_refractions = 1;

    // kicad-cli's default lights
    cfg.m_Render.raytrace_lightColorTop = COLOR4D( 0.0, 0.0, 0.0, 1.0 );
    cfg.m_Render.raytrace_lightColorBottom = COLOR4D( 0.0, 0.0, 0.0, 1.0 );
    cfg.m_Render.raytrace_lightColorCamera = COLOR4D( 0.0, 0.0, 0.0, 1.0 );
    const COLOR4D side4( 0.5, 0.5, 0.5, 1.0 );
    cfg.m_Render.raytrace_lightColor = { side4, side4, side4, side4, side4, side4, side4, side4 };
    cfg.m_Render.raytrace_lightElevation = { 60, 60, 60, 60, -60, -60, -60, -60 };
    cfg.m_Render.raytrace_lightAzimuth = { 45, 135, 225, 315, 45, 135, 225, 315 };
    cfg.m_UseStackupColors = true;
    adapter.m_Cfg = &cfg;
    adapter.Set3dCacheManager( cache );

    wxSize     size( w, h );
    TRACK_BALL camera( 2 * RANGE_SCALE_3D );
    camera.SetProjection( req.perspective() ? PROJECTION_TYPE::PERSPECTIVE : PROJECTION_TYPE::ORTHO );
    camera.SetCurWindowSize( size );

    RENDER_3D_RAYTRACE_RAM raytrace( adapter, camera );
    raytrace.SetCurWindowSize( size );

    NULL_REPORTER              quiet;
    std::shared_ptr<REPORTER> reporter( &quiet, []( REPORTER* ) {} );
    raytrace.SetReporters( reporter, reporter );

    const double zoom = req.zoom() > 0 ? req.zoom() : 1.0;

    for( bool first = true; raytrace.Redraw( false ); first = false )
    {
        if( first )
        {
            // the first redraw puts the look-at point on the board's centre: the camera after it
            camera.ViewCommand_T1( sideIt->second );
            camera.Zoom_T1( (float) zoom );
            camera.RotateX_T1( DEG2RAD( req.rot_x() ) );
            camera.RotateY_T1( DEG2RAD( req.rot_y() ) );
            camera.RotateZ_T1( DEG2RAD( req.rot_z() ) );
            camera.Interpolate( 1.0f );
            camera.SetT0_and_T1_current_T();
            camera.ParametersChanged();
        }
    }

    uint8_t* rgba = raytrace.GetBuffer();
    wxSize   real = raytrace.GetRealBufferSize();

    if( !rgba || real.x <= 0 || real.y <= 0 )
        return tl::unexpected( error3d( ApiStatusCode::AS_INTERNAL_ERROR, "the raytracer produced no picture" ) );

    // RGBA (bottom row first) -> PNG on a white background
    const size_t   n = (size_t) real.x * real.y;
    unsigned char* rgb = (unsigned char*) malloc( n * 3 );

    for( size_t i = 0; i < n; ++i )
    {
        const double a = rgba[i * 4 + 3] / 255.0;

        for( int c = 0; c < 3; ++c )
            rgb[i * 3 + c] = (unsigned char) std::lround( rgba[i * 4 + c] * a + 255 * ( 1 - a ) );
    }

    wxImage image( real, rgb );   // takes rgb
    image = image.Mirror( false );

    if( !wxImage::FindHandler( wxBITMAP_TYPE_PNG ) )
        wxImage::AddHandler( new wxPNGHandler );

    wxMemoryOutputStream stream;

    if( !image.SaveFile( stream, wxBITMAP_TYPE_PNG ) )
        return tl::unexpected( error3d( ApiStatusCode::AS_INTERNAL_ERROR, "the picture could not be encoded" ) );

    std::string bytes( stream.GetLength(), '\0' );
    stream.CopyTo( bytes.data(), bytes.size() );

    out.set_png( bytes );
    out.set_width( real.x );
    out.set_height( real.y );
    out.set_seconds( std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() );
    return out;
}

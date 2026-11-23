/*
 * KiCadD: the Co-pilot panel. See kicadd_copilot_panel.h.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "common/kicadd_copilot_panel.h"
#include "common/kicadd_copilot.h"

#include <algorithm>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/listctrl.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

using kicadd::COPILOT;
using kicadd::COPILOT_EVENT;


KICADD_COPILOT_PANEL::KICADD_COPILOT_PANEL( wxWindow* aParent, std::function<void()> aFocusCanvas ) :
        wxPanel( aParent, wxID_ANY ),
        m_timer( this ),
        m_focusCanvas( std::move( aFocusCanvas ) )
{
    auto* top = new wxBoxSizer( wxVERTICAL );
    auto* row = new wxBoxSizer( wxHORIZONTAL );

    m_pause = new wxCheckBox( this, wxID_ANY, _( "Pause agent edits" ) );
    m_pause->SetToolTip( _( "While paused the agent's edits are refused (it is told so); it can still read "
                            "the board. Ctrl+Alt+P" ) );
    m_clear = new wxButton( this, wxID_ANY, _( "Clear" ), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT );
    row->Add( m_pause, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 4 ) );
    row->Add( m_clear, 0, wxALIGN_CENTER_VERTICAL );

    m_status = new wxStaticText( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                 wxST_ELLIPSIZE_END );

    m_list = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxLC_REPORT | wxLC_NO_HEADER | wxLC_SINGLE_SEL );
    m_list->AppendColumn( _( "Time" ) );
    m_list->AppendColumn( _( "Event" ) );

    top->Add( row, 0, wxEXPAND | wxALL, FromDIP( 4 ) );
    top->Add( m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 4 ) );
    top->Add( m_list, 1, wxEXPAND );
    SetSizer( top );

    m_pause->Bind( wxEVT_CHECKBOX, [this]( wxCommandEvent& ) { COPILOT::Get().SetPaused( m_pause->GetValue() ); } );
    m_clear->Bind( wxEVT_BUTTON, []( wxCommandEvent& ) { COPILOT::Get().Clear(); } );
    m_list->Bind( wxEVT_SIZE,
                  [this]( wxSizeEvent& aEvt )
                  {
                      aEvt.Skip();
                      int w = m_list->GetClientSize().x - m_list->GetColumnWidth( 0 );
                      m_list->SetColumnWidth( 1, std::max( w, FromDIP( 100 ) ) );
                  } );

    // a change arrives while the request is handled: redrawn once, after it
    m_listenId = COPILOT::Get().Listen(
            [this]()
            {
                if( m_dirty )
                    return;

                m_dirty = true;
                CallAfter( [this]()
                           {
                               m_dirty = false;
                               rebuild();
                           } );
            } );

    Bind( wxEVT_TIMER, [this]( wxTimerEvent& ) { updateStatus(); } );

    // a click here (the switch, the list) leaves the keyboard with the editor. Focus and mouse
    // events do not travel up, and wxChildFocusEvent is not sent again for a window wx still
    // thinks has the focus (a second click in the list): every window of the panel, the
    // list's own inner ones included, hands it back itself.
    std::function<void( wxWindow* )> giveBack = [&]( wxWindow* aWin )
    {
        auto back = [this]()
        {
            if( m_focusCanvas )
                CallAfter( [this]() { m_focusCanvas(); } );
        };

        aWin->Bind( wxEVT_SET_FOCUS, [back]( wxFocusEvent& aEvt ) { aEvt.Skip(); back(); } );
        aWin->Bind( wxEVT_LEFT_UP, [back]( wxMouseEvent& aEvt ) { aEvt.Skip(); back(); } );

        for( wxWindow* child : aWin->GetChildren() )
            giveBack( child );
    };

    giveBack( this );
    m_timer.Start( 1000 );

    m_list->SetColumnWidth( 0, GetTextExtent( wxS( "00:00:00 " ) ).x + FromDIP( 8 ) );
    rebuild();
}


KICADD_COPILOT_PANEL::~KICADD_COPILOT_PANEL()
{
    m_timer.Stop();
    COPILOT::Get().Unlisten( m_listenId );
}


void KICADD_COPILOT_PANEL::rebuild()
{
    const std::vector<COPILOT_EVENT>& events = COPILOT::Get().Events();

    m_pause->SetValue( COPILOT::Get().Paused() );

    // the model appends (and drops its oldest); Clear empties it
    if( COPILOT::Get().Clears() != m_clears )
    {
        m_list->DeleteAllItems();
        m_clears = COPILOT::Get().Clears();
        m_added = COPILOT::Get().EventsAdded() - (long long) events.size();
    }

    const size_t fresh = (size_t) std::min<long long>( COPILOT::Get().EventsAdded() - m_added, (long long) events.size() );
    m_added = COPILOT::Get().EventsAdded();
    m_list->Freeze();

    for( size_t i = events.size() - fresh; i < events.size(); ++i )
    {
        const COPILOT_EVENT& e = events[i];
        wxString             text = e.text;

        if( e.kind == COPILOT_EVENT::REFUSED )
            text = _( "refused " ) + text + ( e.repeat > 1 ? wxString::Format( wxS( " (x%d)" ), e.repeat ) : wxString() );

        long row = m_list->InsertItem( 0, e.when.Format( wxS( "%H:%M:%S" ) ) );   // newest on top
        m_list->SetItem( row, 1, text );

        if( e.kind == COPILOT_EVENT::REFUSED )
            m_list->SetItemTextColour( row, wxColour( 200, 90, 40 ) );
        else if( e.kind == COPILOT_EVENT::NOTE )
            m_list->SetItemTextColour( row, wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT ) );
    }

    // the newest refusal may have been repeated since it was listed
    if( !events.empty() && m_list->GetItemCount() > 0 && events.back().repeat > 1 )
    {
        m_list->SetItem( 0, 0, events.back().when.Format( wxS( "%H:%M:%S" ) ) );
        m_list->SetItem( 0, 1, _( "refused " ) + events.back().text
                                       + wxString::Format( wxS( " (x%d)" ), events.back().repeat ) );
    }

    // the list keeps what the model keeps (its oldest at the bottom)
    while( m_list->GetItemCount() > (int) events.size() )
        m_list->DeleteItem( m_list->GetItemCount() - 1 );

    m_list->Thaw();
    updateStatus();
}


void KICADD_COPILOT_PANEL::updateStatus()
{
    const COPILOT& c = COPILOT::Get();
    wxString       s;

    if( c.Requests() == 0 )
    {
        s = _( "No agent requests yet" );
    }
    else
    {
        long secs = (long) ( wxDateTime::Now() - c.LastRequest() ).GetSeconds().ToLong();
        wxString ago = secs < 2 ? _( "just now" )
                     : secs < 120 ? wxString::Format( _( "%ld s ago" ), secs )
                                  : wxString::Format( _( "%ld min ago" ), secs / 60 );

        s = wxString::Format( _( "%lld edits, %lld refused; last %s (%s)" ), c.EditCount(), c.RefusedCount(), ago,
                              c.LastType() );
    }

    if( c.Paused() )
        s = _( "PAUSED: " ) + s;

    if( m_status->GetLabel() != s )
    {
        m_status->SetLabel( s );
        m_status->SetToolTip( s );
    }
}

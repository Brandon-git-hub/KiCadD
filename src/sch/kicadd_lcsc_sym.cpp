/*
 * KiCadD M4b: LCSC part -> KiCad schematic symbol.
 *
 * Lives in the eeschema kiface and is reached from pcbnew (where lcsc_fetch runs) through
 * KIFACE_KICADD_LCSC_SYMBOL. Reads the cached EasyEDA component answer (the same file the
 * footprint comes from), converts the symbol with KiCad's EasyEDA Standard parser
 * (eeschema/sch_io/easyeda) and saves it into a .kicad_sym library.
 *
 * Request (JSON):  {"source": "<cached C....json>", "library": "<.../lcsc.kicad_sym>",
 *                   "lcsc": "C8734", "footprint": "LCSC:<package>", "datasheet": "<url>",
 *                   "description": "...",
 *                   "keywords": "...", "attribution": "..."}
 * Answer (JSON):   {"ok": true, "name": "<symbol>", "reference": "U", "units": 1,
 *                   "pins": ["1", "2", ...]}   or   {"ok": false, "error": "..."}
 *
 * Source data: JLCEDA/EasyEDA Official Library, https://lceda.cn/ , https://easyeda.com .
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include <lib_symbol.h>
#include <sch_field.h>
#include <sch_pin.h>
#include <lib_id.h>
#include <ki_exception.h>
#include <sch_io/easyeda/sch_easyeda_parser.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>

#include <json_common.h>

#include <wx/arrstr.h>
#include <wx/ffile.h>
#include <wx/filename.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>

using json = nlohmann::json;

namespace
{

wxString wx( const std::string& s )
{
    return wxString::FromUTF8( s );
}


std::string jstr( const json& aObj, const char* aKey )
{
    auto it = aObj.find( aKey );

    if( it == aObj.end() || it->is_null() )
        return {};

    return it->is_string() ? it->get<std::string>() : it->dump();
}


double jnum( const json& aObj, const char* aKey )
{
    auto it = aObj.find( aKey );

    if( it == aObj.end() )
        return 0;

    if( it->is_number() )
        return it->get<double>();

    if( it->is_string() )
    {
        double v = 0;
        wx( it->get<std::string>() ).ToCDouble( &v );
        return v;
    }

    return 0;
}


/// An EasyEDA symbol document (head + shape), sometimes still a JSON string.
json symbolDoc( const json& aDataStr )
{
    if( aDataStr.is_string() )
        return json::parse( aDataStr.get<std::string>(), nullptr, false );

    return aDataStr;
}


/// One EasyEDA symbol document -> a LIB_SYMBOL (single unit, items in unit 0 or 1).
std::unique_ptr<LIB_SYMBOL> parseDoc( SCH_EASYEDA_PARSER& aParser, const json& aDoc )
{
    if( !aDoc.is_object() || !aDoc.contains( "head" ) || !aDoc.contains( "shape" ) )
        return nullptr;

    const json& head = aDoc["head"];
    std::map<wxString, wxString> params;

    if( auto cp = head.find( "c_para" ); cp != head.end() && cp->is_object() )
    {
        for( auto& [k, v] : cp->items() )
            params[wx( k )] = v.is_string() ? wx( v.get<std::string>() ) : wx( v.dump() );
    }

    wxArrayString shapes;

    for( const json& s : aDoc["shape"] )
    {
        if( s.is_string() )
            shapes.Add( wx( s.get<std::string>() ) );
    }

    VECTOR2D origin( jnum( head, "x" ), jnum( head, "y" ) );
    return std::unique_ptr<LIB_SYMBOL>( aParser.ParseSymbol( origin, params, shapes ) );
}


wxString answer( const json& aJs )
{
    return wx( aJs.dump() );
}


wxString fail( const std::string& aError )
{
    return answer( json{ { "ok", false }, { "error", aError } } );
}


void setUserField( LIB_SYMBOL& aSym, const wxString& aName, const wxString& aValue, bool aVisible )
{
    SCH_FIELD* f = aSym.GetField( aName );

    if( !f )
    {
        f = new SCH_FIELD( &aSym, FIELD_T::USER, aName );
        aSym.AddField( f );
    }

    f->SetText( aValue );
    f->SetVisible( aVisible );
}

} // namespace


wxString KicaddLcscSymbol( const wxString& aRequest )
{
    json req = json::parse( aRequest.ToStdString( wxConvUTF8 ), nullptr, false );

    if( req.is_discarded() || !req.is_object() )
        return fail( "bad request" );

    const std::string source = jstr( req, "source" );
    const std::string library = jstr( req, "library" );
    const std::string lcsc = jstr( req, "lcsc" );

    std::string body;

    {
        wxFFile f( wx( source ), wxS( "rb" ) );

        if( !f.IsOpened() )
            return fail( "cannot read " + source );

        wxString content;
        f.ReadAll( &content, wxConvUTF8 );
        body = content.ToStdString( wxConvUTF8 );
    }

    json doc = json::parse( body, nullptr, false );

    if( doc.is_discarded() || !doc.contains( "result" ) || !doc["result"].is_object() )
        return fail( lcsc + ": the cached EasyEDA answer is not a part" );

    const json& result = doc["result"];

    try
    {
        SCH_EASYEDA_PARSER parser( nullptr, nullptr, nullptr );

        // Multi-unit parts (e.g. a dual op-amp) come as subparts, one symbol document per unit;
        // everything else is the one document in dataStr.
        std::vector<json> unitDocs;

        if( auto sp = result.find( "subparts" ); sp != result.end() && sp->is_array() && !sp->empty() )
        {
            for( const json& sub : *sp )
            {
                if( sub.is_object() && sub.contains( "dataStr" ) )
                    unitDocs.push_back( symbolDoc( sub["dataStr"] ) );
            }
        }

        if( unitDocs.empty() && result.contains( "dataStr" ) )
            unitDocs.push_back( symbolDoc( result["dataStr"] ) );

        if( unitDocs.empty() )
            return fail( lcsc + " has no schematic symbol in the EasyEDA library" );

        std::unique_ptr<LIB_SYMBOL> sym = parseDoc( parser, unitDocs[0] );

        if( !sym )
            return fail( lcsc + ": the EasyEDA symbol could not be read" );

        if( unitDocs.size() > 1 )
        {
            // Unit 1 is what the first document drew; units 2.. are copied in from the others.
            for( SCH_ITEM& item : sym->GetDrawItems() )
                item.SetUnit( 1 );

            sym->SetUnitCount( (int) unitDocs.size(), false );

            for( size_t u = 1; u < unitDocs.size(); ++u )
            {
                std::unique_ptr<LIB_SYMBOL> other = parseDoc( parser, unitDocs[u] );

                if( !other )
                    return fail( lcsc + ": unit " + std::to_string( u + 1 ) + " could not be read" );

                for( SCH_ITEM& item : other->GetDrawItems() )
                {
                    if( item.Type() == SCH_FIELD_T )
                        continue;

                    SCH_ITEM* copy = static_cast<SCH_ITEM*>( item.Clone() );
                    copy->SetParent( sym.get() );
                    copy->SetUnit( (int) u + 1 );
                    sym->AddDrawItem( copy, false );
                }
            }

            sym->GetDrawItems().sort();
        }

        // Name: the part title (what EasyEDA calls the symbol), made safe for a LIB_ID.
        wxString title = wx( jstr( result, "title" ) );

        if( title.IsEmpty() )
            title = sym->GetName();

        wxString name = wx( LIB_ID::FixIllegalChars( UTF8( title ), true ).c_str() );

        // Clear reference numbers: "U?" -> "U"
        wxString ref;

        for( wxUniChar c : sym->GetReferenceField().GetText() )
        {
            if( wxIsdigit( c ) || c == '?' )
                break;

            ref << c;
        }

        if( ref.IsEmpty() )
            ref = wxS( "U" );

        sym->GetReferenceField().SetText( ref );

        // EasyEDA leaves the pins of passives "unspecified" or, as often, "input" (C25804): pins
        // of resistors, capacitors, inductors, ferrites, fuses, crystals and diodes are passive,
        // as in KiCad's own libraries, unless the symbol gives one of them a real direction.
        static const std::set<wxString> passives = { wxS( "R" ), wxS( "C" ), wxS( "L" ), wxS( "FB" ), wxS( "F" ),
                                                     wxS( "Y" ), wxS( "X" ), wxS( "D" ), wxS( "LED" ), wxS( "RN" ),
                                                     wxS( "RV" ), wxS( "TH" ), wxS( "VR" ) };
        std::vector<SCH_PIN*> symPins = sym->GetGraphicalPins();

        if( passives.count( ref.Upper() ) && !symPins.empty()
            && std::all_of( symPins.begin(), symPins.end(),
                            []( const SCH_PIN* p )
                            {
                                return p->GetType() == ELECTRICAL_PINTYPE::PT_UNSPECIFIED
                                       || p->GetType() == ELECTRICAL_PINTYPE::PT_INPUT;
                            } ) )
        {
            for( SCH_PIN* pin : symPins )
                pin->SetType( ELECTRICAL_PINTYPE::PT_PASSIVE );
        }

        // A different part may already use this name in the library: keep both.
        SCH_IO_KICAD_SEXPR io;
        wxString           libPath = wx( library );

        if( !wxFileName::FileExists( libPath ) )
            io.CreateLibrary( libPath );

        // LoadSymbol hands out the plugin cache's own copy: look, don't delete
        if( const LIB_SYMBOL* old = io.LoadSymbol( libPath, name ) )
        {
            const SCH_FIELD* f = old->GetField( wxS( "LCSC" ) );

            if( f && f->GetText() != wx( lcsc ) )
                name << wxS( "_" ) << wx( lcsc );
        }

        sym->SetName( name );
        sym->SetLibId( LIB_ID( wxS( "LCSC" ), name ) );
        // A passive's value is what it is called on a schematic ("10k", "100nF"), not its
        // manufacturer part number; EasyEDA has it as the "Value" parameter.
        wxString value = title;

        if( passives.count( ref.Upper() ) )
        {
            const json& head = unitDocs[0]["head"];

            if( auto cp = head.find( "c_para" ); cp != head.end() && cp->is_object() )
            {
                wxString v = wx( jstr( *cp, "Value" ) ).Strip( wxString::both );
                v.Replace( wxString::FromUTF8( "\xce\xa9" ), wxEmptyString );       // 10kΩ -> 10k
                v.Replace( wxString::FromUTF8( "\xe2\x84\xa6" ), wxEmptyString );   // (ohm sign)

                if( !v.IsEmpty() )
                    value = v;
            }
        }

        sym->GetValueField().SetText( value );
        sym->GetFootprintField().SetText( wx( jstr( req, "footprint" ) ) );
        sym->GetFootprintField().SetVisible( false );

        for( SCH_FIELD* ds = sym->GetField( wxS( "Datasheet" ) ); ds && ds->GetId() != FIELD_T::DATASHEET;
             ds = nullptr )
        {
            // the parser may keep EasyEDA's "link" as a user field; KiCad has its own
            sym->GetDatasheetField().SetText( ds->GetText() );
            sym->RemoveField( ds );
        }

        if( std::string ds = jstr( req, "datasheet" ); !ds.empty() )
            sym->GetDatasheetField().SetText( wx( ds ) );

        // The parser leaves Reference and Value at the origin, on top of the body: put them
        // above and below it, left aligned, like KiCad's own library does.
        BOX2I body = sym->GetBodyAndPinsBoundingBox();
        int   gap = schIUScale.MilsToIU( 50 );

        for( auto [field, y] : { std::pair{ &sym->GetReferenceField(), body.GetTop() - gap },
                                 std::pair{ &sym->GetValueField(), body.GetBottom() + gap } } )
        {
            field->SetPosition( VECTOR2I( body.GetLeft(), y ) );
            field->SetHorizJustify( GR_TEXT_H_ALIGN_LEFT );
            field->SetVisible( true );
        }

        sym->SetDescription( wx( jstr( req, "description" ) ) );
        sym->SetKeyWords( wx( jstr( req, "keywords" ) ) );
        setUserField( *sym, wxS( "LCSC" ), wx( lcsc ), false );
        setUserField( *sym, wxS( "Source" ), wx( jstr( req, "attribution" ) ), false );

        std::set<wxString> pinSet;

        for( SCH_PIN* pin : sym->GetGraphicalPins( 0, 0 ) )
            pinSet.insert( pin->GetNumber() );

        json pins = json::array();

        for( const wxString& p : pinSet )
            pins.push_back( p.ToStdString( wxConvUTF8 ) );

        json out = { { "ok", true },
                     { "name", name.ToStdString( wxConvUTF8 ) },
                     { "reference", ref.ToStdString( wxConvUTF8 ) },
                     { "units", sym->GetUnitCount() },
                     { "pins", pins } };

        io.SaveSymbol( libPath, std::move( sym ) );
        return answer( out );
    }
    catch( const IO_ERROR& e )
    {
        return fail( lcsc + ": symbol conversion failed: " + e.What().ToStdString( wxConvUTF8 ) );
    }
    catch( const std::exception& e )
    {
        return fail( lcsc + ": symbol conversion failed: " + e.what() );
    }
}

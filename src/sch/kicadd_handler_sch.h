/*
 * KiCadD M5a: schematic generation for agents (proto/kicadd/schematic.proto).
 *
 * Registered next to KiCad's API_HANDLER_SCH on the same SCH_CONTEXT: headless (api-server,
 * eeschema's HandleApiOpenDocument) and in the Schematic Editor (desktop co-pilot: the agent's
 * edits are the editor's commits, on its undo list and canvas; refused while the user is in a
 * tool). Parts come from a library or an LCSC number; pins are
 * connected by a wire stub and a net label, so the agent names every net and never routes.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <api/api_handler.h>
#include <sch_label.h>
#include <kicadd/schematic.pb.h>

class SCH_CONTEXT;
class SCH_COMMIT;
class SCH_EDIT_FRAME;
class wxString;
class SCH_PIN;
class SCH_ITEM;
class SCH_SCREEN;
class SCH_SHEET;
class SCH_SHEET_PATH;
class SCH_SYMBOL;
class SCHEMATIC;


class KICADD_HANDLER_SCH : public API_HANDLER
{
public:
    explicit KICADD_HANDLER_SCH( std::shared_ptr<SCH_CONTEXT> aContext );
    ~KICADD_HANDLER_SCH() override;

    /// One placed pin: the symbol instance it belongs to, its sheet and screen.
    struct PIN_AT;

private:
    SCHEMATIC* schematic() const;

    HANDLER_RESULT<kicadd::schematic::SchSummary>
    handleGetSummary( const HANDLER_CONTEXT<kicadd::schematic::GetSchSummary>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchPartResponse>
    handleAddPart( const HANDLER_CONTEXT<kicadd::schematic::SchAddPart>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchPartResponse>
    handleGetPart( const HANDLER_CONTEXT<kicadd::schematic::SchGetPart>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchConnectResponse>
    handleConnect( const HANDLER_CONTEXT<kicadd::schematic::SchConnect>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchConnectResponse>
    handleDisconnect( const HANDLER_CONTEXT<kicadd::schematic::SchDisconnect>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchNetlist>
    handleGetNetlist( const HANDLER_CONTEXT<kicadd::schematic::GetSchNetlist>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchConnectResponse>
    handleDeleteParts( const HANDLER_CONTEXT<kicadd::schematic::SchDeleteParts>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchConnectResponse>
    handleSave( const HANDLER_CONTEXT<kicadd::schematic::SchSave>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchSheetResponse>
    handleAddSheet( const HANDLER_CONTEXT<kicadd::schematic::SchAddSheet>& aCtx );

    /// Sheet symbols named @a aName (or with that path), with the path of the sheet they sit on.
    std::vector<std::pair<SCH_SHEET*, SCH_SHEET_PATH>> findSheetSymbols( const std::string& aName );

    /// The sheet path for a name ("Power") or a path ("/Power/"); "" or "/" is the root.
    std::optional<SCH_SHEET_PATH> findSheet( const std::string& aNameOrPath, std::string& aError );

    void packSheet( SCH_SHEET* aSheet, const SCH_SHEET_PATH& aParent, kicadd::schematic::SchSheet& aOut );

    /// A sheet pin for a port: inputs on the left, the rest on the right; grows the sheet.
    void addSheetPin( SCH_SHEET* aSheet, const wxString& aName, LABEL_FLAG_SHAPE aShape );

    /// Sheets were added or removed: the hierarchy (and the editor's navigator) again.
    void hierarchyChanged();

    static std::string shapeName( LABEL_FLAG_SHAPE aShape );

    /// Symbols (all units) carrying @a aRef, with their sheet.
    std::vector<std::pair<SCH_SYMBOL*, SCH_SHEET_PATH>> findSymbols( const std::string& aRef );

    /// The pin @a aPin ("3" or a unique name) of part @a aRef.
    bool findPin( const std::string& aRef, const std::string& aPin, PIN_AT& aOut, std::string& aError );

    /// Remove what connects a pin (stub wires with their labels, labels, no-connect flags).
    int disconnectPin( const PIN_AT& aPin, SCH_COMMIT& aCommit );

    /// Net name of a placed pin after a connectivity rebuild ("" when on no net).
    std::string pinNet( SCH_ITEM* aPin, const SCH_SHEET_PATH& aSheet );

    void packSymbol( SCH_SYMBOL* aSymbol, const SCH_SHEET_PATH& aSheet, bool aWithPins,
                     kicadd::schematic::SchSymbol& aOut );

    void rebuildConnectivity();

    /// The Schematic Editor this handler works in (desktop co-pilot), null when headless.
    SCH_EDIT_FRAME* editor() const;

    /// The editor is in an interactive tool: refuse edits (AS_BUSY).
    std::optional<ApiResponseStatus> busy() const;

    /// Name the agent's edit in the status bar and repaint (desktop).
    void announce( const wxString& aWhat ) const;

    std::shared_ptr<SCH_CONTEXT> m_context;
};

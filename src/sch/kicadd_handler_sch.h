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

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <api/api_handler.h>
#include <lib_id.h>
#include <lib_symbol.h>
#include <sch_label.h>
#include <kicadd/schematic.pb.h>
#include "sch/kicadd_sch_history.h"

class SCH_CONTEXT;
class SCH_COMMIT;
class SCH_EDIT_FRAME;
class wxString;
class SCH_PIN;
class SCH_ITEM;
class SCH_SCREEN;
class SCH_LINE;
class SCH_SHEET;
class SCH_SHEET_PATH;
class SCH_SYMBOL;
class SCHEMATIC;
class SCH_REFERENCE_LIST;


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

    HANDLER_RESULT<kicadd::schematic::SchPartResponse>
    handleMove( const HANDLER_CONTEXT<kicadd::schematic::SchMove>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchWireResponse>
    handleWire( const HANDLER_CONTEXT<kicadd::schematic::SchWire>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchCreateSymbolResponse>
    handleCreateSymbol( const HANDLER_CONTEXT<kicadd::schematic::SchCreateSymbol>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchHistoryResponse>
    handleUndo( const HANDLER_CONTEXT<kicadd::schematic::SchUndo>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchConnectResponse>
    handleSetFields( const HANDLER_CONTEXT<kicadd::schematic::SchSetFields>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchAuditResponse>
    handleAudit( const HANDLER_CONTEXT<kicadd::schematic::SchAudit>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchErcResponse>
    handleRunErc( const HANDLER_CONTEXT<kicadd::schematic::SchRunErc>& aCtx );

    HANDLER_RESULT<kicadd::schematic::RegisterLibraryResponse>
    handleRegisterLibrary( const HANDLER_CONTEXT<kicadd::schematic::RegisterLibrary>& aCtx );

    HANDLER_RESULT<kicadd::schematic::SchRenderResponse>
    handleRender( const HANDLER_CONTEXT<kicadd::schematic::SchRender>& aCtx );

    /// A power symbol for net @a aNet ("GND", "+3V3"): the project's power library if it has
    /// one, else made here (KiCadD ships no libraries): a ground or a supply shape.
    const LIB_SYMBOL* powerSymbol( const std::string& aNet, LIB_ID& aId );

    /// A power symbol instance for @a aNet with its pin at @a aPos, pointing along @a aOut
    /// (away from what it connects to), annotated #PWRnn against @a aPowerRefs (the power
    /// symbols there are, plus those added in this commit). Not yet on the screen.
    SCH_SYMBOL* newPowerSymbol( const std::string& aNet, const VECTOR2I& aPos, const VECTOR2I& aOut,
                                const SCH_SHEET_PATH& aSheet, SCH_REFERENCE_LIST& aPowerRefs );

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

    /// aSeed's wire run (through bends, up to pins / junctions / labels / branches) into aGone,
    /// with the labels, power symbols and junctions at its ends that are left on nothing
    /// (aKeep: an end not to clean up, the pin being disconnected).
    void removeWireRun( SCH_SCREEN* aScreen, SCH_LINE* aSeed, const VECTOR2I* aKeep, std::set<SCH_ITEM*>& aGone );

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

    /// Push an agent edit and record it in the agent's history (sch_undo / sch_redo).
    void pushRecorded( kicadd::KICADD_SCH_COMMIT& aCommit, const wxString& aWhat );

    /// Apply a history entry backwards (undo) or forwards (redo) in a new commit.
    void applyEdit( const kicadd::SCH_EDIT& aEdit, bool aUndo, kicadd::schematic::SchHistoryResponse& aOut );

    std::shared_ptr<SCH_CONTEXT> m_context;

    std::vector<kicadd::SCH_EDIT> m_undo, m_redo;

    /// Power symbols made here, by net name (the instances copy them)
    std::map<std::string, std::unique_ptr<LIB_SYMBOL>> m_powerSymbols;
};

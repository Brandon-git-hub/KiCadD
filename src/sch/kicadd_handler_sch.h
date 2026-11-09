/*
 * KiCadD M5a: schematic generation for agents (proto/kicadd/schematic.proto).
 *
 * Registered next to KiCad's API_HANDLER_SCH on the same SCH_CONTEXT (headless api-server:
 * eeschema's HandleApiOpenDocument). Parts come from a library or an LCSC number; pins are
 * connected by a wire stub and a net label, so the agent names every net and never routes.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <api/api_handler.h>
#include <kicadd/schematic.pb.h>

class SCH_CONTEXT;
class SCH_COMMIT;
class SCH_PIN;
class SCH_SCREEN;
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

    /// Symbols (all units) carrying @a aRef, with their sheet.
    std::vector<std::pair<SCH_SYMBOL*, SCH_SHEET_PATH>> findSymbols( const std::string& aRef );

    /// The pin @a aPin ("3" or a unique name) of part @a aRef.
    bool findPin( const std::string& aRef, const std::string& aPin, PIN_AT& aOut, std::string& aError );

    /// Remove what connects a pin (stub wires with their labels, labels, no-connect flags).
    int disconnectPin( const PIN_AT& aPin, SCH_COMMIT& aCommit );

    /// Net name of a placed pin after a connectivity rebuild ("" when on no net).
    std::string pinNet( SCH_PIN* aPin, const SCH_SHEET_PATH& aSheet );

    void packSymbol( SCH_SYMBOL* aSymbol, const SCH_SHEET_PATH& aSheet, bool aWithPins,
                     kicadd::schematic::SchSymbol& aOut );

    void rebuildConnectivity();

    std::shared_ptr<SCH_CONTEXT> m_context;
};

/*
 * KiCadD M4d: "Add LCSC Symbol..." in the Schematic Editor (Place menu).
 *
 * Asks for an LCSC number and has pcbnew (KIFACE_KICADD_LCSC_FETCH, the same code as the
 * agent's lcsc_fetch) fetch the part and save footprint + symbol into the project's LCSC
 * libraries, then places the symbol with KiCad's own symbol placement tool; its Footprint
 * field already names the LCSC footprint.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <tools/sch_tool_base.h>

class SCH_EDIT_FRAME;

class KICADD_LCSC_SCH_TOOL : public SCH_TOOL_BASE<SCH_EDIT_FRAME>
{
public:
    KICADD_LCSC_SCH_TOOL() : SCH_TOOL_BASE<SCH_EDIT_FRAME>( "kicadd.LcscSchTool" ) {}

    int PlaceLcscSymbol( const TOOL_EVENT& aEvent );

    static TOOL_ACTION placeLcscSymbol;

private:
    void setTransitions() override;
};

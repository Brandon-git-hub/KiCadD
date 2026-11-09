/*
 * KiCadD M4d: "Add LCSC Part..." in the PCB Editor (Place menu).
 *
 * Asks for an LCSC number, fetches the part (footprint + symbol into the project's LCSC
 * libraries, see lcsc/kicadd_lcsc.h) and hands the footprint to KiCad's own footprint
 * placement tool, so it follows the cursor and is placed and undone like any other.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <tools/pcb_tool_base.h>

class KICADD_LCSC_TOOL : public PCB_TOOL_BASE
{
public:
    KICADD_LCSC_TOOL();

    void Reset( RESET_REASON aReason ) override {}

    int PlaceLcscPart( const TOOL_EVENT& aEvent );

    static TOOL_ACTION placeLcscPart;

private:
    void setTransitions() override;
};

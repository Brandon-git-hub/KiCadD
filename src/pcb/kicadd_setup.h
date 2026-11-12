/*
 * KiCadD: the board's physical setup as one value (copper layer count, the stackup: board
 * thickness, copper weights, dielectrics), to read, change and put back (board_setup, with the
 * agent's undo). And the fabs' process presets for set_rules.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <layer_ids.h>
#include <lset.h>
#include <board_stackup_manager/board_stackup.h>

#include "pcb/kicadd_rules.h"

class BOARD;

namespace kicadd::setup
{

struct STATE
{
    int           copperLayers = 2;
    LSET          enabled;
    int           thickness = 0;      ///< nm
    bool          hasStackup = false;
    BOARD_STACKUP stackup;

    bool operator==( const STATE& aOther ) const;
};


STATE Capture( BOARD& aBoard );

/// Make the board's setup aState (layers, stackup).
void Apply( BOARD& aBoard, const STATE& aState );


struct CHANGE
{
    std::optional<int>    copperLayers;
    std::optional<int>    thickness;       ///< nm
    std::optional<double> outerOz, innerOz;
};

/// aFrom changed as aChange asks: a stackup made anew for the layer count and thickness, the
/// copper weights set. Fails (aError) when a copper layer that would go has something on it.
std::optional<STATE> Changed( BOARD& aBoard, const STATE& aFrom, const CHANGE& aChange, std::string& aError );

/// One line per stackup layer: "F.Cu copper 0.035 mm", "dielectric 1 core FR4 1.51 mm" ...
std::vector<std::string> Describe( const STATE& aState );

/// The copper weight of a layer in the stackup, oz (0: not in it).
double CopperOz( const STATE& aState, PCB_LAYER_ID aLayer );


/// A fab's standard process (no extra charge) as board minimums, and the Default net class
/// raised to them where it is below: aName "jlcpcb_2layer", "jlcpcb_4layer" ("jlcpcb_6layer").
/// aNote: where the numbers come from. nullopt: no such preset (aNote lists them).
std::optional<kicadd::rules::STATE> Preset( const std::string& aName, const kicadd::rules::STATE& aFrom,
                                            std::string& aNote );

} // namespace kicadd::setup

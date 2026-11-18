/*
 * KiCadD: an EasyEDA 3D model (the OBJ its editor shows, materials inline) as a VRML 2.0 file
 * for the footprint, placed the way EasyEDA places it, so the footprint's own model offset and
 * rotation stay 0. Worked out on the parts' data (both kinds of USB-C, SOT-23, SOT-223: leads on
 * their pads, the shell front on the footprint's outline):
 *
 *   the OBJ is in mm, z up; its box's x / y centre goes on the centre of the footprint's 3D
 *   outline (layer 19), its bottom on the board, then up by the node's z; turned by the node's
 *   c_rotation z, counter-clockwise seen from above
 *
 * KiCad's VRML frame is x right, y up (towards the board's top edge), z up, 1 unit = 2.54 mm.
 * Nothing here knows of KiCad: it can be tested on its own.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <string>

namespace kicadd::obj2wrl
{

struct PLACE
{
    double rotation = 0;   ///< degrees, counter-clockwise from above
    double x = 0;          ///< mm, the model's centre from the footprint's anchor, x right
    double y = 0;          ///< mm, y down (the board's frame)
    double z = 0;          ///< mm, its bottom above the board's top (negative: through it)
};


struct INFO
{
    int    vertices = 0;
    int    faces = 0;
    int    materials = 0;
    double size[3] = { 0, 0, 0 };   ///< mm, the model's box (before turning)
};


/// aObj (EasyEDA's OBJ text) -> aWrl (VRML 2.0). False: aError (no geometry, bad indices).
bool Convert( const std::string& aObj, const PLACE& aPlace, std::string& aWrl, INFO& aInfo, std::string& aError );

} // namespace kicadd::obj2wrl

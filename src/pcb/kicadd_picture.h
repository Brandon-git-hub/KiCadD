/*
 * KiCadD: a picture (PNG) of the board in memory, for the agent to look at.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicadd/board.pb.h>

#include <string>

class BOARD;

namespace kicadd
{

/// Plot @a aBoard's layers into a PNG with the marks asked for. False with @a aError when the
/// request is wrong or the picture could not be made.
bool RenderBoardPng( BOARD* aBoard, const kicadd::board::RenderBoard& aReq,
                     kicadd::board::RenderBoardResponse& aOut, std::string& aError );

} // namespace kicadd

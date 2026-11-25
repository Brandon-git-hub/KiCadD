/*
 * KiCadD MCP - a pin's electrical type for the board tools: the pad's own (the board's netlist),
 * else the open schematic's by reference and pin number. A board made elsewhere (imported, or
 * with an old netlist) may carry no pin types; the stand-alone PCB editor has no schematic to ask,
 * so this lives in kicadd-mcp, which reaches whichever is open (server or desktop). One per tool
 * call: each part's pins are asked once and kept.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_MCP_PINTYPES_H
#define KICADD_MCP_PINTYPES_H

#include "ipc_client.h"

#include <kicadd/schematic.pb.h>

#include <map>
#include <string>

namespace kicadd::mcp
{

class PIN_TYPES
{
public:
    explicit PIN_TYPES( IPC_CLIENT& aIpc ) : m_ipc( aIpc ) {}

    /// aBoardType when the pad has one, else the schematic's ("" when neither knows)
    std::string Of( const std::string& aRef, const std::string& aPin, const std::string& aBoardType = "" )
    {
        if( !aBoardType.empty() )
            return aBoardType;

        auto [it, fresh] = m_parts.try_emplace( aRef );

        if( fresh )
        {
            kicadd::schematic::SchGetPart      q;
            kicadd::schematic::SchPartResponse r;
            std::string                        e;
            q.set_ref( aRef );

            if( m_ipc.Call( q, r, e ) )
            {
                for( const kicadd::schematic::SchPin& sp : r.symbol().pins() )
                    it->second[sp.number()] = sp.type();
            }
        }

        auto pin = it->second.find( aPin );

        if( pin == it->second.end() || pin->second.empty() )
            return {};

        m_fromSchematic = true;
        return pin->second;
    }

    /// whether any type came from the schematic (to say so)
    bool FromSchematic() const { return m_fromSchematic; }

private:
    IPC_CLIENT&                                               m_ipc;
    std::map<std::string, std::map<std::string, std::string>> m_parts;   // ref -> pin -> type
    bool                                                      m_fromSchematic = false;
};

} // namespace kicadd::mcp

#endif

/*
 * KiCadD: the agent's own undo / redo for schematic edits (see kicadd_sch_history.h).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "sch/kicadd_sch_history.h"

#include <sch_item.h>
#include <sch_screen.h>

namespace kicadd
{

COMMIT& KICADD_SCH_COMMIT::Stage( EDA_ITEM* aItem, CHANGE_TYPE aChangeType, BASE_SCREEN* aScreen,
                                  RECURSE_MODE aRecurse )
{
    // the item SCH_COMMIT keeps undo state for (a field or pin: its symbol)
    SCH_ITEM*   item = dynamic_cast<SCH_ITEM*>( aItem ? undoLevelItem( aItem ) : nullptr );
    SCH_SCREEN* screen = dynamic_cast<SCH_SCREEN*>( aScreen );
    const int   type = aChangeType & CHT_TYPE;

    if( item && screen && ( type == CHT_ADD || type == CHT_REMOVE || type == CHT_MODIFY ) )
    {
        REC* rec = nullptr;

        for( REC& r : m_recs )
        {
            if( r.item == item && !r.dropped )
                rec = &r;
        }

        if( !rec )
        {
            REC r{ type == CHT_ADD      ? SCH_CHANGE::ADDED
                   : type == CHT_REMOVE ? SCH_CHANGE::REMOVED
                                        : SCH_CHANGE::MODIFIED,
                   item, screen, item->m_Uuid, nullptr };

            // a removed item (CHT_DONE: off its screen already) is still whole here
            if( type != CHT_ADD )
                r.before.reset( static_cast<SCH_ITEM*>( makeImage( item ) ) );

            m_recs.push_back( std::move( r ) );
        }
        else if( type == CHT_REMOVE )
        {
            // added and removed again: nothing; modified, then removed: removed as it first was
            if( rec->kind == SCH_CHANGE::ADDED )
                rec->dropped = true;
            else
                rec->kind = SCH_CHANGE::REMOVED;
        }
        // added then modified stays an add; modified twice keeps the first before-state
    }

    return SCH_COMMIT::Stage( aItem, aChangeType, aScreen, aRecurse );
}


SCH_EDIT KICADD_SCH_COMMIT::TakeEdit( const std::string& aLabel )
{
    SCH_EDIT edit;
    edit.label = aLabel;

    for( REC& r : m_recs )
    {
        if( r.dropped )
            continue;

        auto change = std::make_shared<SCH_CHANGE>();
        change->kind = r.kind;
        change->id = r.id;
        change->screen = r.screen;
        change->before = std::move( r.before );

        if( r.kind != SCH_CHANGE::REMOVED )
            change->after.reset( static_cast<SCH_ITEM*>( r.item->Clone() ) );

        edit.changes.push_back( std::move( change ) );
    }

    m_recs.clear();
    return edit;
}

} // namespace kicadd

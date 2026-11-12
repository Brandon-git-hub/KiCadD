/*
 * KiCadD: the agent's own undo / redo for schematic edits.
 *
 * KICADD_SCH_COMMIT is the SCH_COMMIT every agent edit goes through. It sees every item staged
 * (including what KiCad's own cleanup stages while the commit is pushed: wires merged or broken,
 * junctions) and keeps copies: what a modified or removed item was before, what an added or
 * modified one is after. SCH_EDIT records that as one history entry; undo / redo apply it in a
 * new commit, item by item and by UUID, leaving alone what was changed since (by the user, in
 * the editor).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <kiid.h>
#include <sch_commit.h>

class SCH_ITEM;
class SCH_SCREEN;

namespace kicadd
{

/// One item of an agent edit: added, removed or modified, with its states.
struct SCH_CHANGE
{
    enum KIND { ADDED, REMOVED, MODIFIED };

    KIND                      kind = MODIFIED;
    KIID                      id;
    SCH_SCREEN*               screen = nullptr;
    std::unique_ptr<SCH_ITEM> before;   ///< removed / modified: as it was (owned, off the screen)
    std::unique_ptr<SCH_ITEM> after;    ///< added / modified: as the edit left it
};


struct SCH_EDIT
{
    std::string                              label;
    KIID                                     root;   ///< the schematic's root sheet: another schematic, another history
    std::vector<std::shared_ptr<SCH_CHANGE>> changes;
};


class KICADD_SCH_COMMIT : public SCH_COMMIT
{
public:
    explicit KICADD_SCH_COMMIT( TOOL_MANAGER* aToolMgr ) : SCH_COMMIT( aToolMgr ) {}

    COMMIT& Stage( EDA_ITEM* aItem, CHANGE_TYPE aChangeType, BASE_SCREEN* aScreen = nullptr,
                   RECURSE_MODE aRecurse = RECURSE_MODE::NO_RECURSE ) override;

    using SCH_COMMIT::Stage;

    /// After Push(): the edit as a history entry (the after-states taken now). Empty when
    /// nothing changed in the end (an item added and removed again).
    SCH_EDIT TakeEdit( const std::string& aLabel );

private:
    struct REC
    {
        SCH_CHANGE::KIND          kind;
        SCH_ITEM*                 item;
        SCH_SCREEN*               screen;
        KIID                      id;
        std::unique_ptr<SCH_ITEM> before;
        bool                      dropped = false;
    };

    std::vector<REC> m_recs;
};

} // namespace kicadd

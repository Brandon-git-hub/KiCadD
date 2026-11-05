/*
 * KiCadD - render snapshot and change journal for remote views.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_RENDER_H
#define KICADD_RENDER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <board.h>
#include <kicadd/render.pb.h>

namespace kicadd
{

/**
 * Listens to a BOARD and records, per top-level item (footprint, track, via, zone), the
 * revision of its last change or removal. Runs on the main thread like all board edits.
 */
class RENDER_FEED : public BOARD_LISTENER
{
public:
    explicit RENDER_FEED( BOARD* aBoard );
    ~RENDER_FEED() override;

    uint64_t Revision() const { return m_revision; }

    void Snapshot( render::RenderSnapshot& aOut ) const;
    void Changes( uint64_t aSince, render::RenderChanges& aOut ) const;

    // BOARD_LISTENER
    void OnBoardItemAdded( BOARD& aBoard, BOARD_ITEM* aItem ) override;
    void OnBoardItemsAdded( BOARD& aBoard, std::vector<BOARD_ITEM*>& aItems ) override;
    void OnBoardItemRemoved( BOARD& aBoard, BOARD_ITEM* aItem ) override;
    void OnBoardItemsRemoved( BOARD& aBoard, std::vector<BOARD_ITEM*>& aItems ) override;
    void OnBoardItemChanged( BOARD& aBoard, BOARD_ITEM* aItem ) override;
    void OnBoardItemsChanged( BOARD& aBoard, std::vector<BOARD_ITEM*>& aItems ) override;
    void OnBoardCompositeUpdate( BOARD& aBoard, std::vector<BOARD_ITEM*>& aAdded,
                                 std::vector<BOARD_ITEM*>& aRemoved,
                                 std::vector<BOARD_ITEM*>& aChanged ) override;

private:
    /// The item a view tracks for aItem (pad -> footprint), or null if not rendered.
    static const BOARD_ITEM* trackedItem( const BOARD_ITEM* aItem );

    void touch( const BOARD_ITEM* aItem );
    void remove( const BOARD_ITEM* aItem );
    void compactRemovals();

    BOARD*   m_board;
    uint64_t m_revision = 1;
    uint64_t m_floor = 0;   ///< Changes(since < m_floor) must resync

    struct ENTRY
    {
        uint64_t revision;
        bool     removed;
    };

    std::unordered_map<std::string, ENTRY> m_journal;   ///< KIID string -> last change
    size_t                                 m_removedCount = 0;
};

} // namespace kicadd

#endif // KICADD_RENDER_H

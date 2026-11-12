/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "history.h"

#include "eventmapper.h"
#include "guiutils.h"
#include "multilangmgr.h"
#include "rtimage.h"

#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;


History::History (bool bookmarkSupport) : historyVPaned (nullptr), bookmarkMenu (nullptr), menuUseCompare (nullptr), menuRestoreBase (nullptr), menuDelete (nullptr), contextPathValid (false), updatingCompareMarker (false), blistener (nullptr), tpc (nullptr), bmnum (1)
{
    set_orientation(Gtk::ORIENTATION_VERTICAL);

    blistenerLock = false; // sets default that the Before preview will not be locked
    /*
        // fill history event message array
        for (int i = 0; i < NUMOFEVENTS; i++) {
            eventDescrArray[i] = M (Glib::ustring::compose ("HISTORY_MSG_%1", i + 1));
        }
    */
    // History List
    // ~~~~~~~~~~~~
    Gtk::ScrolledWindow* hscrollw = Gtk::manage (new Gtk::ScrolledWindow ());
    hscrollw->set_policy (Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);

    Gtk::Frame* histFrame = Gtk::manage (new Gtk::Frame (M ("HISTORY_LABEL")));
    histFrame->set_name ("HistoryPanel");
    histFrame->add (*hscrollw);

    hTreeView = Gtk::manage (new Gtk::TreeView ());
    hscrollw->add (*hTreeView);

    historyModel = Gtk::ListStore::create (historyColumns);
    hTreeView->set_model (historyModel);
    hTreeView->set_headers_visible (false);
    hTreeView->set_hscroll_policy (Gtk::SCROLL_MINIMUM);
    hTreeView->set_vscroll_policy (Gtk::SCROLL_NATURAL);
    hTreeView->set_size_request (80, -1);

    Gtk::CellRendererText *changecrt = Gtk::manage (new Gtk::CellRendererText());
    changecrt->property_ellipsize() = Pango::ELLIPSIZE_END;
    Gtk::CellRendererText *valuecrt  = Gtk::manage (new Gtk::CellRendererText());
    valuecrt->property_ellipsize() = Pango::ELLIPSIZE_END;
    Gtk::TreeView::Column *hviewcol = Gtk::manage (new Gtk::TreeView::Column (""));
    hviewcol->pack_start (*changecrt, true);
    hviewcol->add_attribute (changecrt->property_markup (), historyColumns.text);
    hviewcol->set_expand (true);
    hviewcol->set_resizable (true);
    hviewcol->set_fixed_width (35);
    hviewcol->set_min_width (35);
    hviewcol->set_sizing (Gtk::TREE_VIEW_COLUMN_AUTOSIZE);

    Gtk::TreeView::Column *hviewcol2 = Gtk::manage (new Gtk::TreeView::Column (""));
    hviewcol2->pack_start (*valuecrt, true);
    hviewcol2->add_attribute (valuecrt->property_markup (), historyColumns.value);
    hviewcol2->set_expand (true);
    hviewcol2->set_resizable (true);
    hviewcol2->set_fixed_width (35);
    hviewcol2->set_min_width (35);
    hviewcol2->set_sizing (Gtk::TREE_VIEW_COLUMN_AUTOSIZE);
    valuecrt->set_alignment (1.f, 0.f);

    hTreeView->set_has_tooltip (true);
    hTreeView->signal_query_tooltip().connect ( sigc::mem_fun (*this, &History::on_query_tooltip) );
    hTreeView->append_column (*hviewcol);
    hTreeView->append_column (*hviewcol2);

    selchangehist = hTreeView->get_selection()->signal_changed().connect (sigc::mem_fun (*this, &History::historySelectionChanged));

    // Bookmark List
    // ~~~~~~~~~~~~~

    Gtk::Box* ahbox = Gtk::manage (new Gtk::Box ());
    addBookmark = Gtk::manage (new Gtk::Button ());  // M("HISTORY_NEWSNAPSHOT")
    setExpandAlignProperties (addBookmark, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_START);
    //addBookmark->get_style_context()->set_junction_sides(Gtk::JUNCTION_RIGHT);
    addBookmark->get_style_context()->add_class ("Left");
    addBookmark->set_tooltip_markup (M ("HISTORY_NEWSNAPSHOT_TOOLTIP"));
    Gtk::Image* addimg = Gtk::manage (new RTImage ("add-small", Gtk::ICON_SIZE_BUTTON));
    addBookmark->set_image (*addimg);
    ahbox->pack_start (*addBookmark);

    delBookmark = Gtk::manage (new Gtk::Button ());  // M("HISTORY_DELSNAPSHOT")
    setExpandAlignProperties (delBookmark, true, false, Gtk::ALIGN_FILL, Gtk::ALIGN_START);
    //delBookmark->get_style_context()->set_junction_sides(Gtk::JUNCTION_LEFT);
    delBookmark->get_style_context()->add_class ("Right");
    Gtk::Image* delimg = Gtk::manage (new RTImage ("remove-small", Gtk::ICON_SIZE_BUTTON));
    delBookmark->set_image (*delimg);
    delBookmark->set_sensitive(false);
    ahbox->pack_start (*delBookmark);

    bscrollw = Gtk::manage (new Gtk::ScrolledWindow ());
//    bscrollw->set_policy (Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    bscrollw->set_policy (Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    bscrollw->set_size_request (-1, 45);

    Gtk::Frame* bmFrame = Gtk::manage (new Gtk::Frame (M ("HISTORY_SNAPSHOTS")));
    bmFrame->set_name ("Snapshots");
    Gtk::Box* bmBox = Gtk::manage (new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    bmFrame->add (*bmBox);
    bmBox->pack_start (*bscrollw, Gtk::PACK_EXPAND_WIDGET, 4);
    bmBox->pack_end (*ahbox, Gtk::PACK_SHRINK, 4);
    bmBox->set_size_request (-1, 60);

    if (bookmarkSupport) {
        historyVPaned = Gtk::manage ( new Gtk::Paned (Gtk::ORIENTATION_VERTICAL) );
        historyVPaned->pack1 (*histFrame, true, false);
        historyVPaned->pack2 (*bmFrame, false, false);
        pack_start (*historyVPaned);
    } else {
        pack_start (*histFrame);
    }


    bTreeView = Gtk::manage (new Gtk::TreeView ());
    bscrollw->add (*bTreeView);

    bookmarkModel = Gtk::ListStore::create (bookmarkColumns);
    bTreeView->set_model (bookmarkModel);
    bTreeView->set_headers_visible (false);

    Gtk::CellRendererText* bmarkcrt = Gtk::manage(new Gtk::CellRendererText());
    bmarkcrt->property_xpad() = 2;
    Gtk::TreeView::Column* bmarkcol = Gtk::manage(new Gtk::TreeView::Column(""));
    bmarkcol->pack_start(*bmarkcrt, false);
    bmarkcol->add_attribute(bmarkcrt->property_markup(), bookmarkColumns.marker);
    bmarkcol->set_fixed_width(16);
    bmarkcol->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
    bTreeView->append_column(*bmarkcol);

    Gtk::CellRendererText* bnamecrt = Gtk::manage(new Gtk::CellRendererText());
    bnamecrt->property_ellipsize() = Pango::ELLIPSIZE_END;
    Gtk::TreeView::Column* bnamecol = Gtk::manage(new Gtk::TreeView::Column(M("HISTORY_SNAPSHOTS")));
    bnamecol->pack_start(*bnamecrt, true);
    bnamecol->add_attribute(bnamecrt->property_text(), bookmarkColumns.text);
    bnamecol->add_attribute(bnamecrt->property_editable(), bookmarkColumns.editable);
    bnamecrt->signal_edited().connect(sigc::mem_fun(*this, &History::onBookmarkNameEdited));
    bTreeView->append_column(*bnamecol);

    bTreeView->set_has_tooltip(true);
    bTreeView->signal_query_tooltip().connect(sigc::mem_fun(*this, &History::onBookmarkQueryTooltip));
    bTreeView->signal_button_press_event().connect(sigc::mem_fun(*this, &History::onBookmarkButtonPress), false);

    bookmarkMenu = Gtk::manage(new Gtk::Menu());
    menuUseCompare = Gtk::manage(new Gtk::MenuItem(M("HISTORY_SNAPSHOT_COMPARE_SET")));
    menuRestoreBase = Gtk::manage(new Gtk::MenuItem(M("HISTORY_SNAPSHOT_COMPARE_RESTORE")));
    menuDelete = Gtk::manage(new Gtk::MenuItem(M("HISTORY_SNAPSHOT_DELETE")));
    menuUseCompare->signal_activate().connect(sigc::mem_fun(*this, &History::onMenuUseCompare));
    menuRestoreBase->signal_activate().connect(sigc::mem_fun(*this, &History::onMenuRestoreBase));
    menuDelete->signal_activate().connect(sigc::mem_fun(*this, &History::onMenuDelete));
    bookmarkMenu->append(*menuUseCompare);
    bookmarkMenu->append(*menuRestoreBase);
    bookmarkMenu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
    bookmarkMenu->append(*menuDelete);
    bookmarkMenu->attach_to_widget(*bTreeView);
    bookmarkMenu->show_all();

    selchangebm = bTreeView->get_selection()->signal_changed().connect (sigc::mem_fun (*this, &History::bookmarkSelectionChanged));
    bookmarkModel->signal_row_changed().connect(sigc::mem_fun(*this, &History::onBookmarkRowChanged));

    addBookmark->signal_clicked().connect ( sigc::mem_fun (*this, &History::addBookmarkPressed) );
    delBookmark->signal_clicked().connect ( sigc::mem_fun (*this, &History::delBookmarkPressed) );

    //hTreeView->set_grid_lines (Gtk::TREE_VIEW_GRID_LINES_HORIZONTAL);
    hTreeView->set_grid_lines (Gtk::TREE_VIEW_GRID_LINES_BOTH);
    //hTreeView->signal_size_allocate().connect( sigc::mem_fun(*this, &History::resized) );

    hTreeView->set_enable_search (false);
    bTreeView->set_enable_search (false);

    show_all_children ();
}

void History::initHistory ()
{

    ConnectionBlocker selBlocker (selchangehist);
    historyModel->clear ();
    bookmarkModel->clear ();
    delBookmark->set_sensitive(false);
    compareSnapshotChanged.emit();
}

void History::historySelectionChanged ()
{

    Glib::RefPtr<Gtk::TreeSelection> selection = hTreeView->get_selection();
    Gtk::TreeModel::iterator iter = selection->get_selected();

    if (iter) {
        Gtk::TreeModel::Row row = *iter;

        if (row) {
            bTreeView->get_selection()->unselect_all ();
        }

        if (row && tpc) {
            ProcParams pparams = row[historyColumns.params];
            ParamsEdited pe (true);
            pe.locallab.spots.resize(pparams.locallab.spots.size(), LocallabParamsEdited::LocallabSpotEdited(true));
            PartialProfile pp (&pparams, &pe);
            ParamsEdited paramsEdited = row[historyColumns.paramsEdited];

            tpc->profileChange (&pp, EvHistoryBrowsed, row[historyColumns.text], &paramsEdited);
        }

        if (blistener && !blistenerLock) {
            Gtk::TreeModel::Path path = historyModel->get_path (iter);
            path.prev ();
            iter = historyModel->get_iter (path);

            if (blistener && iter) {
                blistener->historyBeforeLineChanged (iter->get_value (historyColumns.params));
            }
        }
    }
}

void History::bookmarkSelectionChanged ()
{

    Glib::RefPtr<Gtk::TreeSelection> selection = bTreeView->get_selection();
    Gtk::TreeModel::iterator iter = selection->get_selected();

    if (iter) {
        Gtk::TreeModel::Row row = *iter;

        if (row) {
            hTreeView->get_selection()->unselect_all ();
            delBookmark->set_sensitive(row[bookmarkColumns.deletable]);
            delBookmark->set_tooltip_text(
                row[bookmarkColumns.isBase]
                    ? M("SNAPSHOT_COMPARE_BASE_DELETE_REFUSED")
                    : Glib::ustring());
        }

        if (row && tpc) {
            ProcParams pparams = row[bookmarkColumns.params];
            ParamsEdited pe (true);
            pe.locallab.spots.resize(pparams.locallab.spots.size(), LocallabParamsEdited::LocallabSpotEdited(true));
            PartialProfile pp (&pparams, &pe);
            ParamsEdited paramsEdited = row[bookmarkColumns.paramsEdited];
            tpc->profileChange (&pp, EvBookmarkSelected, row[bookmarkColumns.text], &paramsEdited);
        }
    } else {
        delBookmark->set_sensitive(false);
    }
}

void History::procParamsChanged(
    const ProcParams* params,
    const ProcEvent& ev,
    const Glib::ustring& descr,
    const ParamsEdited* paramsEdited
)
{
    // to prevent recursion, we filter out the events triggered by the history and events that should not be registered
    if (ev == EvHistoryBrowsed || ev == EvMonitorTransform || descr == "")  {
        return;
    }

    selchangehist.block(true);
    selchangebm.block(true);

    if (ev == EvPhotoLoaded) {
        initHistory();
    }

    // construct formatted list content

    Glib::RefPtr<Gtk::TreeSelection> selection = hTreeView->get_selection();
    Gtk::TreeModel::iterator iter = selection->get_selected();

    // remove all rows after the selection
    if (iter) {
        ++iter;

        while (iter) {
            iter = historyModel->erase(iter);
        }
    }

    // lookup the last remaining item in the list
    const int size = historyModel->children().size();
    Gtk::TreeModel::Row row;

    if (size > 0) {
        row = historyModel->children()[size - 1];
    }

    // if there is no last item or its chev!=ev, create a new one
    if (size == 0 || !row || row[historyColumns.chev] != ev || ev == EvProfileChanged
            || ev == EvLocallabSpotCreated || ev == EvLocallabSpotDeleted) { // Special cases: If Locallab spot is created , deleted or duplicated several times in a row, a new history row is used
        Gtk::TreeModel::Row newrow = * (historyModel->append());
        newrow[historyColumns.text] = ProcEventMapper::getInstance()->getHistoryMsg(ev);
        newrow[historyColumns.value] = g_markup_escape_text (descr.c_str(), -1);
        newrow[historyColumns.chev] = ev;
        newrow[historyColumns.params] = *params;
        newrow[historyColumns.paramsEdited] = paramsEdited ? *paramsEdited : defParamsEdited;

        if (ev != EvBookmarkSelected) {
            selection->select(newrow);
        }

        if (blistener && row && !blistenerLock) {
            blistener->historyBeforeLineChanged(row[historyColumns.params]);
        } else if (blistener && size == 0 && !blistenerLock) {
            blistener->historyBeforeLineChanged(newrow[historyColumns.params]);
        }
    } else { // else just update it
        row[historyColumns.value] = g_markup_escape_text(descr.c_str(), -1);
        row[historyColumns.params] = *params;
        row[historyColumns.paramsEdited] = paramsEdited ? *paramsEdited : defParamsEdited;

        if (ev != EvBookmarkSelected) {
            selection->select(row);
        }
    }

    if (ev != EvBookmarkSelected) {
        bTreeView->get_selection()->unselect_all();
    }


    if (!selection->get_selected_rows().empty()) {
        std::vector<Gtk::TreeModel::Path> selp = selection->get_selected_rows();
        hTreeView->scroll_to_row(*selp.begin());
    }

    if (ev == EvPhotoLoaded) {
        createBaseSnapshot(*params, paramsEdited ? *paramsEdited : defParamsEdited);
    }

    selchangehist.block(false);
    selchangebm.block(false);
}

void History::clearParamChanges ()
{
    initHistory ();
}

void History::addBookmarkWithText (Glib::ustring text)
{

    // lookup the selected item in the history
    Glib::RefPtr<Gtk::TreeSelection> selection = hTreeView->get_selection();
    Gtk::TreeModel::iterator iter = selection->get_selected();
    Gtk::TreeModel::Row row = *iter;

    if (!row) {
        return;
    }

    // append new row to bookmarks
    Gtk::TreeModel::Row newrow = * (bookmarkModel->append());
    newrow[bookmarkColumns.text] = text;
    ProcParams params = row[historyColumns.params];
    newrow[bookmarkColumns.params] = params;
    ParamsEdited paramsEdited = row[historyColumns.paramsEdited];
    newrow[bookmarkColumns.paramsEdited] = paramsEdited;
    newrow[bookmarkColumns.isBase] = false;
    newrow[bookmarkColumns.isCompareSnapshot] = false;
    newrow[bookmarkColumns.editable] = true;
    newrow[bookmarkColumns.deletable] = true;
    newrow[bookmarkColumns.marker] = Glib::ustring();
}

void History::addBookmarkPressed ()
{

    if (hTreeView->get_selection()->get_selected()) {
        addBookmarkWithText (Glib::ustring::compose ("%1 %2", M ("HISTORY_SNAPSHOT"), bmnum++));
    }
}

Gtk::TreeModel::iterator History::findBaseRow()
{
    for (auto it = bookmarkModel->children().begin(); it != bookmarkModel->children().end(); ++it) {
        if ((*it)[bookmarkColumns.isBase]) {
            return it;
        }
    }

    return bookmarkModel->children().end();
}

Gtk::TreeModel::iterator History::findCompareRow()
{
    for (auto it = bookmarkModel->children().begin(); it != bookmarkModel->children().end(); ++it) {
        if ((*it)[bookmarkColumns.isCompareSnapshot]) {
            return it;
        }
    }

    return bookmarkModel->children().end();
}

void History::setCompareSnapshotInternal(const Gtk::TreeModel::iterator& iter)
{
    if (!iter) {
        return;
    }

    updatingCompareMarker = true;

    for (auto it = bookmarkModel->children().begin(); it != bookmarkModel->children().end(); ++it) {
        const bool selected = (it == iter);
        (*it)[bookmarkColumns.isCompareSnapshot] = selected;
        (*it)[bookmarkColumns.marker] = selected ? Glib::ustring("\u25cf") : Glib::ustring();
    }

    updatingCompareMarker = false;

    compareSnapshotChanged.emit();
}

void History::setCompareSnapshot(const Gtk::TreeModel::iterator& iter)
{
    setCompareSnapshotInternal(iter);
}

void History::restoreBaseComparison()
{
    Gtk::TreeModel::iterator base = findBaseRow();

    if (base) {
        setCompareSnapshotInternal(base);
    }
}

void History::createBaseSnapshot(const ProcParams& params, const ParamsEdited& paramsEdited)
{
    Gtk::TreeModel::iterator base = findBaseRow();
    Gtk::TreeModel::Row row;

    if (base) {
        row = *base;
    } else {
        base = bookmarkModel->prepend();
        row = *base;
    }

    row[bookmarkColumns.text] = M("HISTORY_SNAPSHOT_BASE");
    row[bookmarkColumns.params] = params;
    row[bookmarkColumns.paramsEdited] = paramsEdited;
    row[bookmarkColumns.isBase] = true;
    row[bookmarkColumns.isCompareSnapshot] = false;
    row[bookmarkColumns.editable] = false;
    row[bookmarkColumns.deletable] = false;
    row[bookmarkColumns.marker] = Glib::ustring();

    setCompareSnapshotInternal(base);
}

bool History::getCompareSnapshotParams(ProcParams& params, Glib::ustring& name)
{
    Gtk::TreeModel::iterator iter = findCompareRow();

    if (iter == bookmarkModel->children().end()) {
        return false;
    }

    params = (*iter)[bookmarkColumns.params];
    name = (*iter)[bookmarkColumns.text];
    return true;
}

bool History::deleteBookmark(const Gtk::TreeModel::iterator& iter)
{
    if (!iter) {
        return false;
    }

    const Gtk::TreeModel::Row row = *iter;

    if (!row[bookmarkColumns.deletable]) {
        return false;
    }

    if (row[bookmarkColumns.isCompareSnapshot]) {
        restoreBaseComparison();
    }

    bookmarkModel->erase(iter);
    delBookmark->set_sensitive(false);
    return true;
}

void History::delBookmarkPressed ()
{

    // lookup the selected item in the bookmark
    Glib::RefPtr<Gtk::TreeSelection> selection = bTreeView->get_selection();
    Gtk::TreeModel::iterator iter = selection->get_selected();

    if (!iter || !deleteBookmark(iter)) {
        return;
    }

    // select last item in history
    int size = historyModel->children().size ();

    if (size > 0) {
        Gtk::TreeModel::Row row = historyModel->children()[size - 1];
        hTreeView->get_selection()->select (row);
    }
}

bool History::onBookmarkButtonPress(GdkEventButton* event)
{
    if (event->type != GDK_BUTTON_PRESS || (event->button != 1 && event->button != 3)) {
        return false;
    }

    Gtk::TreeModel::Path path;
    Gtk::TreeView::Column* column = nullptr;
    int cellX = 0;
    int cellY = 0;

    if (!bTreeView->get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y), path, column, cellX, cellY)) {
        contextPathValid = false;
        return false;
    }

    Gtk::TreeModel::iterator iter = bookmarkModel->get_iter(path);

    if (!iter) {
        contextPathValid = false;
        return false;
    }

    if (event->button == 1 && (event->state & GDK_CONTROL_MASK)) {
        setCompareSnapshot(iter);
        return true;
    }

    if (event->button == 3) {
        contextPath = path;
        contextPathValid = true;
        const Gtk::TreeModel::Row row = *iter;
        const bool hasBase = findBaseRow() != bookmarkModel->children().end();
        menuUseCompare->set_sensitive(!row[bookmarkColumns.isCompareSnapshot]);
        menuRestoreBase->set_sensitive(hasBase && !row[bookmarkColumns.isBase]);
        menuDelete->set_sensitive(row[bookmarkColumns.deletable]);
        bookmarkMenu->popup(event->button, event->time);
        return true;
    }

    return false;
}

void History::onBookmarkRowChanged(const Gtk::TreeModel::Path&, const Gtk::TreeModel::iterator& iter)
{
    if (!updatingCompareMarker && iter && (*iter)[bookmarkColumns.isCompareSnapshot]) {
        compareSnapshotChanged.emit();
    }
}

void History::onBookmarkNameEdited(const Glib::ustring& path, const Glib::ustring& newText)
{
    Gtk::TreeModel::iterator iter = bookmarkModel->get_iter(Gtk::TreeModel::Path(path));

    if (iter && (*iter)[bookmarkColumns.editable]) {
        (*iter)[bookmarkColumns.text] = newText;
    }
}

void History::onMenuUseCompare()
{
    if (contextPathValid) {
        Gtk::TreeModel::iterator iter = bookmarkModel->get_iter(contextPath);

        if (iter) {
            setCompareSnapshot(iter);
        }
    }
}

void History::onMenuRestoreBase()
{
    restoreBaseComparison();
}

void History::onMenuDelete()
{
    if (!contextPathValid) {
        return;
    }

    Gtk::TreeModel::iterator iter = bookmarkModel->get_iter(contextPath);

    if (iter && deleteBookmark(iter)) {
        int size = historyModel->children().size();

        if (size > 0) {
            hTreeView->get_selection()->select(historyModel->children()[size - 1]);
        }
    }
}

bool History::onBookmarkQueryTooltip(int x, int y, bool keyboard_tooltip, const Glib::RefPtr<Gtk::Tooltip>& tooltip)
{
    Gtk::TreeModel::Path path;
    int x2 = -1;
    int y2 = -1;
    bTreeView->convert_widget_to_bin_window_coords(x, y, x2, y2);

    if (!bTreeView->get_path_at_pos(x2, y2, path)) {
        return false;
    }

    Gtk::TreeModel::iterator iter = bookmarkModel->get_iter(path);

    if (!iter) {
        return false;
    }

    const Gtk::TreeModel::Row row = *iter;

    if (row[bookmarkColumns.isBase]) {
        tooltip->set_text(M("HISTORY_SNAPSHOT_BASE_TOOLTIP"));
        return true;
    }

    if (row[bookmarkColumns.isCompareSnapshot]) {
        tooltip->set_text(M("HISTORY_SNAPSHOT_COMPARE_MARKER"));
        return true;
    }

    return false;
}

void History::undo ()
{

    Glib::RefPtr<Gtk::TreeSelection> selection = hTreeView->get_selection();
    Gtk::TreeModel::iterator iter = selection->get_selected();

    if (iter && iter != historyModel->children().begin()) {
        selection->select (--iter);
    } else if (!iter) {
        int size = historyModel->children().size ();

        if (size > 1) {
            selection->select (historyModel->children().operator [] (size - 2));
        }
    }
}

void History::redo ()
{

    Glib::RefPtr<Gtk::TreeSelection> selection = hTreeView->get_selection();
    Gtk::TreeModel::iterator iter = selection->get_selected();

    if (iter) {
        ++iter;

        if (iter != historyModel->children().end()) {
            selection->select (iter);
        }
    } else {
        int size = historyModel->children().size ();

        if (size > 1) {
            selection->select (historyModel->children().operator [] (size - 2));
        }
    }
}

/*
void History::resized (Gtk::Allocation& req)
{
}
*/

bool History::getBeforeLineParams (rtengine::procparams::ProcParams& params)
{

    int size = historyModel->children().size ();

    if (size == 0 || !blistener) {
        return false;
    }

    Gtk::TreeModel::Row row;
    row = historyModel->children()[size == 1 ? 0 : size - 2];
    params = row[historyColumns.params];
    return true;
}

bool History::on_query_tooltip (int x, int y, bool keyboard_tooltip, const Glib::RefPtr<Gtk::Tooltip>& tooltip)
{
    bool displayTooltip = false;

    Gtk::TreeModel::Path path;
    int x2 = -1;
    int y2 = -1;
    hTreeView->convert_widget_to_bin_window_coords (x, y, x2, y2);
    bool hasPath = hTreeView->get_path_at_pos (x2, y2, path);

    if (hasPath) {
        if (path && !path.empty()) {
            Gtk::TreeModel::iterator iter = historyModel->get_iter (path);

            if (iter) {
//                Glib::ustring param, val;
//                iter->get_value<Glib::ustring> (1, param);
//                iter->get_value<Glib::ustring> (2, val);
                Glib::ustring text, val;
                iter->get_value<Glib::ustring> (0, text);
                iter->get_value<Glib::ustring> (1, val);

                /*
                 *
                 *
                 * Why this doesn't work ???
                 *
                 *
                 *
                Gtk::Label *left = Gtk::manage (new Gtk::Label(param+" :"));
                Gtk::Label *right = Gtk::manage (new Gtk::Label(val));
                right->set_justify(Gtk::JUSTIFY_LEFT);
                Gtk::Box *hbox = Gtk::manage (new Gtk::Box());
                hbox->set_spacing(5);
                hbox->pack_start(*left, Gtk::PACK_SHRINK, 0);
                hbox->pack_start(*right, Gtk::PACK_SHRINK, 0);
                tooltip->set_custom(*hbox);
                */

//               tooltip->set_text (param + " : " + val);
                tooltip->set_text (text + " : " + val);
                displayTooltip = true;
            }
        }
    }

    return displayTooltip;
}

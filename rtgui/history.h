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
#pragma once

#include <gtkmm.h>

#include "paramsedited.h"
#include "pparamschangelistener.h"
#include "profilechangelistener.h"

#include "rtengine/rtengine.h"

class HistoryBeforeLineListener
{
public:
    virtual ~HistoryBeforeLineListener() = default;
    virtual void historyBeforeLineChanged(const rtengine::procparams::ProcParams& params) = 0;
};

class History : public Gtk::Box, public PParamsChangeListener
{

public:

    class HistoryColumns : public Gtk::TreeModel::ColumnRecord
    {
    public:
        Gtk::TreeModelColumn<Glib::ustring>  text;
        Gtk::TreeModelColumn<Glib::ustring>  value;
        Gtk::TreeModelColumn<rtengine::procparams::ProcParams>     params;
        Gtk::TreeModelColumn<rtengine::ProcEvent>    chev;
        Gtk::TreeModelColumn<ParamsEdited>     paramsEdited;
        HistoryColumns()
        {
            add(text);
            add(value);
            add(chev);
            add(params);
            add(paramsEdited);
        }
    };
    HistoryColumns historyColumns;
    class BookmarkColumns : public Gtk::TreeModel::ColumnRecord
    {
    public:
        Gtk::TreeModelColumn<Glib::ustring>  text;
        Gtk::TreeModelColumn<rtengine::procparams::ProcParams>     params;
        Gtk::TreeModelColumn<ParamsEdited>     paramsEdited;
        Gtk::TreeModelColumn<bool> isBase;
        Gtk::TreeModelColumn<bool> isCompareSnapshot;
        Gtk::TreeModelColumn<bool> editable;
        Gtk::TreeModelColumn<bool> deletable;
        Gtk::TreeModelColumn<Glib::ustring> marker;
        BookmarkColumns()
        {
            add(text);
            add(params);
            add(paramsEdited);
            add(isBase);
            add(isCompareSnapshot);
            add(editable);
            add(deletable);
            add(marker);
        }
    };
    BookmarkColumns bookmarkColumns;

protected:
    Gtk::Paned*            historyVPaned;
    Gtk::TreeView*          hTreeView;
    Glib::RefPtr<Gtk::ListStore> historyModel;

    Gtk::ScrolledWindow*    bscrollw;
    Gtk::TreeView*          bTreeView;
    Glib::RefPtr<Gtk::ListStore> bookmarkModel;

    Gtk::Button*            addBookmark;
    Gtk::Button*            delBookmark;

    Gtk::Menu*              bookmarkMenu;
    Gtk::MenuItem*          menuUseCompare;
    Gtk::MenuItem*          menuRestoreBase;
    Gtk::MenuItem*          menuDelete;
    Gtk::TreeModel::Path    contextPath;
    bool                    contextPathValid;
    bool                    updatingCompareMarker;

    sigc::connection        selchangehist;
    sigc::connection        selchangebm;

    HistoryBeforeLineListener * blistener;
    ProfileChangeListener* tpc;
    ParamsEdited defParamsEdited;
    int bmnum;

    bool on_query_tooltip(int x, int y, bool keyboard_tooltip, const Glib::RefPtr<Gtk::Tooltip>& tooltip);
    bool onBookmarkButtonPress(GdkEventButton* event);
    bool onBookmarkQueryTooltip(int x, int y, bool keyboard_tooltip, const Glib::RefPtr<Gtk::Tooltip>& tooltip);
    void onBookmarkRowChanged(const Gtk::TreeModel::Path& path, const Gtk::TreeModel::iterator& iter);
    void onBookmarkNameEdited(const Glib::ustring& path, const Glib::ustring& newText);
    void onMenuUseCompare();
    void onMenuRestoreBase();
    void onMenuDelete();
    Gtk::TreeModel::iterator findBaseRow();
    Gtk::TreeModel::iterator findCompareRow();
    void setCompareSnapshotInternal(const Gtk::TreeModel::iterator& iter);
    bool deleteBookmark(const Gtk::TreeModel::iterator& iter);

public:

    explicit History (bool bookmarkSupport = true);

    void setProfileChangeListener     (ProfileChangeListener* tpc_)
    {
        tpc = tpc_;
    }
    void setHistoryBeforeLineListener (HistoryBeforeLineListener* bll)
    {
        blistener = bll;
    }

    // pparamschangelistener interface
    void procParamsChanged(
        const rtengine::procparams::ProcParams* params,
        const rtengine::ProcEvent& ev,
        const Glib::ustring& descr,
        const ParamsEdited* paramsEdited = nullptr
    ) override;
    void clearParamChanges () override;

    void historySelectionChanged ();
    void bookmarkSelectionChanged ();
    void initHistory ();

    sigc::signal<void> compareSnapshotChanged;

    void createBaseSnapshot(
        const rtengine::procparams::ProcParams& params,
        const ParamsEdited& paramsEdited
    );
    bool getCompareSnapshotParams(
        rtengine::procparams::ProcParams& params,
        Glib::ustring& name
    );
    bool compareSnapshotIsBase();
    bool isAtLoadState() const;
    void setCompareSnapshot(const Gtk::TreeModel::iterator& iter);
    void restoreBaseComparison();

    bool getBeforeLineParams (rtengine::procparams::ProcParams& params);

    void addBookmarkWithText (Glib::ustring text);
    void addBookmarkPressed ();
    void delBookmarkPressed ();

    //void resized (Gtk::Allocation& req);

    void undo ();
    void redo ();

    bool blistenerLock;
    void resetSnapShotNumber()
    {
        bmnum = 1;
    };
};

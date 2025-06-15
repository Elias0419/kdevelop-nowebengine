/*
    SPDX-FileCopyrightText: 2006-2009 Alexander Dymo <adymo@kdevelop.org>
    SPDX-FileCopyrightText: 2012 Dominik Haumann <dhaumann@kde.org>
    SPDX-FileCopyrightText: 2020 Friedrich W. H. Kossebau <kossebau@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "mainwindow_p.h"

#include <QLayout>
#include <QSplitter>
#include <QApplication>
#include <QMenuBar>
#include <QToolBar>
#include <QKeyEvent>

#include <KActionMenu>
#include <KActionCollection>
#include <KLocalizedString>

#include "area.h"
#include "view.h"
#include "areaindex.h"
#include "document.h"
#include "container.h"
#include "controller.h"
#include "mainwindow.h"
#include "viewbarcontainer.h"
#include "idealcontroller.h"
#include "holdupdates.h"
#include "message.h"
#include "messagewidget.h"
#include <debug.h>

namespace Sublime {

MainWindowPrivate::MainWindowPrivate(MainWindow *w, Controller* controller)
    : controller(controller)
    , area(nullptr)
    , activeView(nullptr)
    , activeToolView(nullptr)
    , bgCentralWidget(nullptr)
    , autoAreaSettingsSave(false)
    , m_mainWindow(w)
{
    KActionCollection *ac = m_mainWindow->actionCollection();

    m_concentrationModeAction = new QAction(i18nc("@option:check", "Concentration Mode"), this);
    m_concentrationModeAction->setIcon(QIcon::fromTheme(QStringLiteral("page-zoom")));
    m_concentrationModeAction->setToolTip(i18nc("@info:tooltip", "Remove most of the controls so you can focus on what matters"));
    m_concentrationModeAction->setCheckable(true);
    m_concentrationModeAction->setChecked(false);
    ac->setDefaultShortcut(m_concentrationModeAction, Qt::META | Qt::Key_C);
    connect(m_concentrationModeAction, &QAction::toggled, this, &MainWindowPrivate::toggleConcentrationMode);
    ac->addAction(QStringLiteral("toggle_concentration_mode"), m_concentrationModeAction);

    // IdealController makes checkable and connects to the Show Dock actions

    auto* action = new QAction(i18nc("@option:check", "Show Left Dock"), this);
    ac->setDefaultShortcut(action, Qt::META | Qt::CTRL | Qt::Key_Left);
    ac->addAction(QStringLiteral("show_left_dock"), action);

    action = new QAction(i18nc("@option:check", "Show Right Dock"), this);
    ac->setDefaultShortcut(action, Qt::META | Qt::CTRL | Qt::Key_Right);
    ac->addAction(QStringLiteral("show_right_dock"), action);

    action = new QAction(i18nc("@option:check", "Show Bottom Dock"), this);
    ac->setDefaultShortcut(action, Qt::META | Qt::CTRL | Qt::Key_Down);
    ac->addAction(QStringLiteral("show_bottom_dock"), action);

    action = new QAction(i18nc("@action", "Focus Editor"), this);
    ac->setDefaultShortcut(action, Qt::META | Qt::CTRL | Qt::Key_E);
    connect(action, &QAction::triggered, m_mainWindow, &MainWindow::focusEditor);
    ac->addAction(QStringLiteral("focus_editor"), action);

    action = new QAction(i18nc("@action", "Hide/Restore Docks"), this);
    ac->setDefaultShortcut(action, Qt::META | Qt::CTRL | Qt::Key_Up);
    connect(action, &QAction::triggered, this, &MainWindowPrivate::toggleDocksShown);
    ac->addAction(QStringLiteral("hide_all_docks"), action);

    action = new QAction(i18nc("@action", "Next Tool View"), this);
    ac->setDefaultShortcut(action, Qt::META | Qt::CTRL | Qt::Key_N);
    action->setIcon(QIcon::fromTheme(QStringLiteral("go-next")));
    connect(action, &QAction::triggered, this, &MainWindowPrivate::selectNextDock);
    ac->addAction(QStringLiteral("select_next_dock"), action);

    action = new QAction(i18nc("@action", "Previous Tool View"), this);
    ac->setDefaultShortcut(action, Qt::META | Qt::CTRL | Qt::Key_P);
    action->setIcon(QIcon::fromTheme(QStringLiteral("go-previous")));
    connect(action, &QAction::triggered, this, &MainWindowPrivate::selectPreviousDock);
    ac->addAction(QStringLiteral("select_previous_dock"), action);

    auto* const toolViewsMenu = new KActionMenu(i18nc("@title:menu", "Tool Views"), this);
    toolViewsMenu->setPopupMode(QToolButton::InstantPopup);
    ac->addAction(QStringLiteral("docks_submenu"), toolViewsMenu);

    idealController = new IdealController(m_mainWindow);

    connect(controller, &Controller::toolViewRemoved, idealController, &IdealController::toolViewRemoved);

    centralWidget = new QWidget;
    centralWidget->setObjectName(QStringLiteral("centralWidget"));
    auto* layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    centralWidget->setLayout(layout);

    messageWidget = new MessageWidget();
    layout->addWidget(messageWidget);

    splitterCentralWidget = new QSplitter(centralWidget);
    // take as much space as possible
    splitterCentralWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(splitterCentralWidget, 2);

    // this view bar container is used for the ktexteditor integration to show
    // all view bars at a central place, esp. for split view configurations
    viewBarContainer = new ViewBarContainer;
    viewBarContainer->setObjectName(QStringLiteral("viewBarContainer"));
    // hide by default
    viewBarContainer->setVisible(false);
    // only take as much as needed
    viewBarContainer->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    layout->addWidget(viewBarContainer);

    m_mainWindow->setCentralWidget(centralWidget);

    connect(idealController, &IdealController::dockBarContextMenuRequested,
            m_mainWindow, &MainWindow::dockBarContextMenuRequested);
}


MainWindowPrivate::~MainWindowPrivate()
{
    // create working copy as messages are auto-removing themselves from the hash on destruction
    const auto messages = m_messageHash.keys();
    qDeleteAll(messages);
}

void MainWindowPrivate::adaptToDockWidgetVisibilities()
{
    qCDebug(SUBLIME) << "adapting to dock widget visibilities";

    Q_ASSERT(m_mainWindow->isVisible());
    waitingToAdaptToDockWidgetVisibilities = false;
    idealController->adaptToDockWidgetVisibilities();

    emit m_mainWindow->toolViewVisibilityRestored(idealController->shownViews());
}

void MainWindowPrivate::disableConcentrationMode()
{
    m_concentrationModeAction->setChecked(false);
}

void MainWindowPrivate::toggleConcentrationMode(bool concentrationModeOn)
{
    QWidget* cornerWidget = nullptr;
    if (m_concentrateToolBar) {
        QLayout* l = m_concentrateToolBar->layout();
        QLayoutItem* li = l->takeAt(1); //ensure the cornerWidget isn't destroyed with the toolbar
        if (li) {
            cornerWidget = li->widget();
            delete li;
        }

        m_concentrateToolBar->deleteLater();
    }

    m_mainWindow->menuBar()->setVisible(!concentrationModeOn);

    if (concentrationModeOn) {
        m_concentrateToolBar = new QToolBar(m_mainWindow);
        m_concentrateToolBar->setObjectName(QStringLiteral("concentrateToolBar"));
        m_concentrateToolBar->addAction(m_concentrationModeAction);
        m_concentrateToolBar->toggleViewAction()->setVisible(false);
        auto *action = new QWidgetAction(this);

        action->setDefaultWidget(m_mainWindow->menuBar()->cornerWidget(Qt::TopRightCorner));
        m_concentrateToolBar->addAction(action);
        m_concentrateToolBar->setMovable(false);

        m_mainWindow->addToolBar(Qt::TopToolBarArea, m_concentrateToolBar);
        m_mainWindow->menuBar()->setCornerWidget(nullptr, Qt::TopRightCorner);
    } else if (cornerWidget) {
        m_mainWindow->menuBar()->setCornerWidget(cornerWidget, Qt::TopRightCorner);
        cornerWidget->show();
    }

    if (concentrationModeOn) {
        idealController->hideToolBars();
        m_mainWindow->installEventFilter(this);
    } else {
        idealController->updateToolBarVisibility();
        m_mainWindow->removeEventFilter(this);
    }
}

bool MainWindowPrivate::eventFilter(QObject* obj, QEvent* event)
{
    Q_ASSERT(m_mainWindow == obj);
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        const auto ev = static_cast<QKeyEvent *>(event);
        Qt::KeyboardModifiers modifiers = ev->modifiers();

        //QLineEdit banned mostly so that alt navigation can be used from QuickOpen
        const bool visible = modifiers == Qt::AltModifier && ev->type() == QEvent::KeyPress && !qApp->focusWidget()->inherits("QLineEdit");
        m_mainWindow->menuBar()->setVisible(visible);
    }

    return false;
}

void MainWindowPrivate::setBackgroundCentralWidget(QWidget* w)
{
    delete bgCentralWidget;

    bgCentralWidget = w;

    if (bgCentralWidget) {
        auto* l = static_cast<QVBoxLayout*>(centralWidget->layout());
        l->addWidget(bgCentralWidget, 2);
        setBackgroundVisible(area->views().isEmpty());
    }
}

void MainWindowPrivate::setBackgroundVisible(bool v)
{
    if(!bgCentralWidget)
        return;

    bgCentralWidget->setVisible(v);
    splitterCentralWidget->setVisible(!v);
}

void MainWindowPrivate::toggleDocksShown()
{
    idealController->toggleDocksShown();
}

void MainWindowPrivate::selectNextDock()
{
    idealController->goPrevNextDock(IdealController::NextDock);
}

void MainWindowPrivate::selectPreviousDock()
{
    idealController->goPrevNextDock(IdealController::PrevDock);
}

Area::WalkerMode MainWindowPrivate::IdealToolViewCreator::operator() (View *view, Sublime::Position position)
{
    if (!d->docks.contains(view))
    {
        d->docks << view;

        //add view
        d->idealController->addView(d->positionToDockArea(position), view);
    }
    return Area::ContinueWalker;
}

Area::WalkerMode MainWindowPrivate::ViewCreator::operator() (AreaIndex *index)
{
    QSplitter *splitter = d->m_indexSplitters.value(index);
    if (!splitter)
    {
        //no splitter - we shall create it and populate with views
        if (!index->parent())
        {
            qCDebug(SUBLIME) << "reconstructing root area";
            //this is root area
            splitter = d->splitterCentralWidget;
            d->m_indexSplitters[index] = splitter;
        }
        else
        {
            if (!d->m_indexSplitters.value(index->parent())) {
                // can happen in working set code, as that adds a view to a child index first
                // hence, recursively reconstruct the parent indices first
                operator()(index->parent());
            }
            QSplitter *parent = d->m_indexSplitters.value(index->parent());
            splitter = new QSplitter(parent);
            d->m_indexSplitters[index] = splitter;

            if(index == index->parent()->first())
                parent->insertWidget(0, splitter);
            else
                parent->addWidget(splitter);
        }
        Q_ASSERT(splitter);
    }

    if (index->isSplit()) //this is a visible splitter
        splitter->setOrientation(index->orientation());
    else
    {
        Container *container = nullptr;

        while(splitter->count() && qobject_cast<QSplitter*>(splitter->widget(0)))
        {
            // After unsplitting, we might have to remove old splitters
            QWidget* widget = splitter->widget(0);
            qCDebug(SUBLIME) << "deleting" << widget;
            widget->setParent(nullptr);
            delete widget;
        }

        if (!splitter->widget(0))
        {
            //we need to create view container
            container = new Container(splitter);
            connect(container, &Container::activateView,
                    d->m_mainWindow, &MainWindow::activateViewAndFocus);
            connect(container, &Container::tabDoubleClicked,
                    d->m_mainWindow, &MainWindow::tabDoubleClicked);
            connect(container, &Container::tabContextMenuRequested,
                    d->m_mainWindow, &MainWindow::tabContextMenuRequested);
            connect(container, &Container::tabToolTipRequested,
                    d->m_mainWindow, &MainWindow::tabToolTipRequested);
            connect(container, QOverload<QWidget*>::of(&Container::requestClose),
                    d, &MainWindowPrivate::widgetCloseRequest, Qt::QueuedConnection);
            connect(container, &Container::newTabRequested,
                    d->m_mainWindow, &MainWindow::newTabRequested);
            splitter->addWidget(container);
        }
        else
            container = qobject_cast<Container*>(splitter->widget(0));
        container->show();

        int position = 0;
        bool hadActiveView = false;
        Sublime::View* activeView = d->activeView;

        for (View* view : std::as_const(index->views())) {
            auto* widget = view->widget();
            if (!widget) {
                widget = view->initializeWidget(container);
            }

            if (widget)
            {
                if(!container->hasWidget(widget))
                {
                    container->addWidget(view, widget, position);
                    d->viewContainers[view] = container;
                    d->widgetToView[widget] = view;
                }
                if(activeView == view)
                {
                    hadActiveView = true;
                    container->setCurrentWidget(widget);
                }else if(topViews.contains(view) && !hadActiveView)
                    container->setCurrentWidget(widget);
            }
            position++;
        }
    }
    return Area::ContinueWalker;
}

void MainWindowPrivate::reconstructViews(const QList<View*>& topViews)
{
    ViewCreator viewCreator(this, topViews);
    area->walkViews(viewCreator, area->rootIndex());
    setBackgroundVisible(area->views().isEmpty());
}

void MainWindowPrivate::reconstruct()
{
    qCDebug(SUBLIME) << "reconstructing area" << area;

    IdealToolViewCreator toolViewCreator(this);
    area->walkToolViews(toolViewCreator, Sublime::AllPositions);

    reconstructViews();
}

void MainWindowPrivate::clearArea()
{
    qCDebug(SUBLIME) << "clearing area" << area;

    //reparent tool view widgets to nullptr to prevent their deletion together with dockwidgets
    for (View* view : std::as_const(area->toolViews())) {
        // FIXME should we really delete here??
        bool nonDestructive = true;
        idealController->removeView(view, nonDestructive);
    }

    docks.clear();

    //reparent all view widgets to 0 to prevent their deletion together with central
    //widget. this reparenting is necessary when switching areas inside the same mainwindow
    const auto views = area->views();
    for (const auto* const view : views) {
        if (auto* const widget = view->widget()) {
            widget->setParent(nullptr);
        }
    }
    cleanCentralWidget();
    m_mainWindow->setActiveView(nullptr);
    m_indexSplitters.clear();
    area = nullptr;
    viewContainers.clear();
}

void MainWindowPrivate::cleanCentralWidget()
{
    while(splitterCentralWidget->count())
        delete splitterCentralWidget->widget(0);

    setBackgroundVisible(true);
}

void MainWindowPrivate::viewRemovedInternal(AreaIndex* index, View* view)
{
    Q_UNUSED(index);
    Q_UNUSED(view);
    setBackgroundVisible(area->views().isEmpty());
}

void MainWindowPrivate::viewAdded(Sublime::AreaIndex *index, Sublime::View *view)
{
    // Remove container objects in the hierarchy from the parents,
    // because they are not needed anymore, and might lead to broken splitter hierarchy and crashes.
    for(Sublime::AreaIndex* current = index; current; current = current->parent())
    {
        QSplitter *splitter = m_indexSplitters[current];
        if (current->isSplit() && splitter)
        {
            // Also update the orientation
            splitter->setOrientation(current->orientation());

            for(int w = 0; w < splitter->count(); ++w)
            {
                auto *container = qobject_cast<Sublime::Container*>(splitter->widget(w));
                //we need to remove extra container before reconstruction
                //first reparent widgets in container so that they are not deleted
                if(container)
                {
                    while (container->count())
                    {
                        container->widget(0)->setParent(nullptr);
                    }
                    //and then delete the container
                    delete container;
                }
            }
        }
    }

    ViewCreator viewCreator(this);
    area->walkViews(viewCreator, index);
    emit m_mainWindow->viewAdded( view );

    setBackgroundVisible(false);
}

void Sublime::MainWindowPrivate::raiseToolView(Sublime::View * view)
{
    idealController->raiseView(view);
    // Raising a tool view shows it in the background and avoids giving it the focus.
    // Treat raising as activation and update the active tool view accordingly.
    // FIXME: Unfortunately, this manual activation produces inconsistent results (that depend on which particular
    // tool view is raised) when another tool view has focus. In this scenario, raising an output tool view (such
    // as Build) makes it the active tool view; but raising the Find/Replace in Files tool view first makes it
    // the active tool view, then transfers the focus back and thus activates the previously focused tool view.
    m_mainWindow->setActiveToolView(view);
}

void MainWindowPrivate::aboutToRemoveView(Sublime::AreaIndex *index, Sublime::View *view)
{
    QSplitter *splitter = m_indexSplitters[index];
    if (!splitter)
        return;

    qCDebug(SUBLIME) << "index " << index << " root " << area->rootIndex();
    qCDebug(SUBLIME) << "splitter " << splitter << " container " << splitter->widget(0);
    qCDebug(SUBLIME) << "structure: " << index->print() << " whole structure: " << area->rootIndex()->print();
    //find the container for the view and remove the widget
    auto *container = qobject_cast<Container*>(splitter->widget(0));
    if (!container) {
        qCWarning(SUBLIME) << "Splitter does not have a left widget!";
        return;
    }

    emit m_mainWindow->aboutToRemoveView( view );

    viewContainers.remove(view);

    const bool wasActive = m_mainWindow->activeView() == view;

    if (auto* const viewWidget = view->widget()) {
        widgetToView.remove(viewWidget);
        container->removeWidget(viewWidget);
        viewWidget->setParent(nullptr);
    } else {
        qCWarning(SUBLIME) << "the view" << view->document()->documentSpecifier() << "does not have a widget";
    }

    if (container->count() > 0) {
        // container is not empty or this is a root index, so keep the container

        //activate what is visible currently in the container if the removed view was active
        if (wasActive) {
            m_mainWindow->setActiveView(container->viewForWidget(container->currentWidget()));
            return;
        }
    }
    else
    {
        // We have just removed the last view from this container. The container is now empty, so destroy it as well.

        // If we have a container, then it should be the only child of
        // the splitter.
        Q_ASSERT(splitter->count() == 1);

        // We can be called from signal handler of container
        // (which is tab widget), so defer deleting it.
        container->deleteLater();
        container->setParent(nullptr);

        /* If we're not at the top level, we get to collapse split views.  */
        if (index->parent())
        {
            /* The splitter used to have container as the only child, now it's
               time to get rid of it.  Make sure deleting splitter does not
               delete container -- per above comment, we'll delete it later.  */
            container->setParent(nullptr);
            m_indexSplitters.remove(index);
            delete splitter;

            AreaIndex *parent = index->parent();
            QSplitter *parentSplitter = m_indexSplitters[parent];

            AreaIndex *sibling = parent->first() == index ? parent->second() : parent->first();
            QSplitter *siblingSplitter = m_indexSplitters[sibling];

            if(siblingSplitter)
            {
                HoldUpdates du(parentSplitter);
                //save sizes and orientation of the sibling splitter
                parentSplitter->setOrientation(siblingSplitter->orientation());
                QList<int> sizes = siblingSplitter->sizes();

                /* Parent has two children -- 'index' that we've deleted and
                'sibling'.  We move all children of 'sibling' into parent,
                and delete 'sibling'.  sibling either contains a single
                Container instance, or a bunch of further QSplitters.  */
                while (siblingSplitter->count() > 0)
                {
                    //reparent contents into parent splitter
                    QWidget *siblingWidget = siblingSplitter->widget(0);
                    siblingWidget->setParent(parentSplitter);
                    parentSplitter->addWidget(siblingWidget);
                }

                m_indexSplitters.remove(sibling);
                delete siblingSplitter;
                parentSplitter->setSizes(sizes);
            }

            qCDebug(SUBLIME) << "after deletion " << parent << " has "
                         << parentSplitter->count() << " elements";


            //find the container somewhere to activate
            auto *containerToActivate = parentSplitter->findChild<Sublime::Container*>();
            //activate the current view there
            if (containerToActivate) {
                m_mainWindow->setActiveView(containerToActivate->viewForWidget(containerToActivate->currentWidget()));
                return;
            }
        }
    }

    if ( wasActive ) {
        m_mainWindow->setActiveView(nullptr);
    }
}

void MainWindowPrivate::toolViewAdded(Sublime::View* /*toolView*/, Sublime::Position position)
{
    IdealToolViewCreator toolViewCreator(this);
    area->walkToolViews(toolViewCreator, position);
}

void MainWindowPrivate::aboutToRemoveToolView(Sublime::View *toolView, Sublime::Position /*position*/)
{
    if (!docks.contains(toolView))
        return;

    idealController->removeView(toolView);
    // TODO are Views unique?
    docks.removeAll(toolView);
}

Qt::DockWidgetArea MainWindowPrivate::positionToDockArea(Position position)
{
    switch (position)
    {
        case Sublime::Left: return Qt::LeftDockWidgetArea;
        case Sublime::Right: return Qt::RightDockWidgetArea;
        case Sublime::Bottom: return Qt::BottomDockWidgetArea;
        case Sublime::Top: return Qt::TopDockWidgetArea;
        default: return Qt::LeftDockWidgetArea;
    }
}

void MainWindowPrivate::updateAreaSwitcher(Sublime::Area *area)
{
    QAction* action = m_areaActions.value(area);
    if (action)
        action->setChecked(true);
}

void MainWindowPrivate::activateFirstVisibleView()
{
    QList<Sublime::View*> views = area->views();
    if (views.count() > 0)
        m_mainWindow->activateView(views.first());
}

void MainWindowPrivate::widgetCloseRequest(QWidget* widget)
{
    if (View *view = widgetToView.value(widget))
    {
        area->closeView(view);
    }
}

void MainWindowPrivate::postMessage(Message* message)
{
    if (!message) {
        return;
    }

    message->setParent(this);

    // if there are no actions, add a close action by default if widget does not auto-hide
    if (message->actions().isEmpty() && message->autoHide() < 0) {
        auto* closeAction = new QAction(QIcon::fromTheme(QStringLiteral("window-close")),
                                        i18nc("@action", "Close"));
        closeAction->setToolTip(i18nc("@info:tooltip", "Close message"));
        message->addAction(closeAction);
    }

    // reparent actions, as we want full control over when they are deleted
    QVector<QSharedPointer<QAction>> managedMessageActions;
    const auto messageActions = message->actions();
    managedMessageActions.reserve(messageActions.size());
    for (QAction* action : messageActions) {
        action->setParent(nullptr);
        managedMessageActions.append(QSharedPointer<QAction>(action));
    }
    m_messageHash.insert(message, managedMessageActions);

    // also catch if the user manually calls delete message
    connect(message, &Message::closed, this, &MainWindowPrivate::messageDestroyed);

    messageWidget->postMessage(message, managedMessageActions);
}

void MainWindowPrivate::messageDestroyed(Message* message)
{
    // Message is already in destructor
    Q_ASSERT(m_messageHash.contains(message));
    m_messageHash.remove(message);
}

}

#include "moc_mainwindow_p.cpp"


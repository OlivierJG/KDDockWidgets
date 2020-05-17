/*
  This file is part of KDDockWidgets.

  Copyright (C) 2020 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Author: Sérgio Martins <sergio.martins@kdab.com>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "Item_p.h"
#include "Separator_p.h"

#include <QEvent>
#include <QDebug>
#include <QScopedValueRollback>
#include <QTimer>

using namespace Layouting;

int Layouting::Item::separatorThickness = 5;
const QSize Layouting::Item::hardcodedMinimumSize = QSize(KDDOCKWIDGETS_MIN_WIDTH, KDDOCKWIDGETS_MIN_HEIGHT);

ItemContainer *Item::root() const
{
    return m_parent ? m_parent->root()
                    : const_cast<ItemContainer*>(qobject_cast<const ItemContainer*>(this));
}

QRect Item::mapToRoot(QRect r) const
{
    const QPoint topLeft = mapToRoot(r.topLeft());
    r.moveTopLeft(topLeft);
    return r;
}

QPoint Item::mapToRoot(QPoint p) const
{
    if (isRoot())
        return p;

    return p + parentContainer()->mapToRoot(pos());
}

int Item::mapToRoot(int p, Qt::Orientation o) const
{
    if (o == Qt::Vertical)
        return mapToRoot(QPoint(0, p)).y();
    return mapToRoot(QPoint(p, 0)).x();
}

QPoint Item::mapFromRoot(QPoint p) const
{
    const Item *it = this;
    while (it) {
        p = p - it->pos();
        it = it->parentContainer();
    }

    return p;
}

QRect Item::mapFromRoot(QRect r) const
{
    const QPoint topLeft = mapFromRoot(r.topLeft());
    r.moveTopLeft(topLeft);
    return r;
}

QPoint Item::mapFromParent(QPoint p) const
{
    if (isRoot())
        return p;

    return p - pos();
}

int Item::mapFromRoot(int p, Qt::Orientation o) const
{
    if (o == Qt::Vertical)
        return mapFromRoot(QPoint(0, p)).y();
    return mapFromRoot(QPoint(p, 0)).x();
}

void Item::setGuest(GuestInterface *guest)
{   
    Q_ASSERT(!guest || !m_guest);
    QWidget *newWidget = guest ? guest->asWidget() : nullptr;
    QWidget *oldWidget = widget();

    if (oldWidget) {
        oldWidget->removeEventFilter(this);
        disconnect(oldWidget, nullptr, this, nullptr);
    }

    m_guest = guest;

    if (m_guest) {
        m_guest->setLayoutItem(this);
        newWidget->installEventFilter(this);
        newWidget->setParent(m_hostWidget);
        setMinSize(widgetMinSize(newWidget));

        connect(newWidget, &QObject::objectNameChanged, this, &Item::updateObjectName);
        connect(newWidget, &QObject::destroyed, this, &Item::onWidgetDestroyed);
        connect(newWidget, SIGNAL(layoutInvalidated()), this, SLOT(onWidgetLayoutRequested()));

        if (m_sizingInfo.geometry.isEmpty()) {
            // Use the widgets geometry, but ensure it's at least hardcodedMinimumSize
            QRect widgetGeo = newWidget->geometry();
            widgetGeo.setSize(widgetGeo.size().expandedTo(Item::hardcodedMinimumSize));
            setGeometry(mapFromRoot(widgetGeo));
        } else {
            updateWidgetGeometries();
        }
    }

    updateObjectName();
}

void Item::updateWidgetGeometries()
{
    if (auto w = widget()) {
        w->setGeometry(mapToRoot(rect()));
    }
}

QVariantMap Item::toVariantMap() const
{
    QVariantMap result;

    result[QStringLiteral("sizingInfo")] = m_sizingInfo.toVariantMap();
    result[QStringLiteral("isVisible")] = m_isVisible;
    result[QStringLiteral("isContainer")] = isContainer();
    result[QStringLiteral("objectName")] = objectName();
    if (m_guest)
        result[QStringLiteral("guestId")] = QString::number(qint64(m_guest->asWidget())); // just for coorelation purposes when restoring

    return result;
}

void Item::fillFromVariantMap(const QVariantMap &map, const QHash<QString, GuestInterface *> &widgets)
{
    m_sizingInfo.fromVariantMap(map[QStringLiteral("sizingInfo")].toMap());
    m_isVisible = map[QStringLiteral("isVisible")].toBool();
    setObjectName(map[QStringLiteral("objectName")].toString());

    const QString guestId = map.value(QStringLiteral("guestId")).toString();
    if (!guestId.isEmpty()) {
        if (GuestInterface *guest = widgets.value(guestId)) {
            setGuest(guest);
            m_guest->asWidget()->setParent(hostWidget());
        } else if (hostWidget()) {
            qWarning() << Q_FUNC_INFO << "Couldn't find frame to restore for" << this;
        }
    }
}

Item *Item::createFromVariantMap(QWidget *hostWidget, ItemContainer *parent,
                                 const QVariantMap &map, const QHash<QString, GuestInterface *> &widgets)
{
    auto item = new Item(hostWidget, parent);
    item->fillFromVariantMap(map, widgets);
    return item;
}

void Item::ref()
{
    m_refCount++;
}

void Item::unref()
{
    Q_ASSERT(m_refCount > 0);
    m_refCount--;
    if (m_refCount == 0) {
        Q_ASSERT(!isRoot());
        parentContainer()->removeItem(this);
    }
}

int Item::refCount() const
{
    return m_refCount;
}

QWidget *Item::hostWidget() const
{
    return m_hostWidget;
}

void Item::restore(GuestInterface *guest)
{
    Q_ASSERT(!isVisible() && !widget());
    if (isContainer()) {
        qWarning() << Q_FUNC_INFO << "Containers can't be restored";
    } else {
        setGuest(guest);
        parentContainer()->restoreChild(this, NeighbourSqueezeStrategy::ImmediateNeighboursFirst);

        // When we restore to previous positions, we only still from the immediate neighbours.
        // It's consistent with closing an item, it also only grows the immediate neighbours
        // By passing ImmediateNeighboursFirst we can hide/show an item multiple times and it
        // uses the same place
    }
}

QVector<int> Item::pathFromRoot() const
{
    // Returns the list of indexes to get to this item, starting from the root container
    // Example [0, 1, 3] would mean that the item is the 4th child of the 2nd child of the 1st child of root
    // [] would mean 'this' is the root item
    // [0] would mean the 1st child of root

    QVector<int> path;
    path.reserve(10); // random big number, good to bootstrap it

    const Item *it = this;
    while (it) {
        if (auto p = it->parentContainer()) {
            const int index = p->m_children.indexOf(const_cast<Item*>(it));
            path.prepend(index);
            it = p;
        } else {
            break;
        }
    }

    return path;
}

void Item::setHostWidget(QWidget *host)
{
    if (m_hostWidget != host) {
        m_hostWidget = host;
        if (auto w = widget()) {
            w->setParent(host);
            w->setVisible(true);
            updateWidgetGeometries();
        }
    }
}

void Item::setSize_recursive(QSize newSize, ChildrenResizeStrategy)
{
    setSize(newSize);
}

QSize Item::missingSize() const
{
    QSize missing = minSize() - this->size();
    missing.setWidth(qMax(missing.width(), 0));
    missing.setHeight(qMax(missing.height(), 0));

    return missing;
}

bool Item::isBeingInserted() const
{
    return m_sizingInfo.isBeingInserted;
}

void Item::setBeingInserted(bool is)
{
    m_sizingInfo.isBeingInserted = is;

    // Trickle up the hierarchy too, as the parent might be hidden due to not having visible children
    if (auto parent = parentContainer()) {
        if (is) {
            if (!parent->hasVisibleChildren())
                parent->setBeingInserted(true);
        } else {
            parent->setBeingInserted(false);
        }
    }
}

void Item::setParentContainer(ItemContainer *parent)
{
    if (parent == m_parent)
        return;

    if (m_parent) {
        disconnect(this, &Item::minSizeChanged, m_parent, &ItemContainer::onChildMinSizeChanged);
        disconnect(this, &Item::visibleChanged, m_parent, &ItemContainer::onChildVisibleChanged);
        Q_EMIT visibleChanged(this, false);
    }

    if (auto c = asContainer()) {
        const bool ceasingToBeRoot = !m_parent && parent;
        if (ceasingToBeRoot && !c->hasVisibleChildren()) {
            // Was root but is not root anymore. So, if empty, then it has an empty rect too.
            // Only root can have a non-empty rect without having children
            c->setGeometry({});
        }
    }

    m_parent = parent;
    connectParent(parent); // Reused by the ctor too

    QObject::setParent(parent);
}

void Item::connectParent(ItemContainer *parent)
{
    if (parent) {
        connect(this, &Item::minSizeChanged, parent, &ItemContainer::onChildMinSizeChanged);
        connect(this, &Item::visibleChanged, parent, &ItemContainer::onChildVisibleChanged);

        setHostWidget(parent->hostWidget());
        updateWidgetGeometries();

        Q_EMIT visibleChanged(this, isVisible());
    }
}

ItemContainer *Item::parentContainer() const
{
    return m_parent;
}

const ItemContainer *Item::asContainer() const
{
    return qobject_cast<const ItemContainer*>(this);
}

ItemContainer *Item::asContainer()
{
    return qobject_cast<ItemContainer*>(this);
}

void Item::setMinSize(QSize sz)
{
    if (sz != m_sizingInfo.minSize) {
        m_sizingInfo.minSize = sz;
        Q_EMIT minSizeChanged(this);
        setSize_recursive(size().expandedTo(sz));
    }
}

void Item::setMaxSize(QSize sz)
{
    Q_ASSERT(!isContainer());
    m_sizingInfo.maxSize = sz;
}

QSize Item::minSize() const
{
    return m_sizingInfo.minSize;
}

QSize Item::maxSize() const
{
    return m_sizingInfo.maxSize;
}

void Item::setPos(QPoint pos)
{
    QRect geo = m_sizingInfo.geometry;
    geo.moveTopLeft(pos);
    setGeometry(geo);
}

void Item::setPos(int pos, Qt::Orientation o)
{
    if (o == Qt::Vertical) {
        setPos({ x(), pos });
    } else {
        setPos({ pos, y() });
    }
}

int Item::pos(Qt::Orientation o) const
{
    return o == Qt::Vertical ? y() : x();
}

void Item::insertItem(Item *item, Location loc, DefaultSizeMode defaultSizeMode, AddingOption option)
{
    Q_ASSERT(item != this);

    item->setIsVisible(!(option & AddingOption_StartHidden));
    Q_ASSERT(!((option & AddingOption_StartHidden) && item->isContainer()));

    if (m_parent->hasOrientationFor(loc)) {
        const bool locIsSide1 = locationIsSide1(loc);
        int indexInParent = m_parent->childItems().indexOf(this);
        if (!locIsSide1)
            indexInParent++;

        const Qt::Orientation orientation = orientationForLocation(loc);
        if (orientation != m_parent->orientation()) {
            Q_ASSERT(m_parent->visibleChildren().size() == 1);
            // This is the case where the container only has one item, so it's both vertical and horizontal
            // Now its orientation gets defined
            m_parent->setOrientation(orientation);
        }

        m_parent->insertItem(item, indexInParent, defaultSizeMode);
    } else {
        ItemContainer *container = m_parent->convertChildToContainer(this);
        container->insertItem(item, loc, defaultSizeMode, option);
    }
}

int Item::x() const
{
    return m_sizingInfo.geometry.x();
}

int Item::y() const
{
    return m_sizingInfo.geometry.y();
}

int Item::width() const
{
    return m_sizingInfo.geometry.width();
}

int Item::height() const
{
    return m_sizingInfo.geometry.height();
}

QSize Item::size() const
{
    return m_sizingInfo.geometry.size();
}

void Item::setSize(QSize sz)
{
    QRect newGeo = m_sizingInfo.geometry;
    newGeo.setSize(sz);
    setGeometry(newGeo);
}

QPoint Item::pos() const
{
    return m_sizingInfo.geometry.topLeft();
}

int Item::position(Qt::Orientation o) const
{
    return o == Qt::Vertical ? y()
                             : x();
}

QRect Item::geometry() const
{
    return isBeingInserted() ? QRect()
                             : m_sizingInfo.geometry;
}

QRect Item::rect() const
{
    return QRect(0, 0, width(), height());
}

bool Item::isContainer() const
{
    return m_isContainer;
}

int Item::minLength(Qt::Orientation o) const
{
    return Layouting::length(minSize(), o);
}

void Item::setLength(int length, Qt::Orientation o)
{
    Q_ASSERT(length > 0);
    if (o == Qt::Vertical) {
        const int w = qMax(width(), hardcodedMinimumSize.width());
        setSize(QSize(w, length));
    } else {
        const int h = qMax(height(), hardcodedMinimumSize.height());
        setSize(QSize(length, h));
    }
}

void Item::setLength_recursive(int length, Qt::Orientation o)
{
    setLength(length, o);
}

int Item::length(Qt::Orientation o) const
{
    return Layouting::length(size(), o);
}

int Item::availableLength(Qt::Orientation o) const
{
    return length(o) - minLength(o);
}

bool Item::isPlaceholder() const
{
    return !isVisible();
}

bool Item::isVisible(bool excludeBeingInserted) const
{
    return m_isVisible && !(excludeBeingInserted && isBeingInserted());
}

void Item::setIsVisible(bool is)
{
    if (is != m_isVisible) {
        m_isVisible = is;
        Q_EMIT visibleChanged(this, is);
    }

    if (is) {
        if (auto w = widget()) {
            w->setGeometry(mapToRoot(rect()));
            w->setVisible(true); // TODO: Only set visible when apply*() ?
        }
    }

    updateObjectName();
}

void Item::setGeometry_recursive(QRect rect)
{
    // Recursiveness doesn't apply for for non-container items
    setGeometry(rect);
}

bool Item::checkSanity()
{
    if (!root())
        return true;

    if (minSize().width() > width() || minSize().height() > height()) {
        root()->dumpLayout();
        qWarning() << Q_FUNC_INFO << "Size constraints not honoured" << this
                   << "; min=" << minSize() << "; size=" << size();
        return false;
    }

    if (auto w = widget()) {

        if (w->parentWidget() != hostWidget()) {
            qWarning() << Q_FUNC_INFO << "Unexpected parent for our guest"
                       << w->parentWidget() << "; host=" << hostWidget()
                       << w << this;
            return false;
        }

        if (false && !w->isVisible() && (!w->parentWidget() || w->parentWidget()->isVisible())) {
            // TODO: if guest is explicitly hidden we're not hidding the item yet
            qWarning() << Q_FUNC_INFO << "Guest widget isn't visible" << this
                       << w;
            return false;
        }

        if (w->geometry() != mapToRoot(rect())) {
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Guest widget doesn't have correct geometry. has"
                       << "guest.global=" << w->geometry()
                       << "; item.local=" << geometry()
                       << "; item.global=" << mapToRoot(rect())
                       << this
                       << w;
            return false;
        }
    }
return true;
    if (!isVisible()) {
        if (auto w = widget()) {
            if (w->isVisible()) {
                qWarning() << Q_FUNC_INFO << "Item is not visible but guest is visible";
                return false;
            }
        }
    }

    return true;
}

void Item::setGeometry(QRect rect)
{
    QRect &m_geometry = m_sizingInfo.geometry;

    if (rect != m_geometry) {
        const QRect oldGeo = m_geometry;

        m_geometry = rect;

        if (rect.isEmpty()) {
            // Just a sanity check...
            ItemContainer *c = asContainer();
            if (c) {
                if (c->hasVisibleChildren()) {
                    if (auto r = root()) r->dumpLayout();
                    Q_ASSERT(false);
                }
            } else {
                qWarning() << Q_FUNC_INFO << "Empty rect";
            }
        }

        const QSize minSz = minSize();
        if (rect.width() < minSz.width() || rect.height() < minSz.height()) {
            if (auto r = root()) r->dumpLayout();
            qWarning() << Q_FUNC_INFO << this << "Constraints not honoured."
                       << "sz=" << rect.size() << "; min=" << minSz
                       << ": parent=" << parentContainer();

        }

        Q_EMIT geometryChanged();

        if (oldGeo.x() != x())
            Q_EMIT xChanged();
        if (oldGeo.y() != y())
            Q_EMIT yChanged();
        if (oldGeo.width() != width())
            Q_EMIT widthChanged();
        if (oldGeo.height() != height())
            Q_EMIT heightChanged();

        updateWidgetGeometries();
    }
}

void Item::dumpLayout(int level)
{
    QString indent;
    indent.fill(QLatin1Char(' '), level);
    const QString beingInserted = m_sizingInfo.isBeingInserted ? QStringLiteral(";beingInserted;")
                                                               : QString();
    const QString visible = !isVisible() ? QStringLiteral(";hidden;")
                                         : QString();

    qDebug().noquote() << indent << "- Widget: " << objectName()
                       << m_sizingInfo.geometry// << "r=" << m_geometry.right() << "b=" << m_geometry.bottom()
                       << "; min=" << minSize()
                       << visible << beingInserted << this
                       << "; guest=" << widget();
}

Item::Item(QWidget *hostWidget, ItemContainer *parent)
    : QObject(parent)
    , m_isContainer(false)
    , m_parent(parent)
    , m_hostWidget(hostWidget)
{
    connectParent(parent);
}

Item::Item(bool isContainer, QWidget *hostWidget, ItemContainer *parent)
    : QObject(parent)
    , m_isContainer(isContainer)
    , m_parent(parent)
    , m_hostWidget(hostWidget)
{
    connectParent(parent);
}

Item::~Item()
{
}

bool Item::eventFilter(QObject *widget, QEvent *e)
{
    if (e->type() != QEvent::ParentChange)
        return false;

    if (widget->parent() != hostWidget()) {
        // Frame was detached into floating window. Turn into placeholder
        Q_ASSERT(isVisible());
        turnIntoPlaceholder();
    }

    return false;
}


void Item::turnIntoPlaceholder()
{
    Q_ASSERT(!isContainer());

    // Turning into placeholder just means hidding it. So we can show it again in its original position.
    // Call removeItem() so we share the code for making the neighbours grow into the space that becomes available
    // after hidding this one
    parentContainer()->removeItem(this, /*hardDelete=*/ false);
}

void Item::updateObjectName()
{
    if (isContainer())
        return;

    if (auto w = widget()) {
        setObjectName(w->objectName().isEmpty() ? QStringLiteral("widget") : w->objectName());
    } else if (!isVisible()) {
        setObjectName(QStringLiteral("hidden"));
    } else if (!m_guest) {
        setObjectName(QStringLiteral("null"));
    } else {
        setObjectName(QStringLiteral("empty"));
    }
}

void Item::onWidgetDestroyed()
{
    if (m_refCount) {
        turnIntoPlaceholder();
    } else if (!isRoot()) {
        parentContainer()->removeItem(this);
    }
}

void Item::onWidgetLayoutRequested()
{
    if (QWidget *w = widget()) {
        if (w->size() != size()) {
            qDebug() << Q_FUNC_INFO << "TODO: Not implemented yet. Widget can't just decide to resize yet"
                       << w->size()
                       << size()
                       << m_sizingInfo.geometry
                       << m_sizingInfo.isBeingInserted;
        }

        if (widgetMinSize(w) != minSize()) {
            setMinSize(widgetMinSize(w));
        }
    }
}

bool Item::isRoot() const
{
    return m_parent == nullptr;
}

int Item::visibleCount_recursive() const
{
    return isVisible() ? 1 : 0;
}

struct ItemContainer::Private
{
    Private(ItemContainer *q)
        : q(q)
    {
    }

    int defaultLengthFor(Item *item, DefaultSizeMode) const;

    ItemContainer *const q;
};

ItemContainer::ItemContainer(QWidget *hostWidget, ItemContainer *parent)
    : Item(true, hostWidget, parent)
    , d(new Private(this))
{
    Q_ASSERT(parent);

    connect(this, &Item::xChanged, this, [this] {
        for (Item *item : qAsConst(m_children)) {
            Q_EMIT item->xChanged();
        }
    });

    connect(this, &Item::yChanged, this, [this] {
        for (Item *item : qAsConst(m_children)) {
            Q_EMIT item->yChanged();
        }
    });
}

ItemContainer::ItemContainer(QWidget *hostWidget)
    : Item(true, hostWidget, /*parentContainer=*/ nullptr)
    , d(new Private(this))
{
}

ItemContainer::~ItemContainer()
{
    delete d;
}

bool ItemContainer::checkSanity()
{
    m_checkSanityScheduled = false;

    if (!hostWidget()) {
        /// This is a dummy ItemContainer, just return true
        return true;
    }

    if (!Item::checkSanity())
        return false;

    if (numChildren() == 0 && !isRoot()) {
        qWarning() << Q_FUNC_INFO << "Container is empty. Should be deleted";
        return false;
    }

    if (m_orientation != Qt::Vertical && m_orientation != Qt::Horizontal) {
        qWarning() << Q_FUNC_INFO << "Invalid orientation" << m_orientation << this;
        return false;
    }

    // Check that the geometries don't overlap
    int expectedPos = 0;
    for (Item *item : m_children) {
        if (!item->isVisible())
            continue;
        const int pos = Layouting::pos(item->pos(), m_orientation);
        if (expectedPos != pos) {
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Unexpected pos" << pos << "; expected=" << expectedPos
                       << "; for item=" << item
                       << "; isContainer=" << item->isContainer();
            return false;
        }

        expectedPos = pos + Layouting::length(item->size(), m_orientation) + separatorThickness;
    }

    const int h1 = Layouting::length(size(), oppositeOrientation(m_orientation));
    for (Item *item : m_children) { 
        if (item->parentContainer() != this) {
            qWarning() << "Invalid parent container for" << item
                       << "; is=" << item->parentContainer() << "; expected=" << this;
            return false;
        }

        if (item->parent() != this) {
            qWarning() << "Invalid QObject parent for" << item
                       << "; is=" << item->parent() << "; expected=" << this;
            return false;
        }

        if (item->isVisible()) {
            // Check the children height (if horizontal, and vice-versa)
            const int h2 = Layouting::length(item->size(), oppositeOrientation(m_orientation));
            if (h1 != h2) {
                root()->dumpLayout();
                qWarning() << Q_FUNC_INFO << "Invalid size for item." << item
                           << "Container.length=" << h1 << "; item.length=" << h2;
                return false;
            }

            if (!rect().contains(item->geometry())) {
                root()->dumpLayout();
                qWarning() << Q_FUNC_INFO << "Item geo is out of bounds. item=" << item << "; geo="
                           << item->geometry() << "; parent.rect=" << rect();
                return false;
            }
        }

        if (!item->checkSanity())
            return false;
    }

    const Item::List visibleChildren = this->visibleChildren();
    const bool isEmptyRoot = isRoot() && visibleChildren.isEmpty();
    if (!isEmptyRoot) {
        int occupied = qMax(0, Item::separatorThickness * (visibleChildren.size() - 1));
        for (Item *item : visibleChildren) {
            occupied += item->length(m_orientation);
        }

        if (occupied != length()) {
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Unexpected length. Expected=" << occupied
                       << "; got=" << length() << "; this=" << this;
            return false;
        }

        const QVector<double> percentages = childPercentages();
        const double totalPercentage = std::accumulate(percentages.begin(), percentages.end(), 0.0);
        const double expectedPercentage = visibleChildren.isEmpty() ? 0.0 : 1.0;
        if (!qFuzzyCompare(totalPercentage, expectedPercentage)) {
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Percentages don't add up"
                       << totalPercentage << percentages
                       << this;
            const_cast<ItemContainer*>(this)->updateSeparators_recursive();
            qWarning() << Q_FUNC_INFO << childPercentages();
            return false;
        }
    }

    const int numVisibleChildren = visibleChildren.size();
    if (m_separators.size() != qMax(0, numVisibleChildren - 1)) {
        root()->dumpLayout();
        qWarning() << Q_FUNC_INFO << "Unexpected number of separators" << m_separators.size()
                   << numVisibleChildren;
        return false;
    }

    const QSize expectedSeparatorSize = isVertical() ? QSize(width(), Item::separatorThickness)
                                                     : QSize(Item::separatorThickness, height());

    const int pos2 = Layouting::pos(mapToRoot(QPoint(0, 0)), oppositeOrientation(m_orientation));

    for (int i = 0; i < m_separators.size(); ++i) {
        Separator *separator = m_separators.at(i);
        Item *item = visibleChildren.at(i);
        const int expectedSeparatorPos = mapToRoot(item->m_sizingInfo.edge(m_orientation) + 1, m_orientation);

        if (separator->hostWidget() != hostWidget()) {
            qWarning() << Q_FUNC_INFO << "Invalid host widget for separator"
                       << separator->hostWidget() << hostWidget() << this;
            return false;
        }

        if (separator->parentContainer() != this) {
            qWarning() << Q_FUNC_INFO << "Invalid parent container for separator"
                       << separator->parentContainer() << separator << this;
            return false;
        }

        if (separator->position() != expectedSeparatorPos) {
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Unexpected separator position" << separator->position()
                       << "; expected=" << expectedSeparatorPos
                       << separator << "; this=" << this;
            return false;
        }

        if (separator->geometry().size() != expectedSeparatorSize) {
            qWarning() << Q_FUNC_INFO << "Unexpected separator size" << separator->geometry().size()
                       << "; expected=" << expectedSeparatorSize
                       << separator << "; this=" << this;
            return false;
        }

        const int separatorPos2 = Layouting::pos(separator->geometry().topLeft(), oppositeOrientation(m_orientation));
        if (Layouting::pos(separator->geometry().topLeft(), oppositeOrientation(m_orientation)) != pos2) {
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Unexpected position pos2=" << separatorPos2
                       << "; expected=" << pos2
                          << separator << "; this=" << this;
            return false;
        }

        if (separator->hostWidget() != hostWidget()) {
            qWarning() << Q_FUNC_INFO << "Unexpected host widget in separator"
                       << separator->hostWidget() << "; expected=" << hostWidget();
            return false;
        }

        const int separatorMinPos = minPosForSeparator_global(separator);
        const int separatorMaxPos = maxPosForSeparator_global(separator);
        const int separatorPos = separator->position();
        if (separatorPos < separatorMinPos || separatorPos > separatorMaxPos ||
                separatorMinPos <= 0 || separatorMaxPos <= 0) {
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Invalid bounds for separator, pos="
                       << separatorPos << "; min=" << separatorMinPos
                       << "; max=" << separatorMaxPos
                       << separator;
            return false;
        }
    }

#ifdef DOCKS_DEVELOPER_MODE
    // Can cause slowdown, so just use it in developer mode.
    if (isRoot()) {
        if (!asContainer()->test_suggestedRect())
            return false;
    }
#endif

    return true;
}

void ItemContainer::scheduleCheckSanity() const
{
    if (!m_checkSanityScheduled) {
        m_checkSanityScheduled = true;
        QTimer::singleShot(0, root(), &ItemContainer::checkSanity);
    }
}

bool ItemContainer::hasOrientation() const
{
    return isVertical() || isHorizontal();
}

int ItemContainer::numChildren() const
{
    return m_children.size();
}

int ItemContainer::numVisibleChildren() const
{
    int num = 0;
    for (Item *child : m_children) {
        if (child->isVisible())
            num++;
    }
    return num;
}

int ItemContainer::indexOfVisibleChild(const Item *item) const
{
    const Item::List items = visibleChildren();
    return items.indexOf(const_cast<Item*>(item));
}

const Item::List ItemContainer::childItems() const
{
    return m_children;
}

void ItemContainer::removeItem(Item *item, bool hardRemove)
{
    Q_ASSERT(!item->isRoot());

    if (!contains(item)) {
        // Not ours, ask parent
        item->parentContainer()->removeItem(item, hardRemove);
        return;
    }

    Item *side1Item = visibleNeighbourFor(item, Side1);
    Item *side2Item = visibleNeighbourFor(item, Side2);

    const bool isContainer = item->isContainer();
    const bool wasVisible = !isContainer && item->isVisible();

    if (hardRemove) {
        m_children.removeOne(item);
        delete item;
        if (!isContainer)
            Q_EMIT root()->numItemsChanged();
    } else {
        item->setIsVisible(false);
        item->setGuest(nullptr);

        if (!wasVisible && !isContainer) {
            // Was already hidden
            return;
        }
    }

    if (wasVisible) {
        Q_EMIT root()->numVisibleItemsChanged(root()->numVisibleChildren());
    }

    if (isEmpty()) {
        // Empty container is useless, delete it
        if (auto p = parentContainer())
            p->removeItem(this, /*hardDelete=*/ true);
    } else if (!hardRemove && !hasVisibleChildren()) {
        if (auto p = parentContainer()) {
            p->removeItem(this, /*hardDelete=*/ false);
            setGeometry(QRect());
        }
    } else {
        // Neighbours will occupy the space of the deleted item
        growNeighbours(side1Item, side2Item);
        Q_EMIT itemsChanged();

        updateSizeConstraints();
        updateSeparators_recursive();
    }
}

bool ItemContainer::isEmpty() const
{
    return m_children.isEmpty();
}

void ItemContainer::setGeometry_recursive(QRect rect)
{
    setPos(rect.topLeft());

    // Call resize, which is recursive and will resize the children too
    setSize_recursive(rect.size());
}

ItemContainer *ItemContainer::convertChildToContainer(Item *leaf)
{
    QScopedValueRollback<bool> converting(m_convertingItemToContainer, true);

    const int index = m_children.indexOf(leaf);
    Q_ASSERT(index != -1);
    auto container = new ItemContainer(hostWidget(), this);
    container->setParentContainer(nullptr);
    container->setParentContainer(this);

    insertItem(container, index, DefaultSizeMode::None);
    m_children.removeOne(leaf);
    container->setGeometry(leaf->geometry());
    container->insertItem(leaf, Location_OnTop, DefaultSizeMode::None);
    Q_EMIT itemsChanged();
    updateSeparators_recursive();

    return container;
}

void ItemContainer::insertItem(Item *item, Location loc, DefaultSizeMode defaultSizeMode,
                               AddingOption addingOption)
{
    Q_ASSERT(item != this);
    if (contains(item)) {
        qWarning() << Q_FUNC_INFO << "Item already exists";
        return;
    }

    item->setIsVisible(!(addingOption & AddingOption_StartHidden));
    Q_ASSERT(!((addingOption & AddingOption_StartHidden) && item->isContainer()));

    const Qt::Orientation locOrientation = orientationForLocation(loc);

    if (hasOrientationFor(loc)) {
        if (m_children.size() == 1) {
            // 2 items is the minimum to know which orientation we're layedout
            m_orientation = locOrientation;
        }

        const int index = locationIsSide1(loc) ? 0 : m_children.size();
        insertItem(item, index, defaultSizeMode);
    } else {
        // Inserting directly in a container ? Only if it's root.
        Q_ASSERT(isRoot());
        auto container = new ItemContainer(hostWidget(), this);
        container->setGeometry(rect());
        container->setChildren(m_children, m_orientation);
        m_children.clear();
        setOrientation(oppositeOrientation(m_orientation));
        insertItem(container, 0, DefaultSizeMode::None);

        // Now we have the correct orientation, we can insert
        insertItem(item, loc, defaultSizeMode, addingOption);

        if (!container->hasVisibleChildren())
            container->setGeometry(QRect());
    }

    updateSeparators_recursive();
    scheduleCheckSanity();
}

void ItemContainer::onChildMinSizeChanged(Item *child)
{
    if (m_convertingItemToContainer || m_isDeserializing || !child->isVisible()) {
        // Don't bother our parents, we're converting
        return;
    }

    updateSizeConstraints();

    if (child->isBeingInserted())
        return;

    if (numVisibleChildren() == 1 && child->isVisible()) {
        // The easy case. Child is alone in the layout, occupies everything.
        child->setGeometry(rect());
        updateChildPercentages();
        return;
    }

    const QSize missingForChild = child->missingSize();
    if (!missingForChild.isNull()) {
        // Child has some growing to do. It will grow left and right equally, (and top-bottom), as needed.
        growItem(child, Layouting::length(missingForChild, m_orientation), GrowthStrategy::BothSidesEqually, NeighbourSqueezeStrategy::AllNeighbours);
    }

    updateChildPercentages();
}

void ItemContainer::updateSizeConstraints()
{
    const QSize missingSize = this->missingSize();
    if (!missingSize.isNull()) {
        if (isRoot()) {
            // Resize the whole layout
            setSize_recursive(size() + missingSize);
        }
    }

    // Our min-size changed, notify our parent, and so on until it reaches root()
    Q_EMIT minSizeChanged(this);
}

void ItemContainer::onChildVisibleChanged(Item */*child*/, bool visible)
{
    if (m_isDeserializing)
        return;

    const int numVisible = numVisibleChildren();
    if (visible && numVisible == 1) {
        // Child became visible and there's only 1 visible child. Meaning there were 0 visible before.
        Q_EMIT visibleChanged(this, true);
    } else if (!visible && numVisible == 0) {
        Q_EMIT visibleChanged(this, false);
    }
}

QRect ItemContainer::suggestedDropRect(const Item *item, const Item *relativeTo, Location loc) const
{
    // Returns the drop rect. This is the geometry used by the rubber band when you hover over an indicator.
    // It's calculated by copying the layout and inserting the item into the dummy/invisible copy
    // The we see which geometry the item got. This way the returned geometry is always what the item will get
    // if you drop it.
    // One exception is if the window doesn't have enough space and it would grow. In this case
    // we fall back to something reasonable


    if (relativeTo && !relativeTo->parentContainer()) {
        qWarning() << Q_FUNC_INFO << "No parent container";
        return {};
    }

    if (relativeTo && relativeTo->parentContainer() != this) {
        qWarning() << Q_FUNC_INFO << "Called on the wrong container";
        return {};
    }

    if (relativeTo && !relativeTo->isVisible()) {
        qWarning() << Q_FUNC_INFO << "relative to isn't visible";
        return {};
    }

    if (loc == Location_None) {
        qWarning() << Q_FUNC_INFO << "Invalid location";
        return {};
    }

    const QSize availableSize = root()->availableSize();
    const QSize minSize = item->minSize();
    const bool isEmpty = !root()->hasVisibleChildren();
    const int extraWidth = (isEmpty || locationIsVertical(loc)) ? 0 : Item::separatorThickness;
    const int extraHeight = (isEmpty || !locationIsVertical(loc)) ? 0 : Item::separatorThickness;
    const bool windowNeedsGrowing = availableSize.width() < minSize.width() + extraWidth ||
                                    availableSize.height() < minSize.height() + extraHeight;

    if (windowNeedsGrowing)
        return suggestedDropRectFallback(item, relativeTo, loc);

    const QVariantMap rootSerialized = root()->toVariantMap();
    ItemContainer rootCopy(nullptr);
    rootCopy.fillFromVariantMap(rootSerialized, {});

    if (relativeTo)
        relativeTo = rootCopy.itemFromPath(relativeTo->pathFromRoot());

    const QVariantMap itemSerialized = item->toVariantMap();
    auto itemCopy = new Item(nullptr);
    itemCopy->fillFromVariantMap(itemSerialized, {});

    if (relativeTo) {
        auto r = const_cast<Item*>(relativeTo);
        r->insertItem(itemCopy, loc, DefaultSizeMode::FairButFloor);
    } else {
        rootCopy.insertItem(itemCopy, loc, DefaultSizeMode::FairButFloor);
    }

    if (rootCopy.size() != root()->size()) {
        // Doesn't happen
        qWarning() << Q_FUNC_INFO << "The root copy grew ?!" << rootCopy.size() << root()->size()
                   << loc;
        return suggestedDropRectFallback(item, relativeTo, loc);
    }

    return itemCopy->mapToRoot(itemCopy->rect());
}

QRect ItemContainer::suggestedDropRectFallback(const Item *item, const Item *relativeTo, Location loc) const
{
    const QSize minSize = item->minSize();
    const int itemMin = Layouting::length(minSize, m_orientation);
    const int available = availableLength() - Item::separatorThickness;
    if (relativeTo) {
        int suggestedPos = 0;
        const QRect relativeToGeo = relativeTo->geometry();
        const int suggestedLength = relativeTo->length(orientationForLocation(loc)) / 2;
        switch (loc) {
        case Location_OnLeft:
            suggestedPos = relativeToGeo.x();
            break;
        case Location_OnTop:
            suggestedPos = relativeToGeo.y();
            break;
        case Location_OnRight:
            suggestedPos = relativeToGeo.right() - suggestedLength + 1;
            break;
        case Location_OnBottom:
            suggestedPos = relativeToGeo.bottom() - suggestedLength + 1;
            break;
        default:
            Q_ASSERT(false);
        }

        QRect rect;
        if (orientationForLocation(loc) == Qt::Vertical) {
            rect.setTopLeft(QPoint(relativeTo->x(), suggestedPos));
            rect.setSize(QSize(relativeTo->width(), suggestedLength));
        } else {
            rect.setTopLeft(QPoint(suggestedPos, relativeTo->y()));
            rect.setSize(QSize(suggestedLength, relativeTo->height()));
        }

        return mapToRoot(rect);
    } else if (isRoot()) {
        // Relative to the window itself
        QRect rect = this->rect();
        const int oneThird = length() / 3;
        const int suggestedLength = qMax(qMin(available, oneThird), itemMin);

        switch (loc) {
        case Location_OnLeft:
            rect.setWidth(suggestedLength);
            break;
        case Location_OnTop:
            rect.setHeight(suggestedLength);
            break;
        case Location_OnRight:
            rect.adjust(rect.width() - suggestedLength, 0, 0, 0);
            break;
        case Location_OnBottom:
            rect.adjust(0, rect.bottom() - suggestedLength, 0, 0);
            break;
        case Location_None:
            return {};
        }

        return rect;

    } else {
        qWarning() << Q_FUNC_INFO << "Shouldn't happen";
    }

    return {};
}

void ItemContainer::positionItems()
{
    SizingInfo::List sizes = this->sizes();
    positionItems(/*by-ref=*/sizes);
    applyPositions(sizes);

    updateSeparators_recursive();
}

void ItemContainer::applyPositions(const SizingInfo::List &sizes)
{
    const Item::List items = visibleChildren();
    const int count = items.size();
    Q_ASSERT(count == sizes.size());
    for (int i = 0; i < count; ++i) {
        Item *item = items.at(i);
        const SizingInfo &sizing = sizes[i];
        if (sizing.isBeingInserted) {
            continue;
        }

        const Qt::Orientation oppositeOrientation = Layouting::oppositeOrientation(m_orientation);
        // If the layout is horizontal, the item will have the height of the container. And vice-versa
        item->setLength_recursive(sizing.length(oppositeOrientation), oppositeOrientation);

        item->setPos(sizing.geometry.topLeft());
    }
}

Qt::Orientation ItemContainer::orientation() const
{
    return m_orientation;
}

void ItemContainer::positionItems(SizingInfo::List &sizes)
{
    int nextPos = 0;
    const int count = sizes.count();
    const Qt::Orientation oppositeOrientation = Layouting::oppositeOrientation(m_orientation);
    for (int i = 0; i < count; ++i) {
        SizingInfo &sizing = sizes[i];
        if (sizing.isBeingInserted) {
            nextPos += Item::separatorThickness;
            continue;
        }

        // If the layout is horizontal, the item will have the height of the container. And vice-versa
        const int oppositeLength = Layouting::length(size(), oppositeOrientation);
        sizing.setLength(oppositeLength, oppositeOrientation);
        sizing.setPos(0, oppositeOrientation);

        sizing.setPos(nextPos, m_orientation);
        nextPos += sizing.length(m_orientation) + Item::separatorThickness;
    }
}

void ItemContainer::clear()
{
    for (Item *item : qAsConst(m_children)) {
        if (ItemContainer *container = item->asContainer())
            container->clear();

        delete item;
    }
    m_children.clear();
    deleteSeparators();
}

Item *ItemContainer::itemForWidget(const QWidget *w) const
{
    for (Item *item : m_children) {
        if (item->isContainer()) {
            if (Item *result = item->asContainer()->itemForWidget(w))
                return result;
        } else if (item->widget() == w) {
            return item;
        }
    }

    return nullptr;
}

int ItemContainer::visibleCount_recursive() const
{
    int count = 0;
    for (Item *item : m_children) {
        count += item->visibleCount_recursive();
    }

    return count;
}

int ItemContainer::count_recursive() const
{
    int count = 0;
    for (Item *item : m_children) {
        if (auto c = item->asContainer()) {
            count += c->count_recursive();
        } else {
            count++;
        }
    }

    return count;
}

Item *ItemContainer::itemAt(QPoint p) const
{
    for (Item *item : m_children) {
        if (item->isVisible() && item->geometry().contains(p))
            return item;
    }

    return nullptr;
}

Item *ItemContainer::itemAt_recursive(QPoint p) const
{
    if (Item *item = itemAt(p)) {
        if (auto c = item->asContainer()) {
            return c->itemAt_recursive(c->mapFromParent(p));
        } else {
            return item;
        }
    }

    return nullptr;
}

Item::List ItemContainer::items_recursive() const
{
   Item::List items;
   items.reserve(30); // sounds like a good upper number to minimize allocations
   for (Item *item : m_children) {
       if (auto c  = item->asContainer()) {
           items << c->items_recursive();
       } else {
           items << item;
       }
   }

   return items;
}

void ItemContainer::setHostWidget(QWidget *host)
{
    Item::setHostWidget(host);
    deleteSeparators_recursive();
    for (Item *item : qAsConst(m_children)) {
        item->setHostWidget(host);
    }

    updateSeparators_recursive();
}

void ItemContainer::setIsVisible(bool)
{
    // no-op for containers, visibility is calculated
}

bool ItemContainer::isVisible(bool excludeBeingInserted) const
{
    return hasVisibleChildren(excludeBeingInserted);
}

void ItemContainer::setLength_recursive(int length, Qt::Orientation o)
{
    QSize sz = size();
    if (o == Qt::Vertical) {
        sz.setHeight(length);
    } else {
        sz.setWidth(length);
    }

    setSize_recursive(sz);
}

void ItemContainer::insertItem(Item *item, int index, DefaultSizeMode defaultSizeMode)
{
    if (defaultSizeMode != DefaultSizeMode::None) {
        /// Choose a nice size for the item we're adding
        const int suggestedLength = d->defaultLengthFor(item, defaultSizeMode);
        item->setLength_recursive(suggestedLength, m_orientation);
    }

    m_children.insert(index, item);
    item->setParentContainer(this);

    Q_EMIT itemsChanged();

    if (!m_convertingItemToContainer && item->isVisible())
        restoreChild(item);

    if (!item->isContainer()) {
        if (item->isVisible())
            Q_EMIT root()->numVisibleItemsChanged(root()->numVisibleChildren());
        Q_EMIT root()->numItemsChanged();
    }
}

bool ItemContainer::hasChildren() const
{
    return !m_children.isEmpty();
}

bool ItemContainer::hasVisibleChildren(bool excludeBeingInserted) const
{
    for (Item *item : m_children) {
        if (item->isVisible(excludeBeingInserted))
            return true;
    }

    return false;
}

bool ItemContainer::hasOrientationFor(Location loc) const
{
    if (m_children.size() <= 1)
        return true;

    return m_orientation == orientationForLocation(loc);
}

Item::List ItemContainer::visibleChildren(bool includeBeingInserted) const
{
    Item::List items;
    items.reserve(m_children.size());
    for (Item *item : m_children) {
        if (includeBeingInserted) {
            if (item->isVisible() || item->isBeingInserted())
                items << item;
        } else {
            if (item->isVisible() && !item->isBeingInserted())
                items << item;
        }
    }

    return items;
}

int ItemContainer::usableLength() const
{
    const Item::List children = visibleChildren();
    const int numVisibleChildren = children.size();

    if (children.size() <= 1)
        return Layouting::length(size(), m_orientation);

    const int separatorWaste = separatorThickness * (numVisibleChildren - 1);
    return length() - separatorWaste;
}

bool ItemContainer::hasSingleVisibleItem() const
{
    return numVisibleChildren() == 1;
}

bool ItemContainer::contains(const Item *item) const
{
    return m_children.contains(const_cast<Item *>(item));
}

bool ItemContainer::contains_recursive(const Item *item) const
{
    for (Item *it : m_children) {
        if (it == item) {
            return true;
        } else if (it->isContainer()) {
            if (it->asContainer()->contains_recursive(item))
                return true;
        }
    }

    return false;
}

void ItemContainer::setChildren(const Item::List &children, Qt::Orientation o)
{
    m_children = children;
    for (Item *item : children)
        item->setParentContainer(this);

    setOrientation(o);
}

void ItemContainer::setOrientation(Qt::Orientation o)
{
    if (o != m_orientation) {
        m_orientation = o;
        updateSeparators_recursive();
    }
}

QSize ItemContainer::minSize() const
{
    int minW = 0;
    int minH = 0;
    int numVisible = 0;
    if (!m_children.isEmpty()) {
        for (Item *item : m_children) {
            if (!(item->isVisible() || item->isBeingInserted()))
                continue;
            numVisible++;
            if (isVertical()) {
                minW = qMax(minW, item->minSize().width());
                minH += item->minSize().height();
            } else {
                minH = qMax(minH, item->minSize().height());
                minW += item->minSize().width();
            }
        }

        const int separatorWaste = qMax(0, (numVisible - 1) * separatorThickness);
        if (isVertical())
            minH += separatorWaste;
        else
            minW += separatorWaste;
    }

    return QSize(minW, minH);
}

QSize ItemContainer::maxSize() const
{
    int maxW = 0;
    int maxH = 0;

    if (!isEmpty()) {
        const Item::List visibleChildren = this->visibleChildren();
        for (Item *item : visibleChildren) {
            if (isVertical()) {
                maxW = qMin(maxW, item->maxSize().width());
                maxH += item->maxSize().height();
            } else {
                maxH = qMin(maxH, item->maxSize().height());
                maxW += item->maxSize().width();
            }
        }

        const int separatorWaste = (visibleChildren.size() - 1) * separatorThickness;
        if (isVertical())
            maxH += separatorWaste;
        else
            maxW += separatorWaste;
    }

    return { maxW, maxH };
}

void ItemContainer::resizeChildren(QSize oldSize, QSize newSize, SizingInfo::List &childSizes,
                                   ChildrenResizeStrategy strategy)
{
    // This container is being resized to @p newSize, so we must resize our children too, based
    //on @p strategy.
    // The new sizes are applied to @p childSizes, which will be applied to the widgets when when we're done

    const QVector<double> childPercentages = this->childPercentages();
    const int count = childSizes.count();
    const bool widthChanged = oldSize.width() != newSize.width();
    const bool heightChanged = oldSize.height() != newSize.height();
    const bool lengthChanged = (isVertical() && heightChanged) || (isHorizontal() && widthChanged);
    const int totalNewLength = usableLength();

    if (strategy == ChildrenResizeStrategy::Percentage) {
        // In this strategy mode, each children will preserve its current relative size. So, if a child
        // is occupying 50% of this container, then it will still occupy that after the container resize

        int remaining = totalNewLength;
        for (int i = 0; i < count; ++i) {
            const bool isLast = i == count - 1;

            SizingInfo &itemSize = childSizes[i];

            const qreal childPercentage = childPercentages.at(i);
            const int newItemLength = lengthChanged ? (isLast ? remaining
                                                              : int(childPercentage * totalNewLength))
                                                    : itemSize.length(m_orientation);

            if (newItemLength <= 0) {
                root()->dumpLayout();
                qWarning() << Q_FUNC_INFO << "Invalid resize newItemLength=" << newItemLength;
                Q_ASSERT(false);
                return;
            }

            remaining = remaining - newItemLength;

            if (isVertical()) {
                itemSize.geometry.setSize({ width(), newItemLength });
            } else {
                itemSize.geometry.setSize({ newItemLength, height() });
            }
        }
    } else if (strategy == ChildrenResizeStrategy::Side1SeparatorMove ||
               strategy == ChildrenResizeStrategy::Side2SeparatorMove) {
        int remaining = Layouting::length(newSize - oldSize, m_orientation); // This is how much we need to give to children (when growing the container), or to take from them when shrinking the container
        const bool isGrowing = remaining > 0;
        remaining = qAbs(remaining); // Easier to deal in positive numbers

        // We're resizing the container, and need to decide if we start resizing the 1st children or
        //, in reverse order.
        // If the separator is being dragged left or top, then isSide1SeparatorMove is true.
        // If isSide1SeparatorMove is true and we're growing, then it means this container is on the right/bottom top of the separator,
        // so should resize its first children first. Same logic for the other 3 cases

        const bool isSide1SeparatorMove = strategy == ChildrenResizeStrategy::Side1SeparatorMove;
        bool resizeHeadFirst = false;
        if (isGrowing && isSide1SeparatorMove) {
            resizeHeadFirst = true;
        } else if (isGrowing && !isSide1SeparatorMove) {
            resizeHeadFirst = false;
        } else if (!isGrowing && isSide1SeparatorMove) {
            resizeHeadFirst = false;
        } else if (!isGrowing && !isSide1SeparatorMove) {
            resizeHeadFirst = true;
        }

        for (int i = 0; i < count; i++) {
            const int index = resizeHeadFirst ? i : count - 1 - i;

            SizingInfo &size = childSizes[index];

            if (isGrowing) {
                // Since we don't honour item max-size yet, it can just grow all it wants
                size.incrementLength(remaining, m_orientation);
                remaining = 0; // and we're done, the first one got everything
            } else {
                const int availableToGive = size.availableLength(m_orientation);
                const int took = qMin(availableToGive, remaining);
                size.incrementLength(-took, m_orientation);
                remaining -= took;
            }

            if (remaining == 0)
                break;
        }
    }
}

void ItemContainer::setSize_recursive(QSize newSize, ChildrenResizeStrategy strategy)
{
    QScopedValueRollback<bool> block(m_blockUpdatePercentages, true);

    const QSize minSize = this->minSize();
    if (newSize.width() < minSize.width() || newSize.height() < minSize.height()) {
        root()->dumpLayout();
        qWarning() << Q_FUNC_INFO << "New size doesn't respect size constraints"
                   << "; new=" << newSize
                   << "; min=" << minSize
                   << this;
        return;
    }
    if (newSize == size())
        return;

    const QSize oldSize = size();
    setSize(newSize);

    const Item::List children = visibleChildren();
    const int count = children.size();
    SizingInfo::List childSizes = sizes();

    // #1 Since we changed size, also resize out children.
    // But apply them to our SizingInfo::List first before setting actual Item/QWidget geometries
    // Because we need step #2 where we ensure min sizes for each item are respected. We could
    // calculate and do everything in a single-step, but we already have the code for #2 in growItem()
    // so doing it in 2 steps will reuse much logic.

    // the sizes:
    resizeChildren(oldSize, newSize, /*by-ref*/ childSizes, strategy);

    // the positions:
    positionItems(/*by-ref*/ childSizes);

    // #2 Adjust sizes so that each item has at least Item::minSize.
    for (int i = 0; i < count; ++i) {
        SizingInfo &size = childSizes[i];
        const int missing = size.missingLength(m_orientation);
        if (missing > 0)
            growItem(i, childSizes, missing, GrowthStrategy::BothSidesEqually, NeighbourSqueezeStrategy::AllNeighbours);
    }

    // #3 Sizes are now correct and honour min/max sizes. So apply them to our Items
    applyGeometries(childSizes, strategy);
}

int ItemContainer::length() const
{
    return isVertical() ? height() : width();
}

QRect ItemContainer::rect() const
{
    QRect rect = m_sizingInfo.geometry;
    rect.moveTo(QPoint(0, 0));
    return rect;
}

void ItemContainer::dumpLayout(int level)
{
    if (level == 0 && hostWidget())
        qDebug().noquote() << " Dump Start: Host=" << hostWidget() << hostWidget()->rect() << ")";

    QString indent;
    indent.fill(QLatin1Char(' '), level);
    const QString beingInserted = m_sizingInfo.isBeingInserted ? QStringLiteral("; beingInserted;")
                                                               : QString();
    const QString visible = !isVisible() ? QStringLiteral(";hidden;")
                                         : QString();

    const QString typeStr = isRoot() ? QStringLiteral("* Root: ")
                                     : QStringLiteral("* Layout: ");

    qDebug().noquote() << indent << typeStr << m_orientation
                       << m_sizingInfo.geometry /*<< "r=" << m_geometry.right() << "b=" << m_geometry.bottom()*/
                       << "; min=" << minSize()
                       << "; this=" << this << beingInserted << visible
                       << "; %=" << childPercentages();
    int i = 0;
    for (Item *item : qAsConst(m_children)) {
        item->dumpLayout(level + 1);
        if (item->isVisible()) {
            if (i < m_separators.size()) {
                auto separator = m_separators.at(i);
                qDebug().noquote() << indent << " - Separator: " << "local.geo=" << mapFromRoot(separator->geometry())
                                   << "global.geo=" << separator->geometry()
                                   << separator;
            }
            ++i;
        }
    }
}

void ItemContainer::updateChildPercentages()
{
    if (m_blockUpdatePercentages)
        return;

    const int usable = usableLength();
    for (Item *item : qAsConst(m_children)) {
        if (item->isVisible() && !item->isBeingInserted()) {
            item->m_sizingInfo.percentageWithinParent = (1.0 * item->length(m_orientation)) / usable;
            auto p = item->m_sizingInfo.percentageWithinParent;
            if (qFuzzyIsNull(p) || p > 1.0) {
                root()->dumpLayout();
                qWarning() << Q_FUNC_INFO << "Invalid percentage" << p << this
                           << "; item=" << item
                           << "; item.length=" << item->length(m_orientation);
            }
        } else {
            item->m_sizingInfo.percentageWithinParent = 0.0;
        }
    }
}

void ItemContainer::updateChildPercentages_recursive()
{
    updateChildPercentages();
    for (Item *item : m_children) {
        if (auto c = item->asContainer())
            c->updateChildPercentages_recursive();
    }
}

QVector<double> ItemContainer::childPercentages() const
{
    QVector<double> percentages;
    percentages.reserve(m_children.size());

    for (Item *item : m_children) {
        if (item->isVisible() && !item->isBeingInserted())
            percentages << item->m_sizingInfo.percentageWithinParent;
    }

    return percentages;
}

void ItemContainer::restoreChild(Item *item, NeighbourSqueezeStrategy neighbourSqueezeStrategy)
{
    Q_ASSERT(contains(item));

    const bool hadVisibleChildren = hasVisibleChildren(/*excludeBeingInserted=*/ true);
    item->setIsVisible(true);
    item->setBeingInserted(true);

    if (!hadVisibleChildren) {
        // This container was hidden and will now be restored too, since a child was restored
        if (auto c = parentContainer()) {
            setSize(item->size()); // give it a decent size. Same size as the item being restored makes sense
            c->restoreChild(this, neighbourSqueezeStrategy);
        }
    }

    // Make sure root() is big enough to respect all item's min-sizes
    updateSizeConstraints();

    item->setBeingInserted(false);

    if (numVisibleChildren() == 1) {
        // The easy case. Child is alone in the layout, occupies everything.
        item->setGeometry_recursive(rect());
        updateSeparators_recursive();
        return;
    }

    const int available = availableOnSide(item, Side1) + availableOnSide(item, Side2) - Item::separatorThickness;

    const int max = available;
    const int min = item->minLength(m_orientation);
    const int proposed = Layouting::length(item->size(), m_orientation);
    const int newLength = qBound(min, proposed, max);

    Q_ASSERT(item->isVisible());

    // growItem() will make it grow by the same amount it steals from the neighbours, so we can't start the growing without zeroing it
    if (isVertical()) {
        item->m_sizingInfo.geometry.setHeight(0);
    } else {
        item->m_sizingInfo.geometry.setWidth(0);
    }

    growItem(item, newLength, GrowthStrategy::BothSidesEqually, neighbourSqueezeStrategy, /*accountForNewSeparator=*/ true);
    updateSeparators_recursive();
}

void ItemContainer::updateWidgetGeometries()
{
    for (Item *item : qAsConst(m_children))
        item->updateWidgetGeometries();
}

int ItemContainer::oppositeLength() const
{
    return isVertical() ? width()
                        : height();
}

void ItemContainer::requestSeparatorMove(Separator *separator, int delta)
{
    const int separatorIndex = m_separators.indexOf(separator);
    if (separatorIndex == -1) {
        // Doesn't happen
        qWarning() << Q_FUNC_INFO << "Unknown separator" << separator << this;
        root()->dumpLayout();
        return;
    }

    if (delta == 0)
        return;

    const int min = minPosForSeparator_global(separator);
    const int pos = separator->position();
    const int max = maxPosForSeparator_global(separator);

    if (pos + delta < min || pos + delta > max) {
        qWarning() << "Separator would have gone out of bounds"
                   << separator << min << pos << max << delta;
        return;
    }

    const Side moveDirection = delta < 0 ? Side1 : Side2;
    const Item::List children = visibleChildren();
    if (children.size() <= separatorIndex) {
        // Doesn't happen
        qWarning() << Q_FUNC_INFO << "Not enough children for separator index" << separator
                   << this << separatorIndex;
        root()->dumpLayout();
        return;
    }

    int remainingToTake = qAbs(delta);
    int tookLocally = 0;

    if (moveDirection == Side1) {
        // Separator is moving left (or top if horizontal)

        // This is the available within our container, which we can use without bothering other other separators
        Item *side2Neighbour = m_children[separatorIndex + 1];
        const int available1 = availableOnSide(side2Neighbour, Side1);
        tookLocally = qMin(available1, remainingToTake);

        if (tookLocally != 0) {
            growItem(side2Neighbour, tookLocally, GrowthStrategy::Side1Only,
                     NeighbourSqueezeStrategy::ImmediateNeighboursFirst, false,
                     ChildrenResizeStrategy::Side1SeparatorMove);
        }

    } else {
        // Separator is moving right (or bottom if horizontal)
        Item *side1Neighbour = m_children[separatorIndex];
        const int available2 = availableOnSide(side1Neighbour, Side2);
        tookLocally = qMin(available2, remainingToTake);
        if (tookLocally != 0) {
            growItem(side1Neighbour, tookLocally, GrowthStrategy::Side2Only,
                     NeighbourSqueezeStrategy::ImmediateNeighboursFirst, false,
                     ChildrenResizeStrategy::Side2SeparatorMove);
        }
    }

    remainingToTake -= tookLocally;

    if (remainingToTake > 0) {
        // Go up the hierarchy and move the next separator on the left
        if (Q_UNLIKELY(isRoot())) {
            // Doesn't happen
            qWarning() << Q_FUNC_INFO << "Not enough space to move separator"
                       << this;
        } else {
            Separator *nextSeparator = parentContainer()->neighbourSeparator(this, moveDirection, m_orientation);
            // nextSeparator might not belong to parentContainer(), due to different orientation
            const int remainingDelta = moveDirection == Side1 ? -remainingToTake : remainingToTake;
            nextSeparator->parentContainer()->requestSeparatorMove(nextSeparator, remainingDelta);
        }
    }
}

void ItemContainer::requestEqualSize(Separator *separator)
{
    const int separatorIndex = m_separators.indexOf(separator);
    if (separatorIndex == -1) {
        // Doesn't happen
        qWarning() << Q_FUNC_INFO << "Separator not found" << separator;
        return;
    }

    const Item::List children = visibleChildren();
    Item *side1Item = children.at(separatorIndex);
    Item *side2Item = children.at(separatorIndex + 1);

    const int length1 = side1Item->length(m_orientation);
    const int length2 = side2Item->length(m_orientation);

    if (qAbs(length1 - length2) <= 1) {
        // items already have the same length, nothing to do.
        // We allow for a difference of 1px, since you can't split that.
        return;
    }

    const int newLength = (length1 + length2) / 2;

    int delta = 0;
    if (length1 < newLength) {
        // Let's move separator to the right
        delta = newLength - length1;
    } else if (length2 < newLength) {
        // or left.
        delta = -(newLength - length2); // negative, since separator is going left
    }

    // Do some bounds checking, to respect min-sizes
    const int min = minPosForSeparator_global(separator);
    const int max = maxPosForSeparator_global(separator);
    const int newPos = qBound(min, separator->position() + delta, max);

    // correct the delta
    delta = newPos - separator->position();

    if (delta != 0)
        requestSeparatorMove(separator, delta);
}

Item *ItemContainer::visibleNeighbourFor(const Item *item, Side side) const
{
    // Item might not be visible, so use m_children instead of visibleChildren()
    const int index = m_children.indexOf(const_cast<Item*>(item));

    if (side == Side1) {
        for (int i = index - 1; i >= 0; i--) {
            Item *item = m_children.at(i);
            if (item->isVisible())
                return item;
        }
    } else {
        for (int i = index + 1; i < m_children.size(); ++i) {
            Item *item = m_children.at(i);
            if (item->isVisible())
                return item;
        }
    }

    return nullptr;
}

QSize ItemContainer::availableSize() const
{
    return size() - this->minSize();
}

int ItemContainer::availableLength() const
{
    return isVertical() ? availableSize().height()
                        : availableSize().width();
}

ItemContainer::LengthOnSide ItemContainer::lengthOnSide(const SizingInfo::List &sizes, int fromIndex,
                                                        Side side, Qt::Orientation o) const
{
    if (fromIndex < 0)
        return {};

    const int count = sizes.count();
    if (fromIndex >= count)
        return {};

    int start = 0;
    int end = -1;
    if (side == Side1) {
        start = 0;
        end = fromIndex;
    } else {
        start = fromIndex;
        end = count - 1;

    }

    LengthOnSide result;
    for (int i = start; i <= end; ++i) {
        const SizingInfo &size = sizes.at(i);
        result.length += size.length(o);
        result.minLength += size.minLength(o);
    }

    return result;
}

int ItemContainer::neighboursLengthFor(const Item *item, Side side, Qt::Orientation o) const
{
    const Item::List children = visibleChildren();
    const int index = children.indexOf(const_cast<Item*>(item));
    if (index == -1) {
        qWarning() << Q_FUNC_INFO << "Couldn't find item" << item;
        return 0;
    }

    if (o == m_orientation) {
        int neighbourLength = 0;
        int start = 0;
        int end = -1;
        if (side == Side1) {
            start = 0;
            end = index - 1;
        } else {
            start = index + 1;
            end = children.size() - 1;
        }

        for (int i = start; i <= end; ++i)
            neighbourLength += children.at(i)->length(m_orientation);

        return neighbourLength;
    } else {
        // No neighbours in the other orientation. Each container is bidimensional.
        return 0;
    }
}

int ItemContainer::neighboursLengthFor_recursive(const Item *item, Side side, Qt::Orientation o) const
{
    return neighboursLengthFor(item, side, o) + (isRoot() ? 0
                                                          : parentContainer()->neighboursLengthFor_recursive(this, side, o));

}

int ItemContainer::neighboursMinLengthFor(const Item *item, Side side, Qt::Orientation o) const
{
    const Item::List children = visibleChildren();
    const int index = children.indexOf(const_cast<Item*>(item));
    if (index == -1) {
        qWarning() << Q_FUNC_INFO << "Couldn't find item" << item;
        return 0;
    }

    if (o == m_orientation) {
        int neighbourMinLength = 0;
        int start = 0;
        int end = -1;
        if (side == Side1) {
            start = 0;
            end = index - 1;
        } else {
            start = index + 1;
            end = children.size() - 1;
        }

        for (int i = start; i <= end; ++i)
            neighbourMinLength += children.at(i)->minLength(m_orientation);

        return neighbourMinLength;
    } else {
        // No neighbours here
        return 0;
    }
}

int ItemContainer::availableOnSide(const Item *child, Side side) const
{
    const int length = neighboursLengthFor(child, side, m_orientation);
    const int min = neighboursMinLengthFor(child, side, m_orientation);

    const int available = length - min;
    if (available < 0) {
        root()->dumpLayout();
        Q_ASSERT(false);
    }
    return available;
}

int ItemContainer::availableOnSide_recursive(const Item *child, Side side, Qt::Orientation orientation) const
{
    if (orientation == m_orientation) {
        const int available = availableOnSide(child, side);
        return isRoot() ? available
                        : (available + parentContainer()->availableOnSide_recursive(this, side, orientation));
    } else {
        return isRoot() ? 0
                        : parentContainer()->availableOnSide_recursive(this, side, orientation);
    }
}

QVariantList ItemContainer::items() const
{
    QVariantList items;
    items.reserve(m_children.size());

    for (auto item : m_children)
        items << QVariant::fromValue(item);

    return items;
}

void ItemContainer::growNeighbours(Item *side1Neighbour, Item *side2Neighbour)
{
    if (!side1Neighbour && !side2Neighbour)
        return;

    if (side1Neighbour && side2Neighbour) {
        // Give half/half to each neighbour
        QRect geo1 = side1Neighbour->geometry();
        QRect geo2 = side2Neighbour->geometry();

        if (isVertical()) {
            const int available = geo2.y() - geo1.bottom() - separatorThickness;
            geo1.setHeight(geo1.height() + available / 2);
            geo2.setTop(geo1.bottom() + separatorThickness + 1);
        } else {
            const int available = geo2.x() - geo1.right() - separatorThickness;
            geo1.setWidth(geo1.width() + available / 2);
            geo2.setLeft(geo1.right() + separatorThickness + 1);
        }

        side1Neighbour->setGeometry_recursive(geo1);
        side2Neighbour->setGeometry_recursive(geo2);

    } else if (side1Neighbour) {
        // Grow all the way to the right (or bottom if vertical)
        QRect geo = side1Neighbour->geometry();

        if (isVertical()) {
            geo.setBottom(rect().bottom());
        } else {
            geo.setRight(rect().right());
        }

        side1Neighbour->setGeometry_recursive(geo);

    } else if (side2Neighbour) {
        // Grow all the way to the left (or top if vertical)
        QRect geo = side2Neighbour->geometry();

        if (isVertical()) {
            geo.setTop(0);
        } else {
            geo.setLeft(0);
        }

        side2Neighbour->setGeometry_recursive(geo);
    }
}

void ItemContainer::growItem(int index, SizingInfo::List &sizes, int missing,
                             GrowthStrategy growthStrategy,
                             NeighbourSqueezeStrategy neighbourSqueezeStrategy,
                             bool accountForNewSeparator)
{
    int toSteal = missing; // The amount that neighbours of @p index will shrink
    if (accountForNewSeparator)
        toSteal += Item::separatorThickness;

    Q_ASSERT(index != -1);
    if (toSteal == 0)
        return;

    // #1. Grow our item
    SizingInfo &sizingInfo = sizes[index];
    sizingInfo.setLength(sizingInfo.length(m_orientation) + missing, m_orientation);
    sizingInfo.setOppositeLength(oppositeLength(), m_orientation);

    int side1Growth = 0;
    int side2Growth = 0;

    if (growthStrategy == GrowthStrategy::BothSidesEqually) {
        const int count = sizes.count();
        if (count == 1) {
            //There's no neighbours to push, we're alone. Occupy the full container
            sizingInfo.incrementLength(missing, m_orientation);
            return;
        }

        // #2. Now shrink the neigbours by the same amount. Calculate how much to shrink from each side
        const LengthOnSide side1Length = lengthOnSide(sizes, index - 1, Side1, m_orientation);
        const LengthOnSide side2Length = lengthOnSide(sizes, index + 1, Side2, m_orientation);

        int available1 = side1Length.available();
        int available2 = side2Length.available();

        if (toSteal > available1 + available2) {
            root()->dumpLayout();
            Q_ASSERT(false);
        }

        while (toSteal > 0) {
            if (available1 == 0) {
                Q_ASSERT(available2 >= toSteal);
                side2Growth += toSteal;
                break;
            } else if (available2 == 0) {
                Q_ASSERT(available1 >= toSteal);
                side1Growth += toSteal;
                break;
            }

            const int toTake = qMax(1, toSteal / 2);
            const int took1 = qMin(toTake, available1);
            toSteal -= took1;
            available1 -= took1;
            side1Growth += took1;
            if (toSteal == 0)
                break;

            const int took2 = qMin(toTake, available2);
            toSteal -= took2;
            side2Growth += took2;
            available2 -= took2;
        }
    } else if (growthStrategy == GrowthStrategy::Side1Only) {
        side1Growth = missing;
        side2Growth = 0;
    } else if (growthStrategy == GrowthStrategy::Side2Only) {
        side1Growth = 0;
        side2Growth = missing;
    }

    shrinkNeighbours(index, sizes, side1Growth, side2Growth, neighbourSqueezeStrategy);
}

void ItemContainer::growItem(Item *item, int amount, GrowthStrategy growthStrategy,
                             NeighbourSqueezeStrategy neighbourSqueezeStrategy,
                             bool accountForNewSeparator,
                             ChildrenResizeStrategy childResizeStrategy)
{
    const Item::List items = visibleChildren();
    const int index = items.indexOf(item);
    SizingInfo::List sizes = this->sizes();

    growItem(index, /*by-ref=*/sizes, amount, growthStrategy, neighbourSqueezeStrategy, accountForNewSeparator);

    applyGeometries(sizes, childResizeStrategy);
}

void ItemContainer::applyGeometries(const SizingInfo::List &sizes, ChildrenResizeStrategy strategy)
{
    const Item::List items = visibleChildren();
    const int count = items.size();
    Q_ASSERT(count == sizes.size());

    for (int i = 0; i < count; ++i) {
        Item *item = items.at(i);
        item->setSize_recursive(sizes[i].geometry.size(), strategy);
    }

    positionItems();
}

SizingInfo::List ItemContainer::sizes(bool ignoreBeingInserted) const
{
    const Item::List children = visibleChildren(ignoreBeingInserted);
    SizingInfo::List result;
    result.reserve(children.count());
    for (Item *item : children) {
        if (item->isContainer())
            item->m_sizingInfo.minSize = item->minSize();
        result << item->m_sizingInfo;
    }

    return result;
}

QVector<int> ItemContainer::calculateSqueezes(SizingInfo::List::ConstIterator begin,
                                              SizingInfo::List::ConstIterator end, int needed,
                                              NeighbourSqueezeStrategy strategy, bool reversed) const
{
    QVector<int> availabilities;
    for (auto it = begin; it < end; ++it) {
        availabilities << it->availableLength(m_orientation);
    }

    const int count = availabilities.count();

    QVector<int> squeezes(count, 0);
    int missing = needed;

    if (strategy == NeighbourSqueezeStrategy::AllNeighbours) {
        while (missing > 0) {
            const int numDonors = std::count_if(availabilities.cbegin(), availabilities.cend(), [] (int num) {
                return num > 0;
            });

            if (numDonors == 0) {
                root()->dumpLayout();
                Q_ASSERT(false);
                return {};
            }

            int toTake = missing / numDonors;
            if (toTake == 0)
                toTake = missing;

            for (int i = 0; i < count; ++i) {
                const int available = availabilities.at(i);
                if (available == 0)
                    continue;
                const int took = qMin(missing, qMin(toTake, available));
                availabilities[i] -= took;
                missing -= took;
                squeezes[i] += took;
                if (missing == 0)
                    break;
            }
        }
    } else if (strategy == NeighbourSqueezeStrategy::ImmediateNeighboursFirst) {
        for (int i = 0; i < count; i++) {
            const int index = reversed ? count - 1 - i : i;

            const int available = availabilities.at(index);
            if (available > 0) {
                const int took = qMin(missing, available);
                missing -= took;
                squeezes[index] += took;
            }

            if (missing == 0)
                break;
        }
    }

    if (missing < 0) {
        // Doesn't really happen
        qWarning() << Q_FUNC_INFO << "Missing is negative" << missing
                   << squeezes;
    }

    return squeezes;
}

void ItemContainer::shrinkNeighbours(int index, SizingInfo::List &sizes, int side1Amount,
                                     int side2Amount, NeighbourSqueezeStrategy strategy)
{
    Q_ASSERT(side1Amount > 0 || side2Amount > 0);
    Q_ASSERT(side1Amount >= 0 && side2Amount >= 0); // never negative

    if (side1Amount > 0) {
        auto begin = sizes.cbegin();
        auto end = sizes.cbegin() + index;
        const bool reversed = strategy == NeighbourSqueezeStrategy::ImmediateNeighboursFirst;
        const QVector<int> squeezes = calculateSqueezes(begin, end, side1Amount, strategy, reversed);
        for (int i = 0; i < squeezes.size(); ++i) {
            const int squeeze = squeezes.at(i);
            SizingInfo &sizing = sizes[i];
            // setSize() or setGeometry() have the same effect here, we don't care about the position yet. That's done in positionItems()
            sizing.setSize(adjustedRect(sizing.geometry, m_orientation, 0, -squeeze).size());
        }
    }

    if (side2Amount > 0) {
        auto begin = sizes.cbegin() + index + 1;
        auto end = sizes.cend();

        const QVector<int> squeezes = calculateSqueezes(begin, end, side2Amount, strategy);
        for (int i = 0; i < squeezes.size(); ++i) {
            const int squeeze = squeezes.at(i);
            SizingInfo &sizing = sizes[i + index + 1];
            sizing.setSize(adjustedRect(sizing.geometry, m_orientation, squeeze, 0).size());
        }
    }
}

QVector<int> ItemContainer::requiredSeparatorPositions() const
{
    const int numSeparators = qMax(0, numVisibleChildren() - 1);
    QVector<int> positions;
    positions.reserve(numSeparators);

    for (Item *item : m_children) {
        if (positions.size() == numSeparators)
            break;

        if (item->isVisible()) {
            const int localPos = item->m_sizingInfo.edge(m_orientation) + 1;
            positions << mapToRoot(localPos, m_orientation);
        }
    }

    return positions;
}

void ItemContainer::updateSeparators()
{
    if (!hostWidget())
        return;

    const QVector<int> positions = requiredSeparatorPositions();
    const int requiredNumSeparators = positions.size();

    const bool numSeparatorsChanged = requiredNumSeparators != m_separators.size();
    if (numSeparatorsChanged) {
        // Instead of just creating N missing ones at the end of the list, let's minimize separators
        // having their position changed, to minimize flicker
        Separator::List newSeparators;
        newSeparators.reserve(requiredNumSeparators);

        for (int position : positions) {
            Separator *separator = separatorAt(position);
            if (separator) {
                // Already existing, reuse
                newSeparators.push_back(separator);
                m_separators.removeOne(separator);
            } else {
                separator = Separator::createSeparator(hostWidget());
                separator->init(this, m_orientation);
                newSeparators.push_back(separator);
            }
        }

        // delete what remained, which is unused
        deleteSeparators();

        m_separators = newSeparators;
    }

    // Update their positions:
    const int pos2 = isVertical() ? mapToRoot(QPoint(0, 0)).x()
                                  : mapToRoot(QPoint(0, 0)).y();

    int i = 0;
    for (int position : positions) {
        m_separators.at(i)->setGeometry(position, pos2, oppositeLength());
        i++;
    }

    updateChildPercentages();
}

void ItemContainer::deleteSeparators()
{
    qDeleteAll(m_separators);
    m_separators.clear();
}

void ItemContainer::deleteSeparators_recursive()
{
    deleteSeparators();

    // recurse into the children:
    for (Item *item : m_children) {
        if (auto c = item->asContainer())
            c->deleteSeparators_recursive();
    }
}

void ItemContainer::updateSeparators_recursive()
{
    updateSeparators();

    // recurse into the children:
    const Item::List items = visibleChildren();
    for (Item *item : items) {
        if (auto c = item->asContainer())
            c->updateSeparators_recursive();
    }
}

Separator *ItemContainer::separatorAt(int p) const
{
    for (Separator *separator : m_separators) {
        if (separator->position() == p)
            return separator;
    }

    return nullptr;
}

bool ItemContainer::isVertical() const
{
    return m_orientation == Qt::Vertical;
}

bool ItemContainer::isHorizontal() const
{
    return m_orientation == Qt::Horizontal;
}

int ItemContainer::indexOf(Separator *separator) const
{
    return m_separators.indexOf(separator);
}

int ItemContainer::minPosForSeparator(Separator *separator) const
{
    const int globalMin = minPosForSeparator_global(separator);
    return mapFromRoot(globalMin, m_orientation);
}

int ItemContainer::maxPosForSeparator(Separator *separator) const
{
    const int globalMax = maxPosForSeparator_global(separator);
    return mapFromRoot(globalMax, m_orientation);
}

int ItemContainer::minPosForSeparator_global(Separator *separator) const
{
    const int separatorIndex = indexOf(separator);
    Q_ASSERT(separatorIndex != -1);

    const Item::List children = visibleChildren();
    Q_ASSERT(separatorIndex + 1 < children.size());
    Item *item = children.at(separatorIndex + 1);

    const int available1 = availableOnSide_recursive(item, Side1, m_orientation);
    return separator->position() - available1;
}

int ItemContainer::maxPosForSeparator_global(Separator *separator) const
{
    const int separatorIndex = indexOf(separator);
    Q_ASSERT(separatorIndex != -1);

    const Item::List children = visibleChildren();
    Item *item = children.at(separatorIndex);

    const int available2 = availableOnSide_recursive(item, Side2, m_orientation);
    return separator->position() + available2;
}

QVariantMap ItemContainer::toVariantMap() const
{
    QVariantMap result = Item::toVariantMap();

    QVariantList childrenV;
    childrenV.reserve(m_children.size());
    for (Item *child : m_children) {
        childrenV.push_back(child->toVariantMap());
    }

    result[QStringLiteral("children")] = childrenV;
    result[QStringLiteral("orientation")] = m_orientation;

    return result;
}

void ItemContainer::fillFromVariantMap(const QVariantMap &map,
                                       const QHash<QString, GuestInterface*> &widgets)
{
    QScopedValueRollback<bool> deserializing(m_isDeserializing, true);

    Item::fillFromVariantMap(map, widgets);
    const QVariantList childrenV = map[QStringLiteral("children")].toList();
    m_orientation = Qt::Orientation(map[QStringLiteral("orientation")].toInt());

    for (const QVariant &childV : childrenV) {
        const QVariantMap childMap = childV.toMap();
        const bool isContainer = childMap[QStringLiteral("isContainer")].toBool();
        Item *child = isContainer ? new ItemContainer(hostWidget(), this)
                                  : new Item(hostWidget(), this);
        child->fillFromVariantMap(childMap, widgets);
        m_children.push_back(child);
    }

    if (isRoot()) {
        updateChildPercentages_recursive();
        if (hostWidget()) {
            updateSeparators_recursive();
            updateWidgets_recursive();
        }
        Q_EMIT minSizeChanged(this);
#ifdef DOCKS_DEVELOPER_MODE
    if (!checkSanity())
        qWarning() << Q_FUNC_INFO << "Resulting layout is invalid";
#endif
    }
}

bool ItemContainer::isDummy() const
{
    return hostWidget() == nullptr;
}

#ifdef DOCKS_DEVELOPER_MODE
bool ItemContainer::test_suggestedRect()
{
    auto itemToDrop = new Item(hostWidget());

    const Item::List children = visibleChildren();
    for (Item *relativeTo : children) {
        if (auto c = relativeTo->asContainer()) {
            c->test_suggestedRect();
        } else {
            QHash<Location, QRect> rects;
            for (Location loc : { Location_OnTop, Location_OnLeft, Location_OnRight, Location_OnBottom}) {
                const QRect rect = suggestedDropRect(itemToDrop, relativeTo, loc);
                rects.insert(loc, rect);
                if (rect.isEmpty()) {
                    qWarning() << Q_FUNC_INFO << "Empty rect";
                    return false;
                } else if (!root()->rect().contains(rect)) {
                    root()->dumpLayout();
                    qWarning() << Q_FUNC_INFO << "Suggested rect is out of bounds" << rect
                               << "; loc=" << loc << "; relativeTo=" << relativeTo;
                    return false;
                }
            }
            if (rects.value(Location_OnBottom).y() <= rects.value(Location_OnTop).y() ||
                rects.value(Location_OnRight).x() <= rects.value(Location_OnLeft).x()) {
                root()->dumpLayout();
                qWarning() << Q_FUNC_INFO << "Invalid suggested rects" << rects
                           << this << "; relativeTo=" << relativeTo;
                return false;
            }
        }
    }

    delete itemToDrop;
    return true;
}
#endif

QVector<Separator *> ItemContainer::separators_recursive() const
{
    Layouting::Separator::List separators = m_separators;

    for (Item *item : m_children) {
        if (auto c = item->asContainer())
            separators << c->separators_recursive();
    }

    return separators;
}

QVector<Separator *> ItemContainer::separators() const
{
    return m_separators;
}

const Item *ItemContainer::itemFromPath(const QVector<int> &path) const
{
    const ItemContainer *container = this;

    for (int i = 0; i < path.size() ; ++i) {
        const int index = path[i];
        const bool isLast = i == path.size() - 1;
        if (index < 0 || index >= container->m_children.size()) {
            // Doesn't happen
            root()->dumpLayout();
            qWarning() << Q_FUNC_INFO << "Invalid index" << index
                       << this << path << isRoot();
            return nullptr;
        }

        if (isLast) {
            return container->m_children.at(index);
        } else {
            container = container->m_children.at(index)->asContainer();
            if (!container) {
                qWarning() << Q_FUNC_INFO << "Invalid index" << path;
                return nullptr;
            }
        }
    }

    return this;
}

Separator *ItemContainer::neighbourSeparator(const Item *item, Side side, Qt::Orientation orientation) const
{
    Item::List children = visibleChildren();
    const int itemIndex = children.indexOf(const_cast<Item *>(item));
    if (itemIndex == -1) {
        qWarning() << Q_FUNC_INFO << "Item not found" << item
                   << this;
        root()->dumpLayout();
        return nullptr;
    }

    if (orientation != this->orientation()) {
        // Go up
        if (isRoot()) {
            return nullptr;
        } else {
            return parentContainer()->neighbourSeparator(this, side, orientation);
        }
    }

    const int separatorIndex = side == Side1 ? itemIndex -1
                                             : itemIndex;

    if (separatorIndex < 0 || separatorIndex >= m_separators.size())
        return nullptr;

    return m_separators[separatorIndex];
}

void ItemContainer::updateWidgets_recursive()
{
    for (Item *item : m_children) {

        if (auto c = item->asContainer()) {
            c->updateWidgets_recursive();
        } else {
            if (item->isVisible()) {
                if (QWidget *widget = item->widget()) {
                    widget->setGeometry(mapToRoot(item->geometry()));
                    widget->setVisible(true);
                } else {
                    qWarning() << Q_FUNC_INFO << "visible item doesn't have a guest"
                               << item;
                }
            }
        }
    }
}

QVariantMap SizingInfo::toVariantMap() const
{
    QVariantMap result;
    result[QStringLiteral("geometry")] = rectToMap(geometry);
    result[QStringLiteral("minSize")] = sizeToMap(minSize);
    result[QStringLiteral("maxSize")] = sizeToMap(maxSize);
    return result;
}

void SizingInfo::fromVariantMap(const QVariantMap &map)
{
    *this = SizingInfo(); // reset any non-important fields to their default
    geometry = mapToRect(map[QStringLiteral("geometry")].toMap());
    minSize = mapToSize(map[QStringLiteral("minSize")].toMap());
    maxSize = mapToSize(map[QStringLiteral("maxSize")].toMap());
}

int ItemContainer::Private::defaultLengthFor(Item *item, DefaultSizeMode mode) const
{
    int result = 0;
    switch (mode) {
    case DefaultSizeMode::None:
        break;
    case DefaultSizeMode::Fair: {
        const int numVisibleChildren = q->numVisibleChildren() + 1; // +1 so it counts with @p item too, which we're adding
        const int usableLength = q->length() - (Item::separatorThickness*(numVisibleChildren - 1));
        result = usableLength / numVisibleChildren;
        break;
    }
    case DefaultSizeMode::FairButFloor: {
        int length = defaultLengthFor(item, DefaultSizeMode::Fair);
        result = qMin(length, item->length(q->m_orientation));
        break;
    }
    case DefaultSizeMode::ItemSize:
        result = item->length(q->m_orientation);
        break;
    case DefaultSizeMode::SizePolicy:
        qWarning() << Q_FUNC_INFO << "Now implemented yet";
        break;
    }

    result = qMax(item->minLength(q->m_orientation), result); // bound with max-size too
    return result;
}

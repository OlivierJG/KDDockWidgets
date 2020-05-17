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

#pragma once

#include <QObject>
#include <QVector>
#include <QRect>
#include <QVariant>
#include <QWidget>
#include <QDebug>
#include <QVariant>

#include <memory>

#define KDDOCKWIDGETS_MIN_WIDTH 80
#define KDDOCKWIDGETS_MIN_HEIGHT 90

#define KDDOCKWIDGETS_MAX_WIDTH 16777215
#define KDDOCKWIDGETS_MAX_HEIGHT 16777215

class TestMultiSplitter;

namespace Layouting {
Q_NAMESPACE

class ItemContainer;
class Item;
class Separator;

enum Location {
    Location_None,
    Location_OnLeft, ///> Left docking location
    Location_OnTop,  ///> Top docking location
    Location_OnRight, ///> Right docking location
    Location_OnBottom ///> Bottom docking location
};
Q_ENUM_NS(Location);

enum AddingOption {
    AddingOption_None = 0, ///> No option set
    AddingOption_StartHidden ///< Don't show the dock widget when adding it
};

enum Side {
    Side1,
    Side2
};

enum class GrowthStrategy {
    BothSidesEqually,
    Side1Only,
    Side2Only
};

enum class SeparatorOption {
    None = 0,
    LazyResize
};
Q_DECLARE_FLAGS(SeparatorOptions, SeparatorOption)

enum class ChildrenResizeStrategy {
    Percentage, ///< Resizes the container in a way that all children will keep occupying the same percentage
    Side1SeparatorMove, ///< When resizing a container, it takes/adds space from Side1 children first
    Side2SeparatorMove ///< When resizing a container, it takes/adds space from Side2 children first
};

enum class NeighbourSqueezeStrategy {
    AllNeighbours, ///< The squeeze is spread between all neighbours, not just immediate ones first
    ImmediateNeighboursFirst ///< The first neighbour takes as much squeeze as it can, only then the next neighbour is squezed, and so forth
};

///@brief When an item is added we need to figure out what's a decent size for it
///This enum specifies the different ways to calculate it
enum class DefaultSizeMode {
    ItemSize, ///< Simply uses the Item::size() of the item being added. Actual used size might be smaller if our window isn't big enough.
    Fair, ///< Gives an equal relative size as the items that are already in the layout
    FairButFloor, ///< Equal to fair, but if the item is smaller than the fair suggestion, then that small size is used.
    SizePolicy, ///< Uses the item's sizeHint() and sizePolicy()
    None, ///< Don't do any sizing
};

inline Qt::Orientation oppositeOrientation(Qt::Orientation o) {
    return o == Qt::Vertical ? Qt::Horizontal
                             : Qt::Vertical;
}

inline int pos(QPoint p, Qt::Orientation o) {
    return o == Qt::Vertical ? p.y()
                             : p.x();
}

inline int length(QSize sz, Qt::Orientation o) {
    return o == Qt::Vertical ? sz.height()
                             : sz.width();
}

inline bool locationIsVertical(Location loc)
{
    return loc == Location_OnTop || loc == Location_OnBottom;
}

inline bool locationIsSide1(Location loc)
{
    return loc == Location_OnLeft || loc == Location_OnTop;
}

inline QRect adjustedRect(QRect r, Qt::Orientation o, int p1, int p2)
{
    if (o == Qt::Vertical) {
        r.adjust(0, p1, 0, p2);
    } else {
        r.adjust(p1, 0, p2, 0);
    }

    return r;
}

inline QVariantMap sizeToMap(QSize sz)
{
    QVariantMap map;
    map.insert(QStringLiteral("width"), sz.width());
    map.insert(QStringLiteral("height"), sz.height());

    return map;
}

inline QVariantMap rectToMap(QRect rect)
{
    QVariantMap map;
    map.insert(QStringLiteral("x"), rect.x());
    map.insert(QStringLiteral("y"), rect.y());
    map.insert(QStringLiteral("width"), rect.width());
    map.insert(QStringLiteral("height"), rect.height());

    return map;
}

inline QSize mapToSize(const QVariantMap &map)
{
    return { map.value(QStringLiteral("width")).toInt(),
             map.value(QStringLiteral("height")).toInt() };
}

inline QRect mapToRect(const QVariantMap &map)
{
    return QRect(map.value(QStringLiteral("x")).toInt(),
                 map.value(QStringLiteral("y")).toInt(),
                 map.value(QStringLiteral("width")).toInt(),
                 map.value(QStringLiteral("height")).toInt());
}

inline Qt::Orientation orientationForLocation(Location loc)
{
    switch (loc) {
    case Location_OnLeft:
    case Location_OnRight:
        return Qt::Horizontal;
    case Location_None:
    case Location_OnTop:
    case Location_OnBottom:
        return Qt::Vertical;
    }

    return Qt::Vertical;
}

inline Side sideForLocation(Location loc)
{
    switch (loc) {
    case Location_OnLeft:
    case Location_OnTop:
        return Side::Side1;
    case Location_OnRight:
    case Location_OnBottom:
        return Side::Side2;
    default:
        return Side::Side1;
    }
}

struct SizingInfo {
    QSize size() const {
        return geometry.size();
    }

    void setSize(QSize sz)
    {
        geometry.setSize(sz);
    }

    int length(Qt::Orientation o) const {
        return Layouting::length(size(), o);
    }

    int minLength(Qt::Orientation o) const {
        return Layouting::length(minSize, o);
    }

    int availableLength(Qt::Orientation o) const {
        return qMax(0, length(o) - minLength(o));
    }

    int missingLength(Qt::Orientation o) const {
        return qMax(0,  minLength(o) - length(o));
    }

    QPoint pos() const {
        return geometry.topLeft();
    }

    int position(Qt::Orientation o) const {
        return Layouting::pos(pos(), o);
    }

    int edge(Qt::Orientation o) const {
        return o == Qt::Vertical ? geometry.bottom()
                                 : geometry.right();
    }

    void setLength(int l, Qt::Orientation o) {
        if (o == Qt::Vertical) {
            geometry.setHeight(l);
        } else {
            geometry.setWidth(l);
        }
    }

    void incrementLength(int byAmount, Qt::Orientation o) {
        setLength(length(o) + byAmount, o);
    }

    void setOppositeLength(int l, Qt::Orientation o) {
        setLength(l, oppositeOrientation(o));
    }

    void setPos(int p, Qt::Orientation o) {
        if (o == Qt::Vertical)
            geometry.moveTop(p);
        else
            geometry.moveLeft(p);
    }

    bool isNull() const {
        return geometry.isNull();
    }

    void setGeometry(QRect geo) {
        geometry = geo;
    }

    QVariantMap toVariantMap() const;
    void fromVariantMap(const QVariantMap &);

    typedef QVector<SizingInfo> List;
    QRect geometry;
    QSize minSize = QSize(KDDOCKWIDGETS_MIN_WIDTH, KDDOCKWIDGETS_MIN_HEIGHT);
    QSize maxSize = QSize(KDDOCKWIDGETS_MAX_WIDTH, KDDOCKWIDGETS_MAX_HEIGHT); // TODO: Not supported yet
    double percentageWithinParent = 0.0;
    bool isBeingInserted = false;
};

class GuestInterface
{
public:
    GuestInterface() = default;
    virtual void setLayoutItem(Item *) = 0;
    virtual QWidget *asWidget() = 0;
private:
    Q_DISABLE_COPY(GuestInterface)
};

class Item : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int x READ x NOTIFY xChanged)
    Q_PROPERTY(int y READ y NOTIFY yChanged)
    Q_PROPERTY(int width READ width NOTIFY widthChanged)
    Q_PROPERTY(int height READ height NOTIFY heightChanged)
    Q_PROPERTY(QRect geometry READ geometry NOTIFY geometryChanged)
    Q_PROPERTY(bool isContainer READ isContainer CONSTANT)
public:
    typedef QVector<Item*> List;

    explicit Item(QWidget *hostWidget, ItemContainer *parent = nullptr);
    ~Item() override;

    bool isRoot() const;

    virtual int visibleCount_recursive() const;
    virtual void insertItem(Item *item, Location,
                            DefaultSizeMode defaultSizeMode = DefaultSizeMode::Fair,
                            AddingOption = AddingOption_None);

    /**
     * @brief No widget can have a minimum size smaller than this, regardless of their minimum size.
     */
    static const QSize hardcodedMinimumSize;
    static int separatorThickness;

    int x() const;
    int y() const;
    int width() const;
    int height() const;
    QSize size() const;
    void setSize(QSize);
    QPoint pos() const;
    int pos(Qt::Orientation) const;
    QRect geometry() const;
    QRect rect() const;
    bool isContainer() const;
    ItemContainer *parentContainer() const;
    void setMinSize(QSize);
    void setMaxSize(QSize);
    bool isPlaceholder() const;
    void setGeometry(QRect rect);
    ItemContainer *root() const;
    QRect mapToRoot(QRect) const;
    QPoint mapToRoot(QPoint) const;
    int mapToRoot(int p, Qt::Orientation) const;
    QPoint mapFromRoot(QPoint) const;
    QRect mapFromRoot(QRect) const;
    QPoint mapFromParent(QPoint) const;
    int mapFromRoot(int p, Qt::Orientation) const;

    QWidget *widget() const { return m_guest ? m_guest->asWidget() : nullptr; }
    GuestInterface *guest() const { return m_guest; }
    void setGuest(GuestInterface *);
    QWidget *window() const {
        return m_guest ? widget()->window() : nullptr;
    }

    void ref();
    void unref();
    int refCount() const;

    int minLength(Qt::Orientation) const;

    QWidget *hostWidget() const;
    void restore(GuestInterface *guest);

    QVector<int> pathFromRoot() const;

    virtual QSize minSize() const;
    virtual QSize maxSize() const;
    virtual void setSize_recursive(QSize newSize, ChildrenResizeStrategy strategy = ChildrenResizeStrategy::Percentage);
    virtual bool isVisible(bool excludeBeingInserted = false) const;
    virtual void setGeometry_recursive(QRect rect);
    virtual void dumpLayout(int level = 0);
    virtual void setHostWidget(QWidget *);
    virtual QVariantMap toVariantMap() const;
    virtual void fillFromVariantMap(const QVariantMap &map, const QHash<QString, GuestInterface*> &widgets);

    static Item* createFromVariantMap(QWidget *hostWidget, ItemContainer *parent,
                                      const QVariantMap &map, const QHash<QString, GuestInterface *> &widgets);

Q_SIGNALS:
    void geometryChanged();
    void xChanged();
    void yChanged();
    void widthChanged();
    void heightChanged();
    void visibleChanged(Layouting::Item *thisItem, bool visible);
    void minSizeChanged(Layouting::Item *thisItem);
protected:
    friend class ::TestMultiSplitter;
    explicit Item(bool isContainer, QWidget *hostWidget, ItemContainer *parent);
    void setParentContainer(ItemContainer *parent);
    void connectParent(ItemContainer *parent);
    Q_REQUIRED_RESULT virtual bool checkSanity();
    void setPos(QPoint);
    void setPos(int pos, Qt::Orientation);
    int position(Qt::Orientation) const;
    const ItemContainer *asContainer() const;
    ItemContainer *asContainer();
    void setLength(int length, Qt::Orientation);
    virtual void setLength_recursive(int length, Qt::Orientation);
    int length(Qt::Orientation) const;
    int availableLength(Qt::Orientation) const;
    QSize missingSize() const;
    virtual void updateWidgetGeometries();
    virtual void setIsVisible(bool);
    bool isBeingInserted() const;
    void setBeingInserted(bool);

    SizingInfo m_sizingInfo;
    const bool m_isContainer;
    ItemContainer *m_parent = nullptr;
private Q_SLOTS:
    void onWidgetLayoutRequested();

private:
    friend class ItemContainer;
    void turnIntoPlaceholder();
    bool eventFilter(QObject *o, QEvent *event) override;
    int m_refCount = 0;
    void updateObjectName();
    void onWidgetDestroyed();
    bool m_isVisible = false;
    QWidget *m_hostWidget = nullptr;
    GuestInterface *m_guest = nullptr;
};

class ItemContainer : public Item {
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
public:

    struct LengthOnSide {
        int length = 0;
        int minLength = 0;

        int available() const {
            return qMax(0, length - minLength);
        }

        int missing() const {
            return qMax(0, minLength - length);
        }
    };

    explicit ItemContainer(QWidget *hostWidget, ItemContainer *parent);
    explicit ItemContainer(QWidget *parent);
    ~ItemContainer();
    void insertItem(Item *item, int index, DefaultSizeMode);
    Q_REQUIRED_RESULT bool checkSanity() override;
    bool hasOrientation() const;
    int numChildren() const;
    int numVisibleChildren() const;
    bool hasChildren() const;
    bool hasVisibleChildren(bool excludeBeingInserted = false) const;
    int indexOfVisibleChild(const Item *) const;
    const List childItems() const;

    void removeItem(Item *, bool hardRemove = true);
    bool isEmpty() const;
    void setGeometry_recursive(QRect rect) override;

    ItemContainer *convertChildToContainer(Item *leaf);
    void insertItem(Item *item, Location, DefaultSizeMode defaultSizeMode = DefaultSizeMode::Fair,
                    AddingOption = AddingOption_None) override;
    bool hasOrientationFor(Location) const;
    Item::List visibleChildren(bool includeBeingInserted = false) const;
    int usableLength() const;
    bool hasSingleVisibleItem() const;
    bool contains(const Item *item) const;
    bool contains_recursive(const Item *item) const;
    void setChildren(const List &children, Qt::Orientation o);
    void setOrientation(Qt::Orientation);
    QSize minSize() const override;
    QSize maxSize() const override;
    void setSize_recursive(QSize newSize, ChildrenResizeStrategy strategy = ChildrenResizeStrategy::Percentage) override;
    int length() const;
    QRect rect() const;
    QVariantList items() const;
    void dumpLayout(int level = 0) override;
    void updateChildPercentages();
    void updateChildPercentages_recursive();
    void restoreChild(Item *,
                      NeighbourSqueezeStrategy neighbourSqueezeStrategy = NeighbourSqueezeStrategy::AllNeighbours);
    void updateWidgetGeometries() override;
    int oppositeLength() const;

    void requestSeparatorMove(Separator *separator, int delta);
    void requestEqualSize(Separator *separator);

    ///@brief Grows the side1Neighbour to the right and the side2Neighbour to the left
    ///So they occupy the empty space that's between them (or bottom/top if Qt::Vertical).
    ///This is useful when an Item is removed. Its neighbours will occupy its space.
    ///side1Neighbour or side2Neighbour are allowed to be null, in which case the non-null one
    ///will occupy the entire space.
    void growNeighbours(Item *side1Neighbour, Item *side2Neighbour);

    ///@brief grows an item by @p amount. It calculates how much to grow on side1 and on side2
    ///Then calls growItem(item, side1Growth, side2Growth) which will effectively grow it,
    ///and shrink the neighbours which are donating the size.
    void growItem(Item *, int amount, GrowthStrategy, NeighbourSqueezeStrategy neighbourSqueezeStrategy, bool accountForNewSeparator = false, ChildrenResizeStrategy = ChildrenResizeStrategy::Percentage);
    void growItem(int index, SizingInfo::List &sizes, int missing, GrowthStrategy,
                  NeighbourSqueezeStrategy neighbourSqueezeStrategy,
                  bool accountForNewSeparator = false);

    ///@brief Shrinks the neighbours of the item at @p index
    ///
    /// The neighbours at the left/top of the item, will be shrunk by @p side1Amount, while the items
    /// at right/bottom will be shrunk by @p side2Amount.
    /// Squeezes all the neighbours (not just the immediate ones).
    void shrinkNeighbours(int index, SizingInfo::List &sizes, int side1Amount, int side2Amount,
                          NeighbourSqueezeStrategy = NeighbourSqueezeStrategy::AllNeighbours);

    Item *visibleNeighbourFor(const Item *item, Side side) const;
    QSize availableSize() const;
    int availableLength() const;
    LengthOnSide lengthOnSide(const SizingInfo::List &sizes, int fromIndex, Side, Qt::Orientation) const;
    int neighboursLengthFor(const Item *item, Side, Qt::Orientation) const;
    int neighboursLengthFor_recursive(const Item *item, Side, Qt::Orientation) const;
    int neighboursMinLengthFor(const Item *item, Side, Qt::Orientation) const;
    int availableOnSide(const Item *child, Side) const;
    int availableOnSide_recursive(const Item *child, Side, Qt::Orientation orientation) const;
    void onChildMinSizeChanged(Item *child);
    void onChildVisibleChanged(Item *child, bool visible);
    void updateSizeConstraints();
    SizingInfo::List sizes(bool ignoreBeingInserted = false) const;
    QVector<int> calculateSqueezes(SizingInfo::List::ConstIterator begin,
                                   SizingInfo::List::ConstIterator end, int needed,
                                   NeighbourSqueezeStrategy, bool reversed = false) const;
    QRect suggestedDropRect(const Item *item, const Item *relativeTo, Location) const;
    QRect suggestedDropRectFallback(const Item *item, const Item *relativeTo, Location) const;
    void positionItems();
    void positionItems(SizingInfo::List &sizes);
    void clear();
    Item* itemForWidget(const QWidget *w) const;
    int visibleCount_recursive() const override;
    int count_recursive() const;
    Item *itemAt(QPoint p) const;
    Item *itemAt_recursive(QPoint p) const;
    Item::List items_recursive() const;
    void setHostWidget(QWidget *) override;
    void setIsVisible(bool) override;
    bool isVisible(bool excludeBeingInserted = false) const override;
    void setLength_recursive(int length, Qt::Orientation) override;
    void applyGeometries(const SizingInfo::List &sizes, ChildrenResizeStrategy = ChildrenResizeStrategy::Percentage);
    void applyPositions(const SizingInfo::List &sizes);
    Qt::Orientation orientation() const;
    bool isVertical() const;
    bool isHorizontal() const;

    int indexOf(Separator *) const;
    int minPosForSeparator(Separator *) const;
    int maxPosForSeparator(Separator *) const;
    int minPosForSeparator_global(Separator *) const;
    int maxPosForSeparator_global(Separator *) const;

    void deleteSeparators_recursive();
    void updateSeparators_recursive();

    QVariantMap toVariantMap() const override;
    void fillFromVariantMap(const QVariantMap &map, const QHash<QString, GuestInterface *> &widgets) override;

    bool isDummy() const;
#ifdef DOCKS_DEVELOPER_MODE
    bool test_suggestedRect();
#endif

Q_SIGNALS:
    void itemsChanged();
    void numVisibleItemsChanged(int);
    void numItemsChanged();
public:
    Item::List m_children;
    bool m_isResizing = false;
    bool m_blockUpdatePercentages = false;
    bool m_isDeserializing = false;
    QVector<Layouting::Separator*> separators_recursive() const;
    QVector<Layouting::Separator*> separators() const;
    Qt::Orientation m_orientation = Qt::Vertical;
private:
    const Item *itemFromPath(const QVector<int> &path) const;
    void resizeChildren(QSize oldSize, QSize newSize, SizingInfo::List &sizes, ChildrenResizeStrategy);
    void scheduleCheckSanity() const;
    Separator *neighbourSeparator(const Item *item, Side, Qt::Orientation) const;
    void updateWidgets_recursive();
    /// Returns the positions that each separator should have (x position if Qt::Horizontal, y otherwise)
    QVector<int> requiredSeparatorPositions() const;
    void updateSeparators();
    void deleteSeparators();
    Separator* separatorAt(int p) const;
    QVector<double> childPercentages() const;
    mutable bool m_checkSanityScheduled = false;
    QVector<Layouting::Separator*> m_separators;
    bool m_convertingItemToContainer = false;

    struct Private;
    Private *const d;
};

/**
 * Returns the widget's min size
 */
inline QSize widgetMinSize(const QWidget *w)
{
    const int minW = w->minimumWidth() > 0 ? w->minimumWidth()
                                           : w->minimumSizeHint().width();

    const int minH = w->minimumHeight() > 0 ? w->minimumHeight()
                                            : w->minimumSizeHint().height();

    return QSize(minW, minH).expandedTo(Item::hardcodedMinimumSize);
}

inline int widgetMinLength(const QWidget *w, Qt::Orientation o) {
    return length(widgetMinSize(w), o);
}

}

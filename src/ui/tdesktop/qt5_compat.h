#pragma once

#include <QtCore/qglobal.h>
#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtCore/QList>
#include <QtGui/qaccessible.h>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)

// 1. Resolve Qt 5 QByteArray != QLatin1String operator ambiguity in MSVC
inline bool operator!=(const QByteArray &a, QLatin1String b) {
    return a != b.latin1();
}

// 2. Resolve missing Qt 6 Accessibility Interfaces in standard Qt 5 headers
struct QAccessibleExt : public QAccessible {
    enum Attribute { Orientation };
    static constexpr InterfaceType SelectionInterface = static_cast<InterfaceType>(1000);
    static constexpr InterfaceType AttributesInterface = static_cast<InterfaceType>(1001);
};
#define QAccessible QAccessibleExt

class QAccessibleSelectionInterface {
public:
    virtual ~QAccessibleSelectionInterface() = default;
    virtual int selectedItemCount() const = 0;
    virtual QList<QAccessibleInterface*> selectedItems() const = 0;
    virtual QAccessibleInterface *selectedItem(int selectionIndex) const = 0;
    virtual bool isSelected(QAccessibleInterface *childItem) const = 0;
    virtual bool select(QAccessibleInterface *childItem) = 0;
    virtual bool unselect(QAccessibleInterface *childItem) = 0;
    virtual bool selectAll() = 0;
    virtual bool clear() = 0;
};

class QAccessibleAttributesInterface {
public:
    virtual ~QAccessibleAttributesInterface() = default;
    virtual QList<QAccessibleExt::Attribute> attributeKeys() const = 0;
    virtual QVariant attributeValue(QAccessibleExt::Attribute key) const = 0;
};

#endif

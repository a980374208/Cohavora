#include "accessible_combo_box.h"

#include <QtCore/QPersistentModelIndex>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QAccessible>
#include <QtWidgets/QAccessibleWidget>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QListView>

namespace MeetingUI {
class WhiteboardSelectionView final : public QListView {
    Q_OBJECT
public:
    explicit WhiteboardSelectionView(QComboBox *combo) : QListView(combo), combo(combo) {}
    QPointer<QComboBox> combo;

    QModelIndex option(int row) const {
        return combo && model() ? model()->index(row, combo->modelColumn(), rootIndex()) : QModelIndex();
    }
    bool selectable(const QModelIndex &index) const {
        return combo && combo->isEnabled() && isEnabled() && index.isValid()
            && index.model() == model() && index.parent() == rootIndex()
            && index.column() == combo->modelColumn()
            && (index.flags() & Qt::ItemIsEnabled) && (index.flags() & Qt::ItemIsSelectable);
    }
    bool commit(const QModelIndex &index) {
        if (!selectable(index)) return false;
        const QPersistentModelIndex stable(index);
        // Selecting a page can synchronously rebuild the entire model. Leave
        // the UIA provider's current interface stack before emitting signals.
        QTimer::singleShot(0, this, [this, stable] {
            if (!selectable(stable)) return;
            QPointer<QComboBox> target = combo;
            target->setCurrentIndex(stable.row());
            if (!target) return;
            target->hidePopup();
            const int row = target->currentIndex();
            const QString text = target->currentText();
            emit target->activated(row);
            if (target) emit target->activated(text);
        });
        return true;
    }
};

namespace {
class AccessibleOption final : public QAccessibleInterface,
        public QAccessibleTableCellInterface, public QAccessibleActionInterface {
public:
    AccessibleOption(WhiteboardSelectionView *view, const QModelIndex &index)
        : view_(view), index_(index) {}
    bool isValid() const override {
        return view_ && view_->combo && index_.isValid() && index_.model() == view_->model()
            && index_.parent() == view_->rootIndex() && index_.column() == view_->combo->modelColumn();
    }
    QObject *object() const override { return nullptr; }
    QAccessibleInterface *parent() const override { return view_ ? QAccessible::queryAccessibleInterface(view_) : nullptr; }
    QAccessibleInterface *child(int) const override { return nullptr; }
    QAccessibleInterface *childAt(int, int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface *) const override { return -1; }
    QAccessible::Role role() const override { return QAccessible::ListItem; }
    QString text(QAccessible::Text type) const override {
        if (!isValid()) return {};
        if (type == QAccessible::Name) {
            const auto accessible = index_.data(Qt::AccessibleTextRole).toString();
            return accessible.isEmpty() ? index_.data(Qt::DisplayRole).toString() : accessible;
        }
        return type == QAccessible::Description ? index_.data(Qt::AccessibleDescriptionRole).toString() : QString();
    }
    void setText(QAccessible::Text, const QString &) override {}
    QRect rect() const override {
        if (!isValid() || !view_->isVisible()) return {};
        const auto local = view_->visualRect(index_).intersected(view_->viewport()->rect());
        return local.isEmpty() ? QRect() : QRect(view_->viewport()->mapToGlobal(local.topLeft()), local.size());
    }
    QAccessible::State state() const override {
        QAccessible::State result;
        result.invalid = !isValid();
        if (result.invalid) return result;
        result.selectable = true;
        result.focusable = true;
        result.disabled = !view_->selectable(index_);
        result.selected = isSelected();
        result.focused = view_->hasFocus() && view_->currentIndex() == index_;
        result.offscreen = rect().isEmpty();
        return result;
    }
    void *interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::TableCellInterface) return static_cast<QAccessibleTableCellInterface *>(this);
        if (type == QAccessible::ActionInterface) return static_cast<QAccessibleActionInterface *>(this);
        return nullptr;
    }
    bool isSelected() const override { return isValid() && view_->option(view_->combo->currentIndex()) == index_; }
    QList<QAccessibleInterface *> columnHeaderCells() const override { return {}; }
    QList<QAccessibleInterface *> rowHeaderCells() const override { return {}; }
    int columnIndex() const override { return 0; }
    int rowIndex() const override { return isValid() ? index_.row() : -1; }
    int columnExtent() const override { return 1; }
    int rowExtent() const override { return 1; }
    QAccessibleInterface *table() const override { return parent(); }
    QStringList actionNames() const override { return {pressAction(), toggleAction()}; }
    void doAction(const QString &action) override {
        if (!isValid()) return;
        // Qt 5's Windows SelectionItem.Select toggles the requested item and
        // then toggles selected siblings. A combo never deselects its current
        // choice: the queued commit atomically replaces that choice instead.
        if (action == pressAction() || (action == toggleAction() && !isSelected()))
            view_->commit(index_);
    }
    QStringList keyBindingsForAction(const QString &) const override { return {}; }
private:
    QPointer<WhiteboardSelectionView> view_;
    QPersistentModelIndex index_;
};

class AccessibleChoices final : public QAccessibleWidget, public QAccessibleTableInterface {
public:
    explicit AccessibleChoices(WhiteboardSelectionView *view) : QAccessibleWidget(view, QAccessible::List), view_(view) {}
    ~AccessibleChoices() override {
        for (const auto id : items_) QAccessible::deleteAccessibleInterface(id);
    }
    QAccessibleInterface *parent() const override {
        return view_ && view_->combo ? QAccessible::queryAccessibleInterface(view_->combo) : nullptr;
    }
    QAccessible::State state() const override {
        auto result = QAccessibleWidget::state();
        // The logical choices remain enumerable when the popup is closed.
        // Report offscreen instead of removing them from the UIA tree.
        result.invisible = false;
        result.offscreen = !view_ || !view_->isVisible();
        return result;
    }
    QRect rect() const override { return view_ && view_->isVisible() ? QAccessibleWidget::rect() : QRect(); }
    int childCount() const override { return rowCount(); }
    QAccessibleInterface *child(int row) const override {
        if (row < 0 || row >= rowCount()) return nullptr;
        for (auto it = items_.begin(); it != items_.end();) {
            auto *item = QAccessible::accessibleInterface(*it);
            if (!item || !item->isValid()) {
                QAccessible::deleteAccessibleInterface(*it);
                it = items_.erase(it);
            } else {
                if (item->tableCellInterface()->rowIndex() == row) return item;
                ++it;
            }
        }
        auto *item = new AccessibleOption(view_, view_->option(row));
        items_.append(QAccessible::registerAccessibleInterface(item));
        return item;
    }
    int indexOfChild(const QAccessibleInterface *item) const override {
        if (!item || item->parent() != this) return -1;
        auto *cell = const_cast<QAccessibleInterface *>(item)->tableCellInterface();
        return cell ? cell->rowIndex() : -1;
    }
    QAccessibleInterface *childAt(int x, int y) const override {
        if (!view_ || !view_->isVisible()) return nullptr;
        const auto index = view_->indexAt(view_->viewport()->mapFromGlobal({x, y}));
        return index.isValid() ? child(index.row()) : nullptr;
    }
    QAccessibleInterface *focusChild() const override { return selectedCellCount() ? child(view_->combo->currentIndex()) : nullptr; }
    void *interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::TableInterface) return static_cast<QAccessibleTableInterface *>(this);
        return QAccessibleWidget::interface_cast(type);
    }
    QAccessibleInterface *caption() const override { return nullptr; }
    QAccessibleInterface *summary() const override { return nullptr; }
    QAccessibleInterface *cellAt(int row, int column) const override { return column == 0 ? child(row) : nullptr; }
    int rowCount() const override { return view_ && view_->combo ? view_->combo->count() : 0; }
    int columnCount() const override { return 1; }
    int selectedCellCount() const override { return view_ && view_->combo && view_->combo->currentIndex() >= 0 ? 1 : 0; }
    QList<QAccessibleInterface *> selectedCells() const override { return selectedCellCount() ? QList<QAccessibleInterface *>{child(view_->combo->currentIndex())} : QList<QAccessibleInterface *>{}; }
    QString columnDescription(int) const override { return {}; }
    QString rowDescription(int row) const override { const auto *item = child(row); return item ? item->text(QAccessible::Name) : QString(); }
    int selectedColumnCount() const override { return selectedCellCount(); }
    int selectedRowCount() const override { return selectedCellCount(); }
    QList<int> selectedColumns() const override { return selectedCellCount() ? QList<int>{0} : QList<int>{}; }
    QList<int> selectedRows() const override { return selectedCellCount() ? QList<int>{view_->combo->currentIndex()} : QList<int>{}; }
    bool isColumnSelected(int column) const override { return column == 0 && selectedCellCount(); }
    bool isRowSelected(int row) const override { return selectedCellCount() && view_->combo->currentIndex() == row; }
    bool selectRow(int row) override { return view_ && view_->commit(view_->option(row)); }
    bool selectColumn(int) override { return false; }
    bool unselectRow(int) override { return false; } // A noneditable combo keeps one choice.
    bool unselectColumn(int) override { return false; }
    void modelChange(QAccessibleTableModelChangeEvent *) override {} // Persistent indexes invalidate removed rows.
private:
    QPointer<WhiteboardSelectionView> view_;
    mutable QList<QAccessible::Id> items_;
};

QAccessibleInterface *choicesFactory(const QString &, QObject *object) {
    if (auto *view = qobject_cast<WhiteboardSelectionView *>(object)) return new AccessibleChoices(view);
    return nullptr;
}
} // namespace

void configureAccessibleComboBox(QComboBox *combo) {
    static const bool installed = [] { QAccessible::installFactory(choicesFactory); return true; }();
    Q_UNUSED(installed);
    combo->setView(new WhiteboardSelectionView(combo));
}
QComboBox *createAccessibleComboBox(QWidget *parent) {
    auto *combo = new QComboBox(parent);
    configureAccessibleComboBox(combo);
    return combo;
}
QComboBox *createAccessibleWhiteboardComboBox(QWidget *parent) {
    return createAccessibleComboBox(parent);
}
} // namespace MeetingUI

#include "accessible_combo_box.moc"

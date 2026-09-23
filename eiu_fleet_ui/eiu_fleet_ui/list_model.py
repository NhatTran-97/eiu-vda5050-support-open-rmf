"""A list model whose rows are keyed, so a new list changes only the rows that differ."""

from PySide6.QtCore import (QAbstractListModel, QByteArray, QModelIndex, Property, Qt, Signal, Slot)

KEY_ROLE = Qt.ItemDataRole.UserRole + 1
ROW_ROLE = Qt.ItemDataRole.UserRole + 2


class KeyedListModel(QAbstractListModel):
    """Rows are dicts identified by one field; set_rows() inserts, removes, moves and updates in place.

    Delegates read the whole row through the `row` role (`required property var row`) and keep
    their state (hover, scroll position, running animations) while the row's values change.
    """

    countChanged = Signal()

    def __init__(self, key: str = "key", parent=None):
        super().__init__(parent)
        self._key = key
        self._rows: list[dict] = []
        self._keys: list[str] = []

    # QAbstractListModel

    def roleNames(self):
        return {KEY_ROLE: QByteArray(b"key"), ROW_ROLE: QByteArray(b"row")}

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self._rows)

    def data(self, index, role=ROW_ROLE):
        if not index.isValid() or not 0 <= index.row() < len(self._rows):
            return None
        if role == ROW_ROLE:
            return self._rows[index.row()]
        if role == KEY_ROLE:
            return self._keys[index.row()]
        return None

    # Updates

    def set_rows(self, rows) -> bool:
        """Make the model hold `rows` in this order; returns whether anything changed."""
        target, keys, seen = [], [], set()
        for row in rows:
            key = str(row.get(self._key, ""))
            if key in seen:
                continue
            seen.add(key)
            target.append(row)
            keys.append(key)

        before = len(self._rows)
        changed = False

        for i in range(len(self._keys) - 1, -1, -1):
            if self._keys[i] not in seen:
                self.beginRemoveRows(QModelIndex(), i, i)
                del self._rows[i]
                del self._keys[i]
                self.endRemoveRows()
                changed = True

        for i, (key, row) in enumerate(zip(keys, target)):
            if i < len(self._keys) and self._keys[i] == key:
                if self._rows[i] != row:
                    self._rows[i] = row
                    self.dataChanged.emit(self.index(i), self.index(i), [ROW_ROLE])
                    changed = True
                continue
            try:
                j = self._keys.index(key, i + 1)
            except ValueError:
                j = -1
            if j >= 0:
                self.beginMoveRows(QModelIndex(), j, j, QModelIndex(), i)
                self._keys.insert(i, self._keys.pop(j))
                self._rows.insert(i, self._rows.pop(j))
                self.endMoveRows()
                if self._rows[i] != row:
                    self._rows[i] = row
                    self.dataChanged.emit(self.index(i), self.index(i), [ROW_ROLE])
            else:
                self.beginInsertRows(QModelIndex(), i, i)
                self._keys.insert(i, key)
                self._rows.insert(i, row)
                self.endInsertRows()
            changed = True

        if len(self._rows) != before:
            self.countChanged.emit()
        return changed

    @Slot("QVariantList")
    def setRows(self, rows):
        """set_rows() for QML; each entry is an object with the key field."""
        self.set_rows([dict(r) for r in rows if isinstance(r, dict)])

    def rows(self) -> list[dict]:
        return list(self._rows)

    # Read by QML

    @Property(int, notify=countChanged)
    def count(self):
        return len(self._rows)

    @Slot(int, result="QVariant")
    def get(self, index):
        return self._rows[index] if 0 <= index < len(self._rows) else None

    @Slot(str, result=int)
    def indexOf(self, key):
        try:
            return self._keys.index(key)
        except ValueError:
            return -1

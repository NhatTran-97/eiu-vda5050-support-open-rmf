from PySide6.QtCore import QObject, Property


class Colors(QObject):
    """Read-only color palette — exposed to QML as context property `C`."""

    # ── Modern control-room palette ──────────────────────────────────────────
    @Property(str, constant=True)
    def bg(self):         return "#07111F"

    @Property(str, constant=True)
    def surface(self):    return "#0D1C2E"

    @Property(str, constant=True)
    def surfaceAlt(self): return "#12263D"

    @Property(str, constant=True)
    def surfaceRaised(self): return "#17314C"

    @Property(str, constant=True)
    def border(self):     return "#203C5A"

    @Property(str, constant=True)
    def text(self):       return "#F5F8FC"

    @Property(str, constant=True)
    def textDim(self):    return "#8EA6BE"

    # Brighter than textDim so empty-field hints stay legible on surfaceAlt,
    # while staying dimmer than `text` so a real value still reads as "filled in".
    @Property(str, constant=True)
    def placeholderText(self): return "#C7D6E8"

    @Property(str, constant=True)
    def accent(self):     return "#1E78FF"

    @Property(str, constant=True)
    def accentDark(self): return "#1456B8"

    @Property(str, constant=True)
    def success(self):    return "#20D0D6"

    @Property(str, constant=True)
    def warn(self):       return "#FFB547"

    @Property(str, constant=True)
    def err(self):        return "#FF5D70"

    @Property(str, constant=True)
    def blue(self):       return "#3B82F6"

    @Property(str, constant=True)
    def cyan(self):       return "#36C5F0"

from PySide6.QtCore import QObject, Property


class Colors(QObject):
    """Read-only color palette — exposed to QML as context property `C`."""

    # ── EIU Green palette (dark navy + green accent) ─────────────────────────
    @Property(str, constant=True)
    def bg(self):         return "#0B1A27"  

    @Property(str, constant=True)
    def surface(self):    return "#162030"   

    @Property(str, constant=True)
    def surfaceAlt(self): return "#1E2E40"   

    @Property(str, constant=True)
    def border(self):     return "#1E3248"

    @Property(str, constant=True)
    def text(self):       return "#ECF0F1"   

    @Property(str, constant=True)
    def textDim(self):    return "#7F8C8D"   

    @Property(str, constant=True)
    def accent(self):     return "#27AE60"  

    @Property(str, constant=True)
    def accentDark(self): return "#1D8348"   

    @Property(str, constant=True)
    def warn(self):       return "#F39C12"  

    @Property(str, constant=True)
    def err(self):        return "#E74C3C"   

    @Property(str, constant=True)
    def blue(self):       return "#2980B9"  

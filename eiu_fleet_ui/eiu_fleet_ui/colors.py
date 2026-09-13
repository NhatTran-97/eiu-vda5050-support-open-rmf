from PySide6.QtCore import QObject, Property


class Colors(QObject):
    """Expose the shared read-only color palette to QML."""

    # Shared color palette for the UI.
    @Property(str, constant=True)
    def bg(self):         return "#071727"

    @Property(str, constant=True)
    def surface(self):    return "#0D2135"

    @Property(str, constant=True)
    def surfaceAlt(self): return "#12283F"

    @Property(str, constant=True)
    def surfaceRaised(self): return "#173049"

    @Property(str, constant=True)
    def border(self):     return "#14324A"

    @Property(str, constant=True)
    def text(self):       return "#F1F6FA"

    @Property(str, constant=True)
    def textDim(self):    return "#7FA4C0"

    # Hint text color between primary and muted text.
    @Property(str, constant=True)
    def placeholderText(self): return "#A9C4DA"

    # Accent for buttons and dispatch actions.
    @Property(str, constant=True)
    def accent(self):     return "#2F80ED"

    @Property(str, constant=True)
    def accentDark(self): return "#1F5FBD"

    # Color for online and successful states.
    @Property(str, constant=True)
    def success(self):    return "#2DDC8C"

    @Property(str, constant=True)
    def warn(self):       return "#F3B33D"

    @Property(str, constant=True)
    def err(self):        return "#F04F64"

    @Property(str, constant=True)
    def blue(self):       return "#2F80ED"

    # Accent for live data and routes.
    @Property(str, constant=True)
    def cyan(self):       return "#18C8E3"

    # Bright accent for pulses and active selections.
    @Property(str, constant=True)
    def cyanBright(self): return "#20E3F0"

from PySide6.QtCore import QObject, Property


class Colors(QObject):
    """Read-only color palette, exposed to QML as context property `C`.

    ~70% dark navy neutrals / 20% cyan+blue accent / 10% status colors, so
    status changes (green/amber/red) stand out against a calm background.
    """

    # ── Modern control-room palette ──────────────────────────────────────────
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

    # Between textDim and text: legible as a hint, but clearly not a real value.
    @Property(str, constant=True)
    def placeholderText(self): return "#A9C4DA"

    # Secondary accent (buttons, dispatch actions), distinct from `cyan`.
    @Property(str, constant=True)
    def accent(self):     return "#2F80ED"

    @Property(str, constant=True)
    def accentDark(self): return "#1F5FBD"

    # True green, not a cyan variant, so "online"/"success" reads distinctly.
    @Property(str, constant=True)
    def success(self):    return "#2DDC8C"

    @Property(str, constant=True)
    def warn(self):       return "#F3B33D"

    @Property(str, constant=True)
    def err(self):        return "#F04F64"

    @Property(str, constant=True)
    def blue(self):       return "#2F80ED"

    # Primary accent for live/telemetry data (borders, route, values).
    @Property(str, constant=True)
    def cyan(self):       return "#18C8E3"

    # Reserved for the one or two things that should actually pop (live pulse, active selection).
    @Property(str, constant=True)
    def cyanBright(self): return "#20E3F0"

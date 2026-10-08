"""API error raised by any layer; the app turns it into {"error": {"code", "message"}} with its HTTP status."""


class ApiFail(Exception):
    def __init__(self, status: int, code: str, message: str = ""):
        super().__init__(code)
        self.status = status
        self.code = code
        self.message = message

class SlimError(Exception):
    def __init__(self, message, code=1):
        Exception.__init__(self, message)
        self.code = code

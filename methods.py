import os
import sys
from enum import Enum

_colorize = bool(sys.stdout.isatty() or os.environ.get("CI"))


class ANSI(Enum):
    RESET = "\x1b[0m"
    BOLD = "\x1b[1m"
    REGULAR = "\x1b[22m"
    RED = "\x1b[31m"

    def __str__(self) -> str:
        return str(self.value) if _colorize else ""


def print_error(*values: object) -> None:
    print(f"{ANSI.RED}{ANSI.BOLD}ERROR:{ANSI.REGULAR}", *values, ANSI.RESET, file=sys.stderr)

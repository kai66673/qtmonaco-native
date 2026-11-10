#!/usr/bin/env python3
"""qtmonaco-native python highlighting demo.

Open with:  ./build/src/app/qmn-spike examples/demo.py
Docstrings, inline comments, decorators, f-strings, escapes and numeric
literals all ride the upstream python `highlights.scm` captures pinned
under third_party/tree-sitter/grammars/python/.
"""

import json  # stdlib import
from dataclasses import dataclass, field
from typing import Iterable

MAX_RETRIES = 10  # ALL_CAPS identifiers hit the @constant (#match?) pattern


@dataclass
class StockLedger:  # CamelCase names resolve through the ^[A-Z] patterns
    """Tiny inventory model."""

    name: str
    quantity: int = 0
    price: float = 1_999.50  # float with a digit-separator underscore
    tags: list[str] = field(default_factory=list)

    def total(self, tax_rate: float = 0.2) -> float:
        # subtotal before tax; the caret here is just a plain comment
        subtotal = self.quantity * self.price
        return subtotal * (1 + tax_rate)


def render(items: Iterable[StockLedger]) -> str:
    lines: list[str] = []
    hex_pad = 0xFF00AA  # hex literal
    for item in items:
        if item.quantity <= 0:
            lines.append(f"{item.name!r:>12}: sold out ({hex_pad:#x})")
        else:
            lines.append(f"{item.name} — {item.total():,.2f}")
    return "\n".join(lines)


class LedgerLoader:
    def load(self, raw: bytes) -> list[dict]:
        try:
            rows = json.loads(raw.decode("utf-8"))
        except (ValueError, OSError) as exc:  # builtin exception classes
            raise RuntimeError("unreadable feed") from exc
        rows.sort(key=lambda row: -row["qty"])
        return [row for row in rows if row.get("qty", 0) >= 0]


if __name__ == "__main__":
    demo = b'[{"name": "widget \u00e9", "qty": 3, "price": 4.5e0}]'
    print(render([StockLedger(**row) for row in LedgerLoader().load(demo)]))

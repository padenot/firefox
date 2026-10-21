# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Extracts the "Tensor limits for <op>()" tables of the WebNN spec.

Writes tensor_limits.json, which webnn_codegen.py uses to generate
MLContext.opSupportLimits():

  {op: {operand: {"dataTypes": [...], "rankRange": [min, max]}}}

The allowed (not required) data types and ranks are used. "any" is expanded to
every MLOperandDataType and "same as <operand>" is resolved. A maximum rank of
"N" is left as is, it is the implementation's maximum rank. Operand names are
the MLOpSupportLimits dictionary member names: "inputs's items" is "inputs",
"outputs[i]" is "output<i>".

Regenerate with:
  ./mach python dom/webnn/codegen/tensor_limits.py \\
      https://webmachinelearning.github.io/webnn/
(or a path to a saved copy of the spec).
"""

import html
import json
import os
import re
import sys
import urllib.request

OUTPUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tensor_limits.json")


def text(fragment):
    return re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", "", fragment))).strip()


def operand_name(cell):
    name = re.sub(r" if .*$", "", cell)
    m = re.fullmatch(r"(\w+)’s items", name)
    if m:
        return m.group(1)
    m = re.fullmatch(r"outputs\[(\d+)\]", name)
    if m:
        return "output" + m.group(1)
    if not re.fullmatch(r"\w+", name):
        raise Exception("Unexpected operand %r" % cell)
    return name


def parse_types(cell, all_types):
    if cell == "any":
        return {"any": True}
    m = re.fullmatch(r"same as (.+)", cell)
    if m:
        return {"same": operand_name(m.group(1))}
    types = re.findall(r'"(\w+)"', cell)
    if not types or any(t not in all_types for t in types):
        raise Exception("Unexpected data types %r" % cell)
    return {"types": types}


def parse_ranks(cell):
    m = re.fullmatch(r"same as (.+)", cell)
    if m:
        return {"same": operand_name(m.group(1))}
    if cell == "N":
        return {"range": [0, "N"]}
    m = re.fullmatch(r"(\d+)(?: to (\d+|N))?", cell)
    if not m:
        raise Exception("Unexpected ranks %r" % cell)
    lo = int(m.group(1))
    hi = m.group(2) or m.group(1)
    return {"range": [lo, hi if hi == "N" else int(hi)]}


def resolve(rows, operand, column, all_types, seen=()):
    value = rows[operand][column]
    if "same" in value:
        target = value["same"]
        if target in seen or target not in rows:
            raise Exception("Cannot resolve %s of %s" % (column, operand))
        return resolve(rows, target, column, all_types, seen + (operand,))
    if "any" in value:
        return list(all_types)
    return value.get("types") or value["range"]


def extract(spec):
    enum = re.search(r"enum MLOperandDataType \{(.*?)\}", text(spec))
    all_types = re.findall(r'"(\w+)"', enum.group(1))
    tables = list(
        re.finditer(
            r'<table class="data" id="tensor-limits[^"]*">(.*?)</table>', spec, re.S
        )
    )
    limits = {}
    for i, table in enumerate(tables):
        caption = text(re.search(r"<caption>(.*?)</caption>", table.group(1), re.S)[1])
        ops = re.findall(r"(\w+)\(\)", caption)
        if not ops:
            # The caption names a group, the MLOpSupportLimits members it
            # describes follow the table.
            end = tables[i + 1].start() if i + 1 < len(tables) else len(spec)
            ops = re.findall(
                r'data-dfn-for="MLOpSupportLimits"[^>]*><code>(\w+)</code>',
                spec[table.end() : end],
            )
        if not ops:
            raise Exception("No MLOpSupportLimits member for %r" % caption)
        rows = {}
        body = table.group(1).split("<tbody>")[1]
        for row in body.split("<tr>")[1:]:
            cells = [text(c) for c in row.split("<td>")[1:]]
            assert len(cells) == 5, (caption, cells)
            rows[operand_name(cells[0])] = {
                "dataTypes": parse_types(cells[1], all_types),
                "rankRange": parse_ranks(cells[3]),
            }
        resolved = {
            operand: {
                column: resolve(rows, operand, column, all_types)
                for column in ("dataTypes", "rankRange")
            }
            for operand in rows
        }
        for op in ops:
            if op in limits:
                raise Exception("Duplicate table for %s" % op)
            limits[op] = resolved
    return limits


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 1
    source = argv[0]
    if re.match(r"https?://", source):
        with urllib.request.urlopen(source) as r:
            spec = r.read().decode("utf-8")
    else:
        with open(source, encoding="utf-8") as f:
            spec = f.read()
    limits = extract(spec)
    with open(OUTPUT, "w", encoding="utf-8") as f:
        ops = []
        for op, operands in limits.items():
            lines = [
                "    %s: %s" % (json.dumps(name), json.dumps(value))
                for name, value in operands.items()
            ]
            ops.append("  %s: {\n%s\n  }" % (json.dumps(op), ",\n".join(lines)))
        f.write("{\n%s\n}\n" % ",\n".join(ops))
    print("Wrote limits for %d operators to %s" % (len(limits), OUTPUT))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

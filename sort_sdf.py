#!/usr/bin/python3
import argparse
import re


def natural_key(s):
    """Split a string into text/number chunks for natural sorting.
    Example: 'gen_3_inst' -> ('gen_', 3, '_inst')"""
    parts = re.split(r"(\d+)", s)
    return tuple(int(p) if p.isdigit() else p for p in parts)


def split_cells(text):
    """Split SDF text into (header, cell blocks, footer).
    Cell blocks are detected by top-level (CELL ... ) with paren matching."""
    cells, buf, depth, in_cell = [], [], 0, False
    prefix, suffix = [], []
    for line in text.splitlines(keepends=True):
        if line.strip().startswith("(CELL") and depth == 0 and not in_cell:
            in_cell, buf, depth = True, [line], line.count("(") - line.count(")")
            continue
        if in_cell:
            buf.append(line)
            depth += line.count("(") - line.count(")")
            if depth == 0:
                cells.append("".join(buf))
                in_cell, buf = False, []
        elif not cells:
            prefix.append(line)
        else:
            suffix.append(line)
    return "".join(prefix), cells, "".join(suffix)


def tokenize_stmt(stmt):
    """Extract sortable tokens from one timing statement."""
    return tuple(
        t.strip() for t in re.findall(r"\(([^()]*)\)|(\S+)", stmt) for t in t if t
    )


def stmt_key(stmt):
    """Sort key for one statement. Full text as final tiebreaker
    so the key order is total and input-order independent."""
    toks = tokenize_stmt(stmt)
    return tuple(natural_key(t) for t in toks) + (stmt,)


def parse_section(lines, i):
    """Collect the inner lines of a section starting at lines[i]
    (e.g. '(DELAY'). Uses paren balancing, so it is safe for
    statements that span multiple lines.
    Returns (inner_lines, closing_line, next_index). The closing line
    (where the parens balance back to zero) is returned separately so
    the caller can keep its original text and indentation."""
    depth = lines[i].count("(") - lines[i].count(")")
    inner, closing, j = [], "", i + 1
    while j < len(lines) and depth > 0:
        depth += lines[j].count("(") - lines[j].count(")")
        if depth > 0:
            inner.append(lines[j])
        else:
            closing = lines[j]
        j += 1
    return inner, closing, j


def group_statements(lines):
    """Group lines into complete statements by paren balancing.
    Unlike the old single-line merge, each group keeps its original
    lines (content and indentation), so sorting only reorders
    statements without rewriting their format.
    Blank lines are attached to the statement that precedes them."""
    groups, cur, depth = [], [], 0
    for ln in lines:
        if not ln.strip():
            if cur:
                cur.append(ln)  # keep blank line with previous statement
            continue
        cur.append(ln)
        depth += ln.count("(") - ln.count(")")
        if depth <= 0:
            groups.append(cur)
            cur, depth = [], 0
    if cur:  # defensive: unbalanced trailing content
        groups.append(cur)
    return groups


def stmt_text(group):
    """Canonical one-line text of a statement group, used only
    for computing the sort key (never emitted)."""
    return " ".join(ln.strip() for ln in group if ln.strip())


def group_key(group):
    """Sort key for one statement group, based on its canonical text."""
    return stmt_key(stmt_text(group))


def sort_group(group):
    """Sort the inner statements of a multi-line (ABSOLUTE ...) or
    (INCREMENT ...) group, wherever the group appears in the section.
    The group's own lines (header, indentation, closing line) are
    preserved; only the order of its inner statements changes."""
    first = group[0].strip()
    if len(group) > 2 and re.match(r"\(\s*(ABSOLUTE|INCREMENT)\s*$", first):
        header, inner, closing = group[0], group[1:-1], group[-1]
        body = [ln for g in sorted(group_statements(inner), key=group_key) for ln in g]
        return [header] + body + [closing]
    return group


def emit_section(out, header, inner, closing):
    """Emit one DELAY/TIMINGCHECK section: statements sorted in place.
    Every statement keeps its original lines and indentation; only the
    order changes. ABSOLUTE/INCREMENT interiors are sorted recursively."""
    out.append(header)  # keep original header line (with indent)
    for g in sorted((sort_group(g) for g in group_statements(inner)), key=group_key):
        out.extend(g)
    if closing:
        out.append(closing)  # keep original closing line (with indent)


def sort_cell_interior(cell):
    """Sort statements inside (DELAY (ABSOLUTE ...)) and
    (TIMINGCHECK ...) sections of one cell block."""
    lines = cell.splitlines(keepends=True)
    out, i, n = [], 0, len(lines)
    while i < n:
        if re.match(r"\(\s*(DELAY|TIMINGCHECK)\s*$", lines[i].strip()):
            header = lines[i]
            inner, closing, i = parse_section(lines, i)
            emit_section(out, header, inner, closing)
        else:
            out.append(lines[i])
            i += 1
    return "".join(out)


def cell_sort_key(cell):
    """Sort key for one cell block. INSTANCE may be empty (top-level
    cell). Full text as final tiebreaker so the key order is total."""
    inst = re.search(r"\(INSTANCE\s*([^\s()]*)\)", cell)  # * allows empty
    ctype = re.search(r'\(CELLTYPE\s+"([^"]+)"\)', cell)
    return (
        natural_key(inst.group(1) if inst else ""),
        natural_key(ctype.group(1) if ctype else ""),
        cell,
    )


def normalize_sdf(input_path, output_path):
    with open(input_path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    prefix, cells, suffix = split_cells(text)
    cells = [sort_cell_interior(c) for c in cells]
    cells = sorted(cells, key=cell_sort_key)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(prefix + "".join(cells) + suffix)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description="Normalize SDF cell/statement order")
    p.add_argument("input", help="input .sdf file")
    p.add_argument("-o", "--output", default=None, help="output file")
    args = p.parse_args()
    if args.output:
        out = args.output
    elif args.input.lower().endswith(".sdf"):
        out = args.input[:-4] + "_sorted.sdf"
    else:
        out = args.input + "_sorted.sdf"
    normalize_sdf(args.input, out)
    print(f"written: {out}")

#!/usr/bin/env python3
"""
Generate SVG bar charts from the JSON/YAML/CSV benchmark tables in README.md.

Produces one SVG per file-size category (small/medium/large). Each SVG is a
3x2 grid: rows are JSON/YAML/CSV, columns are decode/encode, and each cell is
a bar chart comparing libraries for that format's representative file at that
size category.

Usage:
    bin/gen-bench-charts.py [README.md] [output_dir]

Defaults: README.md in the repo root, output to assets/.
"""

import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

TABLES = [
    {"format": "JSON", "heading": "### Benchmarking JSON"},
    {"format": "YAML", "heading": "### Benchmarking YAML"},
    {"format": "CSV",  "heading": "### Benchmarking CSV"},
]

# For each format, which group label represents the small/medium/large file.
# Picked by rank (smallest/middle/largest) within that format's own table.
SIZE_PICKS = {
    "JSON": {"small": "small", "medium": "esad", "large": "twitter"},
    "YAML": {"small": "small", "medium": "esad", "large": "openrtb"},
    "CSV":  {"small": "small", "medium": "medium", "large": "large"},
}

SIZE_CATEGORIES = ["small", "medium", "large"]

NUM_RE = re.compile(r"[-+]?\d+(?:\.\d+)?")


def extract_table_block(text, heading):
    """Return the lines of the fenced code block following `heading`."""
    idx = text.index(heading)
    rest = text[idx:]
    fence_starts = [m.start() for m in re.finditer(r"^```", rest, re.MULTILINE)]
    if len(fence_starts) < 2:
        raise ValueError(f"Could not find fenced block after {heading!r}")
    block = rest[fence_starts[0]:fence_starts[1]]
    return block.splitlines()


def parse_table(lines):
    """
    Parse a benchmark table of the form:

        (numbers in µs)
        <blank-or-label>  group1 (size)   group2 (size)  ...
                        decode  encode    decode  encode  ...
        ----------------------------------------------------
        libname          v1      v2        v3      v4    ...
        ...

    Returns (group_labels, [(libname, [values...]), ...])
    where values align with len(group_labels) * 2 (decode, encode per group).
    """
    group_line = None
    sep_idx = None
    for i, line in enumerate(lines):
        if "(numbers in" in line:
            group_line = lines[i + 1]
        if line.startswith("---"):
            sep_idx = i
            break

    if group_line is None or sep_idx is None:
        raise ValueError("Could not locate table header/separator")

    # Extract group labels, e.g. "twitter (616.7K)" -> "twitter", "(616.7K)" -> "616.7K"
    group_sizes = {}
    group_labels = []
    for m in re.findall(r"[A-Za-z_][\w]*\s*\([^)]*\)", group_line):
        label, _, size = m.partition("(")
        label = label.strip()
        size = size.rstrip(")").strip()
        group_labels.append(label)
        group_sizes[label] = size

    rows = []
    for line in lines[sep_idx + 1:]:
        line = line.rstrip()
        if not line.strip() or line.startswith("```"):
            continue
        parts = line.split()
        name = parts[0]
        rest = parts[1:]
        values = []
        for tok in rest:
            if tok.upper() == "N/A":
                values.append(None)
            elif tok.upper() == "TIMEOUT":
                values.append("TIMEOUT")
            else:
                m = NUM_RE.fullmatch(tok)
                values.append(float(tok) if m else None)
        rows.append((name, values))

    return group_labels, group_sizes, rows


# ---------------------------------------------------------------------------
# SVG rendering
# ---------------------------------------------------------------------------

# Harmonic blue -> teal -> violet progression, chosen for sufficient
# contrast on both a white and a dark (#0d1117, GitHub dark) background.
PALETTE_LIGHT = [
    "#aab8c8", "#94a6ba", "#7e94ac", "#6c84a0", "#5c7494",
    "#4c6488", "#3f5878", "#344a68", "#2a3c58", "#1f2e48",
]
PALETTE_DARK = [
    "#9aacc0", "#8398b0", "#6e84a0", "#5c7290", "#4c6080",
    "#3e5070", "#324060", "#283450", "#1f2840", "#161c30",
]

# Accent color for the glazer bars: the bright cyan from the glazer logo,
# set apart from the rest of the pale harmonic palette.
ACCENT_LIGHT = "#00a6e6"
ACCENT_DARK = "#00b5fe"

STYLE = """
  <style>
    .bg     { fill: #ffffff; }
    .fg     { fill: #1f2328; }
    .muted  { fill: #57606a; }
    .faint  { fill: #8b949e; }
    .grid   { stroke: #d0d7de; }
    .axis   { stroke: #1f2328; }
"""

for _i, (_light, _dark) in enumerate(zip(PALETTE_LIGHT, PALETTE_DARK)):
    STYLE += f"    .bar-{_i} {{ fill: {_light}; }}\n"

STYLE += f"    .bar-glazer {{ fill: {ACCENT_LIGHT}; }}\n"

STYLE += """    @media (prefers-color-scheme: dark) {
      .bg     { fill: #0d1117; }
      .fg     { fill: #e6edf3; }
      .muted  { fill: #c9d1d9; }
      .faint  { fill: #8b949e; }
      .grid   { stroke: #30363d; }
      .axis   { stroke: #e6edf3; }
"""

for _i, (_light, _dark) in enumerate(zip(PALETTE_LIGHT, PALETTE_DARK)):
    STYLE += f"      .bar-{_i} {{ fill: {_dark}; }}\n"

STYLE += f"      .bar-glazer {{ fill: {ACCENT_DARK}; }}\n"

STYLE += """    }
  </style>
"""


def nice_step(rough_step):
    """Round rough_step up to a "nice" 1/2/5 * 10^k value."""
    if rough_step <= 0:
        return 1
    exp = math.floor(math.log10(rough_step))
    base = rough_step / (10 ** exp)
    if base <= 1:
        nice = 1
    elif base <= 2:
        nice = 2
    elif base <= 5:
        nice = 5
    else:
        nice = 10
    return nice * (10 ** exp)


def render_cell(values_by_lib, mode, x0, y0, w, h, title):
    """
    Render one bar chart (single group of bars, one per library) into the
    rectangle (x0, y0, w, h). values_by_lib: [(libname, value_or_None_or_TIMEOUT)].
    """
    margin_left = 50
    margin_right = 10
    margin_top = 18
    margin_bottom = 40
    plot_w = w - margin_left - margin_right
    plot_h = h - margin_top - margin_bottom

    nums = [v for _, v in values_by_lib if isinstance(v, (int, float)) and v > 0]
    vmax = max(nums) if nums else 1

    n_ticks_target = 4
    step = nice_step(vmax / n_ticks_target)
    axis_max = step * math.ceil(vmax / step) if vmax > 0 else step

    def y_for(v):
        if not isinstance(v, (int, float)) or v <= 0:
            return y0 + margin_top + plot_h
        frac = v / axis_max
        return y0 + margin_top + plot_h * (1 - frac)

    svg = []
    svg.append(
        f'<text x="{x0 + w/2:.1f}" y="{y0 + 16:.1f}" text-anchor="middle" '
        f'font-size="13" font-weight="bold" class="fg">{title}</text>'
    )

    base_y = y0 + margin_top + plot_h

    # Y-axis grid lines + labels
    n_ticks = round(axis_max / step) if step > 0 else 0
    for i in range(n_ticks + 1):
        v = i * step
        y = y_for(v) if v > 0 else base_y
        svg.append(
            f'<line x1="{x0 + margin_left:.1f}" y1="{y:.1f}" '
            f'x2="{x0 + w - margin_right:.1f}" y2="{y:.1f}" '
            f'class="grid" stroke-width="1"/>'
        )
        svg.append(
            f'<text x="{x0 + margin_left - 6:.1f}" y="{y + 3:.1f}" text-anchor="end" '
            f'font-size="9" class="muted">{v:g}</text>'
        )

    n_bars = len(values_by_lib)
    bar_gap = 6
    bar_w = (plot_w - bar_gap * (n_bars + 1)) / n_bars

    for i, (name, v) in enumerate(values_by_lib):
        bx = x0 + margin_left + bar_gap + i * (bar_w + bar_gap)
        is_glazer = name == "glazer"
        bar_class = "bar-glazer" if is_glazer else f"bar-{i % len(PALETTE_LIGHT)}"

        if v is None or v == "TIMEOUT":
            label = "TIMEOUT" if v == "TIMEOUT" else "N/A"
            cx = bx + bar_w / 2
            svg.append(
                f'<text x="{cx:.1f}" y="{base_y - 4:.1f}" '
                f'text-anchor="middle" font-size="8" class="faint">{label}</text>'
            )
        else:
            y = y_for(v)
            bh = base_y - y
            svg.append(
                f'<rect x="{bx:.1f}" y="{y:.1f}" width="{bar_w:.1f}" height="{bh:.1f}" '
                f'class="{bar_class}"><title>{name}: {v:g} ({mode})</title></rect>'
            )
            svg.append(
                f'<text x="{bx + bar_w/2:.1f}" y="{y - 3:.1f}" text-anchor="middle" '
                f'font-size="8" class="muted">{v:g}</text>'
            )

        # library name label, rotated
        lx = bx + bar_w / 2
        weight = ' font-weight="bold"' if is_glazer else ""
        svg.append(
            f'<text x="{lx:.1f}" y="{base_y + 10:.1f}" text-anchor="end" '
            f'font-size="9" class="fg"{weight} '
            f'transform="rotate(-35 {lx:.1f},{base_y + 10:.1f})">{name}</text>'
        )

    # X axis line
    svg.append(
        f'<line x1="{x0 + margin_left:.1f}" y1="{base_y:.1f}" '
        f'x2="{x0 + w - margin_right:.1f}" y2="{base_y:.1f}" '
        f'class="axis" stroke-width="1"/>'
    )

    return "\n".join(svg)


def render_page(size_category, format_data):
    """
    format_data: list of (format_name, group_labels, group_sizes, rows, picked_group_label)
    Renders a 3-row x 2-col grid (rows=formats, cols=decode/encode).
    """
    cell_w = 380
    cell_h = 138
    n_rows = len(format_data)
    n_cols = 2
    width = cell_w * n_cols
    height = 50 + cell_h * n_rows

    svg = []
    svg.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" font-family="Helvetica, Arial, sans-serif">'
    )
    svg.append(STYLE)
    svg.append(f'<rect width="{width}" height="{height}" class="bg"/>')
    svg.append(
        f'<text x="{width/2}" y="26" text-anchor="middle" font-size="18" '
        f'font-weight="bold" class="fg">{size_category.capitalize()} input size (time in µs, lower is better)</text>'
    )

    for ri, (fmt, group_labels, group_sizes, rows, picked_label) in enumerate(format_data):
        if picked_label not in group_labels:
            continue
        gi = group_labels.index(picked_label)
        size = group_sizes.get(picked_label, "")

        for ci, mode in enumerate(("decode", "encode")):
            col_offset = 0 if mode == "decode" else 1
            idx = gi * 2 + col_offset

            values_by_lib = []
            for name, values in rows:
                v = values[idx] if idx < len(values) else None
                values_by_lib.append((name, v))

            if not any(isinstance(v, (int, float)) for _, v in values_by_lib):
                continue

            x0 = ci * cell_w
            y0 = 50 + ri * cell_h
            title = f"{fmt} {mode} — {size}" if size else f"{fmt} {mode}"
            svg.append(render_cell(values_by_lib, mode, x0, y0, cell_w, cell_h, title))

    svg.append("</svg>")
    return "\n".join(svg)


def main():
    readme = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "README.md"
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "assets"
    out_dir.mkdir(parents=True, exist_ok=True)

    text = readme.read_text()

    parsed = {}
    for table in TABLES:
        lines = extract_table_block(text, table["heading"])
        group_labels, group_sizes, rows = parse_table(lines)
        parsed[table["format"]] = (group_labels, group_sizes, rows)

    for size_category in SIZE_CATEGORIES:
        format_data = []
        for table in TABLES:
            fmt = table["format"]
            group_labels, group_sizes, rows = parsed[fmt]
            picked_label = SIZE_PICKS[fmt][size_category]
            format_data.append((fmt, group_labels, group_sizes, rows, picked_label))

        svg = render_page(size_category, format_data)
        out_path = out_dir / f"bench_{size_category}.svg"
        out_path.write_text(svg)
        print(f"wrote {out_path.relative_to(ROOT)}")


if __name__ == "__main__":
    main()

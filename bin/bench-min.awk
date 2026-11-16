# Reads the output of several runs of a bench-* task (or of `mix bench`, which
# prints one table per format) on stdin, and prints the same tables with the
# minimum of each numeric cell over the runs (min per column).
#
# Structure:
#   - A line "@@RUN@@" starts a new run. Table numbers restart at each run.
#   - A line starting "===> Running benchmarks" starts a table. Tables are
#     numbered in the order they appear in a run, so table k of run 1 and table
#     k of run 2 are the same table.
#   - Table rows: the first field is a name, the rest are numbers, "n/a" or
#     "TIMEOUT". Rows are keyed by (table, name).
#   - Every other line (banners, headers, separators) is printed once per table,
#     at its first position. Banners are keyed by their label (up to the first
#     colon), so a value that differs between runs (e.g. the niceness) shows the
#     first run's value.
#
# Usage: <bench output, several runs separated by @@RUN@@> | awk -f bin/bench-min.awk

function isnum(s)         { return s ~ /^-?[0-9]+(\.[0-9]+)?$/ }
function isplaceholder(s) { return s == "n/a" || s == "TIMEOUT" }

BEGIN { t = 0 }

$0 == "@@RUN@@" { t = 0; next }

{
  if ($0 ~ /^===> Running benchmarks/) t++

  isrow = ($1 !~ /^===>/) && (NF >= 2)
  for (i = 2; i <= NF && isrow; i++)
    if (!isnum($i) && !isplaceholder($i)) isrow = 0

  if (!isrow) {
    key = t SUBSEP $0
    if ($0 ~ /^===>/ && index($0, ":") > 0) key = t SUBSEP substr($0, 1, index($0, ":"))
    if (!(key in seen)) { seen[key] = 1; out[++nout] = $0 }
    next
  }

  rk = t SUBSEP $1
  if (!(rk in rowidx)) {
    rowidx[rk] = ++nrows
    names[nrows] = $1
    tline[nrows] = $0                   # the first run's line: the layout
    out[++nout] = "\001" nrows          # placeholder, filled in at END
  }
  r = rowidx[rk]
  for (i = 2; i <= NF; i++) {
    c = i - 1
    key = r SUBSEP c
    if (isnum($i)) {
      if (!(key in best) || $i + 0 < best[key]) best[key] = $i + 0
    } else if (!(key in text)) {
      text[key] = $i
    }
    if (c > ncols[r]) ncols[r] = c
  }
}

# Rebuilds row r from the first run's line (tline): every value keeps the
# character position and the gaps of that line, and takes the minimum (or the
# n/a / TIMEOUT text). So the layout is whatever the bench task printed.
function render(r,   s, pos, n, st, en, line, prev, i, txt, w, gap, key) {
  s = tline[r]
  pos = 1; n = 0
  while (match(substr(s, pos), /[^ ]+/)) {
    st = pos + RSTART - 1
    en = st + RLENGTH - 1
    n++; tst[n] = st; ten[n] = en
    pos = en + 1
  }
  line = ""; prev = 0
  for (i = 1; i <= n; i++) {
    gap = substr(s, prev + 1, tst[i] - prev - 1)
    w = ten[i] - tst[i] + 1
    if (i == 1) {
      txt = names[r]
      line = line gap sprintf("%-" w "s", txt)
    } else {
      key = r SUBSEP (i - 1)
      if (key in best)      txt = sprintf("%.1f", best[key])
      else if (key in text) txt = text[key]
      else                  txt = ""
      line = line gap sprintf("%" w "s", txt)
    }
    prev = ten[i]
  }
  return line
}

END {
  for (k = 1; k <= nout; k++) {
    if (substr(out[k], 1, 1) == "\001") print render(substr(out[k], 2) + 0)
    else                                  print out[k]
  }
}

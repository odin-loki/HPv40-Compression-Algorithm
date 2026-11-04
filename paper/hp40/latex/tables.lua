-- tables.lua: pandoc filter for the PDF.
-- * A table whose cells would not fit at natural width gets column widths in
--   proportion to each column's longest cell (capped), so text wraps inside
--   the margins instead of running past them.
-- * Tables with 7 or more columns also get a smaller font and tighter column
--   padding; longtables still break across pages.

local function cell_len(cell)
  return utf8.len(pandoc.utils.stringify(cell.contents or cell)) or 0
end

function Table(tbl)
  if FORMAT ~= "latex" then return nil end
  local ncols = #tbl.colspecs
  if ncols < 2 then return nil end

  local widest = {}
  for i = 1, ncols do widest[i] = 3 end
  local function scan(rows)
    for _, row in ipairs(rows) do
      for i, cell in ipairs(row.cells) do
        if i <= ncols then widest[i] = math.max(widest[i], math.min(cell_len(cell), 60)) end
      end
    end
  end
  scan(tbl.head.rows)
  for _, body in ipairs(tbl.bodies) do scan(body.body) end
  local total = 0
  for i = 1, ncols do total = total + widest[i] end
  if total > 70 then
    for i = 1, ncols do
      tbl.colspecs[i] = {tbl.colspecs[i][1], 0.98 * widest[i] / total}
    end
  end
  if ncols < 7 then return tbl end

  local size, sep = "\\footnotesize", "3pt"
  if ncols >= 9 then size, sep = "\\scriptsize", "2pt" end
  return {
    pandoc.RawBlock("latex", "\\begingroup" .. size .. "\\setlength{\\tabcolsep}{" .. sep .. "}"),
    tbl,
    pandoc.RawBlock("latex", "\\endgroup"),
  }
end

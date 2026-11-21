-- SPDX-License-Identifier: MIT
-- The m6.Root value the typed-value differential starts from (the synthetic m6 registry of kiln-eml's tests).
return {
  child = {base = 3, extra = 4},
  fixed = {5, 6},
  children = {{base = 7, extra = 8}, {base = 9, extra = 10}, {base = 11, extra = 12}},
  maybe = {base = 13, extra = 14},
  choice = {type = "m6.Child", value = {base = 15, extra = 16}},
  text = "plain",
}

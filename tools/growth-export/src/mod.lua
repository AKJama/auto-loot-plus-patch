-- Offline EML export (emm run --export): every growth chain in the game data, one line per stage.
-- A growing template (a planted seedling) has keen::ecs::Upgrade, whose levels name the template
-- the plant turns into at each level. Writes growth-stages.tsv for tools/growth_stages.py.
if not loader.features.export then return end

local function has(components, wanted)
  for _, c in ipairs(components) do
    if tostring(c.type) == wanted then return c.value end
  end
end

local names = {}
local all = game.assets.get_resources_by_type("keen::ecs::TemplateResource")
for _, r in ipairs(all) do names[tostring(r.guid)] = tostring(r.data.name) end

local rows = { "root_guid\troot_name\troot_has_growth\tlevel\tstage_guid\tstage_name\tlast" }
for _, r in ipairs(all) do
  local components = r.data.components or {}
  local upgrade = has(components, "keen::ecs::Upgrade")
  if upgrade then
    local growth = has(components, "keen::ecs::Growth") and "1" or "0"
    local levels = upgrade.levels or {}
    for i, level in ipairs(levels) do
      local stage = tostring(level.upgradeEntity)
      rows[#rows + 1] = table.concat({ tostring(r.guid), tostring(r.data.name), growth, tostring(level.level),
        stage, names[stage] or "?", i == #levels and "1" or "0" }, "\t")
    end
  end
end
io.export("growth-stages.tsv", table.concat(rows, "\n") .. "\n")
print("growth-stages: " .. (#rows - 1) .. " stages")

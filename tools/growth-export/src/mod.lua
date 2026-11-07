-- Offline EML export (emm run --export): every growth chain in the game data, one line per stage.
-- A growing template (a planted seedling) has keen::ecs::Upgrade, whose levels name the template
-- the plant turns into at each level. Writes growth-stages.tsv for tools/growth_stages.py.
-- For each stage it also says how the game harvests it (the "harvest" column):
--   refill     InteractionLootRefill: the plant stays and its stock refills (planted hazelnut, coffee, palms)
--   inventory  a real DefaultInventory with an interaction offer (bushes, beds)
--   pickup     PickupItem (picked up like ground loot)
--   hit        only ResourceNodePickupDrops/Destructible: harvested by hitting it with a tool (trees, artichoke)
--   none       nothing to harvest
-- destroy_on_loot is 1 when looting removes the plant; yield lists the refill items' ids.
if not loader.features.export then return end

local NULL_GUID = "00000000-0000-0000-0000-000000000000"

local function has(components, wanted)
  for _, c in ipairs(components) do
    if tostring(c.type) == wanted then return c.value end
  end
end

local function harvestOf(r)
  local components = (r and r.data.components) or {}
  local dol = has(components, "keen::ecs::DestroyOnLoot") and "1" or "0"
  local refill = has(components, "keen::ecs::InteractionLootRefill")
  if refill then
    local yield = {}
    for _, item in ipairs(refill.items or {}) do
      local ok, info = pcall(function() return game.assets.get_resource(item.lootableItemRef, "keen::ItemInfo") end)
      yield[#yield + 1] = ok and info and tostring(info.data.itemId.value) or "?"
    end
    return "refill", dol, table.concat(yield, ",")
  end
  local inventory = has(components, "keen::ecs::DefaultInventory")
  if inventory and tostring(inventory.defaultInventory) ~= NULL_GUID and
     has(components, "keen::ecs::InteractionOfferRuntime") then return "inventory", dol, "" end
  if has(components, "keen::ecs::PickupItem") then return "pickup", dol, "" end
  if has(components, "keen::ecs::ResourceNodePickupDrops") or has(components, "keen::ecs::Destructible") then
    return "hit", dol, ""
  end
  return "none", dol, ""
end

local byGuid = {}
local all = game.assets.get_resources_by_type("keen::ecs::TemplateResource")
for _, r in ipairs(all) do byGuid[tostring(r.guid)] = r end

local rows = { "root_guid\troot_name\troot_has_growth\tlevel\tstage_guid\tstage_name\tlast\tharvest\tdestroy_on_loot\tyield" }
for _, r in ipairs(all) do
  local components = r.data.components or {}
  local upgrade = has(components, "keen::ecs::Upgrade")
  if upgrade then
    local growth = has(components, "keen::ecs::Growth") and "1" or "0"
    local levels, top = upgrade.levels or {}, -math.huge
    local function levelOf(level) return tonumber(level.level) or 0 end
    -- last = the highest level number (the ripe stage), as in mod.lua's unripeStages
    for _, level in ipairs(levels) do top = math.max(top, levelOf(level)) end
    for _, level in ipairs(levels) do
      local stage = tostring(level.upgradeEntity)
      local harvest, dol, yield = harvestOf(byGuid[stage])
      rows[#rows + 1] = table.concat({ tostring(r.guid), tostring(r.data.name), growth, tostring(level.level),
        stage, byGuid[stage] and tostring(byGuid[stage].data.name) or "?", levelOf(level) == top and "1" or "0",
        harvest, dol, yield }, "\t")
    end
  end
end
io.export("growth-stages.tsv", table.concat(rows, "\n") .. "\n")
print("growth-stages: " .. (#rows - 1) .. " stages")

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

-- how the game harvests a stage, and its refill items
local function harvestOf(components)
  local offered = has(components, "keen::ecs::InteractionOfferRuntime")
  local refill = has(components, "keen::ecs::InteractionLootRefill")
  -- as mod.lua's ripeStages rule: a refill with an interaction offer
  if refill and offered then
    local yield = {}
    for _, item in ipairs(refill.items or {}) do
      local ok, info = pcall(function() return game.assets.get_resource(item.lootableItemRef, "keen::ItemInfo") end)
      yield[#yield + 1] = ok and info and tostring(info.data.itemId.value) or "?"
    end
    return "refill", table.concat(yield, ",")
  end
  local inventory = has(components, "keen::ecs::DefaultInventory")
  if inventory and tostring(inventory.defaultInventory) ~= NULL_GUID and offered then return "inventory", "" end
  if has(components, "keen::ecs::PickupItem") then return "pickup", "" end
  if has(components, "keen::ecs::ResourceNodePickupDrops") or has(components, "keen::ecs::Destructible") then
    return "hit", ""
  end
  return "none", ""
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
      local s = byGuid[stage]
      local stageComponents = (s and s.data.components) or {}
      local harvest, yield = harvestOf(stageComponents)
      rows[#rows + 1] = table.concat({ tostring(r.guid), tostring(r.data.name), growth, tostring(level.level),
        stage, s and tostring(s.data.name) or "?", levelOf(level) == top and "1" or "0",
        harvest, has(stageComponents, "keen::ecs::DestroyOnLoot") and "1" or "0", yield }, "\t")
    end
  end
end
io.export("growth-stages.tsv", table.concat(rows, "\n") .. "\n")
print("growth-stages: " .. (#rows - 1) .. " stages")

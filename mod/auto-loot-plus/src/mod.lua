-- Auto Loot Plus (EML) – data part
-- * Bodenbeute und Sammelstücke werden für den Magneten der dinput8.dll markiert
--   (nur mit touchZones=true zusätzlich der Einsammel-Bereich der Nebel-Kugeln wie bis 1.0.0)
-- * Leichen, Truhen, Büsche und Beete werden als "alles nehmen"-Ziele für die dinput8.dll markiert
-- * eine unsichtbare Aura am Spieler zerschlägt Fässer, Kisten, Vasen usw. (nie nahe einer Basis)
-- * Stapelgröße, optional bestes Rucksack-Rezept
-- Questgegenstände, Tiere, abgelegte Köder, geworfene Fackeln und eigene Truhen bleiben unverändert.

local TAG = "[Auto Loot Plus]"
local VERSION = "1.1.8"
local NULL_GUID = "00000000-0000-0000-0000-000000000000"

---------------------------------------------------------------------------
-- Einstellungen
---------------------------------------------------------------------------

local defaults = {
    enabled = true,
    radiusMeters = 15.0,
    collectGroundLoot = true,
    collectStones = true,
    collectCollectibles = true,
    collectPlants = true,
    collectFarmPlants = true,
    lootEnemyCorpses = true,
    lootChests = true,
    stackSize = 9999,
    autoSmash = true,
    smashDebugVisible = false,
    smashBuildableAwayFromBase = true,
    smashTrees = false,
    smashBeehives = false,
    cheatBestBackpack = false,
    smashDebugVanillaProgram = false,
    -- Einsammel-Bereiche der Nebel-Kugeln wie bis 1.0.0. Sie sammeln unabhängig vom Menü ein
    -- (auch bei "Automatisch einsammeln" aus); seit 1.0.1 erledigt das allein die DLL.
    touchZones = false,
    -- autoLoot, instantLoot, requireLineOfSight: nur die dinput8.dll (live im Menü)
}

local function strip(text)
    return (tostring(text):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function loadSettings()
    local result = {}
    for key, value in pairs(defaults) do result[key] = value end
    if not io.exists("config.txt") then return result end

    for rawLine in io.read_to_string("config.txt"):gmatch("[^\r\n]+") do
        local line = strip(rawLine)
        if line ~= "" and not line:match("^[#;]") then
            local key, value = line:match("^([%w_]+)%s*=%s*([^#;]-)%s*$")
            if key and defaults[key] ~= nil then
                if type(defaults[key]) == "boolean" then
                    local v = value:lower()
                    if v == "true" or v == "1" or v == "yes" or v == "on" then result[key] = true
                    elseif v == "false" or v == "0" or v == "no" or v == "off" then result[key] = false end
                else
                    result[key] = tonumber(value) or result[key]
                end
            end
        end
    end
    return result
end

local cfg = loadSettings()
if not cfg.enabled then
    print(TAG .. " deaktiviert (config.txt)")
    return
end
cfg.radiusMeters = math.max(1.0, math.min(50.0, cfg.radiusMeters))

---------------------------------------------------------------------------
-- Hilfsfunktionen
---------------------------------------------------------------------------

local T = {
    pickup = "keen::ecs::PickupItem",
    destroyOnLoot = "keen::ecs::DestroyOnLoot",
    zone = "keen::ecs::PickupItemZone",
    shape = "keen::ecs::TriggerShape",
    range = "keen::ecs::IsPlayerInRange",
    interaction = "keen::ecs::InteractionOffer",
    interactionRuntime = "keen::ecs::InteractionOfferRuntime",
    spawnTime = "keen::ecs::SpawnTime",
    gravity = "keen::ecs::Gravity",
    areaLoot = "keen::ecs::AreaLootOverride",
    inventory = "keen::ecs::Inventory",
    inventorySetup = "keen::ecs::InventorySetup",
    defaultInventory = "keen::ecs::DefaultInventory",
    resourceDrops = "keen::ecs::ResourceNodePickupDrops",
}

-- Komponenten, die nie automatisch eingesammelt werden dürfen (Tiere, Gegner, Köder ...)
local blockingComponents = {
    ["keen::ecs::Health"] = true,
    ["keen::ecs::Actor"] = true,
    ["keen::ecs::Wildlife"] = true,
    ["keen::ecs::FoodBait"] = true,
    [T.inventory] = true,
    [T.inventorySetup] = true,
}

local function field(object, key)
    local ok, value = pcall(function() return object[key] end)
    if ok then return value end
end

local function setField(object, key, value)
    if not object then return false end
    return (pcall(function() object[key] = value end))
end

local function findComponent(components, wanted)
    for index, component in ipairs(components) do
        if field(component, "type") == wanted then return index, field(component, "value") end
    end
end

local function removeAll(components, wanted)
    local count = 0
    for index = #components, 1, -1 do
        if field(components[index], "type") == wanted then
            table.remove(components, index)
            count = count + 1
        end
    end
    return count
end

local function deepCopy(value, seen)
    if type(value) ~= "table" and type(value) ~= "userdata" then return value end
    seen = seen or {}
    if seen[value] then return seen[value] end
    local copy = {}
    seen[value] = copy
    local ok = pcall(function()
        for k, v in pairs(value) do copy[deepCopy(k, seen)] = deepCopy(v, seen) end
    end)
    if not ok then
        -- Varianten (Komponenten-Hüllen) lassen sich nicht durchlaufen: { type, value }
        copy.type = value.type
        copy.value = deepCopy(value.value, seen)
    end
    return copy
end

local function lower(name) return tostring(name or ""):lower() end

---------------------------------------------------------------------------
-- Einordnung der Vorlagen
---------------------------------------------------------------------------

-- Namensmuster für Questgegenstände (Bewohner-Aufgaben, Hinweise, Kartenteile, Lore-Gegenstände)
local questPatterns = {
    "^lootpickup_npc_", "_npc_", "townsfolk", "quest", "_hint", "mappiece", "_lore_",
}
-- Test-, Demo- und veraltete Vorlagen sowie Spieler-Objekte
local junkPatterns = {
    "deprecated", "_test", "demo", "throwabletorch", "foodbait",
}
-- Vorlagen, die eindeutig Bodenbeute sind (auch Bücher auf Regalen: Prop_Deco_*_Lootpickup)
local lootNamePatterns = {
    "^_?lootpickup", "^base_lootpickup", "^scatter_loot_", "^loottouch_", "^base_loottouch", "_lootpickup$",
}

local function matchesAny(name, patterns)
    for _, pattern in ipairs(patterns) do
        if name:find(pattern) then return true end
    end
    return false
end

-- Steine am Boden und Steinhaufen: eigener Schalter innerhalb der Bodenbeute (Feuerstein zählt nicht dazu)
local function isStone(lname)
    return (lname:find("^lootpickup_gravity_stone") or lname:find("^lootpickup_material_stone_")) ~= nil
end

local function hasRealDefaultInventory(components)
    local _, value = findComponent(components, T.defaultInventory)
    local reference = value and field(value, "defaultInventory")
    return reference ~= nil and tostring(reference) ~= NULL_GUID
end

-- Liefert "loot", "collectible" oder nil (= nicht anfassen) und einen Grund für das Protokoll
local function classify(name, components)
    local lname = lower(name)
    if not findComponent(components, T.pickup) or not findComponent(components, T.destroyOnLoot) then
        return nil, "kein Pickup"
    end
    for _, component in ipairs(components) do
        if blockingComponents[field(component, "type")] then return nil, "Tier/Behälter" end
    end
    if hasRealDefaultInventory(components) then return nil, "Behälter" end
    if matchesAny(lname, questPatterns) then return nil, "Quest" end
    if matchesAny(lname, junkPatterns) then return nil, "Test/Spielerobjekt" end
    if lname:find("plant_growth") and not lname:find("_harvest") then
        return nil, "Beet"
    end

    local _, area = findComponent(components, T.areaLoot)
    local excluded = area and field(area, "exclude") == true
    local looksLikeLoot = matchesAny(lname, lootNamePatterns) or (area ~= nil and not excluded)
    if not looksLikeLoot then return nil, "keine Bodenbeute" end

    if excluded then
        if lname:find("plant_growth") then return "loot", nil end
        -- Pilze und Beeren am Boden (Scatter_Loot_*_01a) sind im Original wie Sammelstücke vom
        -- Einsammeln ausgenommen, gehören aber zur Bodenbeute
        if lname:find("^scatter_loot_") then return "plantPickup", nil end
        return "collectible", nil
    end
    return "loot", nil
end

-- Stufe 2 (seit 1.1.0): Alle Gruppen werden hier freigegeben; welche Gruppe gerade eingesammelt wird,
-- entscheidet die dinput8.dll zur Laufzeit anhand der Vorlage des Ziels (mods\auto-loot-plus\templates.txt)
-- und der Einstellungen – ohne Neustart. Die Schalter in der config.txt wirken deshalb nur noch dort, wo
-- Vorlagen beim Laden verändert werden (Zerschlagen, Steine-Brocken).

---------------------------------------------------------------------------
-- Bauplan für den Einsammel-Bereich aus Base_LootTouch_Orb
---------------------------------------------------------------------------

local templates = game.assets.get_resources_by_type("keen::ecs::TemplateResource")

local blueprint = {}
for _, resource in ipairs(templates) do
    if field(resource.data, "name") == "Base_LootTouch_Orb" then
        for _, kind in ipairs({ T.zone, T.shape, T.range }) do
            local _, value = findComponent(field(resource.data, "components"), kind)
            if value then blueprint[kind] = deepCopy(value) end
        end
        break
    end
end

if cfg.touchZones and not (blueprint[T.zone] and blueprint[T.shape] and blueprint[T.range]) then
    warn(TAG .. " Base_LootTouch_Orb nicht gefunden – Spielversion geändert? Ohne Einsammel-Bereiche weiter.")
    cfg.touchZones = false
end

local function tuneZone(kind, value)
    if kind == T.shape then
        setField(value, "shape", "Range")
        setField(value, "rangeX", cfg.radiusMeters)
        setField(value, "rangeY", cfg.radiusMeters)
        setField(value, "rangeZ", cfg.radiusMeters)
    elseif kind == T.range then
        setField(value, "minRange", 0.0)
        setField(value, "onlyPlayer", true)
    elseif kind == T.zone then
        setField(value, "lastAttempt", { value = 0 })
    end
end

local function addPickupZone(components)
    for _, kind in ipairs({ T.zone, T.shape, T.range }) do
        local index, value = findComponent(components, kind)
        if index then
            tuneZone(kind, value)
        else
            local copy = deepCopy(blueprint[kind])
            tuneZone(kind, copy)
            table.insert(components, { type = kind, value = copy })
        end
    end
end

---------------------------------------------------------------------------
-- Patchen
---------------------------------------------------------------------------

local stats = { loot = 0, collectible = 0, generic = 0, gravity = 0, emitters = 0, corpse = 0, plant = 0, farm = 0, chest = 0,
                invPickup = 0, plantPickup = 0, groundTarget = 0, groundContainer = 0, groundOff = 0, targetsOff = 0, stonesOff = 0,
                unclassified = 0 }
local supported = {}
local handled = {}   -- Vorlagen, die irgendeine Behandlung bekommen haben (für "unclassified" im Bericht)
local report = {}

-- Bodenbeute aus: Der Magnet überspringt Objekte mit AreaLootOverride.exclude = true (seine erste
-- Prüfung, automatisch wie beim Halten von E). Einzeln mit E aufheben geht weiter.
local function excludeFromAreaLoot(components)
    local _, area = findComponent(components, T.areaLoot)
    if area then
        setField(area, "exclude", true)
    else
        table.insert(components, { type = T.areaLoot, value = { exclude = true } })
    end
end

local function patchTemplate(resource, components, category, name)
    -- Der Magnet (automatisch wie beim Halten von E) nimmt alles, was nicht ausgeschlossen ist
    local _, area = findComponent(components, T.areaLoot)
    if area then setField(area, "exclude", false) end

    if cfg.touchZones then
        addPickupZone(components)
        -- Der 1.0-Einsammelbereich verträgt keine Interaktions-Komponenten (siehe XHL)
        removeAll(components, T.interaction)
        removeAll(components, T.interactionRuntime)
        removeAll(components, T.spawnTime)

        local _, gravity = findComponent(components, T.gravity)
        if gravity then
            setField(gravity, "isActive", false)
            setField(gravity, "value", 0.0)
            stats.gravity = stats.gravity + 1
        end
    end

    supported[tostring(resource.guid)] = true
    handled[tostring(resource.guid)] = true
    stats[category] = stats[category] + 1
    report[#report + 1] = category .. "\t" .. name
end

-- Allgemeine Drop-Vorlagen: haben keinen festen Gegenstand (PickupItem kommt erst beim
-- Erzeugen dazu). Daraus entstehen z. B. Holz aus zerschlagenen Objekten und Vasen-Beute.
local genericRefs = {}
for _, resource in ipairs(templates) do
    for _, component in ipairs(field(resource.data, "components") or {}) do
        local kind, value = field(component, "type"), field(component, "value")
        if kind == T.resourceDrops then
            genericRefs[tostring(field(value, "pickupItemReference"))] = true
        elseif kind == "keen::ecs::PickupLootSlot" then
            genericRefs[tostring(field(value, "genericPickupItem"))] = true
        end
    end
end
genericRefs[NULL_GUID] = nil

for _, resource in ipairs(templates) do
    local data = resource.data
    local name = tostring(field(data, "name"))
    local components = field(data, "components")
    if components then
        local category, reason = classify(name, components)
        if not category and genericRefs[tostring(resource.guid)] and reason == "kein Pickup" and
           findComponent(components, T.destroyOnLoot) then
            local blocked = hasRealDefaultInventory(components)
            for _, component in ipairs(components) do
                if blockingComponents[field(component, "type")] then blocked = true end
            end
            if not blocked then category = "generic" end
        end
        local lname = lower(name)
        local ground = category == "generic" or category == "plantPickup" or
                       (category == "loot" and not lname:find("plant_growth"))
        -- Bodenbeute mit eigenem Inventar (z. B. Steinhaufen): das Original sammelt sie über PickupItem
        -- automatisch ein; eingeordnet wird sie hier nur, um sie sperren zu können
        if not category and findComponent(components, T.pickup) and
           (lname:find("^lootpickup_material_") or lname:find("^lootpickup_gravity_")) then
            ground = true
        end
        -- (Gruppen-Sperren macht die DLL live; "ground" bleibt für den Bericht)
        if category then
            patchTemplate(resource, components, category, name)
        elseif reason == "Quest" or reason == "Sammelstück (aus)" then
            report[#report + 1] = "skip:" .. reason .. "\t" .. name
            handled[tostring(resource.guid)] = true
        end
    end
end

-- Abbau-Knoten (Bäume, Büsche ...): Drops ohne Wurfbewegung erzeugen, damit sie
-- ohne Schwerkraft nicht wegfliegen – nur wenn alle Drops unterstützt sind (nur mit touchZones).
local motionFields = { "velocityMin", "velocityMax", "yOffsetMin", "yOffsetMax", "dropRadiusMin", "dropRadiusMax" }
for _, resource in ipairs(cfg.touchZones and templates or {}) do
    local components = field(resource.data, "components")
    local _, drops = nil, nil
    if components then _, drops = findComponent(components, T.resourceDrops) end
    if drops and field(drops, "dropInWorld") == true and (tonumber(field(drops, "numberOfDrops")) or 0) > 0 then
        local allSupported, any = true, false
        local fallback = tostring(field(drops, "pickupItemReference"))
        for _, drop in ipairs(field(drops, "drops") or {}) do
            if (tonumber(field(drop, "weight")) or 0) > 0 then
                local entity = tostring(field(drop, "entity"))
                if entity == NULL_GUID then entity = fallback end
                if not supported[entity] then
                    allSupported = false
                else
                    any = true
                end
            end
        end
        if allSupported and any then
            for _, key in ipairs(motionFields) do setField(drops, key, 0.0) end
            stats.emitters = stats.emitters + 1
        end
    end
end

-- "Alles nehmen"-Ziele für die dinput8.dll: Sie sucht Objekte mit DisableColliderModeChanges
-- und schickt dem Spiel für sie ein "alles plündern". Das Spiel setzt die Markierung selbst,
-- wenn ein toter Gegner in Beute umgewandelt wird (transform_to_loot). Hier bekommen sie
-- zusätzlich Gegner mit eigenem Leichen-Behälter, Truhen, Büsche, Beete und Haufen.
-- Die Suche des Spiels ist grob: Sie liefert auch Nachbarobjekte, und das Spiel prüft jedes Objekt
-- danach selbst noch einmal. Seine erste Prüfung ist AreaLootOverride.exclude – deshalb bekommen
-- Ziele eingeschalteter Gruppen exclude=false (Freigabe) und Ziele ausgeschalteter Gruppen
-- exclude=true (Sperre, wie Fossilien im Original). So bleibt ein Ast-Busch liegen, auch wenn die
-- Suche ihn neben einer Truhe mitliefert.
local lootAllMarker = "keen::ecs::DisableColliderModeChanges"
local lootAllBlocked = {}   -- hier gesperrt: darf unten (Gegenstände mit Inventar) nicht wieder frei werden
local targetEnabled = {
    corpse = "lootEnemyCorpses", chest = "lootChests", groundTarget = "collectGroundLoot", groundContainer = "collectGroundLoot",
    plant = "collectPlants", farm = "collectFarmPlants",
}

local function lootAllCategory(name, family, components)
    local lname = lower(name)
    if name:find("^LootContainer_Enemy") or name:find("^Base_LootContainer_Enemy") then
        return "corpse"
    end
    -- Welt-Truhen mit Zufallsbeute und Möbel mit festem Inhalt. Eigene Lagertruhen haben
    -- keine Zufallsbeute und heißen anders; verschlossene Truhen und Quest-Truhen bleiben zu.
    if lname:find("^lootchest_") or lname:find("^base_lootchest_") then
        if lname:find("locked") or lname:find("cheat") or lname:find("deprecated") then return nil end
        return findComponent(components, "keen::ecs::RandomLoot") and "chest" or nil
    end
    if lname:find("^lootcontainer_prop_") then
        if lname:find("deprecated") then return nil end
        return "chest"
    end
    -- Durchsuchbare Objekte ("Durchsuchen"): Werkzeughaufen, Müllhaufen, Boss-Beute.
    -- Sie verschwinden nach dem Plündern. "_bomb"-Varianten explodieren beim Durchsuchen.
    if family == "Container" and findComponent(components, T.destroyOnLoot) and
       findComponent(components, T.interactionRuntime) and hasRealDefaultInventory(components) then
        if lname:find("bomb") or lname:find("^base") or
           matchesAny(lname, questPatterns) or matchesAny(lname, junkPatterns) then return nil end
        -- auch Müll-, Totholz- und Werkzeughaufen in Lagern
        return "chest"
    end
    if findComponent(components, T.pickup) or family == "Container" then return nil end
    -- Nachwachsende Erntebäume (Haselnuss im Tiefwald, Kaffeebaum): kein DestroyOnLoot, der Ertrag
    -- kommt über die Interaktion und der Baum bleibt stehen (20.10.2026, Christian: Büsche und Pflanzen)
    if lname:find("^scatter_loot_") and lname:find("_harvest") and not lname:find("unused") and
       hasRealDefaultInventory(components) and findComponent(components, T.interactionRuntime) and
       not findComponent(components, T.destroyOnLoot) then
        return "plant"
    end
    if not (hasRealDefaultInventory(components) and findComponent(components, T.destroyOnLoot) and
            findComponent(components, T.interactionRuntime)) then return nil end
    if matchesAny(lname, questPatterns) or matchesAny(lname, junkPatterns) or lname:find("^x_") then return nil end
    -- Nebelnest-Ressourcen in Höhlen (Archaische Essenz I–VII, zum Sockeln): Bodenbeute (20.10.2026, Christian)
    if lname:find("^fognest_ressource") then return "groundContainer" end
    -- Welt-Pflanzen alter Bauart ohne Scatter_Loot-Präfix (Papierschilf, Stroh, Weizenfeld): Pflanzen
    if lname:find("^plant_") and lname:find("grown") then return "plant" end
    if lname:find("plant_growth") then
        return lname:find("_harvest") and "farm" or nil
    end
    -- Ast-Büsche und -Haufen (Twigs) gehören zur Bodenbeute, nicht zu den Pflanzen
    if (lname:find("^scatter_loot_") or lname:find("^lootpickup_")) and lname:find("twig") then
        return "groundTarget"
    end
    -- Was Gegner beim Tod fallen lassen (Tierteile, Knochen, Fledermausfell): zu den Gegnerleichen
    if lname:find("^lootpickup_material_") then
        return "corpse"
    end
    if lname:find("^scatter_loot_") or lname:find("^lootpickup_") then
        return "plant"
    end
    return nil
end

for _, resource in ipairs(templates) do
    local data = resource.data
    local name = tostring(field(data, "name"))
    local components = field(data, "components")
    local category = components and lootAllCategory(name, tostring(field(data, "family")), components)
    -- Boss-Beute nie automatisch (Schlüssel und Quest-Fortschritt hängen am Plündern per Hand): sperren
    if category and lower(name):find("boss") then category = "boss" end
    if category == "boss" then
        -- nie automatisch: sperren, damit die grobe Suche das Objekt nicht als Nachbarn mitnimmt
        excludeFromAreaLoot(components)
        handled[tostring(resource.guid)] = true
        lootAllBlocked[tostring(resource.guid)] = true
        stats.targetsOff = stats.targetsOff + 1
        report[#report + 1] = "off:" .. category .. "\t" .. name
    elseif category then
        if not findComponent(components, lootAllMarker) then
            table.insert(components, { type = lootAllMarker, value = {} })
        end
        handled[tostring(resource.guid)] = true
        -- Freigabe: die erste Prüfung des Magneten für jedes Objekt
        local _, area = findComponent(components, T.areaLoot)
        if area then
            setField(area, "exclude", false)
        else
            table.insert(components, { type = T.areaLoot, value = { exclude = false } })
        end
        stats[category] = stats[category] + 1
        report[#report + 1] = category .. "\t" .. name
    end
end

-- Gegenstände mit eigenem Inventar (z. B. Pilze): der Magnet findet sie über PickupItem und
-- holt sie mit "alles nehmen". Sie dürfen nur nicht vom Einsammeln ausgeschlossen sein.
-- Dazu zählen auch wilde Leuchtpilze (AzureRussula_GlowShroom) und reife Pilzbeete (Plant_Growth_*_harvest).
for _, resource in ipairs(templates) do
    local data = resource.data
    local name = tostring(field(data, "name"))
    local lname = lower(name)
    local components = field(data, "components")
    local nameFits = lname:find("^scatter_loot_") or lname:find("^lootpickup_") or lname:find("shroom") or lname:find("mushroom")
    local seedling = lname:find("plant_growth") and not lname:find("_harvest")
    if components and not supported[tostring(resource.guid)] and findComponent(components, T.pickup) and
       hasRealDefaultInventory(components) and nameFits and not seedling and
       not lootAllBlocked[tostring(resource.guid)] and
       not matchesAny(lname, questPatterns) and not matchesAny(lname, junkPatterns) then
        local blocked = false
        for _, component in ipairs(components) do
            if blockingComponents[field(component, "type")] then blocked = true end
        end
        -- Insekten-Schwärme (Glühwürmchen, Motten ...) gehören zu den Pflanzen, Pilzbeete zu den
        -- Beeten, Pilze und Steinhaufen zur Bodenbeute – die Gruppe entscheidet die DLL live
        local swarm = lname:find("^lootpickup_scatter_vfx_") ~= nil
        if not blocked then
            local _, area = findComponent(components, T.areaLoot)
            if area then
                setField(area, "exclude", false)
            else
                table.insert(components, { type = T.areaLoot, value = { exclude = false } })
            end
            -- Insekten-Schwärme (Glühwürmchen, Motten ...): Der Magnet prüft den Einzelgegenstand,
            -- der bei ihnen leer ist, und überspringt sie. Ohne PickupItem werden sie wie Büsche
            -- über "Interaktion annehmen" + "alles nehmen" geholt. Sie sind reine Effekte ohne Modell.
            if swarm then
                removeAll(components, T.pickup)
                removeAll(components, "keen::ecs::PickupItemModelSetup")
                if not findComponent(components, lootAllMarker) then
                    table.insert(components, { type = lootAllMarker, value = {} })
                end
            end
            stats.invPickup = stats.invPickup + 1
            handled[tostring(resource.guid)] = true
            report[#report + 1] = "invPickup\t" .. name
        end
    end
end

-- Gegenstände ohne jede Einordnung (nur für den Bericht): Einzelstücke sammelt der Magnet wie im
-- normalen Spiel ein, solche mit Inventar nimmt die DLL ohne AreaLootOverride nicht an.
for _, resource in ipairs(templates) do
    local components = field(resource.data, "components")
    if components and not handled[tostring(resource.guid)] and findComponent(components, T.pickup) then
        local name = tostring(field(resource.data, "name"))
        local _, reason = classify(name, components)
        stats.unclassified = stats.unclassified + 1
        report[#report + 1] = "unclassified:" .. tostring(reason) .. "\t" .. name
    end
end

---------------------------------------------------------------------------
-- Automatisch zerschlagen: eine unsichtbare Aura am Spieler trifft jede Sekunde alle
-- markierten Welt-Objekte im Radius (Impact-Programm aus impact/autosmash.impact).
---------------------------------------------------------------------------

local SMASH_TAG = "a5c0de5a-1e00-4d0d-9a11-0000000a5001"
local SMASH_TEMPLATE = "a5c0de5a-1e00-4d0d-9a11-0000000a5002"
local SMASH_SHARED_TAG = "a5c0de5a-1e00-4d0d-9a11-0000000a5003"
local SMASH_ALTAR_TAG = "a5c0de5a-1e00-4d0d-9a11-0000000a5004"
local SMASH_STONE_TAG = "a5c0de5a-1e00-4d0d-9a11-0000000a5005"   -- Gesteinsbrocken (Steine-Schalter)
local SMASH_TREE_TAG = "a5c0de5a-1e00-4d0d-9a11-0000000a5007"    -- Bäume (Schalter "Bäume fällen")
local SMASH_OFF_TAG = "a5c0de5a-1e00-4d0d-9a11-0000000a5009"     -- trägt kein Objekt: Markierungs-Platz "aus"
stats.smash = 0

-- Seit 1.1.0 wird die Aura immer angelegt. Die Schalter stehen als Werte im Datenblock des
-- Programms: Range::value (0 = Zerschlagen aus), SharedTag::value (Aus-Markierung = baubare Möbel
-- nicht) und StoneTag::value (echte Markierung = Gesteinsbrocken auslassen). Hier werden sie beim
-- Laden aus der config gesetzt; die dinput8.dll schreibt dieselben Plätze im laufenden Spiel um.
local function guidBytesLE(guid)
    local hex = guid:gsub("-", "")
    local bytes = {}
    for i = 1, 16 do bytes[i] = tonumber(hex:sub(i * 2 - 1, i * 2), 16) end
    -- die ersten drei Gruppen (4, 2, 2 Byte) liegen im Speicher in umgekehrter Byte-Reihenfolge
    local le = { bytes[4], bytes[3], bytes[2], bytes[1], bytes[6], bytes[5], bytes[8], bytes[7] }
    for i = 9, 16 do le[#le + 1] = bytes[i] end
    return le
end

-- Nur eindeutige Welt-Objekte: ausdrücklich zerstörbare Props, Objekte mit Zufallsbeute beim
-- Zerstören und kaputte Möbel in Ruinen. Alles Baubare bleibt unangetastet.
local smashExclude = {
    "spawn", "nest", "enemy", "door", "npc", "crane", "elevator", "rope", "gate", "barricade", "bridge", "trap", "quest", "totem",
    "altar", "shrine", "statue", "pillar", "column", "stair", "ladder", "platform", "bomb", "explosive",
    "player", "flame", "lore", "switch", "lever", "deprecated", "_test", "debug",
    "craftable", "secret", "writable", "sign", "flower_pot", "lantern", "illumination", "hitable", "cozy",
    "brazier", "candle", "torch", "lamp", "^base", "^lootcontainer", "donotplace", "support",
    "palisade", "decorative", "mirror", "curtain", "pillow", "bed",
    -- Kletter-/Rätsel-Elemente und Kampf-Objekte
    "chains", "fence", "lattice", "hookshot", "grate", "lizardpeople", "obsidian", "spire", "hint", "egg",
    -- Nebelfässer (setzen Sporen/Nebel frei), Boss-Balliste, Lichtquellen, Sammlungs-Vitrinen
    "fog_barrel", "fogspores", "balista", "firefly", "collectible",
}

-- Möbel in der Welt (auch intakte). Baubare Möbel sind ausgeschlossen, weil ein Bau-Gegenstand
-- auf ihre Vorlage verweist (siehe "buildable" unten).
-- Kurze Wörter mit Trennzeichen, damit z. B. "vat" nicht in "Elevator"/"Cultivation" trifft.
local smashFurniture = {
    "table", "chair", "bench", "stool", "seat", "shelf", "cupboard", "sideboard", "closet", "wardrobe",
    "cabinet", "_vat", "bathtub", "_tub", "bucket", "barrel", "crate", "basket", "sack", "bundle", "pottery",
    "vase", "_jar", "_urn", "toilet", "_sink",
}

local function isSmashTarget(lname, components)
    -- Wände (Geheimgänge, Mauern) nie, Wandregale und die Möbelserie "WallBricks03_Furniture" schon
    if lname:find("wall") and not (lname:find("shelf") or lname:find("furniture")) then return false end
    if lname:find("^prop_destructible_") then return true end
    if findComponent(components, "keen::ecs::RandomLootOnDestroy") then return true end
    if lname:find("furniture") and lname:find("broken") then return true end
    local isProp = lname:find("^props?_") or lname:find("^_?dummy_prop")
    return isProp ~= nil and matchesAny(lname, smashFurniture)
end

do
    local program = require("autosmash_program")

    -- Radien aus der config in die Programmdaten schreiben (float32, little endian).
    -- Basis-Schutz: Ein Altar-Baubereich misst höchstens 160 m; gemessen wird vom Spieler aus,
    -- zerschlagen wird bis radiusMeters um ihn herum -> Altar näher als 160 m + Radius = nichts tun.
    local floatValues = {
        ["Range::value"] = cfg.autoSmash and cfg.radiusMeters or 0.0,
        ["BaseRange::value"] = 160.0 + cfg.radiusMeters,
    }
    local guidValues = {
        ["SharedTag::value"] = cfg.smashBuildableAwayFromBase and SMASH_SHARED_TAG or SMASH_OFF_TAG,
        ["StoneTag::value"] = cfg.collectStones and SMASH_OFF_TAG or SMASH_STONE_TAG,
    }
    for _, entry in ipairs(program.dataLayout) do
        local value = floatValues[entry.dbgName]
        if value then
            local buf = buffer.create(4)
            buf:write_f32(value)
            for i = 0, 3 do program.data[entry.offsetInBytes + i + 1] = buf:read_u8() end
        end
        local guid = guidValues[entry.dbgName]
        if guid then
            local bytes = guidBytesLE(guid)
            for i = 1, 16 do program.data[entry.offsetInBytes + i] = bytes[i] end
        end
    end

    local okProgram, programResource = pcall(game.assets.create_resource, program, "keen::ImpactProgram",
        program.programGuid, 0)
    if not okProgram then
        warn(TAG .. " Zerschlagen: Programm konnte nicht angelegt werden: " .. tostring(programResource))
    else
        -- In die zentrale Programmliste eintragen
        for _, registry in ipairs(game.assets.get_resources_by_type("keen::ImpactRegistryResource")) do
            local programs = field(registry.data, "programs")
            if programs then table.insert(programs, program.programGuid) end
        end

        -- Aura-Vorlage nach dem Vorbild des Skills "Massenzerstörung", ohne Kollision
        local blueprintTemplate, subEntityBlueprint
        local transformComponents = {}
        for _, resource in ipairs(templates) do
            local name = tostring(field(resource.data, "name"))
            if name == "Skill_MassDestruction_DealDamageSphere_AllEnemy" then blueprintTemplate = resource end
            if name == "Zone_Enemy_Scavenger_Flamethrower_Discharge" then
                local kinds = { "keen::ecs::CurrentTransform", "keen::ecs::RenderTransform", "keen::ecs::ImpactHierarchy" }
                if cfg.smashDebugVisible then
                    -- Diagnose: Flammen-Effekt, damit sichtbar ist, ob die Aura am Spieler erscheint
                    for _, kind in ipairs({ "keen::ecs::VfxComponent", "keen::ecs::VfxComponentResource",
                                            "keen::ecs::VfxParametersTemplateComponent" }) do
                        kinds[#kinds + 1] = kind
                    end
                end
                for _, kind in ipairs(kinds) do
                    local _, value = findComponent(field(resource.data, "components"), kind)
                    if value then transformComponents[#transformComponents + 1] = { type = kind, value = deepCopy(value) } end
                end
            end
            if not subEntityBlueprint then
                local _, sub = findComponent(field(resource.data, "components") or {}, "keen::ecs::SubEntities")
                local list = sub and field(sub, "subEntities")
                if list and list[1] then subEntityBlueprint = deepCopy(list[1]) end
            end
        end

        if blueprintTemplate and subEntityBlueprint then
            local aura = deepCopy(blueprintTemplate.data)
            aura.name = "AutoLootPlus_SmashAura"
            -- Die Kollisions-Komponenten des Vorbilds bleiben: die Umkreis-Suche des
            -- Impact-Systems braucht sie (Kollider prüft gegen "Combat").
            local keep = {}
            for _, component in ipairs(aura.components) do
                local kind = component.type
                if kind == "keen::ecs::Impact" and not cfg.smashDebugVanillaProgram then
                    component.value.program = program.programGuid
                    component.value.programId = { value = program.id.value }
                end
                keep[#keep + 1] = component
            end
            for _, component in ipairs(transformComponents) do keep[#keep + 1] = component end
            aura.components = keep
            -- Die Aura kann schon da sein: Mod doppelt geladen (zwei Mod-Ordner) oder eine enshrouded.kfc,
            -- die die Änderungen der Mod schon enthält. EML bräche dann mit "resource ... already exists"
            -- ab (neosquall777, 22.10.2026) - die vorhandene Vorlage wird benutzt.
            local auraExists = false
            for _, resource in ipairs(templates) do
                if tostring(resource.guid) == SMASH_TEMPLATE then auraExists = true end
            end
            if auraExists then
                warn(TAG .. " Zerschlagen: Aura-Vorlage schon vorhanden (Mod doppelt installiert oder enshrouded.kfc " ..
                     "enthält schon Änderungen der Mod) - die vorhandene wird benutzt")
            else
                game.assets.create_resource(aura, "keen::ecs::TemplateResource", SMASH_TEMPLATE, 0)
            end
            local auraTypes = {}
            for _, component in ipairs(aura.components) do auraTypes[#auraTypes + 1] = tostring(component.type) end
            report[#report + 1] = "smashAuraComponents\t" .. table.concat(auraTypes, ",")
            report[#report + 1] = "smashSubEntity\t" .. tostring(subEntityBlueprint.attachmentTarget) .. "/" ..
                tostring(subEntityBlueprint.attachmentType) .. "/" .. tostring(subEntityBlueprint.slot)

            -- Aura an die Spielerfigur hängen; addImpactHierarchy macht den Spieler zur Schadensquelle
            local info = subEntityBlueprint
            info.templateRef = SMASH_TEMPLATE
            info.attach = true
            info.hasKnowledgeRequirement = false
            info.despawnOnDeath = false
            info.addDespawnLink = true
            info.addOwnerRelationship = false
            info.doNotSave = true
            info.addImpactHierarchy = true
            info.copyFaction = true
            info.copyLevel = true
            for _, resource in ipairs(templates) do
                local name = tostring(field(resource.data, "name"))
                if name == "1_Player" or name == "1_Player_AG2" then
                    local components = field(resource.data, "components")
                    local _, sub = findComponent(components, "keen::ecs::SubEntities")
                    local attached = false
                    for _, entry in ipairs(sub and field(sub, "subEntities") or {}) do
                        if tostring(field(entry, "templateRef")) == SMASH_TEMPLATE then attached = true end
                    end
                    if attached then
                        -- schon angehängt (siehe oben): keine zweite Aura
                    elseif sub then
                        table.insert(field(sub, "subEntities"), deepCopy(info))
                    else
                        table.insert(components, { type = "keen::ecs::SubEntities",
                            value = { subEntities = { deepCopy(info) }, hasSavedChildren = false } })
                    end
                    report[#report + 1] = "smashAura\t" .. name
                end
            end

            -- Baubare Objekte (deine eigenen Möbel) nie markieren: jede Vorlage, auf die irgendein
            -- Gegenstand verweist, gilt als baubar. Welt-Möbel sind eigene Vorlagen ohne Gegenstand.
            local buildable = {}
            local guidPattern = "%x%x%x%x%x%x%x%x%-%x%x%x%x%-%x%x%x%x%-%x%x%x%x%-%x%x%x%x%x%x%x%x%x%x%x%x"
            for index, item in ipairs(game.assets.get_resources_by_type("keen::ItemInfo")) do
                for guid in tostring(item.data):gmatch(guidPattern) do buildable[guid] = true end
                if index % 200 == 0 then collectgarbage() end
            end

            -- Markierung + Welt-Fraktion setzen. Die Umkreis-Suche des Impact-Systems findet nur
            -- Objekte mit Fraktion; "Environment" ist die neutrale Welt-Fraktion (wie beim Vorbild).
            -- tagList: eine Markierung oder eine Liste. Alle Markierungen einer Vorlage in EINEM Aufruf
            -- setzen: Ein hier neu eingefügter Behälter wird vom Loader umgewandelt, ein zweites
            -- table.insert in dessen Liste bricht dann mit "IncompatibleType" ab (EML 0.2.0).
            local function addTag(components, tagList)
                if type(tagList) ~= "table" then tagList = { tagList } end
                local entries = {}
                for i, tag in ipairs(tagList) do entries[i] = { tag = tag } end
                local _, tags = findComponent(components, "keen::ecs::EntityTagContainer")
                if tags then
                    for _, entry in ipairs(entries) do table.insert(field(tags, "tags"), entry) end
                else
                    table.insert(components, { type = "keen::ecs::EntityTagContainer", value = { tags = entries } })
                end
                if not findComponent(components, "keen::ecs::Faction") then
                    table.insert(components, { type = "keen::ecs::Faction", value = { faction = "Environment" } })
                end
            end

            stats.smashShared, stats.smashStones, stats.altars, stats.smashTrees = 0, 0, 0, 0

            -- Basisschutz macht das Programm am Spieler: In der "sicheren Zone" des Spiels (Basis) nichts,
            -- "nahe Baubereich" keine baubaren Möbel. An den Altären wird nichts verändert: Eine eigene
            -- Altar-Suche fand ihre Ziele nie, weder den Build-Inhibitor (bis 1.1.3) noch einen eigenen
            -- Marker mit Kollision (22.10.2026, mit Feuerwerk-Markierungen geprüft).

            for _, resource in ipairs(templates) do
                local name = tostring(field(resource.data, "name"))
                local lname = lower(name)
                local components = field(resource.data, "components")
                -- Basis-Schutz: Das Programm prüft die Zustände "InSaveZone"/"IsNearBuildingZone" der
                -- Spielerfigur und zerschlägt in oder nahe einer Basis gar nichts.
                if components and findComponent(components, "keen::ecs::Destructible") and
                   not findComponent(components, "keen::ecs::Health") and
                   not findComponent(components, "keen::ecs::Actor") and
                   not matchesAny(lname, smashExclude) and isSmashTarget(lname, components) then
                    local tags
                    if not buildable[tostring(resource.guid)] then
                        -- reines Welt-Objekt: immer zerschlagen
                        tags = { SMASH_TAG }
                        stats.smash = stats.smash + 1
                        report[#report + 1] = "smash\t" .. name
                    else
                        -- auch baubar (Fässer, Stoffballen ...): nur fern jeder Basis zerschlagen;
                        -- der Schalter dazu sitzt im Programm (SharedTag::value), nicht an der Vorlage
                        tags = { SMASH_SHARED_TAG }
                        stats.smashShared = stats.smashShared + 1
                        report[#report + 1] = "smashShared\t" .. name
                    end
                    -- Gesteinsbrocken zusätzlich markieren: Steine aus = das Programm lässt sie stehen,
                    -- sonst bliebe ihr Schutt liegen (Schalter im Programm: StoneTag::value)
                    if lname:find("^prop_destructible_resource_stone_") then
                        tags[#tags + 1] = SMASH_STONE_TAG
                        stats.smashStones = stats.smashStones + 1
                    end
                    addTag(components, tags)
                elseif cfg.smashTrees and components and
                       (lname:find("^scatter_loot_") or lname:find("^prop_fogger_lightplant")) and
                       not lname:find("_harvest") and
                       not lname:find("unused") and findComponent(components, "keen::ecs::Destructible") and
                       not findComponent(components, "keen::ecs::Health") and
                       not findComponent(components, "keen::ecs::Actor") then
                    -- Bäume fällen (1.1.7): Holz-Abbauknoten der Welt (Bäume, Stümpfe, umgestürzte Stämme,
                    -- Totholz). Geschützt wie baubare Möbel; Ernte-Bäume (Haselnuss, Kaffee) bleiben stehen.
                    -- Dazu die Leuchtpflanzen der Nebel-Lager (Prop_Fogger_Lightplant_*, Quelle von "Aktives
                    -- Myzel"): ebenfalls Holz-Knoten, heißen aber nicht scatter_loot_ (23.10.2026).
                    local _, node = findComponent(components, "keen::ecs::MiningNode")
                    if node and tostring(field(node, "resourceType")) == "Wood" then
                        addTag(components, SMASH_TREE_TAG)
                        stats.smashTrees = stats.smashTrees + 1
                        report[#report + 1] = "smashTree\t" .. name
                    end
                elseif (cfg.smashTrees or cfg.smashBeehives) and components and lname:find("^lootpickup_") and
                       lname:find("beehive") and findComponent(components, "keen::ecs::Destructible") and
                       not findComponent(components, "keen::ecs::Health") then
                    if cfg.smashBeehives then
                        -- Schalter "Bienenstöcke zerschlagen" (moosers, 23.10.2026): wie Fässer, Basis-Schutz im Programm
                        addTag(components, buildable[tostring(resource.guid)] and SMASH_SHARED_TAG or SMASH_TAG)
                        stats.smashBeehives = (stats.smashBeehives or 0) + 1
                        report[#report + 1] = "smashBeehive\t" .. name
                    else
                        -- Bienenstöcke hängen an Bäumen und blieben nach dem Fällen in der Luft (22.10.2026):
                        -- sie fallen mit den Bäumen (Honig sammelt die Mod ein)
                        addTag(components, SMASH_TREE_TAG)
                        stats.smashTrees = stats.smashTrees + 1
                        report[#report + 1] = "smashTree\t" .. name
                    end
                end
            end
        else
            warn(TAG .. " Zerschlagen: Vorlage für die Aura nicht gefunden – nichts verändert")
        end
    end
end

-- Schummel-Schalter: Alle Rucksack-Rezepte liefern den besten Rucksack (Backpack_T3_NEW, +32
-- Plätze) und kosten je Zutat nur 1 Stück. Nach dem Herstellen wieder ausschalten.
stats.backpackRecipes = 0
if cfg.cheatBestBackpack then
    local best = "e43e65ad-24d6-4f1e-8c76-0270ef5ab0c6"   -- Backpack_T3_NEW
    local bestId
    for _, item in ipairs(game.assets.get_resources_by_type("keen::ItemInfo")) do
        if tostring(item.guid) == best then bestId = field(field(item.data, "itemId"), "value") end
    end
    for _, registry in ipairs(game.assets.get_resources_by_type("keen::RecipeRegistryResource")) do
        local recipes = field(registry.data, "recipes")
        for i = 1, #recipes do
            local recipe = recipes[i]
            if tostring(field(recipe, "debugName")):find("^Special_Backpack_") and bestId then
                for _, out in ipairs(field(recipe, "output") or {}) do
                    setField(out, "itemRef", best)
                    setField(out, "item", { value = bestId })
                    setField(out, "count", 1)
                end
                for _, input in ipairs(field(recipe, "input") or {}) do
                    local stack = field(input, "itemStack")
                    if stack and (tonumber(field(stack, "count")) or 0) > 1 then setField(stack, "count", 1) end
                end
                stats.backpackRecipes = stats.backpackRecipes + 1
            end
        end
    end
    report[#report + 1] = "cheatBestBackpack\t" .. stats.backpackRecipes .. " recipes"
end

-- Stapelgröße: stapelbare Gegenstände (Spiel-Standard > 1) auf cfg.stackSize anheben.
-- Einzelstücke (Waffen, Rüstung ...) bleiben bei 1, größere Original-Stapel bleiben erhalten.
stats.stacks = 0
if cfg.stackSize > 0 then
    local target = math.floor(math.max(1, math.min(65535, cfg.stackSize)))
    for _, resource in ipairs(game.assets.get_resources_by_type("keen::ItemInfo")) do
        local current = tonumber(field(resource.data, "maxStackSize")) or 0
        if current > 1 and current < target and setField(resource.data, "maxStackSize", target) then
            stats.stacks = stats.stacks + 1
        end
    end
end

if loader.features.export then
    -- Erkundung für "Karte aufdecken": Träger von FogOfWarDiscovery, Markierungstypen und
    -- Markierungen mit ihren Sichtbarkeits-Bedingungen (nur Bericht, nichts wird geändert)
    for _, resource in ipairs(templates) do
        local name = tostring(field(resource.data, "name"))
        local components = field(resource.data, "components") or {}
        local _, fow = findComponent(components, "keen::ecs::FogOfWarDiscovery")
        if fow then report[#report + 1] = "map:fow\t" .. name .. "\t" .. tostring(field(fow, "discoveryRange")) end
        local _, marker = findComponent(components, "keen::ecs::MapMarker")
        if marker then
            local _, unlock = findComponent(components, "keen::ecs::MapMarkerUnlockRequirements")
            local vis = unlock and field(unlock, "visibleState")
            local visText = "-"
            if vis then
                local ok, dumped = pcall(function()
                    local parts = {}
                    for k, v in pairs(vis) do parts[#parts + 1] = tostring(k) .. "=" .. tostring(v) end
                    table.sort(parts)
                    return table.concat(parts, ",")
                end)
                visText = ok and dumped or tostring(vis)
            end
            report[#report + 1] = "map:marker\t" .. name .. "\t" .. tostring(field(marker, "markerType")) .. "\t" .. visText
        end
    end
    -- alle Ressourcentypen, die EML kennt (gibt es Szenen-/Wissens-Daten darunter?)
    local okTypes, typeList = pcall(game.assets.get_resource_types)
    if okTypes then
        for _, typeObject in ipairs(typeList) do
            report[#report + 1] = "map:restype\t" .. tostring(field(typeObject, "qualified_name") or typeObject)
        end
    else
        report[#report + 1] = "map:restype\tFEHLER " .. tostring(typeList)
    end
    -- Bauteile der Markierungs-Vorlagen, je Typ gesammelt
    local partsByType = {}
    for _, resource in ipairs(templates) do
        local components = field(resource.data, "components") or {}
        local _, marker = findComponent(components, "keen::ecs::MapMarker")
        if marker then
            local key = tostring(field(marker, "markerType"))
            partsByType[key] = partsByType[key] or {}
            for _, component in ipairs(components) do
                partsByType[key][tostring(field(component, "type"))] = true
            end
        end
    end
    for key, parts in pairs(partsByType) do
        local names = {}
        for part in pairs(parts) do names[#names + 1] = (part:gsub("^keen::ecs::", "")) end
        table.sort(names)
        report[#report + 1] = "map:parts\t" .. key .. "\t" .. table.concat(names, ",")
    end
    for _, registry in ipairs(game.assets.get_resources_by_type("keen::MapMarkerRegistryResource")) do
        local markers = field(registry.data, "mapMarkers")
        local values = markers and field(markers, "values")
        local keys = markers and field(markers, "keys")
        for index, mt in ipairs(values or {}) do
            report[#report + 1] = "map:type\t" .. tostring(keys and keys[index]) .. "\t" .. tostring(field(mt, "typeName")) .. "\t" .. tostring(field(mt, "sortingCategory")) ..
                "\tfastTravel=" .. tostring(field(mt, "isFastTravelDestination")) .. "\tfilter=" .. tostring(field(mt, "canBeFiltered")) ..
                "\taboveFog=" .. tostring(field(mt, "showAboveFogOfWar"))
        end
    end
    -- Boss-Beute (Kommentare: Ring + Schlüssel vom Pike-Boss, Quest "besiegen und plündern"):
    -- Bauteile aller Boss-Vorlagen und die Felder der beute-/wissensbezogenen Bauteile
    local function shallow(value)
        local ok, text = pcall(function()
            local parts = {}
            for k, v in pairs(value) do
                local s = tostring(v)
                if type(v) == "table" then
                    local inner = {}
                    for k2, v2 in pairs(v) do inner[#inner + 1] = tostring(k2) .. "=" .. tostring(v2) end
                    table.sort(inner)
                    s = "{" .. table.concat(inner, ","):sub(1, 300) .. "}"
                end
                parts[#parts + 1] = tostring(k) .. "=" .. s
            end
            table.sort(parts)
            return table.concat(parts, " ")
        end)
        return ok and text or tostring(value)
    end
    for _, resource in ipairs(templates) do
        local name = tostring(field(resource.data, "name"))
        local components = field(resource.data, "components") or {}
        local isBoss = findComponent(components, "keen::ecs::Boss") ~= nil
        if isBoss or lower(name):find("boss") then
            local names = {}
            for _, component in ipairs(components) do
                local kind = tostring(field(component, "type"))
                names[#names + 1] = (kind:gsub("^keen::ecs::", ""))
                if kind:find("Loot") or kind:find("Knowledge") or kind:find("Inventory") or kind:find("OnDestroy") or
                   kind:find("Transform") and not kind:find("Transform$") then
                    report[#report + 1] = "boss:field\t" .. name .. "\t" .. kind:gsub("^keen::ecs::", "") .. "\t" ..
                                          shallow(field(component, "value"))
                end
            end
            table.sort(names)
            report[#report + 1] = "boss:" .. (isBoss and "parts" or "named") .. "\t" .. name .. "\t" .. tostring(resource.guid) ..
                                  "\t" .. table.concat(names, ",")
        end
    end
    -- Gegenstands-Nummer -> interner Name, für die Diagnose im auto-loot-plus.log
    for _, item in ipairs(game.assets.get_resources_by_type("keen::ItemInfo")) do
        local id = field(field(item.data, "itemId"), "value")
        if id then report[#report + 1] = "item\t" .. tostring(id) .. "\t" .. tostring(field(item.data, "debugName")) end
    end
    table.sort(report)
    io.export("auto-loot-plus-report.txt", table.concat(report, "\n"))
end

print(string.format(
    "%s v%s radius=%.1fm touchZones=%s loot=%d collectibles=%d generic=%d groundOff=%d groundTargets=%d targetsOff=%d stonesOff=%d unclassified=%d gravity=%d emitters=%d corpses=%d plants=%d farm=%d chests=%d invPickups=%d plantPickups=%d stacks=%d(->%d) smash=%d smashShared=%d smashStones=%d smashTrees=%d smashBeehives=%d altars=%d autoSmash=%s",
    TAG, VERSION, cfg.radiusMeters, tostring(cfg.touchZones),
    stats.loot, stats.collectible, stats.generic, stats.groundOff, stats.groundTarget, stats.targetsOff, stats.stonesOff, stats.unclassified, stats.gravity, stats.emitters, stats.corpse, stats.plant, stats.farm,
    stats.chest, stats.invPickup, stats.plantPickup, stats.stacks, cfg.stackSize, stats.smash, stats.smashShared or 0, stats.smashStones or 0, stats.smashTrees or 0,
    stats.smashBeehives or 0, stats.altars or 0, tostring(cfg.autoSmash)
))

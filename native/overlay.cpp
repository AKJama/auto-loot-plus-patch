// Auto Loot Plus – in-game settings menu (F8)
//
// Draws a Dear ImGui window into the game's own Vulkan swapchain:
//   * The game fetches vkGetInstanceProcAddr with GetProcAddress. That import of enshrouded.exe is
//     redirected, so every Vulkan function the game asks for passes our resolver, which hands out
//     wrappers for instance/device/queue/surface/swapchain creation and vkQueuePresentKHR.
//   * vkQueuePresentKHR: while the menu is open, one extra render pass draws the menu on top of the
//     finished frame (waits on the game's semaphores, signals our own one for the present).
//   * Keys and the cursor are polled (GetAsyncKeyState/GetCursorPos); mouse clicks are also taken from
//     the window messages, so short clicks between two frames are not lost. While the menu is open
//     the game window gets no keyboard/mouse messages, and the game cannot recenter or clip the cursor.
// Nothing is drawn or submitted while the menu is closed.

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

void Log(const char* fmt, ...);
extern const char* const kModVersion;   // main.cpp
void ApplyLiveSetting(const std::string& key, const std::string& value);
bool IsUnknownIgnoreEntry(const char* entry);
const std::wstring& GameDirectory();
struct ItemSuggestion { uint32_t id; const char* internalName; const char* displayName; };   // main.cpp
size_t SuggestItems(const char* query, bool german, const char* category, ItemSuggestion* out, size_t max);
const char* ItemDisplayName(const char* internalName, bool german);
const char* ItemDisplayNameById(uint32_t id, bool german);
bool ItemNamesMissing();
bool FilterNotice(uint32_t* id, uint64_t* tick);
bool InventoryFullNotice(uint64_t* tick);

static const ImVec4 kWarnColor(0.9f, 0.45f, 0.35f, 1.0f);   // menu lines about a problem

// Item groups for the filter list (game categories of lootable items)
struct ItemGroup { const char* category; const char* de; const char* en; };
static const ItemGroup kItemGroups[] = {   // keys = column 7 of items.txt
    { "", "Alle Gruppen", "All groups" },
    { "Materials", "Materialien", "Materials" },
    { "Terrain", "Boden und Gestein", "Soil and rock" },
    { "Keys", "Schlüssel", "Keys" },
    { "Bait", "Köder und Futter", "Bait and animal food" },
    { "Food", "Nahrung", "Food" },
    { "Potions", "Tränke und Verbände", "Potions and bandages" },
    { "Ammunition", "Munition und Zauber", "Ammunition and spells" },
    { "Armor", "Rüstung", "Armor" },
    { "Vanity", "Kleidung", "Clothing" },
    { "Rings", "Ringe", "Rings" },
    { "Shields", "Schilde", "Shields" },
    { "Weapons", "Waffen", "Weapons" },
    { "Tools", "Werkzeuge, Dietriche, Fackeln, Angeln", "Tools, lockpicks, torches, rods" },
    { "Collectibles", "Sammelstücke", "Collectibles" },
    { "ColorPalette", "Farbpaletten", "Color palettes" },
    { "WeaponGem", "Waffen-Edelsteine", "Weapon gems" },
    { "Instrument", "Instrumente", "Instruments" },
};

// ---------------------------------------------------------------------------
// Settings (config.txt)
// ---------------------------------------------------------------------------

enum class Kind { Bool, Int, Float, ItemList };

struct Setting {
    const char* section;
    const char* key;
    const char* label;
    const char* help;
    Kind kind;
    float min, max;
    bool live;              // takes effect at once; otherwise after a game restart
    const char* fallback;   // value when the key is missing in config.txt
    std::string value;
    std::string startValue; // value the game was started with
};

static std::vector<Setting> g_settings = {
    { "Einsammeln", "autoLoot", "Automatisch einsammeln",
      "Alles im Radius wird eingesammelt, ohne dass du E hältst.", Kind::Bool, 0, 0, true, "true" },
    { "Einsammeln", "radiusMeters", "Radius",
      "Reichweite in Metern, für Einsammeln und Zerschlagen. Einsammeln ändert sich sofort, Zerschlagen ab dem nächsten "
      "Laden der Welt (Hauptmenü, Welt laden).",
      Kind::Float, 1, 50, true, "15" },
    { "Einsammeln", "instantLoot", "Sofort einsammeln",
      "Dinge im Radius direkt einsammeln statt sie heranfliegen zu lassen (nötig für Pilze, Glühwürmchen).",
      Kind::Bool, 0, 0, true, "true" },
    { "Einsammeln", "requireLineOfSight", "Nur mit Sichtlinie",
      "Nur einsammeln, was du direkt sehen kannst (Spiel-Standard). Aus = auch hinter Blättern und Wänden.",
      Kind::Bool, 0, 0, true, "false" },
    { "Einsammeln", "collectGroundLoot", "Bodenbeute",
      "Alles, was im Wald wächst oder liegt: Steine, Äste, Holz, Erz, Pilze, Beeren am Boden, "
      "Tränke, Munition, Drops. Aus = bleibt liegen, auch beim Halten von E; einzeln mit E geht weiter.",
      Kind::Bool, 0, 0, true, "true" },
    { "Einsammeln", "collectStones", "Steine",
      "Steine am Boden und Steinhaufen. Aus = bleiben liegen, und Gesteinsbrocken werden nicht mehr zerschlagen "
      "(das ab dem nächsten Laden der Welt). "
      "Feuerstein zählt nicht dazu. Ist Bodenbeute aus, sind auch Steine aus.",
      Kind::Bool, 0, 0, true, "true" },
    { "Einsammeln", "collectCollectibles", "Sammelstücke",
      "Fossilien, Urnen, Gnome, Meteoriten, Mosaike, Farbpaletten, Event-Kürbisse usw.",
      Kind::Bool, 0, 0, true, "true" },
    { "Einsammeln", "collectPlants", "Büsche und Pflanzen",
      "Was an lebenden Pflanzen geerntet wird: Beerenbüsche, Ast-Büsche, Kräuter, Pflanzenfasern, Schilf, Glühwürmchen. "
      "Äste, die am Boden liegen, sind Bodenbeute.",
      Kind::Bool, 0, 0, true, "true" },
    { "Einsammeln", "collectFarmPlants", "Reife Beete ernten",
      "Nur erntereife Pflanzen, keine Setzlinge. Gilt auch in deiner Basis.", Kind::Bool, 0, 0, true, "true" },
    { "Einsammeln", "collectFish", "Fische fangen",
      "Fische im Wasser im Radius automatisch einsammeln. Aus = sie bleiben im Wasser.", Kind::Bool, 0, 0, true, "false" },
    { "Einsammeln", "ignoreItems", "Gegenstände ignorieren",
      "Namensteile interner Gegenstandsnamen, durch Komma getrennt (Groß/Klein egal), z. B. stone,twigs. "
      "Passende Gegenstände bleiben am Boden und in Truhen und Leichen; Büsche und Beete mit so einem Gegenstand "
      "werden gar nicht geerntet. Mit E geht es weiter. "
      "=name = nur genau dieser Name. Alle Namen stehen in mods\\auto-loot-plus\\items.txt. Leer = aus.",
      Kind::ItemList, 0, 0, true, "" },
    { "Einsammeln", "ignoreNotice", "Hinweis bei ignorierten Gegenständen",
      "Kurze Einblendung „liegen gelassen: …“, wenn ein ignorierter Gegenstand liegen bleibt (höchstens dreimal je "
      "Gegenstand und Spielsitzung). Aus = keine Einblendung, die Liste wirkt trotzdem.", Kind::Bool, 0, 0, true, "true" },
    { "Plündern", "lootEnemyCorpses", "Gegnerleichen plündern",
      "Auch was Gegner beim Tod fallen lassen: Tierteile, Knochen, Fledermausfell.", Kind::Bool, 0, 0, true, "true" },
    { "Plündern", "lootChests", "Truhen und Behälter leeren",
      "Welt-Truhen, Sarkophage, Schränke, Regale, Müll- und Werkzeughaufen. Questgegenstände bleiben liegen, eigene "
      "Truhen werden nie angefasst.",
      Kind::Bool, 0, 0, true, "true" },
    { "Plündern", "lootBosses", "Bosse und Elite-Gegner plündern",
      "Leichen von Bossen, Minibossen (mit Lebensbalken) und Elite-Gegnern automatisch leeren. Aus = sie bleiben liegen "
      "und du plünderst sie selbst mit E. Bonus-Beute (Juwelen, Runen, Boss-Ausrüstung) kommt seit 1.1.0 vollständig mit "
      "(siehe Wartezeit vor dem Plündern).", Kind::Bool, 0, 0, true, "true" },
    { "Plündern", "lootDelay", "Wartezeit vor dem Plündern",
      "Sekunden, die eine frische Leiche, Truhe oder Pflanze nach dem ersten Erfassen wartet, bevor die Mod sie öffnet. "
      "Das Spiel erzeugt Sonderbeute (Ektoplasma in den Hohlen Hallen, Juwelen und Runen von Elite-Gegnern, Boss- und "
      "Questgegenstände) erst beim Öffnen; zu früh geöffnet ging sie verloren. 0 = sofort (wie 1.0.3, nicht empfohlen). "
      "Geöffnet und geleert wird nie im selben Bild.", Kind::Float, 0, 5, true, "1" },
    { "Plündern", "keepQuestItems", "Questgegenstände liegen lassen",
      "Gegenstände aus mods\\auto-loot-plus\\quest_items.txt bleiben in Truhen und Leichen, du holst sie selbst mit E. "
      "Aus = sie werden mit eingesammelt.", Kind::Bool, 0, 0, true, "true" },
    { "Gegner", "pauseNearEnemies", "Pause bei Gegnern in der Nähe",
      "Solange ein Gegner näher als der Gegner-Radius ist, wird nichts automatisch eingesammelt (z. B. keine "
      "Nebelkerne mitten im Kampf). Zwei Sekunden nach dem letzten Gegner geht es weiter.",
      Kind::Bool, 0, 0, true, "false" },
    { "Gegner", "enemyRadius", "Gegner-Radius",
      "Abstand in Metern, ab dem ein Gegner als „in der Nähe“ zählt.", Kind::Float, 1, 50, true, "10" },
    { "Radius je Gruppe", "radiusGround", "Bodenbeute",
      "Eigener Radius für diese Gruppe in Metern, auch größer als der allgemeine Radius. 0 = der allgemeine Radius.", Kind::Float, 0, 50, true, "0" },
    { "Radius je Gruppe", "radiusCollectibles", "Sammelstücke",
      "Eigener Radius für diese Gruppe in Metern, auch größer als der allgemeine Radius. 0 = der allgemeine Radius.", Kind::Float, 0, 50, true, "0" },
    { "Radius je Gruppe", "radiusPlants", "Büsche und Pflanzen",
      "Eigener Radius für diese Gruppe in Metern, auch größer als der allgemeine Radius. 0 = der allgemeine Radius.", Kind::Float, 0, 50, true, "0" },
    { "Radius je Gruppe", "radiusFarm", "Beete",
      "Eigener Radius für diese Gruppe in Metern, auch größer als der allgemeine Radius. 0 = der allgemeine Radius.", Kind::Float, 0, 50, true, "0" },
    { "Radius je Gruppe", "radiusCorpses", "Gegnerleichen",
      "Eigener Radius für diese Gruppe in Metern, auch größer als der allgemeine Radius. 0 = der allgemeine Radius.", Kind::Float, 0, 50, true, "0" },
    { "Radius je Gruppe", "radiusChests", "Truhen und Behälter",
      "Eigener Radius für diese Gruppe in Metern, auch größer als der allgemeine Radius. 0 = der allgemeine Radius.", Kind::Float, 0, 50, true, "0" },
    { "Zerschlagen", "autoSmash", "Automatisch zerschlagen",
      "Fässer, Kisten, Körbe, Vasen, Urnen und kaputte Möbel in Ruinen im Radius. In deiner Basis nie. "
      "Gilt ab dem nächsten Laden der Welt.",
      Kind::Bool, 0, 0, true, "true" },
    { "Zerschlagen", "smashBuildableAwayFromBase", "Auch baubare Möbel zerschlagen",
      "Fässer, Stoffballen, Tische usw., die man auch selbst bauen kann – nur wenn das Spiel dich nicht nahe einem "
      "Baubereich sieht (Puffer um deine Basis). "
      "Gilt ab dem nächsten Laden der Welt.",
      Kind::Bool, 0, 0, true, "true" },
    { "Zerschlagen", "smashTrees", "Bäume fällen (experimentell)",
      "EXPERIMENTELL: Alle Bäume, Baumstümpfe, umgestürzten Stämme und das Totholz im Radius fallen sofort, auch "
      "gleich beim Verlassen der Basis; Holz, Harz und Äste sammelt die Mod ein. Gefällte Bäume wachsen vielleicht nicht "
      "nach – erst in einer Testwelt ausprobieren oder den Spielstand sichern. In der Basis und nahe einem Baubereich nie, "
      "Ernte-Bäume (Haselnuss, Kaffee) nie. Bienenstöcke an Bäumen fallen mit (evtl. Wespen). Etwas weniger Beute als "
      "von Hand. Eine eigene Taste ist geplant. Braucht \"Automatisch zerschlagen\". Gilt nach einem Neustart des Spiels.",
      Kind::Bool, 0, 0, false, "false" },
    { "Zerschlagen", "smashBeehives", "Bienenstöcke zerschlagen",
      "Wilde Bienenstöcke im Radius zerschlagen, den Honig sammelt die Mod ein. Achtung: Aus manchen Stöcken kommen Wespen. "
      "In der Basis nie. Braucht \"Automatisch zerschlagen\". Gilt nach einem Neustart des Spiels.",
      Kind::Bool, 0, 0, false, "false" },
    { "Inventar", "stackSize", "Stapelgröße",
      "Maximale Stapelgröße für stapelbare Gegenstände. 0 = Spiel-Standard. Verkleinern: Stapel über der neuen Größe "
      "bleiben, bis sie geteilt werden; beim nächsten Laden des Spielstands werden sie gekürzt (wie beim Entfernen der Mod).",
      Kind::Int, 0, 65535, true, "9999" },
    { "Inventar", "cheatBestBackpack", "Bester Rucksack für 1 Zutat",
      "Schummeln: Jedes Rucksack-Rezept liefert den besten Rucksack und kostet je Zutat nur 1 Stück.",
      Kind::Bool, 0, 0, false, "false" },
    { "Mod", "enabled", "Mod aktiv", "Schaltet die ganze Mod an oder aus.", Kind::Bool, 0, 0, false, "true" },
};

// English texts (config.txt "language=auto|de|en"; auto = Windows display language)
struct EnglishText { const char* key; const char* section; const char* label; const char* help; };
static const EnglishText kEnglish[] = {
    { "autoLoot", "Looting", "Auto loot", "Everything in the radius is picked up without holding E." },
    { "radiusMeters", "Looting", "Radius",
      "Range in meters, for looting and smashing. Looting changes at once, smashing the next time the world is loaded "
      "(main menu, load the world)." },
    { "collectGroundLoot", "Looting", "Ground loot",
      "Everything that grows or lies in the wild: stones, branches, wood, ore, mushrooms, berries on the ground, "
      "potions, ammo, drops. Off = stays there, also when holding E; picking up one by one with E still works." },
    { "collectStones", "Looting", "Stones",
      "Stones on the ground and stone piles. Off = they stay there, and rock boulders are no longer smashed "
      "(that part from the next time the world is loaded). "
      "Flint is not included. With ground loot off, stones are off too." },
    { "instantLoot", "Looting", "Instant loot",
      "Loot things in the radius directly instead of pulling them in (needed for mushrooms, fireflies)." },
    { "requireLineOfSight", "Looting", "Line of sight only",
      "Only loot what you can see directly (game default). Off = also behind leaves and walls." },
    { "collectCollectibles", "Looting", "Collectibles",
      "Fossils, urns, gnomes, meteorites, mosaics, color palettes, event pumpkins and more." },
    { "collectPlants", "Looting", "Bushes and plants", "Harvested from living plants: berry bushes, twig bushes, herbs, plant fiber, reeds, fireflies. Branches lying on the ground are ground loot." },
    { "collectFarmPlants", "Looting", "Harvest ripe crops", "Only ripe plants, no seedlings. Also inside your base." },
    { "collectFish", "Looting", "Catch fish", "Pick up fish in the water within the radius automatically. Off = they stay in the water." },
    { "ignoreItems", "Looting", "Ignore items",
      "Parts of internal item names, comma separated (case does not matter), e.g. stone,twigs. Matching items stay on the "
      "ground and in chests and corpses; bushes and beds that yield one are not harvested at all. "
      "Picking them up with E still works. =name = exactly this name. "
      "All names are listed in mods\\auto-loot-plus\\items.txt. Empty = off." },
    { "ignoreNotice", "Looting", "Notice for ignored items",
      "Short notice \"left behind: ...\" when an ignored item stays behind (at most three times per item and game "
      "session). Off = no notice, the list still works." },
    { "lootEnemyCorpses", "Plundering", "Loot enemy corpses", "Also what enemies drop on death: critter parts, bones, bat fur." },
    { "lootBosses", "Plundering", "Loot bosses and elites",
      "Empty the corpses of bosses, mini bosses (with a health bar) and elite enemies automatically. Off = they stay and you "
      "loot them with E. Since 1.1.0 bonus loot (jewels, runes, boss equipment) comes along completely (see Delay before looting)." },
    { "lootDelay", "Plundering", "Delay before looting",
      "Seconds a fresh corpse, chest or plant waits after it was first seen before the mod opens it. The game generates "
      "special loot (ectoplasm in the Hollow Halls, jewels and runes of elites, boss and quest items) only when it is opened; "
      "opened too early, that loot was lost. 0 = at once (as in 1.0.3, not recommended). Opening and emptying never "
      "happen in the same frame." },
    { "keepQuestItems", "Plundering", "Leave quest items",
      "Items listed in mods\\auto-loot-plus\\quest_items.txt stay in chests and corpses for you to take with E. "
      "Off = they are collected too." },
    { "pauseNearEnemies", "Enemies", "Pause while enemies are near",
      "Nothing is picked up automatically while an enemy is closer than the enemy radius (no shroud cores in the middle of a fight). "
      "Two seconds after the last enemy it continues." },
    { "enemyRadius", "Enemies", "Enemy radius", "Distance in meters at which an enemy counts as near." },
    { "radiusGround", "Radius per group", "Ground loot", "Own radius for this group in meters, may also be larger than the general radius. 0 = the general radius." },
    { "radiusCollectibles", "Radius per group", "Collectibles", "Own radius for this group in meters, may also be larger than the general radius. 0 = the general radius." },
    { "radiusPlants", "Radius per group", "Bushes and plants", "Own radius for this group in meters, may also be larger than the general radius. 0 = the general radius." },
    { "radiusFarm", "Radius per group", "Crops", "Own radius for this group in meters, may also be larger than the general radius. 0 = the general radius." },
    { "radiusCorpses", "Radius per group", "Enemy corpses", "Own radius for this group in meters, may also be larger than the general radius. 0 = the general radius." },
    { "radiusChests", "Radius per group", "Chests and containers", "Own radius for this group in meters, may also be larger than the general radius. 0 = the general radius." },
    { "lootChests", "Plundering", "Empty chests and containers",
      "World chests, sarcophagi, cupboards, shelves, trash and tool heaps. Quest items stay, your own chests are never touched." },
    { "autoSmash", "Smashing", "Auto smash",
      "Barrels, crates, baskets, vases, urns and broken furniture in ruins within the radius. Never in your base. "
      "Takes effect the next time the world is loaded." },
    { "smashBuildableAwayFromBase", "Smashing", "Also smash buildable furniture",
      "Barrels, cloth bales, tables etc. that can also be built - only when the game does not see you near a "
      "build zone (a buffer around your base). "
      "Takes effect the next time the world is loaded." },
    { "smashTrees", "Smashing", "Fell trees (experimental)",
      "EXPERIMENTAL: all trees, stumps, fallen trunks and deadwood within the radius fall at once, also right when you "
      "leave your base; the mod picks up wood, resin and twigs. Felled trees may not grow back - try it in a test world "
      "or back up your save first. Never inside a base or near a build zone, never harvest trees (hazelnut, coffee). "
      "Bee hives on trees fall too (wasps may come out). A bit less loot than chopping by hand. A hotkey is planned. "
      "Needs \"Auto smash\". Takes effect after restarting the game." },
    { "smashBeehives", "Smashing", "Smash bee hives",
      "Smash wild bee hives within the radius, the mod picks up the honey. Careful: wasps come out of some hives. "
      "Never in your base. Needs \"Auto smash\". Takes effect after restarting the game." },
    { "stackSize", "Inventory", "Stack size",
      "Maximum stack size for stackable items. 0 = game default. Lowering it: stacks above the new size stay until you "
      "split them; the next time the save is loaded they are cut down (as when the mod is removed)." },
    { "cheatBestBackpack", "Inventory", "Best backpack for 1 ingredient",
      "Cheat: every backpack recipe gives the best backpack and costs only 1 of each ingredient." },
    { "enabled", "Mod", "Mod enabled", "Turns the whole mod on or off." },
};
static bool g_english = false;

// Keys of the menu and of the auto loot toggle (config: menuKey, autoLootKey). Other mods use the same
// function keys - ShroudForge opens its loader window with F9, a world editor uses F2-F8 (bonsaibauer,
// 23.10.2026) - so both can be changed; autoLootKey=none switches the toggle off.
static int g_menuVk = VK_F8, g_toggleVk = VK_F9;

static int KeyFromName(std::string name, int fallback)
{
    for (auto& c : name) c = (char)toupper((unsigned char)c);
    if (name.empty()) return fallback;
    if (name == "NONE" || name == "OFF" || name == "0") return 0;
    if (name[0] == 'F' && name.size() <= 3) {
        int n = atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    static const struct { const char* name; int vk; } kNamed[] = {
        { "INSERT", VK_INSERT }, { "HOME", VK_HOME }, { "END", VK_END }, { "PAGEUP", VK_PRIOR }, { "PAGEDOWN", VK_NEXT },
        { "PAUSE", VK_PAUSE }, { "SCROLLLOCK", VK_SCROLL }, { "DELETE", VK_DELETE },
    };
    for (auto& k : kNamed) if (name == k.name) return k.vk;
    if (name.size() == 7 && name.compare(0, 6, "NUMPAD") == 0 && name[6] >= '0' && name[6] <= '9') return VK_NUMPAD0 + (name[6] - '0');
    return fallback;
}

static std::string KeyName(int vk)
{
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Numpad" + std::to_string(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_INSERT: return "Insert"; case VK_HOME: return "Home"; case VK_END: return "End"; case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown"; case VK_PAUSE: return "Pause"; case VK_SCROLL: return "ScrollLock"; case VK_DELETE: return "Delete";
    }
    return "?";
}

// Installed features, not proof of a running loader. EML can be chain-loaded under another name;
// requiring dbghelp.dll hides full-version settings even when that installation works.
static bool g_lite = false;
static const char* const kDllKeys[] = { "autoLoot", "radiusMeters", "instantLoot", "requireLineOfSight", "ignoreItems", "ignoreNotice",
                                        "collectGroundLoot", "collectStones", "collectCollectibles", "collectFish", "radiusGround", "radiusCollectibles",
                                        "pauseNearEnemies", "enemyRadius", "stackSize", "enabled" };

static bool IsDllSetting(const Setting& st)
{
    for (const char* k : kDllKeys) if (strcmp(k, st.key) == 0) return true;
    return false;
}

static bool FileExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void RefreshInstalledFeatures(bool forceLog = false)
{
    bool lite = !FileExists(GameDirectory() + L"\\mods\\auto-loot-plus\\src\\mod.lua");
    if (forceLog || lite != g_lite)
        Log("Menü: %s (Lua-Datei %s; Loader-Aktivitaet hier nicht geprueft)",
            lite ? "Lite-Einstellungen" : "Vollversion-Einstellungen", lite ? "fehlt" : "vorhanden");
    g_lite = lite;
}

static const EnglishText* EnglishFor(const Setting& st)
{
    if (!g_english) return nullptr;
    for (auto& e : kEnglish) if (strcmp(e.key, st.key) == 0) return &e;
    return nullptr;
}
static const char* SectionText(const Setting& st) { auto e = EnglishFor(st); return e ? e->section : st.section; }
static const char* LabelText(const Setting& st) { auto e = EnglishFor(st); return e ? e->label : st.label; }
static const char* HelpText(const Setting& st) { auto e = EnglishFor(st); return e ? e->help : st.help; }
static const char* T(const char* de, const char* en) { return g_english ? en : de; }

static std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static bool IsTrue(const std::string& v)
{
    std::string s;
    for (char c : v) s += (char)tolower((unsigned char)c);
    return s == "true" || s == "1" || s == "yes" || s == "on";
}

static std::wstring ConfigPath() { return GameDirectory() + L"\\mods\\auto-loot-plus\\config.txt"; }

static bool ReadLines(std::vector<std::string>& lines)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, ConfigPath().c_str(), L"rb") != 0 || !f) return false;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    fclose(f);
    lines.clear();
    size_t start = 0;
    while (start <= all.size()) {
        size_t end = all.find('\n', start);
        if (end == std::string::npos) {
            if (start < all.size()) lines.push_back(all.substr(start));
            break;
        }
        lines.push_back(all.substr(start, end - start));   // keeps a trailing '\r'
        start = end + 1;
    }
    return true;
}

// Returns the key of a "key=value" line (empty for comments and other lines)
static std::string LineKey(const std::string& line, std::string* value)
{
    std::string s = Trim(line);
    if (s.empty() || s[0] == '#' || s[0] == ';') return std::string();
    size_t eq = s.find('=');
    if (eq == std::string::npos) return std::string();
    std::string key = Trim(s.substr(0, eq));
    if (value) {
        std::string v = Trim(s.substr(eq + 1));
        // ignoreItems: "#Group" entries start with '#', no end-of-line comment there
        size_t comment = key == "ignoreItems" ? std::string::npos : v.find_first_of("#;");
        if (comment != std::string::npos) v = Trim(v.substr(0, comment));
        *value = v;
    }
    return key;
}

static void LoadSettings()
{
    for (auto& st : g_settings) st.value = st.fallback;
    std::string language = "auto";
    std::vector<std::string> lines;
    if (ReadLines(lines)) {
        for (auto& line : lines) {
            std::string value;
            std::string key = LineKey(line, &value);
            if (key == "language") language = value;
            if (key == "menuKey") { int vk = KeyFromName(value, VK_F8); g_menuVk = vk ? vk : VK_F8; }   // the menu always needs a key
            if (key == "autoLootKey") g_toggleVk = KeyFromName(value, VK_F9);
            for (auto& st : g_settings)
                if (key == st.key) st.value = value;
        }
    }
    if (language == "de") g_english = false;
    else if (language == "en") g_english = true;
    else g_english = PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_GERMAN;
    for (auto& st : g_settings) st.startValue = st.value;
    RefreshInstalledFeatures(true);
}

static void SaveSetting(const Setting& st)
{
    std::vector<std::string> lines;
    if (!ReadLines(lines)) { Log("Menü: config.txt nicht lesbar – nicht gespeichert"); return; }
    bool found = false;
    for (auto& line : lines) {
        if (LineKey(line, nullptr) == st.key) {
            bool cr = !line.empty() && line.back() == '\r';
            line = std::string(st.key) + "=" + st.value + (cr ? "\r" : "");
            found = true;
        }
    }
    if (!found) {
        lines.push_back("");
        lines.push_back(std::string(st.key) + "=" + st.value);
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, ConfigPath().c_str(), L"wb") != 0 || !f) { Log("Menü: config.txt nicht schreibbar"); return; }
    for (size_t i = 0; i < lines.size(); ++i) {
        fwrite(lines[i].data(), 1, lines[i].size(), f);
        if (i + 1 < lines.size()) fputc('\n', f);
    }
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Hook state
// ---------------------------------------------------------------------------

static std::mutex g_lock;
static std::atomic<bool> g_menuOpen{ false };
bool Overlay_MenuOpen() { return g_menuOpen; }   // main.cpp: magnet pauses while the menu is open
static std::atomic<int> g_wheel{ 0 };

// Mouse clicks from window messages. The menu is drawn once per frame; a click that starts and ends
// between two frames (low FPS, frame generation) would be lost if the buttons were only polled.
struct MouseClick { int button; bool down; };
static std::mutex g_clickLock;
static std::vector<MouseClick> g_clicks;
// Only one source per click, otherwise a click could arrive twice and toggle a checkbox back:
// raw input if the game uses it, else the classic button messages. Polling the buttons (as in 1.0.0)
// is only the fallback while no mouse message at all has reached the window; moving the mouse to a
// checkbox already decides this before the first click.
static std::atomic<bool> g_rawMouse{ false };
static std::atomic<bool> g_legacyMouse{ false };

static void QueueClick(int button, bool down)
{
    std::lock_guard<std::mutex> guard(g_clickLock);
    if (g_clicks.size() < 64) g_clicks.push_back({ button, down });
}

// Typed characters (WM_CHAR) for the text field of the stack size
static std::vector<unsigned int> g_chars;

static void QueueChar(unsigned int c)
{
    std::lock_guard<std::mutex> guard(g_clickLock);
    if (c >= 32 && g_chars.size() < 64) g_chars.push_back(c);
}
static bool g_broken = false;          // a failure disables the overlay for this session
static HWND g_hwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static VkInstance g_instance = VK_NULL_HANDLE;

struct DeviceInfo { VkDevice device; VkPhysicalDevice physical; };
struct QueueInfo { VkQueue queue; uint32_t family; };
static std::vector<DeviceInfo> g_devices;
static std::vector<QueueInfo> g_queues;

#define REAL(fn) static PFN_##fn g_real_##fn = nullptr
REAL(vkGetInstanceProcAddr);
REAL(vkGetDeviceProcAddr);
REAL(vkCreateInstance);
REAL(vkCreateDevice);
REAL(vkDestroyDevice);
REAL(vkGetDeviceQueue);
REAL(vkGetDeviceQueue2);
REAL(vkCreateWin32SurfaceKHR);
REAL(vkCreateSwapchainKHR);
REAL(vkDestroySwapchainKHR);
REAL(vkQueuePresentKHR);
#undef REAL

// Device functions for our own rendering
struct DeviceFns {
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
};
static DeviceFns g_d = {};
static VkDevice g_fnDevice = VK_NULL_HANDLE;

struct Frame {
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore done = VK_NULL_HANDLE;
};

struct Swap {
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {};
    uint32_t minImages = 2;
    std::vector<Frame> frames;
    bool built = false;
};
static Swap g_swap;

static VkCommandPool g_pool = VK_NULL_HANDLE;
static uint32_t g_poolFamily = UINT32_MAX;
static VkRenderPass g_renderPass = VK_NULL_HANDLE;
static VkFormat g_renderPassFormat = VK_FORMAT_UNDEFINED;
static bool g_imguiContext = false;
static bool g_imguiVulkan = false;
static bool g_imguiFunctions = false;
static uint32_t g_imguiImageCount = 0;
static float g_fontSize = 18.0f;
// Latin-1 (umlauts, ß) plus general punctuation (– — „ “ … •)
static const ImWchar kGlyphs[] = { 0x0020, 0x00FF, 0x2010, 0x2027, 0 };

static bool LoadDeviceFns(VkDevice device)
{
    if (g_fnDevice == device) return true;
#define LOAD(name) g_d.name = reinterpret_cast<PFN_vk##name>(g_real_vkGetDeviceProcAddr(device, "vk" #name)); \
    if (!g_d.name) { Log("Menü: Vulkan-Funktion vk%s fehlt", #name); return false; }
    LOAD(GetSwapchainImagesKHR) LOAD(CreateImageView) LOAD(DestroyImageView) LOAD(CreateFramebuffer)
    LOAD(DestroyFramebuffer) LOAD(CreateRenderPass) LOAD(DestroyRenderPass) LOAD(CreateCommandPool)
    LOAD(DestroyCommandPool) LOAD(AllocateCommandBuffers) LOAD(FreeCommandBuffers) LOAD(ResetCommandBuffer)
    LOAD(BeginCommandBuffer) LOAD(EndCommandBuffer) LOAD(CmdBeginRenderPass) LOAD(CmdEndRenderPass)
    LOAD(CreateFence) LOAD(DestroyFence) LOAD(WaitForFences) LOAD(ResetFences) LOAD(CreateSemaphore)
    LOAD(DestroySemaphore) LOAD(QueueSubmit) LOAD(DeviceWaitIdle)
#undef LOAD
    g_fnDevice = device;
    return true;
}

// Frees the per-image resources (views, framebuffers, command buffers, sync objects)
static void DestroyFrames()
{
    if (!g_swap.device || g_fnDevice != g_swap.device) { g_swap.frames.clear(); g_swap.built = false; return; }
    VkDevice dev = g_swap.device;
    for (auto& fr : g_swap.frames) {
        if (fr.fence) g_d.WaitForFences(dev, 1, &fr.fence, VK_TRUE, 1000000000ull);
    }
    for (auto& fr : g_swap.frames) {
        if (fr.framebuffer) g_d.DestroyFramebuffer(dev, fr.framebuffer, nullptr);
        if (fr.view) g_d.DestroyImageView(dev, fr.view, nullptr);
        if (fr.cmd) g_d.FreeCommandBuffers(dev, g_pool, 1, &fr.cmd);
        if (fr.fence) g_d.DestroyFence(dev, fr.fence, nullptr);
        if (fr.done) g_d.DestroySemaphore(dev, fr.done, nullptr);
    }
    g_swap.frames.clear();
    g_swap.built = false;
}

static void DestroyAll()
{
    if (g_imguiVulkan) { ImGui_ImplVulkan_Shutdown(); g_imguiVulkan = false; }
    DestroyFrames();
    if (g_fnDevice) {
        if (g_renderPass) g_d.DestroyRenderPass(g_fnDevice, g_renderPass, nullptr);
        if (g_pool) g_d.DestroyCommandPool(g_fnDevice, g_pool, nullptr);
    }
    g_renderPass = VK_NULL_HANDLE;
    g_renderPassFormat = VK_FORMAT_UNDEFINED;
    g_pool = VK_NULL_HANDLE;
    g_poolFamily = UINT32_MAX;
}

// ---------------------------------------------------------------------------
// Vulkan wrappers handed to the game
// ---------------------------------------------------------------------------

static PFN_vkVoidFunction Intercept(const char* name, PFN_vkVoidFunction real);

static PFN_vkVoidFunction VKAPI_CALL Hook_vkGetInstanceProcAddr(VkInstance instance, const char* name)
{
    if (instance) g_instance = instance;
    return Intercept(name, g_real_vkGetInstanceProcAddr(instance, name));
}

static PFN_vkVoidFunction VKAPI_CALL Hook_vkGetDeviceProcAddr(VkDevice device, const char* name)
{
    return Intercept(name, g_real_vkGetDeviceProcAddr(device, name));
}

static VkResult VKAPI_CALL Hook_vkCreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc,
                                                 VkInstance* instance)
{
    VkResult r = g_real_vkCreateInstance(info, alloc, instance);
    if (r == VK_SUCCESS) g_instance = *instance;
    return r;
}

static VkResult VKAPI_CALL Hook_vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                               const VkAllocationCallbacks* alloc, VkDevice* device)
{
    VkResult r = g_real_vkCreateDevice(physical, info, alloc, device);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> guard(g_lock);
        g_devices.push_back({ *device, physical });
    }
    return r;
}

static void VKAPI_CALL Hook_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc)
{
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (device == g_swap.device || device == g_fnDevice) {
            DestroyAll();
            g_swap = Swap();
            g_fnDevice = VK_NULL_HANDLE;
        }
    }
    g_real_vkDestroyDevice(device, alloc);
}

static void VKAPI_CALL Hook_vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue)
{
    g_real_vkGetDeviceQueue(device, family, index, queue);
    if (*queue) {
        std::lock_guard<std::mutex> guard(g_lock);
        g_queues.push_back({ *queue, family });
    }
}

static void VKAPI_CALL Hook_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue)
{
    g_real_vkGetDeviceQueue2(device, info, queue);
    if (*queue) {
        std::lock_guard<std::mutex> guard(g_lock);
        g_queues.push_back({ *queue, info->queueFamilyIndex });
    }
}

static VkResult VKAPI_CALL Hook_vkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* info,
                                                        const VkAllocationCallbacks* alloc, VkSurfaceKHR* surface)
{
    if (info && info->hwnd) g_hwnd = info->hwnd;
    return g_real_vkCreateWin32SurfaceKHR(instance, info, alloc, surface);
}

static VkResult VKAPI_CALL Hook_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                                     const VkAllocationCallbacks* alloc, VkSwapchainKHR* swapchain)
{
    // We render into the swapchain images; color attachment usage is always supported for surfaces.
    VkSwapchainCreateInfoKHR copy = *info;
    copy.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkResult r = g_real_vkCreateSwapchainKHR(device, &copy, alloc, swapchain);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> guard(g_lock);
        DestroyFrames();
        g_swap.device = device;
        g_swap.swapchain = *swapchain;
        g_swap.format = info->imageFormat;
        g_swap.extent = info->imageExtent;
        g_swap.minImages = info->minImageCount < 2 ? 2 : info->minImageCount;
    }
    return r;
}

static void VKAPI_CALL Hook_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                  const VkAllocationCallbacks* alloc)
{
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (swapchain == g_swap.swapchain) {
            DestroyFrames();
            g_swap.swapchain = VK_NULL_HANDLE;
        }
    }
    g_real_vkDestroySwapchainKHR(device, swapchain, alloc);
}

static VkResult VKAPI_CALL Hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info);

static PFN_vkVoidFunction Intercept(const char* name, PFN_vkVoidFunction real)
{
    if (!real || !name) return real;
#define HOOK(fn) if (strcmp(name, #fn) == 0) { g_real_##fn = reinterpret_cast<PFN_##fn>(real); \
                                               return reinterpret_cast<PFN_vkVoidFunction>(&Hook_##fn); }
    HOOK(vkGetInstanceProcAddr)
    HOOK(vkGetDeviceProcAddr)
    HOOK(vkCreateInstance)
    HOOK(vkCreateDevice)
    HOOK(vkDestroyDevice)
    HOOK(vkGetDeviceQueue)
    HOOK(vkGetDeviceQueue2)
    HOOK(vkCreateWin32SurfaceKHR)
    HOOK(vkCreateSwapchainKHR)
    HOOK(vkDestroySwapchainKHR)
    HOOK(vkQueuePresentKHR)
#undef HOOK
    return real;
}

// ---------------------------------------------------------------------------
// Window input
// ---------------------------------------------------------------------------

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
static SetCursorPosFn g_realSetCursorPos = nullptr;
static ClipCursorFn g_realClipCursor = nullptr;

uint64_t g_lastCursorRecenter = 0;   // diagnostics for main.cpp: the game recenters the cursor during play

static BOOL WINAPI Hook_SetCursorPos(int x, int y)
{
    g_lastCursorRecenter = GetTickCount64();
    if (g_menuOpen) return TRUE;   // the game recenters the cursor every frame
    return g_realSetCursorPos(x, y);
}

static BOOL WINAPI Hook_ClipCursor(const RECT* rect)
{
    return g_realClipCursor(g_menuOpen ? nullptr : rect);
}

static FARPROC WINAPI Hook_GetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC p = GetProcAddress(module, name);
    if (!p || IS_INTRESOURCE(name) || name[0] != 'v' || name[1] != 'k') return p;
    return reinterpret_cast<FARPROC>(Intercept(name, reinterpret_cast<PFN_vkVoidFunction>(p)));
}

// While the menu is open the game gets no presses, but every release still reaches it: a key that
// was pressed before the menu opened must not stay "held" in the game (that blocked Esc).
// The menu key (F8, config menuKey) belongs to the menu and never reaches the game.
static LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INPUT) {
        RAWINPUT ri;
        UINT size = sizeof ri;
        bool ok = GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1;
        bool keyboard = ok && ri.header.dwType == RIM_TYPEKEYBOARD;
        bool mouse = ok && ri.header.dwType == RIM_TYPEMOUSE;
        if (keyboard && ri.data.keyboard.VKey == g_menuVk) return DefWindowProcW(hwnd, msg, wp, lp);
        if (g_menuOpen) {
            if (mouse && (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)) g_wheel += (short)ri.data.mouse.usButtonData;
            if (mouse) {
                g_rawMouse = true;
                USHORT b = ri.data.mouse.usButtonFlags;
                if (b & RI_MOUSE_LEFT_BUTTON_DOWN) QueueClick(0, true);
                if (b & RI_MOUSE_LEFT_BUTTON_UP) QueueClick(0, false);
                if (b & RI_MOUSE_RIGHT_BUTTON_DOWN) QueueClick(1, true);
                if (b & RI_MOUSE_RIGHT_BUTTON_UP) QueueClick(1, false);
            }
            const USHORT kUps = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP |
                                RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
            bool release = (keyboard && (ri.data.keyboard.Flags & RI_KEY_BREAK)) ||
                           (mouse && (ri.data.mouse.usButtonFlags & kUps));
            if (!release) return DefWindowProcW(hwnd, msg, wp, lp);   // the game does not see it
        }
        return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
    }
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && (int)wp == g_menuVk) return 0;
    if (g_menuOpen && !g_rawMouse) {
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) g_legacyMouse = true;
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: QueueClick(0, true); break;
        case WM_LBUTTONUP: QueueClick(0, false); break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: QueueClick(1, true); break;
        case WM_RBUTTONUP: QueueClick(1, false); break;
        }
    }
    if (g_menuOpen) {
        switch (msg) {
        case WM_MOUSEWHEEL:
            g_wheel += GET_WHEEL_DELTA_WPARAM(wp);
            return 0;
        case WM_CHAR:
            QueueChar((unsigned int)wp);
            return 0;
        case WM_KEYDOWN:
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
            return 0;
        case WM_SETCURSOR:
            SetCursor(nullptr);   // ImGui draws the cursor
            return TRUE;
        }
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

static void SetMenuOpen(bool open)
{
    if (g_menuOpen == open) return;
    g_menuOpen = open;
    if (open) {
        RefreshInstalledFeatures();
        {
            std::lock_guard<std::mutex> guard(g_clickLock);
            g_clicks.clear();
            g_chars.clear();
        }
        if (g_imguiContext) {   // no button stays "held" from the last time the menu was open
            ImGui::GetIO().AddMouseButtonEvent(0, false);
            ImGui::GetIO().AddMouseButtonEvent(1, false);
        }
        if (g_realClipCursor) g_realClipCursor(nullptr);
        if (g_hwnd) {
            RECT rc;
            GetClientRect(g_hwnd, &rc);
            POINT center = { rc.right / 2, rc.bottom / 2 };
            ClientToScreen(g_hwnd, &center);
            if (g_realSetCursorPos) g_realSetCursorPos(center.x, center.y);
        }
    }
}

static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Short message at the bottom left (e.g. after a hotkey), shown for kToastMs
static std::string g_toastText;
static uint64_t g_toastTick = 0;
static std::mutex g_toastLock;

static void ShowToast(const std::string& text)
{
    std::lock_guard<std::mutex> guard(g_toastLock);
    g_toastText = text;
    g_toastTick = GetTickCount64();
}

// Hotkeys work in every frame, also while the menu is closed. F9 (autoLootKey) toggles auto loot.
static void PollHotkeys()
{
    static bool f8Before = false, f9Before = false, escBefore = false;
    bool foreground = g_hwnd && GetForegroundWindow() == g_hwnd;
    bool f8 = foreground && KeyDown(g_menuVk);
    bool f9 = foreground && g_toggleVk && KeyDown(g_toggleVk);
    bool esc = foreground && KeyDown(VK_ESCAPE);
    if (f8 && !f8Before) SetMenuOpen(!g_menuOpen);
    else if (esc && !escBefore && g_menuOpen) SetMenuOpen(false);
    if (f9 && !f9Before) {
        for (auto& st : g_settings) {
            if (strcmp(st.key, "autoLoot") != 0) continue;
            bool on = !IsTrue(st.value);
            st.value = on ? "true" : "false";
            ApplyLiveSetting(st.key, st.value);
            SaveSetting(st);
            ShowToast(on ? T("Automatisch einsammeln: an", "Auto loot: on") : T("Automatisch einsammeln: aus", "Auto loot: off"));
        }
    }
    f8Before = f8;
    f9Before = f9;
    escBefore = esc;
}

static void FeedImGuiInput()
{
    ImGuiIO& io = ImGui::GetIO();
    bool foreground = GetForegroundWindow() == g_hwnd;

    POINT pt;
    RECT rc;
    if (GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt) && GetClientRect(g_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
        float sx = io.DisplaySize.x / (float)rc.right;
        float sy = io.DisplaySize.y / (float)rc.bottom;
        io.AddMousePosEvent(pt.x * sx, pt.y * sy);
    }
    if (g_rawMouse || g_legacyMouse) {
        // every press and release in order; ImGui spreads several of them over the next frames
        std::lock_guard<std::mutex> guard(g_clickLock);
        for (auto& c : g_clicks) io.AddMouseButtonEvent(c.button, c.down);
        g_clicks.clear();
        for (unsigned int c : g_chars) io.AddInputCharacter(c);
        g_chars.clear();
        if (!foreground) {   // a release outside the game window never arrives
            io.AddMouseButtonEvent(0, false);
            io.AddMouseButtonEvent(1, false);
        }
    } else {
        io.AddMouseButtonEvent(0, foreground && KeyDown(VK_LBUTTON));
        io.AddMouseButtonEvent(1, foreground && KeyDown(VK_RBUTTON));
    }
    int wheel = g_wheel.exchange(0);
    if (wheel) io.AddMouseWheelEvent(0.0f, wheel / (float)WHEEL_DELTA);

    static const struct { int vk; ImGuiKey key; } kKeys[] = {
        { VK_UP, ImGuiKey_UpArrow }, { VK_DOWN, ImGuiKey_DownArrow }, { VK_LEFT, ImGuiKey_LeftArrow },
        { VK_RIGHT, ImGuiKey_RightArrow }, { VK_RETURN, ImGuiKey_Enter }, { VK_SPACE, ImGuiKey_Space },
        { VK_TAB, ImGuiKey_Tab }, { VK_HOME, ImGuiKey_Home }, { VK_END, ImGuiKey_End },
        { VK_PRIOR, ImGuiKey_PageUp }, { VK_NEXT, ImGuiKey_PageDown },
        { VK_BACK, ImGuiKey_Backspace }, { VK_DELETE, ImGuiKey_Delete }, { VK_ESCAPE, ImGuiKey_Escape },
    };
    for (auto& k : kKeys) io.AddKeyEvent(k.key, foreground && KeyDown(k.vk));
    io.AddKeyEvent(ImGuiMod_Ctrl, foreground && KeyDown(VK_CONTROL));
    io.AddKeyEvent(ImGuiMod_Shift, foreground && KeyDown(VK_SHIFT));
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------

static void ApplyStyle(float scale)
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 10.0f;
    s.FrameRounding = 6.0f;
    s.GrabRounding = 6.0f;
    s.WindowPadding = ImVec2(18, 14);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(12, 8);
    s.WindowBorderSize = 1.0f;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.08f, 0.11f, 0.94f);
    c[ImGuiCol_TitleBg] = ImVec4(0.20f, 0.13f, 0.22f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.30f, 0.18f, 0.32f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.80f, 0.65f, 0.35f, 0.45f);
    c[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.18f, 0.24f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.26f, 0.34f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.36f, 0.30f, 0.40f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.78f, 0.40f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.90f, 0.72f, 0.36f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.82f, 0.45f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.36f, 0.24f, 0.38f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.48f, 0.32f, 0.50f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.56f, 0.38f, 0.58f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.80f, 0.65f, 0.35f, 0.35f);
    c[ImGuiCol_Text] = ImVec4(0.95f, 0.92f, 0.88f, 1.0f);
    c[ImGuiCol_NavHighlight] = ImVec4(0.95f, 0.78f, 0.40f, 1.0f);
    s.ScaleAllSizes(scale);
}

// One setting: checkbox, slider or slider + text field. Returns true when a restart is pending for it.
static bool DrawSetting(Setting& st, float fs)
{
    ImGui::PushID(st.key);
    bool changed = false, commit = false;
    if (st.kind == Kind::Bool) {
        bool v = IsTrue(st.value);
        if (ImGui::Checkbox(LabelText(st), &v)) { st.value = v ? "true" : "false"; changed = commit = true; }
    } else if (st.kind == Kind::Int) {
        int v = atoi(st.value.c_str());
        ImGui::SetNextItemWidth(fs * 10.0f);
        if (ImGui::SliderInt("##slider", &v, (int)st.min, (int)st.max, v == 0 ? T("Standard", "Default") : "%d",
                             ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
            st.value = std::to_string(v);
            changed = true;
        }
        commit = ImGui::IsItemDeactivatedAfterEdit();
        // exact value: type it or step with the buttons (max 65535, the game's limit)
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fs * 7.5f);
        int typed = v;
        if (ImGui::InputInt("##input", &typed, 100, 1000)) {
            if (typed < 0) typed = 0;
            if (typed > 65535) typed = 65535;
            st.value = std::to_string(typed);
            changed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
        ImGui::SameLine();
        ImGui::TextUnformatted(LabelText(st));
    } else if (st.kind == Kind::ItemList) {
        // Item list (ignoreItems): a search box with suggestions (lootable items only, from the third
        // letter, scrollable) and below it the chosen entries, each with a remove button. The config
        // value stays a comma separated list; entries chosen here are stored as "=InternalName" (exact).
        ImGui::TextUnformatted(LabelText(st));
        // chosen entries (config value, comma separated)
        std::vector<std::string> entries;
        size_t p = 0;
        while (p <= st.value.size()) {
            size_t c = st.value.find(',', p);
            std::string e = Trim(st.value.substr(p, c == std::string::npos ? std::string::npos : c - p));
            if (!e.empty()) entries.push_back(e);
            p = c == std::string::npos ? st.value.size() + 1 : c + 1;
        }
        auto chosen = [&](const char* internalName) {
            for (auto& e : entries) if (_stricmp(e[0] == '=' ? e.c_str() + 1 : e.c_str(), internalName) == 0) return true;
            return false;
        };
        auto store = [&]() {
            std::string v;
            for (auto& e : entries) v += (v.empty() ? "" : ", ") + e;
            st.value = v;
            changed = commit = true;
        };

        // search box, group filter
        static char s_search[128] = {};
        static int s_group = 0;
        static bool s_showList = false;
        const int groupCount = (int)(sizeof kItemGroups / sizeof kItemGroups[0]);
        ImGui::SetNextItemWidth(fs * 12.0f);
        bool enter = ImGui::InputTextWithHint("##search", T("Name tippen (ab 3 Buchstaben)", "type a name (3 letters or more)"),
                                              s_search, sizeof s_search, ImGuiInputTextFlags_EnterReturnsTrue);
        bool searching = ImGui::IsItemActive();
        if (searching) s_showList = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fs * 10.0f);
        if (ImGui::BeginCombo("##group", g_english ? kItemGroups[s_group].en : kItemGroups[s_group].de)) {
            for (int i = 0; i < groupCount; ++i) {
                if (ImGui::Selectable(g_english ? kItemGroups[i].en : kItemGroups[i].de, i == s_group)) { s_group = i; s_showList = true; }
            }
            ImGui::EndCombo();
        }
        std::string query = Trim(s_search);
        const char* category = kItemGroups[s_group].category;
        std::string groupEntry = category[0] ? std::string("#") + category : "";
        auto groupChosen = [&]() {
            for (auto& e : entries) if (_stricmp(e.c_str(), groupEntry.c_str()) == 0) return true;
            return false;
        };
        if (category[0]) {
            ImGui::SameLine();
            bool whole = groupChosen();
            if (ImGui::SmallButton(whole ? T("Gruppe wieder einsammeln", "Pick the group up again") : T("Ganze Gruppe ignorieren", "Ignore the whole group"))) {
                if (whole) entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const std::string& e) { return _stricmp(e.c_str(), groupEntry.c_str()) == 0; }), entries.end());
                else entries.push_back(groupEntry);
                store();
            }
        }
        if (ItemNamesMissing()) {
            ImGui::PushTextWrapPos(fs * 26.0f);
            ImGui::TextColored(kWarnColor, "%s",
                               T("items.txt fehlt oder ist leer: keine Vorschläge. Entpacke auto-loot-plus-mod-files.zip in den Spielordner.",
                                 "items.txt missing or empty: no suggestions. Unpack auto-loot-plus-mod-files.zip into the game folder."));
            ImGui::PopTextWrapPos();
        }
        static ItemSuggestion hits[1024];
        bool listable = query.size() >= 3 || (category[0] && s_showList);
        size_t n = listable ? SuggestItems(query.c_str(), !g_english, category, hits, 1024) : 0;
        // chosen items stay in the list with a check mark; a click toggles them
        int pick = -1;
        if (enter && n > 0) pick = 0;   // Enter takes the first suggestion
        if (n > 0 && (s_showList || query.size() >= 3)) {
            float rows = (float)(n < 8 ? n : 8);
            ImGui::BeginChild("##suggestions", ImVec2(fs * 26.0f, rows * ImGui::GetTextLineHeightWithSpacing() + fs * 0.5f), true);
            for (size_t i = 0; i < n; ++i) {
                ImGui::PushID((int)hits[i].id);
                bool isChosen = chosen(hits[i].internalName);
                char label[256];
                snprintf(label, sizeof label, "%s %s", isChosen ? "\xE2\x80\xA2" : "  ", hits[i].displayName);   // U+2022 bullet (in the font range)
                if (ImGui::Selectable(label, isChosen, 0, ImVec2(fs * 11.0f, 0))) pick = (int)i;
                ImGui::SameLine();
                ImGui::TextDisabled("%s", hits[i].internalName);
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::TextDisabled(T("%zu Treffer", "%zu matches"), n);
        }
        if (pick >= 0) {
            const char* name = hits[pick].internalName;
            if (chosen(name)) {
                entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const std::string& e) {
                    return _stricmp(e[0] == '=' ? e.c_str() + 1 : e.c_str(), name) == 0; }), entries.end());
            } else {
                entries.push_back(std::string("=") + name);
            }
            store();
            if (!category[0]) { s_search[0] = 0; s_showList = false; }
        }
        if (!searching && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(0)) s_showList = false;

        // chosen list, scrollable from 8 entries on, with "remove all"
        if (entries.empty()) {
            ImGui::TextDisabled("%s", T("keine – alles wird eingesammelt", "none - everything is picked up"));
        } else {
            float rows = (float)(entries.size() < 8 ? entries.size() : 8);
            ImGui::BeginChild("##chosen", ImVec2(fs * 26.0f, rows * ImGui::GetTextLineHeightWithSpacing() + fs * 0.4f), false);
            int remove = -1;
            for (int i = 0; i < (int)entries.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::SmallButton("x")) remove = i;
                ImGui::SameLine();
                const std::string& e = entries[i];
                const char* internal = e[0] == '=' ? e.c_str() + 1 : e.c_str();
                const char* disp = ItemDisplayName(internal, !g_english);
                const ItemGroup* grp = nullptr;
                if (e[0] == '#') for (auto& g : kItemGroups) if (g.category[0] && _stricmp(g.category, e.c_str() + 1) == 0) grp = &g;
                if (grp) {
                    ImGui::Text(T("Alle: %s", "All: %s"), g_english ? grp->en : grp->de);
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", e.c_str());
                } else if (IsUnknownIgnoreEntry(e.c_str())) {
                    ImGui::TextColored(kWarnColor, "%s", e.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", T("(unbekannt - nicht in items.txt)", "(unknown - not in items.txt)"));
                } else if (disp) {
                    ImGui::TextUnformatted(disp);
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", internal);
                } else if (e[0] == '=') {
                    ImGui::TextUnformatted(internal);   // localized exact name is valid too
                } else {
                    ImGui::TextUnformatted(e.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", T("(Namensteil)", "(name part)"));
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (remove >= 0) { entries.erase(entries.begin() + remove); store(); }
            if (ImGui::SmallButton(T("Alle entfernen", "Remove all"))) { entries.clear(); store(); }
            ImGui::SameLine();
            ImGui::TextDisabled(T("%zu ignoriert", "%zu ignored"), entries.size());
        }
    } else {
        float v = (float)atof(st.value.c_str());
        ImGui::SetNextItemWidth(fs * 10.0f);
        bool seconds = strcmp(st.key, "lootDelay") == 0;   // seconds in 0.1 steps, all others whole meters
        if (ImGui::SliderFloat(LabelText(st), &v, st.min, st.max, seconds ? "%.1f s" : "%.0f m",
                               ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
            char buf[32];
            sprintf_s(buf, seconds ? "%.1f" : "%.0f", v);
            st.value = buf;
            changed = true;
        }
        commit = ImGui::IsItemDeactivatedAfterEdit();
    }
    const char* help = HelpText(st);
    if (help && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(fs * 24.0f);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (changed && st.live) ApplyLiveSetting(st.key, st.value);
    if (commit) SaveSetting(st);
    bool pending = !st.live && st.value != st.startValue;
    if (pending) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.30f, 1.0f), "*");
    }
    ImGui::PopID();
    return pending;
}

// Sections side by side, so the menu fits a 16:9 screen without scrolling
static const char* const kColumns[][5] = {
    { "Einsammeln", nullptr, nullptr, nullptr, nullptr },
    { "Plündern", "Zerschlagen", "Gegner", nullptr, nullptr },
    { "Inventar", "Mod", "Radius je Gruppe", nullptr, nullptr },
};

static void DrawMenu()
{
    ImGuiIO& io = ImGui::GetIO();
    float fs = g_fontSize;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    std::string title = std::string("Auto Loot Plus ") + kModVersion + T("  –  Einstellungen###alp", "  –  Settings###alp");
    if (ImGui::Begin(title.c_str(), &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        bool restartPending = false;
        const int columnCount = (int)(sizeof kColumns / sizeof kColumns[0]);
        auto columnHasContent = [&](int col) {
            for (const char* section : kColumns[col]) {
                if (!section) break;
                for (auto& st : g_settings)
                    if (strcmp(st.section, section) == 0 && (!g_lite || IsDllSetting(st))) return true;
            }
            return false;
        };
        int visibleColumns = 0;
        for (int col = 0; col < columnCount; ++col) if (columnHasContent(col)) ++visibleColumns;
        if (visibleColumns > 0 &&
            ImGui::BeginTable("columns", visibleColumns, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
            for (int col = 0; col < columnCount; ++col) {
                if (!columnHasContent(col)) continue;
                ImGui::TableNextColumn();
                ImGui::BeginGroup();
                ImGui::Dummy(ImVec2(fs * 18.0f, 0.0f));   // minimum column width
                for (const char* section : kColumns[col]) {
                    if (!section) break;
                    bool first = true;
                    for (auto& st : g_settings) {
                        if (strcmp(st.section, section) != 0) continue;
                        if (g_lite && !IsDllSetting(st)) continue;
                        if (first) {
                            ImGui::Spacing();
                            ImGui::SeparatorText(SectionText(st));
                            first = false;
                        }
                        if (DrawSetting(st, fs)) restartPending = true;
                    }
                }
                ImGui::EndGroup();
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::Separator();
        if (g_lite)
            ImGui::TextDisabled("%s", T("Lite: Lua-Teil fehlt. Leichen, Truhen, Pflanzen und Zerschlagen brauchen die Vollversion mit aktivem EML.",
                                        "Lite: Lua part missing. Corpses, chests, plants and smashing need the full version with EML running."));
        ImGui::TextDisabled("%s", T("Änderungen werden sofort gespeichert.", "Changes are saved immediately."));
        if (restartPending)
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.30f, 1.0f), "%s",
                               T("* gilt erst nach einem Neustart des Spiels", "* takes effect after restarting the game"));
        ImGui::Spacing();
        std::string closeLabel = std::string(T("Schließen  (", "Close  (")) + KeyName(g_menuVk) + ")###close";
        if (ImGui::Button(closeLabel.c_str())) open = false;
    }
    ImGui::End();
    if (!open) SetMenuOpen(false);
}

// Small hint at the bottom left while an item was just left behind because of the ignore list
// (at most 3 times per item and session, see main.cpp). Drawn with or without the menu.
static const uint64_t kToastMs = 3000;

// Newest of the two message sources: a filtered item (main.cpp) or a hotkey message. Returns the
// lines to draw, empty when nothing is fresh.
static bool CurrentToast(std::string* line1, std::string* line2)
{
    uint64_t now = GetTickCount64();
    uint32_t id = 0; uint64_t itemTick = 0;
    bool item = FilterNotice(&id, &itemTick) && now - itemTick < kToastMs;
    std::string msg; uint64_t msgTick = 0;
    {
        std::lock_guard<std::mutex> guard(g_toastLock);
        if (g_toastTick && now - g_toastTick < kToastMs) { msg = g_toastText; msgTick = g_toastTick; }
    }
    uint64_t fullTick = 0;
    if (InventoryFullNotice(&fullTick) && now - fullTick < kToastMs && fullTick >= msgTick && fullTick >= itemTick) {
        *line1 = T("Inventar voll – Beute bleibt liegen", "inventory full – loot stays behind");
        *line2 = T("(Leichen und Truhen bleiben, bis wieder Platz ist)", "(corpses and chests wait until there is room again)");
        return true;
    }
    if (!msg.empty() && (!item || msgTick >= itemTick)) { *line1 = msg; line2->clear(); return true; }
    if (item) {
        const char* name = ItemDisplayNameById(id, !g_english);
        if (!name) return false;
        *line1 = std::string(T("liegen gelassen: ", "left behind: ")) + name;
        *line2 = std::string(T("(Gegenstände ignorieren, ", "(Ignore items, ")) + KeyName(g_menuVk) + ")";
        return true;
    }
    return false;
}

static bool ToastActive()
{
    std::string a, b;
    return CurrentToast(&a, &b);
}

static void DrawToast()
{
    std::string line1, line2;
    if (!CurrentToast(&line1, &line2)) return;
    ImGuiIO& io = ImGui::GetIO();
    float fs = g_fontSize;
    ImGui::SetNextWindowPos(ImVec2(fs * 1.5f, io.DisplaySize.y - fs * 4.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (ImGui::Begin("##alp_toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.40f, 1.0f), "Auto Loot Plus");
        ImGui::SameLine();
        ImGui::TextUnformatted(line1.c_str());
        if (!line2.empty()) ImGui::TextDisabled("%s", line2.c_str());
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static PFN_vkVoidFunction LoadForImGui(const char* name, void*)
{
    PFN_vkVoidFunction f = g_real_vkGetDeviceProcAddr(g_swap.device, name);
    if (!f) f = g_real_vkGetInstanceProcAddr(g_instance, name);
    return f;
}

static VkPhysicalDevice PhysicalFor(VkDevice device)
{
    for (auto& d : g_devices) if (d.device == device) return d.physical;
    return VK_NULL_HANDLE;
}

static bool FamilyOf(VkQueue queue, uint32_t* family)
{
    for (auto& q : g_queues) if (q.queue == queue) { *family = q.family; return true; }
    return false;
}

static bool HasGraphics(VkPhysicalDevice physical, uint32_t family)
{
    auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        g_real_vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!getProps) return false;
    uint32_t count = 0;
    getProps(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    getProps(physical, &count, props.data());
    return family < count && (props[family].queueFlags & VK_QUEUE_GRAPHICS_BIT);
}

static bool Fail(const char* what)
{
    Log("Menü: %s – Menü für diese Sitzung aus", what);
    g_broken = true;
    return false;
}

static bool EnsureResources(VkQueue queue, uint32_t family)
{
    VkDevice dev = g_swap.device;
    if (!LoadDeviceFns(dev)) return Fail("Vulkan-Funktionen fehlen");

    if (!g_pool || g_poolFamily != family) {
        if (g_pool) { DestroyFrames(); g_d.DestroyCommandPool(dev, g_pool, nullptr); g_pool = VK_NULL_HANDLE; }
        VkCommandPoolCreateInfo ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = family;
        if (g_d.CreateCommandPool(dev, &ci, nullptr, &g_pool) != VK_SUCCESS) return Fail("Command-Pool");
        g_poolFamily = family;
    }

    if (!g_renderPass || g_renderPassFormat != g_swap.format) {
        if (g_imguiVulkan) { ImGui_ImplVulkan_Shutdown(); g_imguiVulkan = false; }
        if (g_renderPass) g_d.DestroyRenderPass(dev, g_renderPass, nullptr);
        VkAttachmentDescription att{};
        att.format = g_swap.format;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;            // keep the game's frame
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo ci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        ci.attachmentCount = 1;
        ci.pAttachments = &att;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        ci.dependencyCount = 1;
        ci.pDependencies = &dep;
        if (g_d.CreateRenderPass(dev, &ci, nullptr, &g_renderPass) != VK_SUCCESS) return Fail("Render-Pass");
        g_renderPassFormat = g_swap.format;
    }

    if (!g_swap.built) {
        uint32_t count = 0;
        if (g_d.GetSwapchainImagesKHR(dev, g_swap.swapchain, &count, nullptr) != VK_SUCCESS || !count)
            return Fail("Swapchain-Bilder");
        std::vector<VkImage> images(count);
        g_d.GetSwapchainImagesKHR(dev, g_swap.swapchain, &count, images.data());
        g_swap.frames.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            Frame& fr = g_swap.frames[i];
            VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vi.image = images[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = g_swap.format;
            vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (g_d.CreateImageView(dev, &vi, nullptr, &fr.view) != VK_SUCCESS) return Fail("Image-View");
            VkFramebufferCreateInfo fi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fi.renderPass = g_renderPass;
            fi.attachmentCount = 1;
            fi.pAttachments = &fr.view;
            fi.width = g_swap.extent.width;
            fi.height = g_swap.extent.height;
            fi.layers = 1;
            if (g_d.CreateFramebuffer(dev, &fi, nullptr, &fr.framebuffer) != VK_SUCCESS) return Fail("Framebuffer");
            VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = g_pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (g_d.AllocateCommandBuffers(dev, &ai, &fr.cmd) != VK_SUCCESS) return Fail("Command-Buffer");
            VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (g_d.CreateFence(dev, &fci, nullptr, &fr.fence) != VK_SUCCESS) return Fail("Fence");
            VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (g_d.CreateSemaphore(dev, &sci, nullptr, &fr.done) != VK_SUCCESS) return Fail("Semaphore");
        }
        g_swap.built = true;
    }

    if (!g_imguiContext) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.MouseDrawCursor = true;
        g_fontSize = (float)g_swap.extent.height / 50.0f;
        if (g_fontSize < 16.0f) g_fontSize = 16.0f;
        if (g_fontSize > 56.0f) g_fontSize = 56.0f;
        wchar_t winDir[MAX_PATH];
        GetWindowsDirectoryW(winDir, MAX_PATH);
        char font[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, (std::wstring(winDir) + L"\\Fonts\\segoeui.ttf").c_str(), -1, font, sizeof font, nullptr, nullptr);
        if (GetFileAttributesA(font) == INVALID_FILE_ATTRIBUTES || !io.Fonts->AddFontFromFileTTF(font, g_fontSize, nullptr, kGlyphs)) {
            ImFontConfig cfg;
            cfg.SizePixels = g_fontSize;
            io.Fonts->AddFontDefault(&cfg);
        }
        ApplyStyle(g_fontSize / 18.0f);
        g_imguiContext = true;
    }

    uint32_t imageCount = (uint32_t)g_swap.frames.size();
    if (g_imguiVulkan && g_imguiImageCount != imageCount) { ImGui_ImplVulkan_Shutdown(); g_imguiVulkan = false; }
    if (!g_imguiVulkan) {
        if (!g_imguiFunctions) {
            if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, &LoadForImGui, nullptr)) return Fail("ImGui-Vulkan-Funktionen");
            g_imguiFunctions = true;
        }
        ImGui_ImplVulkan_InitInfo ii = {};
        ii.ApiVersion = VK_API_VERSION_1_0;
        ii.Instance = g_instance;
        ii.PhysicalDevice = PhysicalFor(dev);
        ii.Device = dev;
        ii.QueueFamily = family;
        ii.Queue = queue;
        ii.RenderPass = g_renderPass;
        ii.MinImageCount = g_swap.minImages <= imageCount ? g_swap.minImages : imageCount;
        if (ii.MinImageCount < 2) ii.MinImageCount = 2;
        ii.ImageCount = imageCount < ii.MinImageCount ? ii.MinImageCount : imageCount;
        ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE + 4;
        if (!ii.PhysicalDevice) return Fail("Grafikkarte nicht zugeordnet");
        if (!ImGui_ImplVulkan_Init(&ii)) return Fail("ImGui-Vulkan-Start");
        g_imguiVulkan = true;
        g_imguiImageCount = imageCount;
        Log("Menü bereit (%ux%u, Format %d, %u Bilder)", g_swap.extent.width, g_swap.extent.height, (int)g_swap.format, imageCount);
    }
    return true;
}

// Records and submits the menu for one swapchain image. Returns the semaphore the present must
// wait on, or VK_NULL_HANDLE when nothing was drawn.
static VkSemaphore RenderMenu(VkQueue queue, uint32_t imageIndex, uint32_t waitCount, const VkSemaphore* waits)
{
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_real_vkGetInstanceProcAddr || !g_instance) { Fail("Vulkan-Instanz unbekannt"); return VK_NULL_HANDLE; }
    if (!g_real_vkGetDeviceProcAddr)
        g_real_vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(g_real_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr"));
    if (!g_real_vkGetDeviceProcAddr) { Fail("vkGetDeviceProcAddr fehlt"); return VK_NULL_HANDLE; }
    uint32_t family = 0;
    if (!FamilyOf(queue, &family)) { Fail("Present-Queue unbekannt"); return VK_NULL_HANDLE; }
    if (!HasGraphics(PhysicalFor(g_swap.device), family)) { Fail("Present-Queue kann nicht zeichnen"); return VK_NULL_HANDLE; }
    if (!EnsureResources(queue, family)) return VK_NULL_HANDLE;
    if (imageIndex >= g_swap.frames.size()) return VK_NULL_HANDLE;
    VkDevice dev = g_swap.device;
    Frame& fr = g_swap.frames[imageIndex];
    if (g_d.WaitForFences(dev, 1, &fr.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) return VK_NULL_HANDLE;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_swap.extent.width, (float)g_swap.extent.height);
    static LARGE_INTEGER freq = {}, last = {};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    io.DeltaTime = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart : 1.0f / 60.0f;
    if (io.DeltaTime <= 0.0f || io.DeltaTime > 1.0f) io.DeltaTime = 1.0f / 60.0f;
    last = now;

    io.MouseDrawCursor = g_menuOpen;   // no cursor while only the hint is drawn
    if (g_menuOpen) FeedImGuiInput();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (g_menuOpen) DrawMenu();
    DrawToast();
    ImGui::Render();

    g_d.ResetFences(dev, 1, &fr.fence);
    g_d.ResetCommandBuffer(fr.cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_d.BeginCommandBuffer(fr.cmd, &bi);
    VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = g_renderPass;
    rp.framebuffer = fr.framebuffer;
    rp.renderArea.extent = g_swap.extent;
    g_d.CmdBeginRenderPass(fr.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), fr.cmd);
    g_d.CmdEndRenderPass(fr.cmd);
    g_d.EndCommandBuffer(fr.cmd);

    std::vector<VkPipelineStageFlags> stages(waitCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = waitCount;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fr.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &fr.done;
    if (g_d.QueueSubmit(queue, 1, &si, fr.fence) != VK_SUCCESS) {
        // The fence stays unsignaled; recreate it so the next wait does not stall
        g_d.DestroyFence(dev, fr.fence, nullptr);
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        g_d.CreateFence(dev, &fci, nullptr, &fr.fence);
        Fail("Zeichnen abgelehnt");
        return VK_NULL_HANDLE;
    }
    return fr.done;
}

static VkSemaphore RenderMenuGuarded(VkQueue queue, uint32_t imageIndex, uint32_t waitCount, const VkSemaphore* waits)
{
    __try {
        return RenderMenu(queue, imageIndex, waitCount, waits);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_broken = true;
        Log("Menü: Absturz beim Zeichnen abgefangen (Code %08X) – Menü für diese Sitzung aus", GetExceptionCode());
        return VK_NULL_HANDLE;
    }
}

static VkResult VKAPI_CALL Hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info)
{
    static bool logged = false;
    if (!logged) { logged = true; Log("Menü: Spielbild erkannt, %s öffnet die Einstellungen, Auto-Loot-Taste %s", KeyName(g_menuVk).c_str(),
                                      g_toggleVk ? KeyName(g_toggleVk).c_str() : "aus"); }

    if (!g_hwnd) {   // surface created without our wrapper: take the game's foreground window
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) g_hwnd = fg;
    }

    if (g_hwnd && !g_origWndProc) {
        g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuWndProc)));
        if (!g_origWndProc) Log("Menü: Fenster nicht übernommen (%lu)", GetLastError());
    }
    if (g_broken || !g_origWndProc) return g_real_vkQueuePresentKHR(queue, info);

    PollHotkeys();
    if ((!g_menuOpen && !ToastActive()) || info->swapchainCount < 1 || info->pSwapchains[0] != g_swap.swapchain)
        return g_real_vkQueuePresentKHR(queue, info);

    VkSemaphore done = RenderMenuGuarded(queue, info->pImageIndices[0], info->waitSemaphoreCount, info->pWaitSemaphores);
    if (!done) return g_real_vkQueuePresentKHR(queue, info);
    VkPresentInfoKHR copy = *info;
    copy.waitSemaphoreCount = 1;
    copy.pWaitSemaphores = &done;
    return g_real_vkQueuePresentKHR(queue, &copy);
}

// ---------------------------------------------------------------------------
// Install: redirect imports of enshrouded.exe (runs in DllMain, before the game starts Vulkan)
// ---------------------------------------------------------------------------

static bool PatchImport(HMODULE module, void* original, void* replacement)
{
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !original) return false;
    bool done = false;
    for (auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        for (auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk); thunk->u1.Function; ++thunk) {
            if (reinterpret_cast<void*>(thunk->u1.Function) != original) continue;
            DWORD old;
            if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                thunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
                VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
                done = true;
            }
        }
    }
    return done;
}

void Overlay_Install()
{
    LoadSettings();
    HMODULE game = GetModuleHandleW(nullptr);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    HMODULE user = GetModuleHandleW(L"user32.dll");
    void* gpa = kernel ? reinterpret_cast<void*>(GetProcAddress(kernel, "GetProcAddress")) : nullptr;
    g_realSetCursorPos = user ? reinterpret_cast<SetCursorPosFn>(GetProcAddress(user, "SetCursorPos")) : nullptr;
    g_realClipCursor = user ? reinterpret_cast<ClipCursorFn>(GetProcAddress(user, "ClipCursor")) : nullptr;
    bool okGpa = PatchImport(game, gpa, reinterpret_cast<void*>(&Hook_GetProcAddress));
    bool okPos = PatchImport(game, reinterpret_cast<void*>(g_realSetCursorPos), reinterpret_cast<void*>(&Hook_SetCursorPos));
    bool okClip = PatchImport(game, reinterpret_cast<void*>(g_realClipCursor), reinterpret_cast<void*>(&Hook_ClipCursor));
    Log("Menü: Importe umgeleitet GetProcAddress=%d SetCursorPos=%d ClipCursor=%d", okGpa, okPos, okClip);
}

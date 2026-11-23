// SPDX-License-Identifier: MIT
// Generated from the game's crafting menu configuration (FbUiBundle.menu.crafting.recipes) and its English texts, client 26430625
// (exe sha256 5d9ef2f140b8ed49...): one constant per tab of each workshop menu, the tab's first list. Do not edit by hand.
namespace Kiln;

/// <summary>The tabs of the game's crafting menus, as <see cref="CraftingGroupRef"/> constants: <c>CraftingGroups.Carpenter.Essentials</c>. Each puts a recipe in
/// the tab's first list (the one without a header). A subcategory of its own: <c>new CraftingGroupRef(workshop, group, set)</c>. Since 0.2.0-dev.30.5 (mod API 0.2.14).</summary>
public static class CraftingGroups
{
    /// <summary>The player's own crafting menu: workshop 0x00000000.</summary>
    public static class Hand
    {
        /// <summary>"Essentials".</summary>
        public const string Essentials = "0x00000000/8c91ca52-be38-468c-ae74-bc6a5ffab28e";
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0x00000000/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
        /// <summary>"Water utilities".</summary>
        public const string WaterUtilities = "0x00000000/1924563b-8056-4b8e-9317-9d848d084b3b";
        /// <summary>"Survival".</summary>
        public const string Survival = "0x00000000/65e6c94a-31c7-4c1f-ba8d-feb85bb87d42";
        /// <summary>"Weapons".</summary>
        public const string Weapons = "0x00000000/2632c31c-37fe-48f6-b788-dfa649065edb";
        /// <summary>"Armor".</summary>
        public const string Armor = "0x00000000/b50bb867-6d23-4d69-8c51-9e61212e2aec";
    }

    /// <summary>Grinding Stones (Prop_Production_T1_HandGrinder): workshop 0x056d94f8.</summary>
    public static class GrindingStones
    {
        /// <summary>"Terrain Material".</summary>
        public const string TerrainMaterial = "0x056d94f8/6c66ad67-1cc5-45f2-badc-f94971b1f120";
        /// <summary>"Resources".</summary>
        public const string Resources = "0x056d94f8/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Kitchen (Craftingstation_Farmer_Kitchen): workshop 0x07ce61f0.</summary>
    public static class Kitchen
    {
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0x07ce61f0/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
    }

    /// <summary>Ectoplasm Press (Prop_NPC_T2_cryptKeeper_EctoplasmPress): workshop 0x1abf2d27.</summary>
    public static class EctoplasmPress
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0x1abf2d27/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Table Saw (Prop_Production_T2_Table_Saw): workshop 0x1abf8143.</summary>
    public static class TableSaw
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0x1abf8143/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Oven (Prop_Production_T3_Oven): workshop 0x1ee9f0bf.</summary>
    public static class Oven
    {
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0x1ee9f0bf/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
        /// <summary>"Resources".</summary>
        public const string Resources = "0x1ee9f0bf/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Laboratory (Prop_Production_T4_Laboratory): workshop 0x20b86c20.</summary>
    public static class Laboratory
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0x20b86c20/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Charcoal Kiln (Prop_Production_T1_Charcoal_Kiln): workshop 0x2546ae49.</summary>
    public static class CharcoalKiln
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0x2546ae49/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Beehive (Prop_Production_T4_Beehive): workshop 0x269a27a7.</summary>
    public static class Beehive
    {
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0x269a27a7/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
        /// <summary>"Resources".</summary>
        public const string Resources = "0x269a27a7/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Cooking Station (Prop_production_T2_Cooking_Station): workshop 0x39fbd98f.</summary>
    public static class CookingStation
    {
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0x39fbd98f/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
    }

    /// <summary>Loom (Prop_NPC_Loom): workshop 0x3a3cc728.</summary>
    public static class Loom
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0x3a3cc728/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Coloring Station (_Prop_Production_T2_ColoringStation): workshop 0x3b51fb95.</summary>
    public static class ColoringStation
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0x3b51fb95/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Blacksmith (NPC_Crafter_Blacksmith01): workshop 0x41bf48ce.</summary>
    public static class Blacksmith
    {
        /// <summary>"Essentials".</summary>
        public const string Essentials = "0x41bf48ce/8c91ca52-be38-468c-ae74-bc6a5ffab28e";
        /// <summary>"Production place".</summary>
        public const string ProductionPlace = "0x41bf48ce/aa007b5a-f979-46fc-b4bc-4946f5cd684f";
        /// <summary>"Resources".</summary>
        public const string Resources = "0x41bf48ce/cf73af1a-9618-422f-ba2f-32a332a23787";
        /// <summary>"Survival".</summary>
        public const string Survival = "0x41bf48ce/65e6c94a-31c7-4c1f-ba8d-feb85bb87d42";
        /// <summary>"Weapons".</summary>
        public const string Weapons = "0x41bf48ce/2632c31c-37fe-48f6-b788-dfa649065edb";
        /// <summary>"Armor".</summary>
        public const string Armor = "0x41bf48ce/b50bb867-6d23-4d69-8c51-9e61212e2aec";
        /// <summary>"Tools &amp; Components".</summary>
        public const string ToolsAndComponents = "0x41bf48ce/35b13ff3-9156-4ec5-99bc-bee7d4508116";
    }

    /// <summary>Blast Furnace (Prop_Production_T5_BlastFurnace): workshop 0x485ec29c.</summary>
    public static class BlastFurnace
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0x485ec29c/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Workbench (Craftingstation_Workbench): workshop 0x4da88f73.</summary>
    public static class Workbench
    {
        /// <summary>"Essentials".</summary>
        public const string Essentials = "0x4da88f73/8c91ca52-be38-468c-ae74-bc6a5ffab28e";
        /// <summary>"Blocks".</summary>
        public const string Blocks = "0x4da88f73/d66069c5-80f8-4e6d-9d73-30b3b145cd79";
        /// <summary>"Roof Blocks".</summary>
        public const string RoofBlocks = "0x4da88f73/61c837d4-b10e-4361-9c66-156e1fae187d";
        /// <summary>"Road Blocks".</summary>
        public const string RoadBlocks = "0x4da88f73/fa99ee99-ad44-4ebd-83ef-316ea970f273";
        /// <summary>"Terrain Material".</summary>
        public const string TerrainMaterial = "0x4da88f73/6c66ad67-1cc5-45f2-badc-f94971b1f120";
        /// <summary>"Comfort Furniture".</summary>
        public const string ComfortFurniture = "0x4da88f73/84de1663-d5f4-4a39-95d2-7fc5a5d3a12b";
        /// <summary>"Decorative".</summary>
        public const string Decorative = "0x4da88f73/b9c387f3-04fc-4d02-b9f7-5b01b29282fc";
        /// <summary>"Other Furniture".</summary>
        public const string OtherFurniture = "0x4da88f73/a8419106-81c9-46e8-86ea-ef45c884ba21";
        /// <summary>"Fun".</summary>
        public const string Fun = "0x4da88f73/98541544-4d4f-432f-9102-f2481c912e2e";
    }

    /// <summary>Butter Churn (Prop_Production_T3_Butter_Churn): workshop 0x6ad05fce.</summary>
    public static class ButterChurn
    {
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0x6ad05fce/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
        /// <summary>"Resources".</summary>
        public const string Resources = "0x6ad05fce/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Coffee Brewer (Prop_Production_T6_coffeemaker_brewer): workshop 0x6b8247d6.</summary>
    public static class CoffeeBrewer
    {
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0x6b8247d6/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
    }

    /// <summary>Drying Rack (Prop_Production_T1_Drying_Rack): workshop 0x6b95dc33.</summary>
    public static class DryingRack
    {
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0x6b95dc33/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
        /// <summary>"Resources".</summary>
        public const string Resources = "0x6b95dc33/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Mill (Prop_Production_T6_improvedGrinder): workshop 0x7251e0d8.</summary>
    public static class Mill
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0x7251e0d8/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Hand Spindle (Prop_Production_T2_NPC_Hand_Spindle): workshop 0x73306967.</summary>
    public static class HandSpindle
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0x73306967/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Seafood Grill (Prop_Production_T6_MedievalGrill): workshop 0x7d96fde8.</summary>
    public static class SeafoodGrill
    {
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0x7d96fde8/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
    }

    /// <summary>Hunter (NPC_Crafter_Hunter01): workshop 0x841ad755.</summary>
    public static class Hunter
    {
        /// <summary>"Essentials".</summary>
        public const string Essentials = "0x841ad755/8c91ca52-be38-468c-ae74-bc6a5ffab28e";
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0x841ad755/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
        /// <summary>"Ammo".</summary>
        public const string Ammo = "0x841ad755/a9502a9d-8b4a-47b1-89f8-84d6443c9fb2";
        /// <summary>"Production place".</summary>
        public const string ProductionPlace = "0x841ad755/aa007b5a-f979-46fc-b4bc-4946f5cd684f";
        /// <summary>"Resources".</summary>
        public const string Resources = "0x841ad755/cf73af1a-9618-422f-ba2f-32a332a23787";
        /// <summary>"Survival".</summary>
        public const string Survival = "0x841ad755/65e6c94a-31c7-4c1f-ba8d-feb85bb87d42";
        /// <summary>"Weapons".</summary>
        public const string Weapons = "0x841ad755/2632c31c-37fe-48f6-b788-dfa649065edb";
        /// <summary>"Armor".</summary>
        public const string Armor = "0x841ad755/b50bb867-6d23-4d69-8c51-9e61212e2aec";
        /// <summary>"Comfort Furniture".</summary>
        public const string ComfortFurniture = "0x841ad755/84de1663-d5f4-4a39-95d2-7fc5a5d3a12b";
        /// <summary>"Decorative".</summary>
        public const string Decorative = "0x841ad755/b9c387f3-04fc-4d02-b9f7-5b01b29282fc";
    }

    /// <summary>Kiln (Prop_Production_T2_Kiln): workshop 0x847eab53.</summary>
    public static class KilnStation
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0x847eab53/cf73af1a-9618-422f-ba2f-32a332a23787";
        /// <summary>"Decorative".</summary>
        public const string Decorative = "0x847eab53/b9c387f3-04fc-4d02-b9f7-5b01b29282fc";
    }

    /// <summary>Lunar New Year Trader (NPC_Townsfolk_Female_Villager_05_name_chineseNewYear_Trader): workshop 0x8554479a.</summary>
    public static class LunarNewYearTrader
    {
        /// <summary>"Weapons".</summary>
        public const string Weapons = "0x8554479a/2632c31c-37fe-48f6-b788-dfa649065edb";
        /// <summary>"Magical Weapons".</summary>
        public const string MagicalWeapons = "0x8554479a/8e76b57f-2fdb-4eff-b31b-85522f4c0d74";
        /// <summary>"Comfort Furniture".</summary>
        public const string ComfortFurniture = "0x8554479a/84de1663-d5f4-4a39-95d2-7fc5a5d3a12b";
        /// <summary>"Decorative".</summary>
        public const string Decorative = "0x8554479a/b9c387f3-04fc-4d02-b9f7-5b01b29282fc";
        /// <summary>"Other Furniture".</summary>
        public const string OtherFurniture = "0x8554479a/a8419106-81c9-46e8-86ea-ef45c884ba21";
        /// <summary>"Cosmetic".</summary>
        public const string Cosmetic = "0x8554479a/ed4a5a30-3917-45a1-9a70-bae4d6d6db2f";
        /// <summary>"Fun".</summary>
        public const string Fun = "0x8554479a/98541544-4d4f-432f-9102-f2481c912e2e";
    }

    /// <summary>Infusion Altar (Prop_Production_T7_Infusion_Station): workshop 0x865085ea.</summary>
    public static class InfusionAltar
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0x865085ea/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Decoration Workbench (Craftingstation_Carpenter_Workbench): workshop 0x8996a8c4.</summary>
    public static class DecorationWorkbench
    {
        /// <summary>"Decorative".</summary>
        public const string Decorative = "0x8996a8c4/b9c387f3-04fc-4d02-b9f7-5b01b29282fc";
    }

    /// <summary>Forge (Prop_Production_T1_Forge): workshop 0x8dca2c49.</summary>
    public static class Forge
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0x8dca2c49/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Tanning Station (Prop_Production_T3_Tanning_Station): workshop 0xa4b9eaa5.</summary>
    public static class TanningStation
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0xa4b9eaa5/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Roaster (Prop_Production_T6_coffeemaker_roaster): workshop 0xa6ebcde3.</summary>
    public static class Roaster
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0xa6ebcde3/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Collector (NPC_Crafter_CryptKeeper01): workshop 0xad433e64.</summary>
    public static class Collector
    {
        /// <summary>"Essentials".</summary>
        public const string Essentials = "0xad433e64/8c91ca52-be38-468c-ae74-bc6a5ffab28e";
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0xad433e64/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
        /// <summary>"Ammo".</summary>
        public const string Ammo = "0xad433e64/a9502a9d-8b4a-47b1-89f8-84d6443c9fb2";
        /// <summary>"Production place".</summary>
        public const string ProductionPlace = "0xad433e64/aa007b5a-f979-46fc-b4bc-4946f5cd684f";
        /// <summary>"Resources".</summary>
        public const string Resources = "0xad433e64/cf73af1a-9618-422f-ba2f-32a332a23787";
        /// <summary>"Comfort Furniture".</summary>
        public const string ComfortFurniture = "0xad433e64/84de1663-d5f4-4a39-95d2-7fc5a5d3a12b";
        /// <summary>"Decorative".</summary>
        public const string Decorative = "0xad433e64/b9c387f3-04fc-4d02-b9f7-5b01b29282fc";
        /// <summary>"Other Furniture".</summary>
        public const string OtherFurniture = "0xad433e64/a8419106-81c9-46e8-86ea-ef45c884ba21";
    }

    /// <summary>Seedbed (Prop_Production_T1_Seedbed): workshop 0xba67134f.</summary>
    public static class Seedbed
    {
        /// <summary>"Seedlings".</summary>
        public const string Seedlings = "0xba67134f/c23c3be9-5824-4b9a-ba64-daeaa16771da";
    }

    /// <summary>Carpenter (NPC_Crafter_Carpenter01): workshop 0xbddd74e6.</summary>
    public static class Carpenter
    {
        /// <summary>"Essentials".</summary>
        public const string Essentials = "0xbddd74e6/8c91ca52-be38-468c-ae74-bc6a5ffab28e";
        /// <summary>"Production place".</summary>
        public const string ProductionPlace = "0xbddd74e6/aa007b5a-f979-46fc-b4bc-4946f5cd684f";
        /// <summary>"Resources".</summary>
        public const string Resources = "0xbddd74e6/cf73af1a-9618-422f-ba2f-32a332a23787";
        /// <summary>"Comfort Furniture".</summary>
        public const string ComfortFurniture = "0xbddd74e6/84de1663-d5f4-4a39-95d2-7fc5a5d3a12b";
        /// <summary>"Other Furniture".</summary>
        public const string OtherFurniture = "0xbddd74e6/a8419106-81c9-46e8-86ea-ef45c884ba21";
        /// <summary>"Tools &amp; Components".</summary>
        public const string ToolsAndComponents = "0xbddd74e6/35b13ff3-9156-4ec5-99bc-bee7d4508116";
    }

    /// <summary>Fisher (NPC_Townsfolk_Male_Villager_08_fisher_Trader): workshop 0xc4acc104.</summary>
    public static class Fisher
    {
        /// <summary>"Essentials".</summary>
        public const string Essentials = "0xc4acc104/8c91ca52-be38-468c-ae74-bc6a5ffab28e";
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0xc4acc104/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
        /// <summary>"Resources".</summary>
        public const string Resources = "0xc4acc104/cf73af1a-9618-422f-ba2f-32a332a23787";
        /// <summary>"Other Furniture".</summary>
        public const string OtherFurniture = "0xc4acc104/a8419106-81c9-46e8-86ea-ef45c884ba21";
    }

    /// <summary>Alchemist (NPC_Crafter_Alchemist01): workshop 0xc86873f9.</summary>
    public static class Alchemist
    {
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0xc86873f9/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
        /// <summary>"Ammo".</summary>
        public const string Ammo = "0xc86873f9/a9502a9d-8b4a-47b1-89f8-84d6443c9fb2";
        /// <summary>"Production place".</summary>
        public const string ProductionPlace = "0xc86873f9/aa007b5a-f979-46fc-b4bc-4946f5cd684f";
        /// <summary>"Resources".</summary>
        public const string Resources = "0xc86873f9/cf73af1a-9618-422f-ba2f-32a332a23787";
        /// <summary>"Weapons".</summary>
        public const string Weapons = "0xc86873f9/2632c31c-37fe-48f6-b788-dfa649065edb";
        /// <summary>"Magical Weapons".</summary>
        public const string MagicalWeapons = "0xc86873f9/8e76b57f-2fdb-4eff-b31b-85522f4c0d74";
        /// <summary>"Armor".</summary>
        public const string Armor = "0xc86873f9/b50bb867-6d23-4d69-8c51-9e61212e2aec";
        /// <summary>"Other Furniture".</summary>
        public const string OtherFurniture = "0xc86873f9/a8419106-81c9-46e8-86ea-ef45c884ba21";
        /// <summary>"Tools &amp; Components".</summary>
        public const string ToolsAndComponents = "0xc86873f9/35b13ff3-9156-4ec5-99bc-bee7d4508116";
    }

    /// <summary>Alchemy Station (Prop_Production_T3_Alchemy_Station): workshop 0xd08b9300.</summary>
    public static class AlchemyStation
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0xd08b9300/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Spinning Machine (Prop_Production_T5_CardingMachine_Spinning): workshop 0xd4796c9a.</summary>
    public static class SpinningMachine
    {
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0xd4796c9a/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
        /// <summary>"Resources".</summary>
        public const string Resources = "0xd4796c9a/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Farmer (NPC_Crafter_Farmer01): workshop 0xd9a35a6f.</summary>
    public static class Farmer
    {
        /// <summary>"Essentials".</summary>
        public const string Essentials = "0xd9a35a6f/8c91ca52-be38-468c-ae74-bc6a5ffab28e";
        /// <summary>"Production place".</summary>
        public const string ProductionPlace = "0xd9a35a6f/aa007b5a-f979-46fc-b4bc-4946f5cd684f";
        /// <summary>"Soil".</summary>
        public const string Soil = "0xd9a35a6f/74c737bf-3f4f-4e29-b7fe-7037a1685949";
        /// <summary>"Resources".</summary>
        public const string Resources = "0xd9a35a6f/cf73af1a-9618-422f-ba2f-32a332a23787";
        /// <summary>"Animal Husbandry".</summary>
        public const string AnimalHusbandry = "0xd9a35a6f/95a1cb62-ff2b-4b44-865f-0b6932c9fbe5";
        /// <summary>"Decorative".</summary>
        public const string Decorative = "0xd9a35a6f/b9c387f3-04fc-4d02-b9f7-5b01b29282fc";
        /// <summary>"Other Furniture".</summary>
        public const string OtherFurniture = "0xd9a35a6f/a8419106-81c9-46e8-86ea-ef45c884ba21";
        /// <summary>"Tools &amp; Components".</summary>
        public const string ToolsAndComponents = "0xd9a35a6f/35b13ff3-9156-4ec5-99bc-bee7d4508116";
    }

    /// <summary>Spinning Wheel (Prop_Production_T4_Spinning_Wheel): workshop 0xe26134b5.</summary>
    public static class SpinningWheel
    {
        /// <summary>"Supplies".</summary>
        public const string Supplies = "0xe26134b5/dc3627f7-773c-416c-a8c4-6e27a75eb1e1";
        /// <summary>"Resources".</summary>
        public const string Resources = "0xe26134b5/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Bard (NPC_Crafter_Bard01): workshop 0xe672b030.</summary>
    public static class Bard
    {
        /// <summary>"Essentials".</summary>
        public const string Essentials = "0xe672b030/8c91ca52-be38-468c-ae74-bc6a5ffab28e";
        /// <summary>"Instruments".</summary>
        public const string Instruments = "0xe672b030/783600e0-270f-43a6-b7c3-90ded5aaf21c";
        /// <summary>"Comfort Furniture".</summary>
        public const string ComfortFurniture = "0xe672b030/84de1663-d5f4-4a39-95d2-7fc5a5d3a12b";
        /// <summary>"Decorative".</summary>
        public const string Decorative = "0xe672b030/b9c387f3-04fc-4d02-b9f7-5b01b29282fc";
        /// <summary>"Other Furniture".</summary>
        public const string OtherFurniture = "0xe672b030/a8419106-81c9-46e8-86ea-ef45c884ba21";
        /// <summary>"Cosmetic".</summary>
        public const string Cosmetic = "0xe672b030/ed4a5a30-3917-45a1-9a70-bae4d6d6db2f";
    }

    /// <summary>Sparkworks Workbench (Craftingstation_Logic_System_Workbench): workshop 0xe989624a.</summary>
    public static class SparkworksWorkbench
    {
        /// <summary>"Essentials".</summary>
        public const string Essentials = "0xe989624a/8c91ca52-be38-468c-ae74-bc6a5ffab28e";
        /// <summary>"Sparkworks Tools".</summary>
        public const string SparkworksTools = "0xe989624a/4f2c4640-be1b-46cb-ba34-d8b926385a89";
    }

    /// <summary>Smelter (Prop_Production_T2_Smelter): workshop 0xf5aab50a.</summary>
    public static class Smelter
    {
        /// <summary>"Resources".</summary>
        public const string Resources = "0xf5aab50a/cf73af1a-9618-422f-ba2f-32a332a23787";
    }

    /// <summary>Ancient Researcher (NPC_Townsfolk_Female_Villager_06_LoraineCrim_AncientResearcher): workshop 0xfb819774.</summary>
    public static class AncientResearcher
    {
        /// <summary>"Production place".</summary>
        public const string ProductionPlace = "0xfb819774/aa007b5a-f979-46fc-b4bc-4946f5cd684f";
        /// <summary>"Resources".</summary>
        public const string Resources = "0xfb819774/cf73af1a-9618-422f-ba2f-32a332a23787";
        /// <summary>"Weapons".</summary>
        public const string Weapons = "0xfb819774/2632c31c-37fe-48f6-b788-dfa649065edb";
        /// <summary>"Armor".</summary>
        public const string Armor = "0xfb819774/b50bb867-6d23-4d69-8c51-9e61212e2aec";
        /// <summary>"Decorative".</summary>
        public const string Decorative = "0xfb819774/b9c387f3-04fc-4d02-b9f7-5b01b29282fc";
    }
}

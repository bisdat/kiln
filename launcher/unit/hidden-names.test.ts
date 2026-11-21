// The mock's copy of kiln_manage::catalog::is_hidden_name agrees with the Rust test's names (catalog/tests.rs hidden_names).
import { expect, test } from "bun:test";
import { isHiddenGameName } from "../src/api/hiddenNames";

test("old, test and unused game entries are hidden; look-alikes are not", () => {
  for (const n of ["Z_Material_Sand_DEPRECATED", "Z_Prop_NPC_Thermoscope_LATER", "Food_T6_common_Fish_Bait_TEST", "Armor_Tint_TEST", "TestItem_Football2",
    "_Test_Ammo_pvp_tnt2_explosive_arrow", "_UNUSED_Food_T6_raw_herb_Basil", "VC_Body_Head_01_old", "Blueprint_Voxel_Whitebox_Wood_Foundation_4m_old",
    "Consumable_Toy_Confetti_01_Blue_Mortar_WIP", "_Blueprint_Voxel_Debug_buildingmaterials", "Prop_Ancients_TrashhHeap_01_b_DONOTPLACE",
    "Deco_Wetland_medium_Bamboo_DO_NOT_PLACE", "LootChest_CHEAT_Example", "Trap_Test_Prototype_Sawblade", "Attack_Player_2H_Heavy_Whirl01_CCW_TEST2",
    "VFX_Tool_Projectile_Player_ChanneledWater_Impact_TO_BE_DELETED", "Prop_Decoration_T7_AncientAstronomy_TelescopeHuge_Base_deprecated"]) expect(isHiddenGameName(n)).toBe(true);
  for (const n of ["Material_T1_Wood_Logs", "Prop_Decoration_T6_TempleVase_02", "Enemy_Skeleton_Heavy_Attack02_Swipe", "Prop_Mines_oldTools_free_4m_03",
    "NonModalHint_05_TrainingDoll_TargetDummy", "Prop_NPC_barber_hairstyle_dummy_wigHead_02_cowlick", "BuildTool_Block_Remove",
    "_Prop_Decoration_Boss_Trophy_Skeleton_Heavy_Hero_01_wood", "old_books_icon", "Consumable_Potion_Remove_All_Food_Buffs", "Material_T3_Iron_Bar"]) expect(isHiddenGameName(n)).toBe(false);
});

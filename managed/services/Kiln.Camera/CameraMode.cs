// SPDX-License-Identifier: MIT
// The game's camera ids (its reflected enum keen::CameraId, 46 values on game build 26430253), by name and number.
// Made from the type layout in sigdb/types; a game update that changes the enum changes this file.
namespace Kiln;

/// <summary>A stock camera mode of the game, for <see cref="Camera.SetMode"/>. The names and numbers are the game's own.
/// Only <see cref="PlayerCameraBuildingFirstPerson"/> was seen working as a server-side lock in the real game; the
/// others are accepted and sent, and the game may not honour every one of them in every state of the player.</summary>
public enum CameraMode : byte
{
    EntityCamera = 0,
    PlayerCameraSpawning = 1,
    PlayerCameraSpawningDolly = 2,
    PlayerCameraRespawning = 3,
    PlayerCameraDefault = 4,
    PlayerCameraDefaultTarget = 5,
    PlayerCameraSprintingTarget = 6,
    PlayerCameraSprinting = 7,
    PlayerCameraRangedBowDefault = 8,
    PlayerCameraRangedBowSneaking = 9,
    PlayerCameraRangedStaffDefault = 10,
    PlayerCameraRangedStaffSneaking = 11,
    PlayerCameraHipfire = 12,
    PlayerCameraHipfireStaffChanneling = 13,
    PlayerCameraHipfireStaffChannelingTarget = 14,
    PlayerCameraExecute = 15,
    PlayerCameraIdle = 16,
    PlayerCameraSneaking = 17,
    PlayerCameraSitting = 18,
    PlayerCameraCooking = 19,
    PlayerCameraClimbing = 20,
    PlayerCameraHookShot = 21,
    PlayerCameraGlider = 22,
    PlayerCameraSwimming = 23,
    PlayerCameraDiving = 24,
    PlayerCameraHeadDivingStart = 25,
    PlayerCameraHeadDivingDefault = 26,
    PlayerCameraDialogue = 27,
    PlayerCameraAltar = 28,
    PlayerCameraMotherFlame = 29,
    PlayerCameraObelisk = 30,
    PlayerCameraShroudForge = 31,
    PlayerCameraBuildingAltar = 32,
    PlayerCameraBuildingAreaDepositProp = 33,
    PlayerCameraBuildingFirstPerson = 34,
    PlayerCameraBuildingFirstPersonSneak = 35,
    PlayerCameraBuildingThirdPerson = 36,
    PlayerCameraBuildingFlySparkMode = 37,
    PlayerCameraBuildingFlySparkModeSprinting = 38,
    PlayerCameraFishing = 39,
    PlayerCameraFishingHardFocusFloat = 40,
    PlayerCameraFishingSoftFocusFloat = 41,
    PlayerCameraFishingVictory = 42,
    PlayerCameraFreeAimWandDefault = 43,
    PlayerCameraFreeAimWandSprinting = 44,
    PlayerCameraFreeAimWandSneaking = 45,
}

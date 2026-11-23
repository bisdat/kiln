// SPDX-License-Identifier: MIT
// The game's camera ids (its reflected enum keen::CameraId, 46 values on game build 26430253), by name and number.
// Made from the type layout in sigdb/types; a game update that changes the enum changes this file.
namespace Kiln;

/// <summary>A stock camera mode of the game, for <see cref="Camera.SetMode"/>. The names and numbers are the game's own.
/// Only <see cref="PlayerCameraBuildingFirstPerson"/> was seen working as a server-side lock in the real game; the
/// others are accepted and sent, and the game may not honour every one of them in every state of the player.</summary>
public enum CameraMode : byte
{
    /// <summary>The game's entity camera, not one of the player's camera modes.</summary>
    EntityCamera = 0,
    /// <summary>The camera while the player spawns into the world.</summary>
    PlayerCameraSpawning = 1,
    /// <summary>The dolly shot while the player spawns.</summary>
    PlayerCameraSpawningDolly = 2,
    /// <summary>The camera while the player respawns.</summary>
    PlayerCameraRespawning = 3,
    /// <summary>The player's default third-person camera.</summary>
    PlayerCameraDefault = 4,
    /// <summary>The default camera with a locked target.</summary>
    PlayerCameraDefaultTarget = 5,
    /// <summary>The camera while sprinting with a locked target.</summary>
    PlayerCameraSprintingTarget = 6,
    /// <summary>The camera while sprinting.</summary>
    PlayerCameraSprinting = 7,
    /// <summary>The camera while aiming a bow.</summary>
    PlayerCameraRangedBowDefault = 8,
    /// <summary>The camera while aiming a bow and sneaking.</summary>
    PlayerCameraRangedBowSneaking = 9,
    /// <summary>The camera while aiming a staff.</summary>
    PlayerCameraRangedStaffDefault = 10,
    /// <summary>The camera while aiming a staff and sneaking.</summary>
    PlayerCameraRangedStaffSneaking = 11,
    /// <summary>The camera while shooting from the hip.</summary>
    PlayerCameraHipfire = 12,
    /// <summary>The camera while channelling a staff from the hip.</summary>
    PlayerCameraHipfireStaffChanneling = 13,
    /// <summary>The camera while channelling a staff from the hip with a locked target.</summary>
    PlayerCameraHipfireStaffChannelingTarget = 14,
    /// <summary>The camera during an execution move.</summary>
    PlayerCameraExecute = 15,
    /// <summary>The camera while the player idles.</summary>
    PlayerCameraIdle = 16,
    /// <summary>The camera while sneaking.</summary>
    PlayerCameraSneaking = 17,
    /// <summary>The camera while sitting.</summary>
    PlayerCameraSitting = 18,
    /// <summary>The camera while cooking.</summary>
    PlayerCameraCooking = 19,
    /// <summary>The camera while climbing.</summary>
    PlayerCameraClimbing = 20,
    /// <summary>The camera while using the grappling hook.</summary>
    PlayerCameraHookShot = 21,
    /// <summary>The camera while gliding.</summary>
    PlayerCameraGlider = 22,
    /// <summary>The camera while swimming.</summary>
    PlayerCameraSwimming = 23,
    /// <summary>The camera while diving.</summary>
    PlayerCameraDiving = 24,
    /// <summary>The camera at the start of a head-first dive.</summary>
    PlayerCameraHeadDivingStart = 25,
    /// <summary>The camera during a head-first dive.</summary>
    PlayerCameraHeadDivingDefault = 26,
    /// <summary>The camera during a dialogue.</summary>
    PlayerCameraDialogue = 27,
    /// <summary>The camera at a flame altar.</summary>
    PlayerCameraAltar = 28,
    /// <summary>The camera at the mother flame.</summary>
    PlayerCameraMotherFlame = 29,
    /// <summary>The camera at an obelisk.</summary>
    PlayerCameraObelisk = 30,
    /// <summary>The camera at the shroud forge.</summary>
    PlayerCameraShroudForge = 31,
    /// <summary>The camera while building with an altar.</summary>
    PlayerCameraBuildingAltar = 32,
    /// <summary>The camera while placing an area deposit prop in build mode.</summary>
    PlayerCameraBuildingAreaDepositProp = 33,
    /// <summary>The build mode camera in first person. The only mode seen working as a server-side lock in the real game.</summary>
    PlayerCameraBuildingFirstPerson = 34,
    /// <summary>The build mode camera, first person, sneaking.</summary>
    PlayerCameraBuildingFirstPersonSneak = 35,
    /// <summary>The build mode camera, third person.</summary>
    PlayerCameraBuildingThirdPerson = 36,
    /// <summary>The build mode camera, flying (spark mode).</summary>
    PlayerCameraBuildingFlySparkMode = 37,
    /// <summary>The build mode camera, flying (spark mode) and sprinting.</summary>
    PlayerCameraBuildingFlySparkModeSprinting = 38,
    /// <summary>The camera while fishing.</summary>
    PlayerCameraFishing = 39,
    /// <summary>The camera while fishing, focused hard on the float.</summary>
    PlayerCameraFishingHardFocusFloat = 40,
    /// <summary>The camera while fishing, focused softly on the float.</summary>
    PlayerCameraFishingSoftFocusFloat = 41,
    /// <summary>The camera when a fish is caught.</summary>
    PlayerCameraFishingVictory = 42,
    /// <summary>The camera while aiming a wand freely.</summary>
    PlayerCameraFreeAimWandDefault = 43,
    /// <summary>The camera while aiming a wand freely and sprinting.</summary>
    PlayerCameraFreeAimWandSprinting = 44,
    /// <summary>The camera while aiming a wand freely and sneaking.</summary>
    PlayerCameraFreeAimWandSneaking = 45,
}

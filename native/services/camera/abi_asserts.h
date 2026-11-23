/* SPDX-License-Identifier: MIT
 * kiln.camera v1 as bytes: the sizes, offsets and numbers of services/kiln_camera.h. Compiled into the service (the
 * game's ABI) and into tests/abi_test.c (the host's; both are 64-bit with the same layout for these types). A number
 * here changes only with a new table version: the managed mirror, the test kit's fake and the wire codec repeat them. */
#ifndef KILN_CAMERA_ABI_ASSERTS_H
#define KILN_CAMERA_ABI_ASSERTS_H
#include <assert.h>
#include <stddef.h>

#include "services/kiln_camera.h"

static_assert(KILN_CAMERA_SERVER_MODE == 1 && KILN_CAMERA_LOCAL_VISUAL == 2 && KILN_CAMERA_REMOTE_VISUAL == 3, "endpoints");
static_assert(KILN_CAMERA_MODE == 1 && KILN_CAMERA_PATH == 2 && KILN_CAMERA_PROFILE == 4 && KILN_CAMERA_SHAKE == 8 && KILN_CAMERA_LENS == 16, "parts; the lens stands alone");
static_assert(KILN_CAMERA_VISUAL_ONLY == 0 && KILN_CAMERA_STOCK_MODE == 1 && KILN_CAMERA_AIM_TOO == 2, "input policies");
static_assert(KILN_CAMERA_LENS_NONE == 0 && KILN_CAMERA_LENS_CONSTANT == 1 && KILN_CAMERA_LENS_KEYS == 2, "lens sources");
static_assert(KILN_CAMERA_FREE_ORBIT == 0 && KILN_CAMERA_FOLLOW_HEADING == 1 && KILN_CAMERA_FIXED_WORLD == 2 && KILN_CAMERA_COLLISION_NONE == 0, "profile enums");
static_assert(KILN_CAMERA_TRANSITION_NONE == 0 && KILN_CAMERA_TRANSITION_BLINK == 1 && KILN_CAMERA_TRANSITION_LERP == 2, "transitions");
static_assert(KILN_CAMERA_PRESERVE_TIMELINE == 0 && KILN_CAMERA_RESTART_TIMELINE == 1, "update timing");
static_assert(KILN_CAMERA_APPLIED == 1 && KILN_CAMERA_UPDATED == 2 && KILN_CAMERA_ENDED == 3, "change kinds");
static_assert(KILN_CAMERA_END_NONE == 0 && KILN_CAMERA_END_RELEASED == 1 && KILN_CAMERA_END_EXPIRED == 2 && KILN_CAMERA_END_TARGET_LOST == 3 &&
                  KILN_CAMERA_END_PLAYER_LEFT == 4 && KILN_CAMERA_END_WORLD_STOPPED == 5 && KILN_CAMERA_END_SUPERSEDED == 6 &&
                  KILN_CAMERA_END_CAPABILITY_LOST == 7 && KILN_CAMERA_END_OWNER_RETIRED == 8 && KILN_CAMERA_END_PEER_DISCONNECTED == 9 &&
                  KILN_CAMERA_END_AUTHORITY_TIMEOUT == 10 && KILN_CAMERA_END_SAFETY_REJECTED == 11 && KILN_CAMERA_END_TRANSPORT_TIMEOUT == 12 &&
                  KILN_CAMERA_END_CANCELLED_BEFORE_APPLY == 13,
              "end reasons");
static_assert(KILN_CAMERA_REASON_NONE == 0 && KILN_CAMERA_REASON_BUSY == 1 && KILN_CAMERA_REASON_ALREADY_OWNED == 2 && KILN_CAMERA_REASON_UNKNOWN_LEASE == 3 &&
                  KILN_CAMERA_REASON_FOREIGN_OWNER == 4 && KILN_CAMERA_REASON_WRONG_ENDPOINT == 5 && KILN_CAMERA_REASON_INPUT_POLICY == 6 &&
                  KILN_CAMERA_REASON_REQUIRES_KILN_CLIENT == 7 && KILN_CAMERA_REASON_PEER_UNAVAILABLE == 8 && KILN_CAMERA_REASON_NEGOTIATION == 9 &&
                  KILN_CAMERA_REASON_TARGET_UNAVAILABLE == 10 && KILN_CAMERA_REASON_NO_CAMERA_FRAME == 11 && KILN_CAMERA_REASON_STALE_WORLD == 12 &&
                  KILN_CAMERA_REASON_EXPIRED_UPDATE == 13 && KILN_CAMERA_REASON_COLLISION == 14 && KILN_CAMERA_REASON_TRANSPORT_TIMEOUT == 15 &&
                  KILN_CAMERA_REASON_MALFORMED_REMOTE == 16 && KILN_CAMERA_REASON_RESOURCE_BUSY == 17 && KILN_CAMERA_REASON_FRAME_FAULT == 18 &&
                  KILN_CAMERA_REASON_SNAPSHOT_BUSY == 19 && KILN_CAMERA_REASON_SNAPSHOT_STALE == 20 && KILN_CAMERA_REASON_BOUNDS == 21 &&
                  KILN_CAMERA_REASON_RATE_LIMIT == 22 && KILN_CAMERA_REASON_LEASE_KIND == 23 && KILN_CAMERA_REASON_CANCELLED == 24 &&
                  KILN_CAMERA_REASON_CLIENT_CAPABILITY == 25 && KILN_CAMERA_REASON_INSUFFICIENT_LIFETIME == 26 && KILN_CAMERA_REASON_BODY_TOO_LARGE == 27,
              "refusal reasons");
static_assert(KILN_CAMERA_COMMIT_NONE == 0 && KILN_CAMERA_COMMIT_SERVER_STATE == 1 && KILN_CAMERA_COMMIT_CLIENT_FRAME == 2, "commit scopes");
static_assert(KILN_CAMERA_MAY_HAVE_APPLIED == 1 && KILN_CAMERA_ALREADY_ENDED == 2, "result flags");
static_assert(KILN_CAMERA_CAP_SERVER_MODE == 1 && KILN_CAMERA_CAP_LOCAL_POSE == 2 && KILN_CAMERA_CAP_LOCAL_FOV == 4 && KILN_CAMERA_CAP_LOCAL_PROFILE == 8 &&
                  KILN_CAMERA_CAP_LOCAL_SHAKE == 16 && KILN_CAMERA_CAP_RENDERED_SNAPSHOT == 32 && KILN_CAMERA_CAP_REMOTE == 64 &&
                  KILN_CAMERA_CAP_RESOURCE_PROFILE == 128,
              "capability bits");
static_assert(KILN_CAMERA_MAX_KEYS == 256 && KILN_CAMERA_MAX_DURATION_MS == 120000 && KILN_CAMERA_MAX_PENDING == 64, "limits");

static_assert(sizeof(KilnCameraPose) == 40 && offsetof(KilnCameraPose, rotation) == 24, "pose");
static_assert(sizeof(KilnCameraKey) == 56 && offsetof(KilnCameraKey, pose) == 8 && offsetof(KilnCameraKey, fov_y_degrees) == 48, "key");
static_assert(sizeof(KilnCameraProfile) == 52 && offsetof(KilnCameraProfile, distance) == 16 && offsetof(KilnCameraProfile, offset_world_y) == 32 &&
                  offsetof(KilnCameraProfile, rotation) == 36,
              "profile");
static_assert(sizeof(KilnCameraShake) == 16, "shake");
static_assert(sizeof(KilnCameraRequest) == 128 && offsetof(KilnCameraRequest, priority) == 32 && offsetof(KilnCameraRequest, fov_y_degrees) == 40 &&
                  offsetof(KilnCameraRequest, camera_id) == 44 && offsetof(KilnCameraRequest, profile) == 48 && offsetof(KilnCameraRequest, shake) == 100 &&
                  offsetof(KilnCameraRequest, key_count) == 116 && offsetof(KilnCameraRequest, keys) == 120,
              "request");
static_assert(sizeof(KilnCameraResult) == 72 && offsetof(KilnCameraResult, lease) == 32 && offsetof(KilnCameraResult, reason) == 56, "result");
static_assert(sizeof(KilnCameraChange) == 72 && offsetof(KilnCameraChange, lease) == 32 && offsetof(KilnCameraChange, reason) == 56, "change");
static_assert(sizeof(KilnCameraVisual) == 96 && offsetof(KilnCameraVisual, pose) == 40 && offsetof(KilnCameraVisual, fov_y_radians) == 80, "visual snapshot");
static_assert(sizeof(KilnCameraTransport) == 32, "transport bridge v1");
static_assert(sizeof(KilnCameraApi) == 88 && offsetof(KilnCameraApi, capabilities) == 8 && offsetof(KilnCameraApi, get_visual) == 56 &&
                  offsetof(KilnCameraApi, transport_ended) == 80,
              "kiln.camera v1 is 88 bytes");
#endif

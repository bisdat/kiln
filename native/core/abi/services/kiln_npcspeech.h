/* SPDX-License-Identifier: MIT
 * kiln.npcspeech v1: Kiln's lines in the game's own speech bubble over a friendly NPC (villagers, craftspeople, their
 * assistants: every NPC with an NpcDialog). Game client only: the bubble, its text and the line's voice-over are client
 * data (docs/re/npc-speech.md). The host decides and sends the decision to Kiln clients (Kiln.Npcs, channel
 * kiln:npcspeech); this table is what a client applies:
 *   - an override per NPC: Suppress (no greeting bubble, no voice-over of a greeting) or Replace (the bubble shows one of
 *     the given loca tags instead of the game's pick, no voice-over);
 *   - a Say per NPC: one loca tag in the bubble for a time, shown even when the NPC is not the player's target.
 * Texts are loca tags that kiln.content's loca hooks answer: text() registers one (per language). Hooks go in with the
 * first override or Say and stay in (they answer nothing without one). World ends: the caller clears (clear_all).
 * Evidence: docs/re/npc-speech.md; usage: docs/kiln/services/npc-speech.md. */
#ifndef KILN_NPCSPEECH_H
#define KILN_NPCSPEECH_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_NPCSPEECH_SERVICE "kiln.npcspeech"
#define KILN_NPCSPEECH_VERSION 1u

#define KILN_SPEECH_GAME 0u     /* the game's own lines (clears the override) */
#define KILN_SPEECH_SUPPRESS 1u /* no greeting bubble and no greeting voice-over */
#define KILN_SPEECH_REPLACE 2u  /* one of the given tags instead of the game's line; no voice-over */

#define KILN_SPEECH_MAX_LINES 32u
#define KILN_SPEECH_MAX_SAY_MS 60000u

typedef struct KilnNpcSpeechStatus {
    uint32_t size;        /* in: sizeof(KilnNpcSpeechStatus) */
    uint32_t resolved;    /* bit per part found on this build: 1 NpcBubble, 2 BubbleDraw, 4 the record layout (voice), 8 texts (kiln.content) */
    uint32_t installed;   /* 1 NpcBubble, 2 BubbleDraw: hooks in now */
    uint32_t overrides;   /* NPCs with an override now */
    uint32_t says;        /* NPCs with a running Say now */
    uint32_t texts;       /* tags registered through text() */
    uint64_t bubbles;     /* bubble draws of NPCs with an override or Say */
    uint64_t changed;     /* of those, bubbles Kiln changed (text or visibility) */
    uint64_t muted;       /* voice-over entries Kiln muted (put back when the override ends) */
} KilnNpcSpeechStatus;

typedef struct KilnNpcSpeechApi {
    KilnServiceHeader header; /* { sizeof(KilnNpcSpeechApi), KILN_NPCSPEECH_VERSION } */
    /* The effective override of one NPC (the caller resolves owners and priorities). mode KILN_SPEECH_*; tags/n the lines
     * of Replace (1..KILN_SPEECH_MAX_LINES, nonzero), ignored otherwise. Any thread. KILN_ERR_UNAVAILABLE: the bubble
     * hooks are not on this build (or not on a dedicated server); KILN_ERR_INVALID_ARGUMENT: a bad mode, entity 0, no or
     * too many tags. */
    KilnResult(KILN_CALL* set)(KilnEntity npc, uint32_t mode, const uint32_t* tags, uint32_t n);
    /* One tag in the NPC's bubble for ms (1..KILN_SPEECH_MAX_SAY_MS); tag 0 or ms 0 ends a running Say. A new Say replaces
     * the old one. Any thread. Errors as set(). */
    KilnResult(KILN_CALL* say)(KilnEntity npc, uint32_t tag, uint32_t ms);
    /* The texts of a loca tag: "text" or {"en": "...", "de": "..."} ("*" = every language), UTF-8 JSON. The same tag and
     * texts again change nothing. Any thread. KILN_ERR_UNAVAILABLE: kiln.content's loca hooks are not here. */
    KilnResult(KILN_CALL* text)(uint32_t tag, const char* json, size_t len);
    /* Ends every override and Say (a world's end, a reset). Voice-overs come back the next time their NPC's bubble runs.
     * Any thread. */
    KilnResult(KILN_CALL* clear_all)(void);
    /* Any thread. */
    KilnResult(KILN_CALL* status)(KilnNpcSpeechStatus* out);
} KilnNpcSpeechApi;

#ifdef __cplusplus
}
#endif
#endif

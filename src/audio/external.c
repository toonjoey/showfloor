#include <ultra64.h>
#include "sm64.h"
#include "heap.h"
#include "load.h"
#include "data.h"
#include "seqplayer.h"
#include "external.h"
#include "playback.h"
#include "synthesis.h"
#include "game/level_update.h"
#include "game/object_list_processor.h"
#include "game/camera.h"
#include "seq_ids.h"
#include "dialog_ids.h"

#define EU_FLOAT(x) x

// N.B. sound banks are different from the audio banks referred to in other
// files. We should really fix our naming to be less ambiguous...
#define MAX_BACKGROUND_MUSIC_QUEUE_SIZE 6
#define MAX_CHANNELS_PER_SOUND_BANK 1

#define SEQUENCE_NONE 0xFF

#define SAMPLES_TO_OVERPRODUCE 0x10
#define EXTRA_BUFFERED_AI_SAMPLES_TARGET 0x40

struct Sound {
    s32 soundBits;
    f32 *position;
}; // size = 0x8

struct ChannelVolumeScaleFade {
    f32 velocity;
    u8 target;
    f32 current;
    u16 remainingFrames;
}; // size = 0x10

struct SoundCharacteristics {
    f32 *x;
    f32 *y;
    f32 *z;
    f32 distance;
    u32 priority;
    u32 soundBits; // packed bits, same as first arg to play_sound
    u8 soundStatus;
    u8 freshness;
    u8 prev;
    u8 next;
}; // size = 0x1C

// Also the number of frames a discrete sound can be in the WAITING state before being deleted
#define SOUND_MAX_FRESHNESS 10

struct SequenceQueueItem {
    u8 seqId;
    u8 priority;
}; // size = 0x2

// data
s32 gAudioErrorFlags = 0;
s32 sGameLoopTicked = 0;

// Dialog sounds
// The US difference is the sound for DIALOG_037 ("I win! You lose! Ha ha ha ha!
// You're no slouch, but I'm a better sledder! Better luck next time!"), spoken
// by Koopa instead of the penguin in JP.

#define UKIKI 0
#define TUXIE 1
#define BOWS1 2 // Bowser Intro / Doors Laugh
#define KOOPA 3
#define KBOMB 4
#define BOO 5
#define BOMB 6
#define BOWS2 7 // Bowser Battle Laugh
#define GRUNT 8
#define WIGLR 9
#define YOSHI 10
#define _ 0xFF

#define DIFF KOOPA

u8 sDialogSpeaker[] = {
    //       0      1      2      3      4      5      6      7      8      9
    /* 0*/ _,     BOMB,  BOMB,  BOMB,  BOMB,  KOOPA, KOOPA, KOOPA, _,     KOOPA,
    /* 1*/ _,     _,     _,     _,     _,     _,     _,     KBOMB, _,     _,
    /* 2*/ _,     BOWS1, BOWS1, BOWS1, BOWS1, BOWS1, BOWS1, BOWS1, BOWS1, BOWS1,
    /* 3*/ _,     _,     _,     _,     _,     _,     _,     DIFF,  _,     _,
    /* 4*/ _,     KOOPA, _,     _,     _,     _,     _,     BOMB,  _,     _,
    /* 5*/ _,     _,     _,     _,     _,     TUXIE, TUXIE, TUXIE, TUXIE, TUXIE,
    /* 6*/ _,     _,     _,     _,     _,     _,     _,     BOWS2, _,     _,
    /* 7*/ _,     _,     _,     _,     _,     _,     _,     _,     _,     UKIKI,
    /* 8*/ UKIKI, _,     _,     _,     _,     BOO,   _,     _,     _,     _,
    /* 9*/ BOWS2, _,     BOWS2, BOWS2, _,     _,     _,     _,     BOO,   BOO,
    /*10*/ UKIKI, UKIKI, _,     _,     _,     BOMB,  BOMB,  BOO,   BOO,   _,
    /*11*/ _,     _,     _,     _,     GRUNT, GRUNT, KBOMB, GRUNT, GRUNT, _,
    /*12*/ _,     _,     _,     _,     _,     _,     _,     _,     KBOMB, _,
    /*13*/ _,     _,     TUXIE, _,     _,     _,     _,     _,     _,     _,
    /*14*/ _,     _,     _,     _,     _,     _,     _,     _,     _,     _,
    /*15*/ WIGLR, WIGLR, WIGLR, _,     _,     _,     _,     _,     _,     _,
    /*16*/ _,     YOSHI, _,     _,     _,     _,     _,     _,     WIGLR, _
};
#undef _
STATIC_ASSERT(ARRAY_COUNT(sDialogSpeaker) == DIALOG_COUNT,
              "change this array if you are adding dialogs");

s32 sDialogSpeakerVoice[] = {
    SOUND_OBJ_UKIKI_CHATTER_LONG,
    SOUND_OBJ_BIG_PENGUIN_YELL,
    SOUND_OBJ_BOWSER_INTRO_LAUGH,
    SOUND_OBJ_KOOPA_TALK,
    SOUND_OBJ_KING_BOBOMB_TALK,
    SOUND_OBJ_BOO_LAUGH_LONG,
    SOUND_OBJ_BOBOMB_BUDDY_TALK,
    SOUND_OBJ_BOWSER_LAUGH,
    SOUND_OBJ2_BOSS_DIALOG_GRUNT,
    SOUND_OBJ_WIGGLER_TALK,
    SOUND_GENERAL_YOSHI_TALK,
    NO_SOUND,
    NO_SOUND,
    NO_SOUND,
    NO_SOUND,
};

u8 sNumProcessedSoundRequests = 0;
u8 sSoundRequestCount = 0;

// Music dynamic tables. A dynamic describes which volumes to apply to which
// channels of a sequence (I think?), and different parts of a level can have
// different dynamics. Each table below specifies first the sequence to apply
// the dynamics to, then a bunch of conditions for when each dynamic applies
// (e.g. "only if Mario's X position is between 100 and 300"), and finally a
// fallback dynamic. Due to the encoding of the tables, the conditions must
// come in the same order as the macros.
// TODO: dynamic isn't a great term for this, it means other things in music...

#define MARIO_X_GE 0
#define MARIO_Y_GE 1
#define MARIO_Z_GE 2
#define MARIO_X_LT 3
#define MARIO_Y_LT 4
#define MARIO_Z_LT 5
#define MARIO_IS_IN_AREA 6
#define MARIO_IS_IN_ROOM 7

#define DYN1(cond1, val1, res) (s16)(1 << (15 - cond1) | res), val1
#define DYN2(cond1, val1, cond2, val2, res)                                                            \
    (s16)(1 << (15 - cond1) | 1 << (15 - cond2) | res), val1, val2
#define DYN3(cond1, val1, cond2, val2, cond3, val3, res)                                               \
    (s16)(1 << (15 - cond1) | 1 << (15 - cond2) | 1 << (15 - cond3) | res), val1, val2, val3

s16 sDynDDD[] = {
    SEQ_LEVEL_WATER,
    DYN2(MARIO_X_LT, -800, MARIO_IS_IN_AREA, AREA_DDD_WHIRLPOOL & 0xf, 0),
    DYN3(MARIO_Y_GE, -2000, MARIO_X_LT, 470, MARIO_IS_IN_AREA, AREA_DDD_WHIRLPOOL & 0xf, 0),
    DYN2(MARIO_Y_GE, 100, MARIO_IS_IN_AREA, AREA_DDD_SUB & 0xf, 2),
    1,
};
s16 sDynUnk38[] = {
    SEQ_LEVEL_UNDERGROUND,
    DYN1(MARIO_IS_IN_AREA, 1, 3),
    DYN1(MARIO_IS_IN_AREA, 2, 4),
    DYN1(MARIO_IS_IN_AREA, 3, 7),
    0,
};
s16 sDynNone[] = { SEQ_SOUND_PLAYER, 0 };

u8 sCurrentMusicDynamic = 0xff;
u8 sBackgroundMusicForDynamics = SEQUENCE_NONE;

#define STUB_LEVEL(_0, _1, _2, _3, _4, _5, _6, leveldyn, _8) leveldyn,
#define DEFINE_LEVEL(_0, _1, _2, _3, _4, _5, _6, _7, _8, leveldyn, _10) leveldyn,

#define _ sDynNone
s16 *sLevelDynamics[LEVEL_COUNT] = {
    _, // LEVEL_NONE
#include "levels/level_defines.h"
};
#undef _
#undef STUB_LEVEL
#undef DEFINE_LEVEL

struct MusicDynamic {
    /*0x0*/ s16 bits1;
    /*0x2*/ u16 volScale1;
    /*0x4*/ s16 dur1;
    /*0x6*/ s16 bits2;
    /*0x8*/ u16 volScale2;
    /*0xA*/ s16 dur2;
}; // size = 0xC

struct MusicDynamic sMusicDynamics[8] = {
    { 0x0000, 127, 100, 0x0e43, 0, 100 }, // SEQ_LEVEL_WATER
    { 0x0003, 127, 100, 0x0e40, 0, 100 }, // SEQ_LEVEL_WATER
    { 0x0e43, 127, 200, 0x0000, 0, 200 }, // SEQ_LEVEL_WATER
    { 0x02ff, 127, 100, 0x0100, 0, 100 }, // SEQ_LEVEL_UNDERGROUND
    { 0x03f7, 127, 100, 0x0008, 0, 100 }, // SEQ_LEVEL_UNDERGROUND
    { 0x0070, 127, 10, 0x0000, 0, 100 },  // SEQ_LEVEL_SPOOKY
    { 0x0000, 127, 100, 0x0070, 0, 10 },  // SEQ_LEVEL_SPOOKY
    { 0xffff, 127, 100, 0x0000, 0, 100 }, // any (unused)
};

#define STUB_LEVEL(_0, _1, _2, _3, echo1, echo2, echo3, _7, _8) { echo1, echo2, echo3 },
#define DEFINE_LEVEL(_0, _1, _2, _3, _4, _5, echo1, echo2, echo3, _9, _10) { echo1, echo2, echo3 },

u8 sLevelAreaReverbs[LEVEL_COUNT][3] = {
    { 0x00, 0x00, 0x00 }, // LEVEL_NONE
#include "levels/level_defines.h"
};
#undef STUB_LEVEL
#undef DEFINE_LEVEL

#define STUB_LEVEL(_0, _1, _2, volume, _4, _5, _6, _7, _8) volume,
#define DEFINE_LEVEL(_0, _1, _2, _3, _4, volume, _6, _7, _8, _9, _10) volume,

u16 sLevelAcousticReaches[LEVEL_COUNT] = {
    20000, // LEVEL_NONE
#include "levels/level_defines.h"
};

#undef STUB_LEVEL
#undef DEFINE_LEVEL

#define AUDIO_MAX_DISTANCE US_FLOAT(22000.0)

#define LOW_VOLUME_REVERB 48.0

#define VOLUME_RANGE_UNK1 0.8f
#define VOLUME_RANGE_UNK2 1.0f

// Default volume for background music sequences (playing on player 0).
u8 sBackgroundMusicDefaultVolume[] = {
    127, // SEQ_SOUND_PLAYER
    80,  // SEQ_EVENT_CUTSCENE_COLLECT_STAR
    80,  // SEQ_MENU_TITLE_SCREEN
    75,  // SEQ_LEVEL_GRASS
    70,  // SEQ_LEVEL_INSIDE_CASTLE
    75,  // SEQ_LEVEL_WATER
    75,  // SEQ_LEVEL_HOT
    75,  // SEQ_LEVEL_BOSS_KOOPA
    70,  // SEQ_LEVEL_SNOW
    65,  // SEQ_LEVEL_SLIDE
    80,  // SEQ_LEVEL_SPOOKY
    65,  // SEQ_EVENT_PIRANHA_PLANT
    85,  // SEQ_LEVEL_UNDERGROUND
    75,  // SEQ_MENU_STAR_SELECT
    65,  // SEQ_EVENT_POWERUP
    70,  // SEQ_EVENT_METAL_CAP
    65,  // SEQ_EVENT_KOOPA_MESSAGE
    70,  // SEQ_LEVEL_KOOPA_ROAD
    70,  // SEQ_EVENT_HIGH_SCORE
    65,  // SEQ_EVENT_MERRY_GO_ROUND
    80,  // SEQ_EVENT_RACE
    70,  // SEQ_EVENT_CUTSCENE_STAR_SPAWN
    85,  // SEQ_EVENT_BOSS
    75,  // SEQ_EVENT_CUTSCENE_COLLECT_KEY
    75,  // SEQ_EVENT_ENDLESS_STAIRS
    85,  // SEQ_LEVEL_BOSS_KOOPA_FINAL
    70,  // SEQ_EVENT_CUTSCENE_CREDITS
    80,  // SEQ_EVENT_SOLVE_PUZZLE
    80,  // SEQ_EVENT_TOAD_MESSAGE
    70,  // SEQ_EVENT_PEACH_MESSAGE
    75,  // SEQ_EVENT_CUTSCENE_INTRO
    80,  // SEQ_EVENT_CUTSCENE_VICTORY
    70,  // SEQ_EVENT_CUTSCENE_ENDING
    65,  // SEQ_MENU_FILE_SELECT
    0,   // SEQ_EVENT_CUTSCENE_LAKITU (not in JP)
};

STATIC_ASSERT(ARRAY_COUNT(sBackgroundMusicDefaultVolume) == SEQ_COUNT,
              "change this array if you are adding sequences");

u8 sCurrentBackgroundMusicSeqId = SEQUENCE_NONE;
u8 sMusicDynamicDelay = 0;
u8 sSoundBankUsedListBack[SOUND_BANK_COUNT] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
u8 sSoundBankFreeListFront[SOUND_BANK_COUNT] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
u8 sNumSoundsInBank[SOUND_BANK_COUNT] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // only used for debugging
u8 sMaxChannelsForSoundBank[SOUND_BANK_COUNT] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

// Banks 2 and 7 both grew from 0x30 sounds to 0x40 in size in US.
#define BANK27_SIZE 0x30
u8 sNumSoundsPerBank[SOUND_BANK_COUNT] = {
    0x70, 0x30, BANK27_SIZE, 0x80, 0x20, 0x80, 0x20, BANK27_SIZE, 0x80, 0x80,
};
#undef BANK27_SIZE

// sBackgroundMusicMaxTargetVolume and sBackgroundMusicTargetVolume use the 0x80
// bit to indicate that they are set, and the rest of the bits for the actual value
#define TARGET_VOLUME_IS_PRESENT_FLAG 0x80
#define TARGET_VOLUME_VALUE_MASK 0x7f
#define TARGET_VOLUME_UNSET 0x00

f32 gGlobalSoundSource[3] = { 0.0f, 0.0f, 0.0f };
f32 sUnusedSoundArgs[3] = { 1.0f, 1.0f, 1.0f };
u8 sSoundBankDisabled[16] = { 0 };
u8 D_80332108 = 0;
u8 sHasStartedFadeOut = FALSE;
u16 sSoundBanksThatLowerBackgroundMusic = 0;
u8 sUnused80332114 = 0;  // never read, set to 0
u16 sUnused80332118 = 0; // never read, set to 0
u8 sBackgroundMusicMaxTargetVolume = TARGET_VOLUME_UNSET;
u8 D_80332120 = 0;
u8 D_80332124 = 0;

u8 sBackgroundMusicQueueSize = 0;

// bss
s16 *gCurrAiBuffer;

struct Sound sSoundRequests[0x100];
// Curiously, this has size 3, despite SEQUENCE_PLAYERS == 4 on EU
struct ChannelVolumeScaleFade D_80360928[3][CHANNELS_MAX];
u8 sUsedChannelsForSoundBank[SOUND_BANK_COUNT];
u8 sCurrentSound[SOUND_BANK_COUNT][MAX_CHANNELS_PER_SOUND_BANK]; // index into sSoundBanks

/**
 * For each sound bank, a pool containing the currently active sounds for that bank.
 * The free and used slots in these pools are linked lists. The element sSoundBanks[bank][0]
 * is used as a list header for the used list, and never holds an actual sound. See also
 * sSoundBankUsedListBack and sSoundBankFreeListFront.
 */
struct SoundCharacteristics sSoundBanks[SOUND_BANK_COUNT][40];

u8 sSoundMovingSpeed[SOUND_BANK_COUNT];
u8 sBackgroundMusicTargetVolume;
static u8 sLowerBackgroundMusicVolume;
struct SequenceQueueItem sBackgroundMusicQueue[MAX_BACKGROUND_MUSIC_QUEUE_SIZE];
extern OSMesgQueue *D_SH_80350F88;
extern OSMesgQueue *D_SH_80350FA8;
typedef u16 FadeT;

// some sort of main thread -> sound thread dispatchers
extern void func_802ad728(u32 bits, f32 arg);
extern void func_802ad74c(u32 bits, u32 arg);
extern void func_802ad770(u32 bits, s8 arg);

static void update_background_music_after_sound(u8 bank, u8 soundIndex);
static void update_game_sound(void);
static void fade_channel_volume_scale(u8 player, u8 channelId, u8 targetScale, u16 fadeTimer);
void process_level_music_dynamics(void);
static u8 begin_background_music_fade(u16 fadeDuration);
void func_80320ED8(void);

/**
 * Called from threads: thread3_main, thread5_game_loop
 */
static void seq_player_fade_to_zero_volume(s32 player, FadeT fadeDuration) {
    struct SequencePlayer *seqPlayer = &gSequencePlayers[player];

    seqPlayer->fadeVelocity = -(seqPlayer->fadeVolume / fadeDuration);
    seqPlayer->state = SEQUENCE_PLAYER_STATE_FADE_OUT;
    seqPlayer->fadeRemainingFrames = fadeDuration;
}

/**
 * Called from threads: thread4_sound, thread5_game_loop
 */
static void func_8031D690(s32 player, FadeT fadeInTime) {
    struct SequencePlayer *seqPlayer = &gSequencePlayers[player];

    if (fadeInTime == 0 || seqPlayer->state == SEQUENCE_PLAYER_STATE_FADE_OUT) {
        return;
    }

    seqPlayer->state = SEQUENCE_PLAYER_STATE_2;
    seqPlayer->fadeRemainingFrames = fadeInTime;
    seqPlayer->fadeVolume = 0.0f;
    seqPlayer->fadeVelocity = 0.0f;
}

/**
 * Called from threads: thread5_game_loop
 */
static void seq_player_fade_to_percentage_of_volume(s32 player, FadeT fadeDuration, u8 percentage) {
    struct SequencePlayer *seqPlayer = &gSequencePlayers[player];
    f32 targetVolume;

    if (seqPlayer->state == SEQUENCE_PLAYER_STATE_FADE_OUT) {
        return;
    }

    targetVolume = (FLOAT_CAST(percentage) / EU_FLOAT(100.0)) * seqPlayer->fadeVolume;
    seqPlayer->volume = seqPlayer->fadeVolume;

    seqPlayer->fadeRemainingFrames = 0;
    if (fadeDuration == 0) {
        seqPlayer->fadeVolume = targetVolume;
        return;
    }
    seqPlayer->fadeVelocity = (targetVolume - seqPlayer->fadeVolume) / fadeDuration;
    seqPlayer->state = SEQUENCE_PLAYER_STATE_4;

    seqPlayer->fadeRemainingFrames = fadeDuration;
}

/**
 * Called from threads: thread3_main, thread4_sound, thread5_game_loop
 */
static void seq_player_fade_to_normal_volume(s32 player, FadeT fadeDuration) {
    struct SequencePlayer *seqPlayer = &gSequencePlayers[player];

    if (seqPlayer->state == SEQUENCE_PLAYER_STATE_FADE_OUT) {
        return;
    }

    seqPlayer->fadeRemainingFrames = 0;
    if (fadeDuration == 0) {
        seqPlayer->fadeVolume = seqPlayer->volume;
        return;
    }
    seqPlayer->fadeVelocity = (seqPlayer->volume - seqPlayer->fadeVolume) / fadeDuration;
    seqPlayer->state = SEQUENCE_PLAYER_STATE_2;

    seqPlayer->fadeRemainingFrames = fadeDuration;
}

/**
 * Called from threads: thread3_main, thread4_sound, thread5_game_loop
 */
static void seq_player_fade_to_target_volume(s32 player, FadeT fadeDuration, u8 targetVolume) {
    struct SequencePlayer *seqPlayer = &gSequencePlayers[player];

    if (seqPlayer->state == SEQUENCE_PLAYER_STATE_FADE_OUT) {
        return;
    }

    seqPlayer->fadeRemainingFrames = 0;
    if (fadeDuration == 0) {
        seqPlayer->fadeVolume = (FLOAT_CAST(targetVolume) / EU_FLOAT(127.0));
        return;
    }

    seqPlayer->fadeVelocity =
        (((f32) (FLOAT_CAST(targetVolume) / EU_FLOAT(127.0)) - seqPlayer->fadeVolume)
         / (f32) fadeDuration);
    seqPlayer->state = SEQUENCE_PLAYER_STATE_4;

    seqPlayer->fadeRemainingFrames = fadeDuration;
}

/**
 * Called from threads: thread4_sound
 */
struct SPTask *create_next_audio_frame_task(void) {
    u32 samplesRemainingInAI;
    s32 writtenCmds;
    s32 index;
    OSTask_t *task;
    s32 oldDmaCount;
    s32 flags;

    gAudioFrameCount++;
    if (gAudioLoadLock != AUDIO_LOCK_NOT_LOADING) {
        stubbed_printf("DAC:Lost 1 Frame.\n");
        return NULL;
    }

    gAudioTaskIndex ^= 1;
    gCurrAiBufferIndex++;
    gCurrAiBufferIndex %= NUMAIBUFFERS;
    index = (gCurrAiBufferIndex - 2 + NUMAIBUFFERS) % NUMAIBUFFERS;
    samplesRemainingInAI = osAiGetLength() / 4;

    // Audio is triple buffered; the audio interface reads from two buffers
    // while the third is being written by the RSP. More precisely, the
    // lifecycle is:
    // - this function computes an audio command list
    // - wait for vblank
    // - the command list is sent to the RSP (we could have sent it to the
    //   RSP before the vblank, but that gives the RSP less time to finish)
    // - wait for vblank
    // - the RSP is now expected to be finished, and we can send its output
    //   on to the AI
    // Here we thus send to the AI the sound that was generated two frames ago.
    if (gAiBufferLengths[index] != 0) {
        osAiSetNextBuffer(gAiBuffers[index], gAiBufferLengths[index] * 4);
    }

    oldDmaCount = gCurrAudioFrameDmaCount;
    // There has to be some sort of no-op if here, but it's not exactly clear
    // how it should look... It's also very unclear why gCurrAudioFrameDmaQueue
    // isn't read from here, despite gCurrAudioFrameDmaCount being reset.
    if (oldDmaCount > AUDIO_FRAME_DMA_QUEUE_SIZE) {
        stubbed_printf("DMA: Request queue over.( %d )\n", oldDmaCount);
    }
    gCurrAudioFrameDmaCount = 0;

    gAudioTask = &gAudioTasks[gAudioTaskIndex];
    gAudioCmd = gAudioCmdBuffers[gAudioTaskIndex];

    index = gCurrAiBufferIndex;
    gCurrAiBuffer = gAiBuffers[index];
    gAiBufferLengths[index] =
        ((gSamplesPerFrameTarget - samplesRemainingInAI + EXTRA_BUFFERED_AI_SAMPLES_TARGET) & ~0xf)
        + SAMPLES_TO_OVERPRODUCE;
    if (gAiBufferLengths[index] < gMinAiBufferLength) {
        gAiBufferLengths[index] = gMinAiBufferLength;
    }
    if (gAiBufferLengths[index] > gSamplesPerFrameTarget + SAMPLES_TO_OVERPRODUCE) {
        gAiBufferLengths[index] = gSamplesPerFrameTarget + SAMPLES_TO_OVERPRODUCE;
    }

    if (sGameLoopTicked != 0) {
        update_game_sound();
        sGameLoopTicked = 0;
    }

    // For the function to match we have to preserve some arbitrary variable
    // across this function call.
    flags = 0;
    gAudioCmd = synthesis_execute(gAudioCmd, &writtenCmds, gCurrAiBuffer, gAiBufferLengths[index]);
    gAudioRandom = ((gAudioRandom + gAudioFrameCount) * gAudioFrameCount);

    index = gAudioTaskIndex;
    gAudioTask->msgqueue = NULL;
    gAudioTask->msg = NULL;

    task = &gAudioTask->task.t;
    task->type = M_AUDTASK;
    task->flags = flags;
    task->ucode_boot = rspF3DBootStart;
    task->ucode_boot_size = (u8 *) rspF3DBootEnd - (u8 *) rspF3DBootStart;
    task->ucode = rspAspMainStart;
    task->ucode_size = 0x800; // (this size is ignored)
    task->ucode_data = rspAspMainDataStart;
    task->ucode_data_size = (rspAspMainDataEnd - rspAspMainDataStart) * sizeof(u64);
    task->dram_stack = NULL;
    task->dram_stack_size = 0;
    task->output_buff = NULL;
    task->output_buff_size = NULL;
    task->data_ptr = gAudioCmdBuffers[index];
    task->data_size = writtenCmds * sizeof(u64);

    // The audio task never yields, so having a yield buffer is pointless.
    // This wastefulness was fixed in US.
    task->yield_data_ptr = (u64 *) gAudioSPTaskYieldBuffer;
    task->yield_data_size = OS_YIELD_AUDIO_SIZE;

    decrease_sample_dma_ttls();
    return gAudioTask;
}

/**
 * Called from threads: thread5_game_loop
 */
void play_sound(s32 soundBits, f32 *pos) {
    sSoundRequests[sSoundRequestCount].soundBits = soundBits;
    sSoundRequests[sSoundRequestCount].position = pos;
    sSoundRequestCount++;
}

/**
 * Called from threads: thread4_sound, thread5_game_loop (EU only)
 */
static void process_sound_request(u32 bits, f32 *pos) {
    u8 bank;
    u8 soundIndex;
    u8 counter = 0;
    u8 soundId;
    f32 dist;
    const f32 one = 1.0f;

    bank = (bits & SOUNDARGS_MASK_BANK) >> SOUNDARGS_SHIFT_BANK;
    soundId = (bits & SOUNDARGS_MASK_SOUNDID) >> SOUNDARGS_SHIFT_SOUNDID;

    if (soundId >= sNumSoundsPerBank[bank] || sSoundBankDisabled[bank]) {
        return;
    }

    soundIndex = sSoundBanks[bank][0].next;
    while (soundIndex != 0xff && soundIndex != 0) {
        // If an existing sound from the same source exists in the bank, then we should either
        // interrupt that sound and replace it with the new sound, or we should drop the new sound.
        if (sSoundBanks[bank][soundIndex].x == pos) {
            // If the existing sound has lower or equal priority, then we should replace it.
            // Otherwise the new sound will be dropped.
            if ((sSoundBanks[bank][soundIndex].soundBits & SOUNDARGS_MASK_PRIORITY)
                <= (bits & SOUNDARGS_MASK_PRIORITY)) {

                // If the existing sound is discrete or is a different continuous sound, then
                // interrupt it and play the new sound instead.
                // Otherwise the new sound is continuous and equals the existing sound, so we just
                // need to update the sound's freshness.
                if ((sSoundBanks[bank][soundIndex].soundBits & SOUND_DISCRETE) != 0
                    || (bits & SOUNDARGS_MASK_SOUNDID)
                           != (sSoundBanks[bank][soundIndex].soundBits & SOUNDARGS_MASK_SOUNDID)) {
                    update_background_music_after_sound(bank, soundIndex);
                    sSoundBanks[bank][soundIndex].soundBits = bits;
                    // In practice, the starting status is always WAITING
                    sSoundBanks[bank][soundIndex].soundStatus = bits & SOUNDARGS_MASK_STATUS;
                }

                // Reset freshness:
                // - For discrete sounds, this gives the sound SOUND_MAX_FRESHNESS frames to play
                //   before it gets deleted for being stale
                // - For continuous sounds, this gives it another 2 frames before play_sound must
                //   be called again to keep it playing
                sSoundBanks[bank][soundIndex].freshness = SOUND_MAX_FRESHNESS;
            }

            // Prevent allocating a new node - if the existing sound had higher piority, then the
            // new sound will be dropped
            soundIndex = 0;
        } else {
            soundIndex = sSoundBanks[bank][soundIndex].next;
        }
        counter++;
    }

    if (counter == 0) {
        sSoundMovingSpeed[bank] = 32;
    }

    // If free list has more than one element remaining
    if (sSoundBanks[bank][sSoundBankFreeListFront[bank]].next != 0xff && soundIndex != 0) {
        // Allocate from free list
        soundIndex = sSoundBankFreeListFront[bank];

        dist = sqrtf(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]) * one;
        sSoundBanks[bank][soundIndex].x = &pos[0];
        sSoundBanks[bank][soundIndex].y = &pos[1];
        sSoundBanks[bank][soundIndex].z = &pos[2];
        sSoundBanks[bank][soundIndex].distance = dist;
        sSoundBanks[bank][soundIndex].soundBits = bits;
        // In practice, the starting status is always WAITING
        sSoundBanks[bank][soundIndex].soundStatus = bits & SOUNDARGS_MASK_STATUS;
        sSoundBanks[bank][soundIndex].freshness = SOUND_MAX_FRESHNESS;

        // Append to end of used list and pop from front of free list
        sSoundBanks[bank][soundIndex].prev = sSoundBankUsedListBack[bank];
        sSoundBanks[bank][sSoundBankUsedListBack[bank]].next = sSoundBankFreeListFront[bank];
        sSoundBankUsedListBack[bank] = sSoundBankFreeListFront[bank];
        sSoundBankFreeListFront[bank] = sSoundBanks[bank][sSoundBankFreeListFront[bank]].next;
        sSoundBanks[bank][sSoundBankFreeListFront[bank]].prev = 0xff;
        sSoundBanks[bank][soundIndex].next = 0xff;
    }
}

/**
 * Processes all sound requests
 *
 * Called from threads: thread4_sound, thread5_game_loop (EU only)
 */
static void process_all_sound_requests(void) {
    struct Sound *sound;

    while (sSoundRequestCount != sNumProcessedSoundRequests) {
        sound = &sSoundRequests[sNumProcessedSoundRequests];
        process_sound_request(sound->soundBits, sound->position);
        sNumProcessedSoundRequests++;
    }
}

/**
 * Called from threads: thread4_sound, thread5_game_loop (EU only)
 */
static void delete_sound_from_bank(u8 bank, u8 soundIndex) {
    if (sSoundBankUsedListBack[bank] == soundIndex) {
        // Remove from end of used list
        sSoundBankUsedListBack[bank] = sSoundBanks[bank][soundIndex].prev;
    } else {
        // Set sound.next.prev to sound.prev
        sSoundBanks[bank][sSoundBanks[bank][soundIndex].next].prev = sSoundBanks[bank][soundIndex].prev;
    }

    // Set sound.prev.next to sound.next
    sSoundBanks[bank][sSoundBanks[bank][soundIndex].prev].next = sSoundBanks[bank][soundIndex].next;

    // Push to front of free list
    sSoundBanks[bank][soundIndex].next = sSoundBankFreeListFront[bank];
    sSoundBanks[bank][soundIndex].prev = 0xff;
    sSoundBanks[bank][sSoundBankFreeListFront[bank]].prev = soundIndex;
    sSoundBankFreeListFront[bank] = soundIndex;
}

/**
 * Called from threads: thread3_main, thread4_sound, thread5_game_loop
 */
static void update_background_music_after_sound(u8 bank, u8 soundIndex) {
    if (sSoundBanks[bank][soundIndex].soundBits & SOUND_LOWER_BACKGROUND_MUSIC) {
        sSoundBanksThatLowerBackgroundMusic &= (1 << bank) ^ 0xffff;
        begin_background_music_fade(50);
    }
}

/**
 * Called from threads: thread4_sound, thread5_game_loop (EU only)
 */
static void select_current_sounds(u8 bank) {
    u32 isDiscreteAndStatus;
    u8 latestSoundIndex;
    u8 i;
    u8 j;
    u8 soundIndex;
    u32 liveSoundPriorities[16] = { 0x10000000, 0x10000000, 0x10000000, 0x10000000,
                                    0x10000000, 0x10000000, 0x10000000, 0x10000000,
                                    0x10000000, 0x10000000, 0x10000000, 0x10000000,
                                    0x10000000, 0x10000000, 0x10000000, 0x10000000 };
    u8 liveSoundIndices[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    u8 liveSoundStatuses[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    u8 numSoundsInBank = 0;
    u8 requestedPriority;

    //
    // Delete stale sounds and prioritize remaining sounds into the liveSound arrays
    //
    soundIndex = sSoundBanks[bank][0].next;
    while (soundIndex != 0xff) {
        latestSoundIndex = soundIndex;

        // If a discrete sound goes 10 frames without being played (because it is too low
        // priority), then mark it for deletion
        if ((sSoundBanks[bank][soundIndex].soundBits & (SOUND_DISCRETE | SOUNDARGS_MASK_STATUS))
            == (SOUND_DISCRETE | SOUND_STATUS_WAITING)) {
            if (sSoundBanks[bank][soundIndex].freshness-- == 0) {
                sSoundBanks[bank][soundIndex].soundBits = NO_SOUND;
            }
        }
        // If a continuous sound goes 2 frames without play_sound being called, then mark it for
        // deletion
        else if ((sSoundBanks[bank][soundIndex].soundBits & SOUND_DISCRETE) == 0) {
            if (sSoundBanks[bank][soundIndex].freshness-- == SOUND_MAX_FRESHNESS - 2) {
                update_background_music_after_sound(bank, soundIndex);
                sSoundBanks[bank][soundIndex].soundBits = NO_SOUND;
            }
        }

        // If a sound was marked for deletion and hasn't started playing yet, delete it now
        if (sSoundBanks[bank][soundIndex].soundBits == NO_SOUND
            && sSoundBanks[bank][soundIndex].soundStatus == SOUND_STATUS_WAITING) {
            // Since the current sound will be deleted, the next iteration should process
            // sound.prev.next
            latestSoundIndex = sSoundBanks[bank][soundIndex].prev;
            sSoundBanks[bank][soundIndex].soundStatus = SOUND_STATUS_STOPPED;
            delete_sound_from_bank(bank, soundIndex);
        }

        // If the current sound was not just deleted, consider it as a candidate for the currently
        // playing sound
        if (sSoundBanks[bank][soundIndex].soundStatus != SOUND_STATUS_STOPPED
            && soundIndex == latestSoundIndex) {

            // Recompute distance each frame since the sound's position may have changed
            sSoundBanks[bank][soundIndex].distance =
                sqrtf((*sSoundBanks[bank][soundIndex].x * *sSoundBanks[bank][soundIndex].x)
                      + (*sSoundBanks[bank][soundIndex].y * *sSoundBanks[bank][soundIndex].y)
                      + (*sSoundBanks[bank][soundIndex].z * *sSoundBanks[bank][soundIndex].z))
                * 1;

            requestedPriority = (sSoundBanks[bank][soundIndex].soundBits & SOUNDARGS_MASK_PRIORITY)
                                >> SOUNDARGS_SHIFT_PRIORITY;

            // Recompute priority, possibly based on the sound's source position relative to the
            // camera.
            // (Note that the sound's priority is the opposite of requestedPriority; lower is
            // more important)
            if (sSoundBanks[bank][soundIndex].soundBits & SOUND_NO_PRIORITY_LOSS) {
                sSoundBanks[bank][soundIndex].priority = 0x4c * (0xff - requestedPriority);
            } else if (*sSoundBanks[bank][soundIndex].z > 0.0f) {
                sSoundBanks[bank][soundIndex].priority =
                    (u32) sSoundBanks[bank][soundIndex].distance
                    + (u32) (*sSoundBanks[bank][soundIndex].z / US_FLOAT(6.0))
                    + 0x4c * (0xff - requestedPriority);
            } else {
                sSoundBanks[bank][soundIndex].priority =
                    (u32) sSoundBanks[bank][soundIndex].distance + 0x4c * (0xff - requestedPriority);
            }

            // Insert the sound into the liveSound arrays, keeping the arrays sorted by priority.
            // If more than sMaxChannelsForSoundBank[bank] sounds are live, then the
            // sound with lowest priority will be removed from the arrays.
            // In practice sMaxChannelsForSoundBank is always 1, so this code is overly general.
            for (i = 0; i < sMaxChannelsForSoundBank[bank]; i++) {
                // If the correct position is found
                if (liveSoundPriorities[i] >= sSoundBanks[bank][soundIndex].priority) {
                    // Shift remaining sounds to the right
                    for (j = sMaxChannelsForSoundBank[bank] - 1; j > i; j--) {
                        liveSoundPriorities[j] = liveSoundPriorities[j - 1];
                        liveSoundIndices[j] = liveSoundIndices[j - 1];
                        liveSoundStatuses[j] = liveSoundStatuses[j - 1];
                    }
                    // Insert the sound at index i
                    liveSoundPriorities[i] = sSoundBanks[bank][soundIndex].priority;
                    liveSoundIndices[i] = soundIndex;
                    liveSoundStatuses[i] = sSoundBanks[bank][soundIndex].soundStatus; // unused
                    // Break
                    i = sMaxChannelsForSoundBank[bank];
                }
            }

            numSoundsInBank++;
        }

        soundIndex = sSoundBanks[bank][latestSoundIndex].next;
    }

    sNumSoundsInBank[bank] = numSoundsInBank;
    sUsedChannelsForSoundBank[bank] = sMaxChannelsForSoundBank[bank];

    //
    // Remove any sounds from liveSoundIndices that are already playing.
    // Stop any currently playing sounds that are not in liveSoundIndices.
    //
    for (i = 0; i < sUsedChannelsForSoundBank[bank]; i++) {
        // Check if sCurrentSound[bank][i] is present in the liveSound arrays.
        for (soundIndex = 0; soundIndex < sUsedChannelsForSoundBank[bank]; soundIndex++) {
            if (liveSoundIndices[soundIndex] != 0xff
                && sCurrentSound[bank][i] == liveSoundIndices[soundIndex]) {
                // If found, remove it from liveSoundIndices
                liveSoundIndices[soundIndex] = 0xff;
                soundIndex = 0xfe; // Break. Afterward soundIndex will be 0xff
            }
        }

        // If it is not present in the liveSound arrays, then stop playing it
        if (soundIndex != 0xff) {
            if (sCurrentSound[bank][i] != 0xff) {
                // If the sound was marked for deletion and is playing, delete it
                if (sSoundBanks[bank][sCurrentSound[bank][i]].soundBits == NO_SOUND) {
                    if (sSoundBanks[bank][sCurrentSound[bank][i]].soundStatus == SOUND_STATUS_PLAYING) {
                        sSoundBanks[bank][sCurrentSound[bank][i]].soundStatus = SOUND_STATUS_STOPPED;
                        delete_sound_from_bank(bank, sCurrentSound[bank][i]);
                    }
                }

                // If the sound is discrete and is playing, then delete it
                isDiscreteAndStatus = sSoundBanks[bank][sCurrentSound[bank][i]].soundBits
                                      & (SOUND_DISCRETE | SOUNDARGS_MASK_STATUS);
                if (isDiscreteAndStatus >= (SOUND_DISCRETE | SOUND_STATUS_PLAYING)
                    && sSoundBanks[bank][sCurrentSound[bank][i]].soundStatus != SOUND_STATUS_STOPPED) {
                    //! @bug On JP, if a discrete sound that lowers the background music is
                    //  interrupted in this way, it will keep the background music low afterward.
                    //  There are only a few of these sounds, and it probably isn't possible to do
                    //  it in practice without using a time stop glitch like triple star spawn.

                    sSoundBanks[bank][sCurrentSound[bank][i]].soundBits = NO_SOUND;
                    sSoundBanks[bank][sCurrentSound[bank][i]].soundStatus = SOUND_STATUS_STOPPED;
                    delete_sound_from_bank(bank, sCurrentSound[bank][i]);
                }
                // If the sound is continuous and is playing, then stop playing it but don't delete
                // it. (A continuous sound shouldn't be deleted until it stops being requested)
                else {
                    if (isDiscreteAndStatus == SOUND_STATUS_PLAYING
                        && sSoundBanks[bank][sCurrentSound[bank][i]].soundStatus
                               != SOUND_STATUS_STOPPED) {
                        sSoundBanks[bank][sCurrentSound[bank][i]].soundStatus = SOUND_STATUS_WAITING;
                    }
                }
            }
            sCurrentSound[bank][i] = 0xff;
        }
    }

    //
    // Start playing the remaining sounds from liveSoundIndices.
    //
    for (soundIndex = 0; soundIndex < sUsedChannelsForSoundBank[bank]; soundIndex++) {
        if (liveSoundIndices[soundIndex] != 0xff) {
            for (i = 0; i < sUsedChannelsForSoundBank[bank]; i++) {
                if (sCurrentSound[bank][i] == 0xff) {
                    sCurrentSound[bank][i] = liveSoundIndices[soundIndex];

                    // Set (soundBits & status) to WAITING (soundStatus will be updated
                    // shortly after in update_game_sound)
                    sSoundBanks[bank][liveSoundIndices[soundIndex]].soundBits =
                        (sSoundBanks[bank][liveSoundIndices[soundIndex]].soundBits
                         & ~SOUNDARGS_MASK_STATUS)
                        + SOUND_STATUS_WAITING;

                    liveSoundIndices[i] = 0xff; // doesn't do anything
                    i = 0xfe;                   // break
                }
            }
        }
    }
}

/**
 * Given x and z coordinates, return the pan. This is a value nominally between
 * 0 and 1 that represents the audio direction.
 *
 * Pan:
 * 0.0 - fully left
 * 0.5 - center pan
 * 1.0 - fully right
 *
 * Called from threads: thread4_sound, thread5_game_loop (EU only)
 */
static f32 get_sound_pan(f32 x, f32 z) {
    f32 absX;
    f32 absZ;
    f32 pan;

    absX = (x < 0 ? -x : x);
    if (absX > AUDIO_MAX_DISTANCE) {
        absX = AUDIO_MAX_DISTANCE;
    }

    absZ = (z < 0 ? -z : z);
    if (absZ > AUDIO_MAX_DISTANCE) {
        absZ = AUDIO_MAX_DISTANCE;
    }

    // There are 4 panning equations (12-hr clock used for angles)
    // 1. (0,0) fully-centered pan
    // 2. far right pan: between 1:30 and 4:30
    // 3. far left pan: between 7:30 and 10:30
    // 4. center pan: between 4:30 and 7:30 or between 10:30 and 1:30
    if (x == US_FLOAT(0.0) && z == US_FLOAT(0.0)) {
        // x and z being 0 results in a center pan
        pan = US_FLOAT(0.5);
    } else if (x >= US_FLOAT(0.0) && absX >= absZ) {
        // far right pan
        pan = US_FLOAT(1.0)
              - (2 * AUDIO_MAX_DISTANCE - absX) / (US_FLOAT(3.0) * (2 * AUDIO_MAX_DISTANCE - absZ));
    } else if (x < 0 && absX > absZ) {
        // far left pan
        pan = (2 * AUDIO_MAX_DISTANCE - absX) / (US_FLOAT(3.0) * (2 * AUDIO_MAX_DISTANCE - absZ));
    } else {
        // center pan
        //! @bug (JP PU sound glitch) If |x|, |z| > AUDIO_MAX_DISTANCE, we'll
        // end up in this case, and pan may be set to something outside of [0,1]
        // since x is not clamped. On JP, this can lead to an out-of-bounds
        // float read in note_set_vel_pan_reverb when x is highly negative,
        // causing console crashes when that float is a nan or denormal.
        pan = 0.5 + x / (US_FLOAT(6.0) * absZ);
    }

    return pan;
}

/**
 * Called from threads: thread4_sound, thread5_game_loop (EU only)
 */
static f32 get_sound_volume(u8 bank, u8 soundIndex, f32 volumeRange) {
    f32 maxSoundDistance;
    f32 intensity;

    if (!(sSoundBanks[bank][soundIndex].soundBits & SOUND_NO_VOLUME_LOSS)) {
        // Intensity linearly lowers from 1 at the camera to 0 at maxSoundDistance
        maxSoundDistance = sLevelAcousticReaches[gCurrLevelNum];
        if (maxSoundDistance < sSoundBanks[bank][soundIndex].distance) {
            intensity = 0.0f;
        } else {
            intensity = 1.0 - sSoundBanks[bank][soundIndex].distance / maxSoundDistance;
        }

        if (sSoundBanks[bank][soundIndex].soundBits & SOUND_VIBRATO) {
            //! @bug Intensity is 0 when the sound is far away. Due to the subtraction below, it is
            //! possible to end up with a negative intensity.
            // When it is, objects with a volumeRange of 1 can still occasionally be lightly heard.
            if (intensity != 0.0) {
                intensity -= (f32) (gAudioRandom & 0xf) / US_FLOAT(192.0);
            }
        }
    } else {
        intensity = 1.0f;
    }

    // Rise quadratically from 1 - volumeRange to 1
    return volumeRange * intensity * intensity + 1.0f - volumeRange;
}

/**
 * Called from threads: thread4_sound, thread5_game_loop (EU only)
 */
static f32 get_sound_freq_scale(u8 bank, u8 item) {
    f32 amount;

    if (!(sSoundBanks[bank][item].soundBits & SOUND_CONSTANT_FREQUENCY)) {
        amount = sSoundBanks[bank][item].distance / AUDIO_MAX_DISTANCE;
        if (sSoundBanks[bank][item].soundBits & SOUND_VIBRATO) {
            amount += (f32) (gAudioRandom & 0xff) / US_FLOAT(64.0);
        }
    } else {
        amount = 0.0f;
    }

    // Goes from 1 at the camera to 1 + 1/15 at AUDIO_MAX_DISTANCE (and continues rising
    // farther than that)
    return amount / US_FLOAT(15.0) + US_FLOAT(1.0);
}

/**
 * Called from threads: thread4_sound, thread5_game_loop (EU only)
 */
static u8 get_sound_reverb(UNUSED u8 bank, UNUSED u8 soundIndex, u8 channelIndex) {
    u8 area;
    u8 level;
    u8 reverb;

    level = (gCurrLevelNum > LEVEL_MAX ? LEVEL_MAX : gCurrLevelNum);
    area = gCurrAreaIndex - 1;
    if (area > 2) {
        area = 2;
    }

    // reverb = reverb adjustment + level reverb + a volume-dependent value
    // The volume-dependent value is 0 when volume is at maximum, and raises to
    // LOW_VOLUME_REVERB when the volume is 0
    reverb = (u8) ((u8) gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->soundScriptIO[5]
                   + sLevelAreaReverbs[level][area]
                   + (US_FLOAT(1.0) - gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->volume)
                         * LOW_VOLUME_REVERB);

    if (reverb > 0x7f) {
        reverb = 0x7f;
    }
    return reverb;
}

static void noop_8031EEC8(void) {
}

/**
 * Called from the game loop thread to inform the audio thread that a new game
 * frame has started.
 *
 * Called from threads: thread5_game_loop
 */
void audio_signal_game_loop_tick(void) {
    sGameLoopTicked = 1;
    noop_8031EEC8();
}

/**
 * Called from threads: thread4_sound, thread5_game_loop (EU and SH only)
 */
static void update_game_sound(void) {
    u8 soundStatus;
    u8 i;
    u8 soundId;
    u8 bank;
    u8 channelIndex = 0;
    u8 soundIndex;
    f32 value;

    process_all_sound_requests();
    process_level_music_dynamics();

    if (gSequencePlayers[SEQ_PLAYER_SFX].channels[0] == &gSequenceChannelNone) {
        return;
    }

    for (bank = 0; bank < SOUND_BANK_COUNT; bank++) {
        select_current_sounds(bank);

        for (i = 0; i < MAX_CHANNELS_PER_SOUND_BANK; i++) {
            soundIndex = sCurrentSound[bank][i];

            if (soundIndex < 0xff
                && sSoundBanks[bank][soundIndex].soundStatus != SOUND_STATUS_STOPPED) {
                soundStatus = sSoundBanks[bank][soundIndex].soundBits & SOUNDARGS_MASK_STATUS;
                soundId = (sSoundBanks[bank][soundIndex].soundBits >> SOUNDARGS_SHIFT_SOUNDID);

                sSoundBanks[bank][soundIndex].soundStatus = soundStatus;

                if (soundStatus == SOUND_STATUS_WAITING) {
                    if (sSoundBanks[bank][soundIndex].soundBits & SOUND_LOWER_BACKGROUND_MUSIC) {
                        sSoundBanksThatLowerBackgroundMusic |= 1 << bank;
                        begin_background_music_fade(50);
                    }

                    // Set sound status to PLAYING
                    sSoundBanks[bank][soundIndex].soundBits++;
                    sSoundBanks[bank][soundIndex].soundStatus = SOUND_STATUS_PLAYING;

                    // Begin playing the sound
                    gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->soundScriptIO[4] = soundId;
                    gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->soundScriptIO[0] = 1;

                    switch (bank) {
                        case SOUND_BANK_MOVING:
                            if (!(sSoundBanks[bank][soundIndex].soundBits & SOUND_CONSTANT_FREQUENCY)) {
                                if (sSoundMovingSpeed[bank] > 8) {
                                    value = get_sound_volume(bank, soundIndex, VOLUME_RANGE_UNK1);
                                    gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->volume =
                                        value;
                                } else {
                                    value = get_sound_volume(bank, soundIndex, VOLUME_RANGE_UNK1);
                                    gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->volume =
                                        (sSoundMovingSpeed[bank] + 8.0f) / 16 * value;
                                }
                                gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->pan =
                                    get_sound_pan(*sSoundBanks[bank][soundIndex].x,
                                                  *sSoundBanks[bank][soundIndex].z);

                                if ((sSoundBanks[bank][soundIndex].soundBits & SOUNDARGS_MASK_SOUNDID)
                                    == (SOUND_MOVING_FLYING & SOUNDARGS_MASK_SOUNDID)) {
                                    value = get_sound_freq_scale(bank, soundIndex);
                                    gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->freqScale =
                                        ((f32) sSoundMovingSpeed[bank] / US_FLOAT(80.0)) + value;
                                } else {
                                    value = get_sound_freq_scale(bank, soundIndex);
                                    gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->freqScale =
                                        ((f32) sSoundMovingSpeed[bank] / US_FLOAT(400.0)) + value;
                                }
                                gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->reverbVol =
                                    get_sound_reverb(bank, soundIndex, channelIndex);

                                break;
                            }
                        // fallthrough
                        case SOUND_BANK_MENU:
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->volume = 1.0f;
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->pan = 0.5f;
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->freqScale = 1.0f;
                            break;
                        case SOUND_BANK_ACTION:
                        case SOUND_BANK_VOICE:
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->volume =
                                get_sound_volume(bank, soundIndex, VOLUME_RANGE_UNK1);
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->pan =
                                get_sound_pan(*sSoundBanks[bank][soundIndex].x,
                                              *sSoundBanks[bank][soundIndex].z);
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->freqScale =
                                get_sound_freq_scale(bank, soundIndex);
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->reverbVol =
                                get_sound_reverb(bank, soundIndex, channelIndex);
                            break;
                        case SOUND_BANK_GENERAL:
                        case SOUND_BANK_ENV:
                        case SOUND_BANK_OBJ:
                        case SOUND_BANK_AIR:
                        case SOUND_BANK_GENERAL2:
                        case SOUND_BANK_OBJ2:
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->reverbVol =
                                get_sound_reverb(bank, soundIndex, channelIndex);
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->volume =
                                get_sound_volume(bank, soundIndex, VOLUME_RANGE_UNK2);
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->pan =
                                get_sound_pan(*sSoundBanks[bank][soundIndex].x,
                                              *sSoundBanks[bank][soundIndex].z);
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->freqScale =
                                get_sound_freq_scale(bank, soundIndex);
                            break;
                    }
                }
                // If the sound was marked for deletion (bits set to NO_SOUND), then stop playing it
                // and delete it
                // @bug (JP double red coin sound) If the sound finished within the same frame as
                // being marked for deletion, the signal to stop playing will be interpreted as a
                // signal to *start* playing, as .main_loop_023589 in 00_sound_player does not check
                // for soundScriptIO[0] being zero. This happens most commonly for red coin sounds
                // whose sound spawners deactivate 30 frames after the sound starts to play, while
                // the sound itself runs for 1.20 seconds. With enough lag these may coincide.
                // Fixed on US by checking that layer0->finished is FALSE.
                else if (soundStatus == SOUND_STATUS_STOPPED) {
                    update_background_music_after_sound(bank, soundIndex);
                    gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->soundScriptIO[0] = 0;
                    delete_sound_from_bank(bank, soundIndex);
                }
                // If sound has finished playing, then delete it
                // @bug (JP sound glitch) On JP, ...->layers[0] has not been checked for null,
                // so this access can crash if an earlier layer allocation failed due to too
                // many sounds playing at once. This crash is comparatively common; RTA
                // speedrunners even have a setup for avoiding it within the SSL pyramid:
                // https://www.youtube.com/watch?v=QetyTgbQxcw
                else if (gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->layers[0]->enabled
                         == FALSE) {
                    update_background_music_after_sound(bank, soundIndex);
                    sSoundBanks[bank][soundIndex].soundStatus = SOUND_STATUS_STOPPED;
                    delete_sound_from_bank(bank, soundIndex);
                } else {
                    // Exactly the same code as before. Unfortunately we can't
                    // make a macro out of this, because then everything ends up
                    // on the same line after preprocessing, and the compiler,
                    // somehow caring about line numbers, makes it not match (it
                    // computes function arguments in the wrong order).
                    switch (bank) {
                        case SOUND_BANK_MOVING:
                            if (!(sSoundBanks[bank][soundIndex].soundBits & SOUND_CONSTANT_FREQUENCY)) {
                                if (sSoundMovingSpeed[bank] > 8) {
                                    value = get_sound_volume(bank, soundIndex, VOLUME_RANGE_UNK1);
                                    gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->volume =
                                        value;
                                } else {
                                    value = get_sound_volume(bank, soundIndex, VOLUME_RANGE_UNK1);
                                    gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->volume =
                                        (sSoundMovingSpeed[bank] + 8.0f) / 16 * value;
                                }
                                gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->pan =
                                    get_sound_pan(*sSoundBanks[bank][soundIndex].x,
                                                  *sSoundBanks[bank][soundIndex].z);

                                if ((sSoundBanks[bank][soundIndex].soundBits & SOUNDARGS_MASK_SOUNDID)
                                    == (SOUND_MOVING_FLYING & SOUNDARGS_MASK_SOUNDID)) {
                                    value = get_sound_freq_scale(bank, soundIndex);
                                    gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->freqScale =
                                        ((f32) sSoundMovingSpeed[bank] / US_FLOAT(80.0)) + value;
                                } else {
                                    value = get_sound_freq_scale(bank, soundIndex);
                                    gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->freqScale =
                                        ((f32) sSoundMovingSpeed[bank] / US_FLOAT(400.0)) + value;
                                }
                                gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->reverbVol =
                                    get_sound_reverb(bank, soundIndex, channelIndex);

                                break;
                            }
                        // fallthrough
                        case SOUND_BANK_MENU:
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->volume = 1.0f;
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->pan = 0.5f;
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->freqScale = 1.0f;
                            break;
                        case SOUND_BANK_ACTION:
                        case SOUND_BANK_VOICE:
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->volume =
                                get_sound_volume(bank, soundIndex, VOLUME_RANGE_UNK1);
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->pan =
                                get_sound_pan(*sSoundBanks[bank][soundIndex].x,
                                              *sSoundBanks[bank][soundIndex].z);
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->freqScale =
                                get_sound_freq_scale(bank, soundIndex);
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->reverbVol =
                                get_sound_reverb(bank, soundIndex, channelIndex);
                            break;
                        case SOUND_BANK_GENERAL:
                        case SOUND_BANK_ENV:
                        case SOUND_BANK_OBJ:
                        case SOUND_BANK_AIR:
                        case SOUND_BANK_GENERAL2:
                        case SOUND_BANK_OBJ2:
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->reverbVol =
                                get_sound_reverb(bank, soundIndex, channelIndex);
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->volume =
                                get_sound_volume(bank, soundIndex, VOLUME_RANGE_UNK2);
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->pan =
                                get_sound_pan(*sSoundBanks[bank][soundIndex].x,
                                              *sSoundBanks[bank][soundIndex].z);
                            gSequencePlayers[SEQ_PLAYER_SFX].channels[channelIndex]->freqScale =
                                get_sound_freq_scale(bank, soundIndex);
                            break;
                    }
                }
            }

            // Increment to the next channel that this bank owns
            channelIndex++;
        }

        // Increment to the first channel index of the next bank
        // (In practice sUsedChannelsForSoundBank[i] = sMaxChannelsForSoundBank[i] = 1, so this
        // doesn't do anything)
        channelIndex += sMaxChannelsForSoundBank[bank] - sUsedChannelsForSoundBank[bank];
    }
}

/**
 * Called from threads: thread4_sound, thread5_game_loop
 */
static void seq_player_play_sequence(u8 player, u8 seqId, u16 arg2) {
    u8 targetVolume;
    u8 i;

    if (player == SEQ_PLAYER_LEVEL) {
        sCurrentBackgroundMusicSeqId = seqId & SEQ_BASE_ID;
        sBackgroundMusicForDynamics = SEQUENCE_NONE;
        sCurrentMusicDynamic = 0xff;
        sMusicDynamicDelay = 2;
    }

    for (i = 0; i < CHANNELS_MAX; i++) {
        D_80360928[player][i].remainingFrames = 0;
    }

    gSequencePlayers[player].seqVariation = seqId & SEQ_VARIATION;
    load_sequence(player, seqId & SEQ_BASE_ID, 0);

    if (player == SEQ_PLAYER_LEVEL) {
        targetVolume = begin_background_music_fade(0);
        if (targetVolume != 0xff) {
            gSequencePlayers[SEQ_PLAYER_LEVEL].state = SEQUENCE_PLAYER_STATE_4;
            gSequencePlayers[SEQ_PLAYER_LEVEL].fadeVolume = (f32) targetVolume / US_FLOAT(127.0);
        }
    } else {
        func_8031D690(player, arg2);
    }
}

/**
 * Called from threads: thread5_game_loop
 */
void seq_player_fade_out(u8 player, u16 fadeDuration) {
    if (player == SEQ_PLAYER_LEVEL) {
        sCurrentBackgroundMusicSeqId = SEQUENCE_NONE;
    }
    seq_player_fade_to_zero_volume(player, fadeDuration);
}

/**
 * Called from threads: thread5_game_loop
 */
void fade_volume_scale(u8 player, u8 targetScale, u16 fadeDuration) {
    u8 i;
    for (i = 0; i < CHANNELS_MAX; i++) {
        fade_channel_volume_scale(player, i, targetScale, fadeDuration);
    }
}

/**
 * Called from threads: thread3_main, thread4_sound, thread5_game_loop
 */
static void fade_channel_volume_scale(u8 player, u8 channelIndex, u8 targetScale, u16 fadeDuration) {
    struct ChannelVolumeScaleFade *temp;

    if (gSequencePlayers[player].channels[channelIndex] != &gSequenceChannelNone) {
        temp = &D_80360928[player][channelIndex];
        temp->remainingFrames = fadeDuration;
        temp->velocity = ((f32) (targetScale / US_FLOAT(127.0))
                          - gSequencePlayers[player].channels[channelIndex]->volumeScale)
                         / fadeDuration;
        temp->target = targetScale;
        temp->current = gSequencePlayers[player].channels[channelIndex]->volumeScale;
    }
}

/**
 * Called from threads: thread4_sound, thread5_game_loop (EU only)
 */
static void func_8031F96C(u8 player) {
    u8 i;

    // Loop over channels
    for (i = 0; i < CHANNELS_MAX; i++) {
        if (gSequencePlayers[player].channels[i] != &gSequenceChannelNone
            && D_80360928[player][i].remainingFrames != 0) {
            D_80360928[player][i].current += D_80360928[player][i].velocity;
            gSequencePlayers[player].channels[i]->volumeScale = D_80360928[player][i].current;
            D_80360928[player][i].remainingFrames--;
            if (D_80360928[player][i].remainingFrames == 0) {
                gSequencePlayers[player].channels[i]->volumeScale =
                    D_80360928[player][i].target / 127.0f;
            }
        }
    }
}

/**
 * Called from threads: thread4_sound, thread5_game_loop (EU only)
 */
void process_level_music_dynamics(void) {
    u32 conditionBits;
    u16 tempBits;
    UNUSED u16 pad;
    u8 musicDynIndex;
    u8 condIndex;
    u8 i;
    u8 j;
    s16 conditionValues[8];
    u8 conditionTypes[8];
    s16 dur1;
    s16 dur2;
    u16 bit;

    func_8031F96C(0);
    func_8031F96C(2);
    func_80320ED8();
    if (sMusicDynamicDelay != 0) {
        sMusicDynamicDelay--;
    } else {
        sBackgroundMusicForDynamics = sCurrentBackgroundMusicSeqId;
    }

    if (sBackgroundMusicForDynamics != sLevelDynamics[gCurrLevelNum][0]) {
        return;
    }

    conditionBits = sLevelDynamics[gCurrLevelNum][1] & 0xff00;
    musicDynIndex = (u8) sLevelDynamics[gCurrLevelNum][1] & 0xff;
    i = 2;
    while (conditionBits & 0xff00) {
        j = 0;
        condIndex = 0;
        bit = 0x8000;
        while (j < 8) {
            if (conditionBits & bit) {
                conditionValues[condIndex] = sLevelDynamics[gCurrLevelNum][i++];
                conditionTypes[condIndex] = j;
                condIndex++;
            }

            j++;
            bit = bit >> 1;
        }

        for (j = 0; j < condIndex; j++) {
            switch (conditionTypes[j]) {
                case MARIO_X_GE: {
                    if (((s16) gMarioStates[0].pos[0]) < conditionValues[j]) {
                        j = condIndex + 1;
                    }
                    break;
                }
                case MARIO_Y_GE: {
                    if (((s16) gMarioStates[0].pos[1]) < conditionValues[j]) {
                        j = condIndex + 1;
                    }
                    break;
                }
                case MARIO_Z_GE: {
                    if (((s16) gMarioStates[0].pos[2]) < conditionValues[j]) {
                        j = condIndex + 1;
                    }
                    break;
                }
                case MARIO_X_LT: {
                    if (((s16) gMarioStates[0].pos[0]) >= conditionValues[j]) {
                        j = condIndex + 1;
                    }
                    break;
                }
                case MARIO_Y_LT: {
                    if (((s16) gMarioStates[0].pos[1]) >= conditionValues[j]) {
                        j = condIndex + 1;
                    }
                    break;
                }
                case MARIO_Z_LT: {
                    if (((s16) gMarioStates[0].pos[2]) >= conditionValues[j]) {
                        j = condIndex + 1;
                    }
                    break;
                }
                case MARIO_IS_IN_AREA: {
                    if (gCurrAreaIndex != conditionValues[j]) {
                        j = condIndex + 1;
                    }
                    break;
                }
                case MARIO_IS_IN_ROOM: {
                    if (gMarioCurrentRoom != conditionValues[j]) {
                        j = condIndex + 1;
                    }
                    break;
                }
            }
        }

        if (j == condIndex) {
            // The area matches. Break out of the loop.
            tempBits = 0;
        } else {
            tempBits = sLevelDynamics[gCurrLevelNum][i] & 0xff00;
            musicDynIndex = sLevelDynamics[gCurrLevelNum][i] & 0xff;
            i++;
        }

        conditionBits = tempBits;
    }

    if (sCurrentMusicDynamic != musicDynIndex) {
        tempBits = 1;
        if (sCurrentMusicDynamic == 0xff) {
            dur1 = 1;
            dur2 = 1;
        } else {
            dur1 = sMusicDynamics[musicDynIndex].dur1;
            dur2 = sMusicDynamics[musicDynIndex].dur2;
        }

        for (i = 0; i < CHANNELS_MAX; i++) {
            conditionBits = tempBits;
            tempBits = 0;
            if (sMusicDynamics[musicDynIndex].bits1 & conditionBits) {
                fade_channel_volume_scale(SEQ_PLAYER_LEVEL, i, sMusicDynamics[musicDynIndex].volScale1,
                                          dur1);
            }
            if (sMusicDynamics[musicDynIndex].bits2 & conditionBits) {
                fade_channel_volume_scale(SEQ_PLAYER_LEVEL, i, sMusicDynamics[musicDynIndex].volScale2,
                                          dur2);
            }
            tempBits = conditionBits << 1;
        }

        sCurrentMusicDynamic = musicDynIndex;
    }
}

void unused_8031FED0(u8 player, u32 bits, s8 arg2) {
    u8 i;

    if (arg2 < 0) {
        arg2 = -arg2;
    }

    for (i = 0; i < CHANNELS_MAX; i++) {
        if (gSequencePlayers[player].channels[i] != &gSequenceChannelNone) {
            if ((bits & 3) == 0) {
                gSequencePlayers[player].channels[i]->volumeScale = 1.0f;
            } else if ((bits & 1) != 0) {
                gSequencePlayers[player].channels[i]->volumeScale = (f32) arg2 / US_FLOAT(127.0);
            } else {
                gSequencePlayers[player].channels[i]->volumeScale =
                    US_FLOAT(1.0) - (f32) arg2 / US_FLOAT(127.0);
            }
        }
        bits >>= 2;
    }
}

/**
 * Lower a sequence player's volume over fadeDuration frames.
 * If player is SEQ_PLAYER_LEVEL (background music), the given percentage is ignored
 * and a max target volume of 40 is used.
 *
 * Called from threads: thread5_game_loop
 */
void seq_player_lower_volume(u8 player, u16 fadeDuration, u8 percentage) {
    if (player == SEQ_PLAYER_LEVEL) {
        sLowerBackgroundMusicVolume = TRUE;
        begin_background_music_fade(fadeDuration);
    } else if (gSequencePlayers[player].enabled == TRUE) {
        seq_player_fade_to_percentage_of_volume(player, fadeDuration, percentage);
    }
}

/**
 * Remove the lowered volume constraint set by seq_player_lower_volume.
 * If player is SEQ_PLAYER_LEVEL (background music), the music won't necessarily
 * raise back to normal volume if other constraints have been set, e.g.
 * sBackgroundMusicTargetVolume.
 *
 * Called from threads: thread5_game_loop
 */
void seq_player_unlower_volume(u8 player, u16 fadeDuration) {
    sLowerBackgroundMusicVolume = FALSE;
    if (player == SEQ_PLAYER_LEVEL) {
        if (gSequencePlayers[player].state != SEQUENCE_PLAYER_STATE_FADE_OUT) {
            begin_background_music_fade(fadeDuration);
        }
    } else {
        if (gSequencePlayers[player].enabled == TRUE) {
            seq_player_fade_to_normal_volume(player, fadeDuration);
        }
    }
}

/**
 * Begin a volume fade to adjust the background music to the correct volume.
 * The target volume is determined by global variables like sBackgroundMusicTargetVolume
 * and sLowerBackgroundMusicVolume.
 * If none of the relevant global variables are set, then the default background music
 * volume for the sequence is used.
 *
 * Called from threads: thread3_main, thread4_sound, thread5_game_loop
 */
static u8 begin_background_music_fade(u16 fadeDuration) {
    u8 targetVolume = 0xff;

    if (sCurrentBackgroundMusicSeqId == SEQUENCE_NONE
        || sCurrentBackgroundMusicSeqId == SEQ_EVENT_CUTSCENE_CREDITS) {
        return 0xff;
    }

    if (gSequencePlayers[SEQ_PLAYER_LEVEL].volume == 0.0f && fadeDuration) {
        gSequencePlayers[SEQ_PLAYER_LEVEL].volume = gSequencePlayers[SEQ_PLAYER_LEVEL].fadeVolume;
    }

    if (sBackgroundMusicTargetVolume != TARGET_VOLUME_UNSET) {
        targetVolume = (sBackgroundMusicTargetVolume & TARGET_VOLUME_VALUE_MASK);
    }

    if (sBackgroundMusicMaxTargetVolume != TARGET_VOLUME_UNSET) {
        u8 maxTargetVolume = (sBackgroundMusicMaxTargetVolume & TARGET_VOLUME_VALUE_MASK);
        if (targetVolume > maxTargetVolume) {
            targetVolume = maxTargetVolume;
        }
    }

    if (sLowerBackgroundMusicVolume && targetVolume > 40) {
        targetVolume = 40;
    }

    if (sSoundBanksThatLowerBackgroundMusic != 0 && targetVolume > 20) {
        targetVolume = 20;
    }

    if (gSequencePlayers[SEQ_PLAYER_LEVEL].enabled == TRUE) {
        if (targetVolume != 0xff) {
            seq_player_fade_to_target_volume(SEQ_PLAYER_LEVEL, fadeDuration, targetVolume);
        } else {
            gSequencePlayers[SEQ_PLAYER_LEVEL].volume =
                sBackgroundMusicDefaultVolume[sCurrentBackgroundMusicSeqId] / 127.0f;
            seq_player_fade_to_normal_volume(SEQ_PLAYER_LEVEL, fadeDuration);
        }
    }

    return targetVolume;
}

/**
 * Called from threads: thread5_game_loop
 */
void set_audio_muted(u8 muted) {
    u8 i;

    for (i = 0; i < SEQUENCE_PLAYERS; i++) {
        gSequencePlayers[i].muted = muted;
    }
}

/**
 * Called from threads: thread4_sound
 */
void sound_init(void) {
    u8 i;
    u8 j;

    for (i = 0; i < SOUND_BANK_COUNT; i++) {
        // Set each sound in the bank to STOPPED
        for (j = 0; j < 40; j++) {
            sSoundBanks[i][j].soundStatus = SOUND_STATUS_STOPPED;
        }

        // Remove current sounds
        for (j = 0; j < MAX_CHANNELS_PER_SOUND_BANK; j++) {
            sCurrentSound[i][j] = 0xff;
        }

        sSoundBankUsedListBack[i] = 0;
        sSoundBankFreeListFront[i] = 1;
        sNumSoundsInBank[i] = 0;
    }

    for (i = 0; i < SOUND_BANK_COUNT; i++) {
        // Set used list to empty
        sSoundBanks[i][0].prev = 0xff;
        sSoundBanks[i][0].next = 0xff;

        // Set free list to contain every sound slot
        for (j = 1; j < 40 - 1; j++) {
            sSoundBanks[i][j].prev = j - 1;
            sSoundBanks[i][j].next = j + 1;
        }
        sSoundBanks[i][j].prev = j - 1;
        sSoundBanks[i][j].next = 0xff;
    }

    for (j = 0; j < 3; j++) {
        for (i = 0; i < CHANNELS_MAX; i++) {
            D_80360928[j][i].remainingFrames = 0;
        }
    }

    for (i = 0; i < MAX_BACKGROUND_MUSIC_QUEUE_SIZE; i++) {
        sBackgroundMusicQueue[i].priority = 0;
    }

    sound_banks_enable(SEQ_PLAYER_SFX, SOUND_BANKS_ALL_BITS);

    sUnused80332118 = 0;
    sBackgroundMusicTargetVolume = TARGET_VOLUME_UNSET;
    sLowerBackgroundMusicVolume = FALSE;
    sSoundBanksThatLowerBackgroundMusic = 0;
    sUnused80332114 = 0;
    sCurrentBackgroundMusicSeqId = 0xff;
    gSoundMode = SOUND_MODE_STEREO;
    sBackgroundMusicQueueSize = 0;
    sBackgroundMusicMaxTargetVolume = TARGET_VOLUME_UNSET;
    D_80332120 = 0;
    D_80332124 = 0;
    sNumProcessedSoundRequests = 0;
    sSoundRequestCount = 0;
}

// (unused)
void get_currently_playing_sound(u8 bank, u8 *numPlayingSounds, u8 *numSoundsInBank, u8 *soundId) {
    u8 i;
    u8 count = 0;

    for (i = 0; i < sMaxChannelsForSoundBank[bank]; i++) {
        if (sCurrentSound[bank][i] != 0xff) {
            count++;
        }
    }
    *numPlayingSounds = count;

    *numSoundsInBank = sNumSoundsInBank[bank];

    if (sCurrentSound[bank][0] != 0xff) {
        *soundId =
            (u8) (sSoundBanks[bank][sCurrentSound[bank][0]].soundBits >> SOUNDARGS_SHIFT_SOUNDID);
    } else {
        *soundId = 0xff;
    }
}

/**
 * Called from threads: thread5_game_loop
 */
void stop_sound(u32 soundBits, f32 *pos) {
    u8 bank = (soundBits & SOUNDARGS_MASK_BANK) >> SOUNDARGS_SHIFT_BANK;
    u8 soundIndex = sSoundBanks[bank][0].next;

    while (soundIndex != 0xff) {
        // If sound has same id and source position pointer
        if ((u16) (soundBits >> SOUNDARGS_SHIFT_SOUNDID)
                == (u16) (sSoundBanks[bank][soundIndex].soundBits >> SOUNDARGS_SHIFT_SOUNDID)
            && sSoundBanks[bank][soundIndex].x == pos) {

            // Mark sound for deletion
            update_background_music_after_sound(bank, soundIndex);
            sSoundBanks[bank][soundIndex].soundBits = NO_SOUND;
            soundIndex = 0xff; // break
        } else {
            soundIndex = sSoundBanks[bank][soundIndex].next;
        }
    }
}

/**
 * Called from threads: thread5_game_loop
 */
void stop_sounds_from_source(f32 *pos) {
    u8 bank;
    u8 soundIndex;

    for (bank = 0; bank < SOUND_BANK_COUNT; bank++) {
        soundIndex = sSoundBanks[bank][0].next;
        while (soundIndex != 0xff) {
            if (sSoundBanks[bank][soundIndex].x == pos) {
                update_background_music_after_sound(bank, soundIndex);
                sSoundBanks[bank][soundIndex].soundBits = NO_SOUND;
            }
            soundIndex = sSoundBanks[bank][soundIndex].next;
        }
    }
}

/**
 * Called from threads: thread3_main, thread5_game_loop
 */
static void stop_sounds_in_bank(u8 bank) {
    u8 soundIndex = sSoundBanks[bank][0].next;

    while (soundIndex != 0xff) {
        update_background_music_after_sound(bank, soundIndex);
        sSoundBanks[bank][soundIndex].soundBits = NO_SOUND;
        soundIndex = sSoundBanks[bank][soundIndex].next;
    }
}

/**
 * Stops sounds in all of the sound banks that predominantly consist of continuous
 * sounds. Misses some specific continuous sounds in other banks like bird chirping
 * and the ticking sound after pressing a switch.
 *
 * Called from threads: thread3_main, thread5_game_loop
 */
void stop_sounds_in_continuous_banks(void) {
    stop_sounds_in_bank(SOUND_BANK_MOVING);
    stop_sounds_in_bank(SOUND_BANK_ENV);
    stop_sounds_in_bank(SOUND_BANK_AIR);
    stop_sounds_in_bank(SOUND_BANK_MENU);
}

/**
 * Called from threads: thread3_main, thread5_game_loop
 */
void sound_banks_disable(UNUSED u8 player, u16 bankMask) {
    u8 i;

    for (i = 0; i < SOUND_BANK_COUNT; i++) {
        if (bankMask & 1) {
            sSoundBankDisabled[i] = TRUE;
        }
        bankMask = bankMask >> 1;
    }
}

/**
 * Called from threads: thread5_game_loop
 */
void sound_banks_enable(UNUSED u8 player, u16 bankMask) {
    u8 i;

    for (i = 0; i < SOUND_BANK_COUNT; i++) {
        if (bankMask & 1) {
            sSoundBankDisabled[i] = FALSE;
        }
        bankMask = bankMask >> 1;
    }
}

u8 unused_803209D8(u8 player, u8 channelIndex, u8 arg2) {
    u8 ret = 0;
    if (gSequencePlayers[player].channels[channelIndex] != &gSequenceChannelNone) {
        gSequencePlayers[player].channels[channelIndex]->stopSomething2 = arg2;
        ret = arg2;
    }
    return ret;
}

/**
 * Set the moving speed for a sound bank, which may affect the volume and pitch
 * of the sound.
 *
 * Called from threads: thread5_game_loop
 */
void set_sound_moving_speed(u8 bank, u8 speed) {
    sSoundMovingSpeed[bank] = speed;
}

/**
 * Called from threads: thread5_game_loop
 */
void play_music(u8 player, u16 seqArgs, u16 fadeTimer) {
    u8 seqId = seqArgs & 0xff;
    u8 priority = seqArgs >> 8;
    u8 i;
    u8 foundIndex = 0;

    // Except for the background music player, we don't support queued
    // sequences. Just play them immediately, stopping any old sequence.
    if (player != SEQ_PLAYER_LEVEL) {
        seq_player_play_sequence(player, seqId, fadeTimer);
        return;
    }

    // Abort if the queue is already full.
    if (sBackgroundMusicQueueSize == MAX_BACKGROUND_MUSIC_QUEUE_SIZE) {
        return;
    }

    // If already in the queue, abort, after first restarting the sequence if
    // it is first, and handling disabled music somehow.
    // (That handling probably ought to occur even when the queue is full...)
    for (i = 0; i < sBackgroundMusicQueueSize; i++) {
        if (sBackgroundMusicQueue[i].seqId == seqId) {
            if (i == 0) {
                seq_player_play_sequence(SEQ_PLAYER_LEVEL, seqId, fadeTimer);
            } else if (!gSequencePlayers[SEQ_PLAYER_LEVEL].enabled) {
                stop_background_music(sBackgroundMusicQueue[0].seqId);
            }
            return;
        }
    }

    // Find the next sequence slot by priority.
    for (i = 0; i < sBackgroundMusicQueueSize; i++) {
        if (sBackgroundMusicQueue[i].priority <= priority) {
            foundIndex = i;
            i = sBackgroundMusicQueueSize; // break
        }
    }

    // If the sequence ends up first in the queue, start it, and make space for
    // one more entry in the queue.
    if (foundIndex == 0) {
        seq_player_play_sequence(SEQ_PLAYER_LEVEL, seqId, fadeTimer);
        sBackgroundMusicQueueSize++;
    }

    // Move all items up in queue, throwing away the last one if we didn't put
    // the new sequence first.
    for (i = sBackgroundMusicQueueSize - 1; i > foundIndex; i--) {
        sBackgroundMusicQueue[i].priority = sBackgroundMusicQueue[i - 1].priority;
        sBackgroundMusicQueue[i].seqId = sBackgroundMusicQueue[i - 1].seqId;
    }

    // Insert item into queue.
    sBackgroundMusicQueue[foundIndex].priority = priority;
    sBackgroundMusicQueue[foundIndex].seqId = seqId;
}

/**
 * Called from threads: thread5_game_loop
 */
void stop_background_music(u16 seqId) {
    u8 foundIndex;
    u8 i;

    if (sBackgroundMusicQueueSize == 0) {
        return;
    }

    // If sequence is not found, remove an empty queue item (the next empty
    // queue slot).
    foundIndex = sBackgroundMusicQueueSize;

    // Search for the sequence.
    for (i = 0; i < sBackgroundMusicQueueSize; i++) {
        if (sBackgroundMusicQueue[i].seqId == (u8) (seqId & 0xff)) {
            // Remove sequence from queue. If it was first, play the next one,
            // or fade out the music.
            sBackgroundMusicQueueSize--;
            if (i == 0) {
                if (sBackgroundMusicQueueSize != 0) {
                    seq_player_play_sequence(SEQ_PLAYER_LEVEL, sBackgroundMusicQueue[1].seqId, 0);
                } else {
                    seq_player_fade_out(SEQ_PLAYER_LEVEL, 20);
                }
            }
            foundIndex = i;
            i = sBackgroundMusicQueueSize; // "break;"
        }
    }

    // Move later slots down.
    for (i = foundIndex; i < sBackgroundMusicQueueSize; i++) {
        sBackgroundMusicQueue[i].priority = sBackgroundMusicQueue[i + 1].priority;
        sBackgroundMusicQueue[i].seqId = sBackgroundMusicQueue[i + 1].seqId;
    }

    // @bug? If the sequence queue is full and we attempt to stop a sequence
    // that isn't in the queue, this writes out of bounds. Can that happen?
    sBackgroundMusicQueue[i].priority = 0;
}

/**
 * Called from threads: thread5_game_loop
 */
void fadeout_background_music(u16 seqId, u16 fadeOut) {
    if (sBackgroundMusicQueueSize != 0 && sBackgroundMusicQueue[0].seqId == (u8) (seqId & 0xff)) {
        seq_player_fade_out(SEQ_PLAYER_LEVEL, fadeOut);
    }
}

/**
 * Called from threads: thread5_game_loop
 */
void drop_queued_background_music(void) {
    if (sBackgroundMusicQueueSize != 0) {
        sBackgroundMusicQueueSize = 1;
    }
}

/**
 * Called from threads: thread5_game_loop
 */
u16 get_current_background_music(void) {
    if (sBackgroundMusicQueueSize != 0) {
        return (sBackgroundMusicQueue[0].priority << 8) + sBackgroundMusicQueue[0].seqId;
    }
    return -1;
}

/**
 * Called from threads: thread4_sound, thread5_game_loop (EU only)
 */
void func_80320ED8(void) {
    if (gSequencePlayers[SEQ_PLAYER_ENV].enabled
        || sBackgroundMusicMaxTargetVolume == TARGET_VOLUME_UNSET) {
        return;
    }

    sBackgroundMusicMaxTargetVolume = TARGET_VOLUME_UNSET;
    begin_background_music_fade(50);

    if (sBackgroundMusicTargetVolume != TARGET_VOLUME_UNSET
        && (D_80332120 == SEQ_EVENT_MERRY_GO_ROUND || D_80332120 == SEQ_EVENT_PIRANHA_PLANT)) {
        seq_player_play_sequence(SEQ_PLAYER_ENV, D_80332120, 1);
        if (D_80332124 != 0xff) {
            seq_player_fade_to_target_volume(SEQ_PLAYER_ENV, 1, D_80332124);
        }
    }
}

/**
 * Called from threads: thread5_game_loop
 */
void play_secondary_music(u8 seqId, u8 bgMusicVolume, u8 volume, u16 fadeTimer) {
    UNUSED u32 dummy;

    sUnused80332118 = 0;
    if (sCurrentBackgroundMusicSeqId == 0xff || sCurrentBackgroundMusicSeqId == SEQ_MENU_TITLE_SCREEN) {
        return;
    }

    if (sBackgroundMusicTargetVolume == TARGET_VOLUME_UNSET) {
        sBackgroundMusicTargetVolume = bgMusicVolume + TARGET_VOLUME_IS_PRESENT_FLAG;
        begin_background_music_fade(fadeTimer);
        seq_player_play_sequence(SEQ_PLAYER_ENV, seqId, fadeTimer >> 1);
        if (volume < 0x80) {
            seq_player_fade_to_target_volume(SEQ_PLAYER_ENV, fadeTimer, volume);
        }
        D_80332124 = volume;
        D_80332120 = seqId;
    } else if (volume != 0xff) {
        sBackgroundMusicTargetVolume = bgMusicVolume + TARGET_VOLUME_IS_PRESENT_FLAG;
        begin_background_music_fade(fadeTimer);
        seq_player_fade_to_target_volume(SEQ_PLAYER_ENV, fadeTimer, volume);
        D_80332124 = volume;
    }
}

/**
 * Called from threads: thread5_game_loop
 */
void func_80321080(u16 fadeTimer) {
    if (sBackgroundMusicTargetVolume != TARGET_VOLUME_UNSET) {
        sBackgroundMusicTargetVolume = TARGET_VOLUME_UNSET;
        D_80332120 = 0;
        D_80332124 = 0;
        begin_background_music_fade(fadeTimer);
        seq_player_fade_out(SEQ_PLAYER_ENV, fadeTimer);
    }
}

/**
 * Called from threads: thread3_main, thread5_game_loop
 */
void func_803210D4(u16 fadeDuration) {
    u8 i;

    if (sHasStartedFadeOut) {
        return;
    }

    if (gSequencePlayers[SEQ_PLAYER_LEVEL].enabled == TRUE) {
        seq_player_fade_to_zero_volume(SEQ_PLAYER_LEVEL, fadeDuration);
    }

    if (gSequencePlayers[SEQ_PLAYER_ENV].enabled == TRUE) {
        seq_player_fade_to_zero_volume(SEQ_PLAYER_ENV, fadeDuration);
    }

    for (i = 0; i < SOUND_BANK_COUNT; i++) {
        if (i != SOUND_BANK_MENU) {
            fade_channel_volume_scale(SEQ_PLAYER_SFX, i, 0, fadeDuration / 16);
        }
    }

    sHasStartedFadeOut = TRUE;
}

/**
 * Called from threads: thread5_game_loop
 */
void play_course_clear(void) {
    seq_player_play_sequence(SEQ_PLAYER_ENV, SEQ_EVENT_CUTSCENE_COLLECT_STAR, 0);
    sBackgroundMusicMaxTargetVolume = TARGET_VOLUME_IS_PRESENT_FLAG | 0;
    begin_background_music_fade(50);
}

/**
 * Called from threads: thread5_game_loop
 */
void play_peachs_jingle(void) {
    seq_player_play_sequence(SEQ_PLAYER_ENV, SEQ_EVENT_PEACH_MESSAGE, 0);
    sBackgroundMusicMaxTargetVolume = TARGET_VOLUME_IS_PRESENT_FLAG | 0;
    begin_background_music_fade(50);
}

/**
 * Plays the puzzle jingle. Plays the dadada dadada *dadada* jingle
 * that usually plays when you solve a "puzzle", like chests, talking to
 * yoshi, releasing chain chomp, opening the pyramid top, etc.
 *
 * Called from threads: thread5_game_loop
 */
void play_puzzle_jingle(void) {
    seq_player_play_sequence(SEQ_PLAYER_ENV, SEQ_EVENT_SOLVE_PUZZLE, 0);
    sBackgroundMusicMaxTargetVolume = TARGET_VOLUME_IS_PRESENT_FLAG | 20;
    begin_background_music_fade(50);
}

/**
 * Called from threads: thread5_game_loop
 */
void play_star_fanfare(void) {
    seq_player_play_sequence(SEQ_PLAYER_ENV, SEQ_EVENT_HIGH_SCORE, 0);
    sBackgroundMusicMaxTargetVolume = TARGET_VOLUME_IS_PRESENT_FLAG | 20;
    begin_background_music_fade(50);
}

/**
 * Called from threads: thread5_game_loop
 */
void play_power_star_jingle(u8 arg0) {
    if (!arg0) {
        sBackgroundMusicTargetVolume = 0;
    }
    seq_player_play_sequence(SEQ_PLAYER_ENV, SEQ_EVENT_CUTSCENE_STAR_SPAWN, 0);
    sBackgroundMusicMaxTargetVolume = TARGET_VOLUME_IS_PRESENT_FLAG | 20;
    begin_background_music_fade(50);
}

/**
 * Called from threads: thread5_game_loop
 */
void play_race_fanfare(void) {
    seq_player_play_sequence(SEQ_PLAYER_ENV, SEQ_EVENT_RACE, 0);
    sBackgroundMusicMaxTargetVolume = TARGET_VOLUME_IS_PRESENT_FLAG | 20;
    begin_background_music_fade(50);
}

/**
 * Called from threads: thread5_game_loop
 */
void play_toads_jingle(void) {
    seq_player_play_sequence(SEQ_PLAYER_ENV, SEQ_EVENT_TOAD_MESSAGE, 0);
    sBackgroundMusicMaxTargetVolume = TARGET_VOLUME_IS_PRESENT_FLAG | 20;
    begin_background_music_fade(50);
}

/**
 * Called from threads: thread5_game_loop
 */
void sound_reset(void) {
    u8 i;

    sound_init();

    for (i = 0; i < SEQUENCE_PLAYERS; i++) {
        gSequencePlayers[i].muted = FALSE;
    }
}

/**
 * Called from threads: thread5_game_loop
 */
void audio_set_sound_mode(u8 soundMode) {
    D_80332108 = (D_80332108 & 0xf) + (soundMode << 4);
    gSoundMode = soundMode;
}

void unused_80321460(UNUSED s32 arg0, UNUSED s32 arg1, UNUSED s32 arg2, UNUSED s32 arg3) {
}

void unused_80321474(UNUSED s32 arg0) {
}

// Other online players drawn as Link models: see puppet.h.
#ifndef _WIN32
#include <time.h>
#endif
#include "puppet.h"
#include "puppet_pvp.h"
#include "puppet_carry.h"
#include "voice_pitch.h"

#include "guest_coro.h"
#include "gxruntime/guest_memory_dirty.h"
#include "gxruntime/platform.h"
#include "bunny_hood.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

enum {
    // Where the hooks are (mods/betterww/options.txt).
    SITE_DRAW = 0x80234F5Cu,
    SITE_DELETE = 0x802350BCu,

    PLAYER = 0x803CA754u,       // play.mpPlayerPtr[0]
    PLAYER_CL_MODEL = 0x32Cu,   // daPy_lk_c::mpCLModel
    PLAYER_ZOFF_BLEND = 0x374u, // mpZOffBlendShape[4], mpZOffNoneShape[4], mpZOnShape[4]
    STAY_ROOM = 0x803F6A78u,    // dStage_roomControl_c::mStayNo
    ENV_LIGHT = 0x803E4AB4u,    // g_env_light
    J3D_SYS = 0x803EDA58u,      // j3dSys: mModel +0x38, mDrawBuffer[2] +0x48
    DRAW_LIST = 0x803C4C08u + 0x5D1Cu, // g_dComIfG_gameInfo.drawlist (dDlst_list_c)
    DRAW_OPA_P1 = 0x0Cu,        // mpOpaListP1; mpXluListP1 follows

    // J3DModel
    MODEL_DATA = 0x04u,
    MODEL_BASE_MTX = 0x24u,
    MODEL_NODE_MTX = 0x8Cu,

    // Guest functions (GZLE01 symbols.txt).
    FN_CREATE_HEAP_TO_CURRENT = 0x80011CC0u, // mDoExt_createSolidHeapFromGameToCurrent(size, align)
    FN_ADJUST_HEAP = 0x80011D0Cu,            // mDoExt_adjustSolidHeap(heap)
    FN_DESTROY_HEAP = 0x80011D48u,           // mDoExt_destroySolidHeap(heap)
    FN_RESTORE_HEAP = 0x80011DDCu,           // mDoExt_restoreCurrentHeap()
    FN_MODEL_CREATE = 0x80016BB8u,           // mDoExt_J3DModel__create(data, flags, differed)
    FN_NEW = 0x802B0C38u,                    // operator new(size)
    FN_TEVSTR_INIT = 0x80196EB4u,            // dKy_tevstr_init(tevstr, room, envr)
    FN_CALC_WEIGHT_ENV = 0x802EE67Cu,        // J3DModel::calcWeightEnvelopeMtx()
    FN_SETTING_TEVSTRUCT = 0x80193028u,      // dScnKy_env_light_c::settingTevStruct(type, pos, tevstr)
    FN_SET_LIGHT_TEV = 0x80193A34u,          // dScnKy_env_light_c::setLightTevColorType(model, tevstr)
    FN_MODEL_ENTRY_DL = 0x8000F974u,         // mDoExt_modelEntryDL(model)
    FN_NEW_ALIGNED = 0x802B0C60u,            // operator new(size, align)
    FN_LOAD_BDL = 0x802FB94Cu,               // J3DModelLoaderDataBase::loadBinaryDisplayList(data, flags)
    FN_SET_TOON_TEX = 0x8006D990u,           // setToonTex(J3DModelData*)
    BDL_LOAD_FLAGS = 0x00002020u,            // as dRes_info_c loads a 'BDL ' resource
    MODEL_DATA_JOINT_NUM = 0x28u,            // J3DModelData::mJointTree (0x10) + mJointNum (0x18)
    MAX_CUSTOM_BDL = 6 * 1024 * 1024,
    EQUIP_SLOTS = BLUEWAKE_PUPPET_EQUIP_SLOTS, // swgripa, swgripms, sha, shms, swa, swms, hands
    FN_CREATE_HEAP_TO_CURRENT_FROM = 0x80011BE4u, // mDoExt_createSolidHeapToCurrent(size, parent, align)
    FN_GET_ZELDA_HEAP = 0x800118C0u,         // mDoExt_getZeldaHeap()
    PLAYER_HANDS_MODEL = 0x954u,             // daPy_lk_c: mpHandsModel
    PLAYER_EQUIP_SWORD = 0x958u,             // mpEquippedSwordModel
    PLAYER_SWGRIPA = 0x95Cu,                 // mpSwgripaModel (the sword), mpSwgripmsModel (the Master Sword) follows
    PLAYER_EQUIP_SHIELD = 0x980u,            // mpEquippedShieldModel
    PLAYER_SHA = 0x984u,                     // mpShaModel (the shield), mpShmsModel (the Mirror Shield) follows
    PLAYER_LHAND_SHAPE = 0x3A4u,             // mpLhandShape, mpRhandShape follows: the hand shapes his draw chose
    SAVE_SELECT_SWORD = 0x803C4C16u,         // dSv_player_status_a_c::mSelectEquip[0] (0xFF none), [1] the shield
    CL_JNT_PODA = 0x0D,
    CL_JNT_LHANDA = 0x08,
    CL_JNT_RHANDA = 0x0C,
    PUPPET_FLAG_SCABBARD = 2,                // Pose::flags
    PUPPET_FLAG_BLADE = 8,                   // Pose::flags: the sword is drawn (mEquipItem is the sword): the blade model is in his hand
    PLAYER_EQUIP_ITEM_MODEL = 0x2E98u,       // daPy_lk_c::mpEquipItemModel (the blade, while the sword is drawn)
    PLAYER_EQUIP_ITEM = 0x3560u,             // daPy_lk_c::mEquipItem (u16); daPyItem_SWORD_e = 0x103
    FN_GET_RES = 0x8006F208u,                // dRes_control_c::getRes(arcName, index, infos, count)
    ARC_LINK_NAME = 0x803F62A0u,             // l_arcName ("Link")
    RES_OBJECT_INFO = 0x803E0BC8u,           // g_dComIfG_gameInfo.mResControl.mObjectInfo
    RES_BDL_SWA = 0x25, RES_BDL_SWMS = 0x46, // dRes_INDEX_LINK_BDL_SWA_e, _SWMS_e (the blades)
    PUPPET_FLAG_HOOD = 4,                    // Pose::flags: the Bunny Hood is worn
    PLAYER_CUR_PROC = 0x31D8u,               // daPy_lk_c::mCurProc
    JAI_BASIC = 0x803F7578u,                 // JAIBasic::msBasic (the game's audio interface)
    FN_LINK_VOICE_VOWEL = 0x802A86A8u,       // JAIZelBasic::getLinkVoiceVowel(id)
    FN_LINK_VOICE_START = 0x802A8748u,       // JAIZelBasic::linkVoiceStart(id, pos, vowel, reverb)
    FN_SE_START = 0x800F0D10u,               // mDoAud_seStart(id, pos, param, reverb)
    FN_MODEL_CALC = 0x802EE8C0u,             // J3DModel::calc()
    PLAYER_KATSURA_MODEL = 0x330u,           // daPy_lk_c::mpKatsuraModel (casual clothes' back hair)
    CL_JNT_HEAD = 0x0F,
    FN_MODEL_UNLOCK = 0x802EE28Cu,           // J3DModel::unlock()
    FN_ENTRY_IN = 0x802F58D8u,               // J3DJoint::entryIn()
    FN_ENTRY_IMM = 0x802ECCC4u,              // J3DDrawBuffer::entryImm(packet, u16)
    PKT_OFF_CUP_ON_AUP1 = 0x803E46A4u,       // daPy's static state packets
    PKT_OFF_CUP_ON_AUP2 = 0x803E46C0u,
    PKT_ON_CUP_OFF_AUP1 = 0x803E46DCu,
    PKT_ON_CUP_OFF_AUP2 = 0x803E46F8u,

    // Face and shadow (J3DModelData: mMaterialTable +0x58 holds mMaterialNum +0x5C and mMaterialNodePointer +0x60; J3DMaterial:
    // mTevBlock +0x2C, mMaterialAnm +0x3C; the tev blocks' mTexNo[] +8; J3DMaterialAnm: mTexNoAnm[8] +0x2C).
    MODEL_DATA_MATERIAL_NUM = 0x5Cu,
    MODEL_DATA_MATERIALS = 0x60u,
    MAT_TEV_BLOCK = 0x2Cu,
    MAT_ANM = 0x3Cu,
    TEV_BLOCK_TEXNO = 0x08u,
    MATANM_TEXNO = 0x2Cu,
    FN_GND_HEIGHT = 0x800A4564u,             // dBgS_ObjGndChk_Func(cXyz&): the ground height below a point (f1), -1e9 for none
    FN_SHADOW_REAL = 0x80085170u,            // dDlst_shadowControl_c::setReal2(key, fade, model, pos, size f1, height f2, tevstr)
    FN_SHADOW_ADD = 0x8008520Cu,             // dDlst_shadowControl_c::addReal(key, model)
    FN_SHADOW_SIMPLE = 0x80085274u,          // dDlst_shadowControl_c::setSimple(pos, ground f1, scale f2, normal, angle, scaleZ f3, tex)
    SHADOW_CONTROL = 0x803C4C08u + 0x5D1Cu + 0x250u, // g_dComIfG_gameInfo.drawlist.mShadowControl
    SIMPLE_TEX = 0x803E229Cu,                // dDlst_shadowControl_c::mSimpleTexObj
    SHADOW_CASTER_SIZE = 700,                // daPy_HIO_basic_c0::m's shadow size (his drawShadow)
    PUPPET_FACE_MAX = 32,

    // Things he wears and holds (daPy_lk_c, GZLE01).
    PLAYER_NO_RESET_FLG0 = 0x29Cu,
    PLAYER_FLG0_HEAVY_BOOTS = 0x02000000u,   // daPyFlg0_EQUIP_HEAVY_BOOTS
    PLAYER_YAMU_MODEL = 0x334u,              // mpYamuModel (the Hero's Charm)
    PLAYER_HBOOTS = 0x2E84u,                 // mpHbootsModels[2] (the Iron Boots, one on each foot)
    PLAYER_KEEP_EQUIP = 0x317Cu,             // daPy_actorKeep_c (id +0, actor +4): the item actor in his hand; Throw (a boomerang
    PLAYER_KEEP_THROW = 0x3184u,             // in the air) and Grab (what he carries) follow
    PLAYER_KEEP_GRAB = 0x318Cu,
    ACTOR_MODEL = 0x24Cu,                    // fopAc_ac_c::model
    SAVE_COLLECT4 = 0x803C4C08u + 0xB4u + 4u, // dSv_player_c::mCollect (+0xB4 in memory; +0xB2 is the card's packed layout) mCollect[4], bit 1: the Hero's Charm worn
    RES_OBJECT_INFO_NUM = 64,
    RES_INFO_SIZE = 0x24u,                   // dRes_info_c: name +0, count +0xE, archive +0x14, resources +0x20
    ARCHIVE_ENTRY_NUM = 0x40u,               // JKRArchive::mEntryNum
    MODEL_MTX_BUF = 0x8Cu,
    ATT_KIND_ITEM = 1, ATT_KIND_BOOTS = 2, ATT_KIND_CHARM = 3, ATT_KIND_GRAB = 4, ATT_KIND_EQUIP = 5, ATT_KIND_THROW = 6,
    SHIP_ACTOR = 0x803CA754u + 8u,           // play.mpPlayerPtr[2]
    PLAY_STATUS0 = 0x803C5EA8u + 0x4A28u,    // play.mPlayerStatus[0][0]
    PLAY_STATUS0_SHIP_RIDE = 0x00010000u,    // daPyStts0_SHIP_RIDE_e
    SHIP_BODY_ANM = 0x298u,                  // daShip_c::mpBodyAnm, mpHeadAnm follows (mDoExt_McaMorf: mpModel +0x50)
    MORF_MODEL = 0x50u,
    ATT_KIND_SHIP_BODY = 7, ATT_KIND_SHIP_HEAD = 8,
    EVENT_ITEM_PARTNER = 0x803C9EB0u,        // dEvt_control_c::mPtItem: the process id of the item actor of an item get (-1: none)
    PROC_GET_ITEM = 0xAE,                    // daPyProc_DEMO_GET_ITEM_e: the item held up (a chest's, a boss's)
    ITEM_ACTOR_MODEL = 0x29Cu,               // daItemBase_c::mpModel
    ITEM_ACTOR_DRAW = 0x63Bu,                // daItemBase_c::mDrawFlags, bit 0: shown
    FN_SEARCH_BY_ID = 0x800241C0u,           // fopAcM_SearchByID(id, &actor)
    ATT_KIND_GET = 9,
    // Moving equipment: the hookshot's chain and the grappling hook's rope, drawn as the game draws a rope (a strip).
    FN_LINE_INIT = 0x80015328u,              // mDoExt_3DlineMat1_c::init(numLines, numSegments, ResTIMG*, hasSize)
    FN_LINE_UPDATE = 0x80015764u,            // mDoExt_3DlineMat1_c::update(segments, size f1, GXColor&, space, tevstr)
    FN_LINE_SET = 0x80016518u,               // mDoExt_3DlineMatSortPacket::setMat(mat)
    FN_GET_RES_NAMED = 0x8006F298u,          // dRes_control_c::getRes(arcName, resName, infos, count)
    LINE_VTABLE = 0x80371B44u,               // __vt__19mDoExt_3DlineMat1_c
    LINE_SORT_PACKET = 0x803C4C08u + 0x5D1Cu + 0x16078u + 0x14u, // drawlist.m3DLineMatSortPacket[1] (material id 1)
    PLAYER_HOOKSHOT_ROOT = 0x36E8u,          // daPy_lk_c::mHookshotRootPos
    PLAYER_KEEP_ROPE = 0x3194u,              // mActorKeepRope (the grappling hook's rope actor)
    ACTOR_CURRENT_POS = 0x1F8u,
    HOOKSHOT_CHAIN_COUNT = 0x2A8u,           // daHookshot_c::mChainCnt
    ITEM_HOOKSHOT = 0x2F, ITEM_GRAPPLE = 0x25,
    FN_SKIN_CTOR = 0x802F40C0u,              // J3DSkinDeform::J3DSkinDeform()
    FN_SET_SKIN = 0x802EE4BCu,               // J3DModel::setSkinDeform(deform, flags)
    FN_SKIN_DEFORM = 0x802F4CD8u,            // J3DSkinDeform::deform(model)
    // The ship's sail: daGrid_c (ship +0x414), whose mPacket (+0x2A0) is a cloth of 85 points drawn through the Z-sorted
    // translucent list. The sender sends the points and where the sail is; the receiver draws a copy of the packet.
    SHIP_GRID = 0x414u,
    GRID_SCALE_Y = 0x218u,                   // fopAc_ac_c::scale.y (below 0.06 the sail is furled and not drawn)
    GRID_PACKET = 0x2A0u,
    HO_MTX = 0x80u, HO_TEVSTR = 0xB0u, HO_POS = 0xB4u, HO_NRM = 0x8ACu, HO_BACKNRM = 0x10A4u, HO_CUR = 0x18A2u, HO_ALPHA = 0x18A3u,
    HO_SIZE = 0x18A4u,
    FN_RES_LOAD = 0x80053390u,               // dComIfG_resLoad(request_of_phase_process_class*, arcName)
    FN_RES_DELETE = 0x800533D0u,             // dComIfG_resDelete(request_of_phase_process_class*, arcName)
    HO_POINTS = 85,
    FN_ENTRY_Z_SORT = 0x802ECAF0u,           // J3DDrawBuffer::entryZSort(packet)
    DRAW_XLU_LIST = 0x20u,                   // dDlst_list_c::mpXluList
    ATT_MODELS = 8,                          // models a puppet keeps made at once
    ATT_HEAP = 0x20000,

    CL_JOINTS = 42,
    MAX_REMOTES = 16,
    SELF_ID = 1000,
    WIRE_VERSION = 1,
    WIRE_HEADER = 32,
    HEAP_SIZE = 0x50000,
    PLAYER_NO_RESET_FLG1 = 0x2A0u, // daPy_py_c::mNoResetFlg1
    PLAYER_FLG1_CASUAL = 0x8u,     // daPyFlg1_CASUAL_CLOTHES
    PUPPET_FLAG_CASUAL = 1,        // Pose::flags
};

// ---------------------------------------------------------------- math ----

typedef struct Mat34 {
    float m[3][4];
} Mat34;

static void mat_mul(const Mat34* a, const Mat34* b, Mat34* out) {
    Mat34 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
        r.m[i][3] = a->m[i][0] * b->m[0][3] + a->m[i][1] * b->m[1][3] + a->m[i][2] * b->m[2][3] + a->m[i][3];
    }
    *out = r;
}

static bool mat_inverse(const Mat34* in, Mat34* out) {
    const float(*a)[4] = in->m;
    const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                      a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabsf(det) < 1e-12f)
        return false;
    const float d = 1.0f / det;
    Mat34 r;
    r.m[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * d;
    r.m[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * d;
    r.m[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * d;
    r.m[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * d;
    r.m[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * d;
    r.m[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * d;
    r.m[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * d;
    r.m[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * d;
    r.m[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * d;
    for (int i = 0; i < 3; ++i)
        r.m[i][3] = -(r.m[i][0] * a[0][3] + r.m[i][1] * a[1][3] + r.m[i][2] * a[2][3]);
    *out = r;
    return true;
}

// A transform as a rotation (quaternion x y z w, w >= 0), a translation, and a
// scale along the matrix's columns.
typedef struct Xf {
    float q[4];
    float t[3];
    float s[3];
} Xf;

static void quat_normalize(float q[4]) {
    const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-9f) {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    for (int i = 0; i < 4; ++i)
        q[i] /= n;
}

static void xf_from_mat(const Mat34* m, Xf* x) {
    float c[3][3]; // the columns, normalized: c[j][i] is the rotation's [i][j]
    for (int j = 0; j < 3; ++j) {
        const float len = sqrtf(m->m[0][j] * m->m[0][j] + m->m[1][j] * m->m[1][j] + m->m[2][j] * m->m[2][j]);
        x->s[j] = len;
        const float inv = len > 1e-9f ? 1.0f / len : 0.0f;
        for (int i = 0; i < 3; ++i)
            c[j][i] = m->m[i][j] * inv;
    }
    const float r00 = c[0][0], r11 = c[1][1], r22 = c[2][2];
    const float tr = r00 + r11 + r22;
    float q[4];
    if (tr > 0.0f) {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (c[1][2] - c[2][1]) / s;
        q[1] = (c[2][0] - c[0][2]) / s;
        q[2] = (c[0][1] - c[1][0]) / s;
    } else if (r00 > r11 && r00 > r22) {
        const float s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        q[3] = (c[1][2] - c[2][1]) / s;
        q[0] = 0.25f * s;
        q[1] = (c[1][0] + c[0][1]) / s;
        q[2] = (c[2][0] + c[0][2]) / s;
    } else if (r11 > r22) {
        const float s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        q[3] = (c[2][0] - c[0][2]) / s;
        q[0] = (c[1][0] + c[0][1]) / s;
        q[1] = 0.25f * s;
        q[2] = (c[2][1] + c[1][2]) / s;
    } else {
        const float s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        q[3] = (c[0][1] - c[1][0]) / s;
        q[0] = (c[2][0] + c[0][2]) / s;
        q[1] = (c[2][1] + c[1][2]) / s;
        q[2] = 0.25f * s;
    }
    quat_normalize(q);
    if (q[3] < 0.0f)
        for (int i = 0; i < 4; ++i)
            q[i] = -q[i];
    memcpy(x->q, q, sizeof q);
    for (int i = 0; i < 3; ++i)
        x->t[i] = m->m[i][3];
}

static void mat_from_xf(const Xf* x, Mat34* m) {
    const float qx = x->q[0], qy = x->q[1], qz = x->q[2], qw = x->q[3];
    const float r[3][3] = {
        {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)},
        {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
        {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)},
    };
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            m->m[i][j] = r[i][j] * x->s[j];
        m->m[i][3] = x->t[i];
    }
}

// ---------------------------------------------------------------- pose ----

// The voice Link makes as each action starts: the id of the voiceStart in that action's init function in the
// decomp (daPy_lk_c::proc*_init), by mCurProc (0xFF: none). Generated from the decomp's source.
static const uint8_t kProcVoice[] = {255,255,42,255,255,255,255,255,255,255,5,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,7,8,255,255,7,255,6,255,13,255,255,5,255,255,11,255,255,255,32,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,0,0,0,0,1,1,1,1,1,1,1,1,7,1,255,255,1,7,1,255,1,255,255,255,255,37,1,255,6,1,255,7,1,6,255,255,255,2,2,3,255,255,4,44,255,255,16,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,6,255,255,255,255,255,255,255,255,255,255,255,255,7,5,255,255,6,255,0,255,255,255,255,255,36,255,255,255,255,255,255,7,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,28,255,255,255,255,255,255,255,19,255,255,255,255,255,255,28,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255};

// And the sound effect that goes with a few: Link's damage cries, the crash of a roll into a wall, the swish of a cut.
static u32 proc_effect(unsigned proc) {
    switch (proc) {
    case 0x66: case 0x67: return 0x282Du; // JA_SE_LK_DAMAGE_NORMAL
    case 0x68: case 0x6B: case 0x6C: return 0x282Eu; // JA_SE_LK_DAMAGE_LARGE
    case 0x1F: return 0x282Fu; // JA_SE_LK_BODYATTACK
    case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A:
    case 0x5B: case 0x55: return 0x2800u; // JA_SE_LK_SW_KAZEKIRI_S
    default: return 0u;
    }
}

// One expression state of a material (an eye, a brow, the mouth): its texture number for one texture slot, which the
// game's material animation (daPy_matAnm_c, a texture-pattern animation) sets from the player's own state.
typedef struct FaceTex {
    uint8_t mat, slot;
    uint16_t tex;
} FaceTex;

// Something the player has in hand or on (a model of the game's resources) found by the archive and index it is in, with
// where it is and, for models with joints of their own, where those are, all relative to the pose's base.
enum { ATT_MAX = 6, ATT_JOINTS = 40, ATT_ARC = 14 };
typedef struct PoseAtt {
    uint8_t kind;
    char arc[ATT_ARC]; // "" is the Link archive
    uint16_t idx;
    uint8_t nj;        // the model's joints sent (0: only the base)
    Xf base;
    Xf joint[ATT_JOINTS];
} PoseAtt;

typedef struct Pose {
    float base_pos[3];
    float base_q[4];
    Xf joint[CL_JOINTS];
    uint8_t flags, sword, shield; // sword: 0 none, 1 Hero's, 2 Master; shield: 0 none, 1 Hero's, 2 Mirror
    uint16_t proc;                // mCurProc: what Link is doing (puppets make his voice when it changes)
    uint8_t hands;                // low nibble: the left hand's joint in Link's hands model (0: the body's own hand), high: the right
    Xf sword_xf, shield_xf;       // where the sword and the shield are, relative to the base
    // Added after the first wire version (extension blocks after the proc: older builds stop at the proc and ignore them).
    uint8_t face_n;
    FaceTex face[PUPPET_FACE_MAX];
    uint8_t att_n;
    PoseAtt att[ATT_MAX];
    bool sail;       // the ship's sail: where it is (relative to the base) and its 85 cloth points in its own space
    Xf sail_xf;
    int16_t sail_pt[HO_POINTS][3];
    uint8_t line_n;  // a chain or a rope: from his hand to the end of it, relative to the base
    struct PoseLine {
        uint8_t kind; // 1 the hookshot's chain, 2 the grappling hook's rope
        float p[2][3];
    } line[2];
    bool has_time;   // the sender's clock (ms, wraps) when it captured this pose: playback follows it, not the network's jitter
    uint32_t wtime;
} Pose;

static int16_t q15(float v) {
    const float r = roundf(v * 32767.0f);
    return (int16_t)(r > 32767.0f ? 32767 : r < -32767.0f ? -32767 : r);
}

static int16_t t8(float v) {
    const float r = roundf(v * 8.0f);
    return (int16_t)(r > 32767.0f ? 32767 : r < -32767.0f ? -32767 : r);
}

static void put16(uint8_t* p, int16_t v) {
    p[0] = (uint8_t)((uint16_t)v & 0xFFu);
    p[1] = (uint8_t)((uint16_t)v >> 8);
}

static int16_t get16(const uint8_t* p) { return (int16_t)(uint16_t)(p[0] | (p[1] << 8)); }

static bool joint_scaled(const Xf* x) {
    return fabsf(x->s[0] - 1.0f) > 0.01f || fabsf(x->s[1] - 1.0f) > 0.01f || fabsf(x->s[2] - 1.0f) > 0.01f;
}

// A transform on the wire: rotation 4 x s16, translation 3 x s16 and, when `scaled`, the scale as 3 bytes (1/64).
static uint8_t* xf_put(uint8_t* at, const Xf* x, bool scaled) {
    for (int k = 0; k < 4; ++k, at += 2)
        put16(at, q15(x->q[k]));
    for (int k = 0; k < 3; ++k, at += 2)
        put16(at, t8(x->t[k]));
    if (scaled)
        for (int k = 0; k < 3; ++k) {
            const float v = roundf(x->s[k] * 64.0f);
            *at++ = (uint8_t)(v > 255.0f ? 255.0f : v < 0.0f ? 0.0f : v);
        }
    return at;
}

static const uint8_t* xf_get(const uint8_t* at, Xf* x, bool scaled) {
    for (int k = 0; k < 4; ++k, at += 2)
        x->q[k] = (float)get16(at) / 32767.0f;
    quat_normalize(x->q);
    for (int k = 0; k < 3; ++k, at += 2)
        x->t[k] = (float)get16(at) / 8.0f;
    for (int k = 0; k < 3; ++k)
        x->s[k] = scaled ? (float)*at++ / 64.0f : 1.0f;
    return at;
}

// The extension blocks after the proc: tag (u8), length (u16), contents. A reader skips tags it does not know.
enum { EXT_FACE = 1, EXT_ATTACH = 2, EXT_TIME = 3, EXT_LINE = 4, EXT_SAIL = 5 };

static uint8_t* ext_begin(uint8_t* at, uint8_t tag) {
    at[0] = tag;
    return at + 3;
}

static uint8_t* ext_end(uint8_t* head, uint8_t* at) {
    put16(head + 1, (int16_t)(at - head - 3));
    return at;
}

static uint8_t* ext_encode(const Pose* p, uint8_t* at, uint8_t* limit) {
    if (p->line_n != 0 && at + 4 + 14 * p->line_n <= limit) {
        uint8_t* head = at;
        at = ext_begin(at, EXT_LINE);
        *at++ = p->line_n;
        for (int i = 0; i < p->line_n; ++i) {
            *at++ = p->line[i].kind;
            for (int k = 0; k < 6; ++k, at += 2)
                put16(at, t8(p->line[i].p[k / 3][k % 3]));
        }
        at = ext_end(head, at);
    }
    if (p->has_time && at + 7 <= limit) {
        uint8_t* head = at;
        at = ext_begin(at, EXT_TIME);
        put16(at, (int16_t)(p->wtime & 0xFFFFu));
        put16(at + 2, (int16_t)(p->wtime >> 16));
        at += 4;
        at = ext_end(head, at);
    }
    if (p->sail && at + 3 + 17 + 6 * HO_POINTS <= limit) {
        uint8_t* head = at;
        at = ext_begin(at, EXT_SAIL);
        at = xf_put(at, &p->sail_xf, true);
        for (int i = 0; i < HO_POINTS; ++i)
            for (int k = 0; k < 3; ++k, at += 2)
                put16(at, p->sail_pt[i][k]);
        at = ext_end(head, at);
    }
    if (p->face_n != 0 && at + 3 + 4 * p->face_n + 1 <= limit) {
        uint8_t* head = at;
        at = ext_begin(at, EXT_FACE);
        *at++ = p->face_n;
        for (int i = 0; i < p->face_n; ++i) {
            *at++ = p->face[i].mat;
            *at++ = p->face[i].slot;
            put16(at, (int16_t)p->face[i].tex);
            at += 2;
        }
        at = ext_end(head, at);
    }
    if (p->att_n != 0) {
        uint8_t* head = at;
        at = ext_begin(at, EXT_ATTACH);
        uint8_t* count = at++;
        *count = 0;
        for (int i = 0; i < p->att_n; ++i) {
            const PoseAtt* a = &p->att[i];
            const size_t arc = strnlen(a->arc, ATT_ARC);
            if (at + 8 + arc + 17 * (1 + a->nj) > limit)
                break;
            *at++ = a->kind;
            *at++ = (uint8_t)arc;
            memcpy(at, a->arc, arc);
            at += arc;
            put16(at, (int16_t)a->idx);
            at += 2;
            *at++ = a->nj;
            bool scaled = joint_scaled(&a->base); // then every transform of this one carries its scale
            for (int j = 0; j < a->nj; ++j)
                scaled |= joint_scaled(&a->joint[j]);
            *at++ = scaled ? 1u : 0u;
            at = xf_put(at, &a->base, scaled);
            for (int j = 0; j < a->nj; ++j)
                at = xf_put(at, &a->joint[j], scaled);
            ++*count;
        }
        at = *count != 0 ? ext_end(head, at) : head;
    }
    return at;
}

static void ext_decode(const uint8_t* at, const uint8_t* end, Pose* p) {
    p->face_n = 0;
    p->att_n = 0;
    p->has_time = false;
    p->line_n = 0;
    p->sail = false;
    while (at + 3 <= end) {
        const uint8_t tag = at[0];
        const size_t len = (uint16_t)get16(at + 1);
        at += 3;
        if (at + len > end)
            return;
        const uint8_t* block = at;
        const uint8_t* block_end = at + len;
        at += len;
        if (tag == EXT_TIME && len >= 4) {
            p->wtime = (uint32_t)(uint16_t)get16(block) | ((uint32_t)(uint16_t)get16(block + 2) << 16);
            p->has_time = true;
        } else if (tag == EXT_SAIL && len >= 17 + 6 * HO_POINTS) {
            const uint8_t* b = xf_get(block, &p->sail_xf, true);
            for (int i = 0; i < HO_POINTS; ++i)
                for (int k = 0; k < 3; ++k, b += 2)
                    p->sail_pt[i][k] = get16(b);
            p->sail = true;
        } else if (tag == EXT_LINE && len >= 1) {
            for (int i = 0; i < block[0] && i < 2 && block + 1 + 13 * (i + 1) <= block_end; ++i) {
                const uint8_t* b = block + 1 + 13 * i;
                p->line[i].kind = b[0];
                for (int k = 0; k < 6; ++k)
                    p->line[i].p[k / 3][k % 3] = (float)get16(b + 1 + 2 * k) / 8.0f;
                p->line_n = (uint8_t)(i + 1);
            }
        } else if (tag == EXT_FACE && len >= 1) {
            const int n = block[0] < PUPPET_FACE_MAX ? block[0] : PUPPET_FACE_MAX;
            for (int i = 0; i < n && block + 1 + 4 * (i + 1) <= block_end; ++i) {
                p->face[i].mat = block[1 + 4 * i];
                p->face[i].slot = block[2 + 4 * i];
                p->face[i].tex = (uint16_t)get16(block + 3 + 4 * i);
                p->face_n = (uint8_t)(i + 1);
            }
        } else if (tag == EXT_ATTACH && len >= 1) {
            const uint8_t* b = block + 1;
            for (int i = 0; i < block[0] && i < ATT_MAX; ++i) {
                if (b + 2 > block_end)
                    break;
                PoseAtt* a = &p->att[p->att_n];
                memset(a, 0, sizeof *a);
                a->kind = b[0];
                const size_t arc = b[1];
                b += 2;
                if (arc >= ATT_ARC || b + arc + 4 > block_end)
                    break;
                memcpy(a->arc, b, arc);
                b += arc;
                a->idx = (uint16_t)get16(b);
                a->nj = b[2];
                const bool scaled = b[3] != 0;
                b += 4;
                if (a->nj > ATT_JOINTS)
                    break;
                const size_t need = (14u + (scaled ? 3u : 0u)) * (1u + a->nj);
                if (b + need > block_end)
                    break;
                b = xf_get(b, &a->base, scaled);
                for (int j = 0; j < a->nj; ++j)
                    b = xf_get(b, &a->joint[j], scaled);
                ++p->att_n;
            }
        }
    }
}

// The wire form. Header (32 bytes, little endian): version, joint count,
// flags, sword, shield, 0, the base position (3 floats), the base rotation
// (4 x s16), a bitmask of the joints that have a scale. Then each joint:
// rotation 4 x s16 (x y z w, 1/32767), translation 3 x s16 (1/8 unit) and, if
// its bit is set, 3 bytes of scale (1/64).
static size_t wire_encode(const Pose* p, uint8_t* out) {
    memset(out, 0, WIRE_HEADER);
    out[0] = WIRE_VERSION;
    out[1] = CL_JOINTS;
    out[2] = p->flags;
    out[3] = p->sword;
    out[4] = p->shield;
    out[5] = p->hands;
    memcpy(out + 6, p->base_pos, 12);
    for (int i = 0; i < 4; ++i)
        put16(out + 18 + 2 * i, q15(p->base_q[i]));
    uint8_t* at = out + WIRE_HEADER;
    for (int i = 0; i < CL_JOINTS; ++i) {
        const Xf* x = &p->joint[i];
        const bool scaled = joint_scaled(x);
        if (scaled)
            out[26 + i / 8] |= (uint8_t)(1u << (i % 8));
        at = xf_put(at, x, scaled);
    }
    // The sword's and the shield's places, when there are any: rotation 4 x s16, translation 3 x s16.
    for (int e = 0; e < 2; ++e) {
        if ((e == 0 ? p->sword : p->shield) == 0)
            continue;
        const Xf* x = e == 0 ? &p->sword_xf : &p->shield_xf;
        for (int k = 0; k < 4; ++k, at += 2)
            put16(at, q15(x->q[k]));
        for (int k = 0; k < 3; ++k, at += 2)
            put16(at, t8(x->t[k]));
    }
    put16(at, (int16_t)p->proc); // what he is doing, for the puppet's voice
    at += 2;
    at = ext_encode(p, at, out + BLUEWAKE_PUPPET_MAX_WIRE);
    return (size_t)(at - out);
}

static bool wire_decode(const uint8_t* in, size_t length, Pose* p) {
    if (length < WIRE_HEADER || in[0] != WIRE_VERSION || in[1] != CL_JOINTS)
        return false;
    p->flags = in[2];
    p->sword = in[3];
    p->shield = in[4];
    p->hands = in[5];
    memcpy(p->base_pos, in + 6, 12);
    for (int i = 0; i < 4; ++i)
        p->base_q[i] = (float)get16(in + 18 + 2 * i) / 32767.0f;
    quat_normalize(p->base_q);
    const uint8_t* at = in + WIRE_HEADER;
    const uint8_t* end = in + length;
    for (int i = 0; i < CL_JOINTS; ++i) {
        Xf* x = &p->joint[i];
        const bool scaled = (in[26 + i / 8] >> (i % 8)) & 1;
        if (at + 14 + (scaled ? 3 : 0) > end)
            return false;
        for (int k = 0; k < 4; ++k, at += 2)
            x->q[k] = (float)get16(at) / 32767.0f;
        quat_normalize(x->q);
        for (int k = 0; k < 3; ++k, at += 2)
            x->t[k] = (float)get16(at) / 8.0f;
        for (int k = 0; k < 3; ++k)
            x->s[k] = scaled ? (float)*at++ / 64.0f : 1.0f;
    }
    for (int e = 0; e < 2; ++e) {
        Xf* x = e == 0 ? &p->sword_xf : &p->shield_xf;
        memset(x, 0, sizeof *x);
        x->q[3] = 1.0f;
        x->s[0] = x->s[1] = x->s[2] = 1.0f;
        if ((e == 0 ? p->sword : p->shield) == 0)
            continue;
        if (at + 14 > end)
            return false;
        for (int k = 0; k < 4; ++k, at += 2)
            x->q[k] = (float)get16(at) / 32767.0f;
        quat_normalize(x->q);
        for (int k = 0; k < 3; ++k, at += 2)
            x->t[k] = (float)get16(at) / 8.0f;
    }
    p->proc = at + 2 <= end ? (uint16_t)get16(at) : 0xFFFFu;
    ext_decode(at + 2 <= end ? at + 2 : end, end, p);
    return true;
}

static void xf_lerp(const Xf* a, const Xf* b, float f, Xf* out) {
    const float dot = a->q[0] * b->q[0] + a->q[1] * b->q[1] + a->q[2] * b->q[2] + a->q[3] * b->q[3];
    const float sign = dot < 0.0f ? -1.0f : 1.0f;
    for (int i = 0; i < 4; ++i)
        out->q[i] = a->q[i] + (b->q[i] * sign - a->q[i]) * f;
    quat_normalize(out->q);
    for (int i = 0; i < 3; ++i) {
        out->t[i] = a->t[i] + (b->t[i] - a->t[i]) * f;
        out->s[i] = a->s[i] + (b->s[i] - a->s[i]) * f;
    }
}

static void pose_lerp(const Pose* a, const Pose* b, float f, Pose* out) {
    for (int i = 0; i < 3; ++i)
        out->base_pos[i] = a->base_pos[i] + (b->base_pos[i] - a->base_pos[i]) * f;
    const Xf qa = {{a->base_q[0], a->base_q[1], a->base_q[2], a->base_q[3]}, {0, 0, 0}, {1, 1, 1}};
    const Xf qb = {{b->base_q[0], b->base_q[1], b->base_q[2], b->base_q[3]}, {0, 0, 0}, {1, 1, 1}};
    Xf q;
    xf_lerp(&qa, &qb, f, &q);
    memcpy(out->base_q, q.q, sizeof q.q);
    for (int i = 0; i < CL_JOINTS; ++i)
        xf_lerp(&a->joint[i], &b->joint[i], f, &out->joint[i]);
    const Pose* nearer = f < 0.5f ? a : b;
    out->flags = nearer->flags;
    out->sword = nearer->sword;
    out->shield = nearer->shield;
    out->hands = nearer->hands;
    out->proc = nearer->proc;
    xf_lerp(&a->sword_xf, &b->sword_xf, f, &out->sword_xf);
    xf_lerp(&a->shield_xf, &b->shield_xf, f, &out->shield_xf);
    out->face_n = nearer->face_n;
    memcpy(out->face, nearer->face, sizeof out->face);
    // What is held: each of the nearer pose's attachments, moved on from the other pose's same one (same kind and model).
    const Pose* other = nearer == a ? b : a;
    out->sail = nearer->sail;
    out->sail_xf = nearer->sail_xf;
    memcpy(out->sail_pt, nearer->sail_pt, sizeof out->sail_pt);
    out->line_n = nearer->line_n;
    memcpy(out->line, nearer->line, sizeof out->line);
    for (int i = 0; i < nearer->line_n; ++i)
        for (int k = 0; k < other->line_n; ++k)
            if (other->line[k].kind == nearer->line[i].kind)
                for (int j = 0; j < 6; ++j) {
                    const float x = other->line[k].p[j / 3][j % 3], y = nearer->line[i].p[j / 3][j % 3];
                    const float fa = nearer == a ? f : 1.0f - f;
                    out->line[i].p[j / 3][j % 3] = y + (x - y) * fa;
                }
    out->att_n = nearer->att_n;
    for (int i = 0; i < nearer->att_n; ++i) {
        const PoseAtt* n = &nearer->att[i];
        const PoseAtt* o = NULL;
        for (int k = 0; k < other->att_n && o == NULL; ++k) {
            const PoseAtt* c = &other->att[k];
            if (c->kind == n->kind && c->idx == n->idx && c->nj == n->nj && strcmp(c->arc, n->arc) == 0)
                o = c;
        }
        out->att[i] = *n;
        if (o != NULL) {
            const PoseAtt* pa = nearer == a ? n : o;
            const PoseAtt* pb = nearer == a ? o : n;
            xf_lerp(&pa->base, &pb->base, f, &out->att[i].base);
            for (int j = 0; j < n->nj; ++j)
                xf_lerp(&pa->joint[j], &pb->joint[j], f, &out->att[i].joint[j]);
        }
    }
}

// ------------------------------------------------------- guest memory ----

static float read_float(CPUState* cpu, u32 address) {
    const u32 bits = mem_read32(cpu, address);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static void write_float(CPUState* cpu, u32 address, float f) {
    u32 bits;
    memcpy(&bits, &f, sizeof bits);
    mem_write32(cpu, address, bits);
}

// MEM1 (0x80000000 to 0x81800000) or the MEM2 block the host gave the guest (0x90000000 up; bluewake_puppet_mem2_end is set by
// the hook, 0 when there is none), where the puppets' models live.
u32 bluewake_puppet_mem2_end;
u32 bluewake_puppet_mem1_end = 0x81800000u; // the MEM1 array's end (32 or 64 MB: the fast pool lies above the game's 24 MB)
static bool guest_ptr(u32 p) {
    return ((p >= 0x80000000u && p < bluewake_puppet_mem1_end) || (p >= 0x90000000u && p < bluewake_puppet_mem2_end)) && (p & 3u) == 0u;
}

static bool guard_ptr_ok(u32 p) { return guest_ptr(p); }

static void read_mat(CPUState* cpu, u32 address, Mat34* m) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            m->m[i][j] = read_float(cpu, address + 16u * (u32)i + 4u * (u32)j);
}

static void write_mat(CPUState* cpu, u32 address, const Mat34* m) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            write_float(cpu, address + 16u * (u32)i + 4u * (u32)j, m->m[i][j]);
}

// ---------------------------------------------------------- shared state ----

enum { SNAPS = 5 };
typedef struct Snap {
    double time;  // arrival, ms
    double stime; // the sender's clock when captured (unwrapped, ms from its first pose), when it sent one
    Pose pose;
} Snap;

typedef struct Remote {
    int id; // 0 = free
    bool show;
    int count;        // snapshots held (at most SNAPS)
    Snap snap[SNAPS]; // oldest first
    // The sender's clock against ours: the least (arrival - sender time) seen, which is the quickest trip, so playback
    // at (now - offset - delay) in the sender's time does not move with the network's jitter.
    bool have_clock;
    double clock_off, stime_acc;
    uint32_t last_wtime;
    bool drawn;
    float head[3];
    // The head's place on screen (clip x, y, w) in the last two game frames, from each frame's own camera, for the name tag.
    float clip[2][3];
    int clips; // how many of them are set (0 to 2)
    uint8_t* bdl; // the player's own body model (cl.bdl), if one was sent
    size_t bdl_len;
    uint8_t* eq[EQUIP_SLOTS]; // the player's own equipment models (or the game's, for a PC whose own are a mod's)
    size_t eq_len[EQUIP_SLOTS];
    char voice[40];     // the owner's voice id ("": the game's), and whether the local bank has it
    bool voice_local;
    bool face_skip;         // his expression's indices are not for the body drawn here (his own model is not the one shown)
    double active_ms;       // when the player last moved or changed what they were doing (who gets room for custom models first)
    double stock_until_ms;  // an eviction: stock until then (room was given to a more active player)
    double retry_custom_ms; // then the custom models are asked for again
    int last_proc;      // the proc last drawn (-1: none yet), to hear when it changes
    bool model_changed; // a new body model arrived: a puppet already made is made again
    float voice_pitch;  // the owner's Link Voice Pitch as a ratio (0: normal)
    bool map;           // in my stage and room: an arrow on the minimap, whether or not the puppet is drawn
    uint8_t color[3];   // the player's color (the arrow's)
    bool chart;         // an arrow on the Sea Chart, wherever they are
    float chart_x, chart_z;
    int chart_angle;
} Remote;

// What the game thread has made of a remote: its guest objects.
typedef struct Guest {
    u32 heap, model, tevstr, pos;
    u32 blade[2]; // the blade models (Hero's, Master Sword), from Link's archive
    u32 sword[2], shield[2], hands; // Link's sword, Master Sword, shield, Mirror Shield and hands models, made with the puppet
    u32 katsura;    // casual clothes' back hair: a second model on the head joint (Link's own data only)
    u32 model_data; // the model data the puppet's model is made from: the game's Link or the player's own
    unsigned tick;           // frames drawn (far puppets are posed every other one)
    int ds[EQUIP_SLOTS + 1]; // shared data slots held (index + 1; 0 none): [0..7] equipment, hands and hair, [8] the body
    bool denied;             // custom models were asked for and there was no room: made stock; asked again after a while
    double denied_at;
    bool custom;
    // A player's own model keeps its own bone lengths: the matrices sent are Link's, so each joint
    // whose translation (from its parent) is just Link's bind translation takes the model's own.
    bool remap;
    int parent[CL_JOINTS], order[CL_JOINTS];
    float bind_link[CL_JOINTS][3], bind_own[CL_JOINTS][3];
    u32 shadow_key; // the real shadow's key (dDlst_shadowControl_c), 0 until one is set
    int id;         // the player's id (the texture variant and draw identity of its models)
    // Models of what he holds and wears, made when first seen (each in a heap of its own, freed with the puppet).
    struct AttModel {
        char arc[ATT_ARC];
        u16 idx;
        u8 dup; // the nth of the same resource in one pose (the two boots): a model is entered once a frame
        u32 heap, model, ptex, skin; // skin: the hull's J3DSkinDeform
        bool failed;
        int retry;
        unsigned used; // the draw counter when last drawn
    } att[ATT_MODELS];
    u32 sail_heap, sail_pkt, sail_zmtx; // the puppet's sail packet (draw_sail), and the heap it and the phases live in
    u32 sail_phase;                     // two request_of_phase_process_class (Cloth, Ship) while it holds those archives
    int sail_tries, sail_said;
    bool sail_built, sail_given_up;
    u32 line[2];  // mDoExt_3DlineMat1_c objects (made with the puppet): the chain and the rope
    u32 var[2][3]; // the packets that set (1) and clear (0) the texture variant in P0 and P1's opaque and translucent lists
    u32 packet[4]; // own copies of daPy's cup/aup state packets (entering Link's own would link a list into a loop)
    bool failed;
    int retry; // frames since a failed attempt
} Guest;

static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static Remote g_remote[MAX_REMOTES];
static Guest g_guest[MAX_REMOTES];
static atomic_bool g_capture, g_draw;
static atomic_bool g_sounds = true;
static uint8_t g_local_wire[BLUEWAKE_PUPPET_MAX_WIRE];
static size_t g_local_length;
static uint32_t g_local_seq;
static float g_self_offset;
static atomic_bool g_self_on;
static u32 g_scene_id;
enum { MAP_CURSOR_SIZE = 0x28u, MAP_ICONS = 16, CHART_MEM_SIZE = 0x2000 };
static Pose g_cap_pose;   // the local pose waiting for the draw fiber to add the item held up in an item get
static bool g_cap_pending;
static u32 g_scratch;     // a few words of guest memory for calls (made by the draw fiber)
static u32 g_chart_mem;   // the Sea Chart's J2D copies (guest memory, made by the draw fiber)
static u32 g_map_cursors; // MAP_ICONS copies of the minimap's cursor object (guest memory, made by the draw fiber)
static GuestCoro* g_coro_draw;
static GuestCoro* g_coro_delete;

static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {
    }
}
static void unlock(void) { atomic_flag_clear_explicit(&g_lock, memory_order_release); }

static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    return 0.0;
#endif
}

void bluewake_puppet_set_sounds(bool on) { atomic_store(&g_sounds, on); }

void bluewake_puppet_enable(bool capture, bool draw) {
    atomic_store(&g_capture, capture);
    atomic_store(&g_draw, draw);
}

size_t bluewake_puppet_local_pose(uint8_t* out, size_t capacity, uint32_t* seq) {
    lock();
    size_t n = g_local_length;
    if (n > capacity)
        n = 0;
    if (n != 0) {
        memcpy(out, g_local_wire, n);
        *seq = g_local_seq;
    }
    unlock();
    return n;
}

static Remote* find_remote(int id, bool create) {
    Remote* free_slot = NULL;
    for (int i = 0; i < MAX_REMOTES; ++i) {
        if (g_remote[i].id == id)
            return &g_remote[i];
        if (g_remote[i].id == 0 && free_slot == NULL)
            free_slot = &g_remote[i];
    }
    if (create && free_slot == NULL) {
        // All taken (players who crashed and came back get a new id each time, and offline ones keep theirs): the one
        // heard from longest ago gives its slot up. Full slots left a newcomer with a nametag and no puppet.
        double oldest = 0.0;
        for (int i = 0; i < MAX_REMOTES; ++i) {
            const double t = g_remote[i].count > 0 ? g_remote[i].snap[g_remote[i].count - 1].time : 0.0;
            if (free_slot == NULL || t < oldest) {
                free_slot = &g_remote[i];
                oldest = t;
            }
        }
        free(free_slot->bdl);
        for (int k = 0; k < EQUIP_SLOTS; ++k)
            free(free_slot->eq[k]);
    }
    if (create && free_slot != NULL) {
        memset(free_slot, 0, sizeof *free_slot);
        free_slot->id = id;
        free_slot->last_proc = -1;
        // A slot used before still holds that player's puppet (model, colours): made again for this one.
        free_slot->model_changed = true;
        return free_slot;
    }
    return NULL;
}

static void remote_push(int id, const Pose* pose) {
    lock();
    Remote* r = find_remote(id, true);
    if (r != NULL) {
        if (r->count == SNAPS) {
            memmove(&r->snap[0], &r->snap[1], sizeof(Snap) * (SNAPS - 1));
            r->count = SNAPS - 1;
        }
        if (r->count > 0) {
            const Pose* last = &r->snap[r->count - 1].pose;
            const float dx = pose->base_pos[0] - last->base_pos[0], dz = pose->base_pos[2] - last->base_pos[2];
            if (dx * dx + dz * dz > 4.0f || pose->proc != last->proc || pose->flags != last->flags)
                r->active_ms = now_ms();
        } else
            r->active_ms = now_ms();
        Snap* sn = &r->snap[r->count];
        sn->time = now_ms();
        sn->stime = -1.0;
        if (pose->has_time) {
            if (r->have_clock)
                r->stime_acc += (double)(int32_t)(pose->wtime - r->last_wtime);
            else
                r->stime_acc = 0.0;
            r->last_wtime = pose->wtime;
            sn->stime = r->stime_acc;
            const double sample = sn->time - sn->stime;
            if (!r->have_clock || sample < r->clock_off)
                r->clock_off = sample;
            else
                r->clock_off += (sample - r->clock_off) * 0.02; // a slow climb, for a clock that runs a little differently
            r->have_clock = true;
        }
        sn->pose = *pose;
        ++r->count;
    }
    unlock();
}

void bluewake_puppet_remote_update(int id, const uint8_t* wire, size_t length) {
    static Pose pose; // the app's thread only
    if (!wire_decode(wire, length, &pose))
        return;
    remote_push(id, &pose);
}

void bluewake_puppet_remote_model(int id, const uint8_t* bdl, size_t length) {
    uint8_t* copy = NULL;
    if (bdl != NULL && length != 0 && length <= MAX_CUSTOM_BDL) {
        copy = (uint8_t*)malloc(length);
        if (copy != NULL)
            memcpy(copy, bdl, length);
        else
            length = 0;
    } else
        length = 0;
    lock();
    Remote* r = find_remote(id, copy != NULL);
    uint8_t* old = NULL;
    if (r != NULL) {
        old = r->bdl;
        r->bdl = copy;
        r->bdl_len = length;
        r->model_changed = true;
        copy = NULL;
    }
    unlock();
    free(old);
    free(copy);
}

void bluewake_puppet_remote_equipment(int id, int slot, const uint8_t* bdl, size_t length) {
    if (slot < 0 || slot >= EQUIP_SLOTS)
        return;
    uint8_t* copy = NULL;
    if (bdl != NULL && length != 0 && length <= MAX_CUSTOM_BDL) {
        copy = (uint8_t*)malloc(length);
        if (copy != NULL)
            memcpy(copy, bdl, length);
        else
            length = 0;
    } else
        length = 0;
    lock();
    Remote* r = find_remote(id, copy != NULL);
    uint8_t* old = NULL;
    if (r != NULL) {
        old = r->eq[slot];
        if (old != NULL || copy != NULL)
            r->model_changed = true; // the puppet is made again with its equipment
        r->eq[slot] = copy;
        r->eq_len[slot] = length;
        copy = NULL;
    }
    unlock();
    free(old);
    free(copy);
}

void bluewake_puppet_remote_voice(int id, const char* voice_id, bool local_bank) {
    lock();
    Remote* r = find_remote(id, true);
    if (r != NULL) {
        snprintf(r->voice, sizeof r->voice, "%s", voice_id != NULL ? voice_id : "");
        r->voice_local = local_bank;
    }
    unlock();
}

void bluewake_puppet_remote_face_ok(int id, bool ok) {
    lock();
    Remote* r = find_remote(id, true);
    if (r != NULL)
        r->face_skip = !ok;
    unlock();
}

void bluewake_puppet_remote_voice_pitch(int id, float ratio) {
    lock();
    Remote* r = find_remote(id, true);
    if (r != NULL)
        r->voice_pitch = ratio;
    unlock();
}

void bluewake_puppet_remote_show(int id, bool show) {
    lock();
    Remote* r = find_remote(id, show);
    if (r != NULL)
        r->show = show;
    unlock();
}

void bluewake_puppet_remote_map(int id, bool on, uint8_t r, uint8_t g, uint8_t b) {
    lock();
    Remote* rm = find_remote(id, on);
    if (rm != NULL) {
        if (rm->map != on && getenv("BLUEWAKE_PUPPET_MAP_LOG") != NULL)
            fprintf(stderr, "[puppet] minimap: player %d %s\n", id, on ? "on the map" : "off the map");
        rm->map = on;
        rm->color[0] = r;
        rm->color[1] = g;
        rm->color[2] = b;
    }
    unlock();
}

void bluewake_puppet_remote_chart(int id, bool on, float x, float z, int angle, uint8_t r, uint8_t g, uint8_t b) {
    lock();
    Remote* rm = find_remote(id, on);
    if (rm != NULL) {
        rm->chart = on;
        rm->chart_x = x;
        rm->chart_z = z;
        rm->chart_angle = angle;
        rm->color[0] = r;
        rm->color[1] = g;
        rm->color[2] = b;
    }
    unlock();
}

void bluewake_puppet_remote_remove(int id) {
    lock();
    Remote* r = find_remote(id, false);
    uint8_t* bdl = NULL;
    uint8_t* eq[EQUIP_SLOTS] = {NULL};
    if (r != NULL) {
        bdl = r->bdl;
        for (int k = 0; k < EQUIP_SLOTS; ++k)
            eq[k] = r->eq[k];
        memset(r, 0, sizeof *r);
    }
    unlock();
    free(bdl);
    for (int k = 0; k < EQUIP_SLOTS; ++k)
        free(eq[k]);
}

void bluewake_puppet_remote_clear(void) {
    uint8_t* bdl[MAX_REMOTES];
    uint8_t* eq[MAX_REMOTES][EQUIP_SLOTS];
    lock();
    for (int i = 0; i < MAX_REMOTES; ++i) {
        bdl[i] = g_remote[i].bdl;
        for (int k = 0; k < EQUIP_SLOTS; ++k)
            eq[i][k] = g_remote[i].eq[k];
    }
    memset(g_remote, 0, sizeof g_remote);
    unlock();
    for (int i = 0; i < MAX_REMOTES; ++i) {
        free(bdl[i]);
        for (int k = 0; k < EQUIP_SLOTS; ++k)
            free(eq[i][k]);
    }
}

bool bluewake_puppet_drawn(int id, float head[3]) {
    bool drawn = false;
    lock();
    const Remote* r = find_remote(id, false);
    if (r != NULL && r->drawn) {
        drawn = true;
        memcpy(head, r->head, sizeof r->head);
    }
    unlock();
    return drawn;
}

bool bluewake_puppet_tag(int id, float weight, float ndc[3]) {
    bool ok = false;
    lock();
    const Remote* r = find_remote(id, false);
    if (r != NULL && r->drawn && r->clips != 0) {
        // Each frame's head against its own camera, then between the two: the in-between frame the renderer shows first is
        // halfway, the real one after it is the newer.
        float p[2][2];
        ok = r->clip[0][2] > 1.0f && r->clip[1][2] > 1.0f;
        for (int k = 0; k < 2 && ok; ++k) {
            p[k][0] = r->clip[k][0] / r->clip[k][2];
            p[k][1] = r->clip[k][1] / r->clip[k][2];
        }
        if (ok) {
            ndc[0] = p[0][0] + (p[1][0] - p[0][0]) * weight;
            ndc[1] = p[0][1] + (p[1][1] - p[0][1]) * weight;
            ndc[2] = r->clip[1][2];
        }
    }
    unlock();
    return ok;
}

void bluewake_puppet_self_test(float offset) {
    g_self_offset = offset;
    atomic_store(&g_self_on, true);
}

// ------------------------------------------------------------- capture ----

// His expression: the texture numbers his material animation (eyes, brows, mouth) left in the materials of his model
// this frame. Every material that has a texture-number animation on one of its slots is listed with that slot's number.
static void capture_face(CPUState* cpu, u32 data, Pose* pose) {
    const u32 count = mem_read16(cpu, data + MODEL_DATA_MATERIAL_NUM);
    const u32 mats = mem_read32(cpu, data + MODEL_DATA_MATERIALS);
    pose->face_n = 0;
    if (!guest_ptr(mats) || count > 64u)
        return;
    for (u32 i = 0; i < count && pose->face_n < PUPPET_FACE_MAX; ++i) {
        const u32 mat = mem_read32(cpu, mats + 4u * i);
        const u32 anm = guest_ptr(mat) ? mem_read32(cpu, mat + MAT_ANM) : 0u;
        const u32 tev = guest_ptr(mat) ? mem_read32(cpu, mat + MAT_TEV_BLOCK) : 0u;
        if (!guest_ptr(anm) || !guest_ptr(tev))
            continue;
        for (u32 k = 0; k < 8u && pose->face_n < PUPPET_FACE_MAX; ++k) {
            if (!guest_ptr(mem_read32(cpu, anm + MATANM_TEXNO + 4u * k)))
                continue;
            FaceTex* f = &pose->face[pose->face_n++];
            f->mat = (uint8_t)i;
            f->slot = (uint8_t)k;
            f->tex = (uint16_t)mem_read16(cpu, tev + TEV_BLOCK_TEXNO + 2u * k);
        }
    }
}

// Which resource a model data is: the archive and the index in it (the game's resource table, mObjectInfo, lists the
// loaded archives and each one's resources, whose model data are the ones its models are made from). Models built some
// other way are not found and not sent. Answers are kept by data pointer until the scene changes.
typedef struct ModelId {
    u32 data;
    char arc[ATT_ARC];
    u16 idx;
    bool found;
} ModelId;
static ModelId g_model_id[48];
static int g_model_ids;

static bool identify_model(CPUState* cpu, u32 data, char arc[ATT_ARC], u16* idx) {
    for (int i = 0; i < g_model_ids; ++i)
        if (g_model_id[i].data == data) {
            if (g_model_id[i].found) {
                memcpy(arc, g_model_id[i].arc, ATT_ARC);
                *idx = g_model_id[i].idx;
            }
            return g_model_id[i].found;
        }
    ModelId id;
    memset(&id, 0, sizeof id);
    id.data = data;
    for (int i = 0; i < RES_OBJECT_INFO_NUM && !id.found; ++i) {
        const u32 info = RES_OBJECT_INFO + RES_INFO_SIZE * (u32)i;
        const u32 archive = mem_read32(cpu, info + 0x14u);
        const u32 res = mem_read32(cpu, info + 0x20u);
        if (mem_read16(cpu, info + 0x0Eu) == 0u || !guest_ptr(archive) || !guest_ptr(res))
            continue;
        const u32 n = mem_read32(cpu, archive + ARCHIVE_ENTRY_NUM);
        for (u32 j = 0; j < n && j < 4096u; ++j)
            if (mem_read32(cpu, res + 4u * j) == data) {
                for (int c = 0; c < ATT_ARC - 1; ++c)
                    id.arc[c] = (char)mem_read8(cpu, info + (u32)c);
                id.arc[ATT_ARC - 1] = '\0';
                id.idx = (u16)j;
                id.found = true;
                break;
            }
    }
    if (g_model_ids == (int)(sizeof g_model_id / sizeof g_model_id[0]))
        g_model_ids = 0;
    g_model_id[g_model_ids++] = id;
    if (id.found)
        fprintf(stderr, "[puppet] model data %08X is %s resource %u\n", data, id.arc, (unsigned)id.idx);
    else
        fprintf(stderr, "[puppet] model data %08X is in no loaded archive: not sent\n", data);
    if (id.found) {
        memcpy(arc, id.arc, ATT_ARC);
        *idx = id.idx;
    }
    return id.found;
}

// A model of his in the pose: its resource, its base matrix and its joints' matrices relative to his base.
static bool capture_att(CPUState* cpu, u32 model, const Mat34* inv_base, uint8_t kind, Pose* pose) {
    if (pose->att_n >= ATT_MAX || !guest_ptr(model))
        return false;
    const u32 data = mem_read32(cpu, model + MODEL_DATA);
    const u32 nodes = guest_ptr(data) ? mem_read32(cpu, model + MODEL_MTX_BUF) : 0u;
    const u32 nj = guest_ptr(data) ? mem_read16(cpu, data + MODEL_DATA_JOINT_NUM) : 0u;
    if (!guest_ptr(nodes) || nj == 0u || nj > ATT_JOINTS)
        return false;
    PoseAtt* a = &pose->att[pose->att_n];
    memset(a, 0, sizeof *a);
    if (!identify_model(cpu, data, a->arc, &a->idx))
        return false;
    Mat34 m, rel;
    read_mat(cpu, model + MODEL_BASE_MTX, &m);
    mat_mul(inv_base, &m, &rel);
    xf_from_mat(&rel, &a->base);
    for (u32 j = 0; j < nj; ++j) {
        read_mat(cpu, nodes + 48u * j, &m);
        mat_mul(inv_base, &m, &rel);
        xf_from_mat(&rel, &a->joint[j]);
    }
    a->kind = kind;
    a->nj = (uint8_t)nj;
    ++pose->att_n;
    return true;
}

// A point of the world relative to his base.
static void rel_point(CPUState* cpu, u32 address, const Mat34* inv_base, float out[3]) {
    float w[3];
    for (int i = 0; i < 3; ++i)
        w[i] = read_float(cpu, address + 4u * (u32)i);
    for (int i = 0; i < 3; ++i)
        out[i] = inv_base->m[i][0] * w[0] + inv_base->m[i][1] * w[1] + inv_base->m[i][2] * w[2] + inv_base->m[i][3];
}

// Attachments (held items, boots, the charm, carried objects, the ship, the chain and rope lines) are off unless
// BLUEWAKE_PUPPET_ATTACHMENTS=1: in the 2026.10.17c tests they could hide the whole puppet, and the rope strips showed
// as a striped board beside it. Expressions, shadows and the smoothing stay on.
// What a puppet carries and rides (items, boots, charm, the ship and its sail): on unless BLUEWAKE_PUPPET_ATTACHMENTS=0.
// A test variable read once per place, not every frame (getenv scans the environment).
#define ENV_SET(name) ({ static signed char set_ = -1; if (set_ < 0) set_ = getenv(name) != NULL; set_ != 0; })

static bool attachments_on(void) {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("BLUEWAKE_PUPPET_ATTACHMENTS");
        on = !(e != NULL && e[0] == '0');
    }
    return on != 0;
}

// The hookshot's chain and the grappling hook's rope: off unless BLUEWAKE_PUPPET_LINES=1 (a stale line drew a striped
// board beside the puppet).
static bool lines_on(void) {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("BLUEWAKE_PUPPET_LINES");
        on = e != NULL && e[0] == '1';
    }
    return on != 0;
}

// The hookshot's chain (from his hand to the hookshot's head while it is out) and the grappling hook's rope.
static void capture_lines(CPUState* cpu, u32 player, const Mat34* inv_base, Pose* pose) {
    pose->line_n = 0;
    if (getenv("BLUEWAKE_PUPPET_LINETEST") != NULL) { // diagnosis: a chain and a rope out in front of his hands
        for (int k = 0; k < 2; ++k) {
            pose->line[k].kind = (uint8_t)(k + 1);
            for (int i = 0; i < 3; ++i) {
                pose->line[k].p[0][i] = pose->joint[CL_JNT_RHANDA].t[i];
                pose->line[k].p[1][i] = pose->joint[CL_JNT_RHANDA].t[i];
            }
            pose->line[k].p[1][2] += 150.0f;
            pose->line[k].p[1][0] += k == 0 ? -60.0f : 60.0f;
        }
        pose->line_n = 2;
        return;
    }
    const u32 item = mem_read16(cpu, player + PLAYER_EQUIP_ITEM);
    const u32 hook = mem_read32(cpu, player + PLAYER_KEEP_EQUIP + 4u);
    if (item == ITEM_HOOKSHOT && guest_ptr(hook) && (int)mem_read32(cpu, hook + HOOKSHOT_CHAIN_COUNT) > 0 &&
        (int)mem_read32(cpu, hook + HOOKSHOT_CHAIN_COUNT) < 200) {
        pose->line[0].kind = 1;
        rel_point(cpu, player + PLAYER_HOOKSHOT_ROOT, inv_base, pose->line[0].p[0]);
        rel_point(cpu, hook + ACTOR_CURRENT_POS, inv_base, pose->line[0].p[1]);
        pose->line_n = 1;
    }
    const u32 rope = mem_read32(cpu, player + PLAYER_KEEP_ROPE + 4u);
    // Only with the grappling hook in his hand (a stale actor in the slot made a line to nowhere).
    if (item == ITEM_GRAPPLE && guest_ptr(rope) && mem_read32(cpu, player + PLAYER_KEEP_ROPE) != 0xFFFFFFFFu && pose->line_n < 2) {
        struct PoseLine* l = &pose->line[pose->line_n++];
        l->kind = 2;
        const Xf* hand = &pose->joint[CL_JNT_RHANDA];
        for (int i = 0; i < 3; ++i)
            l->p[0][i] = hand->t[i];
        rel_point(cpu, rope + ACTOR_CURRENT_POS, inv_base, l->p[1]);
    }
}

static void capture_attachments(CPUState* cpu, u32 player, const Mat34* inv_base, Pose* pose) {
    pose->att_n = 0;
    pose->line_n = 0;
    if (!attachments_on() || ENV_SET("BLUEWAKE_PUPPET_NOATT"))
        return;
    if (lines_on())
        capture_lines(cpu, player, inv_base, pose);
    // Sailing: the King of Red Lions, the ship actor's body (hull, mast, sail and tiller, by its joints) and head.
    const u32 ship = mem_read32(cpu, SHIP_ACTOR);
    if ((mem_read32(cpu, PLAY_STATUS0) & PLAY_STATUS0_SHIP_RIDE) != 0u && guest_ptr(ship))
        for (u32 k = 0; k < 2u; ++k) {
            const u32 morf = mem_read32(cpu, ship + SHIP_BODY_ANM + 4u * k);
            if (guest_ptr(morf))
                capture_att(cpu, mem_read32(cpu, morf + MORF_MODEL), inv_base, k == 0u ? ATT_KIND_SHIP_BODY : ATT_KIND_SHIP_HEAD, pose);
        }
    pose->sail = false;
    static int sail_sent_state = -1; // 0 not on the ship, 1 on it without a sail actor, 2 sail furled, 3 sail sent
    int sail_now = 0;
    if ((mem_read32(cpu, PLAY_STATUS0) & PLAY_STATUS0_SHIP_RIDE) != 0u && guest_ptr(ship)) {
        const u32 grid = mem_read32(cpu, ship + SHIP_GRID);
        sail_now = !guest_ptr(grid) ? 1 : read_float(cpu, grid + GRID_SCALE_Y) < 0.06f ? 2 : 3;
        if (guest_ptr(grid) && read_float(cpu, grid + GRID_SCALE_Y) >= 0.06f) {
            // The packet's matrix is the view times the sail's place: undone with the view the sail was drawn with.
            const u32 pk = grid + GRID_PACKET;
            Mat34 view, view_inv, mm, world, rel;
            read_mat(cpu, J3D_SYS, &view);
            read_mat(cpu, pk + HO_MTX, &mm);
            if (mat_inverse(&view, &view_inv)) {
                mat_mul(&view_inv, &mm, &world);
                mat_mul(inv_base, &world, &rel);
                xf_from_mat(&rel, &pose->sail_xf);
                const u32 cur = mem_read8(cpu, pk + HO_CUR) & 1u;
                for (int i = 0; i < HO_POINTS; ++i)
                    for (int k = 0; k < 3; ++k)
                        pose->sail_pt[i][k] = t8(read_float(cpu, pk + HO_POS + cur * 12u * HO_POINTS + 12u * (u32)i + 4u * (u32)k));
                pose->sail = true;
            }
        }
    }
    if (sail_now != sail_sent_state) {
        sail_sent_state = sail_now;
        static const char* const kWhy[4] = {"not on the ship", "on the ship, which has no sail actor (nothing to send)", "sail furled (scale.y below 0.06): not sent",
                                            "sail hoisted: sent with the pose"};
        fprintf(stderr, "[puppet] sail: %s%s\n", kWhy[sail_now], sail_now == 3 && !pose->sail ? " (but its matrix could not be undone: not sent)" : "");
    }
    const u32 equip_item = mem_read16(cpu, player + PLAYER_EQUIP_ITEM);
    const u32 item_model = mem_read32(cpu, player + PLAYER_EQUIP_ITEM_MODEL);
    if (equip_item != 0x103u) // the sword's blade is its own flag
        capture_att(cpu, item_model, inv_base, ATT_KIND_ITEM, pose);
    if ((mem_read32(cpu, player + PLAYER_NO_RESET_FLG0) & PLAYER_FLG0_HEAVY_BOOTS) != 0u)
        for (u32 k = 0; k < 2u; ++k)
            capture_att(cpu, mem_read32(cpu, player + PLAYER_HBOOTS + 4u * k), inv_base, ATT_KIND_BOOTS, pose);
    if ((mem_read8(cpu, SAVE_COLLECT4) & 2u) != 0u && !bluewake_bunny_hood_worn())
        capture_att(cpu, mem_read32(cpu, player + PLAYER_YAMU_MODEL), inv_base, ATT_KIND_CHARM, pose);
    // What he carries and the item actor in his hand (a Deku stick, a pot, a bomb), by the actors' own models.
    static const struct { u32 keep; uint8_t kind; } kActors[3] = {
        {PLAYER_KEEP_GRAB, ATT_KIND_GRAB}, {PLAYER_KEEP_EQUIP, ATT_KIND_EQUIP}, {PLAYER_KEEP_THROW, ATT_KIND_THROW}};
    for (int k = 0; k < 3; ++k) {
        const u32 actor = mem_read32(cpu, player + kActors[k].keep + 4u);
        if (guest_ptr(actor) && mem_read32(cpu, player + kActors[k].keep) != 0xFFFFFFFFu)
            capture_att(cpu, mem_read32(cpu, actor + ACTOR_MODEL), inv_base, kActors[k].kind, pose);
    }
}

// Link's pose, from his model as the game has calculated it this frame.
static bool capture_pose(CPUState* cpu, Pose* pose) {
    const u32 player = mem_read32(cpu, PLAYER);
    if (!guest_ptr(player))
        return false;
    const u32 model = mem_read32(cpu, player + PLAYER_CL_MODEL);
    if (!guest_ptr(model))
        return false;
    const u32 nodes = mem_read32(cpu, model + MODEL_NODE_MTX);
    if (!guest_ptr(nodes))
        return false;
    Mat34 base, inv;
    read_mat(cpu, model + MODEL_BASE_MTX, &base);
    if (!mat_inverse(&base, &inv))
        return false;
    Xf bx;
    xf_from_mat(&base, &bx);
    memcpy(pose->base_pos, bx.t, sizeof bx.t);
    memcpy(pose->base_q, bx.q, sizeof bx.q);
    for (int i = 0; i < CL_JOINTS; ++i) {
        Mat34 anm, local;
        read_mat(cpu, nodes + 48u * (u32)i, &anm);
        mat_mul(&inv, &anm, &local);
        xf_from_mat(&local, &pose->joint[i]);
        static int reports, once;
        if (once < 1 && i == CL_JOINTS - 1) {
            ++once;
            for (int j = 0; j < CL_JOINTS; ++j) {
                const Xf* x = &pose->joint[j];
                fprintf(stderr, "[puppet] joint %d q %.3f %.3f %.3f %.3f t %.1f %.1f %.1f s %.3f %.3f %.3f\n", j, x->q[0], x->q[1], x->q[2], x->q[3],
                        x->t[0], x->t[1], x->t[2], x->s[0], x->s[1], x->s[2]);
            }
        }
        if (reports < 60) {
            Mat34 back;
            mat_from_xf(&pose->joint[i], &back);
            float err = 0.0f;
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 4; ++b)
                    err = fmaxf(err, fabsf(back.m[a][b] - local.m[a][b]));
            if (err > 0.05f) {
                ++reports;
                fprintf(stderr, "[puppet] joint %d does not round-trip: error %.3f scale %.3f %.3f %.3f\n", i, err,
                        pose->joint[i].s[0], pose->joint[i].s[1], pose->joint[i].s[2]);
            }
        }
    }
    pose->flags = (mem_read32(cpu, player + PLAYER_NO_RESET_FLG1) & PLAYER_FLG1_CASUAL) ? PUPPET_FLAG_CASUAL : 0;
    pose->sword = pose->shield = pose->hands = 0;
    pose->proc = (uint16_t)mem_read32(cpu, player + PLAYER_CUR_PROC);
    // Equipment, as his draw has it this frame (daPy_lk_c::draw, after the process loop): the models he
    // picked, and where he put them.
    const u32 root = mem_read32(cpu, mem_read32(cpu, model + MODEL_DATA) + 0x2Cu);
    if (guest_ptr(root)) {
        const u32 poda = mem_read32(cpu, root + 4u * CL_JNT_PODA);
        const u32 mesh = guest_ptr(poda) ? mem_read32(cpu, poda + 0x60u) : 0u;
        const u32 shape = guest_ptr(mesh) ? mem_read32(cpu, mesh + 8u) : 0u;
        if (guest_ptr(shape) && (mem_read32(cpu, shape + 0x0Cu) & 1u) == 0u)
            pose->flags |= PUPPET_FLAG_SCABBARD;
    }
    if (bluewake_bunny_hood_worn())
        pose->flags |= PUPPET_FLAG_HOOD;
    if (mem_read16(cpu, player + PLAYER_EQUIP_ITEM) == 0x103u && guest_ptr(mem_read32(cpu, player + PLAYER_EQUIP_ITEM_MODEL)))
        pose->flags |= PUPPET_FLAG_BLADE;
    for (int e = 0; e < 2; ++e) {
        const u32 chosen = mem_read32(cpu, player + (e == 0 ? PLAYER_EQUIP_SWORD : PLAYER_EQUIP_SHIELD));
        const u32 plain = mem_read32(cpu, player + (e == 0 ? PLAYER_SWGRIPA : PLAYER_SHA));
        const bool equipped = mem_read8(cpu, SAVE_SELECT_SWORD + (u32)e) != 0xFFu;
        if (!equipped || !guest_ptr(chosen))
            continue;
        Mat34 m, rel;
        read_mat(cpu, chosen + MODEL_BASE_MTX, &m);
        mat_mul(&inv, &m, &rel);
        xf_from_mat(&rel, e == 0 ? &pose->sword_xf : &pose->shield_xf);
        (e == 0 ? &pose->sword : &pose->shield)[0] = chosen == plain ? 1 : 2;
    }
    // The hand shapes: one of the hands model's joints, or the body's own (0).
    const u32 hands_model = mem_read32(cpu, player + PLAYER_HANDS_MODEL);
    const u32 hands_data = guest_ptr(hands_model) ? mem_read32(cpu, hands_model + MODEL_DATA) : 0u;
    const u32 hands_joints = guest_ptr(hands_data) ? mem_read32(cpu, hands_data + 0x2Cu) : 0u;
    for (int side = 0; side < 2 && guest_ptr(hands_joints); ++side) {
        const u32 want = mem_read32(cpu, player + PLAYER_LHAND_SHAPE + 4u * (u32)side);
        u32 joint_no = 0;
        for (u32 j = 1; j <= 10u && joint_no == 0; ++j) {
            const u32 joint = mem_read32(cpu, hands_joints + 4u * j);
            const u32 m = guest_ptr(joint) ? mem_read32(cpu, joint + 0x60u) : 0u;
            if (guest_ptr(m) && mem_read32(cpu, m + 8u) == want)
                joint_no = j;
        }
        pose->hands |= (uint8_t)(joint_no << (4 * side));
    }
    capture_face(cpu, mem_read32(cpu, model + MODEL_DATA), pose);
    capture_attachments(cpu, player, &inv, pose);
    pose->has_time = true;
    pose->wtime = (uint32_t)now_ms();
    return true;
}

// The local pose to the wire (and, for the self test, back in as another player).
static void publish_local(const Pose* local) {
    uint8_t wire[BLUEWAKE_PUPPET_MAX_WIRE];
    const size_t n = wire_encode(local, wire);
    lock();
    memcpy(g_local_wire, wire, n);
    g_local_length = n;
    ++g_local_seq;
    unlock();
    if (atomic_load(&g_self_on)) {
        // Test: the same pose, some units aside, ten times a second.
        static double last;
        const double t = now_ms();
        if (t - last >= 100.0) {
            last = t;
            static Pose copy;
            copy = *local;
            copy.base_pos[0] += g_self_offset;
            if (getenv("BLUEWAKE_PUPPET_SELF_TURN") != NULL) { // facing the other way: a half turn about Y
                const float x = copy.base_q[0], y = copy.base_q[1], z = copy.base_q[2], w = copy.base_q[3];
                copy.base_q[0] = z;
                copy.base_q[1] = w;
                copy.base_q[2] = -x;
                copy.base_q[3] = -y;
            }
            uint8_t again[BLUEWAKE_PUPPET_MAX_WIRE];
            const size_t m = wire_encode(&copy, again);
            bluewake_puppet_remote_update(SELF_ID, again, m);
            bluewake_puppet_remote_show(SELF_ID, true);
            bluewake_puppet_remote_map(SELF_ID, true, 255, 40, 40); // the self test's arrow is red
        }
    }
}

// ------------------------------------------------------------- drawing ----

// How much of a puppet is done this frame, from where it is on the screen (puppet_view): the puppets off the screen are not
// posed, lit, entered or shadowed at all (the same pose is written again when they come back), and far ones are posed every
// other frame and have no shadow or hands model, and very far ones have no sword or shield. Near puppets are drawn as ever.
enum { LOD_NEAR = 0, LOD_FAR = 1, LOD_HIDDEN = 2, LOD_VERY_FAR = 3 };
typedef struct Item {
    uint8_t lod;
    int slot;
    Mat34 base;
    Mat34 node[CL_JOINTS];
    float pos[3];
    uint8_t flags, sword, shield, hands;
    int voice_proc; // the proc that just started (-1: none)
    int voice_force; // testing: the voice id to make (-1: the proc's)
    float voice_pitch; // its owner's voice pitch (ratio, 0: normal)
    Mat34 sword_mtx, shield_mtx; // world
    uint8_t* bdl; // a copy for the game thread to load, freed after (only for a puppet not yet made)
    size_t bdl_len;
    uint8_t* eq[EQUIP_SLOTS]; // the same for the equipment models
    size_t eq_len[EQUIP_SLOTS];
    char voice[40];
    bool voice_local;
    bool face_skip;   // see Remote
    bool body_wanted; // he has a model of his own to show (a puppet made without it is Link's, and his indices do not fit)
    double active_ms;
    bool recreate;
    int id; // the player's id: the texture variant its draws take
    uint8_t face_n;
    FaceTex face[PUPPET_FACE_MAX];
    bool sail;
    Mat34 sail_mtx; // world
    int16_t sail_pt[HO_POINTS][3];
    int line_n;
    struct { uint8_t kind; float p[2][3]; } line[2]; // world
    int att_n;
    struct ItemAtt {
        uint8_t kind, nj;
        char arc[ATT_ARC];
        u16 idx;
        Mat34 base;               // world
        Mat34 node[ATT_JOINTS];   // world
    } att[ATT_MAX];
} Item;

typedef struct Work {
    int count;
    Item item[MAX_REMOTES];
    u32 model_data; // the body's J3DModelData
    int room;
} Work;

static Work g_work;
static BluewakePuppetVoiceFn g_voice_player;

void bluewake_puppet_set_voice_player(BluewakePuppetVoiceFn fn) { g_voice_player = fn; }

// Where a puppet's voice is heard from, as the game's sound does it: full volume to 333 units, none from 1667 (JAIZelParam::
// MAX_VOLUME_DISTANCE and DISTANCE_MAX), and left or right by the side of the camera it is on. False when the camera is not known.
static bool voice_placement(CPUState* cpu, const float at[3], float* left, float* right) {
    const u32 view = mem_read32(cpu, 0x803CAB58u); // the camera's view_class
    if (!guest_ptr(view))
        return false;
    float eye[3], row[4];
    for (int i = 0; i < 3; ++i)
        eye[i] = read_float(cpu, view + 0x170u + 16u * (u32)i + 12u); // mInvViewMtx's translation
    for (int i = 0; i < 4; ++i)
        row[i] = read_float(cpu, view + 0x140u + 4u * (u32)i);        // mViewMtx's x row
    const float dx = at[0] - eye[0], dy = at[1] - eye[1], dz = at[2] - eye[2];
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    const float side = row[0] * at[0] + row[1] * at[1] + row[2] * at[2] + row[3];
    float volume = dist <= 333.0f ? 1.0f : dist >= 1667.0f ? 0.0f : 1.0f - (dist - 333.0f) / (1667.0f - 333.0f);
    float pan = dist > 1.0f ? side / dist : 0.0f;
    pan = pan < -1.0f ? -1.0f : pan > 1.0f ? 1.0f : pan;
    const float angle = (pan + 1.0f) * 0.78539816f; // constant power: the middle is 0.707 on both sides
    *left = volume * cosf(angle) * 1.41421356f;
    *right = volume * sinf(angle) * 1.41421356f;
    return volume > 0.0f;
}
unsigned long long bluewake_host_retrace_count(void); // main.c

// BLUEWAKE_PUPPET_PROF=1 (testing): the time each guest function the puppets call takes, and the phases of a draw, logged
// every 300 draws.
static int g_prof = -1;
static struct { u32 fn; unsigned n; double us; } g_prof_fn[64];
static double g_prof_phase[8];
static unsigned g_prof_lod[4];
static double prof_now_us(void) {
#ifdef _WIN32
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1.0e6 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1.0e6 + (double)ts.tv_nsec / 1.0e3;
#endif
}
static bool prof_on(void) {
    if (g_prof < 0)
        g_prof = getenv("BLUEWAKE_PUPPET_PROF") != NULL;
    return g_prof != 0;
}
static u32 call(GuestCoro* co, u32 fn, u32 a, u32 b, u32 c, u32 d) {
    if (!prof_on())
        return bluewake_guest_call(co, fn, a, b, c, d, 0, 0);
    const double t0 = prof_now_us();
    const u32 r = bluewake_guest_call(co, fn, a, b, c, d, 0, 0);
    const double dt = prof_now_us() - t0;
    int i = 0;
    while (i < 63 && g_prof_fn[i].fn != 0u && g_prof_fn[i].fn != fn)
        ++i;
    g_prof_fn[i].fn = fn;
    ++g_prof_fn[i].n;
    g_prof_fn[i].us += dt;
    return r;
}
static void prof_report(int draws, int puppets) {
    fprintf(stderr, "[prof] %d draws, %d puppets: phases us/draw: pose %.0f, calc+light %.0f, draw_puppet %.0f, shadow %.0f, total %.0f" "\n", draws, puppets,
            g_prof_phase[0] / draws, g_prof_phase[1] / draws, g_prof_phase[2] / draws, g_prof_phase[3] / draws, g_prof_phase[4] / draws);
    for (int i = 0; i < 64 && g_prof_fn[i].fn != 0u; ++i) {
        fprintf(stderr, "[prof]   fn %08X: %u calls/draw, %.0f us/draw" "\n", g_prof_fn[i].fn, (unsigned)(g_prof_fn[i].n / (unsigned)draws), g_prof_fn[i].us / draws);
        g_prof_fn[i].n = 0;
        g_prof_fn[i].us = 0.0;
    }
    fprintf(stderr, "[prof] per draw: %.1f near, %.1f far, %.1f very far, %.1f hidden" "\n", (double)g_prof_lod[0] / draws, (double)g_prof_lod[1] / draws, (double)g_prof_lod[3] / draws, (double)g_prof_lod[2] / draws);
    memset(g_prof_lod, 0, sizeof g_prof_lod);
    memset(g_prof_phase, 0, sizeof g_prof_phase);
}

// J3DModel::calc for a puppet's own model made from the local Link's model data (the hair, sword, shield, blade).
// The joints are the data's, shared with Link's models, and calcIn calls a joint's mMtxCalc (J3DJoint +0x58) when one
// is set: Link's code leaves its animation objects there, and when it frees or swaps them (Iron Boots on or off, the
// pause menu) a puppet calculated next went through the freed object's vtable (pc 0x80000000: two players' crashes,
// 2026-10-17). Without one, calcIn uses the model's basic calc, which calcAnmMtx sets: what a rigid part wants. The
// joints' pointers are put back for Link right after.
enum { JOINT_MTX_CALC = 0x58u, MODEL_DATA_JOINTS = 0x2Cu, MAX_SAVED_JOINTS = 64u };
static void calc_puppet_part(GuestCoro* co, CPUState* cpu, u32 model) {
    const u32 data = mem_read32(cpu, model + MODEL_DATA);
    const u32 joints = guest_ptr(data) ? mem_read32(cpu, data + MODEL_DATA_JOINTS) : 0u;
    u32 n = guest_ptr(joints) ? mem_read16(cpu, data + MODEL_DATA_JOINT_NUM) : 0u;
    if (n > MAX_SAVED_JOINTS)
        n = MAX_SAVED_JOINTS;
    u32 joint[MAX_SAVED_JOINTS], saved[MAX_SAVED_JOINTS];
    for (u32 i = 0; i < n; ++i) {
        joint[i] = mem_read32(cpu, joints + 4u * i);
        saved[i] = guest_ptr(joint[i]) ? mem_read32(cpu, joint[i] + JOINT_MTX_CALC) : 0u;
        if (saved[i] != 0u)
            mem_write32(cpu, joint[i] + JOINT_MTX_CALC, 0u);
    }
    call(co, FN_MODEL_CALC, model, 0, 0, 0);
    u32 cleared = 0u;
    for (u32 i = 0; i < n; ++i)
        if (saved[i] != 0u) {
            mem_write32(cpu, joint[i] + JOINT_MTX_CALC, saved[i]);
            ++cleared;
        }
    // The evidence that a puppet part met Link's animation objects (what the old calc ran): the first few, then a count.
    static unsigned s_seen, s_total;
    if (cleared != 0u) {
        ++s_total;
        if (s_seen < 4u) {
            ++s_seen;
            fprintf(stderr, "[puppet] part %08X: %u of its %u joints carried Link's animation calc (%08X); left out\n",
                    model, cleared, n, saved[0] != 0u ? saved[0] : saved[n > 1u ? 1u : 0u]);
        } else if ((s_total & 1023u) == 0u) {
            fprintf(stderr, "[puppet] parts that met Link's animation calc: %u\n", s_total);
        }
    }
}

// A model data's joint tree: each joint's parent, and its translation from the parent in the bind pose
// (J3DJoint: mChild +0x10, mYounger +0x14, mJntNo +0x18, mTransformInfo +0x1C, whose translation is at +0x14).
static bool read_skeleton(CPUState* cpu, u32 model_data, int parent[CL_JOINTS], float bind[CL_JOINTS][3]) {
    const u32 joints = mem_read32(cpu, model_data + 0x2Cu);
    if (!guest_ptr(joints) || mem_read16(cpu, model_data + 0x28u) != CL_JOINTS)
        return false;
    u32 ptr[CL_JOINTS];
    for (int j = 0; j < CL_JOINTS; ++j) {
        ptr[j] = mem_read32(cpu, joints + 4u * (u32)j);
        if (!guest_ptr(ptr[j]) || mem_read16(cpu, ptr[j] + 0x18u) != (u32)j)
            return false;
        parent[j] = -2;
        for (int k = 0; k < 3; ++k)
            bind[j][k] = read_float(cpu, ptr[j] + 0x30u + 4u * (u32)k);
    }
    parent[0] = -1;
    for (int j = 0; j < CL_JOINTS; ++j) {
        u32 c = mem_read32(cpu, ptr[j] + 0x10u);
        for (int guard = 0; guard < CL_JOINTS && guard_ptr_ok(c); ++guard, c = mem_read32(cpu, c + 0x14u)) {
            const u32 idx = mem_read16(cpu, c + 0x18u);
            if (idx >= CL_JOINTS || ptr[idx] != c)
                return false;
            parent[idx] = j;
        }
    }
    for (int j = 0; j < CL_JOINTS; ++j)
        if (parent[j] == -2)
            return false;
    return true;
}

// Each puppet's model packets name their own copy of the J3DTexture table (the same images), so a hook on
// J3DMatPacket::draw knows whose draw is next and tells the renderer (BP 0x79) which player's recolors of Link's
// textures to use. The shared display lists cannot say it.
enum { MAT_PACKET_DRAW = 0x802DB494u, MAT_PACKET_VTABLE = 0x8039D910u, MAT_PACKET_SIZE = 0x3Cu, MAT_PACKET_TEXTURE = 0x34u };
// (Each of a puppet's models has one: the body, the hair, the hands, the sword and shield, what it holds. The renderer's
// frame interpolation also takes the variant as the draw's identity, so a puppet's draws are not taken for Link's or
// another puppet's, which share the model's vertices.)
enum { PTEX_MAX = 24 };
static u32 g_ptex[MAX_REMOTES][PTEX_MAX]; // per puppet slot: its table copies
static int g_ptex_id[MAX_REMOTES];
static bool g_ptex_any;
static u32 g_ptex_last;

static u32 own_textures(GuestCoro* co, CPUState* cpu, int slot, int id, u32 model) {
    if (!guest_ptr(model))
        return 0u;
    const u32 data = mem_read32(cpu, model + 4u);
    const u32 tex = guest_ptr(data) ? mem_read32(cpu, data + 0x70u) : 0u;
    u32 packets = mem_read32(cpu, model + 0xB4u);
    if (!guest_ptr(tex) || !guest_ptr(packets))
        return 0u;
    u32 copy = 0u;
    for (int k = 0; k < PTEX_MAX && copy == 0u; ++k)
        if (g_ptex[slot][k] == 0u) {
            copy = call(co, FN_NEW, 8u, 0, 0, 0);
            if (!guest_ptr(copy))
                return 0u;
            mem_write32(cpu, copy, mem_read32(cpu, tex));
            mem_write32(cpu, copy + 4u, mem_read32(cpu, tex + 4u));
            g_ptex[slot][k] = copy;
        }
    if (copy == 0u)
        return 0u;
    int n = 0;
    for (u32 pk = packets; n < 256 && mem_read32(cpu, pk) == MAT_PACKET_VTABLE && mem_read32(cpu, pk + MAT_PACKET_TEXTURE) == tex;
         pk += MAT_PACKET_SIZE, ++n)
        mem_write32(cpu, pk + MAT_PACKET_TEXTURE, copy);
    g_ptex_id[slot] = id;
    g_ptex_any = true;
    fprintf(stderr, "[puppet] slot %d (player %d): %d material packets take their own texture table %08X\n", slot, id, n, copy);
    return copy;
}

// The lean module has no hook at J3DMatPacket::draw (the edge service that observed it does not run there), and the host does
// not see the draws, which the game's own code makes later from the lists. So the variant is set by the lists themselves: each
// puppet has two small display-list packets per list it draws into (P0, P1 opaque, P1 translucent, all drawn in the order they
// were entered, the last entered first): one that writes BP 0x79 = its id, entered after the puppet's models (so it is drawn
// before them), and one that writes 0, entered before them (drawn after). Whatever the puppet's materials draw in between, the
// renderer finds the player's own recolors for.
enum { FN_J3D_DRAW_PACKET_VT = 0x8039D928u, VAR_BLOCK = 0x60u + 6u * 0x24u };

bool bluewake_puppet_mat_observes(u32 address) {
    (void)address;
    return false;
}

void bluewake_puppet_mat_enter(CPUState* cpu, u32 address) {
    (void)cpu;
    (void)address;
}

static bool make_variant_packets(GuestCoro* co, CPUState* cpu, Guest* g, int id) {
    const u32 block = call(co, FN_NEW_ALIGNED, VAR_BLOCK, 0x20u, 0, 0);
    if (!guest_ptr(block))
        return false;
    for (u32 o = 0; o < VAR_BLOCK; o += 4u)
        mem_write32(cpu, block + o, 0u);
    // [0x00] the display list that sets the variant, [0x20] the one that clears it (a BP load, then no-ops to 32 bytes),
    // [0x40] their two J3DDisplayListObjs, [0x60] six J3DDrawPackets (the clearing ones, then the setting ones).
    mem_write8(cpu, block + 0x00u, 0x61u);
    mem_write8(cpu, block + 0x01u, 0x79u);
    mem_write8(cpu, block + 0x02u, (u8)((u32)id >> 16));
    mem_write8(cpu, block + 0x03u, (u8)((u32)id >> 8));
    mem_write8(cpu, block + 0x04u, (u8)id);
    mem_write8(cpu, block + 0x20u, 0x61u);
    mem_write8(cpu, block + 0x21u, 0x79u);
    for (u32 k = 0; k < 2u; ++k) { // obj 0 clears, obj 1 sets
        const u32 obj = block + 0x40u + 0x10u * k;
        const u32 dl = block + (k == 0u ? 0x20u : 0x00u);
        mem_write32(cpu, obj, dl);
        mem_write32(cpu, obj + 4u, dl);
        mem_write32(cpu, obj + 8u, 0x20u);
        mem_write32(cpu, obj + 0xCu, 0x20u);
    }
    for (u32 k = 0; k < 6u; ++k) {
        const u32 pk = block + 0x60u + 0x24u * k;
        mem_write32(cpu, pk, FN_J3D_DRAW_PACKET_VT);
        mem_write32(cpu, pk + 0x20u, block + 0x40u + 0x10u * (k / 3u));
        g->var[k / 3u][k % 3u] = pk;
    }
    dol_guest_memory_dirty_mark(block, VAR_BLOCK);
    return true;
}

// The packets of one kind (0 clearing, 1 setting) into the three lists a puppet draws in.
static void enter_variant_packets(GuestCoro* co, CPUState* cpu, const Guest* g, int which) {
    if (g->var[which][0] == 0u)
        return;
    const u32 lists[3] = {mem_read32(cpu, DRAW_LIST + 0x08u), mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1), mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1 + 4u)};
    for (int k = 0; k < 3; ++k)
        if (guest_ptr(lists[k]))
            call(co, FN_ENTRY_IMM, lists[k], g->var[which][k], 0, 0);
}

// Loads a BDL into the current heap with the game's own loader (as dRes loads a 'BDL ' resource); 0 on any failure.
static u32 load_custom_bdl(GuestCoro* co, CPUState* cpu, const uint8_t* bdl, size_t len) {
    const u32 buf = call(co, FN_NEW_ALIGNED, (u32)len, 0x20u, 0, 0);
    if (buf == 0u)
        return 0u;
    for (size_t i = 0; i < len; ++i)
        mem_write8(cpu, buf + (u32)i, bdl[i]);
    dol_guest_memory_dirty_mark(buf, (u32)len);
    return call(co, FN_LOAD_BDL, buf, BDL_LOAD_FLAGS, 0, 0);
}

// Loaded model data shared between puppets: a body or equipment model is loaded once into a heap of its own and every puppet
// that has the same file (the same bytes) makes its models from that data, so five puppets of one mod, or all the stock
// players on a PC whose own Link is a mod's, cost one copy of the data (300 to 400 KB for a body) and not five. A slot is
// freed with its last puppet (a scene's end, or the puppet made again).
enum { DS_SLOTS = 192 }; // 16 players with a body and seven equipment models of their own each, a hundred and some
typedef struct DataSlot {
    uint64_t key;
    size_t len;
    u32 heap, data;
    int refs;
} DataSlot;
static DataSlot g_ds[DS_SLOTS];

// ---- the puppets' pools: guest memory outside the game's own heaps ----
// The GameCube game has 24 MB and its heaps leave the puppets a few hundred KB (the game heap), 545 KB (the Zelda heap) and
// 1.6 MB of the archive heap, room for two custom bodies. So the puppets' heaps are also made in two pools, each one JKRExpHeap
// over a block of guest memory, made by the game's own constructor (a child of the root heap only in its tree: the block is no
// part of the root's range), from which the same solid heaps are made as before (mDoExt_createSolidHeapToCurrent with the pool
// as the parent). Everything a puppet allocates in such a heap, its models, packets and matrix buffers, is then in the pool.
//   FAST: memory inside the module's own MEM1 array, which the recompiled code reaches with its inline fast path: the 4 MB
//         at the top of the 32 MB guest window (0x81C00000, past the game's 24 MB and the host's own objects) of a 32 MB module, or
//         everything above 0x82000000 (up to 32 MB) of a 64 MB one (puppet_fast_range).
//   MEM2: the Wii's extended RAM (0x90000000, cpu->exram, 64 MB), which the module reaches through its slow path (a call per
//         access: a puppet there costs about twice the game-thread time), so it takes only what the others cannot hold.
// A pool is made on first use and kept for the run; a puppet's heap goes back to it when the puppet is deleted. Anything that
// goes wrong (no such memory, the pool does not take, no room left in it) leaves the puppet to the next tier, the last being the
// game's heaps as before.
enum {
    MEM2_BASE = 0x90000000u,
    POOL_OBJECT_SIZE = 0x100u,         // room for the JKRExpHeap object itself (it is about 0x80 bytes)
    JKR_SYSTEM_HEAP = 0x803F7728u,     // JKRHeap::sSystemHeap, sCurrentHeap follows, then sRootHeap
    JKR_CURRENT_HEAP = 0x803F772Cu,
    JKR_ROOT_HEAP = 0x803F7730u,
    FN_EXP_HEAP_CTOR = 0x802B1728u,    // JKRExpHeap::JKRExpHeap(void* data, u32 size, JKRHeap* parent, bool errorFlag)
    FN_EXP_HEAP_DTOR = 0x802B17B8u,    // JKRExpHeap::~JKRExpHeap()
    FN_HEAP_TOTAL_FREE = 0x802B08C0u,  // JKRHeap::getTotalFreeSize()
    FN_HEAP_MAX_FREE = 0x802B0868u,    // JKRHeap::getFreeSize() (the largest block, for an expanded heap)
    POOL_FAST = 0,
    POOL_MEM2 = 1,
    POOLS = 2,
};

// Where the fast pool is for a MEM1 array of this size; false when there is none.
bool bluewake_puppet_fast_range(u32 ram_size, u32* base, u32* bytes) {
    if (ram_size == 0x02000000u) {
        *base = 0x81C00000u; // the top 4 MB of the 32 MB window: past the game's 24 MB and the host's own objects (runtime.h)
        *bytes = 0x00400000u;
        return true;
    }
    if (ram_size > 0x02000000u) {
        *base = 0x82000000u;
        *bytes = ram_size - 0x02000000u;
        return true;
    }
    return false;
}

typedef struct Pool {
    const char* name;
    u32 base, bytes;      // the block
    u32 heap;             // the pool heap, 0 until made
    u32 size;             // bytes of its data area
    bool failed;          // it did not take (or was forgotten): the next tier for the run
    bool stale;           // a scene began with heaps still in it: reset on the next use
    int users;            // heaps made from it and not destroyed
    u32 peak_used;        // the most it has held
} Pool;
static Pool g_pools[POOLS] = {{"fast"}, {"MEM2"}};

// Sets the blocks from the host's memory (each hook).
static void pools_locate(CPUState* cpu) {
    u32 base = 0u, bytes = 0u;
    if (bluewake_puppet_fast_range(cpu->ram_size, &base, &bytes) && getenv("BLUEWAKE_PUPPET_NOFAST") == NULL) {
        g_pools[POOL_FAST].base = base;
        g_pools[POOL_FAST].bytes = bytes;
    }
    g_pools[POOL_MEM2].base = cpu->exram != NULL ? MEM2_BASE : 0u;
    g_pools[POOL_MEM2].bytes = cpu->exram != NULL ? cpu->exram_size : 0u;
    bluewake_puppet_mem2_end = cpu->exram != NULL ? MEM2_BASE + cpu->exram_size : 0u;
    bluewake_puppet_mem1_end = 0x80000000u + cpu->ram_size;
}

static int pool_of(u32 heap) {
    for (int i = 0; i < POOLS; ++i)
        if (g_pools[i].bytes != 0u && heap >= g_pools[i].base && heap - g_pools[i].base < g_pools[i].bytes)
            return i;
    return -1;
}

static void pool_reset(GuestCoro* co, CPUState* cpu, Pool* p);
static u32 pool_free_of(GuestCoro* co, const Pool* p) { return p->heap != 0u ? call(co, FN_HEAP_TOTAL_FREE, p->heap, 0, 0, 0) : 0u; }
static u32 pools_used(GuestCoro* co) {
    u32 used = 0u;
    for (int i = 0; i < POOLS; ++i)
        if (g_pools[i].heap != 0u)
            used += g_pools[i].size - pool_free_of(co, &g_pools[i]);
    return used;
}
static int pools_users(void) {
    int n = 0;
    for (int i = 0; i < POOLS; ++i)
        n += g_pools[i].users;
    return n;
}

// The pool, made if need be; NULL when there is none (then the caller uses the next tier).
static Pool* pool_get(GuestCoro* co, CPUState* cpu, int which) {
    Pool* p = &g_pools[which];
    if (p->stale) {
        p->stale = false;
        pool_reset(co, cpu, p);
        for (int i = 0; i < DS_SLOTS; ++i) // their heaps went with the pool
            if (g_ds[i].heap != 0u && pool_of(g_ds[i].heap) == which)
                memset(&g_ds[i], 0, sizeof g_ds[i]);
    }
    if (p->heap != 0u)
        return p;
    if (p->failed || p->bytes < 0x40000u || getenv("BLUEWAKE_PUPPET_NOMEM2") != NULL)
        return NULL;
    if (which == POOL_MEM2 && (cpu->exram == NULL || cpu->exram_size < 0x400000u))
        return NULL;
    const u32 root = mem_read32(cpu, JKR_ROOT_HEAP);
    if (!guest_ptr(root)) // the game's heaps are not up yet: asked again
        return NULL;
    const u32 data = p->base + POOL_OBJECT_SIZE;
    const u32 size = (p->bytes - POOL_OBJECT_SIZE) & ~0xFu;
    for (u32 o = 0; o < POOL_OBJECT_SIZE; o += 4u)
        mem_write32(cpu, p->base + o, 0u);
    // The constructor makes the new heap the system and current one if those are the root (it takes a heap without a parent
    // to be a root): put back as they were.
    const u32 sys = mem_read32(cpu, JKR_SYSTEM_HEAP), cur = mem_read32(cpu, JKR_CURRENT_HEAP);
    bluewake_guest_call(co, FN_EXP_HEAP_CTOR, p->base, data, size, root, 0, 0);
    mem_write32(cpu, JKR_SYSTEM_HEAP, sys);
    mem_write32(cpu, JKR_CURRENT_HEAP, cur);
    // The first block's header (its size is the data area less the header), written by the module's own memory accesses and
    // read back here: if the module did not reach the block the pool is no good.
    const u32 first_size = mem_read32(cpu, data + 4u);
    const u32 total = call(co, FN_HEAP_TOTAL_FREE, p->base, 0, 0, 0);
    if (first_size != size - 0x10u || total != size - 0x10u) {
        fprintf(stderr, "[puppet] the %s pool did not take (first block %08X, free %08X, wanted %08X): the next tier is used\n", p->name, first_size, total,
                size - 0x10u);
        p->failed = true;
        return NULL;
    }
    p->heap = p->base;
    p->size = size - 0x10u;
    p->users = 0;
    p->peak_used = 0u;
    fprintf(stderr, "[puppet] %s pool %08X: %u KB of heap at %08X (parent: root heap %08X)\n", p->name, p->heap, p->size >> 10, data, root);
    return p;
}

// The pool dropped (the scene's end found heaps in it that nothing owns); made again empty on the next use.
static void pool_reset(GuestCoro* co, CPUState* cpu, Pool* p) {
    if (p->heap == 0u)
        return;
    const u32 sys = mem_read32(cpu, JKR_SYSTEM_HEAP), cur = mem_read32(cpu, JKR_CURRENT_HEAP);
    bluewake_guest_call(co, FN_EXP_HEAP_DTOR, p->heap, 0, 0, 0, 0, 0); // out of the root's tree
    mem_write32(cpu, JKR_SYSTEM_HEAP, sys);
    mem_write32(cpu, JKR_CURRENT_HEAP, cur);
    p->heap = 0u;
    p->users = 0;
}

bool bluewake_puppet_mem2_in_use(void) { return g_pools[POOL_MEM2].heap != 0u; }

// A state was loaded that has no MEM2 pool in its heap tree (main.c): the pools and what was made in them are forgotten (the
// fast pool's block is MEM1, which the state replaced: whether it holds a pool is not known, so it is not used again).
// The puppets are made again from the remote table when the draw next finds none.
void bluewake_puppet_mem2_forget(void) {
    if (pools_users() == 0 && g_pools[POOL_MEM2].heap == 0u && g_pools[POOL_FAST].heap == 0u)
        return;
    for (int i = 0; i < POOLS; ++i) {
        g_pools[i].heap = 0u;
        g_pools[i].users = 0;
    }
    g_pools[POOL_FAST].failed = true;
    memset(g_ds, 0, sizeof g_ds);
    memset(g_guest, 0, sizeof g_guest);
    memset(g_ptex, 0, sizeof g_ptex);
}

// A solid heap from a pool, made the current heap (the caller restores it); 0 when it cannot be.
static u32 pool_solid_heap_from(GuestCoro* co, int which, u32 size) {
    CPUState* cpu = bluewake_coro_cpu(co);
    Pool* p = pool_get(co, cpu, which);
    if (p == NULL)
        return 0u;
    const u32 heap = call(co, FN_CREATE_HEAP_TO_CURRENT_FROM, size, p->heap, 0x20u, 0);
    if (heap == 0u)
        return 0u;
    ++p->users;
    // The textures drawn from memory that was another heap's are cached by the pages' write marks (the graphics' texture
    // cache): this memory is written by the loader without a flush the marks come from, so the pages are marked here.
    dol_guest_memory_dirty_mark(heap, size);
    const u32 used = p->size - pool_free_of(co, p);
    if (used > p->peak_used)
        p->peak_used = used;
    return heap;
}

// The fast pool first, then MEM2.
static u32 pool_solid_heap(GuestCoro* co, u32 size) {
    for (int i = 0; i < POOLS; ++i) {
        const u32 heap = pool_solid_heap_from(co, i, size);
        if (heap != 0u)
            return heap;
    }
    return 0u;
}

// Destroys a puppet heap, from whichever heap it came from (the game's own destroy frees it to its parent).
static void destroy_puppet_heap(GuestCoro* co, u32 heap) {
    if (heap == 0u)
        return;
    const int which = pool_of(heap);
    if (which == POOL_MEM2 && bluewake_coro_cpu(co)->exram != NULL)
        dol_guest_memory_dirty_mark(MEM2_BASE, bluewake_coro_cpu(co)->exram_size); // what was drawn from it is read again
    else if (which == POOL_FAST)
        dol_guest_memory_dirty_mark(g_pools[POOL_FAST].base, g_pools[POOL_FAST].bytes);
    call(co, FN_DESTROY_HEAP, heap, 0, 0, 0);
    if (which >= 0 && g_pools[which].users > 0)
        --g_pools[which].users;
}

// A puppet's solid heap, the first size that fits: from the fast pool (memory the game does not use, as fast as MEM1), then the
// game heap, the Zelda heap and (the game heaps are small: a stock puppet takes 90 KB, a custom body 400 KB and more) the
// archive heap, which has about 1.6 MB of free room that the Link.arc swap reserve leaves, then MEM2. The game's own heaps are
// the game's: a puppet takes from one only what leaves it a reserve (a scene's actors are made from them: with sixteen puppets'
// small equipment heaps taken out of them, the game's next allocation failed and it jumped to null, 2026-10-19), so only the
// heaps left over from the pools use them. The heap is made the current one; the caller restores it. *size is the size that
// was made (or the last tried).
enum { ARCHIVE_HEADROOM = 0x100000, GAME_HEAP_RESERVE = 0x30000, ZELDA_HEAP_RESERVE = 0x40000 };
static u32 create_puppet_heap(GuestCoro* co, const u32* want, int n, u32* size) {
    u32 heap = pool_solid_heap_from(co, POOL_FAST, *size = want[0]);
    const char* from = "the fast pool";
    if (heap == 0u) {
        const u32 game = call(co, 0x800117E4u, 0, 0, 0, 0); // mDoExt_getGameHeap()
        const u32 game_free = game != 0u ? call(co, FN_HEAP_TOTAL_FREE, game, 0, 0, 0) : 0u;
        from = "the game heap";
        for (int k = 0; k < n && heap == 0u; ++k)
            if (want[k] + GAME_HEAP_RESERVE <= game_free)
                heap = call(co, FN_CREATE_HEAP_TO_CURRENT, *size = want[k], 0x20u, 0, 0);
    }
    if (heap == 0u) {
        const u32 zelda = call(co, FN_GET_ZELDA_HEAP, 0, 0, 0, 0);
        const u32 zelda_free = zelda != 0u ? call(co, FN_HEAP_TOTAL_FREE, zelda, 0, 0, 0) : 0u;
        from = "the Zelda heap";
        for (int k = 0; k < n && heap == 0u && zelda != 0u; ++k)
            if (want[k] + ZELDA_HEAP_RESERVE <= zelda_free)
                heap = call(co, FN_CREATE_HEAP_TO_CURRENT_FROM, *size = want[k], zelda, 0x20u, 0);
    }
    if (heap == 0u) {
        const u32 archive = call(co, 0x80011AB4u, 0, 0, 0, 0); // mDoExt_getArchiveHeap()
        const u32 total = archive != 0u ? call(co, 0x802B08C0u, archive, 0, 0, 0) : 0u; // JKRHeap::getTotalFreeSize
        from = "the archive heap";
        for (int k = 0; k < n && heap == 0u && total > ARCHIVE_HEADROOM; ++k)
            if (want[k] + ARCHIVE_HEADROOM <= total)
                heap = call(co, FN_CREATE_HEAP_TO_CURRENT_FROM, *size = want[k], archive, 0x20u, 0);
    }
    if (heap == 0u) {
        heap = pool_solid_heap_from(co, POOL_MEM2, *size = want[0]);
        from = "the MEM2 pool";
    }
    if (heap == 0u)
        fprintf(stderr, "[puppet] no heap has room for %u bytes (fast pool %u KB free, MEM2 %u KB free)\n", want[0], pool_free_of(co, &g_pools[POOL_FAST]) >> 10,
                pool_free_of(co, &g_pools[POOL_MEM2]) >> 10);
    else
        fprintf(stderr, "[puppet] heap %08X of %u bytes from %s\n", heap, *size, from);
    return heap;
}

static uint64_t fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static void ds_release(GuestCoro* co, Guest* g) {
    for (int k = 0; k <= EQUIP_SLOTS; ++k) {
        const int at = g->ds[k] - 1;
        g->ds[k] = 0;
        if (at < 0 || at >= DS_SLOTS || g_ds[at].refs <= 0)
            continue;
        if (--g_ds[at].refs == 0) {
            if (g_ds[at].heap != 0u)
                destroy_puppet_heap(co, g_ds[at].heap);
            memset(&g_ds[at], 0, sizeof g_ds[at]);
        }
    }
}

static u32 load_custom_bdl(GuestCoro* co, CPUState* cpu, const uint8_t* bdl, size_t len);

// The data of a BDL file in a slot of its own, loaded once; the slot's index, or -1 (no room, not loadable, a body that is
// not Link's 42 joints). The puppet that gets it holds a reference (ds_release).
static int ds_acquire(GuestCoro* co, CPUState* cpu, const uint8_t* bdl, size_t len, bool body) {
    const uint64_t key = fnv64(bdl, len) ^ (body ? 0x5555ull : 0ull);
    int free_slot = -1;
    for (int i = 0; i < DS_SLOTS; ++i) {
        if (g_ds[i].refs > 0 && g_ds[i].key == key && g_ds[i].len == len) {
            ++g_ds[i].refs;
            return i;
        }
        if (g_ds[i].refs == 0 && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return -1;
    // The loaded data takes about 1.1 times its file and 4 KB more (a file of many materials, more): the heap is cut to what
    // was used. One size, and a generous one: a loader that finds its heap too small gets no memory for a material and calls
    // through null (the game's, 2026-10-19: smaller sizes tried when the first did not fit were what crashed).
    u32 size = 0u;
    const u32 n = (u32)len;
    const u32 want[1] = {n + n / 2u + 0x6000u};
    const u32 heap = create_puppet_heap(co, want, 1, &size);
    if (heap == 0u) {
        fprintf(stderr, "[puppet] no room for %s of %zu bytes\n", body ? "a body" : "equipment", len);
        return -1;
    }
    u32 data = load_custom_bdl(co, cpu, bdl, len);
    if (data != 0u && body && mem_read16(cpu, data + MODEL_DATA_JOINT_NUM) != CL_JOINTS) {
        fprintf(stderr, "[puppet] the player's model has %u joints, not %d: Link's own is used\n", mem_read16(cpu, data + MODEL_DATA_JOINT_NUM), CL_JOINTS);
        data = 0u;
    }
    if (data != 0u)
        call(co, FN_SET_TOON_TEX, data, 0, 0, 0);
    const u32 used = data != 0u ? call(co, FN_ADJUST_HEAP, heap, 0, 0, 0) : 0u;
    call(co, FN_RESTORE_HEAP, 0, 0, 0, 0);
    fprintf(stderr, "[puppet] %s of %zu bytes loaded to %08X (heap %08X, %u bytes used)\n", body ? "body" : "equipment", len, data, heap, used);
    if (data == 0u) {
        destroy_puppet_heap(co, heap);
        return -1;
    }
    g_ds[free_slot] = (DataSlot){key, len, heap, data, 1};
    return free_slot;
}

// How much room the game's heaps have (logged when a puppet is made: the puppets' heaps come out of them).
static void log_heaps(GuestCoro* co, CPUState* cpu) {
    static const struct { const char* name; u32 getter; } kHeaps[] = {
        {"game", 0x800117E4u}, {"zelda", 0x800118C0u}, {"command", 0x8001199Cu}, {"archive", 0x80011AB4u}, {"system", 0}, {"root", 0}};
    char text[160];
    int n = 0;
    for (int i = 0; i < 6; ++i) {
        const u32 heap = i == 4 ? mem_read32(cpu, 0x803F7728u) : i == 5 ? mem_read32(cpu, 0x803F7730u) : call(co, kHeaps[i].getter, 0, 0, 0, 0);
        const u32 total = heap != 0u ? call(co, 0x802B08C0u, heap, 0, 0, 0) : 0u;
        const u32 block = heap != 0u ? call(co, 0x802B0868u, heap, 0, 0, 0) : 0u;
        n += snprintf(text + n, sizeof text - (size_t)n, " %s %u/%u", kHeaps[i].name, block, total);
    }
    fprintf(stderr, "[puppet] heaps (largest block/total free):%s fast %u/%u mem2 %u/%u (%d heaps in pools, peak %u + %u KB)\n", text,
            g_pools[POOL_FAST].heap != 0u ? call(co, FN_HEAP_MAX_FREE, g_pools[POOL_FAST].heap, 0, 0, 0) : 0u, pool_free_of(co, &g_pools[POOL_FAST]),
            g_pools[POOL_MEM2].heap != 0u ? call(co, FN_HEAP_MAX_FREE, g_pools[POOL_MEM2].heap, 0, 0, 0) : 0u, pool_free_of(co, &g_pools[POOL_MEM2]),
            pools_users(), g_pools[POOL_FAST].peak_used >> 10, g_pools[POOL_MEM2].peak_used >> 10);
}

static void create_puppet(GuestCoro* co, CPUState* cpu, Guest* g, u32 vanilla_data, int room, const uint8_t* bdl, size_t bdl_len, int id,
                          uint8_t* const* eq, const size_t* eq_len) {
    bool custom = bdl != NULL && bdl_len >= 0x40 && bdl_len <= MAX_CUSTOM_BDL;
    log_heaps(co, cpu);
    const u32 pool_before = pools_used(co);
    size_t eq_total = 0;
    for (int k = 0; k < EQUIP_SLOTS; ++k)
        if (eq[k] != NULL && eq_len[k] >= 0x20 && eq_len[k] <= MAX_CUSTOM_BDL)
            eq_total += eq_len[k];
    u32 heap = 0u, model_data = vanilla_data;
    u32 eq_data[EQUIP_SLOTS] = {0};
    // The player's own body model and equipment, loaded with the game's own loader into shared data heaps (see DataSlot);
    // anything wrong and that part is Link as the game has him.
    if (custom) {
        const int at = ds_acquire(co, cpu, bdl, bdl_len, true);
        if (at >= 0) {
            g->ds[EQUIP_SLOTS] = at + 1;
            model_data = g_ds[at].data;
        } else {
            int holders = 0;
            for (int i = 0; i < MAX_REMOTES; ++i)
                holders += (&g_guest[i] != g && g_guest[i].model != 0u && g_guest[i].ds[EQUIP_SLOTS] != 0) ? 1 : 0;
            fprintf(stderr, "[puppet] player %d: no room for its custom body (%zu bytes; %d other puppets hold custom bodies): stock for now, asked for again later\n", id, bdl_len, holders);
            custom = false;
            g->denied = true;
            g->denied_at = now_ms();
        }
    }
    for (int k = 0; k < EQUIP_SLOTS; ++k) {
        if (eq[k] == NULL || eq_len[k] < 0x20 || eq_len[k] > MAX_CUSTOM_BDL)
            continue;
        const int at = ds_acquire(co, cpu, eq[k], eq_len[k], false);
        if (at >= 0) {
            g->ds[k] = at + 1;
            eq_data[k] = g_ds[at].data;
        } else if (!g->denied) {
            fprintf(stderr, "[puppet] player %d: no room for its custom equipment (slot %d): the game's for now, asked for again later\n", id, k);
            g->denied = true;
            g->denied_at = now_ms();
        }
    }
    {
        // The models made from that data take about 90 KB (the heap is cut to what was used).
        const u32 want[1] = {0x28000u};
        u32 size = 0u;
        heap = create_puppet_heap(co, want, 1, &size);
        if (heap == 0u) {
            ds_release(co, g);
            g->failed = true;
            fprintf(stderr, "[puppet] no heap for a puppet model\n");
            return;
        }
    }
    const u32 model = call(co, FN_MODEL_CREATE, model_data, 0x80000u, 0x37221222u, 0);
    const u32 tev = call(co, FN_NEW, 0xB0u, 0, 0, 0);
    // The puppet's place (+0), then its head's (+0x10), the shadow's root point (+0x20) and ground query point (+0x30),
    // the ground normal (+0x40) and a name (+0x50).
    const u32 pos = call(co, FN_NEW, 0x80u, 0, 0, 0);
    const u32 pk = call(co, FN_NEW, 0x40u, 0, 0, 0);
    if (pk != 0u) {
        static const u32 kStatic[4] = {PKT_OFF_CUP_ON_AUP1, PKT_OFF_CUP_ON_AUP2, PKT_ON_CUP_OFF_AUP1, PKT_ON_CUP_OFF_AUP2};
        for (u32 k = 0; k < 4u; ++k) {
            mem_write32(cpu, pk + 0x10u * k, mem_read32(cpu, kStatic[k])); // the vtable
            for (u32 o = 4u; o < 0x10u; o += 4u)
                mem_write32(cpu, pk + 0x10u * k + o, 0u);
            g->packet[k] = pk + 0x10u * k;
        }
    }
    memset(g->var, 0, sizeof g->var);
    if (!make_variant_packets(co, cpu, g, id))
        fprintf(stderr, "[puppet] player %d: no room for the packets that set its texture variant: its colors are Link's\n", id);
    if (model != 0u && tev != 0u && pos != 0u && pk != 0u) {
        for (u32 i = 0; i < 0xB0u; i += 4u)
            mem_write32(cpu, tev + i, 0u);
        call(co, FN_TEVSTR_INIT, tev, (u32)room, 0xFFu, 0);
    }
    u32 katsura = 0u;
    if (guest_ptr(eq_data[7]))
        katsura = call(co, FN_MODEL_CREATE, eq_data[7], 0x80000u, 0x37221222u, 0); // the player's own (or the game's, see slot 7)
    else if (!custom) {
        const u32 player = mem_read32(cpu, PLAYER);
        const u32 vk = guest_ptr(player) ? mem_read32(cpu, player + PLAYER_KATSURA_MODEL) : 0u;
        const u32 kdata = guest_ptr(vk) ? mem_read32(cpu, vk + MODEL_DATA) : 0u;
        if (guest_ptr(kdata))
            katsura = call(co, FN_MODEL_CREATE, kdata, 0x80000u, 0x37221222u, 0);
    }
    // The equipment, from the game's own Link models (a pack's own sword and shield models are not used yet).
    {
        const u32 player = mem_read32(cpu, PLAYER);
        static const u32 kEquipOffsets[5] = {PLAYER_SWGRIPA, PLAYER_SWGRIPA + 4u, PLAYER_SHA, PLAYER_SHA + 4u, PLAYER_HANDS_MODEL};
        static const int kEquipSlot[5] = {0, 1, 2, 3, 6}; // the pack's slot of each (swgripa, swgripms, sha, shms, hands)
        u32* slots[5] = {&g->sword[0], &g->sword[1], &g->shield[0], &g->shield[1], &g->hands};
        for (int k = 0; k < 5; ++k) {
            *slots[k] = 0u;
            u32 data = eq_data[kEquipSlot[k]]; // the player's own (or the game's, for a PC whose own are a mod's)
            if (!guest_ptr(data)) {
                const u32 local = guest_ptr(player) ? mem_read32(cpu, player + kEquipOffsets[k]) : 0u;
                data = guest_ptr(local) ? mem_read32(cpu, local + MODEL_DATA) : 0u;
            }
            if (guest_ptr(data))
                *slots[k] = call(co, FN_MODEL_CREATE, data, 0x80000u, 0x37221222u, 0);
        }
    }
    // The blades (daPy_lk_c::setSwordModel): Link's archive's models, made with the puppet.
    {
        static const u32 kBlade[2] = {RES_BDL_SWA, RES_BDL_SWMS};
        for (int k = 0; k < 2; ++k) {
            g->blade[k] = 0u;
            const u32 data = guest_ptr(eq_data[4 + k]) ? eq_data[4 + k] : call(co, FN_GET_RES, ARC_LINK_NAME, kBlade[k], RES_OBJECT_INFO, 0x40u);
            if (guest_ptr(data))
                g->blade[k] = call(co, FN_MODEL_CREATE, data, 0x80000u, 0x37221222u, 0);
        }
    }
    {
        const int slot = (int)(g - g_guest);
        memset(g_ptex[slot], 0, sizeof g_ptex[slot]);
        g->id = id;
        if (model != 0u && !custom) { // a player's own model has its own textures, not Link's
            own_textures(co, cpu, slot, id, model);
            own_textures(co, cpu, slot, id, katsura);
            own_textures(co, cpu, slot, id, g->hands);
        }
        for (int k = 0; k < 2; ++k) { // the equipment: not recolored, but this puppet's (see above)
            own_textures(co, cpu, slot, id, g->sword[k]);
            own_textures(co, cpu, slot, id, g->shield[k]);
            own_textures(co, cpu, slot, id, g->blade[k]);
        }
    }
    if (attachments_on() && lines_on()) {
        // The rope's picture (Always archive) and a strip of two points for each of the chain and the rope.
        static const char kArc[8] = "Always";
        static const char kRes[12] = "rope.bti";
        for (u32 i = 0; i < 8u; ++i)
            mem_write8(cpu, pos + 0x50u + i, (u8)kArc[i]);
        for (u32 i = 0; i < 12u; ++i)
            mem_write8(cpu, pos + 0x58u + i, (u8)kRes[i]);
        const u32 img = pos != 0u ? call(co, FN_GET_RES_NAMED, pos + 0x50u, pos + 0x58u, RES_OBJECT_INFO, RES_OBJECT_INFO_NUM) : 0u;
        for (int k = 0; k < 2; ++k) {
            g->line[k] = 0u;
            const u32 mat = guest_ptr(img) ? call(co, FN_NEW, 0x3Cu, 0, 0, 0) : 0u;
            if (!guest_ptr(mat))
                continue;
            for (u32 o = 0; o < 0x3Cu; o += 4u)
                mem_write32(cpu, mat + o, 0u);
            mem_write32(cpu, mat, LINE_VTABLE);
            const u32 ok = call(co, FN_LINE_INIT, mat, 1u, 2u, img);
            fprintf(stderr, "[puppet] line %d %08X init returned %u\n", k, mat, ok);
            if ((ok & 0xFFu) != 0u)
                g->line[k] = mat;
        }
        fprintf(stderr, "[puppet] rope picture %08X\n", img);
    }
    const u32 used = call(co, FN_ADJUST_HEAP, heap, 0, 0, 0);
    call(co, FN_RESTORE_HEAP, 0, 0, 0, 0);
    g->katsura = katsura;
    if (model == 0u || tev == 0u || pos == 0u || pk == 0u) {
        destroy_puppet_heap(co, heap);
        ds_release(co, g);
        g->failed = true;
        fprintf(stderr, "[puppet] the puppet model could not be made (model %08X)\n", model);
        return;
    }
    g->heap = heap;
    g->model = model;
    g->tevstr = tev;
    g->pos = pos;
    g->model_data = model_data;
    g->custom = custom;
    g->remap = false;
    if (custom) {
        int pv[CL_JOINTS], pc[CL_JOINTS];
        if (read_skeleton(cpu, vanilla_data, pv, g->bind_link) && read_skeleton(cpu, model_data, pc, g->bind_own) &&
            memcmp(pv, pc, sizeof pv) == 0) {
            memcpy(g->parent, pv, sizeof pv);
            // Parents first.
            int n = 0;
            bool done[CL_JOINTS] = {false};
            while (n < CL_JOINTS) {
                const int before = n;
                for (int j = 0; j < CL_JOINTS; ++j)
                    if (!done[j] && (g->parent[j] < 0 || done[g->parent[j]])) {
                        done[j] = true;
                        g->order[n++] = j;
                    }
                if (n == before)
                    break;
            }
            g->remap = n == CL_JOINTS;
        }
        fprintf(stderr, "[puppet] the player's skeleton %s Link's: %s\n", g->remap ? "matches" : "differs from",
                g->remap ? "bone lengths are its own" : "Link's matrices as sent");
    }
    fprintf(stderr, "[puppet] model %08X made from %s (heap %08X, adjust returned %u)\n", model,
            custom ? "the player's own model" : "Link's", heap, used);
    if (g_pools[POOL_FAST].heap != 0u || g_pools[POOL_MEM2].heap != 0u) {
        const u32 pool_after = pools_used(co);
        fprintf(stderr, "[puppet] player %d: %u KB more of the pools (data and models; data shared with other players is not counted again), the pools hold %u KB\n", id,
                (pool_after > pool_before ? pool_after - pool_before : 0u) >> 10, pool_after >> 10);
    }
}

static void set_shape_hidden(CPUState* cpu, u32 shape, bool hidden) {
    if (!guest_ptr(shape))
        return;
    const u32 flags = mem_read32(cpu, shape + 0x0Cu);
    mem_write32(cpu, shape + 0x0Cu, hidden ? (flags | 1u) : (flags & ~1u));
}

// The shapes one joint's materials hold, in chain order.
static int joint_shapes(CPUState* cpu, u32 joints, int joint_no, u32 out[8]) {
    int n = 0;
    const u32 joint = mem_read32(cpu, joints + 4u * (u32)joint_no);
    u32 mat = guest_ptr(joint) ? mem_read32(cpu, joint + 0x60u) : 0u; // J3DJoint::mMesh
    for (; n < 8 && guest_ptr(mat); mat = mem_read32(cpu, mat + 4u)) { // J3DMaterial::mNext
        const u32 shape = mem_read32(cpu, mat + 8u);
        if (guest_ptr(shape))
            out[n++] = shape;
    }
    return n;
}

static u32 joint_node(CPUState* cpu, u32 joints, int joint_no) { return mem_read32(cpu, joints + 4u * (u32)joint_no); }

// ---- what he holds and wears ----
// Each is a model of the game's resources (the archive and index he sent), made for the puppet the first time it is
// seen: if that archive is loaded here (the Link archive always is; a carried pot's or a Deku stick's when this player's
// scene has it too). Their joints are written as sent, so the models are never calculated here: a calc would run the
// joint callbacks of the owner that made the shared data (the bow, the leaf: the player actor's).
static void att_forget_textures(Guest* g, u32 ptex) {
    if (ptex == 0u)
        return;
    for (int k = 0; k < PTEX_MAX; ++k)
        if (g_ptex[g - g_guest][k] == ptex)
            g_ptex[g - g_guest][k] = 0u;
}

static void att_free(GuestCoro* co, Guest* g) {
    if (g->sail_phase != 0u) { // the archives it loaded for the sail (none when the PC's own ship had them)
        static const char* const kNames[2] = {"Cloth", "Ship"};
        for (int k = 0; k < 2; ++k)
            if (mem_read32(bluewake_coro_cpu(co), g->sail_phase + 8u * (u32)k + 4u) == 2u) {
                for (int i = 0; i < 6; ++i)
                    mem_write8(bluewake_coro_cpu(co), g->pos + 0x50u + (u32)i, (u8)kNames[k][i]);
                call(co, FN_RES_DELETE, g->sail_phase + 8u * (u32)k, g->pos + 0x50u, 0, 0);
            }
    }
    if (g->sail_heap != 0u)
        destroy_puppet_heap(co, g->sail_heap);
    g->sail_heap = g->sail_pkt = g->sail_zmtx = g->sail_phase = 0u;
    g->sail_tries = g->sail_said = 0;
    g->sail_built = g->sail_given_up = false;
    for (int i = 0; i < ATT_MODELS; ++i) {
        att_forget_textures(g, g->att[i].ptex);
        if (g->att[i].heap != 0u)
            destroy_puppet_heap(co, g->att[i].heap);
        memset(&g->att[i], 0, sizeof g->att[i]);
    }
}

static void att_make(GuestCoro* co, CPUState* cpu, Guest* g, struct AttModel* m) {
    for (int i = 0; i < ATT_ARC; ++i)
        mem_write8(cpu, g->pos + 0x50u + (u32)i, (u8)m->arc[i]);
    const u32 data = call(co, FN_GET_RES, g->pos + 0x50u, m->idx, RES_OBJECT_INFO, RES_OBJECT_INFO_NUM);
    m->failed = true;
    m->retry = 0;
    if (!guest_ptr(data)) {
        fprintf(stderr, "[puppet] slot %d: %s resource %u is not loaded here: not drawn\n", (int)(g - g_guest), m->arc, (unsigned)m->idx);
        return;
    }
    // The ship's hull is skinned on the CPU (the game changes the shared data's display lists for it): room for its arrays.
    const u32 heap_size = strcmp(m->arc, "Ship") == 0 && m->idx == 17u ? 0x60000u : ATT_HEAP;
    u32 heap = pool_solid_heap(co, heap_size);
    if (heap == 0u)
        heap = call(co, FN_CREATE_HEAP_TO_CURRENT, heap_size, 0x20u, 0, 0);
    if (heap == 0u) {
        const u32 zelda = call(co, FN_GET_ZELDA_HEAP, 0, 0, 0, 0);
        if (zelda != 0u)
            heap = call(co, FN_CREATE_HEAP_TO_CURRENT_FROM, heap_size, zelda, 0x20u, 0);
    }
    if (heap == 0u)
        return;
    // The diff flags the owner made it with: the ship's models are not Link's kind (the hull has a skin deform and its own
    // register animations; with Link's flags it drew nothing).
    u32 diff = 0x37221222u;
    if (strcmp(m->arc, "Ship") == 0)
        diff = m->idx == 17u ? 0x11200202u : 0x11000002u;
    const u32 model = call(co, FN_MODEL_CREATE, data, 0x80000u, diff, 0);
    if (model != 0u && strcmp(m->arc, "Ship") == 0 && m->idx == 17u) {
        // As daShip_c::createHeap does: a skin deform, whose changes to the shared display lists the real ship's hull has
        // already made (so a model without one draws the hull's vertices unskinned).
        const u32 def = call(co, FN_NEW, 0x18u, 0, 0, 0);
        if (guest_ptr(def)) {
            call(co, FN_SKIN_CTOR, def, 0, 0, 0);
            const u32 r = call(co, FN_SET_SKIN, model, def, 1u, 0);
            fprintf(stderr, "[puppet] ship hull skin deform %08X, setSkinDeform returned %u\n", def, r);
            m->skin = def;
        }
    }
    if (model != 0u) {
        if (getenv("BLUEWAKE_PUPPET_NOVAR") == NULL) // diagnosis
            m->ptex = own_textures(co, cpu, (int)(g - g_guest), g->id, model); // in the heap: freed with it
        call(co, FN_ADJUST_HEAP, heap, 0, 0, 0);
    }
    call(co, FN_RESTORE_HEAP, 0, 0, 0, 0);
    if (model == 0u) {
        destroy_puppet_heap(co, heap);
        return;
    }
    m->heap = heap;
    m->model = model;
    m->failed = false;
    fprintf(stderr, "[puppet] slot %d: %s resource %u made as model %08X\n", (int)(g - g_guest), m->arc, (unsigned)m->idx, model);
}

static struct AttModel* att_get(GuestCoro* co, CPUState* cpu, Guest* g, const char* arc, u16 idx, u8 dup) {
    struct AttModel* use = NULL;
    for (int i = 0; i < ATT_MODELS; ++i) {
        struct AttModel* m = &g->att[i];
        if (m->arc[0] != '\0' && m->idx == idx && m->dup == dup && strcmp(m->arc, arc) == 0) {
            if (m->failed && ++m->retry < 300) // not there just now (the archive may load): looked at again in some seconds
                return NULL;
            if (m->failed)
                att_make(co, cpu, g, m);
            return m->model != 0u ? m : NULL;
        }
        if (use == NULL || (use->arc[0] != '\0' && (m->arc[0] == '\0' || m->used < use->used)))
            use = m;
    }
    att_forget_textures(g, use->ptex);
    if (use->heap != 0u)
        destroy_puppet_heap(co, use->heap);
    memset(use, 0, sizeof *use);
    memcpy(use->arc, arc, ATT_ARC - 1);
    use->idx = idx;
    use->dup = dup;
    att_make(co, cpu, g, use);
    return use->model != 0u ? use : NULL;
}

static u32 call_f(GuestCoro* co, u32 fn, float f1, float f2, float f3, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f);

// The sail cloth (daHo_packet_c, a J3DMatPacket), made as daGrid_c's own create does and independent of the local ship: the
// puppet's own packet (the game's constructors, the cloth class's vtable), its 85 points from the sender, the normals worked out
// from them by the cloth class's own functions, its place and lighting, entered in the Z-sorted translucent list. Its draw reads
// two pictures from the "Ship" and "Cloth" archives, which a PC without a ship of its own does not have: the puppet loads them
// as daGrid_c does (dComIfG_resLoad, a step each frame) and gives them back with its models.
enum {
    FN_MAT_PACKET_CT = 0x802DB36Cu,   // J3DMatPacket::J3DMatPacket()
    FN_SHAPE_PACKET_CT = 0x802DB524u, // J3DShapePacket::J3DShapePacket()
    HO_VTABLE = 0x8038BBE0u,          // __vt__13daHo_packet_c
    FN_HO_SET_NRM_MTX = 0x800E8D48u,  // daHo_packet_c::setNrmMtx(cXyz&)
    FN_HO_SET_NRM_VTX = 0x800E8D74u,  // daHo_packet_c::setNrmVtx(cXyz* out, int x, int y)
    FN_HO_SET_TOP_NRM = 0x800E92ACu,  // daHo_packet_c::setTopNrmVtx(cXyz* out)
    FN_HO_SET_BACK_NRM = 0x800E8CC0u, // daHo_packet_c::setBackNrm()
    RES_SHIP_NEW_HO1 = 0x17,          // dRes_ID_SHIP_BTI_NEW_HO1_e (the sail's picture)
    RES_CLOTH_TOON = 0x3,             // dRes_ID_CLOTH_BTI_CLOTHTOON_e (its shading)
    PHASE_COMPLETE = 4, PHASE_ERROR = 5,
};

// Says what this puppet's sail is doing when that changes (state: 1 waits, 2 drawn, 3 given up).
static void sail_say(Guest* g, int state, const char* why) {
    if (g->sail_said == state)
        return;
    g->sail_said = state;
    fprintf(stderr, "[puppet] slot %d (player %d): sail %s\n", (int)(g - g_guest), g->id, why);
}

static bool sail_archive_ready(GuestCoro* co, CPUState* cpu, Guest* g, const char* arc, int res) {
    for (int i = 0;; ++i) {
        mem_write8(cpu, g->pos + 0x50u + (u32)i, (u8)arc[i]);
        if (arc[i] == '\0')
            break;
    }
    return guest_ptr(call(co, FN_GET_RES, g->pos + 0x50u, (u32)res, RES_OBJECT_INFO, RES_OBJECT_INFO_NUM));
}

static void draw_sail(GuestCoro* co, CPUState* cpu, Guest* g, const Item* it) {
    if (!it->sail)
        return;
    if (g->sail_given_up)
        return;
    if (g->sail_heap == 0u) {
        // Everything the sail needs from the guest's memory, in one small heap of its own (freed with the puppet's models).
        u32 heap = pool_solid_heap(co, 0x3000u);
        if (heap == 0u)
            heap = call(co, FN_CREATE_HEAP_TO_CURRENT, 0x3000u, 0x20u, 0, 0);
        if (heap == 0u) {
            sail_say(g, 3, "not drawn: no heap for it");
            g->sail_given_up = true;
            return;
        }
        const u32 pkt = call(co, FN_NEW_ALIGNED, HO_SIZE + 0x40u, 0x20u, 0, 0);
        const u32 zm = call(co, FN_NEW, 0x30u, 0, 0, 0);
        const u32 ph = call(co, FN_NEW, 0x10u, 0, 0, 0); // two request_of_phase_process_class
        call(co, FN_ADJUST_HEAP, heap, 0, 0, 0);
        call(co, FN_RESTORE_HEAP, 0, 0, 0, 0);
        if (!guest_ptr(pkt) || !guest_ptr(zm) || !guest_ptr(ph)) {
            destroy_puppet_heap(co, heap);
            sail_say(g, 3, "not drawn: no memory for its packet");
            g->sail_given_up = true;
            return;
        }
        for (u32 o = 0; o < HO_SIZE + 0x40u; o += 4u)
            mem_write32(cpu, pkt + o, 0u);
        for (u32 o = 0; o < 0x10u; o += 4u)
            mem_write32(cpu, ph + o, 0u);
        g->sail_heap = heap;
        g->sail_pkt = pkt;
        g->sail_zmtx = zm;
        g->sail_phase = ph;
    }
    if (!g->sail_built) {
        // The two archives the cloth draws from. A PC with a ship has them (daGrid_c holds them); else this puppet loads them.
        if (ENV_SET("BLUEWAKE_PUPPET_SAIL_LOAD") || !sail_archive_ready(co, cpu, g, "Ship", RES_SHIP_NEW_HO1) ||
            !sail_archive_ready(co, cpu, g, "Cloth", RES_CLOTH_TOON)) { // (the variable: testing, loads them even when the PC has them)
            static const char* const kNames[2] = {"Cloth", "Ship"};
            u32 states[2];
            for (int k = 0; k < 2; ++k) {
                for (int i = 0; i < 6; ++i)
                    mem_write8(cpu, g->pos + 0x50u + (u32)i, (u8)kNames[k][i]);
                states[k] = call(co, FN_RES_LOAD, g->sail_phase + 8u * (u32)k, g->pos + 0x50u, 0, 0) & 0xFFu;
            }
            if (states[0] == PHASE_ERROR || states[1] == PHASE_ERROR || ++g->sail_tries > 600) {
                sail_say(g, 3, "not drawn: the Ship and Cloth archives could not be loaded here");
                g->sail_given_up = true;
                return;
            }
            if (states[0] != PHASE_COMPLETE || states[1] != PHASE_COMPLETE) {
                sail_say(g, 1, "waits: the Ship and Cloth archives are loading (this PC has no ship of its own)");
                return;
            }
            fprintf(stderr, "[puppet] slot %d: loaded the Ship and Cloth archives for the sail\n", (int)(g - g_guest));
        }
        // daHo_packet_c::daHo_packet_c(): the J3DMatPacket and the embedded J3DShapePacket, then the cloth class's vtable, the
        // shape packet set (the Z sort clears it) and the alpha opaque.
        const u32 pkt = g->sail_pkt;
        call(co, FN_MAT_PACKET_CT, pkt, 0, 0, 0);
        call(co, FN_SHAPE_PACKET_CT, pkt + 0x3Cu, 0, 0, 0);
        mem_write32(cpu, pkt, HO_VTABLE);
        mem_write32(cpu, pkt + 0x28u, pkt + 0x3Cu); // J3DMatPacket::mpShapePacket
        mem_write8(cpu, pkt + HO_ALPHA, 0xFFu);
        g->sail_built = true;
        fprintf(stderr, "[puppet] slot %d: sail packet %08X made from the ship archive's own resources (the local ship %s)\n", (int)(g - g_guest), pkt,
                guest_ptr(mem_read32(cpu, SHIP_ACTOR)) ? "exists but is not used" : "does not exist");
    }
    const u32 pk = g->sail_pkt;
    // The sender's points are the packet's current set; the normals come from them by the cloth class's own functions (its yaw
    // field is 0, as in the game, so the light direction only has to be something).
    mem_write8(cpu, pk + HO_CUR, 0u);
    for (int i = 0; i < HO_POINTS; ++i)
        for (u32 k = 0; k < 3u; ++k)
            write_float(cpu, pk + HO_POS + 12u * (u32)i + 4u * k, (float)it->sail_pt[i][k] / 8.0f);
    call(co, FN_HO_SET_NRM_MTX, pk, g->pos + 0x60u, 0, 0);
    u32 nrm = pk + HO_NRM;
    for (u32 y = 0; y < 12u; ++y)
        for (u32 x = 0; x < 7u; ++x, nrm += 12u)
            call(co, FN_HO_SET_NRM_VTX, pk, nrm, x, y);
    call(co, FN_HO_SET_TOP_NRM, pk, nrm, 0, 0);
    call(co, FN_HO_SET_BACK_NRM, pk, 0, 0, 0);
    mem_write32(cpu, pk + HO_TEVSTR, g->tevstr);
    Mat34 view, mm;
    read_mat(cpu, J3D_SYS, &view);
    mat_mul(&view, &it->sail_mtx, &mm);
    write_mat(cpu, pk + HO_MTX, &mm);
    write_mat(cpu, g->sail_zmtx, &it->sail_mtx);
    const u32 buffer = mem_read32(cpu, DRAW_LIST + DRAW_XLU_LIST);
    if (!guest_ptr(buffer)) {
        sail_say(g, 1, "waits: the translucent draw list is not there");
        return;
    }
    const u32 saved = mem_read32(cpu, buffer + 0x1Cu);
    mem_write32(cpu, buffer + 0x1Cu, g->sail_zmtx);
    call(co, FN_ENTRY_Z_SORT, buffer, pk, 0, 0);
    mem_write32(cpu, buffer + 0x1Cu, saved);
    sail_say(g, 2, "drawn");
}

// The hookshot's chain and the grappling hook's rope: a strip of two points between his hand and its end, entered in the
// 3D line list as the ship's rope is.
static void draw_lines(GuestCoro* co, CPUState* cpu, Guest* g, const Item* it) {
    for (int i = 0; i < it->line_n; ++i) {
        const int kind = it->line[i].kind;
        const u32 mat = kind >= 1 && kind <= 2 ? g->line[kind - 1] : 0u;
        if (!guest_ptr(mat))
            continue;
        {
            float d2 = 0.0f;
            for (int r = 0; r < 3; ++r)
                d2 += (it->line[i].p[1][r] - it->line[i].p[0][r]) * (it->line[i].p[1][r] - it->line[i].p[0][r]);
            if (!(d2 < 4000.0f * 4000.0f)) // not finite or far off: not a line of his
                continue;
        }
        const u32 lines = mem_read32(cpu, mat + 0x38u);
        const u32 segs = guest_ptr(lines) ? mem_read32(cpu, lines) : 0u;
        if (!guest_ptr(segs))
            continue;
        for (int k = 0; k < 2; ++k)
            for (int r = 0; r < 3; ++r)
                write_float(cpu, segs + 12u * (u32)k + 4u * (u32)r, it->line[i].p[k][r]);
        const u32 color = g->pos + 0x6Cu;
        static const u8 kColor[2][4] = {{150, 150, 160, 255}, {200, 150, 50, 255}};
        for (u32 c = 0; c < 4u; ++c)
            mem_write8(cpu, color + c, kColor[kind - 1][c]);
        static int said;
        if (said++ < 3)
            fprintf(stderr, "[puppet] line kind %d from %.0f %.0f %.0f to %.0f %.0f %.0f\n", kind, it->line[i].p[0][0], it->line[i].p[0][1], it->line[i].p[0][2], it->line[i].p[1][0], it->line[i].p[1][1], it->line[i].p[1][2]);
        const bool big = getenv("BLUEWAKE_PUPPET_LINETEST") != NULL; // diagnosis: wide, and lit by the world's tev as the bridge's ropes are
        call_f(co, FN_LINE_UPDATE, big ? 30.0f : kind == 1 ? 10.0f : 8.0f, 0.0f, 0.0f, mat, 2u, color, 0u, getenv("BLUEWAKE_PUPPET_LINETEST2") != NULL || !big ? g->tevstr : 0u, 0u);
        mem_write32(cpu, J3D_SYS + 0x48u, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1));
        mem_write32(cpu, J3D_SYS + 0x4Cu, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1 + 4u));
        call(co, FN_LINE_SET, LINE_SORT_PACKET, mat, 0, 0);
        if (said < 4)
            fprintf(stderr, "[puppet] line set: sort packet head %08X (mat %08X), segments %u of %u, packet %08X vtable %08X\n", mem_read32(cpu, LINE_SORT_PACKET + 0x10u), mat,
                    mem_read16(cpu, mat + 0x34u), mem_read16(cpu, mat + 0x32u), LINE_SORT_PACKET, mem_read32(cpu, LINE_SORT_PACKET));
    }
}

static void draw_attachments(GuestCoro* co, CPUState* cpu, Guest* g, const Item* it) {
    static unsigned tick;
    ++tick;
    for (int i = 0; i < it->att_n; ++i) {
        const struct ItemAtt* a = &it->att[i];
        u8 dup = 0;
        for (int k = 0; k < i; ++k)
            dup += it->att[k].idx == a->idx && strcmp(it->att[k].arc, a->arc) == 0;
        struct AttModel* m = att_get(co, cpu, g, a->arc, a->idx, dup);
        if (m == NULL)
            continue;
        m->used = tick;
        const u32 data = mem_read32(cpu, m->model + MODEL_DATA);
        const u32 nodes = mem_read32(cpu, m->model + MODEL_NODE_MTX);
        if (!guest_ptr(data) || !guest_ptr(nodes) || mem_read16(cpu, data + MODEL_DATA_JOINT_NUM) != a->nj)
            continue;
        write_mat(cpu, m->model + MODEL_BASE_MTX, &a->base);
        for (int j = 0; j < a->nj; ++j)
            write_mat(cpu, nodes + 48u * (u32)j, &a->node[j]);
        if (ENV_SET("BLUEWAKE_PUPPET_ATTLOG")) { // diagnosis
            static int said;
            if (said++ < 12) {
                const u32 shapes_n = mem_read16(cpu, data + 0x7Cu), shapes = mem_read32(cpu, data + 0x80u);
                int hidden = 0;
                for (u32 k = 0; k < shapes_n && guest_ptr(shapes); ++k)
                    hidden += (int)(mem_read32(cpu, mem_read32(cpu, shapes + 4u * k) + 0x0Cu) & 1u);
                fprintf(stderr, "[puppet] att kind %d %s/%u nj %d at %.0f %.0f %.0f (puppet %.0f %.0f %.0f): %u shapes, %d hidden, mtxbuf flags %08X\n", a->kind,
                        a->arc, (unsigned)a->idx, a->nj, a->base.m[0][3], a->base.m[1][3], a->base.m[2][3], it->pos[0], it->pos[1], it->pos[2],
                        shapes_n, hidden, mem_read32(cpu, m->model + 8u));
            }
        }
        mem_write32(cpu, J3D_SYS + 0x38u, m->model);
        u32 saved_flags = 0u;
        if (m->skin != 0u) {
            // J3DModel::calc's part for a CPU-skinned model: the vertex arrays back to the first, the system's skin flags,
            // the envelope matrices, then the deform.
            const u32 vb = m->model + 0xCCu;
            mem_write32(cpu, vb + 0x2Cu, mem_read32(cpu, vb + 4u));
            mem_write32(cpu, vb + 0x30u, mem_read32(cpu, vb + 0xCu));
            mem_write32(cpu, vb + 0x34u, mem_read32(cpu, vb + 0x14u));
            saved_flags = mem_read32(cpu, J3D_SYS + 0x34u);
            mem_write32(cpu, J3D_SYS + 0x34u, saved_flags | 0xCu);
        }
        call(co, FN_CALC_WEIGHT_ENV, m->model, 0, 0, 0);
        if (m->skin != 0u)
            call(co, FN_SKIN_DEFORM, m->skin, m->model, 0, 0);
        call(co, FN_SET_LIGHT_TEV, ENV_LIGHT, m->model, g->tevstr, 0);
        mem_write32(cpu, J3D_SYS + 0x48u, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1));
        mem_write32(cpu, J3D_SYS + 0x4Cu, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1 + 4u));
        call(co, FN_MODEL_ENTRY_DL, m->model, 0, 0, 0);
    }
}

// Link's own draw (daPy_lk_c::draw, the normal branch), for one puppet: the
// eyes and brows through the cup/aup state packets into list P0, the face and
// hair, then the rest of the body into P1, with the shared shapes' hide flags
// set as that draw sets them (and put back after).
static void draw_puppet(GuestCoro* co, CPUState* cpu, Guest* g, u32 vanilla_data, u32 player, const Item* it) {
    const uint8_t pose_flags = it->flags;
    const u32 model_data = g->model_data;
    enum { CL_ROOT = 0, CL_LHANDA = 8, CL_RHANDA = 12, CL_PODA = 13, CL_EYE = 0x13, CL_MAYU = 0x15 };
    const u32 joints = mem_read32(cpu, model_data + 0x2Cu); // J3DModelData::mJointTree (0x10) + mJointNodePointer (0x1C)
    if (!guest_ptr(joints) || !guest_ptr(player))
        return;
    u32 blend[4], none[4], zon[4];
    {
        // Link's twelve eye and brow shapes are known to him by their kind; the same
        // shape of another model (same joint, same place in its material chain) is
        // of the same kind.
        const u32 vjoints = mem_read32(cpu, vanilla_data + 0x2Cu);
        for (u32 i = 0; i < 12u; ++i) {
            const u32 shape = mem_read32(cpu, player + PLAYER_ZOFF_BLEND + 4u * i);
            u32 mapped = shape;
            if (joints != vjoints) {
                mapped = 0u;
                for (int jn = 0x13; jn <= 0x15 && mapped == 0u; jn += 2) {
                    u32 vs[8], cs[8];
                    const int vn = joint_shapes(cpu, vjoints, jn, vs);
                    const int cn = joint_shapes(cpu, joints, jn, cs);
                    for (int k = 0; k < vn && k < cn; ++k)
                        if (vs[k] == shape)
                            mapped = cs[k];
                }
            }
            (i < 4u ? blend : i < 8u ? none : zon)[i % 4u] = mapped;
        }
    }
    // Every shape touched, with its flags, to put back.
    u32 touched[64], flags[64];
    int touched_n = 0;
#define TOUCH(shape)                                               \
    do {                                                           \
        if (touched_n < 64 && guest_ptr(shape)) {                  \
            touched[touched_n] = (shape);                          \
            flags[touched_n++] = mem_read32(cpu, (shape) + 0x0Cu); \
        }                                                          \
    } while (0)
    for (int i = 0; i < 4; ++i) {
        TOUCH(blend[i]);
        TOUCH(none[i]);
        TOUCH(zon[i]);
    }
    u32 root[8], hands[8];
    const int root_n = joint_shapes(cpu, joints, CL_ROOT, root);
    for (int i = 0; i < root_n; ++i)
        TOUCH(root[i]);
    const int lh_n = joint_shapes(cpu, joints, CL_LHANDA, hands);
    for (int i = 0; i < lh_n; ++i)
        TOUCH(hands[i]);
    const u32 lh0 = lh_n ? hands[0] : 0u;
    const int rh_n = joint_shapes(cpu, joints, CL_RHANDA, hands);
    for (int i = 0; i < rh_n; ++i)
        TOUCH(hands[i]);
    const u32 rh0 = rh_n ? hands[0] : 0u;
    u32 poda[8], back[8];
    const bool casual = (pose_flags & PUPPET_FLAG_CASUAL) != 0;
    const int back_n = joint_shapes(cpu, joints, 0x29, back); // CL_JNT_CL_BACK: the belt buckle
    for (int i = 0; i < back_n; ++i)
        TOUCH(back[i]);
    const int poda_n = joint_shapes(cpu, joints, CL_PODA, poda);
    for (int i = 0; i < poda_n; ++i)
        TOUCH(poda[i]);

    static int dbg;
    if (dbg < 3) {
        ++dbg;
        fprintf(stderr, "[puppet] draw: joints %08X root_n %d lh %d rh %d poda %d blend0 %08X model %08X\n", joints, root_n, lh_n, rh_n, poda_n, blend[0], g->model);
    }
    const u32 tex = mem_read32(cpu, model_data + 0x70u);
    const u32 eye = joint_node(cpu, joints, CL_EYE), mayu = joint_node(cpu, joints, CL_MAYU),
              root_joint = joint_node(cpu, joints, CL_ROOT);
    mem_write32(cpu, J3D_SYS + 0x38u, g->model);
    mem_write32(cpu, J3D_SYS + 0x58u, tex);
    static const char* only_text;
    static bool only_read;
    if (!only_read) {
        only_read = true;
        only_text = getenv("BLUEWAKE_PUPPET_ONLY");
    }
    const char* only = only_text;
    if (only != NULL) { // diagnosis: just these root materials (hex mask), no eyes
        const unsigned mask = (unsigned)strtoul(only, NULL, 16);
        for (int i = 0; i < 4; ++i) {
            set_shape_hidden(cpu, blend[i], true);
            set_shape_hidden(cpu, zon[i], true);
            set_shape_hidden(cpu, none[i], true);
        }
        for (int i = 0; i < root_n; ++i)
            set_shape_hidden(cpu, root[i], ((mask >> i) & 1u) == 0u);
        set_shape_hidden(cpu, lh0, true);
        set_shape_hidden(cpu, rh0, true);
        mem_write32(cpu, J3D_SYS + 0x48u, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1));
        mem_write32(cpu, J3D_SYS + 0x4Cu, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1 + 4u));
        call(co, FN_MODEL_ENTRY_DL, g->model, 0, 0, 0);
        for (int i = 0; i < touched_n; ++i)
            mem_write32(cpu, touched[i] + 0x0Cu, flags[i]);
        return;
    }
    call(co, FN_MODEL_UNLOCK, g->model, 0, 0, 0); // as daPy_lk_c::draw does before its passes
    // List P0 (both of j3dSys's buffers).
    const u32 p0 = mem_read32(cpu, DRAW_LIST + 0x08u);
    mem_write32(cpu, J3D_SYS + 0x48u, p0);
    mem_write32(cpu, J3D_SYS + 0x4Cu, p0);
    call(co, FN_ENTRY_IMM, p0, g->packet[3], 0, 0);
    for (int i = 0; i < 4; ++i) {
        set_shape_hidden(cpu, blend[i], true);
        set_shape_hidden(cpu, zon[i], true);
        set_shape_hidden(cpu, none[i], false);
    }
    call(co, FN_ENTRY_IN, eye, 0, 0, 0);
    call(co, FN_ENTRY_IN, mayu, 0, 0, 0);
    call(co, FN_ENTRY_IMM, p0, g->packet[1], 0, 0);
    for (int i = 0; i < 4; ++i) {
        set_shape_hidden(cpu, blend[i], false);
        set_shape_hidden(cpu, none[i], true);
    }
    call(co, FN_ENTRY_IN, eye, 0, 0, 0);
    call(co, FN_ENTRY_IN, mayu, 0, 0, 0);
    // The face (material 2) and hair (5) of the root joint, alone.
    for (int i = 0; i < root_n; ++i)
        set_shape_hidden(cpu, root[i], !(i == 2 || i == 5));
    call(co, FN_ENTRY_IN, root_joint, 0, 0, 0);
    mem_write32(cpu, J3D_SYS + 0x38u, g->model);
    mem_write32(cpu, J3D_SYS + 0x58u, tex);
    // hideHatAndBackle: the face and hair are done, the rest shows.
    for (int i = 0; i < root_n; ++i)
        set_shape_hidden(cpu, root[i], i == 2 || i == 5 || (casual && i == 4)); // face, hair; the hat in casual clothes
    for (int i = 0; i < back_n; ++i)
        set_shape_hidden(cpu, back[i], casual);
    call(co, FN_ENTRY_IMM, p0, g->packet[2], 0, 0);
    for (int i = 0; i < 4; ++i) {
        set_shape_hidden(cpu, blend[i], true);
        set_shape_hidden(cpu, zon[i], false);
        set_shape_hidden(cpu, none[i], true);
    }
    call(co, FN_ENTRY_IN, eye, 0, 0, 0);
    call(co, FN_ENTRY_IN, mayu, 0, 0, 0);
    call(co, FN_ENTRY_IMM, p0, g->packet[0], 0, 0);
    for (int i = 0; i < 4; ++i)
        set_shape_hidden(cpu, zon[i], true);
        for (int i = 0; i < poda_n; ++i)
        set_shape_hidden(cpu, poda[i], (pose_flags & PUPPET_FLAG_SCABBARD) == 0); // the scabbard, when he has a sword
    set_shape_hidden(cpu, lh0, (it->hands & 0x0F) != 0); // the body's own hands, unless his hands model has the shape
    set_shape_hidden(cpu, rh0, (it->hands >> 4) != 0);
    // The body proper, into P1. Its materials take their texture numbers from the one material animation of the shared
    // model data, which is the local Link's expression: this player's own are put in them for the entry (the animation
    // set aside so the game's material calc leaves them), then the animation and the local numbers are put back.
    u32 face_mat[PUPPET_FACE_MAX], face_tev[PUPPET_FACE_MAX], face_anm[PUPPET_FACE_MAX], face_old[PUPPET_FACE_MAX];
    u8 face_slot[PUPPET_FACE_MAX];
    int face_n = 0;
    // His indices (material, slot, texture number) are those of his own model. They fit the body drawn when that is the same file
    // (a stock player's on the game's own model, even one supplied from the disc; a custom player's on his own model), not Link
    // standing in for a model that could not be had.
    if (!it->face_skip && (g->custom || !it->body_wanted) && it->face_n != 0) {
        const u32 count = mem_read16(cpu, model_data + MODEL_DATA_MATERIAL_NUM);
        const u32 mats = mem_read32(cpu, model_data + MODEL_DATA_MATERIALS);
        for (int i = 0; i < it->face_n && guest_ptr(mats); ++i) {
            const FaceTex* f = &it->face[i];
            const u32 mat = f->mat < count ? mem_read32(cpu, mats + 4u * f->mat) : 0u;
            const u32 tev = guest_ptr(mat) ? mem_read32(cpu, mat + MAT_TEV_BLOCK) : 0u;
            if (!guest_ptr(tev) || f->slot >= 8u)
                continue;
            face_mat[face_n] = mat;
            face_tev[face_n] = tev;
            face_slot[face_n] = f->slot;
            face_anm[face_n] = mem_read32(cpu, mat + MAT_ANM);
            face_old[face_n] = mem_read16(cpu, tev + TEV_BLOCK_TEXNO + 2u * f->slot);
            mem_write32(cpu, mat + MAT_ANM, 0u);
            mem_write16(cpu, tev + TEV_BLOCK_TEXNO + 2u * f->slot, f->tex);
            ++face_n;
        }
    }
    mem_write32(cpu, J3D_SYS + 0x48u, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1));
    mem_write32(cpu, J3D_SYS + 0x4Cu, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1 + 4u));
    call(co, FN_MODEL_ENTRY_DL, g->model, 0, 0, 0);
    for (int i = face_n - 1; i >= 0; --i) { // in reverse: a material can be listed for two slots
        mem_write16(cpu, face_tev[i] + TEV_BLOCK_TEXNO + 2u * face_slot[i], face_old[i]);
        mem_write32(cpu, face_mat[i] + MAT_ANM, face_anm[i]);
    }
    if (face_n != 0) {
        static int said;
        static u16 last_face[MAX_REMOTES];
        static int changes[MAX_REMOTES];
        const int fs = (int)(g - g_guest);
        if (last_face[fs] != it->face[0].tex && changes[fs]++ < 12) // his expression changed: the log says so (a few times)
            fprintf(stderr, "[puppet] slot %d: expression %u -> %u (%d textures; body is %s)\n", fs, (unsigned)last_face[fs], (unsigned)it->face[0].tex, face_n,
                    g->custom ? "a model" : "Link's");
        last_face[fs] = it->face[0].tex;
        if (said++ < 2)
            fprintf(stderr, "[puppet] slot %d: %d expression textures set (first: material %u slot %u texture %u, his own %u)\n", it->slot,
                    face_n, (unsigned)it->face[0].mat, (unsigned)it->face[0].slot, (unsigned)it->face[0].tex, face_old[0]);
    }
    if (casual && g->katsura != 0u) {
        // Link's own: the model's base is his head joint, it is calculated, lit and entered.
        const u32 nodes = mem_read32(cpu, g->model + MODEL_NODE_MTX);
        Mat34 head;
        read_mat(cpu, nodes + 48u * CL_JNT_HEAD, &head);
        write_mat(cpu, g->katsura + MODEL_BASE_MTX, &head);
        calc_puppet_part(co, cpu, g->katsura);
        call(co, FN_SET_LIGHT_TEV, ENV_LIGHT, g->katsura, g->tevstr, 0);
        mem_write32(cpu, J3D_SYS + 0x48u, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1));
        mem_write32(cpu, J3D_SYS + 0x4Cu, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1 + 4u));
        call(co, FN_MODEL_ENTRY_DL, g->katsura, 0, 0, 0);
    }
    // Sword and shield, where his own draw has them (in the hand, on the back, on the arm), lit and entered.
    for (int e = 0; e < 2; ++e) {
        const uint8_t kind = e == 0 ? it->sword : it->shield;
        const u32 equip = kind == 1 || kind == 2 ? (e == 0 ? g->sword[kind - 1] : g->shield[kind - 1]) : 0u;
        if (equip == 0u || it->lod == LOD_VERY_FAR)
            continue;
        write_mat(cpu, equip + MODEL_BASE_MTX, e == 0 ? &it->sword_mtx : &it->shield_mtx);
        calc_puppet_part(co, cpu, equip);
        call(co, FN_SET_LIGHT_TEV, ENV_LIGHT, equip, g->tevstr, 0);
        mem_write32(cpu, J3D_SYS + 0x48u, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1));
        mem_write32(cpu, J3D_SYS + 0x4Cu, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1 + 4u));
        call(co, FN_MODEL_ENTRY_DL, equip, 0, 0, 0);
        // A drawn sword: the blade too, in the same hand as the grip (daPy_lk_c puts both at lHandA_jnt_mtx).
        const u32 blade = e == 0 && (pose_flags & PUPPET_FLAG_BLADE) ? g->blade[kind - 1] : 0u;
        if (blade != 0u) {
            write_mat(cpu, blade + MODEL_BASE_MTX, &it->sword_mtx);
            calc_puppet_part(co, cpu, blade);
            call(co, FN_SET_LIGHT_TEV, ENV_LIGHT, blade, g->tevstr, 0);
            mem_write32(cpu, J3D_SYS + 0x48u, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1));
            mem_write32(cpu, J3D_SYS + 0x4Cu, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1 + 4u));
            call(co, FN_MODEL_ENTRY_DL, blade, 0, 0, 0);
        }
    }
    // The hands model: the shapes he chose (fists, a grip, a bow hand), at the body's hand joints.
    if (g->hands != 0u && (it->hands != 0) && it->lod == LOD_NEAR) {
        const u32 hdata = mem_read32(cpu, g->hands + MODEL_DATA);
        const u32 hjoints = guest_ptr(hdata) ? mem_read32(cpu, hdata + 0x2Cu) : 0u;
        const u32 hnodes = mem_read32(cpu, g->hands + MODEL_NODE_MTX);
        const u32 bnodes = mem_read32(cpu, g->model + MODEL_NODE_MTX);
        if (guest_ptr(hjoints) && guest_ptr(hnodes) && guest_ptr(bnodes)) {
            u32 shapes[11], saved_flags[11];
            for (u32 j = 1; j <= 10u; ++j) {
                const u32 joint = mem_read32(cpu, hjoints + 4u * j);
                const u32 m = guest_ptr(joint) ? mem_read32(cpu, joint + 0x60u) : 0u;
                shapes[j] = guest_ptr(m) ? mem_read32(cpu, m + 8u) : 0u;
                saved_flags[j] = guest_ptr(shapes[j]) ? mem_read32(cpu, shapes[j] + 0x0Cu) : 0u;
                set_shape_hidden(cpu, shapes[j], true);
            }
            for (int side = 0; side < 2; ++side) {
                const u32 j = (it->hands >> (4 * side)) & 0x0Fu;
                if (j == 0u || j > 10u)
                    continue;
                Mat34 hand;
                read_mat(cpu, bnodes + 48u * (side == 0 ? CL_JNT_LHANDA : CL_JNT_RHANDA), &hand);
                write_mat(cpu, hnodes + 48u * j, &hand);
                set_shape_hidden(cpu, shapes[j], false);
            }
            call(co, FN_SET_LIGHT_TEV, ENV_LIGHT, g->hands, g->tevstr, 0);
            mem_write32(cpu, J3D_SYS + 0x48u, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1));
            mem_write32(cpu, J3D_SYS + 0x4Cu, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1 + 4u));
            call(co, FN_MODEL_ENTRY_DL, g->hands, 0, 0, 0);
            for (u32 j = 1; j <= 10u; ++j)
                if (guest_ptr(shapes[j]))
                    mem_write32(cpu, shapes[j] + 0x0Cu, saved_flags[j]);
        }
    }
    for (int i = 0; i < touched_n; ++i)
        mem_write32(cpu, touched[i] + 0x0Cu, flags[i]);
    if (attachments_on())
        draw_attachments(co, cpu, g, it);
    if (attachments_on() && lines_on())
        draw_lines(co, cpu, g, it);
    if (attachments_on())
        draw_sail(co, cpu, g, it);
    // The Bunny Hood, on his head joint (slot 0 of the hood's wearers is the local player).
    if ((pose_flags & PUPPET_FLAG_HOOD) != 0)
        bluewake_bunny_hood_draw_for(co, it->slot + 1, &it->node[0].m[0][0], it->pos);
#undef TOUCH
}

// A call with floating point arguments (f1, f2, f3): the registers a guest function reads them from, set before it is entered.
static u32 call_f(GuestCoro* co, u32 fn, float f1, float f2, float f3, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f) {
    CPUState* cpu = bluewake_coro_cpu(co);
    cpu->fpr[1] = cpu->ps1[1] = f1;
    cpu->fpr[2] = cpu->ps1[2] = f2;
    cpu->fpr[3] = cpu->ps1[3] = f3;
    return bluewake_guest_call(co, fn, a, b, c, d, e, f);
}

// Link's shadow (daPy_lk_c::drawShadow, the normal branch) for one puppet: the ground height below it by the game's own
// ground check, then a real shadow with the puppet's model (and its hair, sword and shield) or, where the game's eight
// real shadows are used up, the simple blob. No ground below (the open sea, a pit): none, as for Link.
static void puppet_shadow(GuestCoro* co, CPUState* cpu, Guest* g, const Item* it) {
    const bool off = ENV_SET("BLUEWAKE_PUPPET_NOSHADOW"); // diagnosis
    if (off)
        return;
    const u32 s = g->pos;
    for (int i = 0; i < 3; ++i) {
        write_float(cpu, s + 0x20u + 4u * (u32)i, it->node[0].m[i][3]); // the root joint
        write_float(cpu, s + 0x30u + 4u * (u32)i, it->pos[i] + (i == 1 ? 40.0f : 0.0f));
    }
    write_float(cpu, s + 0x40u, 0.0f);
    write_float(cpu, s + 0x44u, 1.0f);
    write_float(cpu, s + 0x48u, 0.0f);
    call(co, FN_GND_HEIGHT, s + 0x30u, 0, 0, 0);
    const float ground = (float)cpu->fpr[1];
    if (!(ground > -1.0e8f) || ground > it->pos[1] + 40.0f)
        return;
    const float height = it->pos[1] - ground;
    const u32 key = call_f(co, FN_SHADOW_REAL, (float)SHADOW_CASTER_SIZE, height, 0.0f, SHADOW_CONTROL, g->shadow_key, 0u, g->model,
                           s + 0x20u, g->tevstr);
    static int said;
    if (said++ < 3)
        fprintf(stderr, "[puppet] slot %d shadow: ground %.1f below %.1f, real shadow key %u\n", it->slot, ground, it->pos[1], key);
    if (key != 0u) {
        g->shadow_key = key;
        if ((it->flags & PUPPET_FLAG_CASUAL) != 0 && g->katsura != 0u)
            call(co, FN_SHADOW_ADD, SHADOW_CONTROL, key, g->katsura, 0);
        for (int e = 0; e < 2; ++e) {
            const uint8_t kind = e == 0 ? it->sword : it->shield;
            const u32 equip = kind == 1 || kind == 2 ? (e == 0 ? g->sword[kind - 1] : g->shield[kind - 1]) : 0u;
            if (equip != 0u)
                call(co, FN_SHADOW_ADD, SHADOW_CONTROL, key, equip, 0);
        }
        return;
    }
    call_f(co, FN_SHADOW_SIMPLE, ground, 30.0f, 1.0f, SHADOW_CONTROL, s, s + 0x40u, 0u, SIMPLE_TEX, 0u);
}

static void draw_body(GuestCoro* co, void* arg) {
    (void)arg;
    CPUState* cpu = bluewake_coro_cpu(co);
    Work* w = &g_work;
    const u32 player = mem_read32(cpu, PLAYER);
    // The draw lists, model and texture j3dSys holds are put back after.
    const u32 saved_opa = mem_read32(cpu, J3D_SYS + 0x48u);
    const u32 saved_xlu = mem_read32(cpu, J3D_SYS + 0x4Cu);
    const u32 saved_model = mem_read32(cpu, J3D_SYS + 0x38u);
    const u32 saved_tex = mem_read32(cpu, J3D_SYS + 0x58u);
    if (g_map_cursors == 0u) {
        const u32 block = call(co, FN_NEW, MAP_CURSOR_SIZE * MAP_ICONS, 0, 0, 0);
        if (guest_ptr(block))
            g_map_cursors = block;
    }
    if (g_chart_mem == 0u) {
        const u32 block = call(co, FN_NEW, CHART_MEM_SIZE, 0, 0, 0);
        if (guest_ptr(block))
            g_chart_mem = block;
    }
    if (g_cap_pending) {
        g_cap_pending = false;
        if (g_scratch == 0u) {
            const u32 block = call(co, FN_NEW, 0x40u, 0, 0, 0);
            if (guest_ptr(block))
                g_scratch = block;
        }
        Pose* pose = &g_cap_pose;
        Mat34 base, inv;
        const Xf bx = {{pose->base_q[0], pose->base_q[1], pose->base_q[2], pose->base_q[3]},
                       {pose->base_pos[0], pose->base_pos[1], pose->base_pos[2]}, {1, 1, 1}};
        mat_from_xf(&bx, &base);
        if (g_scratch != 0u && mat_inverse(&base, &inv) && pose->att_n < ATT_MAX) {
            mem_write32(cpu, g_scratch, 0u);
            call(co, FN_SEARCH_BY_ID, mem_read32(cpu, EVENT_ITEM_PARTNER), g_scratch, 0, 0);
            const u32 actor = mem_read32(cpu, g_scratch);
            if (guest_ptr(actor) && (mem_read8(cpu, actor + ITEM_ACTOR_DRAW) & 1u) != 0u)
                capture_att(cpu, mem_read32(cpu, actor + ITEM_ACTOR_MODEL), &inv, ATT_KIND_GET, pose);
        }
        publish_local(pose);
    }
    bluewake_pvp_fiber(co, cpu); // the bodies and target actors of the puppets (puppet_pvp.c)
    bluewake_carry_fiber(co, cpu); // a lifted Link crouches, a hold is let go (puppet_carry.c)
    // Puppets are made (or made again) before any is drawn, the most recently active player's first, so that when the game's
    // heaps cannot hold every custom body the ones that are moving get them; and a player who is active while others sit
    // still takes a custom body's room from the one that has been still longest (that one is stock for 20 seconds).
    {
        int order[MAX_REMOTES];
        for (int n = 0; n < w->count; ++n)
            order[n] = n;
        for (int x = 1; x < w->count; ++x) {
            const int v = order[x];
            int y = x - 1;
            while (y >= 0 && w->item[order[y]].active_ms < w->item[v].active_ms) {
                order[y + 1] = order[y];
                --y;
            }
            order[y + 1] = v;
        }
        for (int oi = 0; oi < w->count; ++oi) {
            Item* it = &w->item[order[oi]];
            Guest* g = &g_guest[it->slot];
            if (it->recreate && g->heap != 0u) {
                // The player's own model came after the puppet was made as Link's (or room came free): made again.
                att_free(co, g);
                destroy_puppet_heap(co, g->heap);
                ds_release(co, g);
                memset(g_ptex[it->slot], 0, sizeof g_ptex[it->slot]);
                memset(g, 0, sizeof *g);
            }
            if (g->failed) {
                if (++g->retry < 120) // no room just now (a scene loading): tried again in a couple of seconds
                    continue;
                memset(g, 0, sizeof *g);
            }
            if (g->model == 0u) {
                create_puppet(co, cpu, g, w->model_data, w->room, it->bdl, it->bdl_len, it->id, it->eq, it->eq_len);
                if (g->model != 0u && g->denied) {
                    // No room for what this player wears: the least active other player holding custom models gives theirs up.
                    int victim = -1;
                    for (int on = oi + 1; on < w->count; ++on) {
                        const Item* other = &w->item[order[on]];
                        const Guest* og = &g_guest[other->slot];
                        bool holds = false;
                        for (int k = 0; k <= EQUIP_SLOTS; ++k)
                            holds |= og->ds[k] != 0;
                        if (holds && other->active_ms + 3000.0 < it->active_ms)
                            victim = on; // the last in the order holding any: the least active
                    }
                    if (victim >= 0) {
                        Item* vi = &w->item[order[victim]];
                        Guest* vg = &g_guest[vi->slot];
                        fprintf(stderr, "[puppet] player %d (active) takes the room of player %d (idle for %.0f s): that puppet is stock for 20 s\n", it->id, vi->id,
                                (it->active_ms - vi->active_ms) / 1000.0);
                        att_free(co, vg);
                        if (vg->heap != 0u)
                            destroy_puppet_heap(co, vg->heap);
                        ds_release(co, vg);
                        memset(g_ptex[vi->slot], 0, sizeof g_ptex[vi->slot]);
                        memset(vg, 0, sizeof *vg);
                        free(vi->bdl);
                        vi->bdl = NULL;
                        for (int k = 0; k < EQUIP_SLOTS; ++k) {
                            free(vi->eq[k]);
                            vi->eq[k] = NULL;
                        }
                        lock();
                        Remote* vr = &g_remote[vi->slot];
                        vr->stock_until_ms = now_ms() + 20000.0;
                        vr->retry_custom_ms = vr->stock_until_ms;
                        unlock();
                        // This puppet again, now that there is room.
                        att_free(co, g);
                        destroy_puppet_heap(co, g->heap);
                        ds_release(co, g);
                        memset(g_ptex[it->slot], 0, sizeof g_ptex[it->slot]);
                        memset(g, 0, sizeof *g);
                        create_puppet(co, cpu, g, w->model_data, w->room, it->bdl, it->bdl_len, it->id, it->eq, it->eq_len);
                    }
                }
                free(it->bdl);
                it->bdl = NULL;
                for (int k = 0; k < EQUIP_SLOTS; ++k) {
                    free(it->eq[k]);
                    it->eq[k] = NULL;
                }
            }
        }
    }
    const bool prof = prof_on();
    double prof_prev = prof ? prof_now_us() : 0.0;
    const double prof_start = prof_prev;
    int prof_puppets = 0;
#define PROF_PHASE(k)     do {         if (prof) {             const double t_ = prof_now_us();             g_prof_phase[k] += t_ - prof_prev;             prof_prev = t_;         }     } while (0)
    for (int n = 0; n < w->count; ++n) {
        Item* it = &w->item[n];
        Guest* g = &g_guest[it->slot];
        if (g->model == 0u)
            continue;
        if (prof)
            ++g_prof_lod[it->lod];
        if (it->lod == LOD_HIDDEN) // off the screen: nothing is done for it this frame (see LOD_HIDDEN)
            continue;
        ++prof_puppets;
        const bool far_off = it->lod == LOD_FAR || it->lod == LOD_VERY_FAR;
        const bool skip_pose = far_off && (g->tick++ & 1u) != 0u; // far: posed and lit every other frame, drawn with the last
        prof_prev = prof ? prof_now_us() : 0.0;
        // The pose: the matrices the game would have calculated.
        if (g->remap) {
            // Link's joints relative to the base, to each joint's own translation from its parent.
            Mat34 inv_base, rel[CL_JOINTS], fixed[CL_JOINTS];
            if (mat_inverse(&it->base, &inv_base)) {
                for (int i = 0; i < CL_JOINTS; ++i)
                    mat_mul(&inv_base, &it->node[i], &rel[i]);
                for (int n2 = 0; n2 < CL_JOINTS; ++n2) {
                    const int j = g->order[n2];
                    const int pj = g->parent[j];
                    Mat34 local = rel[j];
                    if (pj >= 0) {
                        Mat34 inv_parent;
                        if (!mat_inverse(&rel[pj], &inv_parent))
                            inv_parent = rel[pj];
                        mat_mul(&inv_parent, &rel[j], &local);
                        const float dx = local.m[0][3] - g->bind_link[j][0], dy = local.m[1][3] - g->bind_link[j][1],
                                    dz = local.m[2][3] - g->bind_link[j][2];
                        if (dx * dx + dy * dy + dz * dz < 0.0025f) // not animated: the bone is as long as the model's own
                            for (int k = 0; k < 3; ++k)
                                local.m[k][3] = g->bind_own[j][k];
                        mat_mul(&fixed[pj], &local, &fixed[j]);
                    } else
                        fixed[j] = local;
                }
                for (int i = 0; i < CL_JOINTS; ++i)
                    mat_mul(&it->base, &fixed[i], &it->node[i]);
            }
        }
        if (!skip_pose) {
        write_mat(cpu, g->model + MODEL_BASE_MTX, &it->base);
        const u32 nodes = mem_read32(cpu, g->model + MODEL_NODE_MTX);
        if (!guest_ptr(nodes))
            continue;
        for (int i = 0; i < CL_JOINTS; ++i)
            write_mat(cpu, nodes + 48u * (u32)i, &it->node[i]);
        // The scale flags the game's own calc leaves (the envelope and normal
        // matrices depend on them): the local Link's, which has the same joints.
        {
            const u32 cl = mem_read32(cpu, player + PLAYER_CL_MODEL);
            const u32 evlp = mem_read16(cpu, w->model_data + 0x30u); // J3DModelData::mJointTree.mWEvlpMtxNum
            for (u32 k = 0; k < 2u; ++k) {
                const u32 from = mem_read32(cpu, cl + 0x84u + 4u * k), to = mem_read32(cpu, g->model + 0x84u + 4u * k);
                const u32 n = k == 0u ? (u32)CL_JOINTS : evlp;
                if (guest_ptr(cl) && from != 0u && to != 0u)
                    for (u32 b = 0; b < n && b < 256u; ++b)
                        mem_write8(cpu, to + b, (u8)mem_read8(cpu, from + b));
            }
        }
        PROF_PHASE(0);
        mem_write32(cpu, J3D_SYS + 0x38u, g->model); // J3DModel::calc sets it first
        call(co, FN_CALC_WEIGHT_ENV, g->model, 0, 0, 0);
        }
        if (ENV_SET("BLUEWAKE_PUPPET_COPYW")) { // diagnosis: Link's own envelope matrices, moved to the puppet
            const u32 cl = mem_read32(cpu, player + PLAYER_CL_MODEL);
            const u32 n = mem_read16(cpu, w->model_data + 0x30u);
            const u32 from = mem_read32(cpu, cl + 0x90u), to = mem_read32(cpu, g->model + 0x90u);
            Mat34 lb;
            read_mat(cpu, cl + MODEL_BASE_MTX, &lb);
            if (guest_ptr(from) && guest_ptr(to))
                for (u32 k = 0; k < n && k < 64u; ++k) {
                    Mat34 m;
                    read_mat(cpu, from + 48u * k, &m);
                    {
                        static int reported;
                        Mat34 mine;
                        read_mat(cpu, to + 48u * k, &mine);
                        float worst = 0.0f;
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 4; ++c)
                                worst = fmaxf(worst, fabsf(mine.m[r][c] - (m.m[r][c] + (c == 3 ? it->base.m[r][3] - lb.m[r][3] : 0.0f))));
                        if (worst > 1.0f && reported < 40) {
                            ++reported;
                            fprintf(stderr, "[puppet] envelope %u differs from Link's by %.2f\n", k, worst);
                        }
                    }
                    for (int r = 0; r < 3; ++r)
                        m.m[r][3] += it->base.m[r][3] - lb.m[r][3];
                    write_mat(cpu, to + 48u * k, &m);
                }
            static int once;
            if (!once++)
                fprintf(stderr, "[puppet] copied %u envelope matrices\n", n);
        }
        for (int i = 0; i < 3; ++i)
            write_float(cpu, g->pos + 4u * (u32)i, it->pos[i]);
        if (it->voice_proc >= 0) {
            // What he just started doing, as the game makes it at that place: his voice (the local voice bank: a
            // peer's own voice pack is read once when audio starts and cannot be played) and a sound effect.
            for (int i = 0; i < 3; ++i)
                write_float(cpu, g->pos + 0x10u + 4u * (u32)i, it->pos[i] + (i == 1 ? 115.0f : 0.0f));
            const u32 jai = mem_read32(cpu, JAI_BASIC);
            const unsigned proc = (unsigned)it->voice_proc;
            const u32 voice_id = it->voice_force >= 0 ? (u32)it->voice_force : proc < sizeof kProcVoice ? (u32)kProcVoice[proc] : 0xFFu;
            // A peer whose voice bank is not the one the game loaded sounds from their own, decoded by the app and mixed into the
            // output at the puppet's place; the game's voice is then not made.
            bool own_bank = false;
            if (voice_id != 0xFFu && g_voice_player != NULL && it->voice[0] != '\0' && !it->voice_local) {
                const float head[3] = {it->pos[0], it->pos[1] + 115.0f, it->pos[2]};
                float left = 0.0f, right = 0.0f;
                if (voice_placement(cpu, head, &left, &right))
                    own_bank = g_voice_player(it->id, (int)voice_id, left, right, it->voice_pitch > 0.0f ? it->voice_pitch : 1.0f);
                else
                    own_bank = true; // too far or no camera: silent, as the game's own sound would be
            }
            if (guest_ptr(jai) && voice_id != 0xFFu && !own_bank) {
                const u32 vowel = call(co, FN_LINK_VOICE_VOWEL, jai, voice_id, 0, 0);
                bluewake_voice_pitch_set_scope(it->voice_pitch > 0.0f ? it->voice_pitch : 1.0f); // this player's pitch, not ours
                bluewake_guest_call(co, FN_LINK_VOICE_START, jai, voice_id, g->pos + 0x10u, vowel & 0xFFu, 0, 0);
                bluewake_voice_pitch_set_scope(0.0f);
            }
            const u32 effect = proc_effect(proc);
            if (it->voice_force >= 0)
                fprintf(stderr, "[puppet] sweep: voice id %d at retrace %llu\n", it->voice_force, bluewake_host_retrace_count());
            if (effect != 0u || (proc < sizeof kProcVoice && kProcVoice[proc] != 0xFFu))
                fprintf(stderr, "[puppet] sounds for player %d proc 0x%X: voice %u (%s, %s bank), effect %04X\n", it->id, proc,
                        proc < sizeof kProcVoice ? kProcVoice[proc] : 255u, it->voice[0] != '\0' ? it->voice : "game's voice",
                        own_bank ? "played from its own voice bank" : it->voice_local ? "the voice bank matches" : "the local game voice bank plays: its own bank is not available", effect);
            if (effect != 0u)
                call(co, FN_SE_START, effect, g->pos + 0x10u, 0, 0);
        }
        if (!skip_pose) {
            call(co, FN_SETTING_TEVSTRUCT, ENV_LIGHT, 9u, g->pos, g->tevstr);
            call(co, FN_SET_LIGHT_TEV, ENV_LIGHT, g->model, g->tevstr, 0);
        }
        PROF_PHASE(1);
        enter_variant_packets(co, cpu, g, 0); // (drawn after the puppet's models)
        draw_puppet(co, cpu, g, w->model_data, player, it);
        enter_variant_packets(co, cpu, g, 1); // (drawn before them)
        {
            static unsigned said;
            if (said++ < 3 || (said & 0x3FFF) == 0)
                fprintf(stderr, "[puppet] slot %d (player %d): texture variant packets entered around its models (%08X %08X %08X)\n", it->slot, it->id,
                        g->var[1][0], g->var[1][1], g->var[1][2]);
        }
        PROF_PHASE(2);
        if (!far_off) // far puppets have no shadow
            puppet_shadow(co, cpu, g, it);
        PROF_PHASE(3);
    }
#undef PROF_PHASE
    if (prof) {
        static int draws;
        g_prof_phase[4] += prof_now_us() - prof_start;
        if (prof_puppets != 0 && ++draws >= 300) {
            prof_report(draws, prof_puppets);
            draws = 0;
        }
    }
    mem_write32(cpu, J3D_SYS + 0x48u, saved_opa);
    mem_write32(cpu, J3D_SYS + 0x4Cu, saved_xlu);
    mem_write32(cpu, J3D_SYS + 0x38u, saved_model);
    mem_write32(cpu, J3D_SYS + 0x58u, saved_tex);
}

static void delete_body(GuestCoro* co, void* arg) {
    (void)arg;
    bluewake_pvp_fiber_delete(co, bluewake_coro_cpu(co));
    for (int i = 0; i < MAX_REMOTES; ++i) {
        att_free(co, &g_guest[i]);
        if (g_guest[i].heap != 0u)
            destroy_puppet_heap(co, g_guest[i].heap);
        ds_release(co, &g_guest[i]);
        memset(g_ptex[i], 0, sizeof g_ptex[i]);
        memset(&g_guest[i], 0, sizeof g_guest[i]);
    }
    for (int i = 0; i < POOLS; ++i)
        if (g_pools[i].heap != 0u)
            fprintf(stderr, "[puppet] scene end: %s pool %u of %u KB free, %d heaps in use, peak use %u KB\n", g_pools[i].name, pool_free_of(co, &g_pools[i]) >> 10,
                    g_pools[i].size >> 10, g_pools[i].users, g_pools[i].peak_used >> 10);
    g_scene_id = 0u;
}

// A remote's interpolated pose at render time; false if it is not to be drawn (or, for the minimap, not to be shown).
// Playback runs a little behind (PLAYBACK_DELAY) so the next pose is already here: in the sender's own time when it
// sends its clock (the arrival times of a pose that took longer than the one before it do not make it hitch), else in
// ours. Past the newest pose (a late one) the body goes on along its last velocity for a moment, then holds.
enum { PLAYBACK_DELAY_MS = 140, EXTRAPOLATE_MS = 120 };
static bool remote_pose_any(const Remote* r, double now, Pose* out, bool need_show) {
    if ((need_show && !r->show) || r->count == 0 || now - r->snap[r->count - 1].time > 1500.0)
        return false;
    const bool sender_time = r->have_clock && r->snap[0].stime >= 0.0 && r->snap[r->count - 1].stime >= 0.0;
    const double t = sender_time ? now - r->clock_off - PLAYBACK_DELAY_MS : now - 100.0;
    #define SNAP_T(sn) (sender_time ? (sn)->stime : (sn)->time)
    if (r->count == 1 || t <= SNAP_T(&r->snap[0])) {
        *out = r->snap[0].pose;
        return true;
    }
    for (int i = 0; i + 1 < r->count; ++i) {
        const Snap* a = &r->snap[i];
        const Snap* b = &r->snap[i + 1];
        if (t <= SNAP_T(b)) {
            const double span = SNAP_T(b) - SNAP_T(a);
            pose_lerp(&a->pose, &b->pose, span > 1.0 ? (float)((t - SNAP_T(a)) / span) : 1.0f, out);
            return true;
        }
    }
    const Snap* last = &r->snap[r->count - 1];
    const Snap* before = &r->snap[r->count - 2];
    *out = last->pose;
    const double ahead = t - SNAP_T(last), span = SNAP_T(last) - SNAP_T(before);
    if (sender_time && ahead > 0.0 && span > 1.0) {
        const double along = (ahead < EXTRAPOLATE_MS ? ahead : EXTRAPOLATE_MS) / span;
        for (int i = 0; i < 3; ++i)
            out->base_pos[i] += (float)((last->pose.base_pos[i] - before->pose.base_pos[i]) * (along > 2.0 ? 2.0 : along));
    }
    #undef SNAP_T
    return true;
}

// Where a puppet is for the draw: LOD_HIDDEN when its bounding sphere is out of the camera's frustum (or beyond the draw
// distance), LOD_FAR when it is far off. proj_view is the camera's projection times view (the tag code's), clip space
// (x, y, z, w) from a column vector; a sphere is out when it is beyond a side plane by more than its radius.
enum { PUPPET_RADIUS = 130, PUPPET_FAR_DISTANCE = 1600, PUPPET_VERY_FAR_DISTANCE = 3500, PUPPET_HIDE_DISTANCE = 9000 };
static int puppet_view(const float* pv, const float pos[3]) {
    const float x = pos[0], y = pos[1] + 60.0f, z = pos[2]; // about the middle of the body
    float c[4];
    for (int k = 0; k < 4; ++k)
        c[k] = pv[k * 4] * x + pv[k * 4 + 1] * y + pv[k * 4 + 2] * z + pv[k * 4 + 3];
    const float r = (float)PUPPET_RADIUS;
    // The side planes are the rows w+x, w-x, w+y, w-y of proj_view: a point's distance from one is its row times the point
    // over the row's xyz length.
    const float side[4] = {c[3] + c[0], c[3] - c[0], c[3] + c[1], c[3] - c[1]};
    const float sign_[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int k = 0; k < 4; ++k) {
        float nx = 0.0f;
        for (int m = 0; m < 3; ++m) {
            const float row = pv[12 + m] + sign_[k][0] * pv[m] + sign_[k][1] * pv[4 + m];
            nx += row * row;
        }
        if (side[k] < -r * sqrtf(nx))
            return LOD_HIDDEN;
    }
    // Behind the camera, and the distance along its view (w is the depth for a perspective projection).
    const float lw = sqrtf(pv[12] * pv[12] + pv[13] * pv[13] + pv[14] * pv[14]);
    if (c[3] < -r * lw)
        return LOD_HIDDEN;
    if (c[3] > (float)PUPPET_HIDE_DISTANCE)
        return LOD_HIDDEN;
    return c[3] > (float)PUPPET_VERY_FAR_DISTANCE ? LOD_VERY_FAR : c[3] > (float)PUPPET_FAR_DISTANCE ? LOD_FAR : LOD_NEAR;
}

static bool remote_pose(const Remote* r, double now, Pose* out) { return remote_pose_any(r, now, out, true); }

// ---- the minimap ----
// Another player in my stage and room is an arrow on the minimap in their color, placed as the game places Link's own
// (dMap_c::drawPointGc: the point's offset from the map's center, scaled, around the map's top left) and drawn by the
// game's own cursor object: dMap_c::mCursor (a dMap_2DTri_c) copied, then its position, angle and color set, and put in
// the 2D opaque list's end while the map is drawn (Link's own is not in that list), so it draws under the same
// scissor and alpha at every aspect.
enum {
    MAP_POINTS_FIRST = 0x803C35A4u,    // dMap_c::mPoint[0], the first of the list's points
    MAP_CURSOR = 0x803C3984u,          // dMap_c::mCursor, 0x28 bytes: vtable, s16 x +4, y +6, GXColor +8, s16 angle +0xC
    MAP_PLAYER_COUNT = 0x803F6A8Au,    // dMap_c::mDrawPointCntPlayer
    MAP_DISP_LEFT = 0x803F6AD8u,       // mDispPosLeftUpX (s16), Y at +2
    MAP_CENTER_X = 0x803F6AF8u,        // mNowCenterX, Z at +4, scale X at +8, scale Z at +0xC (floats)
    MAP_ROOM_INFO = 0x803F6B18u,       // mNowRoomInfoP
    DRAWLIST_2DOPA = 0x19Cu,           // dDlst_list_c::mp2DOpa (pointer), mp2DOpaEnd at +4
    MAP_VIEW_HALF = 0x78,
    MAP_VIEW_CENTER = 0x3C,
};

static bool map_alloc_wanted(void) {
    if (g_map_cursors != 0u)
        return false;
    bool any = false;
    lock();
    for (int i = 0; i < MAX_REMOTES; ++i)
        any |= g_remote[i].id != 0 && g_remote[i].map;
    unlock();
    return any;
}

static s16 read_s16(CPUState* cpu, u32 address) { return (s16)mem_read16(cpu, address); }

static void minimap_pass(CPUState* cpu) {
    static int reported;
    if (g_map_cursors == 0u || !guest_ptr(mem_read32(cpu, MAP_ROOM_INFO))) {
        static int why;
        if (why++ % 120 == 0 && getenv("BLUEWAKE_PUPPET_MAP_LOG") != NULL)
            fprintf(stderr, "[puppet] minimap: not drawn (cursors %08X, players on the map %u, room info %08X)\n", g_map_cursors,
                    (unsigned)mem_read8(cpu, MAP_PLAYER_COUNT), mem_read32(cpu, MAP_ROOM_INFO));
        return;
    }
    const u32 list = DRAW_LIST;
    u32 top = mem_read32(cpu, list + DRAWLIST_2DOPA);
    const u32 end = mem_read32(cpu, list + DRAWLIST_2DOPA + 4u);
    const u32 first = list + 0x9Cu; // mp2DOpaArr
    if (!guest_ptr(top) || top < first || top > end)
        return;
    // The map is on screen this frame when its own objects (the enemy points, mPoint[], in dMap_c's statics) are in
    // the list. Link's own cursor is not in the list at this hook (dMap_c draws it by itself), so the copies go at
    // the list's end; each carries the cursor's scissor, which keeps it to the map's window. (11d: a place right
    // after Link's cursor, as first written, never existed.)
    bool map_shown = false;
    for (u32 p = first; p < top && !map_shown; p += 4u) {
        const u32 o = mem_read32(cpu, p);
        map_shown = o >= MAP_POINTS_FIRST && o < MAP_CURSOR;
    }
    if (!map_shown || top <= first) {
        if (reported++ < 2 && getenv("BLUEWAKE_PUPPET_MAP_LOG") != NULL)
            fprintf(stderr, "[puppet] minimap: the map is not drawn this frame\n");
        return;
    }
    const u32 at = top - 4u;
    const float cx = read_float(cpu, MAP_CENTER_X), cz = read_float(cpu, MAP_CENTER_X + 4u);
    const float sx = read_float(cpu, MAP_CENTER_X + 8u), sz = read_float(cpu, MAP_CENTER_X + 12u);
    const s16 left = read_s16(cpu, MAP_DISP_LEFT), up = read_s16(cpu, MAP_DISP_LEFT + 2u);
    const double now = now_ms();
    u32 made[MAP_ICONS];
    int count = 0;
    lock();
    for (int i = 0; i < MAX_REMOTES && count < MAP_ICONS; ++i) {
        Remote* r = &g_remote[i];
        Pose pose;
        if (r->id == 0 || !r->map || !remote_pose_any(r, now, &pose, false))
            continue;
        const float x = pose.base_pos[0], z = pose.base_pos[2];
        s16 dx = (s16)(sx * (x - cx)), dy = (s16)(sz * (z - cz));
        if (dx + 2 < -MAP_VIEW_HALF || dx - 2 > MAP_VIEW_HALF || dy + 2 < -MAP_VIEW_HALF || dy - 2 > MAP_VIEW_HALF)
            continue;
        dx += (s16)(left + MAP_VIEW_CENTER);
        dy += (s16)(up + MAP_VIEW_CENTER);
        // His facing: the base matrix's +Z axis seen from above, as the game's shape angle (0 = +Z, 0x8000 = -Z).
        Mat34 base;
        const Xf bx = {{pose.base_q[0], pose.base_q[1], pose.base_q[2], pose.base_q[3]}, {0, 0, 0}, {1, 1, 1}};
        mat_from_xf(&bx, &base);
        const float yaw = atan2f(base.m[0][2], base.m[2][2]);
        const s16 angle = (s16)lrintf(yaw * (32768.0f / 3.14159265f));
        const u32 c = g_map_cursors + MAP_CURSOR_SIZE * (u32)count;
        for (u32 o = 0; o < MAP_CURSOR_SIZE; o += 4u)
            mem_write32(cpu, c + o, mem_read32(cpu, MAP_CURSOR + o));
        mem_write16(cpu, c + 4u, (u16)dx);
        mem_write16(cpu, c + 6u, (u16)dy);
        mem_write8(cpu, c + 8u, r->color[0]);
        mem_write8(cpu, c + 9u, r->color[1]);
        mem_write8(cpu, c + 10u, r->color[2]); // alpha (+11) stays the game's
        mem_write16(cpu, c + 0xCu, (u16)angle);
        made[count++] = c;
    }
    unlock();
    if (count == 0 || top + 4u * (u32)count > end)
        return;
    // Open a gap after Link's arrow and put the copies in it.
    for (u32 p = top; p > at + 4u; p -= 4u)
        mem_write32(cpu, p + 4u * (u32)count - 4u, mem_read32(cpu, p - 4u));
    for (int k = 0; k < count; ++k)
        mem_write32(cpu, at + 4u + 4u * (u32)k, made[k]);
    mem_write32(cpu, list + DRAWLIST_2DOPA, top + 4u * (u32)count);
    static bool said;
    static unsigned frames;
    if (!said) {
        said = true;
        fprintf(stderr, "[puppet] minimap: %d arrows after Link's (his angle %d)\n", count, (int)read_s16(cpu, MAP_CURSOR + 0xCu));
    }
    if (++frames % 120 == 0 && getenv("BLUEWAKE_PUPPET_MAP_LOG") != NULL)
        fprintf(stderr, "[puppet] minimap: Link's arrow at %d,%d angle %d; the first of %d at %d,%d angle %d (scale %.5f,%.5f)\n",
                (int)read_s16(cpu, MAP_CURSOR + 4u), (int)read_s16(cpu, MAP_CURSOR + 6u), (int)read_s16(cpu, MAP_CURSOR + 0xCu), count,
                (int)read_s16(cpu, made[0] + 4u), (int)read_s16(cpu, made[0] + 6u), (int)read_s16(cpu, made[0] + 0xCu), sx, sz);
}

// ---- the Sea Chart ----
// Another player is an arrow on the pause menu's Sea Chart (dMenu_Fmap_c), wherever they are: at sea their place, ashore
// their island's square. The chart is one J2D screen drawn by dDlst_FMAP_c::draw; Link's own arrow is its "lnk1" pane
// (whole sea) and "lnk2" pane (a square, in the chart "clb" pane). Each arrow here is a copy of that pane (a J2DPicture:
// the same sprite), put at the place the game puts Link's from the chart's own pane (mAreaPane + position * 56 / 100000;
// in a square, mClbPane's size / 2 + offset * (size - 50) / 100000), rotated as it rotates Link's, and its white colour set
// to the player's. The copies hang in a copy of the screen's chain of parents (the screen, then each pane down to the
// arrow's parent, as plain panes, so they draw nothing and the arrows inherit the same matrices and alpha), and a copy
// of the dDlst_FMAP_c object that draws that chain is put at the end of the 2D list, over the chart. The game's own tree
// is never changed.
enum {
    CHART_DLST_VT = 0x80392594u,  // __vt__12dDlst_FMAP_c
    J2DPANE_VT = 0x8039D680u,     // __vt__7J2DPane
    J2DSCREEN_VT = 0x8039D6B8u,   // __vt__9J2DScreen
    FMAP_DLST = 0x1Cu,            // dMenu_Fmap_c::fmapDl
    FMAP_SV = 0x2878u,            // dMenu_Fmap_c::fmapSv (zoom +1, curX +4, curY +5)
    FMAP_AREA = 0x2DCCu,          // fopMsgM_pane_class mAreaPane (mPosCenterOrig +0x14, mSizeOrig +0x24)
    FMAP_LNK1 = 0x2E04u,
    FMAP_LNK2 = 0x2EE4u,
    FMAP_CLB = 0x30A4u,
    FMAP_MAIN_PROC = 0x5113u,     // 1: the comparison screen (dMenu_Fmap2_c)
    J2D_BOUNDS = 0x0Cu,           // mBounds: i.x, i.y, f.x, f.y
    J2D_MTX = 0x3Cu,
    J2D_BASE = 0x9Cu,             // mBasePosition x, y
    J2D_ROT = 0xA4u,
    J2D_VISIBLE = 0xAAu,
    J2D_ALPHA = 0xACu,
    J2D_TREE = 0xB0u,             // JSUTree: head, tail, count, then the link: object +0xBC, list +0xC0, prev, next
    J2D_PANE_SIZE = 0xCCu,
    J2D_SCREEN_SIZE = 0xD4u,
    J2D_PICTURE_SIZE = 0x124u,
    J2D_WHITE = 0x104u,           // J2DPicture::mColorWhite (r, g, b, a)
    CHART_ANC_MAX = 8,
};

static void chart_copy(CPUState* cpu, u32 dst, u32 src, u32 size) {
    for (u32 o = 0; o < size; o += 4u)
        mem_write32(cpu, dst + o, mem_read32(cpu, src + o));
}

// The pane's tree link: it is `pane` itself, a child of `parent` (0: none), between `prev` and `next` panes (0: none).
static void chart_link(CPUState* cpu, u32 pane, u32 parent, u32 prev, u32 next) {
    mem_write32(cpu, pane + 0xBCu, pane);
    mem_write32(cpu, pane + 0xC0u, parent ? parent + J2D_TREE : 0u);
    mem_write32(cpu, pane + 0xC4u, prev ? prev + 0xBCu : 0u);
    mem_write32(cpu, pane + 0xC8u, next ? next + 0xBCu : 0u);
}

static void chart_children(CPUState* cpu, u32 pane, u32 first, u32 last, u32 count) {
    mem_write32(cpu, pane + J2D_TREE, first ? first + 0xBCu : 0u);
    mem_write32(cpu, pane + J2D_TREE + 4u, last ? last + 0xBCu : 0u);
    mem_write32(cpu, pane + J2D_TREE + 8u, count);
}

typedef struct ChartPoint {
    float x, z;
    int angle;
    uint8_t color[3];
} ChartPoint;

static bool chart_alloc_wanted(void) {
    if (g_chart_mem != 0u)
        return false;
    bool any = false;
    lock();
    for (int i = 0; i < MAX_REMOTES; ++i)
        any |= g_remote[i].id != 0 && g_remote[i].chart;
    unlock();
    return any;
}

static void chart_pass(CPUState* cpu) {
    static unsigned frames;
    const bool log = getenv("BLUEWAKE_PUPPET_CHART_LOG") != NULL;
    if (g_chart_mem == 0u)
        return;
    const u32 list = DRAW_LIST;
    const u32 top = mem_read32(cpu, list + DRAWLIST_2DOPA);
    const u32 end = mem_read32(cpu, list + DRAWLIST_2DOPA + 4u);
    const u32 first = list + 0x9Cu;
    if (!guest_ptr(top) || top < first || top + 4u > end)
        return;
    u32 dl = 0u;
    for (u32 p = first; p < top && dl == 0u; p += 4u) {
        const u32 o = mem_read32(cpu, p);
        if (guest_ptr(o) && mem_read32(cpu, o) == CHART_DLST_VT)
            dl = o;
    }
    if (dl == 0u)
        return;
    const u32 self = dl - FMAP_DLST, scrn = mem_read32(cpu, dl + 4u);
    const u32 sv = mem_read32(cpu, self + FMAP_SV);
    if (!guest_ptr(scrn) || !guest_ptr(sv) || mem_read8(cpu, self + FMAP_MAIN_PROC) != 0u)
        return;
    const unsigned zoom = mem_read8(cpu, sv + 1u);
    if (zoom > 1u)
        return; // the island view (zoom 2) is not done
    const s8 cur_x = (s8)mem_read8(cpu, sv + 4u), cur_z = (s8)mem_read8(cpu, sv + 5u);
    const u32 src = mem_read32(cpu, self + (zoom == 0u ? FMAP_LNK1 : FMAP_LNK2));
    if (!guest_ptr(src))
        return;

    // The arrow's parents up to the screen.
    u32 chain[CHART_ANC_MAX]; // chain[0] the nearest parent
    int depth = 0;
    for (u32 cur = src; depth < CHART_ANC_MAX;) {
        const u32 owner = mem_read32(cpu, cur + 0xC0u);
        if (owner == 0u)
            break;
        cur = owner - J2D_TREE;
        if (!guest_ptr(cur))
            return;
        chain[depth++] = cur;
    }
    if (depth == 0 || chain[depth - 1] != scrn || mem_read32(cpu, scrn) != J2DSCREEN_VT) {
        static int said;
        if (said++ < 3 && log)
            fprintf(stderr, "[puppet] sea chart: the arrow's parents do not end at the screen (%d, top %08X, screen %08X, vt %08X)\n",
                    depth, depth ? chain[depth - 1] : 0u, scrn, mem_read32(cpu, scrn));
        return;
    }

    // Who is shown, and where on the pane (its centre).
    const float w = read_float(cpu, src + J2D_BOUNDS + 8u) - read_float(cpu, src + J2D_BOUNDS);
    const float h = read_float(cpu, src + J2D_BOUNDS + 12u) - read_float(cpu, src + J2D_BOUNDS + 4u);
    ChartPoint pts[MAP_ICONS];
    float cx[MAP_ICONS], cy[MAP_ICONS];
    int count = 0;
    lock();
    for (int i = 0; i < MAX_REMOTES && count < MAP_ICONS; ++i) {
        const Remote* r = &g_remote[i];
        if (r->id == 0 || !r->chart)
            continue;
        float x, y;
        if (zoom == 0u) {
            x = read_float(cpu, self + FMAP_AREA + 0x14u) + r->chart_x * 56.0f / 100000.0f;
            y = read_float(cpu, self + FMAP_AREA + 0x18u) + r->chart_z * 56.0f / 100000.0f;
        } else {
            const float dx = r->chart_x - 100000.0f * (float)cur_x, dz = r->chart_z - 100000.0f * (float)cur_z;
            if (dx < -50000.0f || dx > 50000.0f || dz < -50000.0f || dz > 50000.0f)
                continue; // not in the square on show
            const float sx = read_float(cpu, self + FMAP_CLB + 0x24u), sz = read_float(cpu, self + FMAP_CLB + 0x28u);
            x = sx / 2.0f + dx * ((sx - 50.0f) / 100000.0f);
            y = sz / 2.0f + dz * ((sz - 50.0f) / 100000.0f);
        }
        pts[count].x = r->chart_x;
        pts[count].z = r->chart_z;
        pts[count].angle = r->chart_angle;
        memcpy(pts[count].color, r->color, 3);
        cx[count] = x;
        cy[count] = y;
        ++count;
    }
    unlock();
    if (count == 0)
        return;

    // The copies: the draw object, the screen, the parents (top down), then the arrows.
    u32 at = g_chart_mem;
    const u32 dlobj = at;
    at += 8u;
    const u32 root = at;
    at += J2D_SCREEN_SIZE;
    u32 copies[CHART_ANC_MAX];
    chart_copy(cpu, root, scrn, J2D_SCREEN_SIZE);
    mem_write8(cpu, root + 0xD3u, 0u); // the screen's own colour quad: none
    copies[depth - 1] = root;
    for (int k = depth - 2; k >= 0; --k) {
        copies[k] = at;
        at += J2D_PANE_SIZE;
        chart_copy(cpu, copies[k], chain[k], J2D_PANE_SIZE);
        mem_write32(cpu, copies[k], J2DPANE_VT); // a plain pane: nothing of its own to draw
    }
    chart_link(cpu, root, 0u, 0u, 0u);
    for (int k = depth - 2; k >= 0; --k) {
        chart_link(cpu, copies[k], copies[k + 1], 0u, 0u);
        chart_children(cpu, copies[k], 0u, 0u, 0u);
    }
    for (int k = depth - 1; k >= 1; --k)
        chart_children(cpu, copies[k], copies[k - 1], copies[k - 1], 1u);
    u32 arrows[MAP_ICONS];
    for (int k = 0; k < count; ++k) {
        const u32 a = at;
        at += J2D_PICTURE_SIZE;
        arrows[k] = a;
        chart_copy(cpu, a, src, J2D_PICTURE_SIZE);
        chart_children(cpu, a, 0u, 0u, 0u);
        const float ix = cx[k] - w / 2.0f, iy = cy[k] - h / 2.0f;
        write_float(cpu, a + J2D_BOUNDS, ix);
        write_float(cpu, a + J2D_BOUNDS + 4u, iy);
        write_float(cpu, a + J2D_BOUNDS + 8u, ix + w);
        write_float(cpu, a + J2D_BOUNDS + 12u, iy + h);
        // J2DPane::rotate(w/2, h/2, 'z', deg) then makeMatrix(i.x, i.y): Link's arrow turns the way the ship's points.
        const float deg = (float)(s16)(pts[k].angle + 0x8000) * 180.0f / 32768.0f;
        const float bx = (float)(s32)(w * 0.5f), by = (float)(s32)(h * 0.5f);
        const float th = -deg * 3.14159265f / 180.0f, c = cosf(th), s = sinf(th);
        write_float(cpu, a + J2D_BASE, bx);
        write_float(cpu, a + J2D_BASE + 4u, by);
        write_float(cpu, a + J2D_ROT, deg);
        mem_write8(cpu, a + 0xA8u, 'z');
        const float m[12] = {c, -s, 0.0f, bx + ix - (c * bx - s * by), s, c, 0.0f, by + iy - (s * bx + c * by), 0.0f, 0.0f, 1.0f, 0.0f};
        for (u32 j = 0; j < 12u; ++j)
            write_float(cpu, a + J2D_MTX + 4u * j, m[j]);
        mem_write8(cpu, a + J2D_VISIBLE, 1u);
        if (mem_read8(cpu, a + J2D_ALPHA) == 0u)
            mem_write8(cpu, a + J2D_ALPHA, 255u);
        mem_write8(cpu, a + J2D_WHITE, pts[k].color[0]);
        mem_write8(cpu, a + J2D_WHITE + 1u, pts[k].color[1]);
        mem_write8(cpu, a + J2D_WHITE + 2u, pts[k].color[2]);
    }
    for (int k = 0; k < count; ++k)
        chart_link(cpu, arrows[k], copies[0], k > 0 ? arrows[k - 1] : 0u, k + 1 < count ? arrows[k + 1] : 0u);
    chart_children(cpu, copies[0], arrows[0], arrows[count - 1], (u32)count);
    mem_write32(cpu, dlobj, CHART_DLST_VT);
    mem_write32(cpu, dlobj + 4u, root);
    mem_write32(cpu, top, dlobj);
    mem_write32(cpu, list + DRAWLIST_2DOPA, top + 4u);
    if (log && frames++ % 120 == 0)
        fprintf(stderr, "[puppet] sea chart: %d arrows, zoom %u, parents %d, arrow size %.1fx%.1f, first at %.1f,%.1f, white %08X\n", count,
                zoom, depth, w, h, cx[0], cy[0], mem_read32(cpu, src + J2D_WHITE));
}

static void build_item(const Pose* p, Item* it) {
    const Xf bx = {{p->base_q[0], p->base_q[1], p->base_q[2], p->base_q[3]}, {p->base_pos[0], p->base_pos[1], p->base_pos[2]}, {1, 1, 1}};
    mat_from_xf(&bx, &it->base);
    for (int i = 0; i < CL_JOINTS; ++i) {
        Mat34 local;
        mat_from_xf(&p->joint[i], &local);
        mat_mul(&it->base, &local, &it->node[i]);
    }
    memcpy(it->pos, p->base_pos, sizeof it->pos);
    it->flags = p->flags;
    it->sword = p->sword;
    it->shield = p->shield;
    it->hands = p->hands;
    Mat34 e;
    mat_from_xf(&p->sword_xf, &e);
    mat_mul(&it->base, &e, &it->sword_mtx);
    mat_from_xf(&p->shield_xf, &e);
    mat_mul(&it->base, &e, &it->shield_mtx);
    it->face_n = p->face_n;
    memcpy(it->face, p->face, sizeof it->face);
    it->sail = p->sail;
    if (p->sail) {
        mat_from_xf(&p->sail_xf, &e);
        mat_mul(&it->base, &e, &it->sail_mtx);
        memcpy(it->sail_pt, p->sail_pt, sizeof it->sail_pt);
    }
    it->line_n = p->line_n;
    for (int i = 0; i < p->line_n; ++i) {
        it->line[i].kind = p->line[i].kind;
        for (int k = 0; k < 2; ++k)
            for (int r = 0; r < 3; ++r)
                it->line[i].p[k][r] = it->base.m[r][0] * p->line[i].p[k][0] + it->base.m[r][1] * p->line[i].p[k][1] +
                                      it->base.m[r][2] * p->line[i].p[k][2] + it->base.m[r][3];
    }
    it->att_n = p->att_n;
    for (int i = 0; i < p->att_n; ++i) {
        const PoseAtt* a = &p->att[i];
        struct ItemAtt* o = &it->att[i];
        o->kind = a->kind;
        o->nj = a->nj;
        o->idx = a->idx;
        memcpy(o->arc, a->arc, ATT_ARC);
        mat_from_xf(&a->base, &e);
        mat_mul(&it->base, &e, &o->base);
        for (int j = 0; j < a->nj; ++j) {
            mat_from_xf(&a->joint[j], &e);
            mat_mul(&it->base, &e, &o->node[j]);
        }
    }
}

bool bluewake_puppet_draw_active(void) { return g_coro_draw != NULL && bluewake_coro_active(g_coro_draw); }

u32 bluewake_puppet_hook(CPUState* cpu, u32 id) {
    pools_locate(cpu);
    if (id == BLUEWAKE_PUPPET_HOOK_DELETE) {
        if (g_coro_delete == NULL)
            g_coro_delete = bluewake_coro_create(delete_body);
        if (!bluewake_coro_active(g_coro_delete)) {
            bluewake_pvp_scene_end(cpu); // its colliders out of the game's lists before their memory goes
            bool any = bluewake_pvp_has_block();
            for (int i = 0; i < MAX_REMOTES; ++i)
                any |= g_guest[i].heap != 0u;
            if (!any) {
                g_scene_id = 0u;
                return 0u;
            }
        }
        return bluewake_coro_step(g_coro_delete, cpu, SITE_DELETE, NULL);
    }
    if (id != BLUEWAKE_PUPPET_HOOK_DRAW)
        return 0u;
    if (g_coro_draw == NULL)
        g_coro_draw = bluewake_coro_create(draw_body);
    if (bluewake_coro_active(g_coro_draw))
        return bluewake_coro_step(g_coro_draw, cpu, SITE_DRAW, NULL);

    const bool capture = atomic_load(&g_capture);
    const bool draw = atomic_load(&g_draw);
    if (!capture && !draw)
        return 0u;
    // A new play scene: the old one's objects went with the delete hook (or
    // are lost with the scene's own memory).
    // The play scene's process id (base_process_class::mBsPcId), from the execute hook: at this site r30 is
    // g_dComIfG_gameInfo.play and r27 was reused, so no register holds the scene. 1 until that hook has run.
    const u32 scene_id = bluewake_pvp_scene_id() != 0u ? bluewake_pvp_scene_id() : 1u;
    if (scene_id != g_scene_id) {
        g_map_cursors = 0u;
        g_chart_mem = 0u;
        g_scratch = 0u;
        g_model_ids = 0;
        for (int i = 0; i < POOLS; ++i)
            if (g_pools[i].users != 0) { // heaps the scene's end did not free: the pool is made again empty (see pool_get)
                fprintf(stderr, "[puppet] a new scene with %d puppet heaps still in the %s pool: the pool is reset\n", g_pools[i].users, g_pools[i].name);
                g_pools[i].stale = true;
            }
        memset(g_guest, 0, sizeof g_guest);
        g_scene_id = scene_id;
    }
    bluewake_pvp_draw(cpu, scene_id, cpu->gpr[3]); // r3 = dMenu_flag()

    static Pose local;
    if (capture && capture_pose(cpu, &local)) {
        // The item held up in an item get is an actor found by its process id, which is a call to the game: the
        // pose is finished and sent from the draw fiber.
        if (local.proc == PROC_GET_ITEM && mem_read32(cpu, EVENT_ITEM_PARTNER) != 0xFFFFFFFFu && local.att_n < ATT_MAX) {
            g_cap_pose = local;
            g_cap_pending = true;
        } else
            publish_local(&local);
    }

    minimap_pass(cpu);
    chart_pass(cpu);
    if (!draw) {
        if (!map_alloc_wanted() && !chart_alloc_wanted() && !g_cap_pending)
            return 0u;
        g_work.count = 0; // the draw fiber only makes the minimap's cursor block
        return bluewake_coro_step(g_coro_draw, cpu, SITE_DRAW, NULL);
    }
    const u32 player = mem_read32(cpu, PLAYER);
    const u32 cl = guest_ptr(player) ? mem_read32(cpu, player + PLAYER_CL_MODEL) : 0u;
    const u32 model_data = guest_ptr(cl) ? mem_read32(cpu, cl + MODEL_DATA) : 0u;
    if (!guest_ptr(model_data))
        return 0u;
    Work* w = &g_work;
    w->count = 0;
    w->model_data = model_data;
    w->room = (s8)mem_read8(cpu, STAY_ROOM);
    const double now = now_ms();
    float proj_view[16];
    bool view_ok = false;
    {
        const u32 view = mem_read32(cpu, 0x803CAB58u); // the camera's view_class (game_state.c)
        if (guest_ptr(view)) {
            for (u32 k = 0; k < 16u; ++k)
                proj_view[k] = read_float(cpu, view + 0x1A0u + 4u * k);
            view_ok = true;
        }
    }
    lock();
    int first_remote = -1; // the lowest slot drawn (the voice sweep test)
    for (int i = 0; i < MAX_REMOTES; ++i) {
        Remote* r = &g_remote[i];
        r->drawn = false;
        if (r->id == 0)
            continue;
        Pose p;
        if (!remote_pose(r, now, &p))
            continue;
        if (first_remote < 0)
            first_remote = i;
        Item* it = &w->item[w->count++];
        it->slot = i;
        it->id = r->id;
        build_item(&p, it);
        it->lod = view_ok && !ENV_SET("BLUEWAKE_PUPPET_NOCULL") ? (uint8_t)puppet_view(proj_view, it->pos) : (uint8_t)LOD_NEAR;
        it->bdl = NULL;
        it->bdl_len = 0;
        for (int k = 0; k < EQUIP_SLOTS; ++k) {
            it->eq[k] = NULL;
            it->eq_len[k] = 0;
        }
        snprintf(it->voice, sizeof it->voice, "%s", r->voice);
        it->voice_local = r->voice_local;
        it->face_skip = r->face_skip;
        it->body_wanted = r->bdl != NULL;
        it->active_ms = r->active_ms;
        if (r->retry_custom_ms > 0.0 && now_ms() >= r->retry_custom_ms) {
            r->retry_custom_ms = 0.0;
            r->model_changed = true; // the eviction is over: asked for again
        }
        const bool stock_now = now_ms() < r->stock_until_ms;
        it->voice_proc = -1;
        it->voice_pitch = r->voice_pitch;
        if (r->last_proc >= 0 && r->last_proc != (int)p.proc && p.proc != 0xFFFFu && atomic_load(&g_sounds))
            it->voice_proc = (int)p.proc;
        if (r->last_proc != (int)p.proc && ENV_SET("BLUEWAKE_PUPPET_PROC_LOG"))
            fprintf(stderr, "[puppet] player %d proc %d -> %u\n", r->id, r->last_proc, (unsigned)p.proc);
        r->last_proc = p.proc == 0xFFFFu ? -1 : (int)p.proc;
        {   // BLUEWAKE_PUPPET_TEST_VOICE=<proc> (testing): every 180th frame each puppet starts that action, so its voice is made
            static const char* test_voice = NULL;
            static int test_voice_read, test_frames[MAX_REMOTES];
            if (!test_voice_read++)
                test_voice = getenv("BLUEWAKE_PUPPET_TEST_VOICE");
            if (test_voice != NULL && ++test_frames[i] % 180 == 0)
                it->voice_proc = atoi(test_voice);
            // BLUEWAKE_PUPPET_TEST_VOICE_SWEEP=<first>-<last>:<frames> (testing): the first puppet makes voice ids first to last, one
            // every that many frames, whatever it is doing.
            static int sweep_read, sweep_first = -1, sweep_last = -1, sweep_period = 120, sweep_next = -1, sweep_tick;
            if (!sweep_read++) {
                const char* v = getenv("BLUEWAKE_PUPPET_TEST_VOICE_SWEEP");
                if (v != NULL && sscanf(v, "%d-%d:%d", &sweep_first, &sweep_last, &sweep_period) >= 2)
                    sweep_next = sweep_first;
            }
            it->voice_force = -1;
            if (sweep_next >= 0 && sweep_next <= sweep_last && i == first_remote && ++sweep_tick % sweep_period == 0) {
                it->voice_force = sweep_next++;
                it->voice_proc = 30;
            }
        }
        it->recreate = (r->model_changed || (g_guest[i].denied && now_ms() - g_guest[i].denied_at > 5000.0)) && g_guest[i].model != 0u;
        if (g_guest[i].model == 0u || it->recreate)
            r->model_changed = false;
        if ((g_guest[i].model == 0u || it->recreate) && !g_guest[i].failed && !stock_now && r->bdl != NULL && (it->bdl = (uint8_t*)malloc(r->bdl_len)) != NULL) {
            memcpy(it->bdl, r->bdl, r->bdl_len);
            it->bdl_len = r->bdl_len;
        }
        if ((g_guest[i].model == 0u || it->recreate) && !g_guest[i].failed && !stock_now)
            for (int k = 0; k < EQUIP_SLOTS; ++k)
                if (r->eq[k] != NULL && (it->eq[k] = (uint8_t*)malloc(r->eq_len[k])) != NULL) {
                    memcpy(it->eq[k], r->eq[k], r->eq_len[k]);
                    it->eq_len[k] = r->eq_len[k];
                }
        r->drawn = true;
        bluewake_pvp_note(r->id, p.base_pos);
        r->head[0] = p.base_pos[0];
        r->head[1] = p.base_pos[1] + 150.0f;
        r->head[2] = p.base_pos[2];
        if (view_ok) {
            const float v[4] = {r->head[0], r->head[1], r->head[2], 1.0f};
            float c[4];
            for (int k = 0; k < 4; ++k)
                c[k] = proj_view[k * 4] * v[0] + proj_view[k * 4 + 1] * v[1] + proj_view[k * 4 + 2] * v[2] + proj_view[k * 4 + 3] * v[3];
            memcpy(r->clip[0], r->clip[1], sizeof r->clip[0]);
            r->clip[1][0] = c[0];
            r->clip[1][1] = c[1];
            r->clip[1][2] = c[3];
            if (r->clips < 2)
                r->clips++;
            if (r->clips == 1)
                memcpy(r->clip[0], r->clip[1], sizeof r->clip[0]);
        }
    }
    unlock();
    if (w->count == 0 && !map_alloc_wanted() && !chart_alloc_wanted() && !g_cap_pending && !bluewake_pvp_wants_fiber() && !bluewake_carry_wants_fiber())
        return 0u;
    return bluewake_coro_step(g_coro_draw, cpu, SITE_DRAW, NULL);
}

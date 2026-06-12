#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_midna.h"
#include "d/d_meter2.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter2_info.h"

void daAlink_c::handleWolfHowl() {
    if (checkWolf()) {
        if (!dusk::getSettings().game.sunsSong) {
            return;
        }

        // Check to see if Link has the ability to transform.
        if (!dComIfGs_isEventBit(dSv_event_flag_c::M_077)) {
            return;
        }

        // Ensure there is a proper pointer to the mMeterClass and mpMeterDraw structs in
        // g_meter2_info.
        const auto meterClassPtr = g_meter2_info.getMeterClass();
        if (!meterClassPtr) {
            return;
        }

        const auto meterDrawPtr = meterClassPtr->getMeterDrawPtr();
        if (!meterDrawPtr) {
            return;
        }

        // Ensure that link is not in a cutscene.
        if (checkEventRun()) {
            Z2GetAudioMgr()->seStart(Z2SE_SYS_ERROR, NULL, 0, 0, 1.0f, 1.0f, -1.0f, -1.0f, 0);
            return;
        }

        mDoCPd_c::getCpadInfo(mPlayerNo).mPressedButtonFlags = 0;

        // Ensure that the Z Button is not dimmed
        if (meterDrawPtr->getButtonZAlpha() != 1.f) {
            Z2GetAudioMgr()->seStart(Z2SE_SYS_ERROR, NULL, 0, 0, 1.0f, 1.0f, -1.0f, -1.0f, 0);
            return;
        }

        bool canHowl = false;

        if (mLinkAcch.ChkGroundHit() && !checkModeFlg(MODE_PLAYER_FLY) && !checkMagneBootsOn()) {
            if (checkMidnaRide()) {
                if ((checkWolf() &&
                     (checkModeFlg(MODE_UNK_1000) || dComIfGp_checkPlayerStatus0(mPlayerNo, 0x10))) ||
                    (!checkWolf() &&
                     (checkEventRun() || getMidnaActor()->checkMetamorphoseEnable()) &&
                     (checkModeFlg(4) || dComIfGp_checkPlayerStatus0(mPlayerNo, 0x10))))
                {
                    canHowl = true;
                }
            }
        }

        if (!canHowl) {
            Z2GetAudioMgr()->seStart(Z2SE_SYS_ERROR, NULL, 0, 0, 1.0f, 1.0f, -1.0f, -1.0f, 0);
            return;
        }

        getWolfHowlMgrP()->setCorrectCurve(9);
        procWolfHowlDemoInit();
    }
}

void daAlink_c::handleQuickTransform() {
    if (!dusk::getSettings().game.enableQuickTransform) {
        return;
    }

    // Check to see if Link has the ability to transform.
    if (!dComIfGs_isEventBit(dSv_event_flag_c::M_077)) {
        return;
    }

    // Ensure there is a proper pointer to the mMeterClass and mpMeterDraw structs in g_meter2_info.
    const auto meterClassPtr = g_meter2_info.getMeterClass();
    if (!meterClassPtr) {
        return;
    }

    const auto meterDrawPtr = meterClassPtr->getMeterDrawPtr();
    if (!meterDrawPtr) {
        return;
    }

    // Ensure that link is not in a cutscene.
    if (checkEventRun()) {
        Z2GetAudioMgr()->seStart(Z2SE_SYS_ERROR, NULL, 0, 0, 1.0f, 1.0f, -1.0f, -1.0f, 0);
        return;
    }

    mDoCPd_c::getCpadInfo(mPlayerNo).mPressedButtonFlags = 0;

    // Don't allow quick transform while in the STAR tent.
    if (checkStageName("R_SP161")) {
        Z2GetAudioMgr()->seStart(Z2SE_SYS_ERROR, NULL, 0, 0, 1.0f, 1.0f, -1.0f, -1.0f, 0);
        return;
    }

    // Ensure that the Z Button is not dimmed
    if (meterDrawPtr->getButtonZAlpha() != 1.f) {
        Z2GetAudioMgr()->seStart(Z2SE_SYS_ERROR, NULL, 0, 0, 1.0f, 1.0f, -1.0f, -1.0f, 0);
        return;
    }

    // The game will crash if trying to quick transform while holding the Ball and Chain
    if (mEquipItem == dItemNo_IRONBALL_e) {
        Z2GetAudioMgr()->seStart(Z2SE_SYS_ERROR, NULL, 0, 0, 1.0f, 1.0f, -1.0f, -1.0f, 0);
        return;
    }

    // Use the game's default checks for if the player can currently transform
    if (!m_midnaActor->checkMetamorphoseEnableBase()) {
        Z2GetAudioMgr()->seStart(Z2SE_SYS_ERROR, NULL, 0, 0, 1.0f, 1.0f, -1.0f, -1.0f, 0);
        return;
    }

    bool canTransform = false;

    if (mLinkAcch.ChkGroundHit() && !checkModeFlg(MODE_PLAYER_FLY) && !checkMagneBootsOn()) {
        if (checkMidnaRide()) {
            if ((checkWolf() &&
                 (checkModeFlg(MODE_UNK_1000) || dComIfGp_checkPlayerStatus0(mPlayerNo, 0x10))) ||
                (!checkWolf() &&
                 (checkEventRun() || getMidnaActor()->checkMetamorphoseEnable()) &&
                 (checkModeFlg(4) || dComIfGp_checkPlayerStatus0(mPlayerNo, 0x10))))
            {
                canTransform = true;
            }
        }
    }

    if (!canTransform)
    {
        Z2GetAudioMgr()->seStart(Z2SE_SYS_ERROR, NULL, 0, 0, 1.0f, 1.0f, -1.0f, -1.0f, 0);
        return;
    }

    OSReport("Running quick transform!");
    procCoMetamorphoseInit();
}

bool daAlink_c::checkAimContext() {
    switch (mProcID) {
    case PROC_SUBJECTIVITY:
    case PROC_SWIM_SUBJECTIVITY:
    case PROC_HORSE_SUBJECTIVITY:
    case PROC_CANOE_SUBJECTIVITY:
    case PROC_BOARD_SUBJECTIVITY:
    case PROC_WOLF_ROPE_SUBJECTIVITY:
    case PROC_BOW_SUBJECT:
    case PROC_BOOMERANG_SUBJECT:
    case PROC_COPY_ROD_SUBJECT:
    case PROC_HAWK_SUBJECT:
    case PROC_HOOKSHOT_SUBJECT:
    case PROC_SWIM_HOOKSHOT_SUBJECT:
    case PROC_HORSE_BOW_SUBJECT:
    case PROC_HORSE_BOOMERANG_SUBJECT:
    case PROC_HORSE_HOOKSHOT_SUBJECT:
    case PROC_CANOE_BOW_SUBJECT:
    case PROC_CANOE_BOOMERANG_SUBJECT:
    case PROC_CANOE_HOOKSHOT_SUBJECT:
    case PROC_HOOKSHOT_ROOF_WAIT:
    case PROC_HOOKSHOT_ROOF_SHOOT:
    case PROC_HOOKSHOT_WALL_WAIT:
    case PROC_HOOKSHOT_WALL_SHOOT:
        return true;
    case PROC_IRON_BALL_SUBJECT:
        return itemButton() && mItemVar0.field_0x3018 == 2;
    default:
        return false;
    }
}

void daAlink_c::reassertAnmFrames() {
    // Both player instances render the same refcounted J3DModelData, and
    // changeModelDataDirect()/changeModelDataDirectWolf() store the
    // per-instance blend tables (field_0x1f20/field_0x1f24) on its SHARED
    // joints — whoever ran it last (normally the guest, created second) owns
    // the bindings, so the other Link's skeleton gets posed from the wrong
    // player's anim packs. Re-bind this instance's tables right before this
    // instance's model calc consumes them (mirror of the binding sites in
    // d_a_alink_swindow.inc, including the status-window NULL state).
    if (!checkNoResetFlg2(FLG2_STATUS_WINDOW_DRAW) && field_0x064C != NULL) {
        if (checkWolf()) {
            field_0x064C->getJointNodePointer(0)->setMtxCalc(field_0x1f20);
            field_0x064C->getJointNodePointer(3)->setMtxCalc(field_0x1f24);
            field_0x064C->getJointNodePointer(15)->setMtxCalc(field_0x1f20);
        } else {
            field_0x064C->getJointNodePointer(0)->setMtxCalc(field_0x1f20);
            field_0x064C->getJointNodePointer(1)->setMtxCalc(field_0x1f24);
            field_0x064C->getJointNodePointer(16)->setMtxCalc(field_0x1f20);
        }
    }

    // Anim objects from demo archives (daPy_anmHeap_c::loadData with
    // mArcNo != 0xFFFF) are shared resource objects; frame state lives ON
    // them, so the other instance's allAnimePlay() stomps it. Re-assert our
    // frames from the per-instance frame ctrls. Mirrors allAnimePlay()'s
    // traversal: upper packs 0/1 alias the under packs when no separate
    // upper anim is set — their frame is owned by the under ctrl then, so
    // skip them exactly like allAnimePlay() does.
    J3DAnmTransform* under0 = getNowAnmPackUnder(UNDER_0);
    J3DAnmTransform* under1 = getNowAnmPackUnder(UNDER_1);
    J3DAnmTransform* upper0 = getNowAnmPackUpper(UPPER_0);
    J3DAnmTransform* upper1 = getNowAnmPackUpper(UPPER_1);

    for (int i = 0; i < 3; i++) {
        J3DAnmTransform* under = getNowAnmPackUnder((daAlink_UNDER)i);
        if (under != NULL) {
            under->setFrame(mUnderFrameCtrl[i].getFrame());
        }
    }

    if (upper0 != NULL && upper0 != under0) {
        upper0->setFrame(mUpperFrameCtrl[0].getFrame());
    }

    if (upper1 != NULL && upper1 != under1) {
        upper1->setFrame(mUpperFrameCtrl[1].getFrame());
    }

    J3DAnmTransform* upper2 = getNowAnmPackUpper(UPPER_2);
    if (upper2 != NULL) {
        upper2->setFrame(mUpperFrameCtrl[2].getFrame());
    }
}

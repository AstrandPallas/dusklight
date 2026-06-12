#include "dusk/frame_interpolation.h"

#include "dusk/coop.h"
#include "f_op/f_op_camera_mng.h"
#include "m_Do/m_Do_graphic.h"
#include "mtx.h"

#include <absl/container/flat_hash_map.h>

namespace {

struct Recording {
    absl::flat_hash_map<uintptr_t, Mtx> matrix_values;
};

bool s_initialized = false;

bool g_enabled = false;
bool g_recording = false;
bool g_interpolating = false;
bool g_sync_presentation = false;

float g_step = 0.0f;
bool g_is_sim_frame = false;
bool g_ui_tick_pending = false;
uint64_t g_sim_tick_seq = 0;

Recording g_current_recording;
Recording g_previous_recording;

absl::flat_hash_map<uintptr_t, Mtx> g_replacements;

struct CameraSnapshot {
    cXyz eye{};
    cXyz center{};
    cXyz up{};
    s16 bank{};
    f32 fovy{};
    f32 aspect{};
    f32 near_{};
    f32 far_{};
    bool wideZoom{};
    bool valid{};
    uint64_t recorded_seq{};
};

// One snapshot pair per camera id so split co-op windows interpolate
// independently (engine camera slots are kMaxPlayers-wide, see dComIfG's
// mCameraInfo[]).
constexpr int kNumCameraSlots = dusk::coop::kMaxPlayers;

CameraSnapshot s_cam_prev[kNumCameraSlots]{};
CameraSnapshot s_cam_curr[kNumCameraSlots]{};

struct PresentationViewRestore {
    view_class* view{};
    view_class backup{};
};

PresentationViewRestore s_presentation_view_restore[kNumCameraSlots]{};
int s_presentation_depth = 0;

struct InterpolationCallBackWork {
    dusk::frame_interp::InterpolationCallBack pCallBack;
    void* pUserWork;
};

std::vector<InterpolationCallBackWork> s_interpolationCallBackWork;

void reset_camera_slots() {
    for (int i = 0; i < kNumCameraSlots; ++i) {
        s_cam_prev[i].valid = false;
        s_cam_curr[i].valid = false;
    }
}

// A camera slot may only interpolate when it recorded on the current sim tick
// AND the tick immediately before it. Cameras that stop recording — destroyed
// on a scene change, suspended during a coop guest stash, paused, or freshly
// created without a previous tick — go stale automatically and snap to their
// sim view instead of blending from dead data. This replaces the old
// "invalidate when camera 0 is missing" special case.
bool camera_slot_fresh(int camera_id) {
    if (camera_id < 0 || camera_id >= kNumCameraSlots) {
        return false;
    }
    const CameraSnapshot& prev = s_cam_prev[camera_id];
    const CameraSnapshot& curr = s_cam_curr[camera_id];
    return prev.valid && curr.valid && curr.recorded_seq == g_sim_tick_seq &&
           prev.recorded_seq + 1 == curr.recorded_seq;
}

void copy_view_to_snap(CameraSnapshot* dst, const view_class& v) {
    dst->eye = v.lookat.eye;
    dst->center = v.lookat.center;
    dst->up = v.lookat.up;
    dst->bank = v.bank;
    dst->fovy = v.fovy;
    dst->aspect = v.aspect;
    dst->near_ = v.near_;
    dst->far_ = v.far_;
    dst->valid = true;
}

inline void lerp_matrix(Mtx out, const Mtx lhs, const Mtx rhs, float step) {
    for (size_t row = 0; row < 3; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            const float l = lhs[row][col];
            out[row][col] = l + (rhs[row][col] - l) * step;
        }
    }
}

inline void lerp_xyz(cXyz* out, const cXyz& lhs, const cXyz& rhs, float step) {
    out->x = lhs.x + (rhs.x - lhs.x) * step;
    out->y = lhs.y + (rhs.y - lhs.y) * step;
    out->z = lhs.z + (rhs.z - lhs.z) * step;
}

static s16 lerp_bank(s16 a, s16 b, f32 t) {
    const f32 ra = S2RAD(a);
    const f32 d = remainderf(S2RAD(b) - ra, 2.0f * static_cast<f32>(M_PI));
    return cAngle::Radian_to_SAngle(ra + d * t);
}

inline bool matrix_differs(const Mtx lhs, const Mtx rhs, float epsilon = 0.0001f) {
    for (size_t row = 0; row < 3; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            if (std::abs(lhs[row][col] - rhs[row][col]) > epsilon) {
                return true;
            }
        }
    }
    return false;
}

const Mtx* resolve_replacement(const Mtx* source, Mtx* scratch) {
    if (!g_interpolating || source == nullptr || dusk::frame_interp::presentation_sync_active()) {
        return source;
    }

    auto it = g_replacements.find(reinterpret_cast<uintptr_t>(source));
    if (it == g_replacements.end()) {
        return source;
    }

    MTXCopy(it->second, *scratch);
    return scratch;
}

bool has_recording_data(const Recording& recording) {
    return !recording.matrix_values.empty();
}

void clear_replacements() {
    g_replacements.clear();
}

}  // namespace

namespace dusk::frame_interp {
void ensure_initialized() {
    s_initialized = true;
}

void begin_sim_tick() {
    ensure_initialized();
    if (!g_enabled) {
        return;
    }

    s_interpolationCallBackWork.clear();
    for (int i = 0; i < kNumCameraSlots; ++i) {
        s_cam_prev[i] = s_cam_curr[i];
    }
    ++g_sim_tick_seq;
}

uint64_t sim_tick_seq() {
    return g_sim_tick_seq;
}

void begin_frame(FrameInterpMode mode, bool is_sim_frame, float step) {
    g_enabled = mode != FrameInterpMode::Off;
    g_is_sim_frame = is_sim_frame;
    g_step = std::clamp(step, 0.0f, 1.0f);
}

bool is_enabled() {
    return g_enabled;
}

bool is_sim_frame() {
    return g_is_sim_frame;
}

void begin_record() {
    ensure_initialized();

    if (!g_enabled) {
        g_interpolating = false;
        g_sync_presentation = false;
        g_previous_recording = {};
        g_current_recording = {};
        clear_replacements();
        reset_camera_slots();
        return;
    }

    g_sync_presentation = false;
    g_previous_recording = std::move(g_current_recording);
    g_current_recording = {};
    g_recording = true;
    g_interpolating = false;
    clear_replacements();
}

void end_record() {
    g_recording = false;
}

void interpolate() {
    ensure_initialized();
    clear_replacements();
    g_interpolating = g_enabled && !g_recording && !g_sync_presentation && has_recording_data(g_current_recording);
    if (!g_interpolating) {
        return;
    }
    for (auto const& old : g_previous_recording.matrix_values) {
        if (auto it = g_current_recording.matrix_values.find(old.first);
            it != g_current_recording.matrix_values.end())
        {
            lerp_matrix(g_replacements[old.first], old.second, it->second, g_step);
        }
    }
}

void request_presentation_sync() {
    ensure_initialized();
    if (!g_enabled) {
        return;
    }
    g_sync_presentation = true;
}

bool presentation_sync_active() {
    if (!s_initialized || !g_enabled) {
        return false;
    }
    return g_sync_presentation;
}

float get_interpolation_step() {
    ensure_initialized();
    return presentation_sync_active() ? 1.0f : g_step;
}

void set_ui_tick_pending(bool value) {
    if (g_ui_tick_pending == value) { return; }
    g_ui_tick_pending = value;
}

bool get_ui_tick_pending() {
    ensure_initialized();
    return g_enabled ? g_ui_tick_pending : true;
}

void record_final_mtx(Mtx m, const void* key) {
    if (!s_initialized || !g_recording || m == nullptr) {
        return;
    }

    auto& it = g_current_recording.matrix_values[reinterpret_cast<uintptr_t>(key)];
    MTXCopy(m, it);
}

void record_final_mtx(Mtx m) {
    record_final_mtx(m, m);
}

bool lookup_replacement(const void* key, Mtx out) {
    if (presentation_sync_active() || !g_interpolating || key == nullptr) {
        return false;
    }

    auto it = g_replacements.find(reinterpret_cast<uintptr_t>(key));
    if (it == g_replacements.end()) {
        return false;
    }

    MTXCopy(it->second, out);
    return true;
}

bool lookup_concat_replacement(const void* lhs, const void* rhs, Mtx out) {
    if (presentation_sync_active() || !g_interpolating || lhs == nullptr || rhs == nullptr) {
        return false;
    }

    Mtx lhs_scratch;
    Mtx rhs_scratch;
    const Mtx* resolved_lhs = resolve_replacement(reinterpret_cast<const Mtx*>(lhs), &lhs_scratch);
    const Mtx* resolved_rhs = resolve_replacement(reinterpret_cast<const Mtx*>(rhs), &rhs_scratch);
    if (resolved_lhs == reinterpret_cast<const Mtx*>(lhs) && resolved_rhs == reinterpret_cast<const Mtx*>(rhs)) {
        return false;
    }

    MTXConcat(*resolved_lhs, *resolved_rhs, out);
    return true;
}

void record_camera(::camera_process_class* cam, int camera_id) {
    if (!g_enabled || cam == nullptr || camera_id < 0 || camera_id >= kNumCameraSlots) {
        return;
    }
    CameraSnapshot& slot = s_cam_curr[camera_id];
    copy_view_to_snap(&slot, cam->view);
    slot.recorded_seq = g_sim_tick_seq;
#if WIDESCREEN_SUPPORT
    slot.wideZoom = mDoGph_gInf_c::isWideZoom();
#endif
}

void interp_view(::view_class* view, int camera_id) {
    if (!g_enabled)
        return;

    if (!camera_slot_fresh(camera_id))
        return;

    const CameraSnapshot& cam_prev = s_cam_prev[camera_id];
    const CameraSnapshot& cam_curr = s_cam_curr[camera_id];

    const f32 step = get_interpolation_step();
    const bool is_cam_curr_authoritative = g_is_sim_frame && step <= 0.0f;

    cXyz eye;
    cXyz center;
    cXyz up;
    if (is_cam_curr_authoritative) {
        eye = cam_curr.eye;
        center = cam_curr.center;
        up = cam_curr.up;
    } else {
        lerp_xyz(&eye, cam_prev.eye, cam_curr.eye, step);
        lerp_xyz(&center, cam_prev.center, cam_curr.center, step);
        lerp_xyz(&up, cam_prev.up, cam_curr.up, step);
    }
    if (!up.normalizeRS()) {
        up = cam_curr.up;
        up.normalizeRS();
    }

    view->lookat.eye = eye;
    view->lookat.center = center;
    view->lookat.up = up;
    if (is_cam_curr_authoritative) {
        view->bank = cam_curr.bank;
        view->fovy = cam_curr.fovy;
        view->aspect = cam_curr.aspect;
        view->near_ = cam_curr.near_;
        view->far_ = cam_curr.far_;
    } else {
        view->bank = lerp_bank(cam_prev.bank, cam_curr.bank, step);
        view->fovy = cam_prev.fovy + (cam_curr.fovy - cam_prev.fovy) * step;
        view->aspect = cam_prev.aspect + (cam_curr.aspect - cam_prev.aspect) * step;
        view->near_ = cam_prev.near_ + (cam_curr.near_ - cam_prev.near_) * step;
        view->far_ = cam_prev.far_ + (cam_curr.far_ - cam_prev.far_) * step;
    }

    // FRAME INTERP TODO: It might be better if I rewired the game to not clear this flag until the
    // next sim frame, but I don't care enough to right now
#if WIDESCREEN_SUPPORT
    // The wide-zoom flag is global state owned by the primary view; secondary
    // (split co-op) cameras never drive it — widezoom is disabled during split.
    if (camera_id == 0) {
        const f32 wide_step = is_cam_curr_authoritative ? 1.0f : step;
        if (mDoGph_gInf_c::isWide() && !mDoGph_gInf_c::isWideZoom() && wide_step >= 0.5f ? cam_curr.wideZoom : cam_prev.wideZoom) {
            mDoGph_gInf_c::onWideZoom();
        }
    }
#endif
}

static void run_interpolation_callbacks() {
    for (size_t i = 0; i < s_interpolationCallBackWork.size(); i++) {
        auto const& work = s_interpolationCallBackWork[i];
        work.pCallBack(g_is_sim_frame, work.pUserWork);
    }
}

void add_interpolation_callback(InterpolationCallBack pCallBack, void* pUserWork) {
    if (!is_enabled() || s_presentation_depth > 0 || !g_is_sim_frame)
        return;

    s_interpolationCallBackWork.emplace_back(pCallBack, pUserWork);
}

void begin_presentation_camera() {
    ensure_initialized();
    if (!g_enabled) {
        return;
    }
    if (s_presentation_depth > 0) {
        s_presentation_depth++;
        return;
    }
    // Camera 0 drives the global presentation state (audio listener, cull
    // frustum, j3dSys view); without fresh data for it, skip the pass entirely.
    if (!camera_slot_fresh(0)) {
        return;
    }

    view_class* const view = dComIfGd_getView();
    if (view == nullptr) {
        return;
    }

    std::memcpy(&s_presentation_view_restore[0].backup, view, sizeof(view_class));
    s_presentation_view_restore[0].view = view;
    interp_view(view, 0);

    // FRAME INTERP TODO: Largely copied from d_camera's camera_draw function from this point, got any better ideas?
    C_MTXPerspective(view->projMtx, view->fovy, view->aspect, view->near_, view->far_);
    mDoMtx_lookAt(view->viewMtx, &view->lookat.eye, &view->lookat.center, &view->lookat.up, view->bank);
#if WIDESCREEN_SUPPORT
    mDoGph_gInf_c::setWideZoomProjection(view->projMtx);
#endif
    j3dSys.setViewMtx(view->viewMtx);
    cMtx_inverse(view->viewMtx, view->invViewMtx);

    bool camera_attention_status = dComIfGp_getCameraAttentionStatus(0) & 0x80;
    Z2GetAudience()->setAudioCamera(view->viewMtx, view->lookat.eye, view->lookat.center, view->fovy, view->aspect, camera_attention_status, 0, false);

    dBgS_GndChk gndchk;
    gndchk.OnWaterGrp();
    gndchk.SetPos(&view->lookat.eye);
    f32 cross = dComIfG_Bgsp().GroundCross(&gndchk);
    if (cross != -G_CM3D_F_INF) {
        if (dComIfG_Bgsp().ChkGrpInf(gndchk, 0x100)) {
            mDoAud_getCameraMapInfo(6);
        } else {
            mDoAud_getCameraMapInfo(dComIfG_Bgsp().GetMtrlSndId(gndchk));
        }
        mDoAud_setCameraGroupInfo(dComIfG_Bgsp().GetGrpSoundId(gndchk));
        Vec spDC;
        spDC.x = view->lookat.eye.x;
        spDC.y = cross;
        spDC.z = view->lookat.eye.z;
        Z2AudioMgr::getInterface()->setCameraPolygonPos(&spDC);
    } else {
        Z2AudioMgr::getInterface()->setCameraPolygonPos(nullptr);
    }

    MTXCopy(view->viewMtx, view->viewMtxNoTrans);
    view->viewMtxNoTrans[0][3] = 0.0f;
    view->viewMtxNoTrans[1][3] = 0.0f;
    view->viewMtxNoTrans[2][3] = 0.0f;
    cMtx_concatProjView(view->projMtx, view->viewMtx, view->projViewMtx);

    f32 far_;
    f32 var_f30;
    if (dComIfGp_getCameraAttentionStatus(0) & 8) {
        far_ = view->far_;
    } else {
#if DEBUG
        if (g_envHIO.mOther.mAdjustCullFar != 0) {
            var_f30 = g_envHIO.mOther.mCullFarValue;
        } else
#endif
        {
            var_f30 = dStage_stagInfo_GetCullPoint(dComIfGp_getStageStagInfo());
        }
        far_ = var_f30;
    }

    mDoLib_clipper::setup(view->fovy, view->aspect, view->near_, far_);

    // FRAME INTERP NOTE: Removed the call to offWideZoom that was here, it causes problems with presentation during cutscenes.

    // coop: refresh every other live camera the same way, so each split window
    // presents its own interpolated view — the painter's per-window pass reads
    // camera_p->view.{viewMtx,projMtx,fovy,aspect,lookat} directly. The
    // camera-0-pinned globals above (audio listener, cull frustum, j3dSys view)
    // are intentionally not touched here, mirroring view_setup/camera_draw.
    for (int id = 1; id < kNumCameraSlots; ++id) {
        if (!camera_slot_fresh(id)) {
            continue;
        }
        ::camera_process_class* cam = dComIfGp_getCamera(id);
        if (cam == nullptr) {
            continue;
        }
        view_class* const cam_view = &cam->view;
        std::memcpy(&s_presentation_view_restore[id].backup, cam_view, sizeof(view_class));
        s_presentation_view_restore[id].view = cam_view;

        interp_view(cam_view, id);
        C_MTXPerspective(cam_view->projMtx, cam_view->fovy, cam_view->aspect, cam_view->near_,
                         cam_view->far_);
        mDoMtx_lookAt(cam_view->viewMtx, &cam_view->lookat.eye, &cam_view->lookat.center,
                      &cam_view->lookat.up, cam_view->bank);
#if WIDESCREEN_SUPPORT
        mDoGph_gInf_c::setWideZoomProjection(cam_view->projMtx);
#endif
        cMtx_inverse(cam_view->viewMtx, cam_view->invViewMtx);
        MTXCopy(cam_view->viewMtx, cam_view->viewMtxNoTrans);
        cam_view->viewMtxNoTrans[0][3] = 0.0f;
        cam_view->viewMtxNoTrans[1][3] = 0.0f;
        cam_view->viewMtxNoTrans[2][3] = 0.0f;
        cMtx_concatProjView(cam_view->projMtx, cam_view->viewMtx, cam_view->projViewMtx);
    }

    s_presentation_depth = 1;

    run_interpolation_callbacks();
}

void end_presentation_camera() {
    if (s_presentation_depth == 0) {
        return;
    }
    s_presentation_depth--;
    if (s_presentation_depth > 0) {
        return;
    }

    for (PresentationViewRestore& restore : s_presentation_view_restore) {
        if (restore.view == nullptr) {
            continue;
        }
        std::memcpy(restore.view, &restore.backup, sizeof(view_class));
        restore.view = nullptr;
    }
}
}  // namespace dusk::frame_interp

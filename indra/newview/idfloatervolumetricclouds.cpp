/**
 * @file idfloatervolumetricclouds.cpp
 * @brief <ID> Volumetric cloud editor floater, its altitude column and wind dial.
 *
 * Part of Five's custom Firestorm fork.
 */

#include "llviewerprecompiledheaders.h"

#include "idfloatervolumetricclouds.h"

#include "idvolumetricclouds.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfilepicker.h"
#include "llfloaterpreference.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"
#include "llwindow.h"

#include <algorithm>
#include <cmath>

static LLDefaultChildRegistry::Register<IDCloudAltitudeColumn> r_id_cloud_altitude_column("id_cloud_altitude_column");
static LLDefaultChildRegistry::Register<IDCloudCompassDial> r_id_cloud_compass_dial("id_cloud_compass_dial");

// The Experimental preferences panel, so its cloud toggles grey out when the clouds cannot be changed.
class IDPanelPreferenceExperimental : public LLPanelPreference
{
public:
    bool postBuild() override
    {
        mCloudControls = { findChild<LLUICtrl>("volumetric_clouds_enabled"),
                           findChild<LLUICtrl>("volumetric_clouds_full_quality") };
        return LLPanelPreference::postBuild();
    }

    void draw() override
    {
        const bool can_edit = IDVolumetricClouds::canEdit();
        for (LLUICtrl* ctrl : mCloudControls)
        {
            if (ctrl)
            {
                ctrl->setEnabled(can_edit);
            }
        }
        LLPanelPreference::draw();
    }

private:
    std::array<LLUICtrl*, 2> mCloudControls{};
};

static LLPanelInjector<IDPanelPreferenceExperimental> t_id_panel_preference_experimental("id_panel_preference_experimental");

namespace
{
    const char* SETTING_PRESET = "IDVolumetricCloudsPreset";

    const S32 PAD_PX = 8;
    const S32 LABEL_W = 36;
    const S32 EDGE_PX = 4;
    const F32 SNAP_M = 10.f;

    const LLColor4 COLUMN_BG(0.10f, 0.14f, 0.22f, 1.f);
    const LLColor4 COLUMN_GRID(1.f, 1.f, 1.f, 0.12f);
    const LLColor4 COLUMN_LABEL(1.f, 1.f, 1.f, 0.6f);
    const LLColor4 BAND_FILL(0.85f, 0.88f, 0.92f, 0.45f);
    const LLColor4 BAND_SELECTED(1.f, 1.f, 1.f, 0.8f);
    const LLColor4 BAND_OUTLINE(1.f, 0.85f, 0.3f, 1.f);
    const LLColor4 BAND_TEXT(0.08f, 0.1f, 0.15f, 1.f);
    const LLColor4 DIAL_BG(0.10f, 0.14f, 0.22f, 1.f);
    const LLColor4 DIAL_RIM(1.f, 1.f, 1.f, 0.5f);
    const LLColor4 DIAL_ARROW(1.f, 0.85f, 0.3f, 1.f);

    const char* const TYPE_BUTTONS[] = { "type_cumulus", "type_cumulonimbus", "type_stratus",
                                         "type_stratocumulus", "type_altocumulus", "type_cirrus" };
    static_assert(LL_ARRAY_SIZE(TYPE_BUTTONS) == static_cast<size_t>(IDCloudType::Count));

    LLColor4 dimmed(const LLColor4& c, bool enabled)
    {
        return enabled ? c : LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * 0.4f);
    }
}

// ---------------------------------------------------------------------------
// IDCloudAltitudeColumn
// ---------------------------------------------------------------------------

IDCloudAltitudeColumn::IDCloudAltitudeColumn(const Params& p)
    : LLUICtrl(p)
{
}

void IDCloudAltitudeColumn::setLayers(const std::vector<IDCloudLayer>& layers)
{
    mLayers = layers;
    setSelected(mSelected);
}

void IDCloudAltitudeColumn::setSelected(S32 index)
{
    mSelected = mLayers.empty() ? -1 : llclamp(index, 0, static_cast<S32>(mLayers.size()) - 1);
}

S32 IDCloudAltitudeColumn::altToY(F32 alt_m) const
{
    const S32 span = std::max(1, getRect().getHeight() - 2 * PAD_PX);
    return PAD_PX + ll_round(llclamp(alt_m, 0.f, ALT_MAX_M) / ALT_MAX_M * span);
}

F32 IDCloudAltitudeColumn::metersPerPixel() const
{
    return ALT_MAX_M / std::max(1, getRect().getHeight() - 2 * PAD_PX);
}

S32 IDCloudAltitudeColumn::hitTest(S32 y, Drag& mode) const
{
    const S32 count = static_cast<S32>(mLayers.size());
    for (S32 n = -1; n < count; ++n)
    {
        // The selected band first, then the others from the top of the draw order down.
        const S32 i = n < 0 ? mSelected : count - 1 - n;
        if (i < 0 || i >= count || (n >= 0 && i == mSelected))
        {
            continue;
        }
        const S32 y0 = altToY(mLayers[i].mBaseM);
        const S32 y1 = altToY(mLayers[i].mTopM);
        if (y < y0 - EDGE_PX || y > y1 + EDGE_PX)
        {
            continue;
        }
        const bool near_top = std::abs(y - y1) <= EDGE_PX;
        const bool near_base = std::abs(y - y0) <= EDGE_PX;
        if (near_top && near_base)
        {
            mode = y >= (y0 + y1) / 2 ? Drag::Top : Drag::Base;
        }
        else if (near_top)
        {
            mode = Drag::Top;
        }
        else if (near_base)
        {
            mode = Drag::Base;
        }
        else
        {
            mode = Drag::Move;
        }
        return i;
    }
    mode = Drag::None;
    return -1;
}

void IDCloudAltitudeColumn::draw()
{
    const bool enabled = getEnabled();
    const S32 w = getRect().getWidth();
    const S32 h = getRect().getHeight();
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();

    gl_rect_2d(0, h, w, 0, dimmed(COLUMN_BG, enabled), true);

    for (S32 km = 0; km <= static_cast<S32>(ALT_MAX_M / 1000.f); ++km)
    {
        const S32 y = altToY(km * 1000.f);
        gl_line_2d(LABEL_W, y, w, y, dimmed(COLUMN_GRID, enabled));
        if (km % 2 == 0)
        {
            font->renderUTF8(llformat("%d km", km), 0, LABEL_W - 4, y, dimmed(COLUMN_LABEL, enabled),
                             LLFontGL::RIGHT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        }
    }

    const S32 left = LABEL_W + 2;
    const S32 right = w - 3;
    const S32 count = static_cast<S32>(mLayers.size());
    for (S32 n = 0; n <= count; ++n)
    {
        // Draw the selected band last so it sits on top.
        const S32 i = n < count ? n : mSelected;
        if (i < 0 || i >= count || (n < count && i == mSelected))
        {
            continue;
        }
        const IDCloudLayer& l = mLayers[i];
        const S32 y0 = altToY(l.mBaseM);
        const S32 y1 = std::max(altToY(l.mTopM), y0 + 3);
        const bool selected = i == mSelected;

        gl_rect_2d(left, y1, right, y0, dimmed(selected ? BAND_SELECTED : BAND_FILL, enabled), true);
        if (selected)
        {
            gl_rect_2d(left, y1, right, y0, dimmed(BAND_OUTLINE, enabled), false);
        }
        if (y1 - y0 >= font->getLineHeight())
        {
            font->renderUTF8(idCloudTypeName(l.mType), 0, (left + right) / 2, (y0 + y1) / 2, dimmed(BAND_TEXT, enabled),
                             LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        }
    }

    LLUICtrl::draw();
}

bool IDCloudAltitudeColumn::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!getEnabled())
    {
        return false;
    }

    Drag mode = Drag::None;
    const S32 hit = hitTest(y, mode);
    if (hit < 0)
    {
        return true;
    }

    mSelected = hit;
    mDrag = mode;
    mDragStartY = y;
    mStartBaseM = mLayers[hit].mBaseM;
    mStartTopM = mLayers[hit].mTopM;
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool IDCloudAltitudeColumn::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && !getEnabled())
    {
        gFocusMgr.setMouseCapture(nullptr);
    }

    Drag mode = mDrag;
    if (hasMouseCapture() && mDrag != Drag::None && mSelected >= 0 && mSelected < static_cast<S32>(mLayers.size()))
    {
        const F32 delta = std::round((y - mDragStartY) * metersPerPixel() / SNAP_M) * SNAP_M;
        IDCloudLayer& l = mLayers[mSelected];
        const F32 old_base = l.mBaseM;
        const F32 old_top = l.mTopM;
        switch (mDrag)
        {
            case Drag::Move:
            {
                const F32 d = llclamp(delta, -mStartBaseM, std::max(0.f, ALT_MAX_M - mStartTopM));
                l.mBaseM = mStartBaseM + d;
                l.mTopM = mStartTopM + d;
                break;
            }
            case Drag::Top:
                l.mTopM = llclamp(mStartTopM + delta, mStartBaseM + MIN_THICKNESS_M,
                                  std::max(ALT_MAX_M, mStartBaseM + MIN_THICKNESS_M));
                break;
            case Drag::Base:
                l.mBaseM = llclamp(mStartBaseM + delta, 0.f, mStartTopM - MIN_THICKNESS_M);
                break;
            default:
                break;
        }
        if (l.mBaseM != old_base || l.mTopM != old_top)
        {
            onCommit();
        }
    }
    else if (getEnabled())
    {
        hitTest(y, mode);
    }

    getWindow()->setCursor(mode == Drag::Top || mode == Drag::Base ? UI_CURSOR_SIZENS : UI_CURSOR_ARROW);
    return true;
}

bool IDCloudAltitudeColumn::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture())
    {
        return false;
    }
    gFocusMgr.setMouseCapture(nullptr);
    mDrag = Drag::None;
    onCommit();
    return true;
}

void IDCloudAltitudeColumn::onMouseCaptureLost()
{
    mDrag = Drag::None;
}

// ---------------------------------------------------------------------------
// IDCloudCompassDial
// ---------------------------------------------------------------------------

IDCloudCompassDial::IDCloudCompassDial(const Params& p)
    : LLUICtrl(p)
{
}

void IDCloudCompassDial::setValue(const LLSD& value)
{
    F32 deg = std::fmod(static_cast<F32>(value.asReal()), 360.f);
    if (!std::isfinite(deg))
    {
        deg = 0.f;
    }
    mDegrees = deg < 0.f ? deg + 360.f : deg;
}

bool IDCloudCompassDial::setFromPoint(S32 x, S32 y)
{
    const F32 dx = x - getRect().getWidth() * 0.5f;
    const F32 dy = y - getRect().getHeight() * 0.5f;
    if (dx == 0.f && dy == 0.f)
    {
        return false;
    }
    F32 deg = std::round(std::atan2(dx, dy) * RAD_TO_DEG);
    if (deg < 0.f)
    {
        deg += 360.f;
    }
    if (deg >= 360.f)
    {
        deg = 0.f;
    }
    if (deg == mDegrees)
    {
        return false;
    }
    mDegrees = deg;
    return true;
}

void IDCloudCompassDial::draw()
{
    const bool enabled = getEnabled();
    const F32 cx = getRect().getWidth() * 0.5f;
    const F32 cy = getRect().getHeight() * 0.5f;
    const F32 r = std::min(cx, cy) - 2.f;

    gGL.color4fv(dimmed(DIAL_BG, enabled).mV);
    gl_circle_2d(cx, cy, r, 48, true);
    gGL.color4fv(dimmed(DIAL_RIM, enabled).mV);
    gl_circle_2d(cx, cy, r, 48, false);

    for (S32 i = 0; i < 4; ++i)
    {
        const F32 a = i * F_PI_BY_TWO;
        gl_line_2d(ll_round(cx + std::sin(a) * r * 0.8f), ll_round(cy + std::cos(a) * r * 0.8f),
                   ll_round(cx + std::sin(a) * r), ll_round(cy + std::cos(a) * r), dimmed(DIAL_RIM, enabled));
    }

    const F32 rad = mDegrees * DEG_TO_RAD;
    const F32 dx = std::sin(rad);
    const F32 dy = std::cos(rad);
    const F32 head = r * 0.3f;
    const F32 tip_x = cx + dx * r * 0.85f;
    const F32 tip_y = cy + dy * r * 0.85f;
    const F32 base_x = tip_x - dx * head;
    const F32 base_y = tip_y - dy * head;
    const LLColor4 arrow = dimmed(DIAL_ARROW, enabled);

    gl_line_2d(ll_round(cx - dx * r * 0.6f), ll_round(cy - dy * r * 0.6f), ll_round(base_x), ll_round(base_y), arrow);
    gl_triangle_2d(ll_round(tip_x), ll_round(tip_y),
                   ll_round(base_x + dy * head * 0.5f), ll_round(base_y - dx * head * 0.5f),
                   ll_round(base_x - dy * head * 0.5f), ll_round(base_y + dx * head * 0.5f), arrow, true);

    LLUICtrl::draw();
}

bool IDCloudCompassDial::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!getEnabled())
    {
        return false;
    }
    gFocusMgr.setMouseCapture(this);
    if (setFromPoint(x, y))
    {
        onCommit();
    }
    return true;
}

bool IDCloudCompassDial::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && !getEnabled())
    {
        gFocusMgr.setMouseCapture(nullptr);
    }
    else if (hasMouseCapture() && setFromPoint(x, y))
    {
        onCommit();
    }
    getWindow()->setCursor(UI_CURSOR_ARROW);
    return true;
}

bool IDCloudCompassDial::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture())
    {
        return false;
    }
    gFocusMgr.setMouseCapture(nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// IDFloaterVolumetricClouds
// ---------------------------------------------------------------------------

IDFloaterVolumetricClouds::IDFloaterVolumetricClouds(const LLSD& key)
    : LLFloater(key)
{
}

bool IDFloaterVolumetricClouds::postBuild()
{
    mPresetCombo = getChild<LLComboBox>("preset_combo");
    mSaveBtn = getChild<LLButton>("save_btn");
    mImportBtn = getChild<LLButton>("import_btn");
    mExportBtn = getChild<LLButton>("export_btn");
    mEnabledCheck = getChild<LLCheckBoxCtrl>("enabled_check");
    mFullQualityCheck = getChild<LLCheckBoxCtrl>("full_quality_check");
    mReasonText = getChild<LLTextBox>("reason_text");
    mColumn = getChild<IDCloudAltitudeColumn>("altitude_column");
    mAddBtn = getChild<LLButton>("add_layer_btn");
    mDeleteBtn = getChild<LLButton>("delete_layer_btn");
    mBaseSlider = getChild<LLSliderCtrl>("base_slider");
    mTopSlider = getChild<LLSliderCtrl>("top_slider");
    mCoverageSlider = getChild<LLSliderCtrl>("coverage_slider");
    mDensitySlider = getChild<LLSliderCtrl>("density_slider");
    mDetailSlider = getChild<LLSliderCtrl>("detail_slider");
    mWindDial = getChild<IDCloudCompassDial>("wind_dial");
    mWindSpeedSlider = getChild<LLSliderCtrl>("wind_speed_slider");

    mPresetCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetPicked(); });
    mSaveBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSave(); });
    mImportBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onImport(); });
    mExportBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onExport(); });
    mColumn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onColumnCommit(); });
    mAddBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddLayer(); });
    mDeleteBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDeleteLayer(); });

    for (size_t i = 0; i < mTypeBtns.size(); ++i)
    {
        const IDCloudType type = static_cast<IDCloudType>(i);
        mTypeBtns[i] = getChild<LLButton>(TYPE_BUTTONS[i]);
        mTypeBtns[i]->setCommitCallback([this, type](LLUICtrl*, const LLSD&) { onTypePicked(type); });
    }

    mBaseSlider->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const F32 v = mBaseSlider->getValueF32();
        editSelected([v](IDCloudLayer& l)
        {
            l.mBaseM = llclamp(v, 0.f, std::max(0.f, l.mTopM - IDCloudAltitudeColumn::MIN_THICKNESS_M));
        });
    });
    mTopSlider->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const F32 v = mTopSlider->getValueF32();
        editSelected([v](IDCloudLayer& l) { l.mTopM = std::max(v, l.mBaseM + IDCloudAltitudeColumn::MIN_THICKNESS_M); });
    });
    mCoverageSlider->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const F32 v = llclamp(mCoverageSlider->getValueF32(), 0.f, 1.f);
        editSelected([v](IDCloudLayer& l) { l.mCoverage = v; });
    });
    mDensitySlider->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const F32 v = llclamp(mDensitySlider->getValueF32(), 0.f, 1.f);
        editSelected([v](IDCloudLayer& l) { l.mDensity = v; });
    });
    mDetailSlider->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const F32 v = llclamp(mDetailSlider->getValueF32(), 0.f, 1.f);
        editSelected([v](IDCloudLayer& l) { l.mDetail = v; });
    });
    mWindDial->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const F32 v = static_cast<F32>(mWindDial->getValue().asReal());
        editSelected([v](IDCloudLayer& l) { l.mWindDirDeg = v; });
    });
    mWindSpeedSlider->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const F32 v = llclamp(mWindSpeedSlider->getValueF32(), 0.f, 60.f);
        editSelected([v](IDCloudLayer& l) { l.mWindSpeedMS = v; });
    });

    mStateConnection = IDCloudState::instance().mChanged.connect([this]() { refreshFromState(); });

    refreshPresets();
    refreshFromState();
    updateAvailability();
    return true;
}

void IDFloaterVolumetricClouds::onOpen(const LLSD& key)
{
    refreshPresets();
    refreshFromState();
    updateAvailability();
}

void IDFloaterVolumetricClouds::draw()
{
    updateAvailability();
    LLFloater::draw();
}

void IDFloaterVolumetricClouds::updateAvailability()
{
    const Blocked blocked = IDVolumetricClouds::canEdit() ? Blocked::None
                          : !IDVolumetricClouds::isSupported() ? Blocked::Unsupported
                          : Blocked::Rlv;
    if (blocked == mBlocked)
    {
        return;
    }
    mBlocked = blocked;

    if (blocked == Blocked::Unsupported)
    {
        mReasonText->setText(getString("reason_unsupported"));
    }
    else if (blocked == Blocked::Rlv)
    {
        mReasonText->setText(getString("reason_rlv"));
    }
    mReasonText->setVisible(blocked != Blocked::None);
    updateEnabled();
}

void IDFloaterVolumetricClouds::updateEnabled()
{
    const bool ok = mBlocked == Blocked::None;
    const S32 count = static_cast<S32>(IDCloudState::instance().current().mLayers.size());
    const S32 sel = mColumn->getSelected();
    const bool has_layer = ok && sel >= 0 && sel < count;

    mPresetCombo->setEnabled(ok);
    mSaveBtn->setEnabled(ok);
    mImportBtn->setEnabled(ok);
    mExportBtn->setEnabled(ok);
    mEnabledCheck->setEnabled(ok);
    mFullQualityCheck->setEnabled(ok);
    mColumn->setEnabled(ok);
    mAddBtn->setEnabled(ok && count < static_cast<S32>(IDCloudPreset::MAX_LAYERS));
    mDeleteBtn->setEnabled(ok && count > 1);

    for (LLButton* btn : mTypeBtns)
    {
        btn->setEnabled(has_layer);
    }
    mBaseSlider->setEnabled(has_layer);
    mTopSlider->setEnabled(has_layer);
    mCoverageSlider->setEnabled(has_layer);
    mDensitySlider->setEnabled(has_layer);
    mDetailSlider->setEnabled(has_layer);
    mWindDial->setEnabled(has_layer);
    mWindSpeedSlider->setEnabled(has_layer);
}

void IDFloaterVolumetricClouds::refreshFromState()
{
    const IDCloudPreset& p = IDCloudState::instance().current();
    mColumn->setLayers(p.mLayers);

    const S32 sel = mColumn->getSelected();
    const bool has_layer = sel >= 0 && sel < static_cast<S32>(p.mLayers.size());
    for (size_t i = 0; i < mTypeBtns.size(); ++i)
    {
        mTypeBtns[i]->setToggleState(has_layer && p.mLayers[sel].mType == static_cast<IDCloudType>(i));
    }

    if (has_layer)
    {
        const IDCloudLayer& l = p.mLayers[sel];
        mBaseSlider->setValue(l.mBaseM);
        mTopSlider->setValue(l.mTopM);
        mCoverageSlider->setValue(l.mCoverage);
        mDensitySlider->setValue(l.mDensity);
        mDetailSlider->setValue(l.mDetail);
        mWindDial->setValue(LLSD(l.mWindDirDeg));
        mWindSpeedSlider->setValue(l.mWindSpeedMS);
    }

    updateEnabled();
}

void IDFloaterVolumetricClouds::refreshPresets()
{
    mPresetCombo->removeall();
    for (const std::string& name : IDCloudPresets::list())
    {
        mPresetCombo->add(name);
    }

    const std::string current = gSavedSettings.getString(SETTING_PRESET);
    if (current.empty() || !mPresetCombo->setSimple(current))
    {
        mPresetCombo->clear();
    }
}

void IDFloaterVolumetricClouds::editSelected(const std::function<void(IDCloudLayer&)>& edit)
{
    IDCloudPreset p = IDCloudState::instance().current();
    const S32 sel = mColumn->getSelected();
    if (sel < 0 || sel >= static_cast<S32>(p.mLayers.size()))
    {
        return;
    }
    edit(p.mLayers[sel]);
    markModified();
    IDCloudState::instance().setCurrent(p);
}

void IDFloaterVolumetricClouds::markModified()
{
    gSavedSettings.setString(SETTING_PRESET, std::string());
    mPresetCombo->clear();
}

void IDFloaterVolumetricClouds::showFailure(const std::string& reason_string)
{
    LLSD args;
    args["REASON"] = getString(reason_string);
    LLNotificationsUtil::add("IDCloudPresetFailed", args);
}

void IDFloaterVolumetricClouds::onPresetPicked()
{
    const std::string name = mPresetCombo->getSimple();
    if (name.empty())
    {
        return;
    }

    IDCloudPreset p;
    if (!IDCloudPresets::load(name, p))
    {
        showFailure("load_failed");
        refreshPresets();
        return;
    }
    gSavedSettings.setString(SETTING_PRESET, name);
    IDCloudState::instance().setCurrent(p);
}

void IDFloaterVolumetricClouds::onSave()
{
    LLSD args;
    args["NAME"] = gSavedSettings.getString(SETTING_PRESET);
    LLHandle<IDFloaterVolumetricClouds> handle = getDerivedHandle<IDFloaterVolumetricClouds>();
    LLNotificationsUtil::add("IDCloudPresetSaveAs", args, LLSD(),
                             [handle](const LLSD& notification, const LLSD& response)
                             {
                                 if (IDFloaterVolumetricClouds* self = handle.get())
                                 {
                                     self->onSaveName(notification, response);
                                 }
                             });
}

void IDFloaterVolumetricClouds::onSaveName(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
    {
        return;
    }

    std::string name = response["message"].asString();
    LLStringUtil::trim(name);
    if (name.empty())
    {
        return;
    }

    if (!IDCloudPresets::save(name, IDCloudState::instance().current()))
    {
        showFailure("save_failed");
        return;
    }
    gSavedSettings.setString(SETTING_PRESET, name);
    refreshPresets();
}

void IDFloaterVolumetricClouds::onImport()
{
    LLHandle<IDFloaterVolumetricClouds> handle = getDerivedHandle<IDFloaterVolumetricClouds>();
    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (IDFloaterVolumetricClouds* self = handle.get())
            {
                self->onImportPicked(files);
            }
        },
        LLFilePicker::FFLOAD_XML, false);
}

void IDFloaterVolumetricClouds::onImportPicked(const std::vector<std::string>& files)
{
    if (files.empty())
    {
        return;
    }

    std::string name;
    IDCloudPreset p;
    if (!IDCloudPresets::importFile(files.front(), name) || !IDCloudPresets::load(name, p))
    {
        showFailure("import_failed");
        return;
    }
    gSavedSettings.setString(SETTING_PRESET, name);
    refreshPresets();
    IDCloudState::instance().setCurrent(p);
}

void IDFloaterVolumetricClouds::onExport()
{
    std::string proposed = gSavedSettings.getString(SETTING_PRESET);
    if (proposed.empty())
    {
        proposed = "clouds";
    }

    LLHandle<IDFloaterVolumetricClouds> handle = getDerivedHandle<IDFloaterVolumetricClouds>();
    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (IDFloaterVolumetricClouds* self = handle.get())
            {
                self->onExportPicked(files);
            }
        },
        LLFilePicker::FFSAVE_XML, proposed + ".xml");
}

void IDFloaterVolumetricClouds::onExportPicked(const std::vector<std::string>& files)
{
    if (!files.empty() && !IDCloudPresets::exportPreset(IDCloudState::instance().current(), files.front()))
    {
        showFailure("export_failed");
    }
}

void IDFloaterVolumetricClouds::onColumnCommit()
{
    IDCloudPreset p = IDCloudState::instance().current();
    const std::vector<IDCloudLayer>& layers = mColumn->getLayers();

    bool moved = false;
    if (layers.size() == p.mLayers.size())
    {
        for (size_t i = 0; i < layers.size(); ++i)
        {
            if (layers[i].mBaseM != p.mLayers[i].mBaseM || layers[i].mTopM != p.mLayers[i].mTopM)
            {
                p.mLayers[i].mBaseM = layers[i].mBaseM;
                p.mLayers[i].mTopM = layers[i].mTopM;
                moved = true;
            }
        }
    }

    if (moved)
    {
        markModified();
        IDCloudState::instance().setCurrent(p);
    }
    else
    {
        refreshFromState();
    }
}

void IDFloaterVolumetricClouds::onAddLayer()
{
    IDCloudPreset p = IDCloudState::instance().current();
    if (p.mLayers.size() >= IDCloudPreset::MAX_LAYERS)
    {
        return;
    }

    static const IDCloudType order[] = { IDCloudType::Cumulus, IDCloudType::Altocumulus, IDCloudType::Cirrus,
                                         IDCloudType::Stratus, IDCloudType::Stratocumulus, IDCloudType::Cumulonimbus };
    IDCloudLayer layer = idCloudTypeDefaults(IDCloudType::Cumulus);
    for (IDCloudType type : order)
    {
        const IDCloudLayer candidate = idCloudTypeDefaults(type);
        const bool overlaps = std::any_of(p.mLayers.begin(), p.mLayers.end(), [&candidate](const IDCloudLayer& l)
        {
            return candidate.mBaseM < l.mTopM && l.mBaseM < candidate.mTopM;
        });
        if (!overlaps)
        {
            layer = candidate;
            break;
        }
    }

    p.mLayers.push_back(layer);
    const S32 index = static_cast<S32>(p.mLayers.size()) - 1;
    markModified();
    IDCloudState::instance().setCurrent(p);
    mColumn->setSelected(index);
    refreshFromState();
}

void IDFloaterVolumetricClouds::onDeleteLayer()
{
    const S32 count = static_cast<S32>(IDCloudState::instance().current().mLayers.size());
    const S32 sel = mColumn->getSelected();
    if (count <= 1 || sel < 0 || sel >= count)
    {
        return;
    }
    markModified();
    IDCloudState::instance().removeLayer(static_cast<U32>(sel));
}

void IDFloaterVolumetricClouds::onTypePicked(IDCloudType type)
{
    editSelected([type](IDCloudLayer& l)
    {
        IDCloudLayer d = idCloudTypeDefaults(type);
        d.mWindDirDeg = l.mWindDirDeg;
        d.mWindSpeedMS = l.mWindSpeedMS;
        l = d;
    });
}

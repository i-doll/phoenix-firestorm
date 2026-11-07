/**
 * @file idfloatervolumetricclouds.h
 * @brief <ID> Volumetric cloud editor floater, its altitude column and wind dial.
 *
 * Part of Five's custom Firestorm fork.
 */

#ifndef ID_FLOATER_VOLUMETRIC_CLOUDS_H
#define ID_FLOATER_VOLUMETRIC_CLOUDS_H

#include "llfloater.h"
#include "lluictrl.h"

#include "idvolumetriccloudsettings.h"

#include <array>
#include <functional>

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLSliderCtrl;
class LLTextBox;

class IDCloudAltitudeColumn : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params> {};

    static constexpr F32 ALT_MAX_M = 10000.f;
    static constexpr F32 MIN_THICKNESS_M = 50.f;

    void setLayers(const std::vector<IDCloudLayer>& layers);
    const std::vector<IDCloudLayer>& getLayers() const { return mLayers; }
    S32 getSelected() const { return mSelected; }
    void setSelected(S32 index);

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

protected:
    friend class LLUICtrlFactory;
    IDCloudAltitudeColumn(const Params& p);

private:
    enum class Drag { Idle, Move, Base, Top };

    S32 altToY(F32 alt_m) const;
    F32 metersPerPixel() const;
    S32 hitTest(S32 y, Drag& mode) const;

    std::vector<IDCloudLayer> mLayers;
    S32  mSelected = -1;
    Drag mDrag = Drag::Idle;
    S32  mDragStartY = 0;
    F32  mStartBaseM = 0.f;
    F32  mStartTopM = 0.f;
};

class IDCloudCompassDial : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params> {};

    void setValue(const LLSD& value) override;
    LLSD getValue() const override { return LLSD(mDegrees); }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;

protected:
    friend class LLUICtrlFactory;
    IDCloudCompassDial(const Params& p);

private:
    bool setFromPoint(S32 x, S32 y);

    F32 mDegrees = 0.f;
};

class IDFloaterVolumetricClouds : public LLFloater
{
public:
    IDFloaterVolumetricClouds(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    ~IDFloaterVolumetricClouds() override = default;

    enum class Blocked { Unknown, Available, Unsupported, Rlv };

    void refreshFromState();
    void refreshPresets();
    void updateAvailability();
    void updateEnabled();
    void editSelected(const std::function<void(IDCloudLayer&)>& edit);
    void markModified();
    void showFailure(const std::string& reason_string);

    void onPresetPicked();
    void onSave();
    void onSaveName(const LLSD& notification, const LLSD& response);
    void onImport();
    void onImportPicked(const std::vector<std::string>& files);
    void onExport();
    void onExportPicked(const std::vector<std::string>& files);
    void onColumnCommit();
    void onAddLayer();
    void onDeleteLayer();
    void onTypePicked(IDCloudType type);

    LLComboBox*            mPresetCombo = nullptr;
    LLButton*              mSaveBtn = nullptr;
    LLButton*              mImportBtn = nullptr;
    LLButton*              mExportBtn = nullptr;
    LLCheckBoxCtrl*        mEnabledCheck = nullptr;
    LLCheckBoxCtrl*        mFullQualityCheck = nullptr;
    LLTextBox*             mReasonText = nullptr;
    IDCloudAltitudeColumn* mColumn = nullptr;
    LLButton*              mAddBtn = nullptr;
    LLButton*              mDeleteBtn = nullptr;
    std::array<LLButton*, static_cast<size_t>(IDCloudType::Count)> mTypeBtns{};
    LLSliderCtrl*          mBaseSlider = nullptr;
    LLSliderCtrl*          mTopSlider = nullptr;
    LLSliderCtrl*          mCoverageSlider = nullptr;
    LLSliderCtrl*          mDensitySlider = nullptr;
    LLSliderCtrl*          mDetailSlider = nullptr;
    IDCloudCompassDial*    mWindDial = nullptr;
    LLSliderCtrl*          mWindSpeedSlider = nullptr;

    Blocked mBlocked = Blocked::Unknown;
    boost::signals2::scoped_connection mStateConnection;
};

#endif // ID_FLOATER_VOLUMETRIC_CLOUDS_H

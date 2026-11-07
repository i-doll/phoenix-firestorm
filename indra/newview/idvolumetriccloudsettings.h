/**
 * @file idvolumetriccloudsettings.h
 * @brief <ID> Volumetric cloud layer model, preset files and live cloud state.
 *
 * Part of Five's custom Firestorm fork.
 */

#ifndef ID_VOLUMETRIC_CLOUD_SETTINGS_H
#define ID_VOLUMETRIC_CLOUD_SETTINGS_H

#include "llsingleton.h"
#include "llsd.h"
#include "v2math.h"

#include <boost/signals2.hpp>
#include <string>
#include <vector>

enum class IDCloudType : U8
{
    Cumulus,
    Cumulonimbus,
    Stratus,
    Stratocumulus,
    Altocumulus,
    Cirrus,
    Count
};

struct IDCloudLayer
{
    IDCloudType mType = IDCloudType::Cumulus;
    F32 mBaseM = 1200.f;
    F32 mTopM = 2400.f;
    F32 mCoverage = 0.4f;
    F32 mDensity = 0.6f;
    F32 mDetail = 0.7f;
    F32 mWindDirDeg = 90.f;
    F32 mWindSpeedMS = 5.f;
};

struct IDCloudPreset
{
    static constexpr U32 MAX_LAYERS = 4;
    std::vector<IDCloudLayer> mLayers;
};

IDCloudLayer idCloudTypeDefaults(IDCloudType type);
const char*  idCloudTypeName(IDCloudType type);

LLSD toLLSD(const IDCloudPreset& p);
bool fromLLSD(const LLSD& sd, IDCloudPreset& out);

class IDCloudPresets
{
public:
    static std::vector<std::string> list();
    static bool load(const std::string& name, IDCloudPreset& out);
    static bool save(const std::string& name, const IDCloudPreset& p);
    static bool remove(const std::string& name);
    static bool importFile(const std::string& path, std::string& name_out);
    static bool exportFile(const std::string& name, const std::string& path);
    static bool exportPreset(const IDCloudPreset& p, const std::string& path);
};

class IDCloudState : public LLSingleton<IDCloudState>
{
    LLSINGLETON(IDCloudState);

public:
    static constexpr F32 WIND_WRAP_M = 40960.f;

    const IDCloudPreset& current() const { return mCurrent; }
    void setCurrent(const IDCloudPreset& p);
    void removeLayer(U32 index);
    void advance(F32 dt_seconds);
    LLVector2 windOffsetM(U32 layer) const;
    void freezeWind() { ++mWindFrozen; }
    void thawWind() { if (mWindFrozen > 0) --mWindFrozen; }

    boost::signals2::signal<void()> mChanged;

private:
    void initSingleton() override;

    IDCloudPreset          mCurrent;
    std::vector<LLVector2> mWind;
    U32                    mWindFrozen = 0;
};

// Holds the cloud wind still while it is in scope, so every tile of a snapshot sees the same clouds.
class IDCloudWindFreeze
{
public:
    IDCloudWindFreeze() { IDCloudState::instance().freezeWind(); }
    ~IDCloudWindFreeze() { IDCloudState::instance().thawWind(); }
    IDCloudWindFreeze(const IDCloudWindFreeze&) = delete;
    IDCloudWindFreeze& operator=(const IDCloudWindFreeze&) = delete;
};

#endif // ID_VOLUMETRIC_CLOUD_SETTINGS_H

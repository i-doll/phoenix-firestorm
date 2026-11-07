/**
 * @file idvolumetriccloudsettings.cpp
 * @brief <ID> Volumetric cloud layer model, preset files and live cloud state.
 *
 * Part of Five's custom Firestorm fork.
 */

#include "llviewerprecompiledheaders.h"

#include "idvolumetriccloudsettings.h"

#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cmath>

namespace
{
    const char* SETTING_STATE = "IDVolumetricCloudsState";
    const char* PRESET_DIR    = "volumetric_clouds";
    const S32   FORMAT_VERSION = 1;
    const S32   FORMAT_VERSION_FOG = 2;

    const char* const TYPE_NAMES[] = { "Cumulus", "Cumulonimbus", "Stratus", "Stratocumulus", "Altocumulus", "Cirrus", "Fog" };

    IDCloudPreset defaultPreset()
    {
        IDCloudPreset p;
        p.mLayers.push_back(idCloudTypeDefaults(IDCloudType::Cumulus));
        return p;
    }

    F32 clampF(F32 v, F32 lo, F32 hi)
    {
        if (!std::isfinite(v))
        {
            return lo;
        }
        return std::min(std::max(v, lo), hi);
    }

    std::string presetDir()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, PRESET_DIR);
    }

    bool validName(const std::string& name)
    {
        return !name.empty() && name.front() != '.'
            && name.find_first_of("/\\:*?\"<>|") == std::string::npos;
    }

    std::string presetPath(const std::string& name)
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, PRESET_DIR, name + ".xml");
    }

    bool ensureDir()
    {
        const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, PRESET_DIR);
        if (LLFile::isdir(dir))
        {
            return true;
        }
        LLFile::mkdir(dir);
        return LLFile::isdir(dir);
    }

    bool readFile(const std::string& path, IDCloudPreset& out)
    {
        llifstream in(path.c_str());
        if (!in.is_open())
        {
            return false;
        }
        LLSD sd;
        if (LLSDSerialize::fromXML(sd, in) == LLSDParser::PARSE_FAILURE)
        {
            return false;
        }
        return fromLLSD(sd, out);
    }

    bool writeFile(const std::string& path, const IDCloudPreset& p)
    {
        llofstream out(path.c_str());
        if (!out.is_open())
        {
            return false;
        }
        LLSDSerialize::toPrettyXML(toLLSD(p), out);
        out.flush();
        return out.good();
    }
}

IDCloudLayer idCloudTypeDefaults(IDCloudType type)
{
    IDCloudLayer l;
    l.mType = type;
    l.mWindDirDeg = 90.f;
    l.mWindSpeedMS = 5.f;
    switch (type)
    {
        case IDCloudType::Cumulus:       l.mBaseM = 1200.f; l.mTopM = 2400.f; l.mCoverage = 0.40f; l.mDensity = 0.60f; l.mDetail = 0.70f; break;
        case IDCloudType::Cumulonimbus:  l.mBaseM = 1500.f; l.mTopM = 9000.f; l.mCoverage = 0.45f; l.mDensity = 0.90f; l.mDetail = 0.60f; break;
        case IDCloudType::Stratus:       l.mBaseM = 600.f;  l.mTopM = 1100.f; l.mCoverage = 0.90f; l.mDensity = 0.40f; l.mDetail = 0.30f; break;
        case IDCloudType::Stratocumulus: l.mBaseM = 1000.f; l.mTopM = 1600.f; l.mCoverage = 0.80f; l.mDensity = 0.50f; l.mDetail = 0.50f; break;
        case IDCloudType::Altocumulus:   l.mBaseM = 3000.f; l.mTopM = 3500.f; l.mCoverage = 0.50f; l.mDensity = 0.40f; l.mDetail = 0.60f; break;
        case IDCloudType::Cirrus:        l.mBaseM = 7000.f; l.mTopM = 7500.f; l.mCoverage = 0.50f; l.mDensity = 0.15f; l.mDetail = 0.80f; break;
        case IDCloudType::Fog:           l.mBaseM = 0.f;    l.mTopM = 60.f;   l.mCoverage = 1.00f; l.mDensity = 0.50f; l.mDetail = 0.60f; l.mWindSpeedMS = 1.f; break;
        default: break;
    }
    return l;
}

F32 idCloudMinThickness(IDCloudType type)
{
    return type == IDCloudType::Fog ? 10.f : 50.f;
}

const char* idCloudTypeName(IDCloudType type)
{
    const U32 i = static_cast<U32>(type);
    return i < static_cast<U32>(IDCloudType::Count) ? TYPE_NAMES[i] : TYPE_NAMES[0];
}

LLSD toLLSD(const IDCloudPreset& p)
{
    LLSD layers = LLSD::emptyArray();
    for (const IDCloudLayer& l : p.mLayers)
    {
        LLSD e;
        e["type"] = idCloudTypeName(l.mType);
        e["base_m"] = l.mBaseM;
        e["top_m"] = l.mTopM;
        e["coverage"] = l.mCoverage;
        e["density"] = l.mDensity;
        e["detail"] = l.mDetail;
        e["wind_dir_deg"] = l.mWindDirDeg;
        e["wind_speed_ms"] = l.mWindSpeedMS;
        layers.append(e);
    }
    const bool has_fog = std::any_of(p.mLayers.begin(), p.mLayers.end(),
                                     [](const IDCloudLayer& l) { return l.mType == IDCloudType::Fog; });
    LLSD sd;
    sd["version"] = has_fog ? FORMAT_VERSION_FOG : FORMAT_VERSION;
    sd["layers"] = layers;
    return sd;
}

bool fromLLSD(const LLSD& sd, IDCloudPreset& out)
{
    if (!sd.isMap() || !sd.has("version") || !sd["layers"].isArray())
    {
        return false;
    }
    const S32 version = sd["version"].asInteger();
    if (version != FORMAT_VERSION && version != FORMAT_VERSION_FOG)
    {
        return false;
    }

    IDCloudPreset p;
    for (const LLSD& e : llsd::inArray(sd["layers"]))
    {
        if (p.mLayers.size() >= IDCloudPreset::MAX_LAYERS)
        {
            break;
        }

        IDCloudType type = IDCloudType::Cumulus;
        const std::string type_name = e["type"].asString();
        for (U32 i = 0; i < static_cast<U32>(IDCloudType::Count); ++i)
        {
            if (type_name == TYPE_NAMES[i])
            {
                type = static_cast<IDCloudType>(i);
                break;
            }
        }

        IDCloudLayer l = idCloudTypeDefaults(type);
        if (e.has("base_m"))        l.mBaseM = static_cast<F32>(e["base_m"].asReal());
        if (e.has("top_m"))         l.mTopM = static_cast<F32>(e["top_m"].asReal());
        if (e.has("coverage"))      l.mCoverage = static_cast<F32>(e["coverage"].asReal());
        if (e.has("density"))       l.mDensity = static_cast<F32>(e["density"].asReal());
        if (e.has("detail"))        l.mDetail = static_cast<F32>(e["detail"].asReal());
        if (e.has("wind_dir_deg"))  l.mWindDirDeg = static_cast<F32>(e["wind_dir_deg"].asReal());
        if (e.has("wind_speed_ms")) l.mWindSpeedMS = static_cast<F32>(e["wind_speed_ms"].asReal());

        l.mBaseM = clampF(l.mBaseM, 0.f, 12000.f);
        l.mTopM = clampF(l.mTopM, 0.f, 12000.f);
        const F32 min_thickness = idCloudMinThickness(l.mType);
        if (l.mTopM < l.mBaseM + min_thickness)
        {
            l.mTopM = l.mBaseM + min_thickness;
        }
        l.mCoverage = clampF(l.mCoverage, 0.f, 1.f);
        l.mDensity = clampF(l.mDensity, 0.f, 1.f);
        l.mDetail = clampF(l.mDetail, 0.f, 1.f);
        l.mWindSpeedMS = clampF(l.mWindSpeedMS, 0.f, 60.f);
        F32 dir = std::isfinite(l.mWindDirDeg) ? std::fmod(l.mWindDirDeg, 360.f) : 0.f;
        if (dir < 0.f)
        {
            dir += 360.f;
        }
        if (dir >= 360.f)
        {
            dir = 0.f;
        }
        l.mWindDirDeg = dir;

        p.mLayers.push_back(l);
    }

    out = std::move(p);
    return true;
}

std::vector<std::string> IDCloudPresets::list()
{
    std::vector<std::string> names;
    LLDirIterator iter(presetDir(), "*.xml");
    std::string file;
    while (iter.next(file))
    {
        names.push_back(gDirUtilp->getBaseFileName(file, true));
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool IDCloudPresets::load(const std::string& name, IDCloudPreset& out)
{
    return validName(name) && readFile(presetPath(name), out);
}

bool IDCloudPresets::save(const std::string& name, const IDCloudPreset& p)
{
    return validName(name) && ensureDir() && writeFile(presetPath(name), p);
}

bool IDCloudPresets::remove(const std::string& name)
{
    return validName(name) && LLFile::remove(presetPath(name)) == 0;
}

bool IDCloudPresets::importFile(const std::string& path, std::string& name_out)
{
    IDCloudPreset p;
    if (!readFile(path, p))
    {
        return false;
    }

    const std::string stem = gDirUtilp->getBaseFileName(path, true);
    if (!validName(stem) || !ensureDir())
    {
        return false;
    }

    std::string name = stem;
    for (U32 n = 2; LLFile::isfile(presetPath(name)); ++n)
    {
        name = stem + " " + std::to_string(n);
    }

    if (!writeFile(presetPath(name), p))
    {
        return false;
    }
    name_out = name;
    return true;
}

bool IDCloudPresets::exportFile(const std::string& name, const std::string& path)
{
    IDCloudPreset p;
    return load(name, p) && writeFile(path, p);
}

bool IDCloudPresets::exportPreset(const IDCloudPreset& p, const std::string& path)
{
    return writeFile(path, p);
}

IDCloudState::IDCloudState()
{
}

void IDCloudState::initSingleton()
{
    IDCloudPreset p;
    if (!fromLLSD(gSavedSettings.getLLSD(SETTING_STATE), p) || p.mLayers.empty())
    {
        p = defaultPreset();
    }
    mCurrent = p;
    mWind.assign(mCurrent.mLayers.size(), LLVector2::zero);
    LL_INFOS("VolumetricClouds") << "Loaded cloud state with " << mCurrent.mLayers.size()
                                 << (mCurrent.mLayers.size() == 1 ? " layer" : " layers") << LL_ENDL;
}

void IDCloudState::setCurrent(const IDCloudPreset& p)
{
    fromLLSD(toLLSD(p), mCurrent);
    mWind.resize(mCurrent.mLayers.size(), LLVector2::zero);
    gSavedSettings.setLLSD(SETTING_STATE, toLLSD(mCurrent));
    mChanged();
}

void IDCloudState::removeLayer(U32 index)
{
    if (index >= mCurrent.mLayers.size())
    {
        return;
    }
    mCurrent.mLayers.erase(mCurrent.mLayers.begin() + index);
    if (index < mWind.size())
    {
        mWind.erase(mWind.begin() + index);
    }
    mWind.resize(mCurrent.mLayers.size(), LLVector2::zero);
    gSavedSettings.setLLSD(SETTING_STATE, toLLSD(mCurrent));
    mChanged();
}

void IDCloudState::advance(F32 dt_seconds)
{
    if (mWindFrozen > 0)
    {
        return;
    }
    for (size_t i = 0; i < mCurrent.mLayers.size() && i < mWind.size(); ++i)
    {
        const IDCloudLayer& l = mCurrent.mLayers[i];
        const F32 rad = l.mWindDirDeg * DEG_TO_RAD;
        const F32 d = l.mWindSpeedMS * dt_seconds;
        LLVector2& w = mWind[i];
        w.mV[VX] = std::fmod(w.mV[VX] + std::sin(rad) * d, WIND_WRAP_M);
        w.mV[VY] = std::fmod(w.mV[VY] + std::cos(rad) * d, WIND_WRAP_M);
        if (w.mV[VX] < 0.f) w.mV[VX] += WIND_WRAP_M;
        if (w.mV[VY] < 0.f) w.mV[VY] += WIND_WRAP_M;
    }
}

LLVector2 IDCloudState::windOffsetM(U32 layer) const
{
    return layer < mWind.size() ? mWind[layer] : LLVector2::zero;
}

#pragma once

/**
 * LibreSecondary - DaVinci Resolve-Style Secondary Keyer (Qualifier) OFX Plugin
 * Compatible with Foundry Nuke and other OpenFX compliant hosts.
 *
 * Licensed under the MIT License.
 */

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsParam.h"

#include <vector>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace LibreSecondary {

// Plugin metadata constants
constexpr const char* kPluginIdentifier = "org.openfx.libresecondary";
constexpr unsigned int kPluginVersionMajor = 1;
constexpr unsigned int kPluginVersionMinor = 1;

// Maximum number of dynamic list colors for Add and Sub
constexpr int kMaxListColors = 16;

// Parameter identifier constants
constexpr const char* kParamColorSpace   = "color_space";
constexpr const char* kParamFalloffCurve = "falloff_curve";
constexpr const char* kParamFilterType   = "filter_type";

constexpr const char* kParamHsvPick      = "hsv_pick";

// Legacy single add/sub parameters (preserved for backward compatibility)
constexpr const char* kParamEnableAdd    = "enable_add";
constexpr const char* kParamColorAdd     = "color_add";
constexpr const char* kParamEnableSub    = "enable_sub";
constexpr const char* kParamColorSub     = "color_sub";

// Add Colors list parameters
constexpr const char* kParamGrpAddColors  = "grp_add_colors";
constexpr const char* kParamPickAddColor  = "pick_add_color";
constexpr const char* kParamContinuousAdd = "continuous_add";
constexpr const char* kParamBtnAddColor   = "btn_add_color";
constexpr const char* kParamBtnClearAdd   = "btn_clear_add";
constexpr const char* kParamNumAddColors  = "num_add_colors";
constexpr const char* kParamPrefixAddColor = "add_color_";
constexpr const char* kParamPrefixDelAdd   = "del_add_";

// Sub Colors list parameters
constexpr const char* kParamGrpSubColors  = "grp_sub_colors";
constexpr const char* kParamPickSubColor  = "pick_sub_color";
constexpr const char* kParamContinuousSub = "continuous_sub";
constexpr const char* kParamBtnSubColor   = "btn_sub_color";
constexpr const char* kParamBtnClearSub   = "btn_clear_sub";
constexpr const char* kParamNumSubColors  = "num_sub_colors";
constexpr const char* kParamPrefixSubColor = "sub_color_";
constexpr const char* kParamPrefixDelSub   = "del_sub_";

// Qualification tolerance controls
constexpr const char* kParamHueSpread    = "hue_spread";
constexpr const char* kParamHueFalloff   = "hue_falloff";
constexpr const char* kParamSatSpread    = "sat_spread";
constexpr const char* kParamSatFalloff   = "sat_falloff";
constexpr const char* kParamValSpread    = "val_spread";
constexpr const char* kParamValFalloff   = "val_falloff";

// Matte Finesse controls
constexpr const char* kParamMatteShrink  = "matte_shrink";
constexpr const char* kParamMatteGrow    = "matte_grow";
constexpr const char* kParamMatteBlur    = "matte_blur";

constexpr const char* kParamOutputMode   = "output_mode";

enum ColorSpaceEnum {
    eColorSpaceHSV = 0,
    eColorSpaceHSL = 1,
    eColorSpaceYCbCr = 2
};

enum FalloffCurveEnum {
    eFalloffCurveSmoothstep = 0,    // Hermite cubic (3t^2 - 2t^3)
    eFalloffCurveSmootherstep = 1,  // Perlin quintic (6t^5 - 15t^4 + 10t^3)
    eFalloffCurveCosine = 2,        // Filmic Cosine S-curve (0.5 * (1 + cos(pi * t)))
    eFalloffCurveLinear = 3         // Linear slope (1 - t)
};

enum FilterTypeEnum {
    eFilterTypeGaussian = 0,    // 2-Pass Separable Gaussian convolution
    eFilterTypeBox = 1,         // Moving average box blur
    eFilterTypeTriangle = 2,    // Tent / triangle linear weighted convolution
    eFilterTypeExponential = 3  // Exponential / Cauchy optical bloom
};

enum OutputModeEnum {
    eOutputModePremultiplied = 0,
    eOutputModeAlphaOnly = 1
};

struct RGBColor {
    float r{0.0f}, g{0.0f}, b{0.0f};
};

struct Color3 {
    float c1{0.0f}, c2{0.0f}, c3{0.0f};
};

/**
 * @brief Snapshot of evaluated parameter values at a specific frame/time.
 */
struct KeyerParams {
    ColorSpaceEnum colorSpace{eColorSpaceHSV};
    FalloffCurveEnum falloffCurve{eFalloffCurveSmoothstep};
    FilterTypeEnum filterType{eFilterTypeGaussian};

    RGBColor basePick{0.0f, 0.0f, 0.0f};

    // Legacy single add/sub
    bool enableLegacyAdd{false};
    RGBColor legacyAdd{0.0f, 0.0f, 0.0f};
    bool enableLegacySub{false};
    RGBColor legacySub{0.0f, 0.0f, 0.0f};

    // Dynamic Multi-Color Lists
    std::vector<RGBColor> addColors;
    std::vector<RGBColor> subColors;

    // Tolerance Spread and Falloff
    float spread1{0.1f}, falloff1{0.05f};
    float spread2{0.2f}, falloff2{0.1f};
    float spread3{0.2f}, falloff3{0.1f};

    // Matte Finesse
    float matteShrink{0.0f};
    float matteGrow{0.0f};
    float matteBlur{0.0f};

    OutputModeEnum outputMode{eOutputModePremultiplied};
};

/**
 * @brief 2D floating-point matte buffer used for multi-pass spatial operations.
 */
class MatteBuffer {
public:
    MatteBuffer() : _bounds{0, 0, 0, 0}, _width(0), _height(0) {}

    explicit MatteBuffer(const OfxRectI &bounds)
        : _bounds(bounds),
          _width(std::max(0, bounds.x2 - bounds.x1)),
          _height(std::max(0, bounds.y2 - bounds.y1)),
          _data(_width * _height, 0.0f) {}

    const OfxRectI& bounds() const { return _bounds; }
    int width() const { return _width; }
    int height() const { return _height; }

    inline float get(int x, int y) const {
        if (_data.empty()) return 0.0f;
        int cx = std::max(_bounds.x1, std::min(x, _bounds.x2 - 1));
        int cy = std::max(_bounds.y1, std::min(y, _bounds.y2 - 1));
        return _data[(cy - _bounds.y1) * _width + (cx - _bounds.x1)];
    }

    inline void set(int x, int y, float val) {
        if (x >= _bounds.x1 && x < _bounds.x2 && y >= _bounds.y1 && y < _bounds.y2) {
            _data[(y - _bounds.y1) * _width + (x - _bounds.x1)] = val;
        }
    }

private:
    OfxRectI _bounds;
    int _width;
    int _height;
    std::vector<float> _data;
};

/**
 * @brief Multi-threaded image processing engine using OFX::MultiThread::Processor.
 */
class SecondaryKeyerProcessor : public OFX::MultiThread::Processor {
public:
    enum PassType {
        ePassDirectRender,
        ePassRawMatte,
        ePassShrink,
        ePassGrow,
        ePassBlurH,
        ePassBlurV,
        ePassOutputComposite
    };

    SecondaryKeyerProcessor(OFX::ImageEffect &effect,
                           OFX::Image *srcImg,
                           OFX::Image *dstImg,
                           const OfxRectI &renderWindow,
                           const KeyerParams &params);

    virtual ~SecondaryKeyerProcessor() override = default;

    /* Execution orchestrator */
    void process();

    /* Multi-thread callback override */
    virtual void multiThreadFunction(unsigned int threadID, unsigned int nThreads) override;

private:
    void processDirectRender(int yStart, int yEnd);
    void processRawMatte(int yStart, int yEnd);
    void processShrink(int yStart, int yEnd);
    void processGrow(int yStart, int yEnd);
    void processBlurH(int yStart, int yEnd);
    void processBlurV(int yStart, int yEnd);
    void processOutputComposite(int yStart, int yEnd);

    void sampleSource(int x, int y, float &r, float &g, float &b, float &a) const;
    float evaluateMatte(float r, float g, float b) const;
    float evaluateSingleTarget(const Color3 &sample, const Color3 &target) const;

    OFX::ImageEffect &_effect;
    OFX::Image *_srcImg{nullptr};
    OFX::Image *_dstImg{nullptr};
    OfxRectI _renderWindow;
    OfxRectI _srcBounds{0, 0, 0, 0};
    KeyerParams _params;

    PassType _currentPass{ePassDirectRender};
    OfxRectI _currentRect{0, 0, 0, 0};
    const MatteBuffer *_inBuf{nullptr};

    // Pre-converted target color coordinates in the active color space
    Color3 _targetBase;
    std::vector<Color3> _targetsAdd;
    std::vector<Color3> _targetsSub;

    // Intermediate spatial buffers
    MatteBuffer _bufRaw;
    MatteBuffer _bufShrink;
    MatteBuffer _bufGrow;
    MatteBuffer _bufBlurH;
    MatteBuffer _bufBlurV;

    // Separable 1D convolution kernel
    std::vector<float> _blurKernel;
    int _blurRadius{0};
};

/**
 * @brief OpenFX ImageEffect plugin instance.
 */
class SecondaryKeyerPlugin : public OFX::ImageEffect {
public:
    explicit SecondaryKeyerPlugin(OfxImageEffectHandle handle);
    virtual ~SecondaryKeyerPlugin() override = default;

    /* OFX ImageEffect Actions */
    virtual void render(const OFX::RenderArguments &args) override;
    virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod) override;
    virtual void getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args, OFX::RegionOfInterestSetter &rois) override;
    virtual bool isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip *&identityClip, double &identityTime) override;
    virtual void changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName) override;

    double calculateSpatialPadding(double time) const;
    KeyerParams fetchParams(double time) const;

private:
    int getNumAddColors() const;
    void setNumAddColors(int count);
    void removeAddColor(int idx);

    int getNumSubColors() const;
    void setNumSubColors(int count);
    void removeSubColor(int idx);

    void syncListVisibility();

    // Clips
    OFX::Clip *dstClip_{nullptr};
    OFX::Clip *srcClip_{nullptr};

    // General Configuration Parameters
    OFX::ChoiceParam *colorSpace_{nullptr};
    OFX::ChoiceParam *falloffCurve_{nullptr};

    // Base Color Picker
    OFX::RGBParam *hsvPick_{nullptr};

    // Legacy Single Add/Sub parameters
    OFX::BooleanParam *enableAdd_{nullptr};
    OFX::RGBParam     *colorAdd_{nullptr};
    OFX::BooleanParam *enableSub_{nullptr};
    OFX::RGBParam     *colorSub_{nullptr};

    // Dynamic Add Colors Controls
    OFX::GroupParam      *grpAddColors_{nullptr};
    OFX::RGBParam        *pickAddColor_{nullptr};
    OFX::BooleanParam    *continuousAdd_{nullptr};
    OFX::PushButtonParam *btnAddColor_{nullptr};
    OFX::PushButtonParam *btnClearAdd_{nullptr};
    OFX::IntParam        *numAddColors_{nullptr};
    OFX::RGBParam        *addColors_[kMaxListColors]{nullptr};
    OFX::PushButtonParam *delAddBtns_[kMaxListColors]{nullptr};

    // Dynamic Sub Colors Controls
    OFX::GroupParam      *grpSubColors_{nullptr};
    OFX::RGBParam        *pickSubColor_{nullptr};
    OFX::BooleanParam    *continuousSub_{nullptr};
    OFX::PushButtonParam *btnSubColor_{nullptr};
    OFX::PushButtonParam *btnClearSub_{nullptr};
    OFX::IntParam        *numSubColors_{nullptr};
    OFX::RGBParam        *subColors_[kMaxListColors]{nullptr};
    OFX::PushButtonParam *delSubBtns_[kMaxListColors]{nullptr};

    // Tolerance controls
    OFX::DoubleParam *hueSpread_{nullptr};
    OFX::DoubleParam *hueFalloff_{nullptr};
    OFX::DoubleParam *satSpread_{nullptr};
    OFX::DoubleParam *satFalloff_{nullptr};
    OFX::DoubleParam *valSpread_{nullptr};
    OFX::DoubleParam *valFalloff_{nullptr};

    // Matte Finesse controls
    OFX::DoubleParam *matteShrink_{nullptr};
    OFX::DoubleParam *matteGrow_{nullptr};
    OFX::DoubleParam *matteBlur_{nullptr};
    OFX::ChoiceParam *filterType_{nullptr};

    OFX::ChoiceParam *outputMode_{nullptr};
};

/**
 * @brief OpenFX Factory for SecondaryKeyer plugin.
 */
class SecondaryKeyerFactory : public OFX::PluginFactoryHelper<SecondaryKeyerFactory> {
public:
    SecondaryKeyerFactory();
    virtual ~SecondaryKeyerFactory() = default;

    virtual void load() override {}
    virtual void unload() override {}
    virtual void describe(OFX::ImageEffectDescriptor &desc) override;
    virtual void describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context) override;
    virtual OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context) override;
};

} // namespace LibreSecondary

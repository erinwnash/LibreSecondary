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
constexpr unsigned int kPluginVersionMinor = 0;

// Parameter identifier constants
constexpr const char* kParamHsvPick    = "hsv_pick";
constexpr const char* kParamEnableAdd  = "enable_add";
constexpr const char* kParamColorAdd   = "color_add";
constexpr const char* kParamEnableSub  = "enable_sub";
constexpr const char* kParamColorSub   = "color_sub";

constexpr const char* kParamHueSpread  = "hue_spread";
constexpr const char* kParamHueFalloff = "hue_falloff";
constexpr const char* kParamSatSpread  = "sat_spread";
constexpr const char* kParamSatFalloff = "sat_falloff";
constexpr const char* kParamValSpread  = "val_spread";
constexpr const char* kParamValFalloff = "val_falloff";

constexpr const char* kParamMatteShrink = "matte_shrink";
constexpr const char* kParamMatteGrow   = "matte_grow";
constexpr const char* kParamMatteBlur   = "matte_blur";

constexpr const char* kParamOutputMode  = "output_mode";

enum OutputModeEnum {
    eOutputModePremultiplied = 0,
    eOutputModeAlphaOnly = 1
};

/**
 * @brief Snapshot of evaluated parameter values at a specific frame/time.
 */
struct KeyerParams {
    float pickR{0.0f}, pickG{0.0f}, pickB{0.0f};

    bool  enableAdd{false};
    float addR{0.0f}, addG{0.0f}, addB{0.0f};

    bool  enableSub{false};
    float subR{0.0f}, subG{0.0f}, subB{0.0f};

    float hueSpread{0.1f};
    float hueFalloff{0.05f};

    float satSpread{0.2f};
    float satFalloff{0.1f};

    float valSpread{0.2f};
    float valFalloff{0.1f};

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
    float computeMatte(float h, float s, float v, float targetH, float targetS, float targetV) const;

    OFX::ImageEffect &_effect;
    OFX::Image *_srcImg{nullptr};
    OFX::Image *_dstImg{nullptr};
    OfxRectI _renderWindow;
    OfxRectI _srcBounds{0, 0, 0, 0};
    KeyerParams _params;

    PassType _currentPass{ePassDirectRender};
    OfxRectI _currentRect{0, 0, 0, 0};
    const MatteBuffer *_inBuf{nullptr};

    // Target HSV values
    float _pickH{0.0f}, _pickS{0.0f}, _pickV{0.0f};
    float _addH{0.0f},  _addS{0.0f},  _addV{0.0f};
    float _subH{0.0f},  _subS{0.0f},  _subV{0.0f};

    // Intermediate spatial buffers
    MatteBuffer _bufRaw;
    MatteBuffer _bufShrink;
    MatteBuffer _bufGrow;
    MatteBuffer _bufBlurH;
    MatteBuffer _bufBlurV;

    // Separable Gaussian blur kernel
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

    double calculateSpatialPadding(double time) const;
    KeyerParams fetchParams(double time) const;

private:
    // Clips
    OFX::Clip *dstClip_{nullptr};
    OFX::Clip *srcClip_{nullptr};

    // Parameters
    OFX::RGBParam *hsvPick_{nullptr};
    OFX::BooleanParam *enableAdd_{nullptr};
    OFX::RGBParam *colorAdd_{nullptr};
    OFX::BooleanParam *enableSub_{nullptr};
    OFX::RGBParam *colorSub_{nullptr};

    OFX::DoubleParam *hueSpread_{nullptr};
    OFX::DoubleParam *hueFalloff_{nullptr};
    OFX::DoubleParam *satSpread_{nullptr};
    OFX::DoubleParam *satFalloff_{nullptr};
    OFX::DoubleParam *valSpread_{nullptr};
    OFX::DoubleParam *valFalloff_{nullptr};

    OFX::DoubleParam *matteShrink_{nullptr};
    OFX::DoubleParam *matteGrow_{nullptr};
    OFX::DoubleParam *matteBlur_{nullptr};

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

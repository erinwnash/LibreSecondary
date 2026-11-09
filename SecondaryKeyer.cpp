/**
 * LibreSecondary - DaVinci Resolve-Style Secondary Keyer (Qualifier) OFX Plugin
 * Compatible with Foundry Nuke and other OpenFX compliant hosts.
 *
 * Licensed under the MIT License.
 */

#include "SecondaryKeyer.h"

#ifdef _WINDOWS
#include <windows.h>
#endif

namespace LibreSecondary {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Helper: RGB to HSV conversion (inputs/outputs normalized to [0.0, 1.0])
inline void rgb2hsv(float r, float g, float b, float &h, float &s, float &v) {
    float maxVal = std::max(r, std::max(g, b));
    float minVal = std::min(r, std::min(g, b));
    float delta = maxVal - minVal;

    v = maxVal;

    if (maxVal > 1e-6f) {
        s = delta / maxVal;
    } else {
        s = 0.0f;
        h = 0.0f;
        return;
    }

    if (delta < 1e-6f) {
        h = 0.0f;
        return;
    }

    if (r >= maxVal) {
        h = (g - b) / delta;
    } else if (g >= maxVal) {
        h = 2.0f + (b - r) / delta;
    } else {
        h = 4.0f + (r - g) / delta;
    }

    h /= 6.0f;
    if (h < 0.0f) {
        h += 1.0f;
    }
}

// Helper: RGB to HSL conversion (inputs/outputs normalized to [0.0, 1.0])
inline void rgb2hsl(float r, float g, float b, float &h, float &s, float &l) {
    float maxVal = std::max(r, std::max(g, b));
    float minVal = std::min(r, std::min(g, b));
    float delta = maxVal - minVal;

    l = (maxVal + minVal) * 0.5f;

    if (delta < 1e-6f) {
        s = 0.0f;
        h = 0.0f;
        return;
    }

    float denom = 1.0f - std::fabs(2.0f * l - 1.0f);
    s = (denom > 1e-6f) ? (delta / denom) : 0.0f;

    if (r >= maxVal) {
        h = (g - b) / delta;
    } else if (g >= maxVal) {
        h = 2.0f + (b - r) / delta;
    } else {
        h = 4.0f + (r - g) / delta;
    }

    h /= 6.0f;
    if (h < 0.0f) {
        h += 1.0f;
    }
}

// Helper: RGB to YCbCr conversion (ITU-R BT.709 standard, normalized to [0.0, 1.0])
inline void rgb2ycbcr(float r, float g, float b, float &y, float &cb, float &cr) {
    y  =  0.2126f * r + 0.7152f * g + 0.0722f * b;
    // Map bipolar chroma [-0.5, 0.5] to [0.0, 1.0] with neutral 0.5
    cb = -0.1146f * r - 0.3854f * g + 0.5000f * b + 0.5f;
    cr =  0.5000f * r - 0.4542f * g - 0.0458f * b + 0.5f;
}

// Unified color space transformation helper
inline Color3 rgbToSpace(float r, float g, float b, ColorSpaceEnum space) {
    Color3 out{0.0f, 0.0f, 0.0f};
    switch (space) {
        case eColorSpaceHSV:
            rgb2hsv(r, g, b, out.c1, out.c2, out.c3);
            break;
        case eColorSpaceHSL:
            rgb2hsl(r, g, b, out.c1, out.c2, out.c3);
            break;
        case eColorSpaceYCbCr:
            rgb2ycbcr(r, g, b, out.c1, out.c2, out.c3);
            break;
    }
    return out;
}

// Helper: compute distances between sample and target in the active color space
inline void computeDistances(const Color3 &sample, const Color3 &target, ColorSpaceEnum space,
                             float &d1, float &d2, float &d3) {
    if (space == eColorSpaceHSV || space == eColorSpaceHSL) {
        // Channel 1 is Hue (circular on [0, 1))
        float dh = std::fabs(sample.c1 - target.c1);
        if (dh > 0.5f) {
            dh = 1.0f - dh;
        }
        d1 = dh;
        // Channel 2 is Saturation
        d2 = std::fabs(sample.c2 - target.c2);
        // Channel 3 is Value (HSV) or Lightness (HSL)
        d3 = std::fabs(sample.c3 - target.c3);
    } else {
        // eColorSpaceYCbCr: Cartesian Euclidean distances on [0, 1]
        // Channel 1: Luma Y
        d1 = std::fabs(sample.c1 - target.c1);
        // Channel 2: Chroma Cb
        d2 = std::fabs(sample.c2 - target.c2);
        // Channel 3: Chroma Cr
        d3 = std::fabs(sample.c3 - target.c3);
    }
}

// Helper: 4 best falloff transition curves
inline float evaluateFalloff(float dist, float spread, float falloff, FalloffCurveEnum curve) {
    if (dist <= spread) {
        return 1.0f;
    }
    float edge1 = spread + std::max(1e-5f, falloff);
    if (dist >= edge1) {
        return 0.0f;
    }
    float t = (dist - spread) / (edge1 - spread);
    t = std::max(0.0f, std::min(t, 1.0f));

    switch (curve) {
        case eFalloffCurveSmoothstep:
            // Hermite cubic: 1.0 - (3t^2 - 2t^3)
            return 1.0f - t * t * (3.0f - 2.0f * t);

        case eFalloffCurveSmootherstep:
            // Perlin quintic: 1.0 - (6t^5 - 15t^4 + 10t^3)
            return 1.0f - t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);

        case eFalloffCurveCosine:
            // Photographic Cosine S-curve: 0.5 * (1.0 + cos(pi * t))
            return 0.5f * (1.0f + std::cos(kPi * t));

        case eFalloffCurveLinear:
            // Linear ramp: 1.0 - t
            return 1.0f - t;
    }
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

// Helper: generate 1D separable convolution blur kernel for the 4 best filter types
inline void generateBlurKernel(FilterTypeEnum type, float radiusParam, int kRadius, std::vector<float> &kernel) {
    kernel.resize(2 * kRadius + 1);
    float sumW = 0.0f;
    float sigma = std::max(0.1f, radiusParam);

    switch (type) {
        case eFilterTypeGaussian: {
            float twoSigmaSq = 2.0f * sigma * sigma;
            for (int i = -kRadius; i <= kRadius; ++i) {
                float w = std::exp(-static_cast<float>(i * i) / twoSigmaSq);
                kernel[i + kRadius] = w;
                sumW += w;
            }
            break;
        }
        case eFilterTypeBox: {
            for (int i = -kRadius; i <= kRadius; ++i) {
                kernel[i + kRadius] = 1.0f;
                sumW += 1.0f;
            }
            break;
        }
        case eFilterTypeTriangle: {
            float rPlus1 = static_cast<float>(kRadius + 1);
            for (int i = -kRadius; i <= kRadius; ++i) {
                float w = 1.0f - std::fabs(static_cast<float>(i)) / rPlus1;
                kernel[i + kRadius] = w;
                sumW += w;
            }
            break;
        }
        case eFilterTypeExponential: {
            for (int i = -kRadius; i <= kRadius; ++i) {
                float w = std::exp(-std::fabs(static_cast<float>(i)) / sigma);
                kernel[i + kRadius] = w;
                sumW += w;
            }
            break;
        }
    }

    if (sumW > 1e-6f) {
        for (float &w : kernel) {
            w /= sumW;
        }
    }
}

// Helper: Expand an OfxRectI by uniform padding
inline OfxRectI expandRect(const OfxRectI &rect, int pad) {
    OfxRectI r;
    r.x1 = rect.x1 - pad;
    r.y1 = rect.y1 - pad;
    r.x2 = rect.x2 + pad;
    r.y2 = rect.y2 + pad;
    return r;
}

} // anonymous namespace

////////////////////////////////////////////////////////////////////////////////
// SecondaryKeyerProcessor Implementation
////////////////////////////////////////////////////////////////////////////////

SecondaryKeyerProcessor::SecondaryKeyerProcessor(OFX::ImageEffect &effect,
                                                 OFX::Image *srcImg,
                                                 OFX::Image *dstImg,
                                                 const OfxRectI &renderWindow,
                                                 const KeyerParams &params)
    : OFX::MultiThread::Processor(),
      _effect(effect),
      _srcImg(srcImg),
      _dstImg(dstImg),
      _renderWindow(renderWindow),
      _params(params)
{
    if (_srcImg) {
        _srcBounds = _srcImg->getBounds();
    }

    // Pre-convert target colors to the active color space
    _targetBase = rgbToSpace(_params.basePick.r, _params.basePick.g, _params.basePick.b, _params.colorSpace);

    // Additional colors: include legacy add color if enabled
    if (_params.enableLegacyAdd) {
        _targetsAdd.push_back(rgbToSpace(_params.legacyAdd.r, _params.legacyAdd.g, _params.legacyAdd.b, _params.colorSpace));
    }
    for (const auto &col : _params.addColors) {
        _targetsAdd.push_back(rgbToSpace(col.r, col.g, col.b, _params.colorSpace));
    }

    // Subtracted colors: include legacy sub color if enabled
    if (_params.enableLegacySub) {
        _targetsSub.push_back(rgbToSpace(_params.legacySub.r, _params.legacySub.g, _params.legacySub.b, _params.colorSpace));
    }
    for (const auto &col : _params.subColors) {
        _targetsSub.push_back(rgbToSpace(col.r, col.g, col.b, _params.colorSpace));
    }

    // Determine spatial filtering radii
    int rShrink = static_cast<int>(std::ceil(_params.matteShrink));
    int rGrow   = static_cast<int>(std::ceil(_params.matteGrow));
    int rBlur   = static_cast<int>(std::ceil(_params.matteBlur * 2.0f));

    bool needsSpatial = (rShrink > 0 || rGrow > 0 || rBlur > 0);

    if (needsSpatial) {
        OfxRectI rectDst    = _renderWindow;
        OfxRectI rectGrow   = expandRect(rectDst, rBlur);
        OfxRectI rectShrink = expandRect(rectGrow, rGrow);
        OfxRectI rectRaw    = expandRect(rectShrink, rShrink);

        _bufRaw = MatteBuffer(rectRaw);

        if (rShrink > 0) {
            _bufShrink = MatteBuffer(rectShrink);
        }
        if (rGrow > 0) {
            _bufGrow = MatteBuffer(rectGrow);
        }
        if (rBlur > 0) {
            _blurRadius = rBlur;
            _bufBlurH = MatteBuffer(OfxRectI{rectDst.x1, rectGrow.y1, rectDst.x2, rectGrow.y2});
            _bufBlurV = MatteBuffer(rectDst);

            generateBlurKernel(_params.filterType, _params.matteBlur, _blurRadius, _blurKernel);
        }
    }
}

void SecondaryKeyerProcessor::process() {
    int rShrink = static_cast<int>(std::ceil(_params.matteShrink));
    int rGrow   = static_cast<int>(std::ceil(_params.matteGrow));
    int rBlur   = static_cast<int>(std::ceil(_params.matteBlur * 2.0f));

    bool needsSpatial = (rShrink > 0 || rGrow > 0 || rBlur > 0);

    if (!needsSpatial) {
        // Fast direct pass: compute qualifier and composite straight to destination
        _currentPass = ePassDirectRender;
        _currentRect = _renderWindow;
        multiThread();
        return;
    }

    // Pass 1: Raw Matte Generation for padded boundary
    _currentPass = ePassRawMatte;
    _currentRect = _bufRaw.bounds();
    multiThread();

    const MatteBuffer *curBuf = &_bufRaw;

    // Pass 2: Matte Shrink (Erosion / Minimum filter)
    if (rShrink > 0) {
        _currentPass = ePassShrink;
        _currentRect = _bufShrink.bounds();
        _inBuf = curBuf;
        multiThread();
        curBuf = &_bufShrink;
    }

    // Pass 3: Matte Grow (Dilation / Maximum filter)
    if (rGrow > 0) {
        _currentPass = ePassGrow;
        _currentRect = _bufGrow.bounds();
        _inBuf = curBuf;
        multiThread();
        curBuf = &_bufGrow;
    }

    // Pass 4 & 5: Separable Blur Convolution (Horizontal & Vertical passes)
    if (rBlur > 0) {
        _currentPass = ePassBlurH;
        _currentRect = _bufBlurH.bounds();
        _inBuf = curBuf;
        multiThread();

        _currentPass = ePassBlurV;
        _currentRect = _bufBlurV.bounds();
        _inBuf = &_bufBlurH;
        multiThread();
        curBuf = &_bufBlurV;
    }

    // Pass 6: Final Output Composite
    _currentPass = ePassOutputComposite;
    _currentRect = _renderWindow;
    _inBuf = curBuf;
    multiThread();
}

void SecondaryKeyerProcessor::multiThreadFunction(unsigned int threadID, unsigned int nThreads) {
    if (_effect.abort()) return;

    int totalY = _currentRect.y2 - _currentRect.y1;
    if (totalY <= 0) return;

    int sliceH = (totalY + static_cast<int>(nThreads) - 1) / static_cast<int>(nThreads);
    int yStart = _currentRect.y1 + static_cast<int>(threadID) * sliceH;
    int yEnd   = std::min(_currentRect.y2, yStart + sliceH);

    if (yStart >= _currentRect.y2) return;

    switch (_currentPass) {
        case ePassDirectRender:
            processDirectRender(yStart, yEnd);
            break;
        case ePassRawMatte:
            processRawMatte(yStart, yEnd);
            break;
        case ePassShrink:
            processShrink(yStart, yEnd);
            break;
        case ePassGrow:
            processGrow(yStart, yEnd);
            break;
        case ePassBlurH:
            processBlurH(yStart, yEnd);
            break;
        case ePassBlurV:
            processBlurV(yStart, yEnd);
            break;
        case ePassOutputComposite:
            processOutputComposite(yStart, yEnd);
            break;
    }
}

void SecondaryKeyerProcessor::sampleSource(int x, int y, float &r, float &g, float &b, float &a) const {
    if (!_srcImg) {
        r = g = b = a = 0.0f;
        return;
    }
    int cx = std::max(_srcBounds.x1, std::min(x, _srcBounds.x2 - 1));
    int cy = std::max(_srcBounds.y1, std::min(y, _srcBounds.y2 - 1));
    const float *p = static_cast<const float*>(_srcImg->getPixelAddress(cx, cy));
    if (p) {
        r = p[0];
        g = p[1];
        b = p[2];
        a = p[3];
    } else {
        r = g = b = a = 0.0f;
    }
}

float SecondaryKeyerProcessor::evaluateSingleTarget(const Color3 &sample, const Color3 &target) const {
    float d1, d2, d3;
    computeDistances(sample, target, _params.colorSpace, d1, d2, d3);

    float m1 = evaluateFalloff(d1, _params.spread1, _params.falloff1, _params.falloffCurve);
    float m2 = evaluateFalloff(d2, _params.spread2, _params.falloff2, _params.falloffCurve);
    float m3 = evaluateFalloff(d3, _params.spread3, _params.falloff3, _params.falloffCurve);

    return m1 * m2 * m3;
}

float SecondaryKeyerProcessor::evaluateMatte(float r, float g, float b) const {
    Color3 sample = rgbToSpace(r, g, b, _params.colorSpace);

    // Base color qualification
    float matte = evaluateSingleTarget(sample, _targetBase);

    // Additional colors: union (max)
    for (const auto &target : _targetsAdd) {
        float mAdd = evaluateSingleTarget(sample, target);
        matte = std::max(matte, mAdd);
    }

    // Subtracted colors: subtraction (carve out)
    for (const auto &target : _targetsSub) {
        float mSub = evaluateSingleTarget(sample, target);
        matte = std::max(0.0f, matte - mSub);
    }

    return std::max(0.0f, std::min(matte, 1.0f));
}

void SecondaryKeyerProcessor::processDirectRender(int yStart, int yEnd) {
    for (int y = yStart; y < yEnd; ++y) {
        if (_effect.abort()) break;
        float *dstPix = static_cast<float*>(_dstImg->getPixelAddress(_renderWindow.x1, y));
        for (int x = _renderWindow.x1; x < _renderWindow.x2; ++x) {
            float srcR = 0.0f, srcG = 0.0f, srcB = 0.0f, srcA = 0.0f;
            sampleSource(x, y, srcR, srcG, srcB, srcA);

            float m = evaluateMatte(srcR, srcG, srcB);

            if (_params.outputMode == eOutputModePremultiplied) {
                dstPix[0] = srcR * m;
                dstPix[1] = srcG * m;
                dstPix[2] = srcB * m;
                dstPix[3] = m;
            } else {
                dstPix[0] = m;
                dstPix[1] = m;
                dstPix[2] = m;
                dstPix[3] = m;
            }
            dstPix += 4;
        }
    }
}

void SecondaryKeyerProcessor::processRawMatte(int yStart, int yEnd) {
    for (int y = yStart; y < yEnd; ++y) {
        if (_effect.abort()) break;
        for (int x = _currentRect.x1; x < _currentRect.x2; ++x) {
            float srcR = 0.0f, srcG = 0.0f, srcB = 0.0f, srcA = 0.0f;
            sampleSource(x, y, srcR, srcG, srcB, srcA);
            float m = evaluateMatte(srcR, srcG, srcB);
            _bufRaw.set(x, y, m);
        }
    }
}

void SecondaryKeyerProcessor::processShrink(int yStart, int yEnd) {
    int r = static_cast<int>(std::ceil(_params.matteShrink));
    int r2 = r * r;

    for (int y = yStart; y < yEnd; ++y) {
        if (_effect.abort()) break;
        for (int x = _currentRect.x1; x < _currentRect.x2; ++x) {
            float minVal = 1.0f;
            for (int dy = -r; dy <= r; ++dy) {
                int maxDx = static_cast<int>(std::sqrt(std::max(0, r2 - dy * dy)));
                for (int dx = -maxDx; dx <= maxDx; ++dx) {
                    float val = _inBuf->get(x + dx, y + dy);
                    if (val < minVal) {
                        minVal = val;
                    }
                }
            }
            _bufShrink.set(x, y, minVal);
        }
    }
}

void SecondaryKeyerProcessor::processGrow(int yStart, int yEnd) {
    int r = static_cast<int>(std::ceil(_params.matteGrow));
    int r2 = r * r;

    for (int y = yStart; y < yEnd; ++y) {
        if (_effect.abort()) break;
        for (int x = _currentRect.x1; x < _currentRect.x2; ++x) {
            float maxVal = 0.0f;
            for (int dy = -r; dy <= r; ++dy) {
                int maxDx = static_cast<int>(std::sqrt(std::max(0, r2 - dy * dy)));
                for (int dx = -maxDx; dx <= maxDx; ++dx) {
                    float val = _inBuf->get(x + dx, y + dy);
                    if (val > maxVal) {
                        maxVal = val;
                    }
                }
            }
            _bufGrow.set(x, y, maxVal);
        }
    }
}

void SecondaryKeyerProcessor::processBlurH(int yStart, int yEnd) {
    int kRadius = _blurRadius;
    const float *kernel = _blurKernel.data();

    for (int y = yStart; y < yEnd; ++y) {
        if (_effect.abort()) break;
        for (int x = _currentRect.x1; x < _currentRect.x2; ++x) {
            float sum = 0.0f;
            for (int k = -kRadius; k <= kRadius; ++k) {
                sum += _inBuf->get(x + k, y) * kernel[k + kRadius];
            }
            _bufBlurH.set(x, y, sum);
        }
    }
}

void SecondaryKeyerProcessor::processBlurV(int yStart, int yEnd) {
    int kRadius = _blurRadius;
    const float *kernel = _blurKernel.data();

    for (int y = yStart; y < yEnd; ++y) {
        if (_effect.abort()) break;
        for (int x = _currentRect.x1; x < _currentRect.x2; ++x) {
            float sum = 0.0f;
            for (int k = -kRadius; k <= kRadius; ++k) {
                sum += _inBuf->get(x, y + k) * kernel[k + kRadius];
            }
            _bufBlurV.set(x, y, sum);
        }
    }
}

void SecondaryKeyerProcessor::processOutputComposite(int yStart, int yEnd) {
    for (int y = yStart; y < yEnd; ++y) {
        if (_effect.abort()) break;
        float *dstPix = static_cast<float*>(_dstImg->getPixelAddress(_renderWindow.x1, y));
        for (int x = _renderWindow.x1; x < _renderWindow.x2; ++x) {
            float srcR = 0.0f, srcG = 0.0f, srcB = 0.0f, srcA = 0.0f;
            sampleSource(x, y, srcR, srcG, srcB, srcA);

            float m = _inBuf->get(x, y);
            m = std::max(0.0f, std::min(m, 1.0f));

            if (_params.outputMode == eOutputModePremultiplied) {
                dstPix[0] = srcR * m;
                dstPix[1] = srcG * m;
                dstPix[2] = srcB * m;
                dstPix[3] = m;
            } else {
                dstPix[0] = m;
                dstPix[1] = m;
                dstPix[2] = m;
                dstPix[3] = m;
            }
            dstPix += 4;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// SecondaryKeyerPlugin Implementation
////////////////////////////////////////////////////////////////////////////////

SecondaryKeyerPlugin::SecondaryKeyerPlugin(OfxImageEffectHandle handle)
    : OFX::ImageEffect(handle)
{
    // Fetch input and output clips
    dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
    srcClip_ = fetchClip(kOfxImageEffectSimpleSourceClipName);

    // Global settings
    colorSpace_   = fetchChoiceParam(kParamColorSpace);
    falloffCurve_ = fetchChoiceParam(kParamFalloffCurve);

    // Base Color Picker
    hsvPick_   = fetchRGBParam(kParamHsvPick);

    // Legacy Single Add/Sub parameters
    enableAdd_ = fetchBooleanParam(kParamEnableAdd);
    colorAdd_  = fetchRGBParam(kParamColorAdd);
    enableSub_ = fetchBooleanParam(kParamEnableSub);
    colorSub_  = fetchRGBParam(kParamColorSub);

    // Dynamic Add Colors Group Controls
    grpAddColors_   = fetchGroupParam(kParamGrpAddColors);
    pickAddColor_   = fetchRGBParam(kParamPickAddColor);
    continuousAdd_  = fetchBooleanParam(kParamContinuousAdd);
    btnAddColor_    = fetchPushButtonParam(kParamBtnAddColor);
    btnClearAdd_    = fetchPushButtonParam(kParamBtnClearAdd);
    numAddColors_   = fetchIntParam(kParamNumAddColors);

    for (int i = 0; i < kMaxListColors; ++i) {
        std::string nameColor = std::string(kParamPrefixAddColor) + std::to_string(i);
        std::string nameDel   = std::string(kParamPrefixDelAdd) + std::to_string(i);
        addColors_[i] = fetchRGBParam(nameColor);
        delAddBtns_[i] = fetchPushButtonParam(nameDel);
    }

    // Dynamic Sub Colors Group Controls
    grpSubColors_   = fetchGroupParam(kParamGrpSubColors);
    pickSubColor_   = fetchRGBParam(kParamPickSubColor);
    continuousSub_  = fetchBooleanParam(kParamContinuousSub);
    btnSubColor_    = fetchPushButtonParam(kParamBtnSubColor);
    btnClearSub_    = fetchPushButtonParam(kParamBtnClearSub);
    numSubColors_   = fetchIntParam(kParamNumSubColors);

    for (int i = 0; i < kMaxListColors; ++i) {
        std::string nameColor = std::string(kParamPrefixSubColor) + std::to_string(i);
        std::string nameDel   = std::string(kParamPrefixDelSub) + std::to_string(i);
        subColors_[i] = fetchRGBParam(nameColor);
        delSubBtns_[i] = fetchPushButtonParam(nameDel);
    }

    // Tolerance controls
    hueSpread_  = fetchDoubleParam(kParamHueSpread);
    hueFalloff_ = fetchDoubleParam(kParamHueFalloff);
    satSpread_  = fetchDoubleParam(kParamSatSpread);
    satFalloff_ = fetchDoubleParam(kParamSatFalloff);
    valSpread_  = fetchDoubleParam(kParamValSpread);
    valFalloff_ = fetchDoubleParam(kParamValFalloff);

    // Matte Finesse controls
    matteShrink_ = fetchDoubleParam(kParamMatteShrink);
    matteGrow_   = fetchDoubleParam(kParamMatteGrow);
    matteBlur_   = fetchDoubleParam(kParamMatteBlur);
    filterType_  = fetchChoiceParam(kParamFilterType);

    outputMode_  = fetchChoiceParam(kParamOutputMode);

    syncListVisibility();
}

int SecondaryKeyerPlugin::getNumAddColors() const {
    if (!numAddColors_) return 0;
    int v = 0;
    numAddColors_->getValue(v);
    return std::max(0, std::min(v, kMaxListColors));
}

void SecondaryKeyerPlugin::setNumAddColors(int count) {
    if (numAddColors_) {
        numAddColors_->setValue(std::max(0, std::min(count, kMaxListColors)));
    }
}

void SecondaryKeyerPlugin::removeAddColor(int idx) {
    int count = getNumAddColors();
    if (idx < 0 || idx >= count) return;

    for (int i = idx; i < count - 1; ++i) {
        if (addColors_[i] && addColors_[i + 1]) {
            double r, g, b;
            addColors_[i + 1]->getValue(r, g, b);
            addColors_[i]->setValue(r, g, b);
        }
    }

    if (addColors_[count - 1]) addColors_[count - 1]->setIsSecret(true);
    if (delAddBtns_[count - 1]) delAddBtns_[count - 1]->setIsSecret(true);
    setNumAddColors(count - 1);
}

int SecondaryKeyerPlugin::getNumSubColors() const {
    if (!numSubColors_) return 0;
    int v = 0;
    numSubColors_->getValue(v);
    return std::max(0, std::min(v, kMaxListColors));
}

void SecondaryKeyerPlugin::setNumSubColors(int count) {
    if (numSubColors_) {
        numSubColors_->setValue(std::max(0, std::min(count, kMaxListColors)));
    }
}

void SecondaryKeyerPlugin::removeSubColor(int idx) {
    int count = getNumSubColors();
    if (idx < 0 || idx >= count) return;

    for (int i = idx; i < count - 1; ++i) {
        if (subColors_[i] && subColors_[i + 1]) {
            double r, g, b;
            subColors_[i + 1]->getValue(r, g, b);
            subColors_[i]->setValue(r, g, b);
        }
    }

    if (subColors_[count - 1]) subColors_[count - 1]->setIsSecret(true);
    if (delSubBtns_[count - 1]) delSubBtns_[count - 1]->setIsSecret(true);
    setNumSubColors(count - 1);
}

void SecondaryKeyerPlugin::syncListVisibility() {
    int addCount = getNumAddColors();
    for (int i = 0; i < kMaxListColors; ++i) {
        bool visible = (i < addCount);
        if (addColors_[i]) addColors_[i]->setIsSecret(!visible);
        if (delAddBtns_[i]) delAddBtns_[i]->setIsSecret(!visible);
    }

    int subCount = getNumSubColors();
    for (int i = 0; i < kMaxListColors; ++i) {
        bool visible = (i < subCount);
        if (subColors_[i]) subColors_[i]->setIsSecret(!visible);
        if (delSubBtns_[i]) delSubBtns_[i]->setIsSecret(!visible);
    }
}

void SecondaryKeyerPlugin::changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName) {
    // Continuous selection or button click: Add Colors
    if (paramName == kParamBtnAddColor ||
        (paramName == kParamPickAddColor && continuousAdd_ && args.reason == OFX::eChangeUserEdit)) {
        bool continuous = false;
        if (continuousAdd_) continuousAdd_->getValue(continuous);

        if (paramName == kParamBtnAddColor || continuous) {
            double r, g, b;
            pickAddColor_->getValueAtTime(args.time, r, g, b);

            int count = getNumAddColors();
            bool isDuplicate = false;
            if (count > 0 && addColors_[count - 1]) {
                double pr, pg, pb;
                addColors_[count - 1]->getValue(pr, pg, pb);
                double dr = r - pr, dg = g - pg, db = b - pb;
                if (dr * dr + dg * dg + db * db < 0.0004) {
                    isDuplicate = true;
                }
            }

            if (!isDuplicate && count < kMaxListColors) {
                if (addColors_[count]) {
                    addColors_[count]->setValue(r, g, b);
                    addColors_[count]->setIsSecret(false);
                }
                if (delAddBtns_[count]) {
                    delAddBtns_[count]->setIsSecret(false);
                }
                setNumAddColors(count + 1);
            }
        }
    } else if (paramName == kParamBtnClearAdd) {
        for (int i = 0; i < kMaxListColors; ++i) {
            if (addColors_[i]) addColors_[i]->setIsSecret(true);
            if (delAddBtns_[i]) delAddBtns_[i]->setIsSecret(true);
        }
        setNumAddColors(0);
    } else if (paramName.rfind(kParamPrefixDelAdd, 0) == 0) {
        try {
            int idx = std::stoi(paramName.substr(std::string(kParamPrefixDelAdd).length()));
            removeAddColor(idx);
        } catch (...) {}
    }

    // Continuous selection or button click: Sub Colors
    if (paramName == kParamBtnSubColor ||
        (paramName == kParamPickSubColor && continuousSub_ && args.reason == OFX::eChangeUserEdit)) {
        bool continuous = false;
        if (continuousSub_) continuousSub_->getValue(continuous);

        if (paramName == kParamBtnSubColor || continuous) {
            double r, g, b;
            pickSubColor_->getValueAtTime(args.time, r, g, b);

            int count = getNumSubColors();
            bool isDuplicate = false;
            if (count > 0 && subColors_[count - 1]) {
                double pr, pg, pb;
                subColors_[count - 1]->getValue(pr, pg, pb);
                double dr = r - pr, dg = g - pg, db = b - pb;
                if (dr * dr + dg * dg + db * db < 0.0004) {
                    isDuplicate = true;
                }
            }

            if (!isDuplicate && count < kMaxListColors) {
                if (subColors_[count]) {
                    subColors_[count]->setValue(r, g, b);
                    subColors_[count]->setIsSecret(false);
                }
                if (delSubBtns_[count]) {
                    delSubBtns_[count]->setIsSecret(false);
                }
                setNumSubColors(count + 1);
            }
        }
    } else if (paramName == kParamBtnClearSub) {
        for (int i = 0; i < kMaxListColors; ++i) {
            if (subColors_[i]) subColors_[i]->setIsSecret(true);
            if (delSubBtns_[i]) delSubBtns_[i]->setIsSecret(true);
        }
        setNumSubColors(0);
    } else if (paramName.rfind(kParamPrefixDelSub, 0) == 0) {
        try {
            int idx = std::stoi(paramName.substr(std::string(kParamPrefixDelSub).length()));
            removeSubColor(idx);
        } catch (...) {}
    }
}

double SecondaryKeyerPlugin::calculateSpatialPadding(double time) const {
    double shrink = matteShrink_ ? matteShrink_->getValueAtTime(time) : 0.0;
    double grow   = matteGrow_   ? matteGrow_->getValueAtTime(time)   : 0.0;
    double blur   = matteBlur_   ? matteBlur_->getValueAtTime(time)   : 0.0;

    double padShrink = std::max(0.0, std::ceil(shrink));
    double padGrow   = std::max(0.0, std::ceil(grow));
    double padBlur   = std::max(0.0, std::ceil(blur * 2.0));

    return padShrink + padGrow + padBlur;
}

KeyerParams SecondaryKeyerPlugin::fetchParams(double time) const {
    KeyerParams p{};

    if (colorSpace_) {
        int cs = 0;
        colorSpace_->getValueAtTime(time, cs);
        p.colorSpace = static_cast<ColorSpaceEnum>(cs);
    }
    if (falloffCurve_) {
        int fc = 0;
        falloffCurve_->getValueAtTime(time, fc);
        p.falloffCurve = static_cast<FalloffCurveEnum>(fc);
    }
    if (filterType_) {
        int ft = 0;
        filterType_->getValueAtTime(time, ft);
        p.filterType = static_cast<FilterTypeEnum>(ft);
    }

    if (hsvPick_) {
        double r, g, b;
        hsvPick_->getValueAtTime(time, r, g, b);
        p.basePick.r = static_cast<float>(r);
        p.basePick.g = static_cast<float>(g);
        p.basePick.b = static_cast<float>(b);
    }

    // Legacy Single Add/Sub
    if (enableAdd_) {
        bool v;
        enableAdd_->getValueAtTime(time, v);
        p.enableLegacyAdd = v;
    }
    if (colorAdd_) {
        double r, g, b;
        colorAdd_->getValueAtTime(time, r, g, b);
        p.legacyAdd.r = static_cast<float>(r);
        p.legacyAdd.g = static_cast<float>(g);
        p.legacyAdd.b = static_cast<float>(b);
    }
    if (enableSub_) {
        bool v;
        enableSub_->getValueAtTime(time, v);
        p.enableLegacySub = v;
    }
    if (colorSub_) {
        double r, g, b;
        colorSub_->getValueAtTime(time, r, g, b);
        p.legacySub.r = static_cast<float>(r);
        p.legacySub.g = static_cast<float>(g);
        p.legacySub.b = static_cast<float>(b);
    }

    // Dynamic Multi-Color Lists
    int addCount = getNumAddColors();
    for (int i = 0; i < addCount; ++i) {
        if (addColors_[i]) {
            double r, g, b;
            addColors_[i]->getValueAtTime(time, r, g, b);
            p.addColors.push_back({static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)});
        }
    }

    int subCount = getNumSubColors();
    for (int i = 0; i < subCount; ++i) {
        if (subColors_[i]) {
            double r, g, b;
            subColors_[i]->getValueAtTime(time, r, g, b);
            p.subColors.push_back({static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)});
        }
    }

    if (hueSpread_)  p.spread1  = static_cast<float>(hueSpread_->getValueAtTime(time));
    if (hueFalloff_) p.falloff1 = static_cast<float>(std::max(0.001, hueFalloff_->getValueAtTime(time)));
    if (satSpread_)  p.spread2  = static_cast<float>(satSpread_->getValueAtTime(time));
    if (satFalloff_) p.falloff2 = static_cast<float>(std::max(0.001, satFalloff_->getValueAtTime(time)));
    if (valSpread_)  p.spread3  = static_cast<float>(valSpread_->getValueAtTime(time));
    if (valFalloff_) p.falloff3 = static_cast<float>(std::max(0.001, valFalloff_->getValueAtTime(time)));

    if (matteShrink_) p.matteShrink = static_cast<float>(std::max(0.0, matteShrink_->getValueAtTime(time)));
    if (matteGrow_)   p.matteGrow   = static_cast<float>(std::max(0.0, matteGrow_->getValueAtTime(time)));
    if (matteBlur_)   p.matteBlur   = static_cast<float>(std::max(0.0, matteBlur_->getValueAtTime(time)));

    if (outputMode_) {
        int m = 0;
        outputMode_->getValueAtTime(time, m);
        p.outputMode = static_cast<OutputModeEnum>(m);
    }

    return p;
}

bool SecondaryKeyerPlugin::getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod) {
    if (!srcClip_) return false;

    OfxRectD srcRoD = srcClip_->getRegionOfDefinition(args.time);
    double padding = calculateSpatialPadding(args.time);

    rod.x1 = srcRoD.x1 - padding;
    rod.y1 = srcRoD.y1 - padding;
    rod.x2 = srcRoD.x2 + padding;
    rod.y2 = srcRoD.y2 + padding;

    return true;
}

void SecondaryKeyerPlugin::getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args,
                                                OFX::RegionOfInterestSetter &rois) {
    if (!srcClip_) return;

    double padding = calculateSpatialPadding(args.time);
    OfxRectD srcRoI;
    srcRoI.x1 = args.regionOfInterest.x1 - padding;
    srcRoI.y1 = args.regionOfInterest.y1 - padding;
    srcRoI.x2 = args.regionOfInterest.x2 + padding;
    srcRoI.y2 = args.regionOfInterest.y2 + padding;

    rois.setRegionOfInterest(*srcClip_, srcRoI);
}

bool SecondaryKeyerPlugin::isIdentity(const OFX::IsIdentityArguments &/*args*/,
                                      OFX::Clip *&/*identityClip*/,
                                      double &/*identityTime*/) {
    return false;
}

void SecondaryKeyerPlugin::render(const OFX::RenderArguments &args) {
    if (!dstClip_ || !srcClip_) {
        return;
    }

    // Enforce 32-bit floating-point RGBA
    if (dstClip_->getPixelDepth() != OFX::eBitDepthFloat ||
        dstClip_->getPixelComponents() != OFX::ePixelComponentRGBA) {
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }

    std::unique_ptr<OFX::Image> dst(dstClip_->fetchImage(args.time));
    if (!dst) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }

    std::unique_ptr<OFX::Image> src(srcClip_->fetchImage(args.time));
    if (!src) {
        // Output clear/transparent black if no source available
        for (int y = args.renderWindow.y1; y < args.renderWindow.y2; ++y) {
            float *dstPix = static_cast<float*>(dst->getPixelAddress(args.renderWindow.x1, y));
            if (dstPix) {
                int count = (args.renderWindow.x2 - args.renderWindow.x1) * 4;
                std::fill(dstPix, dstPix + count, 0.0f);
            }
        }
        return;
    }

    KeyerParams params = fetchParams(args.time);

    // Instantiate and execute multi-threaded spatial processing engine
    SecondaryKeyerProcessor processor(*this, src.get(), dst.get(), args.renderWindow, params);
    processor.process();
}

////////////////////////////////////////////////////////////////////////////////
// SecondaryKeyerFactory Implementation
////////////////////////////////////////////////////////////////////////////////

SecondaryKeyerFactory::SecondaryKeyerFactory()
    : OFX::PluginFactoryHelper<SecondaryKeyerFactory>(kPluginIdentifier,
                                                      kPluginVersionMajor,
                                                      kPluginVersionMinor)
{
}

void SecondaryKeyerFactory::describe(OFX::ImageEffectDescriptor &desc) {
    desc.setLabels("LibreSecondary", "LibreSecondary", "Libre Secondary Keyer");
    desc.setPluginGrouping("Color/Keyer");
    desc.setPluginDescription("Multi-space (HSV, HSL, YCbCr) secondary qualifier with continuous color picking and finesse filtering.");

    // Support Contexts: Filter and General
    desc.addSupportedContext(OFX::eContextFilter);
    desc.addSupportedContext(OFX::eContextGeneral);

    // Support Pixel Formats: 32-bit float RGBA
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);

    // Plugin capabilities
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(true);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
}

void SecondaryKeyerFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum /*context*/) {
    // 1. Clips
    OFX::ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(OFX::ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(true);
    srcClip->setIsMask(false);

    OFX::ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(OFX::ePixelComponentRGBA);
    dstClip->setSupportsTiles(true);

    // 2. Parameters Page
    OFX::PageParamDescriptor *page = desc.definePageParam("Controls");

    // Color Space Selection
    OFX::ChoiceParamDescriptor *colorSpace = desc.defineChoiceParam(kParamColorSpace);
    colorSpace->setLabels("Color Space", "Color Space", "Qualification color space");
    colorSpace->setHint("Choose operating space: HSV, HSL, or broadcast YCbCr (Rec. 709)");
    colorSpace->appendOption("HSV (Hue, Saturation, Value)");
    colorSpace->appendOption("HSL (Hue, Saturation, Lightness)");
    colorSpace->appendOption("YCbCr (Rec. 709 Luma & Chroma)");
    colorSpace->setDefault(0);
    colorSpace->setAnimates(true);
    page->addChild(*colorSpace);

    // Falloff Curve Selection (4 best curves)
    OFX::ChoiceParamDescriptor *falloffCurve = desc.defineChoiceParam(kParamFalloffCurve);
    falloffCurve->setLabels("Falloff Curve", "Falloff Curve", "Transition falloff profile");
    falloffCurve->setHint("Select the mathematical curve used for transition roll-off");
    falloffCurve->appendOption("Smoothstep (Hermite Cubic)");
    falloffCurve->appendOption("Smootherstep (Perlin Quintic)");
    falloffCurve->appendOption("Cosine (Filmic S-Curve)");
    falloffCurve->appendOption("Linear (Crisp Slope)");
    falloffCurve->setDefault(0);
    falloffCurve->setAnimates(true);
    page->addChild(*falloffCurve);

    // Base Color Picker
    OFX::RGBParamDescriptor *hsvPick = desc.defineRGBParam(kParamHsvPick);
    hsvPick->setLabels("Pick Base Color", "Pick Color", "Base color to qualify");
    hsvPick->setHint("Target primary base color to isolate");
    hsvPick->setDefault(0.0, 0.0, 0.0);
    hsvPick->setAnimates(true);
    page->addChild(*hsvPick);

    // -------------------------------------------------------------------------
    // Add Colors (Expand Matte) Group - Spins up under an arrow
    // -------------------------------------------------------------------------
    OFX::GroupParamDescriptor *grpAdd = desc.defineGroupParam(kParamGrpAddColors);
    grpAdd->setLabels("Add Colors (Expand Matte)", "Add Colors", "Additional qualified colors list");
    grpAdd->setHint("List of colors to add/union into the qualified matte");
    grpAdd->setOpen(false); // Spins up (collapsed) under an arrow by default
    page->addChild(*grpAdd);

    OFX::RGBParamDescriptor *pickAdd = desc.defineRGBParam(kParamPickAddColor);
    pickAdd->setLabels("Sample Add Color", "Sample Add", "Eye-dropper for adding colors");
    pickAdd->setHint("Sample with the eye-dropper to add colors continuously or via button");
    pickAdd->setDefault(0.0, 0.0, 0.0);
    pickAdd->setParent(*grpAdd);
    page->addChild(*pickAdd);

    OFX::BooleanParamDescriptor *contAdd = desc.defineBooleanParam(kParamContinuousAdd);
    contAdd->setLabels("Continuous Selection Mode", "Continuous", "Auto-add on eye-dropper sampling");
    contAdd->setHint("Automatically add new sampled colors to the list when picking footage");
    contAdd->setDefault(true);
    contAdd->setParent(*grpAdd);
    page->addChild(*contAdd);

    OFX::PushButtonParamDescriptor *btnAdd = desc.definePushButtonParam(kParamBtnAddColor);
    btnAdd->setLabels("+ Add Sampled Color", "+ Add Color", "Append sampled color to list");
    btnAdd->setParent(*grpAdd);
    page->addChild(*btnAdd);

    OFX::PushButtonParamDescriptor *btnClearAdd = desc.definePushButtonParam(kParamBtnClearAdd);
    btnClearAdd->setLabels("Clear All Add Colors", "Clear All", "Clear the entire add colors list");
    btnClearAdd->setParent(*grpAdd);
    page->addChild(*btnClearAdd);

    OFX::IntParamDescriptor *numAdd = desc.defineIntParam(kParamNumAddColors);
    numAdd->setDefault(0);
    numAdd->setIsSecret(true); // Persists count in script
    numAdd->setParent(*grpAdd);
    page->addChild(*numAdd);

    for (int i = 0; i < kMaxListColors; ++i) {
        std::string nameColor = std::string(kParamPrefixAddColor) + std::to_string(i);
        std::string labelColor = "Color " + std::to_string(i + 1);
        OFX::RGBParamDescriptor *slot = desc.defineRGBParam(nameColor);
        slot->setLabels(labelColor, labelColor, labelColor);
        slot->setDefault(0.0, 0.0, 0.0);
        slot->setIsSecret(true);
        slot->setParent(*grpAdd);
        page->addChild(*slot);

        std::string nameDel = std::string(kParamPrefixDelAdd) + std::to_string(i);
        std::string labelDel = "[X] Delete " + std::to_string(i + 1);
        OFX::PushButtonParamDescriptor *delBtn = desc.definePushButtonParam(nameDel);
        delBtn->setLabels(labelDel, labelDel, labelDel);
        delBtn->setIsSecret(true);
        delBtn->setParent(*grpAdd);
        page->addChild(*delBtn);
    }

    // -------------------------------------------------------------------------
    // Sub Colors (Exclude from Matte) Group - Spins up under an arrow
    // -------------------------------------------------------------------------
    OFX::GroupParamDescriptor *grpSub = desc.defineGroupParam(kParamGrpSubColors);
    grpSub->setLabels("Sub Colors (Exclude from Matte)", "Sub Colors", "Subtracted qualified colors list");
    grpSub->setHint("List of colors to subtract/carve out from the qualified matte");
    grpSub->setOpen(false); // Spins up (collapsed) under an arrow by default
    page->addChild(*grpSub);

    OFX::RGBParamDescriptor *pickSub = desc.defineRGBParam(kParamPickSubColor);
    pickSub->setLabels("Sample Sub Color", "Sample Sub", "Eye-dropper for subtracting colors");
    pickSub->setHint("Sample with the eye-dropper to subtract colors continuously or via button");
    pickSub->setDefault(0.0, 0.0, 0.0);
    pickSub->setParent(*grpSub);
    page->addChild(*pickSub);

    OFX::BooleanParamDescriptor *contSub = desc.defineBooleanParam(kParamContinuousSub);
    contSub->setLabels("Continuous Selection Mode", "Continuous", "Auto-subtract on eye-dropper sampling");
    contSub->setHint("Automatically add new sampled colors to the sub list when picking footage");
    contSub->setDefault(true);
    contSub->setParent(*grpSub);
    page->addChild(*contSub);

    OFX::PushButtonParamDescriptor *btnSub = desc.definePushButtonParam(kParamBtnSubColor);
    btnSub->setLabels("+ Add Sampled Sub Color", "+ Add Sub Color", "Append sampled sub color to list");
    btnSub->setParent(*grpSub);
    page->addChild(*btnSub);

    OFX::PushButtonParamDescriptor *btnClearSub = desc.definePushButtonParam(kParamBtnClearSub);
    btnClearSub->setLabels("Clear All Sub Colors", "Clear All", "Clear the entire sub colors list");
    btnClearSub->setParent(*grpSub);
    page->addChild(*btnClearSub);

    OFX::IntParamDescriptor *numSub = desc.defineIntParam(kParamNumSubColors);
    numSub->setDefault(0);
    numSub->setIsSecret(true);
    numSub->setParent(*grpSub);
    page->addChild(*numSub);

    for (int i = 0; i < kMaxListColors; ++i) {
        std::string nameColor = std::string(kParamPrefixSubColor) + std::to_string(i);
        std::string labelColor = "Sub " + std::to_string(i + 1);
        OFX::RGBParamDescriptor *slot = desc.defineRGBParam(nameColor);
        slot->setLabels(labelColor, labelColor, labelColor);
        slot->setDefault(0.0, 0.0, 0.0);
        slot->setIsSecret(true);
        slot->setParent(*grpSub);
        page->addChild(*slot);

        std::string nameDel = std::string(kParamPrefixDelSub) + std::to_string(i);
        std::string labelDel = "[X] Delete " + std::to_string(i + 1);
        OFX::PushButtonParamDescriptor *delBtn = desc.definePushButtonParam(nameDel);
        delBtn->setLabels(labelDel, labelDel, labelDel);
        delBtn->setIsSecret(true);
        delBtn->setParent(*grpSub);
        page->addChild(*delBtn);
    }

    // -------------------------------------------------------------------------
    // Tolerance Controls (Ch1 Hue/Y, Ch2 Sat/Cb, Ch3 Val/Cr)
    // -------------------------------------------------------------------------
    OFX::DoubleParamDescriptor *hueSpread = desc.defineDoubleParam(kParamHueSpread);
    hueSpread->setLabels("Hue / Y Spread", "Hue Spread", "Channel 1 tolerance plateau");
    hueSpread->setRange(0.0, 0.5);
    hueSpread->setDisplayRange(0.0, 0.5);
    hueSpread->setDefault(0.1);
    hueSpread->setIncrement(0.01);
    hueSpread->setAnimates(true);
    page->addChild(*hueSpread);

    OFX::DoubleParamDescriptor *hueFalloff = desc.defineDoubleParam(kParamHueFalloff);
    hueFalloff->setLabels("Hue / Y Falloff", "Hue Falloff", "Channel 1 softness transition");
    hueFalloff->setRange(0.001, 0.5);
    hueFalloff->setDisplayRange(0.001, 0.5);
    hueFalloff->setDefault(0.05);
    hueFalloff->setIncrement(0.01);
    hueFalloff->setAnimates(true);
    page->addChild(*hueFalloff);

    OFX::DoubleParamDescriptor *satSpread = desc.defineDoubleParam(kParamSatSpread);
    satSpread->setLabels("Sat / Cb Spread", "Sat Spread", "Channel 2 tolerance plateau");
    satSpread->setRange(0.0, 1.0);
    satSpread->setDisplayRange(0.0, 1.0);
    satSpread->setDefault(0.2);
    satSpread->setIncrement(0.01);
    satSpread->setAnimates(true);
    page->addChild(*satSpread);

    OFX::DoubleParamDescriptor *satFalloff = desc.defineDoubleParam(kParamSatFalloff);
    satFalloff->setLabels("Sat / Cb Falloff", "Sat Falloff", "Channel 2 softness transition");
    satFalloff->setRange(0.001, 1.0);
    satFalloff->setDisplayRange(0.001, 1.0);
    satFalloff->setDefault(0.1);
    satFalloff->setIncrement(0.01);
    satFalloff->setAnimates(true);
    page->addChild(*satFalloff);

    OFX::DoubleParamDescriptor *valSpread = desc.defineDoubleParam(kParamValSpread);
    valSpread->setLabels("Val / Cr Spread", "Val Spread", "Channel 3 tolerance plateau");
    valSpread->setRange(0.0, 1.0);
    valSpread->setDisplayRange(0.0, 1.0);
    valSpread->setDefault(0.2);
    valSpread->setIncrement(0.01);
    valSpread->setAnimates(true);
    page->addChild(*valSpread);

    OFX::DoubleParamDescriptor *valFalloff = desc.defineDoubleParam(kParamValFalloff);
    valFalloff->setLabels("Val / Cr Falloff", "Val Falloff", "Channel 3 softness transition");
    valFalloff->setRange(0.001, 1.0);
    valFalloff->setDisplayRange(0.001, 1.0);
    valFalloff->setDefault(0.1);
    valFalloff->setIncrement(0.01);
    valFalloff->setAnimates(true);
    page->addChild(*valFalloff);

    // -------------------------------------------------------------------------
    // Matte Finesse Controls
    // -------------------------------------------------------------------------
    OFX::DoubleParamDescriptor *matteShrink = desc.defineDoubleParam(kParamMatteShrink);
    matteShrink->setLabels("Matte Shrink", "Matte Shrink", "Circular erosion radius (pixels)");
    matteShrink->setRange(0.0, 50.0);
    matteShrink->setDisplayRange(0.0, 50.0);
    matteShrink->setDefault(0.0);
    matteShrink->setIncrement(0.5);
    matteShrink->setAnimates(true);
    page->addChild(*matteShrink);

    OFX::DoubleParamDescriptor *matteGrow = desc.defineDoubleParam(kParamMatteGrow);
    matteGrow->setLabels("Matte Grow", "Matte Grow", "Circular dilation radius (pixels)");
    matteGrow->setRange(0.0, 50.0);
    matteGrow->setDisplayRange(0.0, 50.0);
    matteGrow->setDefault(0.0);
    matteGrow->setIncrement(0.5);
    matteGrow->setAnimates(true);
    page->addChild(*matteGrow);

    OFX::DoubleParamDescriptor *matteBlur = desc.defineDoubleParam(kParamMatteBlur);
    matteBlur->setLabels("Matte Blur", "Matte Blur", "Blur radius (pixels)");
    matteBlur->setRange(0.0, 100.0);
    matteBlur->setDisplayRange(0.0, 100.0);
    matteBlur->setDefault(0.0);
    matteBlur->setIncrement(0.5);
    matteBlur->setAnimates(true);
    page->addChild(*matteBlur);

    // Spatial Filter Type (4 best filters)
    OFX::ChoiceParamDescriptor *filterType = desc.defineChoiceParam(kParamFilterType);
    filterType->setLabels("Blur Filter Type", "Filter Type", "Spatial convolution kernel");
    filterType->setHint("Select the spatial blur convolution kernel type");
    filterType->appendOption("Gaussian (Separable Optical)");
    filterType->appendOption("Box Blur (Uniform Window)");
    filterType->appendOption("Triangle (Tent Linear)");
    filterType->appendOption("Exponential (Decay Tail)");
    filterType->setDefault(0);
    filterType->setAnimates(true);
    page->addChild(*filterType);

    // -------------------------------------------------------------------------
    // Output Mode Choice
    // -------------------------------------------------------------------------
    OFX::ChoiceParamDescriptor *outputMode = desc.defineChoiceParam(kParamOutputMode);
    outputMode->setLabels("Output Mode", "Output Mode", "Render mode selection");
    outputMode->appendOption("Premultiplied RGBA");
    outputMode->appendOption("Alpha Matte Only");
    outputMode->setDefault(0);
    outputMode->setAnimates(true);
    page->addChild(*outputMode);

    // -------------------------------------------------------------------------
    // Legacy Single Add/Sub (hidden by default for backward compatibility)
    // -------------------------------------------------------------------------
    OFX::BooleanParamDescriptor *enableAdd = desc.defineBooleanParam(kParamEnableAdd);
    enableAdd->setDefault(false);
    enableAdd->setIsSecret(true);
    page->addChild(*enableAdd);

    OFX::RGBParamDescriptor *colorAdd = desc.defineRGBParam(kParamColorAdd);
    colorAdd->setDefault(0.0, 0.0, 0.0);
    colorAdd->setIsSecret(true);
    page->addChild(*colorAdd);

    OFX::BooleanParamDescriptor *enableSub = desc.defineBooleanParam(kParamEnableSub);
    enableSub->setDefault(false);
    enableSub->setIsSecret(true);
    page->addChild(*enableSub);

    OFX::RGBParamDescriptor *colorSub = desc.defineRGBParam(kParamColorSub);
    colorSub->setDefault(0.0, 0.0, 0.0);
    colorSub->setIsSecret(true);
    page->addChild(*colorSub);
}

OFX::ImageEffect* SecondaryKeyerFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum /*context*/) {
    return new SecondaryKeyerPlugin(handle);
}

} // namespace LibreSecondary

////////////////////////////////////////////////////////////////////////////////
// OpenFX Plugin Registration Entrypoint
////////////////////////////////////////////////////////////////////////////////

namespace OFX {
namespace Plugin {

void getPluginIDs(OFX::PluginFactoryArray &ids) {
    static LibreSecondary::SecondaryKeyerFactory p;
    ids.push_back(&p);
}

} // namespace Plugin
} // namespace OFX

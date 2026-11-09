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

// Helper: Standard Hermite smoothstep interpolation
inline float smoothstep(float edge0, float edge1, float x) {
    if (edge1 <= edge0) {
        return (x >= edge0) ? 1.0f : 0.0f;
    }
    if (x <= edge0) return 0.0f;
    if (x >= edge1) return 1.0f;
    float t = (x - edge0) / (edge1 - edge0);
    return t * t * (3.0f - 2.0f * t);
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

    // Convert target colors to HSV space
    rgb2hsv(_params.pickR, _params.pickG, _params.pickB, _pickH, _pickS, _pickV);
    if (_params.enableAdd) {
        rgb2hsv(_params.addR, _params.addG, _params.addB, _addH, _addS, _addV);
    }
    if (_params.enableSub) {
        rgb2hsv(_params.subR, _params.subG, _params.subB, _subH, _subS, _subV);
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

            // Precalculate 1D Gaussian kernel
            float sigma = std::max(0.1f, _params.matteBlur);
            int kRadius = _blurRadius;
            _blurKernel.resize(2 * kRadius + 1);
            float sumW = 0.0f;
            float twoSigmaSq = 2.0f * sigma * sigma;

            for (int i = -kRadius; i <= kRadius; ++i) {
                float w = std::exp(-static_cast<float>(i * i) / twoSigmaSq);
                _blurKernel[i + kRadius] = w;
                sumW += w;
            }
            if (sumW > 1e-6f) {
                for (float &w : _blurKernel) {
                    w /= sumW;
                }
            }
        }
    }
}

void SecondaryKeyerProcessor::process() {
    int rShrink = static_cast<int>(std::ceil(_params.matteShrink));
    int rGrow   = static_cast<int>(std::ceil(_params.matteGrow));
    int rBlur   = static_cast<int>(std::ceil(_params.matteBlur * 2.0f));

    bool needsSpatial = (rShrink > 0 || rGrow > 0 || rBlur > 0);

    if (!needsSpatial) {
        // Fast direct pass: compute raw qualifier and composite straight to destination
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

    // Pass 4 & 5: Separable Gaussian Blur (Horizontal & Vertical convolution)
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

float SecondaryKeyerProcessor::computeMatte(float h, float s, float v,
                                            float targetH, float targetS, float targetV) const {
    // Hue circular distance on [0, 1)
    float dh = std::fabs(h - targetH);
    if (dh > 0.5f) {
        dh = 1.0f - dh;
    }
    float ds = std::fabs(s - targetS);
    float dv = std::fabs(v - targetV);

    // Hermite smoothstep keying curves
    float mH = 1.0f - smoothstep(_params.hueSpread, _params.hueSpread + _params.hueFalloff, dh);
    float mS = 1.0f - smoothstep(_params.satSpread, _params.satSpread + _params.satFalloff, ds);
    float mV = 1.0f - smoothstep(_params.valSpread, _params.valSpread + _params.valFalloff, dv);

    return mH * mS * mV;
}

float SecondaryKeyerProcessor::evaluateMatte(float r, float g, float b) const {
    float h, s, v;
    rgb2hsv(r, g, b, h, s, v);

    float baseMatte = computeMatte(h, s, v, _pickH, _pickS, _pickV);
    float combined = baseMatte;

    if (_params.enableAdd) {
        float addMatte = computeMatte(h, s, v, _addH, _addS, _addV);
        combined = std::max(combined, addMatte);
    }

    if (_params.enableSub) {
        float subMatte = computeMatte(h, s, v, _subH, _subS, _subV);
        combined = std::max(0.0f, combined - subMatte);
    }

    return std::max(0.0f, std::min(combined, 1.0f));
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

    // Fetch parameter handles
    hsvPick_   = fetchRGBParam(kParamHsvPick);
    enableAdd_ = fetchBooleanParam(kParamEnableAdd);
    colorAdd_  = fetchRGBParam(kParamColorAdd);
    enableSub_ = fetchBooleanParam(kParamEnableSub);
    colorSub_  = fetchRGBParam(kParamColorSub);

    hueSpread_  = fetchDoubleParam(kParamHueSpread);
    hueFalloff_ = fetchDoubleParam(kParamHueFalloff);
    satSpread_  = fetchDoubleParam(kParamSatSpread);
    satFalloff_ = fetchDoubleParam(kParamSatFalloff);
    valSpread_  = fetchDoubleParam(kParamValSpread);
    valFalloff_ = fetchDoubleParam(kParamValFalloff);

    matteShrink_ = fetchDoubleParam(kParamMatteShrink);
    matteGrow_   = fetchDoubleParam(kParamMatteGrow);
    matteBlur_   = fetchDoubleParam(kParamMatteBlur);

    outputMode_ = fetchChoiceParam(kParamOutputMode);
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

    if (hsvPick_) {
        double r, g, b;
        hsvPick_->getValueAtTime(time, r, g, b);
        p.pickR = static_cast<float>(r);
        p.pickG = static_cast<float>(g);
        p.pickB = static_cast<float>(b);
    }
    if (enableAdd_) {
        bool v;
        enableAdd_->getValueAtTime(time, v);
        p.enableAdd = v;
    }
    if (colorAdd_) {
        double r, g, b;
        colorAdd_->getValueAtTime(time, r, g, b);
        p.addR = static_cast<float>(r);
        p.addG = static_cast<float>(g);
        p.addB = static_cast<float>(b);
    }
    if (enableSub_) {
        bool v;
        enableSub_->getValueAtTime(time, v);
        p.enableSub = v;
    }
    if (colorSub_) {
        double r, g, b;
        colorSub_->getValueAtTime(time, r, g, b);
        p.subR = static_cast<float>(r);
        p.subG = static_cast<float>(g);
        p.subB = static_cast<float>(b);
    }

    if (hueSpread_)  p.hueSpread  = static_cast<float>(hueSpread_->getValueAtTime(time));
    if (hueFalloff_) p.hueFalloff = static_cast<float>(std::max(0.001, hueFalloff_->getValueAtTime(time)));
    if (satSpread_)  p.satSpread  = static_cast<float>(satSpread_->getValueAtTime(time));
    if (satFalloff_) p.satFalloff = static_cast<float>(std::max(0.001, satFalloff_->getValueAtTime(time)));
    if (valSpread_)  p.valSpread  = static_cast<float>(valSpread_->getValueAtTime(time));
    if (valFalloff_) p.valFalloff = static_cast<float>(std::max(0.001, valFalloff_->getValueAtTime(time)));

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
    desc.setPluginDescription("High-performance DaVinci Resolve-style secondary qualifier for Foundry Nuke.");

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

    // HSV Base Color Picker
    OFX::RGBParamDescriptor *hsvPick = desc.defineRGBParam(kParamHsvPick);
    hsvPick->setLabels("Pick Color", "Pick Color", "Pick base color");
    hsvPick->setHint("Target base color to isolate using HSV qualification");
    hsvPick->setDefault(0.0, 0.0, 0.0);
    hsvPick->setAnimates(true);
    page->addChild(*hsvPick);

    // Additional Qualified Color
    OFX::BooleanParamDescriptor *enableAdd = desc.defineBooleanParam(kParamEnableAdd);
    enableAdd->setLabels("Enable Add", "Enable Add", "Enable add color");
    enableAdd->setHint("Include a secondary color in the qualified matte");
    enableAdd->setDefault(false);
    enableAdd->setAnimates(true);
    page->addChild(*enableAdd);

    OFX::RGBParamDescriptor *colorAdd = desc.defineRGBParam(kParamColorAdd);
    colorAdd->setLabels("Color Add", "Color Add", "Additional color");
    colorAdd->setHint("Color to add to the qualified matte");
    colorAdd->setDefault(0.0, 0.0, 0.0);
    colorAdd->setAnimates(true);
    page->addChild(*colorAdd);

    // Subtracted Qualified Color
    OFX::BooleanParamDescriptor *enableSub = desc.defineBooleanParam(kParamEnableSub);
    enableSub->setLabels("Enable Sub", "Enable Sub", "Enable subtract color");
    enableSub->setHint("Exclude a color range from the qualified matte");
    enableSub->setDefault(false);
    enableSub->setAnimates(true);
    page->addChild(*enableSub);

    OFX::RGBParamDescriptor *colorSub = desc.defineRGBParam(kParamColorSub);
    colorSub->setLabels("Color Sub", "Color Sub", "Subtracted color");
    colorSub->setHint("Color to remove from the qualified matte");
    colorSub->setDefault(0.0, 0.0, 0.0);
    colorSub->setAnimates(true);
    page->addChild(*colorSub);

    // Hue Controls
    OFX::DoubleParamDescriptor *hueSpread = desc.defineDoubleParam(kParamHueSpread);
    hueSpread->setLabels("Hue Spread", "Hue Spread", "Hue tolerance spread");
    hueSpread->setRange(0.0, 0.5);
    hueSpread->setDisplayRange(0.0, 0.5);
    hueSpread->setDefault(0.1);
    hueSpread->setIncrement(0.01);
    hueSpread->setAnimates(true);
    page->addChild(*hueSpread);

    OFX::DoubleParamDescriptor *hueFalloff = desc.defineDoubleParam(kParamHueFalloff);
    hueFalloff->setLabels("Hue Falloff", "Hue Falloff", "Hue softness falloff");
    hueFalloff->setRange(0.001, 0.5);
    hueFalloff->setDisplayRange(0.001, 0.5);
    hueFalloff->setDefault(0.05);
    hueFalloff->setIncrement(0.01);
    hueFalloff->setAnimates(true);
    page->addChild(*hueFalloff);

    // Saturation Controls
    OFX::DoubleParamDescriptor *satSpread = desc.defineDoubleParam(kParamSatSpread);
    satSpread->setLabels("Sat Spread", "Sat Spread", "Saturation tolerance spread");
    satSpread->setRange(0.0, 1.0);
    satSpread->setDisplayRange(0.0, 1.0);
    satSpread->setDefault(0.2);
    satSpread->setIncrement(0.01);
    satSpread->setAnimates(true);
    page->addChild(*satSpread);

    OFX::DoubleParamDescriptor *satFalloff = desc.defineDoubleParam(kParamSatFalloff);
    satFalloff->setLabels("Sat Falloff", "Sat Falloff", "Saturation softness falloff");
    satFalloff->setRange(0.001, 1.0);
    satFalloff->setDisplayRange(0.001, 1.0);
    satFalloff->setDefault(0.1);
    satFalloff->setIncrement(0.01);
    satFalloff->setAnimates(true);
    page->addChild(*satFalloff);

    // Value Controls
    OFX::DoubleParamDescriptor *valSpread = desc.defineDoubleParam(kParamValSpread);
    valSpread->setLabels("Val Spread", "Val Spread", "Value/Luma tolerance spread");
    valSpread->setRange(0.0, 1.0);
    valSpread->setDisplayRange(0.0, 1.0);
    valSpread->setDefault(0.2);
    valSpread->setIncrement(0.01);
    valSpread->setAnimates(true);
    page->addChild(*valSpread);

    OFX::DoubleParamDescriptor *valFalloff = desc.defineDoubleParam(kParamValFalloff);
    valFalloff->setLabels("Val Falloff", "Val Falloff", "Value/Luma softness falloff");
    valFalloff->setRange(0.001, 1.0);
    valFalloff->setDisplayRange(0.001, 1.0);
    valFalloff->setDefault(0.1);
    valFalloff->setIncrement(0.01);
    valFalloff->setAnimates(true);
    page->addChild(*valFalloff);

    // Matte Finesse (Spatial Morphological & Convolution Ops)
    OFX::DoubleParamDescriptor *matteShrink = desc.defineDoubleParam(kParamMatteShrink);
    matteShrink->setLabels("Matte Shrink", "Matte Shrink", "Erode matte (pixels)");
    matteShrink->setRange(0.0, 50.0);
    matteShrink->setDisplayRange(0.0, 50.0);
    matteShrink->setDefault(0.0);
    matteShrink->setIncrement(0.5);
    matteShrink->setAnimates(true);
    page->addChild(*matteShrink);

    OFX::DoubleParamDescriptor *matteGrow = desc.defineDoubleParam(kParamMatteGrow);
    matteGrow->setLabels("Matte Grow", "Matte Grow", "Dilate matte (pixels)");
    matteGrow->setRange(0.0, 50.0);
    matteGrow->setDisplayRange(0.0, 50.0);
    matteGrow->setDefault(0.0);
    matteGrow->setIncrement(0.5);
    matteGrow->setAnimates(true);
    page->addChild(*matteGrow);

    OFX::DoubleParamDescriptor *matteBlur = desc.defineDoubleParam(kParamMatteBlur);
    matteBlur->setLabels("Matte Blur", "Matte Blur", "Gaussian blur radius (pixels)");
    matteBlur->setRange(0.0, 100.0);
    matteBlur->setDisplayRange(0.0, 100.0);
    matteBlur->setDefault(0.0);
    matteBlur->setIncrement(0.5);
    matteBlur->setAnimates(true);
    page->addChild(*matteBlur);

    // Output Mode Choice
    OFX::ChoiceParamDescriptor *outputMode = desc.defineChoiceParam(kParamOutputMode);
    outputMode->setLabels("Output Mode", "Output Mode", "Render mode selection");
    outputMode->appendOption("Premultiplied RGBA");
    outputMode->appendOption("Alpha Matte Only");
    outputMode->setDefault(0);
    outputMode->setAnimates(true);
    page->addChild(*outputMode);
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

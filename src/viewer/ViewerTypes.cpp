///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — shared types implementation.
///////////////////////////////////////////////////////////////////////////////

#include "ViewerTypes.h"

namespace raygen {
namespace viewer {

bool onlyPostProcessChanged(const ViewerParams& a, const ViewerParams& b) {
    const bool pp_same =
        a.postProcess     == b.postProcess &&
        a.bloomThreshold  == b.bloomThreshold &&
        a.bloomStrength   == b.bloomStrength &&
        a.bloomCurve      == b.bloomCurve &&
        a.bloomRadius     == b.bloomRadius;
    const bool cam_same =
        a.camLocation[0]   == b.camLocation[0] &&
        a.camLocation[1]   == b.camLocation[1] &&
        a.camLocation[2]   == b.camLocation[2] &&
        a.camAngle[0]      == b.camAngle[0] &&
        a.camAngle[1]      == b.camAngle[1] &&
        a.camAngle[2]      == b.camAngle[2] &&
        a.fieldOfView      == b.fieldOfView &&
        a.depthOfField     == b.depthOfField &&
        a.aperture         == b.aperture &&
        a.apertureBlades   == b.apertureBlades &&
        a.apertureRotation == b.apertureRotation;
    const bool medium_same =
        a.mediumEnabled     == b.mediumEnabled &&
        a.mediumSigmaA[0]   == b.mediumSigmaA[0] &&
        a.mediumSigmaA[1]   == b.mediumSigmaA[1] &&
        a.mediumSigmaA[2]   == b.mediumSigmaA[2] &&
        a.mediumSigmaS[0]   == b.mediumSigmaS[0] &&
        a.mediumSigmaS[1]   == b.mediumSigmaS[1] &&
        a.mediumSigmaS[2]   == b.mediumSigmaS[2] &&
        a.mediumEmission[0] == b.mediumEmission[0] &&
        a.mediumEmission[1] == b.mediumEmission[1] &&
        a.mediumEmission[2] == b.mediumEmission[2] &&
        a.mediumG           == b.mediumG &&
        a.mediumDensity     == b.mediumDensity;
    const bool rest_same =
        a.samples            == b.samples &&
        a.threads            == b.threads &&
        a.denoise            == b.denoise &&
        a.denoiseIntensity   == b.denoiseIntensity &&
        a.adaptiveSampling   == b.adaptiveSampling &&
        a.adaptiveBaseSamples == b.adaptiveBaseSamples &&
        a.adaptiveThreshold  == b.adaptiveThreshold &&
        a.exposure           == b.exposure &&
        a.envIntensity       == b.envIntensity &&
        a.envRotation        == b.envRotation &&
        cam_same &&
        medium_same;
    return rest_same && !pp_same;
}

}  // namespace viewer
}  // namespace raygen

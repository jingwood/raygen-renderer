///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — Interior medium editor implementation.
///////////////////////////////////////////////////////////////////////////////

#include "MediumEditor.h"

#include "imgui.h"

#include "raygen/medium.h"
#include "raygen/scene.h"
#include "ugm/color.h"
#include "ugm/vector.h"

namespace raygen {
namespace viewer {

namespace {

// Base optical properties shared by both emission modes: σ_a, σ_s, asymmetry
// parameter g, and overall density multiplier. Returns true on any edit.
bool drawSigmaCommon(HomogeneousMedium& m, float sa[3], float ss[3]) {
    bool changed = false;
    changed |= ImGui::DragFloat3 ("sigma_a##im",  sa, 0.01f, 0.0f, 20.0f, "%.4f");
    changed |= ImGui::DragFloat3 ("sigma_s##im",  ss, 0.01f, 0.0f, 20.0f, "%.4f");
    changed |= ImGui::SliderFloat("g (HG)##im",  &m.g,       -0.95f, 0.95f, "%.2f");
    changed |= ImGui::SliderFloat("density##im", &m.density,  0.0f,  8.0f,  "%.2f");
    return changed;
}

// Emission picker. Constant uses the analytic σ_e integral; Cone evaluates a
// procedural jet-flame profile sampled along the ray. Flipping the mode
// hides/shows the relevant slider set so the panel doesn't sprout dead
// inputs.
bool drawEmissionMode(HomogeneousMedium& m, float se[3]) {
    bool changed = false;

    const char* modeLabels[] = { "Constant", "Cone (jet flame)" };
    int modeIdx = (int)m.emissionMode;
    if (ImGui::Combo("emissionMode##im", &modeIdx, modeLabels, 2)) {
        m.emissionMode = (HomogeneousMedium::EmissionMode)modeIdx;
        changed = true;
    }

    if (m.emissionMode == HomogeneousMedium::EmissionMode_Constant) {
        changed |= ImGui::DragFloat3("emission##im", se, 0.05f, 0.0f, 100.0f, "%.3f");
        return changed;
    }

    // Cone params. With coneFollowObject ON (the default for newly-created
    // media in the viewer), coneOrigin and coneAxis are interpreted in the
    // SceneObject's *local* space — moving the bounding mesh moves the flame.
    // Toggle it off if you authored world-space params in JSON and want to
    // keep that behaviour.
    bool coneChanged = false;
    coneChanged |= ImGui::Checkbox("followObject##im", &m.coneFollowObject);
    ImGui::SameLine();
    ImGui::TextDisabled(m.coneFollowObject ? "(object-local)" : "(world-space)");

    float coAxis[3]   = { m.coneAxis.x,   m.coneAxis.y,   m.coneAxis.z };
    float coOrigin[3] = { m.coneOrigin.x, m.coneOrigin.y, m.coneOrigin.z };
    float coIn[3]     = { m.coneInner.r,  m.coneInner.g,  m.coneInner.b };
    float coOut[3]    = { m.coneOuter.r,  m.coneOuter.g,  m.coneOuter.b };
    coneChanged |= ImGui::DragFloat3 ("coneOrigin##im",        coOrigin, 0.05f, -1000.0f, 1000.0f, "%.3f");
    coneChanged |= ImGui::DragFloat3 ("coneAxis##im",          coAxis,   0.05f, -1.0f, 1.0f, "%.3f");
    coneChanged |= ImGui::SliderFloat("coneLength##im",       &m.coneLength,   0.05f, 20.0f, "%.3f");
    coneChanged |= ImGui::SliderFloat("coneRadius##im",       &m.coneRadius,   0.01f, 5.0f,  "%.3f");
    coneChanged |= ImGui::ColorEdit3 ("coneInner##im",         coIn);
    coneChanged |= ImGui::ColorEdit3 ("coneOuter##im",         coOut);
    coneChanged |= ImGui::DragFloat  ("coneIntensity##im",    &m.coneIntensity,     1.0f, 0.0f, 10000.0f, "%.1f");
    coneChanged |= ImGui::SliderFloat("conePeakAxial##im",    &m.conePeakAxial,     0.0f, 1.0f, "%.3f");
    coneChanged |= ImGui::SliderFloat("conePeakSharpness##im",&m.conePeakSharpness, 0.5f, 20.0f, "%.2f");
    coneChanged |= ImGui::SliderInt  ("emissionSamples##im",  &m.coneEmissionSamples, 1, 32);
    if (coneChanged) {
        m.coneAxis   = ugm::vec3(coAxis[0],   coAxis[1],   coAxis[2]);
        m.coneOrigin = ugm::vec3(coOrigin[0], coOrigin[1], coOrigin[2]);
        m.coneInner  = ugm::color3(coIn[0],   coIn[1],   coIn[2]);
        m.coneOuter  = ugm::color3(coOut[0],  coOut[1],  coOut[2]);
        changed = true;
    }
    return changed;
}

// Phase 3: density field. fBm noise modulates σ_a/σ_s/σ_e at each ray point —
// turns uniform fog into wispy clouds, smooth flames into turbulent ones.
// Authoring tip: noiseBias=-0.2 carves empty pockets ("wisps");
// noiseFrequency sets the world-space scale of detail.
bool drawDensityField(HomogeneousMedium& m) {
    bool changed = false;

    const char* dfLabels[] = { "None", "fBm noise" };
    int dfIdx = (int)m.densityField;
    if (ImGui::Combo("densityField##im", &dfIdx, dfLabels, 2)) {
        m.densityField = (HomogeneousMedium::DensityFieldMode)dfIdx;
        changed = true;
    }

    if (m.densityField != HomogeneousMedium::DensityField_FBmNoise) return changed;

    float noff[3] = { m.noiseOffset.x, m.noiseOffset.y, m.noiseOffset.z };
    bool nChanged = false;
    nChanged |= ImGui::SliderFloat("noiseFrequency##im",  &m.noiseFrequency,  0.05f, 16.0f, "%.3f");
    nChanged |= ImGui::SliderInt  ("noiseOctaves##im",    &m.noiseOctaves,    1, 6);
    nChanged |= ImGui::SliderFloat("noiseGain##im",       &m.noiseGain,       0.0f, 1.0f, "%.3f");
    nChanged |= ImGui::SliderFloat("noiseLacunarity##im", &m.noiseLacunarity, 1.0f, 4.0f, "%.3f");
    nChanged |= ImGui::SliderFloat("noiseAmplitude##im",  &m.noiseAmplitude,  0.0f, 4.0f, "%.3f");
    nChanged |= ImGui::SliderFloat("noiseBias##im",       &m.noiseBias,      -1.0f, 1.0f, "%.3f");
    nChanged |= ImGui::DragFloat3 ("noiseOffset##im",      noff, 0.05f, -1000.0f, 1000.0f, "%.3f");
    if (nChanged) {
        m.noiseOffset = ugm::vec3(noff[0], noff[1], noff[2]);
        changed = true;
    }
    return changed;
}

// Phase 4: heat haze (refractive shimmer / 陽炎). The volume bends rays
// instead of scattering / emitting, so this section unlocks under its own
// checkbox and visually replaces the σ-driven preview when on. Realistic
// hot-air values for iorAmplitude live around 1e-3..5e-3; push 0.05+ for
// stylised distortion. iorFrequency sets the world-scale of the wobble.
bool drawHeatHaze(HomogeneousMedium& m) {
    bool changed = false;
    changed |= ImGui::Checkbox("heatHaze##im", &m.heatHaze);
    if (!m.heatHaze) return changed;

    ImGui::TextDisabled("(σ_a/σ_s/emission ignored on this path)");
    changed |= ImGui::SliderFloat("iorAmplitude##im",  &m.iorAmplitude, 0.0f,  0.1f,  "%.5f");
    changed |= ImGui::SliderFloat("iorFrequency##im",  &m.iorFrequency, 0.05f, 32.0f, "%.3f");
    changed |= ImGui::SliderInt  ("iorOctaves##im",    &m.iorOctaves,   1, 6);
    changed |= ImGui::SliderFloat("iorGain##im",       &m.iorGain,      0.0f, 1.0f, "%.3f");
    changed |= ImGui::SliderFloat("iorLacunarity##im", &m.iorLacunarity, 1.0f, 4.0f, "%.3f");
    changed |= ImGui::SliderInt  ("iorMarchSteps##im", &m.iorMarchSteps, 1, 64);

    float ioff[3] = { m.iorOffset.x, m.iorOffset.y, m.iorOffset.z };
    if (ImGui::DragFloat3("iorOffset##im", ioff, 0.05f, -1000.0f, 1000.0f, "%.3f")) {
        m.iorOffset = ugm::vec3(ioff[0], ioff[1], ioff[2]);
        changed = true;
    }
    // Axial fade reuses the cone params authored on this medium
    // (coneOrigin / coneAxis / coneLength). Single signed knob: 0 = no fade,
    // +ve fades along +coneAxis, -ve along -coneAxis, |value| = strength.
    changed |= ImGui::SliderFloat("iorFalloff##im", &m.iorFalloff, -1.0f, 1.0f, "%.3f");
    return changed;
}

}  // namespace

bool drawInteriorMedium(SceneObject& so) {
    if (!ImGui::CollapsingHeader("Interior medium")) return false;

    // Enabled state is "the SceneObject owns a HomogeneousMedium", not derived
    // from σ values: pulling density (or every σ) to 0 would otherwise flip
    // the checkbox off and hide the sliders, so the user couldn't dial it
    // back up. Disable deletes the medium; re-enable allocates a fresh
    // zero-init one.
    HomogeneousMedium* m = so.interiorMedium;
    bool dirty = false;
    bool enabled = (m != NULL);
    if (ImGui::Checkbox("enable##interiorMedium", &enabled)) {
        if (enabled && m == NULL) {
            so.interiorMedium = new HomogeneousMedium();
            // Default newly-created media to follow the object so dragging
            // the bounding mesh in the Property panel also moves the flame.
            // Existing JSON-loaded media keep whatever the file authored.
            so.interiorMedium->coneFollowObject = true;
            so.interiorMedium->prepare();
        } else if (!enabled && m != NULL) {
            delete m;
            so.interiorMedium = NULL;
        }
        dirty = true;
    }

    m = so.interiorMedium;
    if (!enabled || m == NULL) return dirty;

    float sa[3] = { m->sigma_a.r, m->sigma_a.g, m->sigma_a.b };
    float ss[3] = { m->sigma_s.r, m->sigma_s.g, m->sigma_s.b };
    float se[3] = { m->sigma_e.r, m->sigma_e.g, m->sigma_e.b };

    bool changed = false;
    changed |= drawSigmaCommon(*m, sa, ss);
    changed |= drawEmissionMode(*m, se);
    changed |= drawDensityField(*m);
    changed |= drawHeatHaze(*m);

    if (changed) {
        m->sigma_a = ugm::color3(sa[0], sa[1], sa[2]);
        m->sigma_s = ugm::color3(ss[0], ss[1], ss[2]);
        m->sigma_e = ugm::color3(se[0], se[1], se[2]);
        m->prepare();
        dirty = true;
    }
    return dirty;
}

}  // namespace viewer
}  // namespace raygen

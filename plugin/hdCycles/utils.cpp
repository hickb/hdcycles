//  Copyright 2020 Tangent Animation
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied,
//  including without limitation, as related to merchantability and fitness
//  for a particular purpose.
//
//  In no event shall any copyright holder be liable for any damages of any kind
//  arising from the use of this software, whether in contract, tort or otherwise.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#include "utils.h"

#include "config.h"
#include "mesh.h"

#include <render/nodes.h>
#include <subd/subd_dice.h>
#include <subd/subd_split.h>
#include <util/util_path.h>

#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/extComputationUtils.h>
#include <pxr/usd/sdf/assetPath.h>

#ifdef USE_HBOOST
#    include <hboost/filesystem.hpp>
#else
#    include <boost/filesystem.hpp>
#endif

PXR_NAMESPACE_OPEN_SCOPE

/* =========- Texture ========== */

bool
HdCyclesPathIsUDIM(const ccl::string& a_filepath)
{
#ifndef USD_HAS_UDIM_RESOLVE_FIX
    // Added precheck to ensure no UDIM is accepted with relative path
    BOOST_NS::filesystem::path filepath(a_filepath);
    if (filepath.is_relative())
        return false;
#endif
    return a_filepath.find("<UDIM>") != std::string::npos;
}

// TODO: Investigate getting these tiles from uv data
// The cycles function ImageTextureNode::cull_tiles does not properly load tiles
// in an interactive session when not provided by Blender. We could assume these
// tiles based on uv primvars, but I have a feeling the material loading happens
// before the mesh syncing. More rnd needs to be done.
void
HdCyclesParseUDIMS(const ccl::string& a_filepath, ccl::vector<int>& a_tiles)
{
    BOOST_NS::filesystem::path filepath(a_filepath);

    size_t offset            = filepath.stem().string().find("<UDIM>");
    std::string baseFileName = filepath.stem().string().substr(0, offset);

    std::vector<std::string> files;

    BOOST_NS::filesystem::path path(ccl::path_dirname(a_filepath));
    for (BOOST_NS::filesystem::directory_iterator it(path);
         it != BOOST_NS::filesystem::directory_iterator(); ++it) {
        if (BOOST_NS::filesystem::is_regular_file(it->status())
            || BOOST_NS::filesystem::is_symlink(it->status())) {
            std::string foundFile = BOOST_NS::filesystem::basename(
                it->path().filename());

            if (baseFileName == (foundFile.substr(0, offset))) {
                files.push_back(foundFile);
            }
        }
    }

    a_tiles.clear();

    for (std::string file : files) {
        a_tiles.push_back(atoi(file.substr(offset, offset + 3).c_str()));
    }
}

void
HdCyclesMeshTextureSpace(ccl::Geometry* a_geom, ccl::float3& a_loc,
                         ccl::float3& a_size)
{
    // m_cyclesMesh->compute_bounds must be called before this
    a_loc  = (a_geom->bounds.max + a_geom->bounds.min) / 2.0f;
    a_size = (a_geom->bounds.max - a_geom->bounds.min) / 2.0f;

    if (a_size.x != 0.0f)
        a_size.x = 0.5f / a_size.x;
    if (a_size.y != 0.0f)
        a_size.y = 0.5f / a_size.y;
    if (a_size.z != 0.0f)
        a_size.z = 0.5f / a_size.z;

    a_loc = a_loc * a_size - ccl::make_float3(0.5f, 0.5f, 0.5f);
}

/* ========== Material ========== */

ccl::Shader*
HdCyclesCreateDefaultShader()
{
    ccl::Shader* shader = new ccl::Shader();

    shader->graph = new ccl::ShaderGraph();

    ccl::VertexColorNode* vc = new ccl::VertexColorNode();
    vc->layer_name           = ccl::ustring("displayColor");

    ccl::PrincipledBsdfNode* bsdf = new ccl::PrincipledBsdfNode();

    shader->graph->add(bsdf);
    shader->graph->add(vc);

    ccl::ShaderNode* out = shader->graph->output();
    shader->graph->connect(vc->output("Color"), bsdf->input("Base Color"));
    shader->graph->connect(bsdf->output("BSDF"), out->input("Surface"));

    return shader;
}

bool
_DumpGraph(ccl::ShaderGraph* shaderGraph, const char* name)
{
    if (!shaderGraph)
        return false;

    static const HdCyclesConfig& config = HdCyclesConfig::GetInstance();

    if (config.cycles_shader_graph_dump_dir.size() > 0) {
        std::string dump_location = config.cycles_shader_graph_dump_dir + "/"
                                    + TfMakeValidIdentifier(name)
                                    + "_graph.txt";
        std::cout << "Dumping shader graph: " << dump_location << '\n';
        try {
            shaderGraph->dump_graph(dump_location.c_str());
            return true;
        } catch (...) {
            std::cout << "Couldn't dump shadergraph: " << dump_location << "\n";
        }
    }
    return false;
}

/* ========= Conversion ========= */

// TODO: Make this function more robust
// Along with making point sampling more robust
// UPDATE:
// This causes a known slowdown to deforming motion blur renders
// This will be addressed in an upcoming PR
HdTimeSampleArray<GfMatrix4d, HD_CYCLES_MOTION_STEPS>
HdCyclesSetTransform(ccl::Object* object, HdSceneDelegate* delegate,
                     const SdfPath& id, bool use_motion)
{
    if (!object)
        return {};

    HdTimeSampleArray<GfMatrix4d, HD_CYCLES_MOTION_STEPS> xf {};

    delegate->SampleTransform(id, &xf);

    int sampleCount = xf.count;

    if (sampleCount == 0) {
        object->tfm = ccl::transform_identity();
        return xf;
    }

    if (sampleCount > 1) {
        bool foundCenter = false;
        for (int i = 0; i < sampleCount; i++) {
            if (xf.times.data()[i] == 0.0f) {
                object->tfm = mat4d_to_transform(xf.values.data()[i]);
                foundCenter = true;
            }
        }
        if (!foundCenter)
            object->tfm = mat4d_to_transform(xf.values.data()[0]);
    } else {
        object->tfm = mat4d_to_transform(xf.values.data()[0]);
    }

    if (!use_motion) {
        return xf;
    }

    object->motion.clear();
    if (object->geometry) {
        if (object->geometry->use_motion_blur
            && object->geometry->motion_steps != sampleCount) {
            object->motion.resize(object->geometry->motion_steps, object->tfm);
            return xf;
        }
    }

    // TODO: This might still be wrong on some edge cases...
    // The order of point sampling and transform sampling is the only reason
    // that this works
    if (object->geometry && object->geometry->motion_steps == sampleCount) {
        object->geometry->use_motion_blur = true;

        if (object->geometry->type == ccl::Geometry::MESH) {
            ccl::Mesh* mesh = (ccl::Mesh*)object->geometry;
            if (mesh->transform_applied)
                mesh->need_update = true;
        }

        object->motion.resize(sampleCount, ccl::transform_empty());

        for (int i = 0; i < sampleCount; i++) {
            if (xf.times.data()[i] == 0.0f) {
                object->tfm = mat4d_to_transform(xf.values.data()[i]);
            }

            int idx = i;
            if (object->geometry)
                object->geometry->motion_step(xf.times.data()[i]);

            object->motion[idx] = mat4d_to_transform(xf.values.data()[i]);
        }
    }

    return xf;
}

ccl::Transform
HdCyclesExtractTransform(HdSceneDelegate* delegate, const SdfPath& id)
{
    constexpr size_t maxSamples = 2;
    HdTimeSampleArray<GfMatrix4d, maxSamples> xf {};

    delegate->SampleTransform(id, &xf);

    return mat4d_to_transform(xf.values[0]);
}

GfMatrix4d
ConvertCameraTransform(const GfMatrix4d& a_cameraTransform)
{
    GfMatrix4d viewToWorldCorrectionMatrix(1.0);

    GfMatrix4d flipZ(1.0);
    flipZ[2][2]                 = -1.0;
    viewToWorldCorrectionMatrix = flipZ * viewToWorldCorrectionMatrix;

    return viewToWorldCorrectionMatrix * a_cameraTransform;
}

ccl::Transform
mat4d_to_transform(const GfMatrix4d& mat)
{
    ccl::Transform outTransform = ccl::transform_identity();

    outTransform.x.x = static_cast<float>(mat[0][0]);
    outTransform.x.y = static_cast<float>(mat[1][0]);
    outTransform.x.z = static_cast<float>(mat[2][0]);
    outTransform.x.w = static_cast<float>(mat[3][0]);

    outTransform.y.x = static_cast<float>(mat[0][1]);
    outTransform.y.y = static_cast<float>(mat[1][1]);
    outTransform.y.z = static_cast<float>(mat[2][1]);
    outTransform.y.w = static_cast<float>(mat[3][1]);

    outTransform.z.x = static_cast<float>(mat[0][2]);
    outTransform.z.y = static_cast<float>(mat[1][2]);
    outTransform.z.z = static_cast<float>(mat[2][2]);
    outTransform.z.w = static_cast<float>(mat[3][2]);

    return outTransform;
}

ccl::Transform
mat4f_to_transform(const GfMatrix4f& mat)
{
    ccl::Transform outTransform = ccl::transform_identity();

    outTransform.x.x = static_cast<float>(mat[0][0]);
    outTransform.x.y = static_cast<float>(mat[1][0]);
    outTransform.x.z = static_cast<float>(mat[2][0]);
    outTransform.x.w = static_cast<float>(mat[3][0]);

    outTransform.y.x = static_cast<float>(mat[0][1]);
    outTransform.y.y = static_cast<float>(mat[1][1]);
    outTransform.y.z = static_cast<float>(mat[2][1]);
    outTransform.y.w = static_cast<float>(mat[3][1]);

    outTransform.z.x = static_cast<float>(mat[0][2]);
    outTransform.z.y = static_cast<float>(mat[1][2]);
    outTransform.z.z = static_cast<float>(mat[2][2]);
    outTransform.z.w = static_cast<float>(mat[3][2]);

    return outTransform;
}

ccl::int2
vec2i_to_int2(const GfVec2i& a_vec)
{
    return ccl::make_int2(a_vec[0], a_vec[1]);
}

GfVec2i
int2_to_vec2i(const ccl::int2& a_int)
{
    return GfVec2i(a_int.x, a_int.y);
}

ccl::float2
vec2f_to_float2(const GfVec2f& a_vec)
{
    return ccl::make_float2(a_vec[0], a_vec[1]);
}

ccl::float2
vec2i_to_float2(const GfVec2i& a_vec)
{
    return ccl::make_float2((float)a_vec[0], (float)a_vec[1]);
}

ccl::float2
vec2d_to_float2(const GfVec2d& a_vec)
{
    return ccl::make_float2((float)a_vec[0], (float)a_vec[1]);
}

ccl::float2
vec3f_to_float2(const GfVec3f& a_vec)
{
    return ccl::make_float2(a_vec[0], a_vec[1]);
}

ccl::float3
float_to_float3(const float& a_vec)
{
    return ccl::make_float3(a_vec, a_vec, a_vec);
}

ccl::float3
vec2f_to_float3(const GfVec2f& a_vec)
{
    return ccl::make_float3(a_vec[0], a_vec[1], 0.0f);
}

ccl::float3
vec3f_to_float3(const GfVec3f& a_vec)
{
    return ccl::make_float3(a_vec[0], a_vec[1], a_vec[2]);
}

ccl::float3
vec3i_to_float3(const GfVec3i& a_vec)
{
    return ccl::make_float3((float)a_vec[0], (float)a_vec[1], (float)a_vec[2]);
}

ccl::float3
vec3d_to_float3(const GfVec3d& a_vec)
{
    return ccl::make_float3((float)a_vec[0], (float)a_vec[1], (float)a_vec[2]);
}

ccl::float3
vec4f_to_float3(const GfVec4f& a_vec)
{
    return ccl::make_float3(a_vec[0], a_vec[1], a_vec[2]);
}

ccl::float4
vec1f_to_float4(const float& a_val)
{
    return ccl::make_float4(a_val, a_val, a_val, a_val);
}

ccl::float4
vec2f_to_float4(const GfVec2f& a_vec, float a_z, float a_alpha)
{
    return ccl::make_float4(a_vec[0], a_vec[1], a_z, a_alpha);
}

ccl::float4
vec3f_to_float4(const GfVec3f& a_vec, float a_alpha)
{
    return ccl::make_float4(a_vec[0], a_vec[1], a_vec[2], a_alpha);
}

ccl::float4
vec4f_to_float4(const GfVec4f& a_vec)
{
    return ccl::make_float4(a_vec[0], a_vec[1], a_vec[2], a_vec[3]);
}

ccl::float4
vec4i_to_float4(const GfVec4i& a_vec)
{
    return ccl::make_float4((float)a_vec[0], (float)a_vec[1], (float)a_vec[2],
                            (float)a_vec[3]);
}

ccl::float4
vec4d_to_float4(const GfVec4d& a_vec)
{
    return ccl::make_float4((float)a_vec[0], (float)a_vec[1], (float)a_vec[2],
                            (float)a_vec[3]);
}

/* ========= Primvars ========= */

const std::array<HdInterpolation, HdInterpolationCount> interpolations {
    HdInterpolationConstant,    HdInterpolationUniform,
    HdInterpolationVarying,     HdInterpolationVertex,
    HdInterpolationFaceVarying, HdInterpolationInstance,
};

inline void
_HdCyclesInsertPrimvar(HdCyclesPrimvarMap& primvars, const TfToken& name,
                       const TfToken& role, HdInterpolation interpolation,
                       const VtValue& value)
{
    auto it = primvars.find(name);
    if (it == primvars.end()) {
        primvars.insert({ name, { value, role, interpolation } });
    } else {
        it->second.value         = value;
        it->second.role          = role;
        it->second.interpolation = interpolation;
        it->second.dirtied       = true;
    }
}

// Get Computed primvars
bool
HdCyclesGetComputedPrimvars(HdSceneDelegate* a_delegate, const SdfPath& a_id,
                            HdDirtyBits a_dirtyBits,
                            HdCyclesPrimvarMap& a_primvars)
{
    // First we are querying which primvars need to be computed, and storing them in a list to rely
    // on the batched computation function in HdExtComputationUtils.
    HdExtComputationPrimvarDescriptorVector dirtyPrimvars;
    for (HdInterpolation interpolation : interpolations) {
        auto computedPrimvars
            = a_delegate->GetExtComputationPrimvarDescriptors(a_id,
                                                              interpolation);
        for (const auto& primvar : computedPrimvars) {
            if (HdChangeTracker::IsPrimvarDirty(a_dirtyBits, a_id,
                                                primvar.name)) {
                dirtyPrimvars.emplace_back(primvar);
            }
        }
    }

    // Early exit.
    if (dirtyPrimvars.empty()) {
        return false;
    }

    auto changed = false;
    auto valueStore
        = HdExtComputationUtils::GetComputedPrimvarValues(dirtyPrimvars,
                                                          a_delegate);
    for (const auto& primvar : dirtyPrimvars) {
        const auto itComputed = valueStore.find(primvar.name);
        if (itComputed == valueStore.end()) {
            continue;
        }
        changed = true;
        _HdCyclesInsertPrimvar(a_primvars, primvar.name, primvar.role,
                               primvar.interpolation, itComputed->second);
    }

    return changed;
}

// Get Non-computed primvars
bool
HdCyclesGetPrimvars(HdSceneDelegate* a_delegate, const SdfPath& a_id,
                    HdDirtyBits a_dirtyBits, bool a_multiplePositionKeys,
                    HdCyclesPrimvarMap& a_primvars)
{
    for (auto interpolation : interpolations) {
        const auto primvarDescs
            = a_delegate->GetPrimvarDescriptors(a_id, interpolation);
        for (const auto& primvarDesc : primvarDescs) {
            if (primvarDesc.name == HdTokens->points) {
                continue;
            }
            // The number of motion keys has to be matched between points and normals, so
            _HdCyclesInsertPrimvar(a_primvars, primvarDesc.name,
                                   primvarDesc.role, primvarDesc.interpolation,
                                   (a_multiplePositionKeys
                                    && primvarDesc.name == HdTokens->normals)
                                       ? VtValue {}
                                       : a_delegate->Get(a_id,
                                                         primvarDesc.name));
        }
    }

    return true;
}


void
HdCyclesPopulatePrimvarDescsPerInterpolation(
    HdSceneDelegate* a_sceneDelegate, SdfPath const& a_id,
    HdCyclesPDPIMap* a_primvarDescsPerInterpolation)
{
    if (!a_primvarDescsPerInterpolation->empty()) {
        return;
    }

    auto interpolations = {
        HdInterpolationConstant,    HdInterpolationUniform,
        HdInterpolationVarying,     HdInterpolationVertex,
        HdInterpolationFaceVarying, HdInterpolationInstance,
    };
    for (auto& interpolation : interpolations) {
        a_primvarDescsPerInterpolation->emplace(
            interpolation,
            a_sceneDelegate->GetPrimvarDescriptors(a_id, interpolation));
    }
}

bool
HdCyclesIsPrimvarExists(TfToken const& a_name,
                        HdCyclesPDPIMap const& a_primvarDescsPerInterpolation,
                        HdInterpolation* a_interpolation)
{
    for (auto& entry : a_primvarDescsPerInterpolation) {
        for (auto& pv : entry.second) {
            if (pv.name == a_name) {
                if (a_interpolation) {
                    *a_interpolation = entry.first;
                }
                return true;
            }
        }
    }
    return false;
}

template<>
inline float
to_cycles<float>(const float& v) noexcept
{
    return v;
}
template<>
inline float
to_cycles<double>(const double& v) noexcept
{
    return static_cast<float>(v);
}
template<>
inline float
to_cycles<int>(const int& v) noexcept
{
    return static_cast<float>(v);
}


template<>
inline ccl::float2
to_cycles<GfVec2f>(const GfVec2f& v) noexcept
{
    return ccl::make_float2(v[0], v[1]);
}
template<>
inline ccl::float2
to_cycles<GfVec2h>(const GfVec2h& v) noexcept
{
    return ccl::make_float2(static_cast<float>(v[0]), static_cast<float>(v[1]));
}
template<>
inline ccl::float2
to_cycles<GfVec2d>(const GfVec2d& v) noexcept
{
    return ccl::make_float2(static_cast<float>(v[0]), static_cast<float>(v[1]));
}
template<>
inline ccl::float2
to_cycles<GfVec2i>(const GfVec2i& v) noexcept
{
    return ccl::make_float2(static_cast<float>(v[0]), static_cast<float>(v[1]));
}

template<>
inline ccl::float3
to_cycles<GfVec3f>(const GfVec3f& v) noexcept
{
    return ccl::make_float3(v[0], v[1], v[2]);
}
template<>
inline ccl::float3
to_cycles<GfVec3h>(const GfVec3h& v) noexcept
{
    return ccl::make_float3(static_cast<float>(v[0]), static_cast<float>(v[1]),
                            static_cast<float>(v[2]));
}
template<>
inline ccl::float3
to_cycles<GfVec3d>(const GfVec3d& v) noexcept
{
    return ccl::make_float3(static_cast<float>(v[0]), static_cast<float>(v[1]),
                            static_cast<float>(v[2]));
}
template<>
inline ccl::float3
to_cycles<GfVec3i>(const GfVec3i& v) noexcept
{
    return ccl::make_float3(static_cast<float>(v[0]), static_cast<float>(v[1]),
                            static_cast<float>(v[2]));
}

template<>
inline ccl::float4
to_cycles<GfVec4f>(const GfVec4f& v) noexcept
{
    return ccl::make_float4(v[0], v[1], v[2], v[3]);
}
template<>
inline ccl::float4
to_cycles<GfVec4h>(const GfVec4h& v) noexcept
{
    return ccl::make_float4(static_cast<float>(v[0]), static_cast<float>(v[1]),
                            static_cast<float>(v[2]), static_cast<float>(v[3]));
}
template<>
inline ccl::float4
to_cycles<GfVec4d>(const GfVec4d& v) noexcept
{
    return ccl::make_float4(static_cast<float>(v[0]), static_cast<float>(v[1]),
                            static_cast<float>(v[2]), static_cast<float>(v[3]));
}
template<>
inline ccl::float4
to_cycles<GfVec4i>(const GfVec4i& v) noexcept
{
    return ccl::make_float4(static_cast<float>(v[0]), static_cast<float>(v[1]),
                            static_cast<float>(v[2]), static_cast<float>(v[3]));
}


template<typename T, typename U>
bool
_PopulateAttribte_Vertex(const VtValue& value, ccl::Attribute* attr)
{
    VtArray<T> usd_data = value.UncheckedGet<VtArray<T>>();
    size_t arr_size     = value.GetArraySize();

    if (arr_size <= 0)
        return false;

    char* data = attr->data();

    for (int i = 0; i < arr_size; i++)
        ((U*)data)[i] = to_cycles<T, U>(usd_data[i]);

    return true;
}

template<typename T, typename U>
bool
_PopulateAttribte_Uniform(const VtValue& value, ccl::Attribute* attr,
                          HdCyclesMesh* mesh)
{
    VtArray<T> usd_data = value.UncheckedGet<VtArray<T>>();
    size_t arr_size     = value.GetArraySize();

    if (arr_size <= 0)
        return false;

    char* data = attr->data();

    int idx = 0;
    for (size_t i = 0; i < arr_size; i++)
        for (size_t j = 0; j < (mesh->GetFaceVertexCounts()[i] - 2); j++, idx++)
            ((U*)data)[idx] = to_cycles<T, U>(usd_data[i]);

    return true;
}

template<typename T, typename U>
bool
_PopulateAttribte_FaceVarying(const VtValue& value, ccl::Attribute* attr,
                              HdCyclesMesh* mesh)
{
    VtArray<T> usd_data = value.UncheckedGet<VtArray<T>>();
    size_t arr_size     = value.GetArraySize();
    size_t data_size    = sizeof(U);

    if (arr_size <= 0)
        return false;

    char* data = attr->data();

    int count = 0;
    for (int i = 0; i < mesh->GetFaceVertexCounts().size(); i++) {
        const int vCount = mesh->GetFaceVertexCounts()[i];

        for (int j = 1; j < vCount - 1; ++j) {
            int v0 = count;
            int v1 = (count + j + 0);
            int v2 = (count + j + 1);

            if (mesh->GetOrientation() == HdTokens->leftHanded) {
                v1 = (count + ((vCount - 1) - j) + 0);
                v2 = (count + ((vCount - 1) - j) + 1);
            }

            ((U*)data)[0] = to_cycles<T, U>(usd_data[v0]);
            ((U*)data)[1] = to_cycles<T, U>(usd_data[v1]);
            ((U*)data)[2] = to_cycles<T, U>(usd_data[v2]);

            data += 3 * data_size;
        }
        count += vCount;
    }

    return true;
}

template<typename T, typename U>
bool
_PopulateAttribte_Constant(const VtValue& value, ccl::Attribute* attr)
{
    VtArray<T> usd_data = value.UncheckedGet<VtArray<T>>();
    size_t arr_size     = value.GetArraySize();
    if (arr_size != 1) {
        std::cout << "Constant attribute, incompatible size: " << arr_size
                  << '\n';
        return false;
    }

    char* data = attr->data();

    ((U*)data)[0] = to_cycles<T, U>(usd_data[0]);

    return true;
}


void
_PopulateAttribute(const TfToken& name, const TfToken& role,
                   HdInterpolation interpolation, const VtValue& value,
                   ccl::Attribute* attr, HdCyclesMesh* mesh)
{
//    std::cout << "Populate " << name << " " << interpolation << std::endl;

    if (interpolation == HdInterpolationVertex) {
        if (value.IsHolding<VtArray<float>>()) {
            _PopulateAttribte_Vertex<float, float>(value, attr);
        } else if (value.IsHolding<VtArray<double>>()) {
            _PopulateAttribte_Vertex<double, float>(value, attr);
        } else if (value.IsHolding<VtArray<int>>()) {
            _PopulateAttribte_Vertex<int, float>(value, attr);
        }

        else if (value.IsHolding<VtArray<GfVec2f>>()) {
            _PopulateAttribte_Vertex<GfVec2f, ccl::float2>(value, attr);
        } else if (value.IsHolding<VtArray<GfVec2d>>()) {
            _PopulateAttribte_Vertex<GfVec2d, ccl::float2>(value, attr);
        } else if (value.IsHolding<VtArray<GfVec2i>>()) {
            _PopulateAttribte_Vertex<GfVec2i, ccl::float2>(value, attr);
        }

        else if (value.IsHolding<VtArray<GfVec3f>>()) {
            _PopulateAttribte_Vertex<GfVec3f, ccl::float3>(value, attr);
        } else if (value.IsHolding<VtArray<GfVec3d>>()) {
            _PopulateAttribte_Vertex<GfVec3d, ccl::float3>(value, attr);
        } else if (value.IsHolding<VtArray<GfVec3i>>()) {
            _PopulateAttribte_Vertex<GfVec3i, ccl::float3>(value, attr);
        }

        else if (value.IsHolding<VtArray<GfVec4f>>()) {
            _PopulateAttribte_Vertex<GfVec4f, ccl::float4>(value, attr);
        } else if (value.IsHolding<VtArray<GfVec4d>>()) {
            _PopulateAttribte_Vertex<GfVec4d, ccl::float4>(value, attr);
        } else if (value.IsHolding<VtArray<GfVec4i>>()) {
            _PopulateAttribte_Vertex<GfVec4i, ccl::float4>(value, attr);
        }

    } else if (interpolation == HdInterpolationUniform) {
        std::cout << value.GetArraySize() << " " << mesh->GetFaceVertexCounts().size() << std::endl;
        if (value.GetArraySize() > mesh->GetFaceVertexCounts().size()) {
            std::cout << "Oversized...\n";
            return;
        }

        if (value.IsHolding<VtArray<float>>()) {
            _PopulateAttribte_Uniform<float, float>(value, attr, mesh);
        } else if (value.IsHolding<VtArray<double>>()) {
            _PopulateAttribte_Uniform<double, float>(value, attr, mesh);
        } else if (value.IsHolding<VtArray<int>>()) {
            _PopulateAttribte_Uniform<int, float>(value, attr, mesh);
        }

        else if (value.IsHolding<VtArray<GfVec2f>>()) {
            _PopulateAttribte_Uniform<GfVec2f, ccl::float2>(value, attr, mesh);
        } else if (value.IsHolding<VtArray<GfVec2d>>()) {
            _PopulateAttribte_Uniform<GfVec2d, ccl::float2>(value, attr, mesh);
        } else if (value.IsHolding<VtArray<GfVec2i>>()) {
            _PopulateAttribte_Uniform<GfVec2i, ccl::float2>(value, attr, mesh);
        }

        else if (value.IsHolding<VtArray<GfVec3f>>()) {
            _PopulateAttribte_Uniform<GfVec3f, ccl::float3>(value, attr, mesh);
        } else if (value.IsHolding<VtArray<GfVec3d>>()) {
            _PopulateAttribte_Uniform<GfVec3d, ccl::float3>(value, attr, mesh);
        } else if (value.IsHolding<VtArray<GfVec3i>>()) {
            _PopulateAttribte_Uniform<GfVec3i, ccl::float3>(value, attr, mesh);
        }

        else if (value.IsHolding<VtArray<GfVec4f>>()) {
            _PopulateAttribte_Uniform<GfVec4f, ccl::float4>(value, attr, mesh);
        } else if (value.IsHolding<VtArray<GfVec4d>>()) {
            _PopulateAttribte_Uniform<GfVec4d, ccl::float4>(value, attr, mesh);
        } else if (value.IsHolding<VtArray<GfVec4i>>()) {
            _PopulateAttribte_Uniform<GfVec4i, ccl::float4>(value, attr, mesh);
        }
    } else if (interpolation == HdInterpolationFaceVarying) {
        if (value.IsHolding<VtArray<float>>()) {
            _PopulateAttribte_FaceVarying<float, float>(value, attr, mesh);
        } else if (value.IsHolding<VtArray<double>>()) {
            _PopulateAttribte_FaceVarying<double, float>(value, attr, mesh);
        } else if (value.IsHolding<VtArray<int>>()) {
            _PopulateAttribte_FaceVarying<int, float>(value, attr, mesh);
        }

        else if (value.IsHolding<VtArray<GfVec2f>>()) {
            _PopulateAttribte_FaceVarying<GfVec2f, ccl::float2>(value, attr,
                                                                mesh);
        } else if (value.IsHolding<VtArray<GfVec2d>>()) {
            _PopulateAttribte_FaceVarying<GfVec2d, ccl::float2>(value, attr,
                                                                mesh);
        } else if (value.IsHolding<VtArray<GfVec2i>>()) {
            _PopulateAttribte_FaceVarying<GfVec2i, ccl::float2>(value, attr,
                                                                mesh);
        }

        else if (value.IsHolding<VtArray<GfVec3f>>()) {
            _PopulateAttribte_FaceVarying<GfVec3f, ccl::float3>(value, attr,
                                                                mesh);
        } else if (value.IsHolding<VtArray<GfVec3d>>()) {
            _PopulateAttribte_FaceVarying<GfVec3d, ccl::float3>(value, attr,
                                                                mesh);
        } else if (value.IsHolding<VtArray<GfVec3i>>()) {
            _PopulateAttribte_FaceVarying<GfVec3i, ccl::float3>(value, attr,
                                                                mesh);
        }

        else if (value.IsHolding<VtArray<GfVec4f>>()) {
            _PopulateAttribte_FaceVarying<GfVec4f, ccl::float4>(value, attr,
                                                                mesh);
        } else if (value.IsHolding<VtArray<GfVec4d>>()) {
            _PopulateAttribte_FaceVarying<GfVec4d, ccl::float4>(value, attr,
                                                                mesh);
        } else if (value.IsHolding<VtArray<GfVec4i>>()) {
            _PopulateAttribte_FaceVarying<GfVec4i, ccl::float4>(value, attr,
                                                                mesh);
        }
    } else if (interpolation == HdInterpolationConstant) {
        if (value.IsHolding<VtArray<float>>()) {
            _PopulateAttribte_Constant<float, float>(value, attr);
        } else if (value.IsHolding<VtArray<double>>()) {
            _PopulateAttribte_Constant<double, float>(value, attr);
        } else if (value.IsHolding<VtArray<int>>()) {
            _PopulateAttribte_Constant<int, float>(value, attr);
        }

        else if (value.IsHolding<VtArray<GfVec2f>>()) {
            _PopulateAttribte_Constant<GfVec2f, ccl::float2>(value, attr);
        } else if (value.IsHolding<VtArray<GfVec2d>>()) {
            _PopulateAttribte_Constant<GfVec2d, ccl::float2>(value, attr);
        } else if (value.IsHolding<VtArray<GfVec2i>>()) {
            _PopulateAttribte_Constant<GfVec2i, ccl::float2>(value, attr);
        }

        else if (value.IsHolding<VtArray<GfVec3f>>()) {
            _PopulateAttribte_Constant<GfVec3f, ccl::float3>(value, attr);
        } else if (value.IsHolding<VtArray<GfVec3d>>()) {
            _PopulateAttribte_Constant<GfVec3d, ccl::float3>(value, attr);
        } else if (value.IsHolding<VtArray<GfVec3i>>()) {
            _PopulateAttribte_Constant<GfVec3i, ccl::float3>(value, attr);
        }

        else if (value.IsHolding<VtArray<GfVec4f>>()) {
            _PopulateAttribte_Constant<GfVec4f, ccl::float4>(value, attr);
        } else if (value.IsHolding<VtArray<GfVec4d>>()) {
            _PopulateAttribte_Constant<GfVec4d, ccl::float4>(value, attr);
        } else if (value.IsHolding<VtArray<GfVec4i>>()) {
            _PopulateAttribte_Constant<GfVec4i, ccl::float4>(value, attr);
        }
    } else {
        std::cout << "HdCycles WARNING: Interpolation unsupported: "
                  << interpolation << '\n';
    }
}


/* ========= MikkTSpace ========= */

struct MikkUserData {
    MikkUserData(const char* layer_name, ccl::Mesh* mesh, ccl::float3* tangent,
                 float* tangent_sign)
        : mesh(mesh)
        , texface(NULL)
        , tangent(tangent)
        , tangent_sign(tangent_sign)
    {
        const ccl::AttributeSet& attributes = (mesh->subd_faces.size())
                                                  ? mesh->subd_attributes
                                                  : mesh->attributes;

        ccl::Attribute* attr_vN = attributes.find(ccl::ATTR_STD_VERTEX_NORMAL);
        if (!attr_vN) {
            mesh->add_face_normals();
            mesh->add_vertex_normals();
            attr_vN = attributes.find(ccl::ATTR_STD_VERTEX_NORMAL);
        }
        vertex_normal = attr_vN->data_float3();

        ccl::Attribute* attr_uv = attributes.find(ccl::ustring(layer_name));
        if (attr_uv != NULL) {
            texface = attr_uv->data_float2();
        }
    }

    ccl::Mesh* mesh;
    int num_faces;

    ccl::float3* vertex_normal;
    ccl::float2* texface;

    ccl::float3* tangent;
    float* tangent_sign;
};

int
mikk_get_num_faces(const SMikkTSpaceContext* context)
{
    const MikkUserData* userdata = (const MikkUserData*)context->m_pUserData;
    if (userdata->mesh->subd_faces.size()) {
        return userdata->mesh->subd_faces.size();
    } else {
        return userdata->mesh->num_triangles();
    }
}

int
mikk_get_num_verts_of_face(const SMikkTSpaceContext* context,
                           const int face_num)
{
    const MikkUserData* userdata = (const MikkUserData*)context->m_pUserData;
    if (userdata->mesh->subd_faces.size()) {
        const ccl::Mesh* mesh = userdata->mesh;
        return mesh->subd_faces[face_num].num_corners;
    } else {
        return 3;
    }
}

int
mikk_vertex_index(const ccl::Mesh* mesh, const int face_num, const int vert_num)
{
    if (mesh->subd_faces.size()) {
        const ccl::Mesh::SubdFace& face = mesh->subd_faces[face_num];
        return mesh->subd_face_corners[face.start_corner + vert_num];
    } else {
        return mesh->triangles[face_num * 3 + vert_num];
    }
}

int
mikk_corner_index(const ccl::Mesh* mesh, const int face_num, const int vert_num)
{
    if (mesh->subd_faces.size()) {
        const ccl::Mesh::SubdFace& face = mesh->subd_faces[face_num];
        return face.start_corner + vert_num;
    } else {
        return face_num * 3 + vert_num;
    }
}

void
mikk_get_position(const SMikkTSpaceContext* context, float P[3],
                  const int face_num, const int vert_num)
{
    const MikkUserData* userdata = (const MikkUserData*)context->m_pUserData;
    const ccl::Mesh* mesh        = userdata->mesh;
    const int vertex_index       = mikk_vertex_index(mesh, face_num, vert_num);
    const ccl::float3 vP         = mesh->verts[vertex_index];
    P[0]                         = vP.x;
    P[1]                         = vP.y;
    P[2]                         = vP.z;
}

void
mikk_get_texture_coordinate(const SMikkTSpaceContext* context, float uv[2],
                            const int face_num, const int vert_num)
{
    const MikkUserData* userdata = (const MikkUserData*)context->m_pUserData;
    const ccl::Mesh* mesh        = userdata->mesh;
    if (userdata->texface != NULL) {
        const int corner_index = mikk_corner_index(mesh, face_num, vert_num);
        ccl::float2 tfuv       = userdata->texface[corner_index];
        uv[0]                  = tfuv.x;
        uv[1]                  = tfuv.y;
    } else {
        uv[0] = 0.0f;
        uv[1] = 0.0f;
    }
}

void
mikk_get_normal(const SMikkTSpaceContext* context, float N[3],
                const int face_num, const int vert_num)
{
    const MikkUserData* userdata = (const MikkUserData*)context->m_pUserData;
    const ccl::Mesh* mesh        = userdata->mesh;
    ccl::float3 vN;
    if (mesh->subd_faces.size()) {
        const ccl::Mesh::SubdFace& face = mesh->subd_faces[face_num];
        if (face.smooth) {
            const int vertex_index = mikk_vertex_index(mesh, face_num,
                                                       vert_num);
            vN                     = userdata->vertex_normal[vertex_index];
        } else {
            vN = face.normal(mesh);
        }
    } else {
        if (mesh->smooth[face_num]) {
            const int vertex_index = mikk_vertex_index(mesh, face_num,
                                                       vert_num);
            vN                     = userdata->vertex_normal[vertex_index];
        } else {
            const ccl::Mesh::Triangle tri = mesh->get_triangle(face_num);
            vN                            = tri.compute_normal(&mesh->verts[0]);
        }
    }
    N[0] = vN.x;
    N[1] = vN.y;
    N[2] = vN.z;
}

void
mikk_set_tangent_space(const SMikkTSpaceContext* context, const float T[],
                       const float sign, const int face_num, const int vert_num)
{
    MikkUserData* userdata = (MikkUserData*)context->m_pUserData;
    const ccl::Mesh* mesh  = userdata->mesh;
    const int corner_index = mikk_corner_index(mesh, face_num, vert_num);
    userdata->tangent[corner_index] = ccl::make_float3(T[0], T[1], T[2]);
    if (userdata->tangent_sign != NULL) {
        userdata->tangent_sign[corner_index] = sign;
    }
}

void
mikk_compute_tangents(const char* layer_name, ccl::Mesh* mesh, bool need_sign,
                      bool active_render)
{
    /* Create tangent attributes. */
    ccl::AttributeSet& attributes = (mesh->subd_faces.size())
                                        ? mesh->subd_attributes
                                        : mesh->attributes;
    ccl::Attribute* attr;
    ccl::ustring name;

    if (layer_name != NULL) {
        name = ccl::ustring((std::string(layer_name) + ".tangent").c_str());
    } else {
        name = ccl::ustring("orco.tangent");
    }

    if (active_render) {
        attr = attributes.add(ccl::ATTR_STD_UV_TANGENT, name);
    } else {
        attr = attributes.add(name, ccl::TypeDesc::TypeVector,
                              ccl::ATTR_ELEMENT_CORNER);
    }
    ccl::float3* tangent = attr->data_float3();
    /* Create bitangent sign attribute. */
    float* tangent_sign = NULL;
    if (need_sign) {
        ccl::Attribute* attr_sign;
        ccl::ustring name_sign;

        if (layer_name != NULL) {
            name_sign = ccl::ustring(
                (std::string(layer_name) + ".tangent_sign").c_str());
        } else {
            name_sign = ccl::ustring("orco.tangent_sign");
        }

        if (active_render) {
            attr_sign = attributes.add(ccl::ATTR_STD_UV_TANGENT_SIGN,
                                       name_sign);
        } else {
            attr_sign = attributes.add(name_sign, ccl::TypeDesc::TypeFloat,
                                       ccl::ATTR_ELEMENT_CORNER);
        }
        tangent_sign = attr_sign->data_float();
    }
    /* Setup userdata. */
    MikkUserData userdata(layer_name, mesh, tangent, tangent_sign);
    /* Setup interface. */
    SMikkTSpaceInterface sm_interface;
    memset(&sm_interface, 0, sizeof(sm_interface));
    sm_interface.m_getNumFaces          = mikk_get_num_faces;
    sm_interface.m_getNumVerticesOfFace = mikk_get_num_verts_of_face;
    sm_interface.m_getPosition          = mikk_get_position;
    sm_interface.m_getTexCoord          = mikk_get_texture_coordinate;
    sm_interface.m_getNormal            = mikk_get_normal;
    sm_interface.m_setTSpaceBasic       = mikk_set_tangent_space;
    /* Setup context. */
    SMikkTSpaceContext context;
    memset(&context, 0, sizeof(context));
    context.m_pUserData  = &userdata;
    context.m_pInterface = &sm_interface;
    /* Compute tangents. */
    genTangSpaceDefault(&context);
}

template<>
bool
_HdCyclesGetVtValue<bool>(VtValue a_value, bool a_default, bool* a_hasChanged,
                          bool a_checkWithDefault)
{
    bool val = a_default;
    if (!a_value.IsEmpty()) {
        if (a_value.IsHolding<bool>()) {
            if (!a_checkWithDefault && a_hasChanged)
                *a_hasChanged = true;
            val = a_value.UncheckedGet<bool>();
        } else if (a_value.IsHolding<int>()) {
            if (!a_checkWithDefault && a_hasChanged)
                *a_hasChanged = true;
            val = (bool)a_value.UncheckedGet<int>();
        } else if (a_value.IsHolding<float>()) {
            if (!a_checkWithDefault && a_hasChanged)
                val = (a_value.UncheckedGet<float>() == 1.0f);
        } else if (a_value.IsHolding<double>()) {
            if (!a_checkWithDefault && a_hasChanged)
                *a_hasChanged = true;
            val = (a_value.UncheckedGet<double>() == 1.0);
        }
    }
    if (a_hasChanged && a_checkWithDefault && val != a_default)
        *a_hasChanged = true;
    return val;
}

PXR_NAMESPACE_CLOSE_SCOPE
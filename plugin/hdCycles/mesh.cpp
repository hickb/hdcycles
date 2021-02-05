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

#include "mesh.h"

#include "config.h"
#include "instancer.h"
#include "material.h"
#include "renderDelegate.h"
#include "renderParam.h"
#include "utils.h"
#include "meshRefiner.h"

#include "Mikktspace/mikktspace.h"

#include <vector>

#include <render/mesh.h>
#include <render/object.h>
#include <render/scene.h>
#include <render/shader.h>
#include <subd/subd_dice.h>
#include <subd/subd_split.h>
#include <util/util_math_float2.h>
#include <util/util_math_float3.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/tf/hash.h>
#include <pxr/base/tf/hashmap.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/extComputationUtils.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshUtil.h>
#include <pxr/imaging/hd/points.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/smoothNormals.h>
#include <pxr/imaging/pxOsd/tokens.h>
#include <pxr/imaging/hd/vtBufferSource.h>

#ifdef USE_USD_CYCLES_SCHEMA
#    include <usdCycles/tokens.h>
#endif

PXR_NAMESPACE_OPEN_SCOPE

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens, 
    (st)
    (uv)
);
// clang-format on

HdCyclesMesh::HdCyclesMesh(SdfPath const& id, SdfPath const& instancerId,
                           HdCyclesRenderDelegate* a_renderDelegate)
    : HdMesh(id, instancerId)
    , m_renderDelegate(a_renderDelegate)
    , m_cyclesMesh(nullptr)
    , m_cyclesObject(nullptr)
    , m_hasVertexColors(false)
    , m_visibilityFlags(ccl::PATH_RAY_ALL_VISIBILITY)
    , m_visCamera(true)
    , m_visDiffuse(true)
    , m_visGlossy(true)
    , m_visScatter(true)
    , m_visShadow(true)
    , m_visTransmission(true)
    , m_velocityScale(1.0f)
    , m_useMotionBlur(false)
    , m_useDeformMotionBlur(false)
{
    static const HdCyclesConfig& config = HdCyclesConfig::GetInstance();
    config.enable_subdivision.eval(m_subdivEnabled, true);
    config.subdivision_dicing_rate.eval(m_dicingRate, true);
    config.max_subdivision.eval(m_maxSubdivision, true);
    config.enable_motion_blur.eval(m_useMotionBlur, true);

    m_cyclesObject = _CreateCyclesObject();

    m_cyclesMesh = _CreateCyclesMesh();

    m_numTransformSamples = HD_CYCLES_MOTION_STEPS;

    if (m_useMotionBlur) {
        // Motion steps are currently a static const compile time
        // variable... This is likely an issue...
        // TODO: Get this from usdCycles schema
        //m_motionSteps = config.motion_steps;
        m_motionSteps = m_numTransformSamples;

        // Hardcoded for now until schema PR
        m_useDeformMotionBlur = true;

        // TODO: Needed when we properly handle motion_verts
        m_cyclesMesh->motion_steps    = m_motionSteps;
        m_cyclesMesh->use_motion_blur = m_useDeformMotionBlur;
    }

    m_cyclesObject->geometry = m_cyclesMesh;

    m_renderDelegate->GetCyclesRenderParam()->AddGeometry(m_cyclesMesh);
    m_renderDelegate->GetCyclesRenderParam()->AddObject(m_cyclesObject);
}

HdCyclesMesh::~HdCyclesMesh()
{
    if (m_cyclesMesh) {
        m_renderDelegate->GetCyclesRenderParam()->RemoveMesh(m_cyclesMesh);
        delete m_cyclesMesh;
    }

    if (m_cyclesObject) {
        m_renderDelegate->GetCyclesRenderParam()->RemoveObject(m_cyclesObject);
        delete m_cyclesObject;
    }

    if (m_cyclesInstances.size() > 0) {
        for (auto instance : m_cyclesInstances) {
            if (instance) {
                m_renderDelegate->GetCyclesRenderParam()->RemoveObject(
                    instance);
                delete instance;
            }
        }
    }
}

HdDirtyBits
HdCyclesMesh::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::Clean | HdChangeTracker::DirtyPoints
           | HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyPrimvar
           | HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyVisibility
           | HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtySubdivTags
           | HdChangeTracker::DirtyPrimID | HdChangeTracker::DirtyDisplayStyle
           | HdChangeTracker::DirtyDoubleSided;
}
template<typename T>
bool
HdCyclesMesh::GetPrimvarData(TfToken const& name,
                             HdSceneDelegate* sceneDelegate,
                             std::map<HdInterpolation, HdPrimvarDescriptorVector>
                                 primvarDescsPerInterpolation,
                             VtArray<T>& out_data, VtIntArray& out_indices)
{
    out_data.clear();
    out_indices.clear();

    auto& vertex_indices = GetFaceVertexIndices();
    for (auto& primvarDescsEntry : primvarDescsPerInterpolation) {
        for (auto& pv : primvarDescsEntry.second) {
            if (pv.name == name) {
                auto value = GetPrimvar(sceneDelegate, name);
                if (value.IsHolding<VtArray<T>>()) {
                    out_data = value.UncheckedGet<VtArray<T>>();
                    if (primvarDescsEntry.first == HdInterpolationFaceVarying) {
                        out_indices.reserve(vertex_indices.size());
                        for (int i = 0; i < vertex_indices.size(); ++i) {
                            out_indices.push_back(i);
                        }
                    }
                    return true;
                }
                return false;
            }
        }
    }

    return false;
}
template bool
HdCyclesMesh::GetPrimvarData<GfVec2f>(
    TfToken const&, HdSceneDelegate*,
    std::map<HdInterpolation, HdPrimvarDescriptorVector>, VtArray<GfVec2f>&,
    VtIntArray&);
template bool
HdCyclesMesh::GetPrimvarData<GfVec3f>(
    TfToken const&, HdSceneDelegate*,
    std::map<HdInterpolation, HdPrimvarDescriptorVector>, VtArray<GfVec3f>&,
    VtIntArray&);

HdDirtyBits
HdCyclesMesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void
HdCyclesMesh::_InitRepr(TfToken const& reprToken, HdDirtyBits* dirtyBits)
{
}

void
HdCyclesMesh::_ComputeTangents(bool needsign)
{
    // This is likely deprecated now
    const ccl::AttributeSet& attributes = (m_useSubdivision && m_subdivEnabled)
                                              ? m_cyclesMesh->subd_attributes
                                              : m_cyclesMesh->attributes;

    ccl::Attribute* attr = attributes.find(ccl::ATTR_STD_UV);
    if (attr) {
        mikk_compute_tangents(attr->standard_name(ccl::ATTR_STD_UV),
                              m_cyclesMesh, needsign, true);
    }
}

void
HdCyclesMesh::_AddUVSet(TfToken name, VtValue uvs, ccl::Scene* scene,
                        HdInterpolation interpolation)
{
    ccl::AttributeSet* attributes = &m_cyclesMesh->attributes;
    bool subdivide_uvs = false;

    ccl::ustring uv_name      = ccl::ustring(name.GetString());
    ccl::ustring tangent_name = ccl::ustring(name.GetString() + ".tangent");

    bool need_uv = m_cyclesMesh->need_attribute(scene, uv_name)
                   || m_cyclesMesh->need_attribute(scene, ccl::ATTR_STD_UV);
    bool need_tangent
        = m_cyclesMesh->need_attribute(scene, tangent_name)
          || m_cyclesMesh->need_attribute(scene, ccl::ATTR_STD_UV_TANGENT);

    // Forced true for now... Should be based on shader compilation needs
    need_tangent = true;

    ccl::Attribute* attr = attributes->add(ccl::ATTR_STD_UV, uv_name);

    if (m_useSubdivision && subdivide_uvs && m_subdivEnabled)
        attr->flags |= ccl::ATTR_SUBDIVIDED;

    _PopulateAttribute(name, HdPrimvarRoleTokens->textureCoordinate,
                       interpolation, uvs, attr, this);

    if (need_tangent) {
        ccl::ustring sign_name = ccl::ustring(name.GetString()
                                              + ".tangent_sign");
        bool need_sign
            = m_cyclesMesh->need_attribute(scene, sign_name)
              || m_cyclesMesh->need_attribute(scene,
                                              ccl::ATTR_STD_UV_TANGENT_SIGN);


        // Forced for now
        need_sign = true;
        mikk_compute_tangents(name.GetString().c_str(), m_cyclesMesh, need_sign,
                              true);
    }
}

void
HdCyclesMesh::_AddVelocities(VtVec3fArray& velocities,
                             HdInterpolation interpolation)
{
    ccl::AttributeSet* attributes = (m_useSubdivision && m_subdivEnabled)
                                        ? &m_cyclesMesh->subd_attributes
                                        : &m_cyclesMesh->attributes;

    m_cyclesMesh->use_motion_blur = true;
    m_cyclesMesh->motion_steps    = 3;

    ccl::Attribute* attr_mP = attributes->find(
        ccl::ATTR_STD_MOTION_VERTEX_POSITION);

    if (attr_mP)
        attributes->remove(attr_mP);

    if (!attr_mP) {
        attr_mP = attributes->add(ccl::ATTR_STD_MOTION_VERTEX_POSITION);
    }
    //ccl::float3* vdata = attr_mP->data_float3();

    /*if (interpolation == HdInterpolationVertex) {
        VtIntArray::const_iterator idxIt = m_faceVertexIndices.begin();

        // TODO: Add support for subd faces?
        for (int i = 0; i < m_faceVertexCounts.size(); i++) {
            const int vCount = m_faceVertexCounts[i];

            for (int j = 1; j < vCount - 1; ++j) {
                int v0 = *idxIt;
                int v1 = *(idxIt + j + 0);
                int v2 = *(idxIt + j + 1);

                if (m_orientation == HdTokens->leftHanded) {
                    int temp = v2;
                    v2       = v0;
                    v0       = temp;
                }

                vdata[0] = vec3f_to_float3(velocities[v0]);
                vdata[1] = vec3f_to_float3(velocities[v1]);
                vdata[2] = vec3f_to_float3(velocities[v2]);
                vdata += 3;
            }
            idxIt += vCount;
        }
    } else {*/

    ccl::float3* mP = attr_mP->data_float3();

    for (size_t i = 0; i < m_cyclesMesh->motion_steps; ++i) {
        //VtVec3fArray pp;
        //pp = m_pointSamples.values.data()[i].Get<VtVec3fArray>();

        for (size_t j = 0; j < velocities.size(); ++j, ++mP) {
            *mP = vec3f_to_float3(m_points[j]
                                  + (velocities[j] * m_velocityScale));
        }
    }
}

void
HdCyclesMesh::_AddColors(TfToken name, TfToken role, VtValue colors,
                         ccl::Scene* scene, HdInterpolation interpolation)
{
    if (colors.IsEmpty())
        return;

    ccl::AttributeSet* attributes = &m_cyclesMesh->attributes;

    ccl::AttributeStandard vcol_std = ccl::ATTR_STD_VERTEX_COLOR;
    ccl::ustring vcol_name          = ccl::ustring(name.GetString());

    const bool need_vcol = m_cyclesMesh->need_attribute(scene, vcol_name)
                           || m_cyclesMesh->need_attribute(scene, vcol_std);

    // TODO: Maybe we move this to _PopulateAttributes as well?
    // seems generic enough. Although different types (uv/vel/cols)
    // require different handling...

    ccl::TypeDesc ctype;

    ccl::AttributeElement celem = ccl::ATTR_ELEMENT_NONE;

    switch (interpolation) {
    case HdInterpolationConstant: celem = ccl::ATTR_ELEMENT_MESH; break;
    case HdInterpolationVertex:
        if (attributes->geometry->type == ccl::Geometry::HAIR) {
            celem = ccl::ATTR_ELEMENT_CURVE_KEY;
        } else {
            celem = ccl::ATTR_ELEMENT_VERTEX;
        }
        break;
    case HdInterpolationVarying:
    case HdInterpolationFaceVarying: celem = ccl::ATTR_ELEMENT_CORNER; break;
    case HdInterpolationUniform: celem = ccl::ATTR_ELEMENT_FACE; break;
    default: break;
    }

    if (colors.IsHolding<VtArray<float>>()
        || colors.IsHolding<VtArray<double>>()
        || colors.IsHolding<VtArray<int>>()
        || colors.IsHolding<VtArray<bool>>()) {
        ctype = ccl::TypeDesc::TypeFloat;
    } else if (colors.IsHolding<VtArray<GfVec2f>>()
               || colors.IsHolding<VtArray<GfVec2d>>()
               || colors.IsHolding<VtArray<GfVec2i>>()) {
        ctype = ccl::TypeFloat2;
    } else if (colors.IsHolding<VtArray<GfVec3f>>()
               || colors.IsHolding<VtArray<GfVec3d>>()
               || colors.IsHolding<VtArray<GfVec3i>>()) {
        ctype = ccl::TypeDesc::TypeColor;

    } else if (colors.IsHolding<VtArray<GfVec4f>>()
               || colors.IsHolding<VtArray<GfVec4d>>()
               || colors.IsHolding<VtArray<GfVec4i>>()) {
        ctype = ccl::TypeDesc::TypeVector;
    }

    ccl::Attribute* vcol_attr = NULL;
    vcol_attr                 = attributes->add(vcol_name, ctype, celem);

    _PopulateAttribute(name, HdPrimvarRoleTokens->vector, interpolation, colors,
                       vcol_attr, this);

    if (name == HdTokens->displayColor
        && interpolation == HdInterpolationConstant) {
        ccl::float4 displayColor;

        if (colors.IsHolding<VtArray<float>>()) {
            displayColor = vec1f_to_float4(
                colors.UncheckedGet<VtArray<float>>()[0]);
        } else if (colors.IsHolding<VtArray<GfVec2f>>()) {
            displayColor = vec2f_to_float4(
                colors.UncheckedGet<VtArray<GfVec2f>>()[0]);

        } else if (colors.IsHolding<VtArray<GfVec3f>>()) {
            displayColor = vec3f_to_float4(
                colors.UncheckedGet<VtArray<GfVec3f>>()[0]);

        } else if (colors.IsHolding<VtArray<GfVec4f>>()) {
            displayColor = vec4f_to_float4(
                colors.UncheckedGet<VtArray<GfVec4f>>()[0]);
        } else {
            std::cout
                << "Invalid color size. Only float, vec2, vec3, and vec4 are supported. Found"
                << colors.GetTypeName() << "\n";
        }

        m_cyclesObject->color = ccl::make_float3(displayColor.x, displayColor.y,
                                                 displayColor.z);
    }
}

void
HdCyclesMesh::_AddNormals(VtVec3fArray& normals, HdInterpolation interpolation)
{
    ccl::AttributeSet& attributes = m_cyclesMesh->attributes;

    if (interpolation == HdInterpolationUniform) {
        ccl::Attribute* attr_fN = attributes.add(ccl::ATTR_STD_FACE_NORMAL);
        ccl::float3* fN         = attr_fN->data_float3();

        int idx = 0;
        auto& vertex_counts = GetFaceVertexCounts();
        for (int i = 0; i < vertex_counts.size(); i++) {
            const int vCount = vertex_counts[i];

            // This needs to be checked
            for (int j = 1; j < vCount - 1; ++idx) {
                fN[idx] = vec3f_to_float3(normals[idx]);
            }
        }

    } else if (interpolation == HdInterpolationVertex) {
        ccl::Attribute* attr = attributes.add(ccl::ATTR_STD_VERTEX_NORMAL);
        ccl::float3* cdata   = attr->data_float3();

        memset(cdata, 0, m_cyclesMesh->verts.size() * sizeof(ccl::float3));

        for (size_t i = 0; i < m_cyclesMesh->verts.size(); i++) {
            ccl::float3 n = vec3f_to_float3(normals[i]);
            if (m_orientation == HdTokens->leftHanded)
                n = -n;
            cdata[i] = n;
        }

    } else if (interpolation == HdInterpolationFaceVarying) {
        //ccl::Attribute* attr = attributes.add(ccl::ATTR_STD_VERTEX_NORMAL);
        //ccl::float3* cdata   = attr->data_float3();

        // TODO: For now, this method produces very wrong results. Some other solution will be needed

        m_cyclesMesh->add_face_normals();
        m_cyclesMesh->add_vertex_normals();

        return;

        //memset(cdata, 0, m_cyclesMesh->verts.size() * sizeof(ccl::float3));

        // Although looping through all faces, normals are averaged per
        // vertex. This seems to be a limitation of cycles. Not allowing
        // face varying/loop_normals/corner_normals natively.

        // For now, we add all corner normals and normalize separately.
        // TODO: Update when Cycles supports corner_normals
        /*for (size_t i = 0; i < m_numMeshFaces; i++) {
            for (size_t j = 0; j < 3; j++) {
                ccl::float3 n = vec3f_to_float3(normals[(i * 3) + j]);
                cdata[m_cyclesMesh->get_triangle(i).v[j]] += n;
            }
        }

        for (size_t i = 0; i < m_cyclesMesh->verts.size(); i++) {
            cdata[i] = ccl::normalize(cdata[i]);
        }*/
    }
}

ccl::Mesh*
HdCyclesMesh::_CreateCyclesMesh()
{
    ccl::Mesh* mesh = new ccl::Mesh();
    mesh->clear();

    if (m_useMotionBlur && m_useDeformMotionBlur) {
        mesh->use_motion_blur = true;
    }

    m_numMeshVerts = 0;
    m_numMeshFaces = 0;

    mesh->subdivision_type = ccl::Mesh::SUBDIVISION_NONE;
    return mesh;
}

ccl::Object*
HdCyclesMesh::_CreateCyclesObject()
{
    ccl::Object* object = new ccl::Object();

    object->tfm     = ccl::transform_identity();
    object->pass_id = -1;

    object->visibility = ccl::PATH_RAY_ALL_VISIBILITY;

    return object;
}

void
HdCyclesMesh::_PopulateVertices(HdSceneDelegate* sceneDelegate)
{
    auto points_value = GetPrimvar(sceneDelegate, HdTokens->points);

    VtVec3fArray points;
    VtValue refined_points_value = m_refiner->RefineVertexData(points_value);
    if(refined_points_value.IsHolding<VtVec3fArray>()) {
        points = refined_points_value.Get<VtVec3fArray>();
    }

    for(size_t i{}; i < points.size(); ++i) {
        auto& data = points[i];
        m_cyclesMesh->add_vertex(ccl::make_float3(data[0], data[1], data[2]));
    }
}

void
HdCyclesMesh::_PopulateMotion()
{
    if (m_pointSamples.count <= 1) {
        return;
    }

    ccl::AttributeSet* attributes = (m_useSubdivision)
                                        ? &m_cyclesMesh->subd_attributes
                                        : &m_cyclesMesh->attributes;

    m_cyclesMesh->use_motion_blur = true;

    m_cyclesMesh->motion_steps = m_pointSamples.count + 1;

    ccl::Attribute* attr_mP = attributes->find(
        ccl::ATTR_STD_MOTION_VERTEX_POSITION);

    if (attr_mP)
        attributes->remove(attr_mP);

    if (!attr_mP) {
        attr_mP = attributes->add(ccl::ATTR_STD_MOTION_VERTEX_POSITION);
    }

    ccl::float3* mP = attr_mP->data_float3();
    for (size_t i = 0; i < m_pointSamples.count; ++i) {
        if (m_pointSamples.times.data()[i] == 0.0f)
            continue;

        VtVec3fArray pp;
        pp = m_pointSamples.values.data()[i].Get<VtVec3fArray>();

        for (size_t j = 0; j < m_numMeshVerts; ++j, ++mP) {
            *mP = vec3f_to_float3(pp[j]);
        }
    }
}

void
HdCyclesMesh::_PopulateFaces(VtIntArray& input_material_ids)
{
    // allocate mesh
    m_cyclesMesh->reserve_mesh(m_refiner->GetNumVertices(),
                               m_refiner->GetNumTriangles());

    // refine refined_indices, has to be run first to precompute primitiveParam
    const VtVec3iArray& refined_indices = m_refiner->RefinedIndices();

    // refine materials per face
    VtIntArray material_ids;
    auto refined_materials_value = m_refiner->RefineUniformData(VtValue{ input_material_ids });
    if(refined_materials_value.IsHolding<VtIntArray>()) {
        material_ids =refined_materials_value.Get<VtIntArray>();
    }

    for(size_t i{}; i < refined_indices.size(); ++i) {
        auto material_id = i < material_ids.size() ? material_ids[i] : 0;
        m_cyclesMesh->add_triangle(refined_indices[i][0],
                                   refined_indices[i][1],
                                   refined_indices[i][2],
                                   material_id, true);
    }
}

void
HdCyclesMesh::_PopulateGenerated(ccl::Scene* scene)
{
    if (m_cyclesMesh->need_attribute(scene, ccl::ATTR_STD_GENERATED)) {
        ccl::float3 loc, size;
        HdCyclesMeshTextureSpace(m_cyclesMesh, loc, size);

        ccl::AttributeSet* attributes = (m_useSubdivision)
                                            ? &m_cyclesMesh->subd_attributes
                                            : &m_cyclesMesh->attributes;
        ccl::Attribute* attr = attributes->add(ccl::ATTR_STD_GENERATED);

        ccl::float3* generated = attr->data_float3();
        for (int i = 0; i < m_cyclesMesh->verts.size(); i++) {
            generated[i] = m_cyclesMesh->verts[i] * size - loc;
        }
    }
}

void
HdCyclesMesh::_FinishMesh(ccl::Scene* scene)
{
    // Deprecated in favour of adding when uv's are added
    // This should no longer be necessary
    //_ComputeTangents(true);

    // This must be done first, because HdCyclesMeshTextureSpace requires computed min/max
    m_cyclesMesh->compute_bounds();

    _PopulateGenerated(scene);
}

void
HdCyclesMesh::Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
                   HdDirtyBits* dirtyBits, TfToken const& reprToken)
{
    HdCyclesRenderParam* param = (HdCyclesRenderParam*)renderParam;
    ccl::Scene* scene          = param->GetCyclesScene();

    std::lock_guard<decltype(scene->mutex)> scene_lock{scene->mutex};

    const SdfPath& id = GetId();

    // -------------------------------------
    // -- Pull scene data

    bool mesh_updated = false;

    bool newMesh = false;

    bool pointsIsComputed = false;

    // This is needed for USD Skel, however is currently buggy...
    auto extComputationDescs
        = sceneDelegate->GetExtComputationPrimvarDescriptors(
            id, HdInterpolationVertex);
    for (auto& desc : extComputationDescs) {
        if (desc.name != HdTokens->points)
            continue;

        if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, desc.name)) {
            mesh_updated    = true;
            auto valueStore = HdExtComputationUtils::GetComputedPrimvarValues(
                { desc }, sceneDelegate);
            auto pointValueIt = valueStore.find(desc.name);
            if (pointValueIt != valueStore.end()) {
                if (!pointValueIt->second.IsEmpty()) {
                    m_points       = pointValueIt->second.Get<VtVec3fArray>();
                    m_numMeshVerts = m_points.size();

                    m_normalsValid   = false;
                    pointsIsComputed = true;
                    newMesh          = true;
                }
            }
        }
        break;
    }

//    if (!pointsIsComputed
//        && HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
//        mesh_updated        = true;
//        VtValue pointsValue = sceneDelegate->Get(id, HdTokens->points);
//        if (!pointsValue.IsEmpty()) {
//            m_points = pointsValue.Get<VtVec3fArray>();
//            if (m_points.size() > 0) {
//                m_numMeshVerts = m_points.size();
//
//                m_normalsValid = false;
//                newMesh        = true;
//            }
//
//            // TODO: Should we check if time varying?
//            // TODO: can we use this for m_points too?
//            sceneDelegate->SamplePrimvar(id, HdTokens->points, &m_pointSamples);
//        }
//    }

    static const HdCyclesConfig& config = HdCyclesConfig::GetInstance();

    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id) ||
        HdChangeTracker::IsSubdivTagsDirty(*dirtyBits, id) ||
        HdChangeTracker::IsDisplayStyleDirty(*dirtyBits, id)) {
        HdDisplayStyle display_style = sceneDelegate->GetDisplayStyle(id);

        // topology can not outlive the refiner
        m_topology = GetMeshTopology(sceneDelegate);
        m_refiner = HdCyclesMeshRefiner::Create(m_topology, 2, id);

        m_adjacencyValid = false;
        m_normalsValid   = false;
        newMesh = true;
    }

    std::map<HdInterpolation, HdPrimvarDescriptorVector>
        primvarDescsPerInterpolation = {
            { HdInterpolationFaceVarying, sceneDelegate->GetPrimvarDescriptors(
                                              id, HdInterpolationFaceVarying) },
            { HdInterpolationVertex,
              sceneDelegate->GetPrimvarDescriptors(id, HdInterpolationVertex) },
            { HdInterpolationConstant,
              sceneDelegate->GetPrimvarDescriptors(id,
                                                   HdInterpolationConstant) },
            { HdInterpolationUniform,
              sceneDelegate->GetPrimvarDescriptors(id, HdInterpolationUniform) },

        };

    if (*dirtyBits & HdChangeTracker::DirtyDoubleSided) {
        mesh_updated  = true;
        m_doubleSided = sceneDelegate->GetDoubleSided(id);
    }

    // -------------------------------------
    // -- Resolve Drawstyles

    bool isRefineLevelDirty = false;
    if (*dirtyBits & HdChangeTracker::DirtyDisplayStyle) {
        mesh_updated = true;

        m_displayStyle = sceneDelegate->GetDisplayStyle(id);
        if (m_refineLevel != m_displayStyle.refineLevel) {
            isRefineLevelDirty = true;
            m_refineLevel      = m_displayStyle.refineLevel;
            newMesh            = true;
        }
    }


#ifdef USE_USD_CYCLES_SCHEMA
    for (auto& primvarDescsEntry : primvarDescsPerInterpolation) {
        for (auto& pv : primvarDescsEntry.second) {
            // Mesh Specific

            m_useMotionBlur = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectMblur, m_useMotionBlur);

            m_useDeformMotionBlur = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectMblurDeform,
                m_useDeformMotionBlur);

            m_motionSteps = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectMblurSteps, m_motionSteps);

            TfToken subdivisionType = usdCyclesTokens->catmull_clark;

            subdivisionType = _HdCyclesGetMeshParam<TfToken>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesMeshSubdivision_type,
                subdivisionType);

            if (subdivisionType == usdCyclesTokens->catmull_clark) {
                m_cyclesMesh->subdivision_type
                    = ccl::Mesh::SUBDIVISION_CATMULL_CLARK;
            } else if (subdivisionType == usdCyclesTokens->linear) {
                m_cyclesMesh->subdivision_type = ccl::Mesh::SUBDIVISION_LINEAR;
            } else {
                m_cyclesMesh->subdivision_type = ccl::Mesh::SUBDIVISION_NONE;
            }

            m_dicingRate = _HdCyclesGetMeshParam<float>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesMeshDicingRate, m_dicingRate);

            m_maxSubdivision = _HdCyclesGetMeshParam<int>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesMeshSubdivision_max_level,
                m_maxSubdivision);

            // Object Generic

            m_cyclesObject->is_shadow_catcher = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectIs_shadow_catcher,
                m_cyclesObject->is_shadow_catcher);

            m_cyclesObject->pass_id = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectPass_id,
                m_cyclesObject->pass_id);

            m_cyclesObject->use_holdout = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectUse_holdout,
                m_cyclesObject->use_holdout);

            // Visibility

            m_visibilityFlags = 0;

            m_visCamera = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectVisibilityCamera,
                m_visCamera);

            m_visDiffuse = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectVisibilityDiffuse,
                m_visDiffuse);

            m_visGlossy = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectVisibilityGlossy,
                m_visGlossy);

            m_visScatter = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectVisibilityScatter,
                m_visScatter);

            m_visShadow = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectVisibilityShadow,
                m_visShadow);

            m_visTransmission = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectVisibilityTransmission,
                m_visTransmission);

            m_visibilityFlags |= m_visCamera ? ccl::PATH_RAY_CAMERA : 0;
            m_visibilityFlags |= m_visDiffuse ? ccl::PATH_RAY_DIFFUSE : 0;
            m_visibilityFlags |= m_visGlossy ? ccl::PATH_RAY_GLOSSY : 0;
            m_visibilityFlags |= m_visScatter ? ccl::PATH_RAY_VOLUME_SCATTER
                                              : 0;
            m_visibilityFlags |= m_visShadow ? ccl::PATH_RAY_SHADOW : 0;
            m_visibilityFlags |= m_visTransmission ? ccl::PATH_RAY_TRANSMIT : 0;

            mesh_updated = true;
        }
    }
#endif

    // -------------------------------------
    // -- Create Cycles Mesh

    if (newMesh) {
        m_cyclesMesh->clear();

        VtIntArray faceMaterials;
        faceMaterials.resize(m_topology.GetNumFaces());
        for (auto const& subset : m_topology.GetGeomSubsets()) {
            int subsetMaterialIndex = 0;

            if (!subset.materialId.IsEmpty()) {
                HdRenderIndex& render_index = sceneDelegate->GetRenderIndex();
                HdSprim* state_prim = render_index.GetSprim(HdPrimTypeTokens->material, subset.materialId);
                auto subMat = dynamic_cast<const HdCyclesMaterial*>(state_prim);
                if (subMat && subMat->GetCyclesShader()) {
                    if (m_materialMap.find(subset.materialId) == m_materialMap.end()) {
                        m_usedShaders.push_back(subMat->GetCyclesShader());
                        subMat->GetCyclesShader()->tag_update(scene);
                        m_materialMap.emplace(subset.materialId, m_usedShaders.size());
                        subsetMaterialIndex = m_usedShaders.size();
                    } else {
                        subsetMaterialIndex = m_materialMap.at(subset.materialId);
                    }
                    m_cyclesMesh->used_shaders = m_usedShaders;
                }
            }

            for (int i : subset.indices) {
                faceMaterials[i] = std::max(subsetMaterialIndex - 1, 0);
            }
        }

        _PopulateFaces(faceMaterials);
        _PopulateVertices(sceneDelegate);

        if (m_useMotionBlur && m_useDeformMotionBlur)
            _PopulateMotion();

        // Ingest mesh primvars (data, not schema)
        for (auto& primvarDescsEntry : primvarDescsPerInterpolation) {
            for (auto& pv : primvarDescsEntry.second) {
                if (!HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, pv.name)) {
                    continue;
                }

                auto value = GetPrimvar(sceneDelegate, pv.name);
                auto& interpolation = primvarDescsEntry.first;

                // - Normals
                if (pv.name == HdTokens->normals || pv.role == HdPrimvarRoleTokens->normal) {
                    auto refined_value = m_refiner->RefineData(value, interpolation);
                    if(refined_value.GetArraySize() > 0 && refined_value.IsHolding<VtVec3fArray>()) {
                        auto normals = refined_value.Get<VtVec3fArray>();
                        _AddNormals(normals, primvarDescsEntry.first);
                        mesh_updated = true;
                    } else {
                        TF_CODING_WARNING("Failed to compute normals!");
                    }

                    continue;
                }

                // - Velocities
                if(pv.name == HdTokens->velocities) {

                    continue;
                }

                // - Texture Coordinates
                if (pv.role == HdPrimvarRoleTokens->textureCoordinate) {
                    auto refined_value = m_refiner->RefineData(value, interpolation);
                    if(refined_value.GetArraySize() >= value.GetArraySize()) {
                        _AddUVSet(pv.name, refined_value, scene, primvarDescsEntry.first);
                        mesh_updated = true;
                    } else {
                        TF_CODING_WARNING("Failed to compute texture coordinates!");
                    }

                    continue;
                }

                // - Colors
                if(pv.name == HdTokens->displayColor || pv.role == HdPrimvarRoleTokens->color) {
                    auto refined_value = m_refiner->RefineData(value, interpolation);
                    if(refined_value.GetArraySize() >= value.GetArraySize()) {
                        _AddColors(pv.name, pv.role, refined_value, scene, interpolation);
                    } else {
                        TF_CODING_WARNING("Failed to compute colors!");
                    }

                    // This swaps the default_surface to one that uses displayColor for diffuse
                    if (pv.name == HdTokens->displayColor) {
                        m_hasVertexColors = true;
                    }
                    mesh_updated = true;

                    continue;
                }
            }
        }

        // Apply existing shaders
        if (m_usedShaders.size() > 0)
            m_cyclesMesh->used_shaders = m_usedShaders;
    }

    if (*dirtyBits & HdChangeTracker::DirtyTransform) {
        // This causes a known slowdown to deforming motion blur renders
        // This will be addressed in an upcoming PR
        m_transformSamples = HdCyclesSetTransform(m_cyclesObject, sceneDelegate,
                                                  id, m_useMotionBlur);

        if (m_cyclesMesh && m_cyclesMesh->subd_params) {
            m_cyclesMesh->subd_params->objecttoworld = m_cyclesObject->tfm;
        }

        mesh_updated = true;
    }

    ccl::Shader* fallbackShader = scene->default_surface;

    if (m_hasVertexColors) {
        fallbackShader = param->default_vcol_surface;
    }

    if (*dirtyBits & HdChangeTracker::DirtyPrimID) {
        // Offset of 1 added because Cycles primId pass needs to be shifted down to -1
        m_cyclesObject->pass_id = this->GetPrimId() + 1;
    }

    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        // We probably need to clear this array, however putting this here,
        // breaks some IPR sessions
        // m_usedShaders.clear();

        if (m_cyclesMesh) {
            m_cachedMaterialId = sceneDelegate->GetMaterialId(id);
            if (GetFaceVertexCounts().size() > 0) {
                if (!m_cachedMaterialId.IsEmpty()) {
                    const HdCyclesMaterial* material
                        = static_cast<const HdCyclesMaterial*>(
                            sceneDelegate->GetRenderIndex().GetSprim(
                                HdPrimTypeTokens->material, m_cachedMaterialId));

                    if (material && material->GetCyclesShader()) {
                        m_usedShaders.push_back(material->GetCyclesShader());

                        material->GetCyclesShader()->tag_update(scene);
                    } else {
                        m_usedShaders.push_back(fallbackShader);
                    }
                } else {
                    m_usedShaders.push_back(fallbackShader);
                }

                m_cyclesMesh->used_shaders = m_usedShaders;
            }
        }
    }

    if (*dirtyBits & HdChangeTracker::DirtyVisibility) {
        mesh_updated        = true;
        _sharedData.visible = sceneDelegate->GetVisible(id);
    }

    // -------------------------------------
    // -- Handle point instances

    if (newMesh || (*dirtyBits & HdChangeTracker::DirtyInstancer)) {
        mesh_updated = true;
        if (auto instancer = static_cast<HdCyclesInstancer*>(
                sceneDelegate->GetRenderIndex().GetInstancer(
                    GetInstancerId()))) {
            auto instanceTransforms = instancer->SampleInstanceTransforms(id);
            auto newNumInstances    = (instanceTransforms.count > 0)
                                       ? instanceTransforms.values[0].size()
                                       : 0;
            // Clear all instances...
            if (m_cyclesInstances.size() > 0) {
                for (auto instance : m_cyclesInstances) {
                    if (instance) {
                        m_renderDelegate->GetCyclesRenderParam()->RemoveObject(
                            instance);
                        delete instance;
                    }
                }
                m_cyclesInstances.clear();
            }

            if (newNumInstances != 0) {
                std::vector<TfSmallVector<GfMatrix4d, 1>> combinedTransforms;
                combinedTransforms.reserve(newNumInstances);
                for (size_t i = 0; i < newNumInstances; ++i) {
                    // Apply prototype transform (m_transformSamples) to all the instances
                    combinedTransforms.emplace_back(instanceTransforms.count);
                    auto& instanceTransform = combinedTransforms.back();

                    if (m_transformSamples.count == 0
                        || (m_transformSamples.count == 1
                            && (m_transformSamples.values[0]
                                == GfMatrix4d(1)))) {
                        for (size_t j = 0; j < instanceTransforms.count; ++j) {
                            instanceTransform[j]
                                = instanceTransforms.values[j][i];
                        }
                    } else {
                        for (size_t j = 0; j < instanceTransforms.count; ++j) {
                            GfMatrix4d xf_j = m_transformSamples.Resample(
                                instanceTransforms.times[j]);
                            instanceTransform[j]
                                = xf_j * instanceTransforms.values[j][i];
                        }
                    }
                }

                for (int j = 0; j < newNumInstances; ++j) {
                    ccl::Object* instanceObj = _CreateCyclesObject();

                    instanceObj->tfm = mat4d_to_transform(
                        combinedTransforms[j].data()[0]);
                    instanceObj->geometry = m_cyclesMesh;

                    // TODO: Implement motion blur for point instanced objects
                    /*if (m_useMotionBlur) {
                        m_cyclesMesh->motion_steps    = m_motionSteps;
                        m_cyclesMesh->use_motion_blur = m_useMotionBlur;

                        instanceObj->motion.clear();
                        instanceObj->motion.resize(m_motionSteps);
                        for (int j = 0; j < m_motionSteps; j++) {
                            instanceObj->motion[j] = mat4d_to_transform(
                                combinedTransforms[j].data()[j]);
                        }
                    }*/

                    m_cyclesInstances.push_back(instanceObj);

                    m_renderDelegate->GetCyclesRenderParam()->AddObject(
                        instanceObj);
                }

                // Hide prototype
                if (m_cyclesObject)
                    m_visibilityFlags = 0;
            }
        }
    }

    // -------------------------------------
    // -- Finish Mesh

    if (newMesh && m_cyclesMesh) {
        _FinishMesh(scene);
    }

    if (mesh_updated || newMesh) {
        m_cyclesObject->visibility = m_visibilityFlags;
        if (!_sharedData.visible)
            m_cyclesObject->visibility = 0;

        m_cyclesMesh->tag_update(scene, false);
        m_cyclesObject->tag_update(scene);
        param->Interrupt();
    }

    *dirtyBits = HdChangeTracker::Clean;
}

const VtIntArray& HdCyclesMesh::GetFaceVertexCounts() const
{
    return m_refiner->RefinedCounts();
}

const VtVec3iArray& HdCyclesMesh::GetFaceVertexIndices() const {
    return m_refiner->RefinedIndices();
}

PXR_NAMESPACE_CLOSE_SCOPE

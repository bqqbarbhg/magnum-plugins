/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
                2020, 2021, 2022 Vladimír Vondruš <mosra@centrum.cz>
    Copyright © 2022 Samuli Raivio <bqqbarbhg@gmail.com>

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include "UfbxImporter.h"

#define UFBX_NO_INDEX_GENERATION
#define UFBX_NO_GEOMETRY_CACHE
#define UFBX_NO_TESSELLATION
#define UFBX_NO_SUBDIVISION
#define UFBX_NO_SCENE_EVALUATION
#define UFBX_NO_SKINNING_EVALUATION

/* Include error stack on debug builds for juicy details in bug reports */
#if defined(CORRADE_IS_DEBUG_BUILD) || !defined(NDEBUG)
    #define UFBX_ENABLE_ERROR_STACK
#else
    #define UFBX_NO_ERROR_STACK
#endif

/* Embed ufbx into the translation unit as static, allows for more optimizations
   and users to have an alternate version of ufbx loaded simultaneously */
#define ufbx_abi static
#include "ufbx.h"
#include "ufbx.c"

#include "UfbxMaterials.h"

#include <unordered_map>
#include <Corrade/Containers/Array.h>
#include <Corrade/Containers/ArrayTuple.h>
#include <Corrade/Containers/BitArray.h>
#include <Corrade/Containers/StaticArray.h>
#include <Corrade/Containers/GrowableArray.h>
#include <Corrade/Containers/Optional.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/Math.h>
#include <Corrade/Utility/Path.h>
#include <Magnum/FileCallback.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/RectangularMatrix.h>
#include <Magnum/Math/Vector3.h>
#include <Magnum/Math/Quaternion.h>
#include <Magnum/Trade/MeshData.h>
#include <Magnum/Trade/CameraData.h>
#include <Magnum/Trade/MaterialData.h>
#include <Magnum/Trade/TextureData.h>
#include <Magnum/Trade/ImageData.h>
#include <Magnum/Trade/LightData.h>
#include <Magnum/Trade/SceneData.h>
#include <Magnum/MeshTools/RemoveDuplicates.h>
#include <MagnumPlugins/AnyImageImporter/AnyImageImporter.h>

namespace Corrade { namespace Containers { namespace Implementation {

template<> struct StringConverter<ufbx_string> {
    static String from(const ufbx_string& other) {
        return String{other.data, other.length};
    }
};
template<> struct StringViewConverter<const char, ufbx_string> {
    static StringView from(const ufbx_string& other) {
        return StringView{other.data, other.length, Containers::StringViewFlag::NullTerminated};
    }
    static ufbx_string to(StringView other) {
        return ufbx_string{other.data(), other.size()};
    }
};

}}}

namespace Magnum { namespace Math { namespace Implementation {

template<> struct VectorConverter<2, Float, ufbx_vec2> {
    static Vector<2, Float> from(const ufbx_vec2& other) {
        return {Float(other.x), Float(other.y)};
    }
};

template<> struct VectorConverter<3, Float, ufbx_vec3> {
    static Vector<3, Float> from(const ufbx_vec3& other) {
        return {Float(other.x), Float(other.y), Float(other.z)};
    }
};

template<> struct VectorConverter<4, Float, ufbx_vec4> {
    static Vector<4, Float> from(const ufbx_vec4& other) {
        return {Float(other.x), Float(other.y), Float(other.z), Float(other.w)};
    }
};

template<> struct QuaternionConverter<Float, ufbx_quat> {
    constexpr static Quaternion<Float> from(const ufbx_quat& other) {
        return {{Float(other.x), Float(other.y), Float(other.z)}, Float(other.w)};
    }
};

template<> struct VectorConverter<2, Double, ufbx_vec2> {
    static Vector<2, Double> from(const ufbx_vec2& other) {
        return {other.x, other.y};
    }
};

template<> struct VectorConverter<3, Double, ufbx_vec3> {
    static Vector<3, Double> from(const ufbx_vec3& other) {
        return {other.x, other.y, other.z};
    }
};

template<> struct VectorConverter<4, Double, ufbx_vec4> {
    static Vector<4, Double> from(const ufbx_vec4& other) {
        return {other.x, other.y, other.z, other.w};
    }
};

template<> struct QuaternionConverter<Double, ufbx_quat> {
    constexpr static Quaternion<Double> from(const ufbx_quat& other) {
        return {{other.x, other.y, other.z}, other.w};
    }
};

template<> struct RectangularMatrixConverter<4, 3, Double, ufbx_matrix> {
    constexpr static RectangularMatrix<4, 3, Double> from(const ufbx_matrix& other) {
        return RectangularMatrix<4, 3, Double>{
            Vector3d(other.cols[0]),
            Vector3d(other.cols[1]),
            Vector3d(other.cols[2]),
            Vector3d(other.cols[3]),
        };
    }
};

}}}

namespace Magnum { namespace Trade {

using namespace Containers::Literals;

namespace {

constexpr SceneField SceneFieldVisibility = sceneFieldCustom(0);
constexpr SceneField SceneFieldGeometryTransformHelper = sceneFieldCustom(1);
constexpr SceneField SceneFieldGeometryTranslation = sceneFieldCustom(2);
constexpr SceneField SceneFieldGeometryRotation = sceneFieldCustom(3);
constexpr SceneField SceneFieldGeometryScaling = sceneFieldCustom(4);

constexpr Containers::StringView sceneFieldNames[] = {
    "Visibility"_s,
    "GeometryTransformHelper"_s,
    "GeometryTranslation"_s,
    "GeometryRotation"_s,
    "GeometryScaling"_s,
};

bool getLoadOptsFromConfiguration(ufbx_load_opts& opts, Utility::ConfigurationGroup& conf, const char* errorPrefix) {
    const Long maxTemporaryMemory = conf.value<Long>("maxTemporaryMemory");
    const Long maxResultMemory = conf.value<Long>("maxResultMemory");
    const std::string geometryTransformHandling = conf.value("geometryTransformHandling");
    const std::string unitNormalizationHandling = conf.value("unitNormalizationHandling");
    const bool normalizeUnits = conf.value<bool>("normalizeUnits");

    opts.generate_missing_normals = conf.value<bool>("generateMissingNormals");
    opts.strict = conf.value<bool>("strict");
    opts.disable_quirks = conf.value<bool>("disableQuirks");
    opts.load_external_files = conf.value<bool>("loadExternalFiles");
    opts.ignore_geometry = conf.value<bool>("ignoreGeometry");
    opts.ignore_animation = conf.value<bool>("ignoreAnimation");
    opts.ignore_embedded = conf.value<bool>("ignoreEmbedded");
    opts.ignore_all_content = conf.value<bool>("ignoreAllContent");
    opts.ignore_missing_external_files = true;

    /* Substitute zero maximum memory to one, so that if the user computes the
       maximum memory and ends up with zero it doesn't result in unlimited */
    if(maxTemporaryMemory >= 0)
        opts.temp_allocator.memory_limit = size_t(Utility::max(maxTemporaryMemory, Long(1)));
    if(maxResultMemory >= 0)
        opts.result_allocator.memory_limit = size_t(Utility::max(maxResultMemory, Long(1)));

    /* By default FBX has cameras pointing at +X and lights at -Y, let ufbx
       normalize those to what Magnum expects */
    opts.target_light_axes = ufbx_axes_right_handed_y_up;
    opts.target_camera_axes = ufbx_axes_right_handed_y_up;

    /* @todo expose more of these as options? need to think of reasonable
       defaults anyways, feels like ignoring geometry transform is not great */
    if(unitNormalizationHandling == "transformRoot") {
        opts.space_conversion = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;
    } else if(unitNormalizationHandling == "adjustTransforms") {
        opts.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
    } else {
        Error{} << errorPrefix << "Unsupported unitNormalizationHandling configuration:" << Containers::StringView(unitNormalizationHandling);
        return false;
    }

    if(geometryTransformHandling == "preserve") {
        opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_PRESERVE;
    } else if(geometryTransformHandling == "helperNodes") {
        opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
    } else if(geometryTransformHandling == "modifyGeometry") {
        opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    } else {
        Error{} << errorPrefix << "Unsupported geometryTransformHandling configuration:" << Containers::StringView(geometryTransformHandling);
        return false;
    }

    if(normalizeUnits) {
        opts.target_axes = ufbx_axes_right_handed_y_up;
        opts.target_unit_meters = 1.0f;
    }

    /* We need to split meshes by material so create a dummy ufbx_mesh_material
       containing the whole mesh to make processing code simpler. */
    opts.allow_null_material = true;

    return true;
}

inline Int typedId(const ufbx_element* element) {
    return element ? Int(element->typed_id) : -1;
}

inline void logError(const char* prefix, const ufbx_error& error, ImporterFlags flags) {
    if(flags & ImporterFlag::Verbose) {
        char message[1024];
        ufbx_format_error(message, sizeof(message), &error);
        Error{Utility::Debug::Flag::NoSpace|Utility::Debug::Flag::NoNewlineAtTheEnd} << prefix << message;
    } else if(error.info_length > 0) {
        Error{Utility::Debug::Flag::NoSpace} << prefix << Containers::StringView(error.description) << ": " << Containers::StringView(error.info, error.info_length);
    } else {
        Error{Utility::Debug::Flag::NoSpace} << prefix << Containers::StringView(error.description);
    }
}

struct FileOpener {
    FileOpener(): _callback{nullptr}, _userData{nullptr} {}
    explicit FileOpener(Containers::Optional<Containers::ArrayView<const char>>(*callback)(const std::string&, InputFileCallbackPolicy, void*), void* userData): _callback{callback}, _userData{userData} {}

    bool operator()(ufbx_stream* stream, const char* path, size_t path_len, const ufbx_open_file_info* info)
    {
        /* We should never try to load geometry caches as they are disabled at
           compile time */
        CORRADE_INTERNAL_ASSERT(info->type != UFBX_OPEN_FILE_GEOMETRY_CACHE);

        /* If we don't have a callback just defer to ufbx file loading */
        if(!_callback) return ufbx_open_file(stream, path, path_len);

        std::string file{path, path_len};
        const Containers::Optional<Containers::ArrayView<const char>> data = _callback(file, InputFileCallbackPolicy::LoadTemporary, _userData);
        if(!data) return false;

        ufbx_open_memory_opts opts = {};
        opts.allocator.allocator = info->temp_allocator;

        /* We don't need to copy the file data as it's guaranteed to live for
           the duration of the load function we are currently executing */
        opts.no_copy = true;

        return ufbx_open_memory(stream, data->data(), data->size(), &opts, nullptr);
    }

    Containers::Optional<Containers::ArrayView<const char>> (*_callback)(const std::string&, InputFileCallbackPolicy, void*);
    void* _userData;
};

struct MeshChunk {
    /* ufbx_scene::meshes[] */
    UnsignedInt meshId;
    /* ufbx_mesh::materials[] (NOT ufbx_scene::materials[] !) */
    UnsignedInt meshMaterialIndex;
    /* Faces are filtered based on the primitive type */
    MeshPrimitive primitive;
};

struct FileTexture {
    /* ufbx_scene::textures[] */
    UnsignedInt textureIndex;
    /* ufbx_scene::file_textures[] */
    UnsignedInt fileTextureIndex;
};

struct MeshChunkMapping {
    /* Index range within State::meshChunks[] */
    UnsignedInt baseIndex, count;
};

}

struct UfbxImporter::State {
    ufbx_scene_ref scene;

    /* Meshes split by material */
    Containers::Array<MeshChunk> meshChunks;

    /* Mapping from ufbx_scene::meshes[] -> State::meshChunks[] */
    Containers::Array<MeshChunkMapping> meshChunkMapping;

    /* Offset subtracted from ufbx IDs to UfbxImporter::object() IDs, usually
       one as the root node is excluded */
    UnsignedInt nodeIdOffset = 0;
    UnsignedInt objectCount = 0;

    /* true if loaded from openFile(), false from openData() */
    bool fromFile = false;

    /* Name to ufbx_scene::texture_files[] */
    std::unordered_map<std::string, UnsignedInt> imageNameMap;

    /* Textures that have actual files */
    Containers::Array<FileTexture> textures;

    /* ufbx_scene::textures[] to State::textures[] */
    Containers::Array<Int> textureRemap;

    /* Cached AnyImageImporter for image2D() and image2DLevelCount */
    UnsignedInt imageImporterId = ~UnsignedInt{};
    Containers::Optional<AnyImageImporter> imageImporter;

    /* If true preserve the implicit root node */
    bool preserveRootNode = false;
};

UfbxImporter::UfbxImporter(PluginManager::AbstractManager& manager, const Containers::StringView& plugin): AbstractImporter{manager, plugin} {
}

UfbxImporter::~UfbxImporter() = default;

ImporterFeatures UfbxImporter::doFeatures() const { return ImporterFeature::OpenData|ImporterFeature::FileCallback; }

bool UfbxImporter::doIsOpened() const { return !!_state; }

void UfbxImporter::doClose() { _state = nullptr; }

void UfbxImporter::doOpenData(Containers::Array<char>&& data, const DataFlags) {
    ufbx_load_opts opts = { };
    if (!getLoadOptsFromConfiguration(opts, configuration(), "Trade::UfbxImporter::openData():"))
        return;

    FileOpener opener{fileCallback(), fileCallbackUserData()};
    opts.open_file_cb = &opener;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_memory(data.data(), data.size(), &opts, &error);
    if(!scene) {
        logError("Trade::UfbxImporter::openData(): loading failed: ", error, flags());
        return;
    }

    openInternal(scene, &opts, {});
}

void UfbxImporter::doOpenFile(Containers::StringView filename) {
    ufbx_load_opts opts = { };
    if (!getLoadOptsFromConfiguration(opts, configuration(), "Trade::UfbxImporter::openFile():"))
        return;

    FileOpener opener{fileCallback(), fileCallbackUserData()};
    opts.open_file_cb = &opener;

    opts.filename = filename;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file_len(filename.data(), filename.size(), &opts, &error);
    if(!scene) {
        logError("Trade::UfbxImporter::openFile(): loading failed: ", error, flags());
        return;
    }

    openInternal(scene, &opts, true);
}

void UfbxImporter::openInternal(void* opaqueScene, const void* opaqueOpts, bool fromFile) {
    ufbx_scene* scene = static_cast<ufbx_scene*>(opaqueScene);
    const ufbx_load_opts& opts = *static_cast<const ufbx_load_opts*>(opaqueOpts);

    Containers::StringView warningPrefix = fromFile ? "Trade::UfbxImporter::openFile(): "_s : "Trade::UfbxImporter::openData(): "_s;
    for(const ufbx_warning& warning : scene->metadata.warnings) {
        if(warning.count > 1)
            Warning{Utility::Debug::Flag::NoSpace} << warningPrefix << Containers::StringView(warning.description) << " (x" << warning.count << ")";
        else
            Warning{Utility::Debug::Flag::NoSpace} << warningPrefix << Containers::StringView(warning.description);
    }

    _state.reset(new State{});
    if(opts.space_conversion == UFBX_SPACE_CONVERSION_TRANSFORM_ROOT)
        _state->preserveRootNode = true;

    _state->fromFile = fromFile;
    _state->scene = ufbx_scene_ref{scene};

    /* Split meshes into chunks by material, ufbx_mesh::materials[] has always
       at least one material as we use ufbx_load_opts::allow_null_material. */
    arrayResize(_state->meshChunkMapping, UnsignedInt(scene->meshes.count));
    for(UnsignedInt i = 0; i < scene->meshes.count; ++i) {
        const ufbx_mesh* mesh = scene->meshes[i];

        MeshChunkMapping& mapping = _state->meshChunkMapping[i];
        mapping.baseIndex = UnsignedInt(_state->meshChunks.size());
        for(UnsignedInt j = 0; j < mesh->materials.count; ++j) {
            const ufbx_mesh_material& mat = mesh->materials[j];

            if(mat.num_point_faces > 0)
                arrayAppend(_state->meshChunks, {mesh->typed_id, j, MeshPrimitive::Points});
            if(mat.num_line_faces > 0)
                arrayAppend(_state->meshChunks, {mesh->typed_id, j, MeshPrimitive::Lines});
            if(mat.num_triangles > 0)
                arrayAppend(_state->meshChunks, {mesh->typed_id, j, MeshPrimitive::Triangles});
        }

        mapping.count = UnsignedInt(_state->meshChunks.size()) - mapping.baseIndex;
    }

    /* Count the final number of nodes in the scene as we may remove the root. */
    _state->nodeIdOffset = 0;
    _state->objectCount = UnsignedInt(scene->nodes.count);
    if(!_state->preserveRootNode) {
        --_state->objectCount;
        ++_state->nodeIdOffset;
    }

    /* Filter out textures that don't have any file associated with them. */
    arrayResize(_state->textureRemap, scene->textures.count, -1);
    for(const ufbx_texture* texture : scene->textures) {
        if(!texture->has_file) continue;

        const UnsignedInt id = texture->typed_id;
        _state->textureRemap[id] = Int(_state->textures.size());
        arrayAppend(_state->textures, {id, texture->file_index});
    }

    for(UnsignedInt i = 0; i < scene->texture_files.count; ++i) {
        const ufbx_string name = scene->texture_files[i].relative_filename;
        if(name.length == 0) continue;

        _state->imageNameMap.emplace(std::string(name.data, name.length), i);
    }
}

Int UfbxImporter::doDefaultScene() const { return 0; }

UnsignedInt UfbxImporter::doSceneCount() const { return 1; }

Containers::Optional<SceneData> UfbxImporter::doScene(UnsignedInt) {
    const ufbx_scene* scene = _state->scene.get();

    const bool retainGeometryTransforms = configuration().value("geometryTransformHandling") == "preserve";

    const UnsignedInt nodeCount = _state->objectCount;
    const UnsignedInt nodeIdOffset = _state->nodeIdOffset;
    const UnsignedInt geometryTransformCount = retainGeometryTransforms ? nodeCount : 0;

    UnsignedInt meshCount = 0;
    UnsignedInt skinCount = 0;
    UnsignedInt cameraCount = 0;
    UnsignedInt lightCount = 0;

    /* We need to bind each chunk of a mesh to each node that refers to it */
    for(const ufbx_mesh* mesh : scene->meshes) {
        UnsignedInt instanceCount = UnsignedInt(mesh->instances.count);
        UnsignedInt chunkCount = _state->meshChunkMapping[mesh->typed_id].count;
        meshCount += instanceCount * chunkCount;
        if(mesh->skin_deformers.count > 0)
            skinCount += instanceCount * chunkCount;
    }

    /* Collect instanced camera/light counts */
    for(const ufbx_light* light : scene->lights)
        lightCount += UnsignedInt(light->instances.count);
    for(const ufbx_camera* camera : scene->cameras)
        cameraCount += UnsignedInt(camera->instances.count);

    /* Allocate the output array. */
    Containers::ArrayView<UnsignedInt> nodeObjects;
    Containers::ArrayView<Int> parents;
    Containers::ArrayView<Vector3d> translations;
    Containers::ArrayView<Quaterniond> rotations;
    Containers::ArrayView<Vector3d> scalings;
    Containers::ArrayView<UnsignedByte> visibilities; /* @todo should be bool */
    Containers::ArrayView<UnsignedByte> geometryTransformHelpers; /* @todo should be bool */
    Containers::ArrayView<Vector3d> geometryTranslations;
    Containers::ArrayView<Quaterniond> geometryRotations;
    Containers::ArrayView<Vector3d> geometryScalings;
    Containers::ArrayView<UnsignedInt> meshMaterialObjects;
    Containers::ArrayView<UnsignedInt> meshes;
    Containers::ArrayView<Int> meshMaterials;
    Containers::ArrayView<UnsignedInt> cameraObjects;
    Containers::ArrayView<UnsignedInt> cameras;
    Containers::ArrayView<UnsignedInt> lightObjects;
    Containers::ArrayView<UnsignedInt> lights;
    Containers::Array<char> data = Containers::ArrayTuple{
        {NoInit, nodeCount, nodeObjects},
        {NoInit, nodeCount, parents},
        {NoInit, nodeCount, translations},
        {NoInit, nodeCount, rotations},
        {NoInit, nodeCount, scalings},
        {NoInit, nodeCount, visibilities},
        {NoInit, nodeCount, geometryTransformHelpers},
        {NoInit, geometryTransformCount, geometryTranslations},
        {NoInit, geometryTransformCount, geometryRotations},
        {NoInit, geometryTransformCount, geometryScalings},
        {NoInit, meshCount, meshMaterialObjects},
        {NoInit, meshCount, meshes},
        {NoInit, meshCount, meshMaterials},
        {NoInit, cameraCount, cameraObjects},
        {NoInit, cameraCount, cameras},
        {NoInit, lightCount, lightObjects},
        {NoInit, lightCount, lights},
    };

    UnsignedInt meshMaterialOffset = 0;
    UnsignedInt lightOffset = 0;
    UnsignedInt cameraOffset = 0;

    for(const ufbx_node* node : scene->nodes) {
        if(!_state->preserveRootNode && node->is_root) continue;

        UnsignedInt nodeId = node->typed_id - nodeIdOffset;
        nodeObjects[nodeId] = nodeId;
        if(node->parent && (_state->preserveRootNode || !node->parent->is_root)) {
            parents[nodeId] = Int(node->parent->typed_id - nodeIdOffset);
        } else {
            parents[nodeId] = -1;
        }

        translations[nodeId] = Vector3d(node->local_transform.translation);
        rotations[nodeId] = Quaterniond(node->local_transform.rotation);
        scalings[nodeId] = Vector3d(node->local_transform.scale);
        visibilities[nodeId] = UnsignedByte(node->visible);
        geometryTransformHelpers[nodeId] = UnsignedInt(node->is_geometry_transform_helper);

        if(retainGeometryTransforms) {
            geometryTranslations[nodeId] = Vector3d(node->geometry_transform.translation);
            geometryRotations[nodeId] = Quaterniond(node->geometry_transform.rotation);
            geometryScalings[nodeId] = Vector3d(node->geometry_transform.scale);
        }

        for(const ufbx_element* element : node->all_attribs) {
            if(const ufbx_mesh* mesh = ufbx_as_mesh(element)) {
                UnsignedInt materialIndex = 0;
                MeshChunkMapping chunkMapping = _state->meshChunkMapping[mesh->typed_id];
                for(UnsignedInt i = 0; i < chunkMapping.count; ++i) {
                    UnsignedInt chunkIndex = chunkMapping.baseIndex + i;
                    const MeshChunk& chunk = _state->meshChunks[chunkIndex];
                    const ufbx_mesh_material& mat = mesh->materials[chunk.meshMaterialIndex];

                    /* Fetch the material from the ufbx_node to get per instance
                       materials unless configured otherwise */
                    const ufbx_material* material = nullptr;
                    if(materialIndex < node->materials.count)
                        material = node->materials[materialIndex];
                    else
                        material = mat.material;

                    meshMaterialObjects[meshMaterialOffset] = nodeId;
                    meshes[meshMaterialOffset] = chunkIndex;
                    meshMaterials[meshMaterialOffset] = material ? Int(material->typed_id) : -1;

                    ++meshMaterialOffset;
                    ++materialIndex;
                }
            } else if(const ufbx_light* light = ufbx_as_light(element)) {
                lightObjects[lightOffset] = nodeId;
                lights[lightOffset] = light->typed_id;
                ++lightOffset;
            } else if(const ufbx_camera* camera = ufbx_as_camera(element)) {
                cameraObjects[cameraOffset] = nodeId;
                cameras[cameraOffset] = camera->typed_id;
                ++cameraOffset;
            }
        }
    }

    CORRADE_INTERNAL_ASSERT(meshMaterialOffset == meshMaterialObjects.size());
    CORRADE_INTERNAL_ASSERT(lightOffset == lightObjects.size());
    CORRADE_INTERNAL_ASSERT(cameraOffset == cameraObjects.size());

    /* Put everything together. For simplicity the imported data could always
       have all fields present, with some being empty, but this gives less noise
       for asset introspection purposes. */
    Containers::Array<SceneFieldData> fields;

    /* Parent, TRS and Visibility all share the implicit object mapping */
    arrayAppend(fields, {
        /** @todo once there's a flag to annotate implicit fields, omit the
            parent field if it's all -1s; or alternatively we could also have a
            stride of 0 for this case */
        SceneFieldData{SceneField::Parent, nodeObjects, parents, SceneFieldFlag::ImplicitMapping},
        SceneFieldData{SceneField::Translation, nodeObjects, translations, SceneFieldFlag::ImplicitMapping},
        SceneFieldData{SceneField::Rotation, nodeObjects, rotations, SceneFieldFlag::ImplicitMapping},
        SceneFieldData{SceneField::Scaling, nodeObjects, scalings, SceneFieldFlag::ImplicitMapping},
        SceneFieldData{SceneFieldVisibility, nodeObjects, visibilities, SceneFieldFlag::ImplicitMapping},
        SceneFieldData{SceneFieldGeometryTransformHelper, nodeObjects, geometryTransformHelpers, SceneFieldFlag::ImplicitMapping},
    });

    /* Geometry transforms if user wants to preserve them */
    if(retainGeometryTransforms) arrayAppend(fields, {
        /** @todo once there's a flag to annotate implicit fields, omit the
            parent field if it's all -1s; or alternatively we could also have a
            stride of 0 for this case */
        SceneFieldData{SceneFieldGeometryTranslation, nodeObjects, geometryTranslations, SceneFieldFlag::ImplicitMapping},
        SceneFieldData{SceneFieldGeometryRotation, nodeObjects, geometryRotations, SceneFieldFlag::ImplicitMapping},
        SceneFieldData{SceneFieldGeometryScaling, nodeObjects, geometryScalings, SceneFieldFlag::ImplicitMapping},
    });

    /* All other fields have the mapping ordered (they get filed as we iterate
       through objects) */
    if(meshCount) arrayAppend(fields, {
        SceneFieldData{SceneField::Mesh, meshMaterialObjects, meshes, SceneFieldFlag::OrderedMapping},
        SceneFieldData{SceneField::MeshMaterial, meshMaterialObjects, meshMaterials, SceneFieldFlag::OrderedMapping},
    });
    if(lightCount) arrayAppend(fields, SceneFieldData{
        SceneField::Light, lightObjects, lights, SceneFieldFlag::OrderedMapping
    });
    if(cameraCount) arrayAppend(fields, SceneFieldData{
        SceneField::Camera, cameraObjects, cameras, SceneFieldFlag::OrderedMapping
    });

    /* Convert back to the default deleter to avoid dangling deleter function
       pointer issues when unloading the plugin */
    arrayShrink(fields, DefaultInit);

    return SceneData{SceneMappingType::UnsignedInt, nodeCount, std::move(data), std::move(fields)};
}

SceneField UfbxImporter::doSceneFieldForName(Containers::StringView name) {
    for(UnsignedInt i = 0; i < Containers::arraySize(sceneFieldNames); ++i) {
        if(name == sceneFieldNames[i]) return sceneFieldCustom(i);
    }
    return SceneField{};
}

Containers::String UfbxImporter::doSceneFieldName(UnsignedInt name) {
    if(name < Containers::arraySize(sceneFieldNames))
        return sceneFieldNames[name];
    return {};
}

UnsignedLong UfbxImporter::doObjectCount() const {
    return _state->objectCount;
}

Long UfbxImporter::doObjectForName(const Containers::StringView name) {
    const ufbx_scene* scene = _state->scene.get();
    const ufbx_node* node = ufbx_find_node_len(scene, name.data(), name.size());
    return node ? Long(node->typed_id - _state->nodeIdOffset) : -1;
}

Containers::String UfbxImporter::doObjectName(const UnsignedLong id) {
    /* Should always be in bounds as AbstractImporter validates the index */
    return _state->scene->nodes[id + _state->nodeIdOffset]->name;
}

UnsignedInt UfbxImporter::doCameraCount() const {
    return UnsignedInt(_state->scene->cameras.count);
}

Int UfbxImporter::doCameraForName(const Containers::StringView name) {
    return typedId(ufbx_find_element_len(_state->scene.get(), UFBX_ELEMENT_CAMERA, name.data(), name.size()));
}

Containers::String UfbxImporter::doCameraName(const UnsignedInt id) {
    return _state->scene->cameras[id]->name;
}

Containers::Optional<CameraData> UfbxImporter::doCamera(UnsignedInt id) {
    const ufbx_camera* camera = _state->scene->cameras[id];

    switch(camera->projection_mode) {
    case UFBX_PROJECTION_MODE_PERSPECTIVE:
        return CameraData{CameraType::Perspective3D,
            Deg(Float(camera->field_of_view_deg.x)), Float(camera->aspect_ratio),
            Float(camera->near_plane), Float(camera->far_plane)};
    case UFBX_PROJECTION_MODE_ORTHOGRAPHIC:
        return CameraData{CameraType::Orthographic3D,
            Vector2(camera->orthographic_size),
            Float(camera->near_plane), Float(camera->far_plane)};
    }

    /* Not expected to gain new projection modes */
    CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
}

UnsignedInt UfbxImporter::doLightCount() const {
    return UnsignedInt(_state->scene->lights.count);
}

Int UfbxImporter::doLightForName(Containers::StringView name) {
    return typedId(ufbx_find_element_len(_state->scene.get(), UFBX_ELEMENT_LIGHT, name.data(), name.size()));
}

Containers::String UfbxImporter::doLightName(UnsignedInt id) {
    return _state->scene->lights[id]->name;
}

Containers::Optional<LightData> UfbxImporter::doLight(UnsignedInt id) {
    const ufbx_light* light = _state->scene->lights[id];

    const Float intensity = Float(light->intensity);
    const Color3 color{light->color};

    LightData::Type lightType;
    if(light->type == UFBX_LIGHT_POINT) {
        lightType = LightData::Type::Point;
    } else if(light->type == UFBX_LIGHT_DIRECTIONAL) {
        lightType = LightData::Type::Directional;
    } else if(light->type == UFBX_LIGHT_SPOT) {
        lightType = LightData::Type::Spot;
    } else {
        /** @todo area and volume lights */
        Error{} << "Trade::UfbxImporter::light(): light type" << light->type << "is not supported";
        return {};
    }

    Vector3 attenuation;
    if(light->decay == UFBX_LIGHT_DECAY_NONE) {
        attenuation = {1.0f, 0.0f, 0.0f};
    } else if(light->decay == UFBX_LIGHT_DECAY_LINEAR) {
        attenuation = {0.0f, 1.0f, 0.0f};
    } else if(light->decay == UFBX_LIGHT_DECAY_QUADRATIC) {
        attenuation = {0.0f, 0.0f, 1.0f};
    } else if(light->decay == UFBX_LIGHT_DECAY_CUBIC) {
        Warning{} << "Trade::UfbxImporter::light(): cubic attenuation not supported, patching to quadratic";
        attenuation = {0.0f, 0.0f, 1.0f};
    } else {
        Error{} << "Trade::UfbxImporter::light(): light decay" << light->decay << "is not supported"; /* LCOV_EXCL_LINE */
    }

    /* FBX and many modeling programs don't constrain decay to match ligh type */
    if((lightType == LightData::Type::Directional || lightType == LightData::Type::Ambient) && attenuation != Vector3{1.0f, 0.0f, 0.0f}) {
        Warning{} << "Trade::UfbxImporter::light(): patching attenuation" << attenuation << "to" << Vector3{1.0f, 0.0f, 0.0f} << "for" << lightType;
        attenuation = {1.0f, 0.0f, 0.0f};
    }

    Float innerAngle = 360.0f;
    Float outerAngle = 360.0f;
    if(lightType == LightData::Type::Spot) {
        innerAngle = Math::clamp(Float(light->inner_angle), 0.0f, 360.0f);
        outerAngle = Math::clamp(Float(light->outer_angle), innerAngle, 360.0f);
    }

    return LightData{lightType, color, intensity, attenuation,
        Deg{innerAngle}, Deg{outerAngle}};
}

UnsignedInt UfbxImporter::doMeshCount() const {
    return UnsignedInt(_state->meshChunks.size());
}

namespace {

inline UnsignedInt unboundedIfNegative(Int value) {
    return value >= 0 ? UnsignedInt(value) : ~UnsignedInt{};
}

}

Containers::Optional<MeshData> UfbxImporter::doMesh(UnsignedInt id, UnsignedInt level) {
    if(level != 0) return {};

    const MeshChunk chunk = _state->meshChunks[id];
    const ufbx_mesh* mesh = _state->scene->meshes[chunk.meshId];
    const ufbx_mesh_material mat = mesh->materials[chunk.meshMaterialIndex];

    UnsignedInt indexCount = 0;
    switch(chunk.primitive) {
    case MeshPrimitive::Points: indexCount = mat.num_point_faces * 1; break;
    case MeshPrimitive::Lines: indexCount = mat.num_line_faces * 2; break;
    case MeshPrimitive::Triangles: indexCount = mat.num_triangles * 3; break;
    default: CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
    }

    const UnsignedInt maxUvSets = unboundedIfNegative(configuration().value<Int>("maxUvSets"));
    const UnsignedInt maxTangentSets = unboundedIfNegative(configuration().value<Int>("maxTangentSets"));
    const UnsignedInt maxColorSets = unboundedIfNegative(configuration().value<Int>("maxColorSets"));

    const UnsignedInt uvSetCount = Utility::min(UnsignedInt(mesh->uv_sets.count), maxUvSets);
    const UnsignedInt colorSetCount = Utility::min(UnsignedInt(mesh->color_sets.count), maxColorSets);

    /* Include tangents for UV layers until we hit a layer with missing or
       incomplete tangents as at that point the implicit mapping breaks. */
    UnsignedInt tangentSetCount = Utility::min(uvSetCount, maxTangentSets);
    UnsignedInt bitangentSetCount = tangentSetCount;
    for(UnsignedInt i = 0; i < tangentSetCount; i++) {
        const ufbx_uv_set& uv_set = mesh->uv_sets[i];
        if(!uv_set.vertex_tangent.exists || !uv_set.vertex_bitangent.exists) {
            /* Include the last partial tangent/bitangent set */
            tangentSetCount = i + (uv_set.vertex_tangent.exists ? 1u : 0u);
            bitangentSetCount = i + + (uv_set.vertex_bitangent.exists ? 1u : 0u);
            break;
        }
    }

    /* Calculate the stride (ie. size of a single vertex) */
    UnsignedInt attributeCount = 0;
    UnsignedInt stride = 0;

    /* ufbx guarantees that position always exists */
    CORRADE_INTERNAL_ASSERT(mesh->vertex_position.exists);
    ++attributeCount;
    stride += sizeof(Vector3);

    if(mesh->vertex_normal.exists) {
        ++attributeCount;
        stride += sizeof(Vector3);
    }

    attributeCount += uvSetCount;
    stride += uvSetCount * sizeof(Vector2);

    attributeCount += tangentSetCount;
    stride += tangentSetCount * sizeof(Vector3);

    attributeCount += bitangentSetCount;
    stride += bitangentSetCount * sizeof(Vector3);

    attributeCount += colorSetCount;
    stride += colorSetCount * sizeof(Color4);

    /* Need space for maximum triangles or at least a single point/line */
    Containers::Array<UnsignedInt> primitiveIndices{Utility::max(mesh->max_face_triangles * 3, size_t(2))};
    Containers::Array<char> vertexData{NoInit, stride*indexCount};

    Containers::Array<MeshAttributeData> attributeData{attributeCount};
    UnsignedInt attributeOffset = 0;
    UnsignedInt attributeIndex = 0;

    Containers::StridedArrayView1D<Vector3> positions;
    Containers::StridedArrayView1D<Vector3> normals;
    Containers::Array<Containers::StridedArrayView1D<Vector2>> uvSets{uvSetCount};
    Containers::Array<Containers::StridedArrayView1D<Vector3>> tangentSets{tangentSetCount};
    Containers::Array<Containers::StridedArrayView1D<Vector3>> bitangentSets{bitangentSetCount};
    Containers::Array<Containers::StridedArrayView1D<Color4>> colorSets{colorSetCount};

    {
        positions = {vertexData,
            reinterpret_cast<Vector3*>(vertexData + attributeOffset),
            indexCount, stride};

        attributeData[attributeIndex++] = MeshAttributeData{
            MeshAttribute::Position, positions};
        attributeOffset += sizeof(Vector3);
    }

    if(mesh->vertex_normal.exists) {
        normals = {vertexData,
            reinterpret_cast<Vector3*>(vertexData + attributeOffset),
            indexCount, stride};

        attributeData[attributeIndex++] = MeshAttributeData{
            MeshAttribute::Normal, normals};
        attributeOffset += sizeof(Vector3);
    }

    for(UnsignedInt i = 0; i < uvSetCount; ++i) {
        uvSets[i] = {vertexData,
            reinterpret_cast<Vector2*>(vertexData + attributeOffset),
            indexCount, stride};

        attributeData[attributeIndex++] = MeshAttributeData{
            MeshAttribute::TextureCoordinates, uvSets[i]};
        attributeOffset += sizeof(Vector2);
    }

    for(UnsignedInt i = 0; i < tangentSetCount; ++i) {
        tangentSets[i] = {vertexData,
            reinterpret_cast<Vector3*>(vertexData + attributeOffset),
            indexCount, stride};

        attributeData[attributeIndex++] = MeshAttributeData{
            MeshAttribute::Tangent, tangentSets[i]};
        attributeOffset += sizeof(Vector3);
    }

    for(UnsignedInt i = 0; i < bitangentSetCount; ++i) {
        bitangentSets[i] = {vertexData,
            reinterpret_cast<Vector3*>(vertexData + attributeOffset),
            indexCount, stride};

        attributeData[attributeIndex++] = MeshAttributeData{
            MeshAttribute::Bitangent, bitangentSets[i]};
        attributeOffset += sizeof(Vector3);
    }

    for(UnsignedInt i = 0; i < colorSetCount; ++i) {
        colorSets[i] = {vertexData,
            reinterpret_cast<Color4*>(vertexData + attributeOffset),
            indexCount, stride};

        attributeData[attributeIndex++] = MeshAttributeData{
            MeshAttribute::Color, colorSets[i]};
        attributeOffset += sizeof(Color4);
    }

    CORRADE_INTERNAL_ASSERT(attributeIndex == attributeCount);
    CORRADE_INTERNAL_ASSERT(attributeOffset == stride);

    UnsignedInt dstIx = 0;
    for(UnsignedInt faceIndex : mat.face_indices) {
        const ufbx_face face = mesh->faces[faceIndex];

        UnsignedInt numIndices = 0;

        switch(chunk.primitive) {
        case MeshPrimitive::Points:
            numIndices = face.num_indices == 1 ? 1u : 0u;
            primitiveIndices[0] = face.index_begin;
            break;
        case MeshPrimitive::Lines:
            numIndices = face.num_indices == 2 ? 2u : 0u;
            primitiveIndices[0] = face.index_begin + 0;
            primitiveIndices[1] = face.index_begin + 1;
            break;
        case MeshPrimitive::Triangles:
            numIndices = ufbx_triangulate_face(primitiveIndices.data(), primitiveIndices.size(), mesh, face) * 3;
            break;
        default: CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
        }

        for(UnsignedInt i = 0; i < numIndices; i++) {
            const UnsignedInt srcIx = primitiveIndices[i];

            positions[dstIx] = Vector3(mesh->vertex_position[srcIx]);
            if(mesh->vertex_normal.exists)
                normals[dstIx] = Vector3(mesh->vertex_normal[srcIx]);
            for(UnsignedInt set = 0; set < uvSetCount; ++set)
                uvSets[set][dstIx] = Vector2(mesh->uv_sets[set].vertex_uv[srcIx]);
            for(UnsignedInt set = 0; set < tangentSetCount; ++set)
                tangentSets[set][dstIx] = Vector3(mesh->uv_sets[set].vertex_tangent[srcIx]);
            for(UnsignedInt set = 0; set < bitangentSetCount; ++set)
                bitangentSets[set][dstIx] = Vector3(mesh->uv_sets[set].vertex_bitangent[srcIx]);
            for(UnsignedInt set = 0; set < colorSetCount; ++set)
                colorSets[set][dstIx] = Color4(mesh->color_sets[set].vertex_color[srcIx]);
            dstIx++;
        }
    }

    Containers::Array<char> indexData{NoInit, indexCount*sizeof(UnsignedInt)};
    Containers::ArrayView<UnsignedInt> indices = Containers::arrayCast<UnsignedInt>(indexData);

    /* The vertex data is unindexed, so generate a contiguous index range */
    for(UnsignedInt i = 0; i < indexCount; i++)
        indices[i] = i;

    MeshData meshData{chunk.primitive,
        std::move(indexData), MeshIndexData{indices},
        std::move(vertexData), std::move(attributeData),
        UnsignedInt(indexCount)};

    /* Generate proper indices if configured (or by default) */
    const bool generateIndices = configuration().value<bool>("generateIndices");
    if(generateIndices) meshData = MeshTools::removeDuplicates(meshData);

    /* GCC 4.8 needs extra help here */
    return Containers::optional(std::move(meshData));
}

UnsignedInt UfbxImporter::doMaterialCount() const {
    return UnsignedInt(_state->scene->materials.count);
}

Int UfbxImporter::doMaterialForName(Containers::StringView name) {
    return typedId(ufbx_find_element_len(_state->scene.get(), UFBX_ELEMENT_MATERIAL, name.data(), name.size()));
}

Containers::String UfbxImporter::doMaterialName(UnsignedInt id) {
    return _state->scene->materials[id]->name;
}

namespace {

Containers::StringView blendModeToString(ufbx_blend_mode mode) {
    switch(mode) {
    /* LCOV_EXCL_START */
    case UFBX_BLEND_TRANSLUCENT: return "translucent"_s;
    case UFBX_BLEND_ADDITIVE: return "additive"_s;
    case UFBX_BLEND_MULTIPLY: return "multiply"_s;
    case UFBX_BLEND_MULTIPLY_2X: return "multiply2x"_s;
    case UFBX_BLEND_OVER: return "over"_s;
    case UFBX_BLEND_REPLACE: return "replace"_s;
    case UFBX_BLEND_DISSOLVE: return "dissolve"_s;
    case UFBX_BLEND_DARKEN: return "darken"_s;
    case UFBX_BLEND_COLOR_BURN: return "colorBurn"_s;
    case UFBX_BLEND_LINEAR_BURN: return "linearBurn"_s;
    case UFBX_BLEND_DARKER_COLOR: return "darkerColor"_s;
    case UFBX_BLEND_LIGHTEN: return "lighten"_s;
    case UFBX_BLEND_SCREEN: return "screen"_s;
    case UFBX_BLEND_COLOR_DODGE: return "colorDodge"_s;
    case UFBX_BLEND_LINEAR_DODGE: return "linearDodge"_s;
    case UFBX_BLEND_LIGHTER_COLOR: return "lighterColor"_s;
    case UFBX_BLEND_SOFT_LIGHT: return "softLight"_s;
    case UFBX_BLEND_HARD_LIGHT: return "hardLight"_s;
    case UFBX_BLEND_VIVID_LIGHT: return "vividLight"_s;
    case UFBX_BLEND_LINEAR_LIGHT: return "linearLight"_s;
    case UFBX_BLEND_PIN_LIGHT: return "pinLight"_s;
    case UFBX_BLEND_HARD_MIX: return "hardMix"_s;
    case UFBX_BLEND_DIFFERENCE: return "difference"_s;
    case UFBX_BLEND_EXCLUSION: return "exclusion"_s;
    case UFBX_BLEND_SUBTRACT: return "subtract"_s;
    case UFBX_BLEND_DIVIDE: return "divide"_s;
    case UFBX_BLEND_HUE: return "hue"_s;
    case UFBX_BLEND_SATURATION: return "saturation"_s;
    case UFBX_BLEND_COLOR: return "color"_s;
    case UFBX_BLEND_LUMINOSITY: return "luminosity"_s;
    case UFBX_BLEND_OVERLAY: return "overlay"_s;
    /* LCOV_EXCL_STOP */
    }

    CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
}

inline bool isMapUsed(const ufbx_material_map& map) {
    return map.has_value || map.texture != nullptr;
}

}

Containers::Optional<MaterialData> UfbxImporter::doMaterial(UnsignedInt id) {
    const ufbx_material* material = _state->scene->materials[id];

    const bool preserveMaterialFactors = configuration().value<bool>("preserveMaterialFactors");

    MaterialExclusionGroups seenExclusionGroups;

    /* Mappings from ufbx_material_map to MaterialAttribute. We list the PBR
       mappings first as some of the attributes overlap and we prefer the
       instance we hit first. */
    struct MaterialMappingList {
        Containers::ArrayView<const MaterialMapping> mappings;
        Containers::ArrayView<const ufbx_material_map> maps;
        bool pbr;
        bool factor;
    };
    MaterialMappingList mappingLists[] = {
        { Containers::arrayView(materialMappingPbr), Containers::arrayView(material->pbr.maps), true, false },
        { Containers::arrayView(materialMappingFbx), Containers::arrayView(material->fbx.maps), false, false },
        { Containers::arrayView(materialMappingPbrFactor), Containers::arrayView(material->pbr.maps), true, true },
        { Containers::arrayView(materialMappingFbxFactor), Containers::arrayView(material->fbx.maps), false, true },
    };

    /* This naming is a bit confusing as we're using "layer" to mean two
       different things. First we have UfbxMaterialLayerCount "static" layers:
       These contain related attributes like Transmission or Subsurface. Within
       each of these layers we can have multiple layers of attributes to support
       layered textures. These are rare enough that we don't want to allocate an
       array unless we need the extra layers. */
    struct UfbxMaterialLayerAttributes {
        Containers::Array<MaterialAttributeData> defaultLayer;
        Containers::Array<Containers::Array<MaterialAttributeData>> extraLayers;
    };
    Containers::StaticArray<UfbxMaterialLayerCount, UfbxMaterialLayerAttributes> layerAttributes;

    MaterialTypes types;

    /* If we have DiffuseColor specified from the FBX properties the fallback
       FBX material should be quite well defined. */
    if(isMapUsed(material->fbx.diffuse_color)) {
        types |= MaterialType::Phong;
    }

    /* PbrMetallicRoughness and PbrSpecularGlossiness are mutually exclusive,
       most PBR models in FBX use MetallicRoughness as a base but support
       tinting the specular color */
    if(isMapUsed(material->pbr.metalness) && isMapUsed(material->pbr.roughness)) {
        types |= MaterialType::PbrMetallicRoughness;
    } else if(isMapUsed(material->pbr.specular_color) && isMapUsed(material->pbr.glossiness)) {
        types |= MaterialType::PbrSpecularGlossiness;
    }

    if(isMapUsed(material->pbr.coat_factor)) {
        types |= MaterialType::PbrClearCoat;
    }

    for(const MaterialMappingList& list : mappingLists) {
        /* Ignore implicitly derived PBR values and factors if we don't want to
           explicitly retain them */
        if(list.pbr && !material->features.pbr.enabled) continue;
        if(list.factor && !preserveMaterialFactors) continue;

        for(const MaterialMapping& mapping : list.mappings) {
            const ufbx_material_map& map = list.maps[mapping.valueMap];

            /* Ignore maps with no value or texture */
            if(!map.has_value && !map.texture) continue;

            /* If the map has an exclusion group and we have seen one instance
               of it already, skip this one. */
            if(mapping.exclusionGroup != MaterialExclusionGroup{}) {
                if(seenExclusionGroups & mapping.exclusionGroup) continue;
                seenExclusionGroups |= mapping.exclusionGroup;
            }

            Containers::StringView attribute = mapping.attribute;
            UfbxMaterialLayerAttributes& attributesForLayer = layerAttributes[UnsignedInt(mapping.layer)];

            /* Premultiply factor unless configured not to */
            Float factor = 1.0f;
            if(mapping.factorMap >= 0) {
                const ufbx_material_map& factorMap = list.maps[mapping.factorMap];
                if(factorMap.has_value && !preserveMaterialFactors) {
                    factor = Float(factorMap.value_real);
                }
            }

            /* Patch opacity to BaseColor.a if it's defined as a scalar value */
            Float opacity = 1.0f;
            if(list.pbr && mapping.valueMap == UFBX_MATERIAL_PBR_BASE_COLOR) {
                if(material->pbr.opacity.has_value && material->pbr.opacity.value_components == 1) {
                    opacity = Float(material->pbr.opacity.value_real);
                }
            }

            /* Create an attribute for the value if defined */
            if(attribute && map.has_value) {
                Containers::Array<MaterialAttributeData>& attributes = attributesForLayer.defaultLayer;
                if(mapping.attributeType == MaterialAttributeType::Float) {
                    Float value = Float(map.value_real) * factor;
                    arrayAppend(attributes, {attribute, value});
                } else if(mapping.attributeType == MaterialAttributeType::Vector3) {
                    Vector3 value = Vector3(map.value_vec3) * factor;
                    arrayAppend(attributes, {attribute, value});
                } else if(mapping.attributeType == MaterialAttributeType::Vector4) {
                    Vector4 value = Vector4(map.value_vec4) * Vector4{factor,factor,factor,opacity};
                    arrayAppend(attributes, {attribute, value});
                } else if(mapping.attributeType == MaterialAttributeType::Long) {
                    arrayAppend(attributes, {attribute, map.value_int});
                } else if(mapping.attributeType == MaterialAttributeType::Bool) {
                    arrayAppend(attributes, {attribute, map.value_int != 0});
                } else {
                    CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
                }
            }

            /* Create a texture attribute if defined and allowed */
            if(map.texture && mapping.textureAttribute != MaterialMapping::DisallowTexture()) {
                /* We may have multiple file_textures in two cases:
                     UFBX_TEXTURE_LAYERED: Well defined texture layers
                     UFBX_TEXTURE_SHADER: Arbitrary references in a shader graph
                   Normal UFBX_TEXTURE_FILE textures also always contain a
                   single texture (themselves) in file_textures */
                UnsignedInt layer = 0;
                for(const ufbx_texture* texture : map.texture->file_textures) {
                    Int textureId = _state->textureRemap[texture->typed_id];
                    if(textureId < 0) continue;

                    /* Make sure we have a layer to add the attribute to */
                    if(layer > 0 && layer - 1 >= attributesForLayer.extraLayers.size())
                        arrayResize(attributesForLayer.extraLayers, layer);
                    Containers::Array<MaterialAttributeData>& attributes = layer == 0
                        ? attributesForLayer.defaultLayer
                        : attributesForLayer.extraLayers[layer - 1];

                    /* Generate an automatic name if not defined */
                    Containers::String textureAttribute;
                    if(mapping.textureAttribute)
                        textureAttribute = mapping.textureAttribute;
                    else
                        textureAttribute = attribute + "Texture"_s;

                    /* Base texture attribute */
                    arrayAppend(attributes, {textureAttribute, UnsignedInt(textureId)});

                    if(texture->has_uv_transform) {
                        const Containers::String matrixAttribute = textureAttribute + "Matrix"_s;
                        const ufbx_matrix& mat = map.texture->uv_to_texture;
                        const Matrix3 value = {
                            { Float(mat.m00), Float(mat.m10), 0.0f },
                            { Float(mat.m01), Float(mat.m11), 0.0f },
                            { Float(mat.m03), Float(mat.m13), 1.0f },
                        };
                        arrayAppend(attributes, {matrixAttribute, value});
                    }

                    /* @todo map from UV set names to indices? */

                    /* Read blending mode if the texture has proper layers.
                       Note that we may have more file_textures than layers if
                       there are shaders/recursive layers involved.. */
                    if(map.texture->type == UFBX_TEXTURE_LAYERED && layer < map.texture->layers.count) {
                        const ufbx_texture_layer& texLayer = map.texture->layers[layer];
                        if(texLayer.texture == texture) {
                            const Containers::String blendModeAttribute = textureAttribute + "BlendMode"_s;
                            const Containers::String blendAlphaAttribute = textureAttribute + "BlendAlpha"_s;
                            arrayAppend(attributes, {blendModeAttribute, blendModeToString(texLayer.blend_mode)});
                            arrayAppend(attributes, {blendAlphaAttribute, Float(texLayer.alpha)});
                        }
                    }

                    ++layer;
                }
            }
        }
    }

    Containers::Array<MaterialAttributeData> flatAttributes;
    Containers::Array<UnsignedInt> layerSizes;
    UnsignedInt layerOffset = 0;

    /* Concatenate all layers, the first layer is special and doesn't have a
       LayerName entry and gets a zero attribute layer if necessary. */
    for(UnsignedInt layer = 0; layer < layerAttributes.size(); ++layer) {
        UfbxMaterialLayerAttributes& attributesForLayer = layerAttributes[UnsignedInt(layer)];

        /* Skip empty layers after the first one */
        if(layer != 0 && attributesForLayer.defaultLayer.isEmpty()) continue;

        /* Default layer within the named layer */
        {
            UnsignedInt layerAttributeCount = 0;
            Containers::ArrayView<MaterialAttributeData> attributes = attributesForLayer.defaultLayer;
            if(layer != 0) {
                arrayAppend(flatAttributes, {MaterialAttribute::LayerName, ufbxMaterialLayerNames[layer]});
                ++layerAttributeCount;
            }
            arrayAppend(flatAttributes, attributes);
            layerAttributeCount += attributes.size();

            layerOffset += layerAttributeCount;
            arrayAppend(layerSizes, layerOffset);
        }

        /* Extra layers (ie. extra texture layers in FBX) */
        for(UnsignedInt i = 0; i < attributesForLayer.extraLayers.size(); ++i) {
            UnsignedInt layerAttributeCount = 0;
            Containers::ArrayView<MaterialAttributeData> attributes = attributesForLayer.extraLayers[i];
            if(layer != 0) {
                arrayAppend(flatAttributes, {MaterialAttribute::LayerName, ufbxMaterialLayerNames[layer]});
                ++layerAttributeCount;
            }
            arrayAppend(flatAttributes, attributes);
            layerAttributeCount += attributes.size();

            layerOffset += layerAttributeCount;
            arrayAppend(layerSizes, layerOffset);
        }
    }

    /* Convert back to the default deleter to avoid dangling deleter function
       pointer issues when unloading the plugin */
    arrayShrink(flatAttributes, DefaultInit);
    arrayShrink(layerSizes, DefaultInit);

    return MaterialData{types, std::move(flatAttributes), std::move(layerSizes)};
}

UnsignedInt UfbxImporter::doTextureCount() const {
    return UnsignedInt(_state->textures.size());
}

Int UfbxImporter::doTextureForName(Containers::StringView name) {
    const ufbx_element* element = ufbx_find_element_len(_state->scene.get(), UFBX_ELEMENT_TEXTURE, name.data(), name.size());
    return element ? _state->textureRemap[element->typed_id] : -1;
}

Containers::String UfbxImporter::doTextureName(UnsignedInt id) {
    const FileTexture& fileTexture = _state->textures[id];
    return _state->scene->textures[fileTexture.textureIndex]->name;
}

namespace {

inline SamplerWrapping toSamplerWrapping(ufbx_wrap_mode mode) {
    switch(mode) {
    case UFBX_WRAP_REPEAT: return SamplerWrapping::Repeat;
    case UFBX_WRAP_CLAMP: return SamplerWrapping::ClampToEdge;
    }

    CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
}

}

Containers::Optional<TextureData> UfbxImporter::doTexture(UnsignedInt id) {
    const FileTexture& fileTexture = _state->textures[id];
    const ufbx_texture* texture = _state->scene->textures[fileTexture.textureIndex];

    const SamplerWrapping wrappingU = toSamplerWrapping(texture->wrap_u);
    const SamplerWrapping wrappingV = toSamplerWrapping(texture->wrap_v);
    return TextureData{TextureType::Texture2D,
        SamplerFilter::Linear, SamplerFilter::Linear, SamplerMipmap::Linear,
        {wrappingU, wrappingV, SamplerWrapping::ClampToEdge},
        fileTexture.fileTextureIndex};
}

AbstractImporter* UfbxImporter::setupOrReuseImporterForImage(UnsignedInt id, const char* errorPrefix) {
    const ufbx_texture_file& file = _state->scene->texture_files[id];

    /* Looking for the same ID, so reuse an importer populated before. If the
       previous attempt failed, the importer is not set, so return nullptr in
       that case. Going through everything below again would not change the
       outcome anyway, only spam the output with redundant messages. */
    if(_state->imageImporterId == id)
        return _state->imageImporter ? &*_state->imageImporter : nullptr;

    /* Otherwise reset the importer and remember the new ID. If the import
       fails, the importer will stay unset, but the ID will be updated so the
       next round can again just return nullptr above instead of going through
       the doomed-to-fail process again. */
    _state->imageImporter = Containers::NullOpt;
    _state->imageImporterId = id;

    AnyImageImporter importer{*manager()};
    importer.setFlags(flags());
    if(fileCallback()) importer.setFileCallback(fileCallback(), fileCallbackUserData());

    if(file.content.size > 0) {
        auto textureData = Containers::ArrayView<const char>(reinterpret_cast<const char*>(file.content.data), file.content.size);
        if(!importer.openData(textureData))
            return nullptr;
    } else {
        if(!_state->fromFile && !fileCallback()) {
            Error{} << errorPrefix << "external images can be imported only when opening files from the filesystem or if a file callback is present";
            return nullptr;
        }

        ufbx_string filename = file.filename.length > 0 ? file.filename : file.absolute_filename;
        if(!importer.openFile(filename))
            return nullptr;
    }

    if(importer.image2DCount() != 1) {
        Error{} << errorPrefix << "expected exactly one 2D image in an image file but got" << importer.image2DCount();
        return nullptr;
    }

    return &_state->imageImporter.emplace(std::move(importer));
}

UnsignedInt UfbxImporter::doImage2DCount() const {
    return UnsignedInt(_state->scene->texture_files.count);
}

UnsignedInt UfbxImporter::doImage2DLevelCount(UnsignedInt id) {
    CORRADE_ASSERT(manager(), "Trade::UfbxImporter::image2DLevelCount(): the plugin must be instantiated with access to plugin manager in order to open image files", {});

    AbstractImporter* importer = setupOrReuseImporterForImage(id, "Trade::UfbxImporter::image2DLevelCount():");
    /* image2DLevelCount() isn't supposed to fail (image2D() is, instead), so
       report 1 on failure and expect image2D() to fail later */
    if(!importer) return 1;

    return importer->image2DLevelCount(0);
}

Containers::Optional<ImageData2D> UfbxImporter::doImage2D(UnsignedInt id, UnsignedInt level) {
    CORRADE_ASSERT(manager(), "Trade::UfbxImporter::image2D(): the plugin must be instantiated with access to plugin manager in order to open image files", {});

    AbstractImporter* importer = setupOrReuseImporterForImage(id, "Trade::UfbxImporter::image2D():");
    if(!importer) return Containers::NullOpt;

    return importer->image2D(0, level);
}

Int UfbxImporter::doImage2DForName(Containers::StringView name) {
    auto found = _state->imageNameMap.find(name);
    return found == _state->imageNameMap.end() ? -1 : Int(found->second);
}

Containers::String UfbxImporter::doImage2DName(UnsignedInt id) {
    return _state->scene->texture_files[id].relative_filename;
}

}}

CORRADE_PLUGIN_REGISTER(UfbxImporter, Magnum::Trade::UfbxImporter,
    "cz.mosra.magnum.Trade.AbstractImporter/0.5")

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

#define ufbx_abi static

#define UFBX_NO_INDEX_GENERATION
#define UFBX_NO_GEOMETRY_CACHE
#define UFBX_NO_TESSELLATION
#define UFBX_NO_SUBDIVISION
#define UFBX_NO_SCENE_EVALUATION
#define UFBX_NO_SKINNING_EVALUATION

/* Include error stack on debug builds for juicy details in bugreports */
#if !defined(CORRADE_IS_DEBUG_BUILD) && defined(NDEBUG)
    #define UFBX_NO_ERROR_STACK
#else
    #define UFBX_ENABLE_ERROR_STACK
#endif

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
    static String from(const ufbx_string &other) {
        return String{other.data, other.length};
    }
};
template<> struct StringViewConverter<const char, ufbx_string> {
    static StringView from(const ufbx_string &other) {
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

constexpr Containers::StringView sceneFieldNames[] = {
    "Visibility"_s,
};

Containers::StringView blendModeToString(ufbx_blend_mode mode) {
    switch (mode) {
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
    default:
        Warning{} << "Unhandled blend mode" << Int(mode);
        return {};
    }
}

struct MeshChunk {
    uint32_t meshId;
    uint32_t meshMaterialIndex;
};

ufbx_load_opts loadOptsFromConfiguration(Utility::ConfigurationGroup& conf) {
    static_cast<void>(conf);

    ufbx_load_opts opts = { };

    opts.generate_missing_normals = conf.value<bool>("generateMissingNormals");
    opts.strict = conf.value<bool>("strict");
    opts.disable_quirks = conf.value<bool>("disableQuirks");
    opts.load_external_files = conf.value<bool>("loadExternalFiles");
    opts.ignore_geometry = conf.value<bool>("ignoreGeometry");
    opts.ignore_animation = conf.value<bool>("ignoreAnimation");
    opts.ignore_embedded = conf.value<bool>("ignoreEmbedded");
    opts.ignore_all_content = conf.value<bool>("ignoreAllContent");
    opts.ignore_missing_external_files = true;

    /* @todo: ufbx can normalize scenes to units/coordinate systems, this is a
      very rudimentary implementation used for testing. Not sure how exposing
      something like this in configuration would work. Also for extra goodness
      it depends on preserveRootNode as that's where ufbx writes the mapping.
      Another alternative would be to expose the current scene units as some
      custom scene fields and let users do the math. */
    if (conf.value<bool>("normalizeUnits")) {
        opts.target_axes = ufbx_axes_right_handed_y_up;
        opts.target_unit_meters = 1.0f;
    }

    /* We need to split meshes by material so create a dummy ufbx_mesh_material
       containing the whole mesh to make processing code simpler. */
    opts.allow_null_material = true;

    return opts;
}

inline Int typedId(ufbx_element *element) {
    return element ? Int(element->typed_id) : -1;
}

inline SamplerWrapping toSamplerWrapping(ufbx_wrap_mode mode) {
    if (mode == UFBX_WRAP_CLAMP) {
        return SamplerWrapping::ClampToEdge;
    } else if (mode == UFBX_WRAP_REPEAT) {
        return SamplerWrapping::Repeat;
    } else {
        /* @todo: What to do about unhandled enums */
        return SamplerWrapping::Repeat; /* LCOV_EXCL_LINE */
    }
}

inline void logError(const char *prefix, const ufbx_error &error, ImporterFlags flags) {
    if (flags & ImporterFlag::Verbose) {
        char message[1024];
        ufbx_format_error(message, sizeof(message), &error);
        Error{Utility::Debug::Flag::NoSpace|Utility::Debug::Flag::NoNewlineAtTheEnd} << prefix << message;
    } else if (error.info_length > 0) {
        Error{Utility::Debug::Flag::NoSpace} << prefix << Containers::StringView(error.description) << ": " << Containers::StringView(error.info, error.info_length);
    } else {
        Error{Utility::Debug::Flag::NoSpace} << prefix << Containers::StringView(error.description);
    }
}

inline UnsignedInt unboundedIfNegative(Int value) {
    return value >= 0 ? UnsignedInt(value) : ~UnsignedInt{};
}

inline bool mapUsed(const ufbx_material_map &map)
{
    return map.has_value || map.texture != nullptr;
}

struct FileOpener {
    FileOpener(): _callback{nullptr}, _userData{nullptr} {}
    explicit FileOpener(Containers::Optional<Containers::ArrayView<const char>>(*callback)(const std::string&, InputFileCallbackPolicy, void*), void* userData): _callback{callback}, _userData{userData} {}

    bool operator()(ufbx_stream *stream, const char *path, size_t path_len, const ufbx_open_file_info *info)
    {
        switch (info->type) {
        case UFBX_OPEN_FILE_MAIN_MODEL:
        case UFBX_OPEN_FILE_OBJ_MTL:
            /* Continue loading below */
            break;
        default:
            /* Ignore all others */
            return false;
        }

        /* If we don't have a callback just defer to ufbx file loading */
        if (!_callback) {
            return ufbx_open_file(stream, path, path_len);
        }

        std::string file{path, path_len};
        const Containers::Optional<Containers::ArrayView<const char>> data = _callback(file, InputFileCallbackPolicy::LoadTemporary, _userData);
        if(!data) return false;

        ufbx_open_memory_opts opts = { };
        opts.allocator.allocator = info->temp_allocator;

        /* We don't need to copy the file data as it's guaranteed to live for
           the duration of the load function we are currently executing */
        opts.no_copy = true;

        return ufbx_open_memory(stream, data->data(), data->size(), &opts);
    }

    Containers::Optional<Containers::ArrayView<const char>> (*_callback)(const std::string&, InputFileCallbackPolicy, void*);
    void *_userData;
};

struct FileTexture {
    UnsignedInt textureIndex;
    UnsignedInt fileIndex;
};

}

struct UfbxImporter::State {
    ufbx_scene_ref scene;

    Containers::Array<MeshChunk> meshChunks;

    /* Mapping from ufbx_mesh.typed_id -> State.meshChunks */
    Containers::Array<UnsignedInt> meshChunkBase;

    UnsignedInt nodeIdOffset = 0;
    UnsignedInt originalNodeCount = 0;
    UnsignedInt nodeCountWithSynthetic = 0;

    bool fromFile = false;

    UnsignedInt imageImporterId = ~UnsignedInt{};
    Containers::Optional<AnyImageImporter> imageImporter;

    std::unordered_map<std::string, UnsignedInt> textureNameMap;
    Containers::Array<FileTexture> textures;
    Containers::Array<Int> textureRemap;
};

UfbxImporter::UfbxImporter(PluginManager::AbstractManager& manager, const Containers::StringView& plugin): AbstractImporter{manager, plugin} {
}

UfbxImporter::~UfbxImporter() = default;

ImporterFeatures UfbxImporter::doFeatures() const { return ImporterFeature::OpenData|ImporterFeature::FileCallback; }

bool UfbxImporter::doIsOpened() const { return !!_state; }

void UfbxImporter::doClose() { _state = nullptr; }

void UfbxImporter::doOpenData(Containers::Array<char>&& data, const DataFlags) {
    _state.reset();

    ufbx_load_opts opts = loadOptsFromConfiguration(configuration());

    FileOpener opener{fileCallback(), fileCallbackUserData()};
    opts.open_file_cb = &opener;

    ufbx_error error;
    ufbx_scene *scene = ufbx_load_memory(data.data(), data.size(), &opts, &error);
    if (!scene) {
        logError("Trade::UfbxImporter::openData(): loading failed: ", error, flags());
        return;
    }

    /* Handle the opened scene with doOpenState() */
    openInternal(scene, {});
}

void UfbxImporter::doOpenFile(Containers::StringView filename) {
    _state.reset();

    ufbx_load_opts opts = loadOptsFromConfiguration(configuration());
    opts.filename = filename;

    FileOpener opener{fileCallback(), fileCallbackUserData()};
    opts.open_file_cb = &opener;

    ufbx_error error;
    ufbx_scene *scene = ufbx_load_file_len(filename.data(), filename.size(), &opts, &error);
    if (!scene) {
        logError("Trade::UfbxImporter::openFile(): loading failed: ", error, flags());
        return;
    }

    /* Handle the opened scene with doOpenState() */
    openInternal(scene, true);
}

void UfbxImporter::openInternal(void* state, bool fromFile) {
    ufbx_scene *scene = static_cast<ufbx_scene*>(state);

    _state.reset(new State{});
    _state->fromFile = fromFile;
    _state->scene = ufbx_scene_ref{scene};

    /* We need to split meshes into chunks per material, so precompute the
       number of required chunks at the start, as eg. meshCount() depends on it */
    {
        UnsignedInt chunkCount = 0;
        arrayResize(_state->meshChunkBase, UnsignedInt(scene->meshes.count));

        /* ufbx meshes can contain per-face materials so we need to separate them
           into pieces containing a single material for SceneData. */
        for(std::size_t i = 0; i < scene->meshes.count; ++i) {
            ufbx_mesh *mesh = scene->meshes[i];

            _state->meshChunkBase[i] = chunkCount;

            for(const ufbx_mesh_material &mat : mesh->materials) {
                if(mat.num_faces == 0) continue;
                ++chunkCount;
            }
        }

        arrayResize(_state->meshChunks, chunkCount);

        /* Initialize mesh chunks */
        for(ufbx_mesh *mesh : scene->meshes) {
            UnsignedInt chunkOffset = _state->meshChunkBase[mesh->typed_id];
            for (std::size_t i = 0; i < mesh->materials.count; ++i) {
                const ufbx_mesh_material &mat = mesh->materials[i];
                if(mat.num_faces == 0) continue;

                MeshChunk &chunk = _state->meshChunks[chunkOffset];
                chunk.meshId = mesh->typed_id;
                chunk.meshMaterialIndex = UnsignedInt(i);
                ++chunkOffset;
            }
        }
    }

    /* Count the final number of nodes in the scene, we may remove some (root)
       or add (synthetic geometry transform nodes) */
    {
        const bool preserveRootNode = configuration().value<bool>("preserveRootNode");
        const bool geometricTransformNodes = configuration().value<bool>("geometricTransformNodes");

        _state->nodeIdOffset = 0;
        _state->originalNodeCount = UnsignedInt(scene->nodes.count);

        if(!preserveRootNode) {
            --_state->originalNodeCount;
            ++_state->nodeIdOffset;
        }

        _state->nodeCountWithSynthetic = _state->originalNodeCount;

        /* Reserve space for nodes if we want to create dummy nodes for geometric
           transforms */
        if(geometricTransformNodes) {
            for(ufbx_node *node : scene->nodes) {
                if(node->has_geometry_transform) {
                    ++_state->nodeCountWithSynthetic;
                }
            }
        }
    }

    /* Use only textures which have a potential file */
    arrayResize(_state->textureRemap, scene->textures.count, -1);
    for (ufbx_texture *texture : scene->textures) {
        if (!texture->has_file) continue;

        UnsignedInt id = texture->typed_id;
        _state->textureRemap[id] = Int(_state->textures.size());
        arrayAppend(_state->textures, { id, texture->file_index });
    }

    for (UnsignedInt i = 0; i < scene->texture_files.count; ++i) {
        ufbx_string name = scene->texture_files[i].relative_filename;
        if (name.length > 0) {
            _state->textureNameMap.emplace(std::string(name.data, name.length), i);
        }
    }
}

Int UfbxImporter::doDefaultScene() const { return 0; }

UnsignedInt UfbxImporter::doSceneCount() const { return 1; }

Containers::Optional<SceneData> UfbxImporter::doScene(UnsignedInt) {
    ufbx_scene *scene = _state->scene.get();

    const bool preserveRootNode = configuration().value<bool>("preserveRootNode");
    const bool geometricTransformNodes = configuration().value<bool>("geometricTransformNodes");
    const bool perInstanceMaterials = configuration().value<bool>("perInstanceMaterials");

    UnsignedInt meshCount = 0;
    UnsignedInt skinCount = 0;
    UnsignedInt nodeCount = _state->nodeCountWithSynthetic;
    UnsignedInt cameraCount = 0;
    UnsignedInt lightCount = 0;

    /* ufbx meshes can contain per-face materials so we need to separate them
       into pieces containing a single material for SceneData. */
    for(std::size_t i = 0; i < scene->meshes.count; ++i) {
        ufbx_mesh *mesh = scene->meshes[i];

        UnsignedInt instanceCount = UnsignedInt(mesh->instances.count);
        for(const ufbx_mesh_material &mat : mesh->materials) {
            if(mat.num_faces == 0) continue;
            meshCount += instanceCount;
            if(mesh->skin_deformers.count > 0)
                skinCount += instanceCount;
        }
    }

    /* Collect instanced camera/light counts */
    for(ufbx_light *light : scene->lights)
        lightCount += UnsignedInt(light->instances.count);
    for(ufbx_camera *camera : scene->cameras)
        cameraCount += UnsignedInt(camera->instances.count);

    /* Allocate the output array. */
    Containers::ArrayView<UnsignedInt> nodeObjects;
    Containers::ArrayView<Int> parents;
    Containers::ArrayView<Vector3d> translations;
    Containers::ArrayView<Quaterniond> rotations;
    Containers::ArrayView<Vector3d> scalings;
    Containers::ArrayView<UnsignedByte> visibilities; /* @todo should be bool */
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
    UnsignedInt syntheticNodeCount = 0;
    UnsignedInt nodeIdOffset = _state->nodeIdOffset;

    for(std::size_t i = 0; i < scene->nodes.count; i++) {
        ufbx_node *node = scene->nodes[i];
        if (!preserveRootNode && node->is_root) continue;

        UnsignedInt nodeId = node->typed_id - nodeIdOffset;

        nodeObjects[nodeId] = nodeId;

        if (node->parent && (preserveRootNode || !node->parent->is_root)) {
            parents[nodeId] = Int(node->parent->typed_id - nodeIdOffset);
        } else {
            parents[nodeId] = -1;
        }

        translations[nodeId] = Vector3d(node->local_transform.translation);
        rotations[nodeId] = Quaterniond(node->local_transform.rotation);
        scalings[nodeId] = Vector3d(node->local_transform.scale);
        visibilities[nodeId] = static_cast<UnsignedByte>(node->visible);

        UnsignedInt objectId = nodeId;

        /* Create synthetic geometry node if necessary */
        if(geometricTransformNodes && node->has_geometry_transform) {
            UnsignedInt geomId = _state->originalNodeCount + syntheticNodeCount;
            objectId = geomId;

            nodeObjects[geomId] = geomId;
            parents[geomId] = Int(nodeId);
            translations[geomId] = Vector3d(node->geometry_transform.translation);
            rotations[geomId] = Quaterniond(node->geometry_transform.rotation);
            scalings[geomId] = Vector3d(node->geometry_transform.scale);
            visibilities[nodeId] = 1;

            ++syntheticNodeCount;
        }

        for (ufbx_element *element : node->all_attribs) {
            if (ufbx_mesh *mesh = ufbx_as_mesh(element)) {

                /* We may need to add multiple "chunks" for each mesh as one
                   ufbx_mesh may contain multiple materials. */
                UnsignedInt chunkOffset = _state->meshChunkBase[mesh->typed_id];
                for (UnsignedInt matIndex = 0; matIndex < mesh->materials.count; ++matIndex) {
                    const ufbx_mesh_material &mat = mesh->materials[matIndex];
                    if (mat.num_faces == 0) continue;

                    /* Meshes should ignore geometry transform if skinned as
                       the skinning matrices already contain them */
                    if (mesh->skin_deformers.count > 0) {
                        meshMaterialObjects[meshMaterialOffset] = nodeId;
                    } else {
                        meshMaterialObjects[meshMaterialOffset] = objectId;
                    }

                    if (mat.material) {
                        ufbx_material *material = perInstanceMaterials ? node->materials[matIndex] : mat.material;
                        meshMaterials[meshMaterialOffset] = Int(material->typed_id);
                    } else {
                        meshMaterials[meshMaterialOffset] = -1;
                    }
                    meshes[meshMaterialOffset] = chunkOffset;

                    ++meshMaterialOffset;
                    ++chunkOffset;
                }

            } else if (ufbx_light *light = ufbx_as_light(element)) {
                lightObjects[lightOffset] = objectId;
                lights[lightOffset] = light->typed_id;
                ++lightOffset;
            } else if (ufbx_camera *camera = ufbx_as_camera(element)) {
                cameraObjects[cameraOffset] = objectId;
                cameras[cameraOffset] = camera->typed_id;
                ++cameraOffset;
            }
        }
    }

    CORRADE_INTERNAL_ASSERT(meshMaterialOffset == meshMaterialObjects.size());
    CORRADE_INTERNAL_ASSERT(lightOffset == lightObjects.size());
    CORRADE_INTERNAL_ASSERT(cameraOffset == cameraObjects.size());
    CORRADE_INTERNAL_ASSERT(_state->originalNodeCount + syntheticNodeCount == _state->nodeCountWithSynthetic);

    /* Put everything together. For simplicity the imported data could always
       have all fields present, with some being empty, but this gives less
       noise for asset introspection purposes. */
    Containers::Array<SceneFieldData> fields;

    /* Parent, Transformation and TRS all share the implicit
       object mapping */
    arrayAppend(fields, {
        /** @todo once there's a flag to annotate implicit fields */
        SceneFieldData{SceneField::Parent, nodeObjects, parents, SceneFieldFlag::ImplicitMapping},
        SceneFieldData{SceneField::Translation, nodeObjects, translations, SceneFieldFlag::ImplicitMapping},
        SceneFieldData{SceneField::Rotation, nodeObjects, rotations, SceneFieldFlag::ImplicitMapping},
        SceneFieldData{SceneField::Scaling, nodeObjects, scalings, SceneFieldFlag::ImplicitMapping},
        SceneFieldData{SceneFieldVisibility, nodeObjects, visibilities, SceneFieldFlag::ImplicitMapping},
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
        if(name == sceneFieldNames[i])
            return sceneFieldCustom(i);
    }
    return SceneField{};
}

Containers::String UfbxImporter::doSceneFieldName(UnsignedInt name) {
    if(name < Containers::arraySize(sceneFieldNames))
        return sceneFieldNames[name];
    return {};
}

UnsignedLong UfbxImporter::doObjectCount() const {
    return _state->nodeCountWithSynthetic;
}

Long UfbxImporter::doObjectForName(const Containers::StringView name) {
    ufbx_scene *scene = _state->scene.get();
    ufbx_node *node = ufbx_find_node_len(scene, name.data(), name.size());
    return node ? Long(node->typed_id - _state->nodeIdOffset) : -1;
}

Containers::String UfbxImporter::doObjectName(const UnsignedLong id) {
    ufbx_scene *scene = _state->scene.get();
    UnsignedLong localId = id + _state->nodeIdOffset;
    if(localId < scene->nodes.count) {
        return scene->nodes[localId]->name;
    } else {
        return {};
    }
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
    ufbx_camera *cam = _state->scene->cameras[id];

    if (cam->projection_mode == UFBX_PROJECTION_MODE_PERSPECTIVE) {
        return CameraData{CameraType::Orthographic3D,
            Vector2{Float(cam->orthographic_size.x), Float(cam->orthographic_size.y)},
            Float(cam->near_plane), Float(cam->far_plane)};
    } else if (cam->projection_mode == UFBX_PROJECTION_MODE_ORTOGRAPHIC) {
        return CameraData{CameraType::Perspective3D,
            Deg(Float(cam->field_of_view_deg.x)), Float(cam->aspect_ratio),
            Float(cam->near_plane), Float(cam->far_plane)};
    } else {
        Error() << "Trade::UfbxImporter::light(): camera projection mode" << cam->projection_mode << "is not supported";
        return {};
    }
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
    const ufbx_light* l = _state->scene->lights[id];

    Float intensity = Float(l->intensity);
    Color3 color{l->color};

    Vector3 attenuation;
    LightData::Type lightType;

    if(l->type == UFBX_LIGHT_POINT) {
        lightType = LightData::Type::Point;
    } else if(l->type == UFBX_LIGHT_DIRECTIONAL) {
        lightType = LightData::Type::Directional;
    } else if(l->type == UFBX_LIGHT_SPOT) {
        lightType = LightData::Type::Spot;
    } else {
        /** @todo area and volume lights */
        Error{} << "Trade::UfbxImporter::light(): light type" << l->type << "is not supported";
        return {};
    }

    if (l->decay == UFBX_LIGHT_DECAY_NONE) {
        attenuation = {1.0f, 0.0f, 0.0f};
    } else if (l->decay == UFBX_LIGHT_DECAY_LINEAR) {
        attenuation = {0.0f, 1.0f, 0.0f};
    } else if (l->decay == UFBX_LIGHT_DECAY_QUADRATIC) {
        attenuation = {0.0f, 0.0f, 1.0f};
    } else if (l->decay == UFBX_LIGHT_DECAY_CUBIC) {
        Warning{} << "Trade::UfbxImporter::light(): cubic attenuation not supported, patching to quadratic";
        attenuation = {0.0f, 0.0f, 1.0f};
    } else {
        Error{} << "Trade::UfbxImporter::light(): light type" << l->type << "is not supported";
    }


    if((lightType == LightData::Type::Directional || lightType == LightData::Type::Ambient) && attenuation != Vector3{1.0f, 0.0f, 0.0f}) {
        Warning{} << "Trade::UfbxImporter::light(): patching attenuation" << attenuation << "to" << Vector3{1.0f, 0.0f, 0.0f} << "for" << lightType;
        attenuation = {1.0f, 0.0f, 0.0f};
    }

    Float innerAngle = 360.0f;
    Float outerAngle = 360.0f;

    if (lightType == LightData::Type::Spot) {
        innerAngle = Math::clamp(Float(l->inner_angle), 0.0f, 360.0f);
        outerAngle = Math::clamp(Float(l->outer_angle), innerAngle, 360.0f);
    }

    return LightData{lightType, color, intensity, attenuation,
        Deg{innerAngle}, Deg{outerAngle}};
}

UnsignedInt UfbxImporter::doMeshCount() const {
    return UnsignedInt(_state->meshChunks.size());
}

Containers::Optional<MeshData> UfbxImporter::doMesh(UnsignedInt id, UnsignedInt level) {
    if (level != 0) return {};

    MeshChunk chunk = _state->meshChunks[id];
    ufbx_mesh *mesh = _state->scene->meshes[chunk.meshId];
    ufbx_mesh_material mat = mesh->materials[chunk.meshMaterialIndex];

    const UnsignedInt indexCount = mat.num_triangles * 3;

    const UnsignedInt maxUvSets = unboundedIfNegative(configuration().value<Int>("maxUvSets"));
    const UnsignedInt maxTangentSets = unboundedIfNegative(configuration().value<Int>("maxTangentSets"));
    const UnsignedInt maxColorSets = unboundedIfNegative(configuration().value<Int>("maxColorSets"));

    UnsignedInt uvSetCount = Utility::min(UnsignedInt(mesh->uv_sets.count), maxUvSets);
    UnsignedInt tangentSetCount = Utility::min(uvSetCount, maxTangentSets);
    UnsignedInt bitangentSetCount = tangentSetCount;
    UnsignedInt colorSetCount = Utility::min(UnsignedInt(mesh->color_sets.count), maxColorSets);

    /* Include tangents for UV layers until we hit a layer with missing or
       incomplete tangents as at that point the implicit mapping breaks. */
    for (UnsignedInt i = 0; i < tangentSetCount; i++) {
        ufbx_uv_set &uv_set = mesh->uv_sets[i];
        if (!uv_set.vertex_tangent.exists || !mesh->uv_sets[i].vertex_bitangent.exists) {
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

    Containers::Array<UnsignedInt> triangleIndices{mesh->max_face_triangles * 3};
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

    if (mesh->vertex_normal.exists) {
        normals = {vertexData,
            reinterpret_cast<Vector3*>(vertexData + attributeOffset),
            indexCount, stride};

        attributeData[attributeIndex++] = MeshAttributeData{
            MeshAttribute::Normal, normals};
        attributeOffset += sizeof(Vector3);
    }

    for (UnsignedInt i = 0; i < uvSetCount; ++i) {
        uvSets[i] = {vertexData,
            reinterpret_cast<Vector2*>(vertexData + attributeOffset),
            indexCount, stride};

        attributeData[attributeIndex++] = MeshAttributeData{
            MeshAttribute::TextureCoordinates, uvSets[i]};
        attributeOffset += sizeof(Vector2);
    }

    for (UnsignedInt i = 0; i < tangentSetCount; ++i) {
        tangentSets[i] = {vertexData,
            reinterpret_cast<Vector3*>(vertexData + attributeOffset),
            indexCount, stride};

        attributeData[attributeIndex++] = MeshAttributeData{
            MeshAttribute::Tangent, tangentSets[i]};
        attributeOffset += sizeof(Vector3);
    }

    for (UnsignedInt i = 0; i < bitangentSetCount; ++i) {
        bitangentSets[i] = {vertexData,
            reinterpret_cast<Vector3*>(vertexData + attributeOffset),
            indexCount, stride};

        attributeData[attributeIndex++] = MeshAttributeData{
            MeshAttribute::Bitangent, bitangentSets[i]};
        attributeOffset += sizeof(Vector3);
    }

    for (UnsignedInt i = 0; i < colorSetCount; ++i) {
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
        ufbx_face face = mesh->faces[faceIndex];
        UnsignedInt numTriangles = ufbx_triangulate_face(triangleIndices.data(), triangleIndices.size(), mesh, face);
        UnsignedInt numIndices = numTriangles * 3;

        for(UnsignedInt i = 0; i < numIndices; i++) {
            UnsignedInt srcIx = triangleIndices[i];

            positions[dstIx] = Vector3(mesh->vertex_position[srcIx]);
            if (mesh->vertex_normal.exists)
                normals[dstIx] = Vector3(mesh->vertex_normal[srcIx]);
            for (UnsignedInt set = 0; set < uvSetCount; ++set)
                uvSets[set][dstIx] = Vector2(mesh->uv_sets[set].vertex_uv[srcIx]);
            for (UnsignedInt set = 0; set < tangentSetCount; ++set)
                tangentSets[set][dstIx] = Vector3(mesh->uv_sets[set].vertex_tangent[srcIx]);
            for (UnsignedInt set = 0; set < bitangentSetCount; ++set)
                bitangentSets[set][dstIx] = Vector3(mesh->uv_sets[set].vertex_bitangent[srcIx]);
            for (UnsignedInt set = 0; set < colorSetCount; ++set)
                colorSets[set][dstIx] = Color4(mesh->color_sets[set].vertex_color[srcIx]);
            dstIx++;
        }
    }

    Containers::Array<char> indexData{NoInit, indexCount*sizeof(UnsignedInt)};
    Containers::ArrayView<UnsignedInt> indices = Containers::arrayCast<UnsignedInt>(indexData);

    /* The vertex data is unindexed, so generate a contiguous index range */
    for (UnsignedInt i = 0; i < indexCount; i++)
        indices[i] = i;

    MeshData meshData{MeshPrimitive::Triangles,
        std::move(indexData), MeshIndexData{indices},
        std::move(vertexData), std::move(attributeData),
        UnsignedInt(indexCount)};

    const bool generateIndices = configuration().value<bool>("generateIndices");
    if (generateIndices)
        meshData = MeshTools::removeDuplicates(meshData);

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

Containers::Optional<MaterialData> UfbxImporter::doMaterial(UnsignedInt id) {
    ufbx_material *material = _state->scene->materials[id];

    MaterialExclusionGroups seenExclusionGroups;

    struct MaterialMappingList {
        Containers::ArrayView<const MaterialMapping> mappings;
        Containers::ArrayView<ufbx_material_map> maps;
    };
    MaterialMappingList mappingLists[] = {
        { Containers::arrayView(materialMappingPbr), Containers::arrayView(material->pbr.maps) },
        { Containers::arrayView(materialMappingFbx), Containers::arrayView(material->fbx.maps) },
    };

    /* Attributes for a single UfbxMaterialLayer */
    struct UfbxMaterialLayerAttributes {
        Containers::Array<MaterialAttributeData> defaultLayer;
        Containers::Array<Containers::Array<MaterialAttributeData>> extraLayers;
    };
    Containers::StaticArray<UfbxMaterialLayerCount, UfbxMaterialLayerAttributes> layerAttributes;

    MaterialTypes types;

    /* Do some feature detection */

    /* If we have DiffuseColor specified from the FBX properties the fallback
       FBX material should be quite well defined. */
    if (mapUsed(material->fbx.diffuse_color)) {
        types |= MaterialType::Phong;
    }

    if (mapUsed(material->pbr.metalness) && mapUsed(material->pbr.roughness)) {
        types |= MaterialType::PbrMetallicRoughness;
    }

    if (mapUsed(material->pbr.specular_color) && mapUsed(material->pbr.glossiness)) {
        types |= MaterialType::PbrSpecularGlossiness;
    }

    if (mapUsed(material->pbr.coat_factor)) {
        types |= MaterialType::PbrClearCoat;
    }

    for (UnsignedInt listIndex = 0; listIndex < 2; ++listIndex) {
        bool pbr = listIndex == 0;
        const MaterialMappingList &list = mappingLists[listIndex];
        for (const MaterialMapping &mapping : list.mappings) {
            const ufbx_material_map &map = list.maps[mapping.valueMap];

            /* Ignore maps with no value or texture */
            if (!map.has_value && !map.texture) continue;

            /* If the map has an exclusion group and we have seen one instance
               of it already, skip this one. */
            if (mapping.exclusionGroup != MaterialExclusionGroup{}) {
                if (seenExclusionGroups & mapping.exclusionGroup) continue;
                seenExclusionGroups |= mapping.exclusionGroup;
            }

            Float factor = 1.0f, opacity = 1.0f;
            if (mapping.factorMap >= 0) {
                const ufbx_material_map &factorMap = list.maps[mapping.factorMap];
                if (factorMap.has_value) {
                    factor = Float(factorMap.value_real);
                }
            }

            /* Handle some special cases */

            /* Patch opacity to BaseColor.a */
            if (pbr && mapping.valueMap == UFBX_MATERIAL_PBR_BASE_COLOR) {
                if (material->pbr.opacity.has_value && material->pbr.opacity.value_components == 1) {
                    opacity = Float(material->pbr.opacity.value_real);
                }
            }

            Containers::StringView attribute = mapping.attribute;
            UfbxMaterialLayerAttributes &attributesForLayer = layerAttributes[UnsignedInt(mapping.layer)];

            if (attribute && map.has_value) {
                Containers::Array<MaterialAttributeData> &attributes = attributesForLayer.defaultLayer;
                if (mapping.attributeType == MaterialAttributeType::Float) {
                    Float value = Float(map.value_real) * factor;
                    arrayAppend(attributes, {attribute, value});
                } else if (mapping.attributeType == MaterialAttributeType::Vector3) {
                    Vector3 value = Vector3(map.value_vec3) * factor;
                    arrayAppend(attributes, {attribute, value});
                } else if (mapping.attributeType == MaterialAttributeType::Vector4) {
                    Vector4 value = Vector4(map.value_vec4) * Vector4{factor,factor,factor,opacity};
                    arrayAppend(attributes, {attribute, value});
                } else if (mapping.attributeType == MaterialAttributeType::Long) {
                    arrayAppend(attributes, {attribute, map.value_int});
                } else if (mapping.attributeType == MaterialAttributeType::Bool) {
                    arrayAppend(attributes, {attribute, map.value_int != 0});
                } else {
                    CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
                }
            }

            if (map.texture) {
                /* We may have multiple file_textures in two cases:
                     UFBX_TEXTURE_LAYERED: Well defined texture layers
                     UFBX_TEXTURE_SHADER: Arbitrary references in a shader graph
                   Normal UFBX_TEXTURE_FILE textures also always contain
                   a single texture (themselves) in file_textures */
                UnsignedInt layer = 0;
                for (ufbx_texture *texture : map.texture->file_textures) {
                    Int textureId = _state->textureRemap[texture->typed_id];
                    if (textureId < 0) continue;

                    Containers::String textureAttribute;
                    if (mapping.textureAttribute)
                        textureAttribute = mapping.textureAttribute;
                    else
                        textureAttribute = attribute + "Texture"_s;

                    if (layer > 0 && layer - 1 >= attributesForLayer.extraLayers.size())
                        arrayResize(attributesForLayer.extraLayers, layer);

                    Containers::Array<MaterialAttributeData> &attributes = layer == 0
                        ? attributesForLayer.defaultLayer : attributesForLayer.extraLayers[layer - 1];

                    arrayAppend(attributes, {textureAttribute, UnsignedInt(textureId)});

                    if (texture->has_uv_transform) {
                        const Containers::String matrixAttribute = textureAttribute + "Matrix"_s;
                        const ufbx_matrix &mat = map.texture->uv_to_texture;
                        Matrix3 value = {
                            { Float(mat.m00), Float(mat.m10), 0.0f },
                            { Float(mat.m01), Float(mat.m11), 0.0f },
                            { Float(mat.m03), Float(mat.m13), 1.0f },
                        };
                        arrayAppend(attributes, {matrixAttribute, value});
                    }

                    /* @todo map from UV set names to indices? */

                    /* If we are a proper layered texture read blending mode.
                       Note that we may have more file_textures than layers if
                       there are shaders/recursive layers involved..
                       Only include layer details if it matches with the actual
                       file textures. */
                    if (map.texture->type == UFBX_TEXTURE_LAYERED && layer < map.texture->layers.count) {
                        const ufbx_texture_layer &texLayer = map.texture->layers[layer];
                        if (texLayer.texture == texture) {
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
    for (UnsignedInt layer = 0; layer < layerAttributes.size(); ++layer) {
        UfbxMaterialLayerAttributes &attributesForLayer = layerAttributes[UnsignedInt(layer)];

        /* Skip empty layers after the first one */
        if (layer != 0 && attributesForLayer.defaultLayer.isEmpty()) continue;

        /* Default layer */
        {
            UnsignedInt layerAttributeCount = 0;
            Containers::ArrayView<MaterialAttributeData> attributes = attributesForLayer.defaultLayer;
            if (layer != 0) {
                arrayAppend(flatAttributes, {MaterialAttribute::LayerName, ufbxMaterialLayerNames[layer]});
                ++layerAttributeCount;
            }
            arrayAppend(flatAttributes, attributes);
            layerAttributeCount += attributes.size();

            layerOffset += layerAttributeCount;
            arrayAppend(layerSizes, layerOffset);
        }

        /* Extra layers */
        for (UnsignedInt i = 0; i < attributesForLayer.extraLayers.size(); ++i) {
            UnsignedInt layerAttributeCount = 0;
            Containers::ArrayView<MaterialAttributeData> attributes = attributesForLayer.extraLayers[i];
            if (layer != 0) {
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
    ufbx_element *element = ufbx_find_element_len(_state->scene.get(), UFBX_ELEMENT_TEXTURE, name.data(), name.size());
    return element ? _state->textureRemap[element->typed_id] : -1;
}

Containers::String UfbxImporter::doTextureName(UnsignedInt id) {
    const FileTexture& fileTexture = _state->textures[id];
    return _state->scene->textures[fileTexture.textureIndex]->name;
}

Containers::Optional<TextureData> UfbxImporter::doTexture(UnsignedInt id) {
    const FileTexture& fileTexture = _state->textures[id];
    ufbx_texture *texture = _state->scene->textures[fileTexture.textureIndex];

    SamplerWrapping wrappingU = toSamplerWrapping(texture->wrap_u);
    SamplerWrapping wrappingV = toSamplerWrapping(texture->wrap_v);

    /* @todo: Image deduplication */
    return TextureData{TextureType::Texture2D,
        SamplerFilter::Linear, SamplerFilter::Linear, SamplerMipmap::Linear,
        {wrappingU, wrappingV, SamplerWrapping::ClampToEdge}, fileTexture.fileIndex};
}

AbstractImporter* UfbxImporter::setupOrReuseImporterForImage(UnsignedInt id, const char* errorPrefix) {
    ufbx_texture_file &file = _state->scene->texture_files[id];

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

    if (file.content.size > 0) {
        auto textureData = Containers::ArrayView<const char>(reinterpret_cast<const char*>(file.content.data), file.content.size);
        if(!importer.openData(textureData))
            return nullptr;
    } else if (file.filename.length > 0) {
        if (!_state->fromFile && !fileCallback()) {
            Error{} << errorPrefix << "external images can be imported only when opening files from the filesystem or if a file callback is present";
            return nullptr;
        }

        if(!importer.openFile(file.filename))
            return nullptr;
    } else {
        Error{} << errorPrefix << "empty filename";
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
    auto it = _state->textureNameMap.find(name);
    if (it != _state->textureNameMap.end()) {
        return Int(it->second);
    } else {
        return -1;
    }
}

Containers::String UfbxImporter::doImage2DName(UnsignedInt id) {
    return _state->scene->texture_files[id].relative_filename;
}

}}

CORRADE_PLUGIN_REGISTER(UfbxImporter, Magnum::Trade::UfbxImporter,
    "cz.mosra.magnum.Trade.AbstractImporter/0.5")


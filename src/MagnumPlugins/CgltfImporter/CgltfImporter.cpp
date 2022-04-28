/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
                2020, 2021, 2022 Vladimír Vondruš <mosra@centrum.cz>
    Copyright © 2021 Pablo Escobar <mail@rvrs.in>

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

#include "CgltfImporter.h"

#include <algorithm> /* std::stable_sort() */
#include <cctype>
#include <limits>
#include <unordered_map>
#include <Corrade/Containers/Array.h>
#include <Corrade/Containers/ArrayTuple.h>
#include <Corrade/Containers/ArrayView.h>
#include <Corrade/Containers/GrowableArray.h>
#include <Corrade/Containers/Optional.h>
#include <Corrade/Containers/Pair.h>
#include <Corrade/Containers/StaticArray.h>
#include <Corrade/Containers/String.h>
#include <Corrade/Containers/StringView.h>
#include <Corrade/Utility/Algorithms.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/Format.h>
#include <Corrade/Utility/Path.h>
#include <Corrade/Utility/MurmurHash2.h>
#include <Magnum/FileCallback.h>
#include <Magnum/Mesh.h>
#include <Magnum/PixelFormat.h>
#include <Magnum/Math/CubicHermite.h>
#include <Magnum/Math/FunctionsBatch.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Matrix4.h>
#include <Magnum/Math/Quaternion.h>
#include <Magnum/Trade/AnimationData.h>
#include <Magnum/Trade/CameraData.h>
#include <Magnum/Trade/ImageData.h>
#include <Magnum/Trade/LightData.h>
#include <Magnum/Trade/MaterialData.h>
#include <Magnum/Trade/MeshData.h>
#include <Magnum/Trade/SceneData.h>
#include <Magnum/Trade/SkinData.h>
#include <Magnum/Trade/TextureData.h>
#include <MagnumPlugins/AnyImageImporter/AnyImageImporter.h>

/* Cgltf doesn't load .glb on big-endian correctly:
   https://github.com/jkuhlmann/cgltf/issues/150
   Even if we patched .glb files in memory, we'd still need to convert all
   buffers from little-endian. Adds a lot of complexity, and not testable. */
#ifdef CORRADE_TARGET_BIG_ENDIAN
#error big-endian systems are not supported by cgltf
#endif

/* Since we set custom allocator callbacks for cgltf_parse, we can override
   CGLTF_MALLOC / CGLTF_FREE to assert that the default allocation functions
   aren't called */
namespace {
    /* LCOV_EXCL_START */
    void* mallocNoop(size_t) { CORRADE_INTERNAL_ASSERT_UNREACHABLE(); }
    void freeNoop(void*) { CORRADE_INTERNAL_ASSERT_UNREACHABLE(); }
    /* LCOV_EXCL_STOP */
}
#define CGLTF_MALLOC(size) mallocNoop(size)
#define CGLTF_FREE(ptr) freeNoop(ptr)
/* If we had a good replacement for ato(i|f|ll) we could set the corresponding
   CGLTF_ATOI etc. here and prevent stdlib.h from being included in cgltf.h */
/** @todo Override CGLTF_ATOI with a parsing function that handles integers
    with exponent notation:
    https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#json-encoding */

#define CGLTF_IMPLEMENTATION

#include <cgltf.h>

/* std::hash specialization to be able to use StringView in unordered_map.
   Injecting this into namespace std seems to be the designated way but it
   feels wrong. */
namespace std {
    template<> struct hash<Corrade::Containers::StringView> {
        std::size_t operator()(const Corrade::Containers::StringView& key) const {
            const Corrade::Utility::MurmurHash2 hash;
            const Corrade::Utility::HashDigest<sizeof(std::size_t)> digest = hash(key.data(), key.size());
            return *reinterpret_cast<const std::size_t*>(digest.byteArray());
        }
    };
}

namespace Magnum { namespace Trade {

using namespace Containers::Literals;
using namespace Math::Literals;

namespace {

/* Convert cgltf type enums back into strings for useful error output */
Containers::StringView gltfTypeName(cgltf_type type) {
    switch(type) {
        case cgltf_type_scalar: return "SCALAR"_s;
        case cgltf_type_vec2:   return "VEC2"_s;
        case cgltf_type_vec3:   return "VEC3"_s;
        case cgltf_type_vec4:   return "VEC4"_s;
        case cgltf_type_mat2:   return "MAT2"_s;
        case cgltf_type_mat3:   return "MAT3"_s;
        case cgltf_type_mat4:   return "MAT4"_s;
        case cgltf_type_invalid:
            break;
    }

    return "UNKNOWN"_s;
}

Containers::StringView gltfComponentTypeName(cgltf_component_type type) {
    switch(type) {
        case cgltf_component_type_r_8:   return "BYTE (5120)"_s;
        case cgltf_component_type_r_8u:  return "UNSIGNED_BYTE (5121)"_s;
        case cgltf_component_type_r_16:  return "SHORT (5122)"_s;
        case cgltf_component_type_r_16u: return "UNSIGNED_SHORT (5123)"_s;
        case cgltf_component_type_r_32u: return "UNSIGNED_INT (5125)"_s;
        case cgltf_component_type_r_32f: return "FLOAT (5126)"_s;
        case cgltf_component_type_invalid:
            break;
    }

    return "UNKNOWN"_s;
}

std::size_t elementSize(const cgltf_accessor* accessor) {
    /* Technically cgltf_calc_size isn't part of the public API but we bundle
       cgltf so there shouldn't be any surprises. Worst case we'll have to copy
       its content from an old version if it gets removed. */
    return cgltf_calc_size(accessor->type, accessor->component_type);
}

/* Data URI according to RFC 2397 */
bool isDataUri(Containers::StringView uri) {
    return uri.hasPrefix("data:"_s);
}

/* Decode percent-encoded characters in URIs:
   https://datatracker.ietf.org/doc/html/rfc3986#section-2.1
   This returns std::string because we don't have growable Containers::String
   yet. */
std::string decodeUri(Containers::StringView uri) {
    std::string decoded = uri;
    const std::size_t decodedSize = cgltf_decode_uri(&decoded[0]);
    decoded.resize(decodedSize);

    return decoded;
}

/* Cgltf's JSON parser jsmn doesn't decode escaped characters so we do it after
   parsing. If there's nothing to escape, returns an empty Optional. */
Containers::Optional<Containers::String> decodeString(Containers::StringView str) {
    /* The input string can be UTF-8 encoded but we can use a byte search here
       since all multi-byte UTF-8 characters have the high bit set and '\\'
       doesn't, so this will only match single-byte ASCII characters. */
    const Containers::StringView escape = str.find('\\');
    if(escape.isEmpty())
        return {};

    /* Skip any processing until the first escape character */
    const std::size_t start = escape.data() - str.data();

    Containers::String decoded{str};
    const std::size_t decodedSize = cgltf_decode_string(decoded.data() + start) + start;
    CORRADE_INTERNAL_ASSERT(decodedSize < str.size());

    return Containers::String{decoded.prefix(decodedSize)};
}

Containers::Array<jsmntok_t> parseJson(Containers::StringView str) {
    jsmn_parser parser{0, 0, 0};
    Int numTokens = jsmn_parse(&parser, str.data(), str.size(), nullptr, 0);
    /* All JSON strings we're parsing come from cgltf and should already have
       passed jsmn parsing */
    CORRADE_INTERNAL_ASSERT(numTokens >= 0);

    Containers::Array<jsmntok_t> tokens{std::size_t(numTokens)};
    jsmn_init(&parser);
    numTokens = jsmn_parse(&parser, str.data(), str.size(), tokens.data(), numTokens);
    CORRADE_INTERNAL_ASSERT(std::size_t(numTokens) == tokens.size());

    return tokens;
}

Containers::StringView tokenString(Containers::StringView json, const jsmntok_t& token) {
    return json.slice(token.start, token.end);
}

std::size_t skipJson(Containers::ArrayView<const jsmntok_t> tokens, std::size_t start = 0) {
    const int skipped = cgltf_skip_json(tokens, int(start));
    /* Negative return value only happens for tokens with type JSMN_UNDEFINED,
       which we should never get for valid JSON files */
    CORRADE_INTERNAL_ASSERT(skipped >= 0 && std::size_t(skipped) > start);
    return skipped;
}

}

struct CgltfImporter::Document {
    ~Document();

    Containers::Optional<Containers::String> filePath;
    Containers::Array<char> fileData;

    cgltf_options options;
    cgltf_data* data = nullptr;

    /* Storage for buffer content if the user set no file callback or a buffer
       is embedded as base64. These are filled on demand. We don't check for
       duplicate URIs since that's incredibly unlikely and hard to get right,
       so the buffer id is used as the index. */
    Containers::Array<Containers::Array<char>> bufferData;

    /* Decode and cache strings in a map indexed by the input view data
       pointer. This works because we only call this function with views on
       strings from cgltf_data.

       Note that parsing inside cgltf happens with unescaped strings, but we
       have no influence on that. In practice, this shouldn't be a problem. Old
       versions of the spec used to explicitly forbid non-ASCII keys/enums:
       https://github.com/KhronosGroup/glTF/tree/fd3ab461a1114fb0250bd76099153d2af50a7a1d/specification/2.0#json-encoding
       Newer spec versions changed this to "ASCII characters [...] SHOULD be
       written without JSON escaping". */
    Containers::StringView decodeCachedString(Containers::StringView str);

    std::unordered_map<const char*, const Containers::String> decodedStrings;

    /* We can use StringView as the map key here because all underlying strings
       won't go out of scope while a file is opened. They either point to the
       original name strings in cgltf_data or to decodedStrings. */
    Containers::Optional<std::unordered_map<Containers::StringView, Int>>
        animationsForName,
        camerasForName,
        lightsForName,
        scenesForName,
        skinsForName,
        nodesForName,
        meshesForName,
        materialsForName,
        imagesForName,
        texturesForName;

    /* Unlike the ones above, these are filled already during construction as
       we need them in three different places and on-demand construction would
       be too annoying to test. Also, assuming the importer knows all builtin
       names, in most cases these would be empty anyway. */
    std::unordered_map<Containers::StringView, MeshAttribute>
        meshAttributesForName;
    Containers::Array<Containers::StringView> meshAttributeNames;

    /* Mapping for multi-primitive meshes:

        -   meshMap.size() is the count of meshes reported to the user
        -   meshSizeOffsets.size() is the count of original meshes in the file
        -   meshMap[id] is a pair of (original mesh ID, primitive ID)
        -   meshSizeOffsets[j] points to the first item in meshMap for
            original mesh ID `j` -- which also translates the original ID to
            reported ID
        -   meshSizeOffsets[j + 1] - meshSizeOffsets[j] is count of meshes for
            original mesh ID `j` (or number of primitives in given mesh)
    */
    Containers::Array<Containers::Pair<std::size_t, std::size_t>> meshMap;
    Containers::Array<std::size_t> meshSizeOffsets;

    /* If a file contains texture coordinates that are not floats or normalized
       in the 0-1, the textureCoordinateYFlipInMaterial option is enabled
       implicitly as we can't perform Y-flip directly on the data. */
    bool textureCoordinateYFlipInMaterial = false;

    void materialTexture(const cgltf_texture_view& texture, Containers::Array<MaterialAttributeData>& attributes, Containers::StringView attribute, Containers::StringView matrixAttribute, Containers::StringView coordinateAttribute) const;

    bool open = false;

    UnsignedInt imageImporterId = ~UnsignedInt{};
    Containers::Optional<AnyImageImporter> imageImporter;
};

CgltfImporter::Document::~Document() {
    if(data) cgltf_free(data);
}

Containers::Optional<Containers::ArrayView<const char>> CgltfImporter::loadUri(const char* const errorPrefix, const Containers::StringView uri, Containers::Array<char>& storage) {
    if(isDataUri(uri)) {
        /* Data URI with base64 payload according to RFC 2397:
           data:[<mediatype>][;base64],<data> */
        Containers::StringView base64;
        const Containers::Array3<Containers::StringView> parts = uri.partition(',');

        /* Non-base64 data URIs are allowed by RFC 2397, but make no sense for
           glTF. cgltf_load_buffers doesn't allow them, either. */
        if(parts.front().hasSuffix(";base64"_s)) {
            /* This will be empty for both a missing comma and an empty payload */
            base64 = parts.back();
        }

        if(base64.isEmpty()) {
            Error{} << errorPrefix << "data URI has no base64 payload";
            return {};
        }

        /* Decoded size. For some reason cgltf_load_buffer_base64 doesn't take
           the string length as input, and fails if it finds a padding
           character. */
        const std::size_t padding = base64.size() - base64.trimmedSuffix("="_s).size();
        const std::size_t size = base64.size()/4*3 - padding;

        /* cgltf_load_buffer_base64 will allocate using the memory callbacks
           set in doOpenData() which use new char[] and delete[]. We can wrap
           that memory in an Array with the default deleter. */
        void* decoded = nullptr;
        const cgltf_result result = cgltf_load_buffer_base64(&_d->options, size, base64.data(), &decoded);
        if(result == cgltf_result_success) {
            CORRADE_INTERNAL_ASSERT(decoded);
            storage = Containers::Array<char>{static_cast<char*>(decoded), size};
            return Containers::ArrayView<const char>{storage};
        }

        Error{} << errorPrefix << "invalid base64 string in data URI";
        return {};

    } else if(fileCallback()) {
        const Containers::String fullPath = Utility::Path::join(_d->filePath ? *_d->filePath : "", decodeUri(_d->decodeCachedString(uri)));
        if(const Containers::Optional<Containers::ArrayView<const char>> view = fileCallback()(fullPath, InputFileCallbackPolicy::LoadPermanent, fileCallbackUserData()))
            return *view;

        Error{} << errorPrefix << "error opening" << fullPath << "through a file callback";
        return {};

    } else {
        if(!_d->filePath) {
            Error{} << errorPrefix << "external buffers can be imported only when opening files from the filesystem or if a file callback is present";
            return {};
        }

        const Containers::String fullPath = Utility::Path::join(*_d->filePath, decodeUri(_d->decodeCachedString(uri)));
        if(Containers::Optional<Containers::Array<char>> data = Utility::Path::read(fullPath)) {
            storage = *std::move(data);
            return Containers::arrayCast<const char>(storage);
        }

        Error{} << errorPrefix << "error opening" << fullPath;
        return {};
    }
}

bool CgltfImporter::loadBuffer(const char* const errorPrefix, const UnsignedInt id) {
    CORRADE_INTERNAL_ASSERT(id < _d->data->buffers_count);
    cgltf_buffer& buffer = _d->data->buffers[id];
    if(buffer.data)
        return true;

    Containers::ArrayView<const char> view;
    if(buffer.uri) {
        const Containers::Optional<Containers::ArrayView<const char>> loaded = loadUri(errorPrefix, buffer.uri, _d->bufferData[id]);
        if(!loaded) return false;
        view = *loaded;
    } else {
        /* URI may only be empty for buffers referencing the glb binary blob */
        if(id != 0 || !_d->data->bin) {
            Error{} << errorPrefix << "buffer" << id << "has no URI";
            return false;
        }
        view = Containers::arrayView(static_cast<const char*>(_d->data->bin), _d->data->bin_size);
    }

    /* The spec mentions that non-GLB buffer length can be greater than
       byteLength. GLB buffer chunks may also be up to 3 bytes larger than
       byteLength because of padding. So we can't check for equality. */
    if(view.size() < buffer.size) {
        Error{} << errorPrefix << "buffer" << id << "is too short, expected"
            << buffer.size << "bytes but got" << view.size();
        return false;
    }

    buffer.data = const_cast<char*>(view.data()); /* sigh */
    /* Tell cgltf not to free buffer.data in cgltf_free */
    buffer.data_free_method = cgltf_data_free_method_none;

    return true;
}

bool CgltfImporter::checkBufferView(const char* const errorPrefix, const cgltf_buffer_view* const bufferView) {
    CORRADE_INTERNAL_ASSERT(bufferView);
    const cgltf_buffer* buffer = bufferView->buffer;
    const std::size_t requiredBufferSize = bufferView->offset + bufferView->size;
    if(buffer->size < requiredBufferSize) {
        const UnsignedInt bufferViewId = bufferView - _d->data->buffer_views;
        const UnsignedInt bufferId = buffer - _d->data->buffers;
        Error{} << errorPrefix << "buffer view" << bufferViewId << "needs" << requiredBufferSize << "bytes but buffer" << bufferId << "has only" << buffer->size;
        return false;
    }

    return true;
}

bool CgltfImporter::checkAccessor(const char* const errorPrefix, const cgltf_accessor* const accessor) {
    CORRADE_INTERNAL_ASSERT(accessor);
    const UnsignedInt accessorId = accessor - _d->data->accessors;

    /** @todo Validate alignment rules, calculate correct stride in accessorView():
        https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#data-alignment */

    if(accessor->is_sparse) {
        Error{} << errorPrefix << "accessor" << accessorId << "is using sparse storage, which is unsupported";
        return false;
    }
    /* Buffer views are optional in accessors, we're supposed to fill the view
       with zeros. Only makes sense with sparse data and we don't support
       that. */
    if(!accessor->buffer_view) {
        Error{} << errorPrefix << "accessor" << accessorId << "has no buffer view";
        return false;
    }

    const cgltf_buffer_view* bufferView = accessor->buffer_view;
    const UnsignedInt bufferViewId = bufferView - _d->data->buffer_views;

    const std::size_t typeSize = elementSize(accessor);
    const std::size_t requiredBufferViewSize = accessor->offset + accessor->stride*(accessor->count - 1) + typeSize;
    if(bufferView->size < requiredBufferViewSize) {
        Error{} << errorPrefix << "accessor" << accessorId << "needs" << requiredBufferViewSize << "bytes but buffer view" << bufferViewId << "has only" << bufferView->size;
        return false;
    }

    if(!checkBufferView(errorPrefix, bufferView))
        return false;

    /* Cgltf copies the bufferview stride into the accessor. If that's zero, it
       copies the element size into the stride. */
    if(accessor->stride < typeSize) {
        Error{} << errorPrefix << typeSize << Debug::nospace << "-byte type defined by accessor" << accessorId << "can't fit into buffer view" << bufferViewId << "stride of" << accessor->stride;
        return false;
    }

    return true;
}

Containers::Optional<Containers::StridedArrayView2D<const char>> CgltfImporter::accessorView(const char* const errorPrefix, const cgltf_accessor* const accessor) {
    if(!checkAccessor(errorPrefix, accessor))
        return {};

    const cgltf_buffer_view* bufferView = accessor->buffer_view;
    const cgltf_buffer* buffer = bufferView->buffer;
    const UnsignedInt bufferId = buffer - _d->data->buffers;
    if(!loadBuffer(errorPrefix, bufferId))
        return {};

    return Containers::StridedArrayView2D<const char>{Containers::arrayView(buffer->data, buffer->size),
        reinterpret_cast<const char*>(buffer->data) + bufferView->offset + accessor->offset,
        {accessor->count, elementSize(accessor)},
        {std::ptrdiff_t(accessor->stride), 1}};
}

Containers::StringView CgltfImporter::Document::decodeCachedString(Containers::StringView str) {
    if(str.isEmpty())
        return str;

    /* StringView constructed from nullptr doesn't have this flag, but it's
       caught by isEmpty() above */
    CORRADE_INTERNAL_ASSERT(str.flags() >= Containers::StringViewFlag::NullTerminated);

    /* Return cached value if the string has been decoded before */
    const auto found = decodedStrings.find(str.data());
    if(found != decodedStrings.end())
        return found->second;

    Containers::Optional<Containers::String> decoded = decodeString(str);
    /* Nothing to escape. This creates a non-owning String with a view on the
       input data. */
    if(!decoded)
        return decodedStrings.emplace(str.data(), Containers::String::nullTerminatedView(str)).first->second;

    return decodedStrings.emplace(str.data(), std::move(*decoded)).first->second;
}

namespace {

void fillDefaultConfiguration(Utility::ConfigurationGroup& conf) {
    /** @todo horrible workaround, fix this properly */
    conf.setValue("ignoreRequiredExtensions", false);
    conf.setValue("optimizeQuaternionShortestPath", true);
    conf.setValue("normalizeQuaternions", true);
    conf.setValue("mergeAnimationClips", false);
    conf.setValue("phongMaterialFallback", true);
    conf.setValue("objectIdAttribute", "_OBJECT_ID");
}

}

CgltfImporter::CgltfImporter() {
    /** @todo horrible workaround, fix this properly */
    fillDefaultConfiguration(configuration());
}

CgltfImporter::CgltfImporter(PluginManager::AbstractManager& manager, const Containers::StringView& plugin): AbstractImporter{manager, plugin} {}

CgltfImporter::CgltfImporter(PluginManager::Manager<AbstractImporter>& manager): AbstractImporter{manager} {
    /** @todo horrible workaround, fix this properly */
    fillDefaultConfiguration(configuration());
}

CgltfImporter::~CgltfImporter() = default;

ImporterFeatures CgltfImporter::doFeatures() const { return ImporterFeature::OpenData|ImporterFeature::FileCallback; }

bool CgltfImporter::doIsOpened() const { return !!_d && _d->open; }

void CgltfImporter::doClose() { _d = nullptr; }

void CgltfImporter::doOpenFile(const Containers::StringView filename) {
    _d.reset(new Document);
    /* Since the slice won't be null terminated, nullTerminatedGlobalView()
       won't help anything here */
    _d->filePath.emplace(Utility::Path::split(filename).first());
    AbstractImporter::doOpenFile(filename);
}

void CgltfImporter::doOpenData(Containers::Array<char>&& data, const DataFlags dataFlags) {
    if(!_d) _d.reset(new Document);

    /* Copy file content. Take over the existing array or copy the data if we
       can't. We need to keep the data around for .glb binary blobs and
       extension data which cgltf stores as pointers into the original memory
       passed to cgltf_parse. */
    if(dataFlags & (DataFlag::Owned|DataFlag::ExternallyOwned)) {
        _d->fileData = std::move(data);
    } else {
        _d->fileData = Containers::Array<char>{NoInit, data.size()};
        Utility::copy(data, _d->fileData);
    }

    /* Auto-detect glb/gltf */
    _d->options.type = cgltf_file_type::cgltf_file_type_invalid;
    /* Determine json token count to allocate (by parsing twice) */
    _d->options.json_token_count = 0;

    /* Set up memory callbacks. The default memory callbacks (when set to
       nullptr) use malloc and free. Prefer using new and delete, allows us to
       use the default deleter when wrapping memory in Array, and it'll throw
       bad_alloc if allocation fails. */
    _d->options.memory.alloc = [](void*, cgltf_size size) -> void* { return new char[size]; };
    _d->options.memory.free = [](void*, void* ptr) { delete[] static_cast<char*>(ptr); };
    _d->options.memory.user_data = nullptr;

    /* The file callbacks are only needed for cgltf_load_buffers which we don't
       call, but we still replace the default ones to assert that they're
       never called. Unfortunately, this doesn't prevent cgltf from linking to
       stdio anyway. */
    _d->options.file.read = [](const cgltf_memory_options*, const cgltf_file_options*, const char*, cgltf_size*, void**) -> cgltf_result {
        CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
    };
    _d->options.file.release = [](const cgltf_memory_options*, const cgltf_file_options*, void* ptr) -> void {
        /* cgltf_free calls this function with a nullptr file_data that's only
           set when using cgltf_parse_file */
        if(ptr == nullptr) return;
        CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
    };
    _d->options.file.user_data = nullptr;

    /* Parse file, without loading or decoding buffers/images */
    const cgltf_result result = cgltf_parse(&_d->options, _d->fileData.data(), _d->fileData.size(), &_d->data);

    /* A general note on error checking in cgltf:
       - cgltf_parse fails if any index is out of bounds, mandatory or not
       - cgltf_parse fails if a mandatory property is missing
       - optional properties are set to the spec-mandated default value (if
         there is one), 0 or nullptr (if they're indices).

       We're not using cgltf_validate() because the error granularity is rather
       underwhelming. All of its relevant checks are implemented on our side,
       allowing us to delay them to when they're needed, e.g. accessor and
       buffer size. */
    if(result != cgltf_result_success) {
        const char* error{};
        switch(result) {
            /* This can also be returned for arrays with too many items before
               any allocation happens, so we can't quite ignore it. Rather
               impossible to test, however. */
            case cgltf_result_out_of_memory:
                error = "out of memory";
                break;
            case cgltf_result_unknown_format:
                error = "unknown binary glTF format";
                break;
            case cgltf_result_invalid_json:
                error = "invalid JSON";
                break;
            case cgltf_result_invalid_gltf:
                error = "invalid glTF, usually caused by invalid indices or missing required attributes";
                break;
            case cgltf_result_legacy_gltf:
                error = "legacy glTF version";
                break;
            case cgltf_result_data_too_short:
                error = "data too short";
                break;
            /* LCOV_EXCL_START */
            /* Only returned from cgltf's default file callback */
            case cgltf_result_file_not_found:
            /* Only returned by cgltf_load_buffer_base64 and cgltf's default
               file callback */
            case cgltf_result_io_error:
            /* We passed a nullptr somewhere, this should never happen */
            case cgltf_result_invalid_options:
            default:
                CORRADE_INTERNAL_ASSERT_UNREACHABLE();
            /* LCOV_EXCL_STOP */
        }

        Error{} << "Trade::CgltfImporter::openData(): error opening file:" << error;
        doClose();
        return;
    }

    CORRADE_INTERNAL_ASSERT(_d->data != nullptr);

    /* Major versions are forward- and backward-compatible, but minVersion can
       be used to require support for features added in new minor versions.
       So far there's only 2.0 so we can use an exact comparison.
       cgltf already checked that asset.version >= 2.0 (if it exists). */
    const cgltf_asset& asset = _d->data->asset;
    if(asset.min_version && asset.min_version != "2.0"_s) {
        Error{} << "Trade::CgltfImporter::openData(): unsupported minVersion" << asset.min_version << Debug::nospace << ", expected 2.0";
        doClose();
        return;
    }
    if(asset.version && !Containers::StringView{asset.version}.hasPrefix("2."_s)) {
        Error{} << "Trade::CgltfImporter::openData(): unsupported version" << asset.version << Debug::nospace << ", expected 2.x";
        doClose();
        return;
    }

    /* Check required extensions. Every extension in extensionsRequired is
       required to "load and/or render an asset". */
    /** @todo Allow ignoring specific extensions through a config option, e.g.
        ignoreRequiredExtension=KHR_materials_volume */
    const bool ignoreRequiredExtensions = configuration().value<bool>("ignoreRequiredExtensions");

    constexpr Containers::StringView supportedExtensions[]{
        /* Parsed by cgltf and handled by us */
        "KHR_lights_punctual"_s,
        "KHR_materials_clearcoat"_s,
        "KHR_materials_pbrSpecularGlossiness"_s,
        "KHR_materials_unlit"_s,
        "KHR_mesh_quantization"_s,
        "KHR_texture_basisu"_s,
        "KHR_texture_transform"_s,
        /* Manually parsed */
        "GOOGLE_texture_basis"_s,
        "MSFT_texture_dds"_s
    };

    /* M*N loop should be okay here, extensionsRequired should usually have no
       or very few entries. Consider binary search if the list of supported
       extensions reaches a few dozen. */
    for(Containers::StringView required: Containers::arrayView(_d->data->extensions_required, _d->data->extensions_required_count)) {
        bool found = false;
        for(const auto& supported: supportedExtensions) {
            if(supported == required) {
                found = true;
                break;
            }
        }

        if(!found) {
            if(ignoreRequiredExtensions) {
                Warning{} << "Trade::CgltfImporter::openData(): required extension" << required << "not supported";
            } else {
                Error{} << "Trade::CgltfImporter::openData(): required extension" << required << "not supported";
                doClose();
                return;
            }
        }
    }

    /* Find cycles in node tree */
    for(std::size_t i = 0; i != _d->data->nodes_count; ++i) {
        const cgltf_node* p1 = _d->data->nodes[i].parent;
        const cgltf_node* p2 = p1 ? p1->parent : nullptr;

        while(p1 && p2) {
            if(p1 == p2) {
                Error{} << "Trade::CgltfImporter::openData(): node tree contains cycle starting at node" << i;
                doClose();
                return;
            }

            p1 = p1->parent;
            p2 = p2->parent ? p2->parent->parent : nullptr;
        }
    }

    /* Treat meshes with multiple primitives as separate meshes. Each mesh gets
       duplicated as many times as is the size of the primitives array. */
    arrayReserve(_d->meshMap, _d->data->meshes_count);
    _d->meshSizeOffsets = Containers::Array<std::size_t>{_d->data->meshes_count + 1};

    _d->meshSizeOffsets[0] = 0;
    for(std::size_t i = 0; i != _d->data->meshes_count; ++i) {
        const std::size_t count = _d->data->meshes[i].primitives_count;
        CORRADE_INTERNAL_ASSERT(count > 0);
        for(std::size_t j = 0; j != count; ++j)
            arrayAppend(_d->meshMap, InPlaceInit, i, j);

        _d->meshSizeOffsets[i + 1] = _d->meshMap.size();
    }

    /* Go through all meshes, collect custom attributes and decide about
       implicitly enabling textureCoordinateYFlipInMaterial if it isn't already
       requested from the configuration and there are any texture coordinates
       that need it */
    if(configuration().value<bool>("textureCoordinateYFlipInMaterial"))
        _d->textureCoordinateYFlipInMaterial = true;
    for(const cgltf_mesh& mesh: Containers::arrayView(_d->data->meshes, _d->data->meshes_count)) {
        for(const cgltf_primitive& primitive: Containers::arrayView(mesh.primitives, mesh.primitives_count)) {
            for(const cgltf_attribute& attribute: Containers::arrayView(primitive.attributes, primitive.attributes_count)) {
                if(attribute.type == cgltf_attribute_type_texcoord) {
                    if(!_d->textureCoordinateYFlipInMaterial) {
                        const cgltf_component_type type = attribute.data->component_type;
                        const bool normalized = attribute.data->normalized;
                        if(type == cgltf_component_type_r_8 ||
                           type == cgltf_component_type_r_16 ||
                          (type == cgltf_component_type_r_8u && !normalized) ||
                          (type == cgltf_component_type_r_16u && !normalized)) {
                            Debug{} << "Trade::CgltfImporter::openData(): file contains non-normalized texture coordinates, implicitly enabling textureCoordinateYFlipInMaterial";
                            _d->textureCoordinateYFlipInMaterial = true;
                        }
                    }

                /* If the name isn't recognized or not in MeshAttribute, add
                   the attribute to custom if not there already */
                } else if(attribute.type != cgltf_attribute_type_position &&
                    attribute.type != cgltf_attribute_type_normal &&
                    attribute.type != cgltf_attribute_type_tangent &&
                    attribute.type != cgltf_attribute_type_color)
                {
                    /* Get the semantic base name ([semantic]_[set_index]) for
                       known attributes that are not supported in MeshAttribute
                       (JOINTS_n and WEIGHTS_n). This lets us group multiple
                       sets to the same attribute.
                       For unknown/user-defined attributes all name formats are
                       allowed and we don't attempt to group them. */
                    /** @todo Remove all this once Magnum adds these to MeshAttribute
                       (pending https://github.com/mosra/magnum/pull/441) */
                    const Containers::StringView name{attribute.name};
                    const Containers::StringView semantic = attribute.type != cgltf_attribute_type_invalid ?
                        name.partition('_')[0] : name;

                    /* The spec says that all user-defined attributes must
                       start with an underscore. We don't really care and just
                       print a warning. */
                    if(attribute.type == cgltf_attribute_type_invalid && !name.hasPrefix("_"_s))
                        Warning{} << "Trade::CgltfImporter::openData(): unknown attribute" << name << Debug::nospace << ", importing as custom attribute";

                    if(_d->meshAttributesForName.emplace(semantic,
                        meshAttributeCustom(_d->meshAttributeNames.size())).second)
                        arrayAppend(_d->meshAttributeNames, semantic);
                }
            }
        }
    }

    _d->open = true;

    /* Buffers are loaded on demand, but we need to prepare the storage array */
    _d->bufferData = Containers::Array<Containers::Array<char>>{_d->data->buffers_count};

    /* Name maps are lazy-loaded because these might not be needed every time */
}

UnsignedInt CgltfImporter::doAnimationCount() const {
    /* If the animations are merged, there's at most one */
    if(configuration().value<bool>("mergeAnimationClips"))
        return _d->data->animations_count == 0 ? 0 : 1;

    return _d->data->animations_count;
}

Int CgltfImporter::doAnimationForName(const Containers::StringView name) {
    /* If the animations are merged, don't report any names */
    if(configuration().value<bool>("mergeAnimationClips")) return -1;

    if(!_d->animationsForName) {
        _d->animationsForName.emplace();
        _d->animationsForName->reserve(_d->data->animations_count);
        for(std::size_t i = 0; i != _d->data->animations_count; ++i)
            _d->animationsForName->emplace(_d->decodeCachedString(_d->data->animations[i].name), i);
    }

    const auto found = _d->animationsForName->find(name);
    return found == _d->animationsForName->end() ? -1 : found->second;
}

Containers::String CgltfImporter::doAnimationName(UnsignedInt id) {
    /* If the animations are merged, don't report any names */
    if(configuration().value<bool>("mergeAnimationClips")) return {};
    return _d->decodeCachedString(_d->data->animations[id].name);
}

namespace {

template<class V> void postprocessSplineTrack(const cgltf_accessor* timeTrackUsed, const Containers::ArrayView<const Float> keys, const Containers::ArrayView<Math::CubicHermite<V>> values) {
    /* Already processed, don't do that again */
    if(timeTrackUsed != nullptr) return;

    CORRADE_INTERNAL_ASSERT(keys.size() == values.size());
    if(keys.size() < 2) return;

    /* Convert the `a` values to `n` and the `b` values to `m` as described in
       https://github.com/KhronosGroup/glTF/tree/master/specification/2.0#appendix-c-spline-interpolation
       Unfortunately I was not able to find any concrete name for this, so it's
       not part of the CubicHermite implementation but is kept here locally. */
    for(std::size_t i = 0; i < keys.size() - 1; ++i) {
        const Float timeDifference = keys[i + 1] - keys[i];
        values[i].outTangent() *= timeDifference;
        values[i + 1].inTangent() *= timeDifference;
    }
}

}

Containers::Optional<AnimationData> CgltfImporter::doAnimation(UnsignedInt id) {
    /* Import either a single animation or all of them together. At the moment,
       Blender doesn't really support cinematic animations (affecting multiple
       objects): https://blender.stackexchange.com/q/5689. And since
       https://github.com/KhronosGroup/glTF-Blender-Exporter/pull/166, these
       are exported as a set of object-specific clips, which may not be wanted,
       so we give the users an option to merge them all together. */
    const std::size_t animationBegin =
        configuration().value<bool>("mergeAnimationClips") ? 0 : id;
    const std::size_t animationEnd =
        configuration().value<bool>("mergeAnimationClips") ? _d->data->animations_count : id + 1;

    const Containers::ArrayView<cgltf_animation> animations = Containers::arrayView(
        _d->data->animations + animationBegin, animationEnd - animationBegin);

    /* First gather the input and output data ranges. Key is unique accessor
       pointer so we don't duplicate shared data, value is range in the input
       buffer, offset in the output data and pointer of the corresponding key
       track in case given track is a spline interpolation. The key pointer is
       initialized to nullptr and will be used later to check that a spline
       track was not used with more than one time track, as it needs to be
       postprocessed for given time track. */
    struct SamplerData {
        Containers::StridedArrayView2D<const char> src;
        std::size_t outputOffset;
        const cgltf_accessor* timeTrack;
    };
    std::unordered_map<const cgltf_accessor*, SamplerData> samplerData;
    std::size_t dataSize = 0;
    for(const cgltf_animation& animation: animations) {
        for(std::size_t i = 0; i != animation.samplers_count; ++i) {
            const cgltf_animation_sampler& sampler = animation.samplers[i];

            /** @todo handle alignment once we do more than just four-byte types */

            /* If the input view is not yet present in the output data buffer,
               add it */
            if(samplerData.find(sampler.input) == samplerData.end()) {
                Containers::Optional<Containers::StridedArrayView2D<const char>> view = accessorView("Trade::CgltfImporter::animation():", sampler.input);
                if(!view)
                    return {};

                samplerData.emplace(sampler.input, SamplerData{*view, dataSize, nullptr});
                dataSize += view->size()[0]*view->size()[1];
            }

            /* If the output view is not yet present in the output data buffer,
               add it */
            if(samplerData.find(sampler.output) == samplerData.end()) {
                Containers::Optional<Containers::StridedArrayView2D<const char>> view = accessorView("Trade::CgltfImporter::animation():", sampler.output);
                if(!view)
                    return {};

                samplerData.emplace(sampler.output, SamplerData{*view, dataSize, nullptr});
                dataSize += view->size()[0]*view->size()[1];
            }
        }
    }

    /* Populate the data array */
    /**
     * @todo Once memory-mapped files are supported, this can all go away
     *      except when spline tracks are present -- in that case we need to
     *      postprocess them and can't just use the memory directly.
     */
    Containers::Array<char> data{dataSize};
    for(const std::pair<const cgltf_accessor* const, SamplerData>& view: samplerData) {
        Containers::StridedArrayView2D<const char> src = view.second.src;
        Containers::StridedArrayView2D<char> dst{data.exceptPrefix(view.second.outputOffset),
            src.size()};
        Utility::copy(src, dst);
    }

    /* Calculate total track count. If merging all animations together, this is
       the sum of all clip track counts. */
    std::size_t trackCount = 0;
    for(const cgltf_animation& animation: animations) {
        for(std::size_t i = 0; i != animation.channels_count; ++i) {
            /* Skip animations without a target node. See comment below. */
            if(animation.channels[i].target_node)
                ++trackCount;
        }
    }

    /* Import all tracks */
    bool hadToRenormalize = false;
    std::size_t trackId = 0;
    Containers::Array<Trade::AnimationTrackData> tracks{trackCount};
    for(const cgltf_animation& animation: animations) {
        for(std::size_t i = 0; i != animation.channels_count; ++i) {
            const cgltf_animation_channel& channel = animation.channels[i];
            const cgltf_animation_sampler& sampler = *channel.sampler;

            /* Skip animations without a target node. Consistent with
               tinygltf's behavior, currently there are no extensions for
               animating materials or anything else so there's no point in
               importing such animations. */
            /** @todo revisit once KHR_animation2 is a thing:
                https://github.com/KhronosGroup/glTF/pull/2033 */
            if(!channel.target_node)
                continue;

            /* Key properties -- always float time. Not using checkAccessor()
               as this was all checked above once already. */
            const cgltf_accessor* input = sampler.input;
            if(input->type != cgltf_type_scalar || input->component_type != cgltf_component_type_r_32f || input->normalized) {
                Error{} << "Trade::CgltfImporter::animation(): time track has unexpected type"
                    << (input->normalized ? "normalized " : "") << Debug::nospace
                    << gltfTypeName(input->type) << "/" << gltfComponentTypeName(input->component_type);
                return {};
            }

            /* View on the key data */
            const auto inputDataFound = samplerData.find(input);
            CORRADE_INTERNAL_ASSERT(inputDataFound != samplerData.end());
            const auto keys = Containers::arrayCast<Float>(
                data.exceptPrefix(inputDataFound->second.outputOffset).prefix(
                    inputDataFound->second.src.size()[0]*
                    inputDataFound->second.src.size()[1]));

            /* Interpolation mode */
            Animation::Interpolation interpolation;
            if(sampler.interpolation == cgltf_interpolation_type_linear) {
                interpolation = Animation::Interpolation::Linear;
            } else if(sampler.interpolation == cgltf_interpolation_type_cubic_spline) {
                interpolation = Animation::Interpolation::Spline;
            } else if(sampler.interpolation == cgltf_interpolation_type_step) {
                interpolation = Animation::Interpolation::Constant;
            } else {
                /* There is no cgltf_interpolation_type_invalid, cgltf falls
                   back to linear for invalid interpolation modes */
                CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
            }

            /* Decide on value properties. Not using checkAccessor() as this
               was all checked above once already. */
            const cgltf_accessor* output = sampler.output;
            AnimationTrackTargetType target;
            AnimationTrackType type, resultType;
            Animation::TrackViewStorage<const Float> track;
            const auto outputDataFound = samplerData.find(output);
            CORRADE_INTERNAL_ASSERT(outputDataFound != samplerData.end());
            const auto outputData = data.exceptPrefix(outputDataFound->second.outputOffset)
                .prefix(outputDataFound->second.src.size()[0]*
                        outputDataFound->second.src.size()[1]);
            const cgltf_accessor*& timeTrackUsed = outputDataFound->second.timeTrack;

            const std::size_t valuesPerKey = interpolation == Animation::Interpolation::Spline ? 3 : 1;
            if(input->count*valuesPerKey != output->count) {
                Error{} << "Trade::CgltfImporter::animation(): target track size doesn't match time track size, expected" << output->count << "but got" << input->count*valuesPerKey;
                return {};
            }

            /* Translation */
            if(channel.target_path == cgltf_animation_path_type_translation) {
                if(output->type != cgltf_type_vec3 || output->component_type != cgltf_component_type_r_32f || output->normalized) {
                    Error{} << "Trade::CgltfImporter::animation(): translation track has unexpected type"
                        << (output->normalized ? "normalized " : "") << Debug::nospace
                        << gltfTypeName(output->type) << "/" << gltfComponentTypeName(output->component_type);
                    return {};
                }

                /* View on the value data */
                target = AnimationTrackTargetType::Translation3D;
                resultType = AnimationTrackType::Vector3;
                if(interpolation == Animation::Interpolation::Spline) {
                    /* Postprocess the spline track. This can be done only once
                       for every track -- postprocessSplineTrack() checks
                       that. */
                    const auto values = Containers::arrayCast<CubicHermite3D>(outputData);
                    postprocessSplineTrack(timeTrackUsed, keys, values);

                    type = AnimationTrackType::CubicHermite3D;
                    track = Animation::TrackView<const Float, const CubicHermite3D>{
                        keys, values, interpolation,
                        animationInterpolatorFor<CubicHermite3D>(interpolation),
                        Animation::Extrapolation::Constant};
                } else {
                    type = AnimationTrackType::Vector3;
                    track = Animation::TrackView<const Float, const Vector3>{keys,
                        Containers::arrayCast<Vector3>(outputData),
                        interpolation,
                        animationInterpolatorFor<Vector3>(interpolation),
                        Animation::Extrapolation::Constant};
                }

            /* Rotation */
            } else if(channel.target_path == cgltf_animation_path_type_rotation) {
                /** @todo rotation can be also normalized (?!) to a vector of 8/16bit (signed?!) integers
                    cgltf_accessor_unpack_floats might help with unpacking them */

                if(output->type != cgltf_type_vec4 || output->component_type != cgltf_component_type_r_32f || output->normalized) {
                    Error{} << "Trade::CgltfImporter::animation(): rotation track has unexpected type"
                        << (output->normalized ? "normalized " : "") << Debug::nospace
                        << gltfTypeName(output->type) << "/" << gltfComponentTypeName(output->component_type);
                    return {};
                }

                /* View on the value data */
                target = AnimationTrackTargetType::Rotation3D;
                resultType = AnimationTrackType::Quaternion;
                if(interpolation == Animation::Interpolation::Spline) {
                    /* Postprocess the spline track. This can be done only once
                       for every track -- postprocessSplineTrack() checks
                       that. */
                    const auto values = Containers::arrayCast<CubicHermiteQuaternion>(outputData);
                    postprocessSplineTrack(timeTrackUsed, keys, values);

                    type = AnimationTrackType::CubicHermiteQuaternion;
                    track = Animation::TrackView<const Float, const CubicHermiteQuaternion>{
                        keys, values, interpolation,
                        animationInterpolatorFor<CubicHermiteQuaternion>(interpolation),
                        Animation::Extrapolation::Constant};
                } else {
                    /* Ensure shortest path is always chosen. Not doing this
                       for spline interpolation, there it would cause war and
                       famine. */
                    const auto values = Containers::arrayCast<Quaternion>(outputData);
                    if(configuration().value<bool>("optimizeQuaternionShortestPath")) {
                        Float flip = 1.0f;
                        for(std::size_t j = 0; j + 1 < values.size(); ++j) {
                            if(Math::dot(values[j], values[j + 1]*flip) < 0) flip = -flip;
                            values[j + 1] *= flip;
                        }
                    }

                    /* Normalize the quaternions if not already. Don't attempt
                       to normalize every time to avoid tiny differences, only
                       when the quaternion looks to be off. Again, not doing
                       this for splines as it would cause things to go
                       haywire. */
                    if(configuration().value<bool>("normalizeQuaternions")) {
                        for(auto& quat: values) if(!quat.isNormalized()) {
                            quat = quat.normalized();
                            hadToRenormalize = true;
                        }
                    }

                    type = AnimationTrackType::Quaternion;
                    track = Animation::TrackView<const Float, const Quaternion>{
                        keys, values, interpolation,
                        animationInterpolatorFor<Quaternion>(interpolation),
                        Animation::Extrapolation::Constant};
                }

            /* Scale */
            } else if(channel.target_path == cgltf_animation_path_type_scale) {
                if(output->type != cgltf_type_vec3 || output->component_type != cgltf_component_type_r_32f || output->normalized) {
                    Error{} << "Trade::CgltfImporter::animation(): scaling track has unexpected type"
                        << (output->normalized ? "normalized " : "") << Debug::nospace
                        << gltfTypeName(output->type) << "/" << gltfComponentTypeName(output->component_type);
                    return {};
                }

                /* View on the value data */
                target = AnimationTrackTargetType::Scaling3D;
                resultType = AnimationTrackType::Vector3;
                if(interpolation == Animation::Interpolation::Spline) {
                    /* Postprocess the spline track. This can be done only once
                       for every track -- postprocessSplineTrack() checks
                       that. */
                    const auto values = Containers::arrayCast<CubicHermite3D>(outputData);
                    postprocessSplineTrack(timeTrackUsed, keys, values);

                    type = AnimationTrackType::CubicHermite3D;
                    track = Animation::TrackView<const Float, const CubicHermite3D>{
                        keys, values, interpolation,
                        animationInterpolatorFor<CubicHermite3D>(interpolation),
                        Animation::Extrapolation::Constant};
                } else {
                    type = AnimationTrackType::Vector3;
                    track = Animation::TrackView<const Float, const Vector3>{keys,
                        Containers::arrayCast<Vector3>(outputData),
                        interpolation,
                        animationInterpolatorFor<Vector3>(interpolation),
                        Animation::Extrapolation::Constant};
                }

            } else {
                Error{} << "Trade::CgltfImporter::animation(): unsupported track target" << channel.target_path;
                return {};
            }

            /* Splines were postprocessed using the corresponding time track.
               If a spline is not yet marked as postprocessed, mark it.
               Otherwise check that the spline track is always used with the
               same time track. */
            if(interpolation == Animation::Interpolation::Spline) {
                if(timeTrackUsed == nullptr)
                    timeTrackUsed = sampler.input;
                else if(timeTrackUsed != sampler.input) {
                    Error{} << "Trade::CgltfImporter::animation(): spline track is shared with different time tracks, we don't support that, sorry";
                    return {};
                }
            }

            tracks[trackId++] = AnimationTrackData{type, resultType, target,
                UnsignedInt(channel.target_node - _d->data->nodes), track};
        }
    }

    if(hadToRenormalize)
        Warning{} << "Trade::CgltfImporter::animation(): quaternions in some rotation tracks were renormalized";

    return AnimationData{std::move(data), std::move(tracks)};
}

UnsignedInt CgltfImporter::doCameraCount() const {
    return _d->data->cameras_count;
}

Int CgltfImporter::doCameraForName(const Containers::StringView name) {
    if(!_d->camerasForName) {
        _d->camerasForName.emplace();
        _d->camerasForName->reserve(_d->data->cameras_count);
        for(std::size_t i = 0; i != _d->data->cameras_count; ++i)
            _d->camerasForName->emplace(_d->decodeCachedString(_d->data->cameras[i].name), i);
    }

    const auto found = _d->camerasForName->find(name);
    return found == _d->camerasForName->end() ? -1 : found->second;
}

Containers::String CgltfImporter::doCameraName(const UnsignedInt id) {
    return _d->decodeCachedString(_d->data->cameras[id].name);
}

Containers::Optional<CameraData> CgltfImporter::doCamera(UnsignedInt id) {
    const cgltf_camera& camera = _d->data->cameras[id];

    /* https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#projection-matrices */

    /* Perspective camera. glTF uses vertical FoV and X/Y aspect ratio, so to
       avoid accidental bugs we will directly calculate the near plane size and
       use that to create the camera data (instead of passing it the horizontal
       FoV). */
    /** @todo What if znear == 0? aspect_ratio == 0? cgltf exposes
        has_aspect_ratio */
    if(camera.type == cgltf_camera_type_perspective) {
        const cgltf_camera_perspective& data = camera.data.perspective;
        const Vector2 size = 2.0f*data.znear*Math::tan(data.yfov*0.5_radf)*Vector2::xScale(data.aspect_ratio);
        const Float far = data.has_zfar ? data.zfar : Constants::inf();
        return CameraData{CameraType::Perspective3D, size, data.znear, far};
    }

    /* Orthographic camera. glTF uses a "scale" instead of "size", which means
       we have to double. */
    if(camera.type == cgltf_camera_type_orthographic) {
        const cgltf_camera_orthographic& data = camera.data.orthographic;
        return CameraData{CameraType::Orthographic3D,
            Vector2{data.xmag, data.ymag}*2.0f, data.znear, data.zfar};
    }

    CORRADE_INTERNAL_ASSERT(camera.type == cgltf_camera_type_invalid);
    Error{} << "Trade::CgltfImporter::camera(): invalid camera type";
    return {};
}

UnsignedInt CgltfImporter::doLightCount() const {
    return _d->data->lights_count;
}

Int CgltfImporter::doLightForName(const Containers::StringView name) {
    if(!_d->lightsForName) {
        _d->lightsForName.emplace();
        _d->lightsForName->reserve(_d->data->lights_count);
        for(std::size_t i = 0; i != _d->data->lights_count; ++i)
            _d->lightsForName->emplace(_d->decodeCachedString(_d->data->lights[i].name), i);
    }

    const auto found = _d->lightsForName->find(name);
    return found == _d->lightsForName->end() ? -1 : found->second;
}

Containers::String CgltfImporter::doLightName(const UnsignedInt id) {
    return _d->decodeCachedString(_d->data->lights[id].name);
}

Containers::Optional<LightData> CgltfImporter::doLight(UnsignedInt id) {
    const cgltf_light& light = _d->data->lights[id];

    /* https://github.com/KhronosGroup/glTF/tree/5d3dfa44e750f57995ac6821117d9c7061bba1c9/extensions/2.0/Khronos/KHR_lights_punctual */

    /* Light type */
    LightData::Type type;
    if(light.type == cgltf_light_type_point) {
        type = LightData::Type::Point;
    } else if(light.type == cgltf_light_type_spot) {
        type = LightData::Type::Spot;
    } else if(light.type == cgltf_light_type_directional) {
        type = LightData::Type::Directional;
    } else {
        CORRADE_INTERNAL_ASSERT(light.type == cgltf_light_type_invalid);
        Error{} << "Trade::CgltfImporter::light(): invalid light type";
        return {};
    }

    /* Cgltf sets range to 0 instead of infinity when it's not present.
       That's stupid because it would divide by zero, fix that. Even more
       stupid is JSON not having ANY way to represent an infinity, FFS. */
    const Float range = light.range == 0.0f ? Constants::inf() : light.range;

    /* Spotlight cone angles. In glTF they're specified as half-angles (which
       is also why the limit on outer angle is 90°, not 180°), to avoid
       confusion report a potential error in the original half-angles and
       double the angle only at the end. */
    Rad innerConeAngle{NoInit}, outerConeAngle{NoInit};
    if(type == LightData::Type::Spot) {
        innerConeAngle = Rad{light.spot_inner_cone_angle};
        outerConeAngle = Rad{light.spot_outer_cone_angle};

        if(innerConeAngle < Rad(0.0_degf) || innerConeAngle >= outerConeAngle || outerConeAngle >= Rad(90.0_degf)) {
            Error{} << "Trade::CgltfImporter::light(): inner and outer cone angle" << Deg(innerConeAngle) << "and" << Deg(outerConeAngle) << "out of allowed bounds";
            return {};
        }
    } else innerConeAngle = outerConeAngle = 180.0_degf;

    /* Range should be infinity for directional lights. Because there's no way
       to represent infinity in JSON, directly suggest to remove the range
       property, don't even bother printing the value. */
    if(type == LightData::Type::Directional && range != Constants::inf()) {
        Error{} << "Trade::CgltfImporter::light(): range can't be defined for a directional light";
        return {};
    }

    /* As said above, glTF uses half-angles, while we have full angles (for
       consistency with existing APIs such as OpenAL cone angles or math intersection routines as well as Blender). */
    return LightData{type, Color3::from(light.color), light.intensity, range, innerConeAngle*2.0f, outerConeAngle*2.0f};
}

Int CgltfImporter::doDefaultScene() const {
    /* While https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#scenes
       says that "When scene is undefined, client implementations MAY delay
       rendering until a particular scene is requested.", several official
       sample glTF models (e.g. the AnimatedTriangle) have no "scene" property,
       so that's a bit stupid behavior to have. As per discussion at
       https://github.com/KhronosGroup/glTF/issues/815#issuecomment-274286889,
       if a default scene isn't defined and there is at least one scene, just
       use the first one. */
    if(!_d->data->scene)
        return _d->data->scenes_count > 0 ? 0 : -1;

    const Int sceneId = _d->data->scene - _d->data->scenes;
    return sceneId;
}

UnsignedInt CgltfImporter::doSceneCount() const {
    return _d->data->scenes_count;
}

Int CgltfImporter::doSceneForName(const Containers::StringView name) {
    if(!_d->scenesForName) {
        _d->scenesForName.emplace();
        _d->scenesForName->reserve(_d->data->scenes_count);
        for(std::size_t i = 0; i != _d->data->scenes_count; ++i)
            _d->scenesForName->emplace(_d->decodeCachedString(_d->data->scenes[i].name), i);
    }

    const auto found = _d->scenesForName->find(name);
    return found == _d->scenesForName->end() ? -1 : found->second;
}

Containers::String CgltfImporter::doSceneName(const UnsignedInt id) {
    return _d->decodeCachedString(_d->data->scenes[id].name);
}

Containers::Optional<SceneData> CgltfImporter::doScene(UnsignedInt id) {
    const cgltf_scene& scene = _d->data->scenes[id];

    /* Cgltf checks all node / mesh / light ... references during initial file
       parsing (and unfortunately produces a rather unhelpful generic message)
       so this code doesn't have to do any range checks anymore. */

    /* Gather all top-level nodes belonging to a scene and recursively populate
       the children ranges. Optimistically assume the glTF has just a single
       scene and reserve for that. */
    /** @todo once we have BitArrays use the objects array to mark nodes that
        are present in the scene and then create a new array from those but
        ordered so we can have OrderedMapping for parents and also all other
        fields */
    Containers::Array<UnsignedInt> objects;
    arrayReserve(objects, _d->data->nodes_count);
    for(UnsignedInt i = 0; i != scene.nodes_count; ++i) {
        arrayAppend(objects, scene.nodes[i] - _d->data->nodes);
    }

    Containers::Array<Range1Dui> children;
    arrayReserve(children, _d->data->nodes_count + 1);
    arrayAppend(children, InPlaceInit, 0u, UnsignedInt(objects.size()));
    for(std::size_t i = 0; i != children.size(); ++i) {
        const Range1Dui& nodeRangeToProcess = children[i];
        for(std::size_t j = nodeRangeToProcess.min(); j != nodeRangeToProcess.max(); ++j) {
            const cgltf_node& node = _d->data->nodes[objects[j]];
            arrayAppend(children, InPlaceInit, UnsignedInt(objects.size()), UnsignedInt(objects.size() + node.children_count));
            for(std::size_t k = 0; k != node.children_count; ++k)
                arrayAppend(objects, UnsignedInt(node.children[k] - _d->data->nodes));
        }
    }

    /** @todo once there's SceneData::mappingRange(), calculate also min here */
    const UnsignedInt maxObjectIndexPlusOne = objects.isEmpty() ? 0 : Math::max(objects) + 1;

    /* Count how many objects have matrices, how many have separate TRS
       properties and which of the set are present. Then also gather mesh,
       light, camera and skin assignment count. Materials have to use the same
       object mapping as meshes, so only check if there's any material
       assignment at all -- if not, then we won't need to store that field. */
    UnsignedInt transformationCount = 0;
    UnsignedInt trsCount = 0;
    bool hasTranslations = false;
    bool hasRotations = false;
    bool hasScalings = false;
    UnsignedInt meshCount = 0;
    bool hasMeshMaterials = false;
    UnsignedInt lightCount = 0;
    UnsignedInt cameraCount = 0;
    UnsignedInt skinCount = 0;
    for(const UnsignedInt i: objects) {
        const cgltf_node& node = _d->data->nodes[i];

        /* Everything that has a TRS should have a transformation matrix as
           well. OTOH there can be a transformation matrix but no TRS, and
           there can also be objects without any transformation. */
        if(node.has_translation ||
           node.has_rotation ||
           node.has_scale) {
            ++trsCount;
            ++transformationCount;
        } else if(node.has_matrix) ++transformationCount;

        if(node.has_translation) hasTranslations = true;
        if(node.has_rotation) hasRotations = true;
        if(node.has_scale) hasScalings = true;
        if(node.mesh) {
            meshCount += node.mesh->primitives_count;
            for(std::size_t j = 0; j != node.mesh->primitives_count; ++j) {
                if(node.mesh->primitives[j].material) {
                    hasMeshMaterials = true;
                    break;
                }
            }
        }
        if(node.camera) ++cameraCount;
        if(node.skin) ++skinCount;
        if(node.light) ++lightCount;
    }

    /* If all objects that have transformations have TRS as well, no need to
       store the combined transform field */
    if(trsCount == transformationCount) transformationCount = 0;

    /* Allocate the output array */
    Containers::ArrayView<UnsignedInt> parentObjects;
    Containers::ArrayView<Int> parents;
    Containers::ArrayView<UnsignedInt> transformationObjects;
    Containers::ArrayView<Matrix4> transformations;
    Containers::ArrayView<UnsignedInt> trsObjects;
    Containers::ArrayView<Vector3> translations;
    Containers::ArrayView<Quaternion> rotations;
    Containers::ArrayView<Vector3> scalings;
    Containers::ArrayView<UnsignedInt> meshMaterialObjects;
    Containers::ArrayView<UnsignedInt> meshes;
    Containers::ArrayView<Int> meshMaterials;
    Containers::ArrayView<UnsignedInt> lightObjects;
    Containers::ArrayView<UnsignedInt> lights;
    Containers::ArrayView<UnsignedInt> cameraObjects;
    Containers::ArrayView<UnsignedInt> cameras;
    Containers::ArrayView<UnsignedInt> skinObjects;
    Containers::ArrayView<UnsignedInt> skins;
    Containers::Array<char> data = Containers::ArrayTuple{
        {NoInit, objects.size(), parentObjects},
        {NoInit, objects.size(), parents},
        {NoInit, transformationCount, transformationObjects},
        {NoInit, transformationCount, transformations},
        {NoInit, trsCount, trsObjects},
        {NoInit, hasTranslations ? trsCount : 0, translations},
        {NoInit, hasRotations ? trsCount : 0, rotations},
        {NoInit, hasScalings ? trsCount : 0, scalings},
        {NoInit, meshCount, meshMaterialObjects},
        {NoInit, meshCount, meshes},
        {NoInit, hasMeshMaterials ? meshCount : 0, meshMaterials},
        {NoInit, lightCount, lightObjects},
        {NoInit, lightCount, lights},
        {NoInit, cameraCount, cameraObjects},
        {NoInit, cameraCount, cameras},
        {NoInit, skinCount, skinObjects},
        {NoInit, skinCount, skins}
    };

    /* Populate object mapping for parents and importer state, synthesize
       parent info from the child ranges */
    Utility::copy(objects, parentObjects);
    for(std::size_t i = 0; i != children.size(); ++i) {
        Int parent = Int(i) - 1;
        for(std::size_t j = children[i].min(); j != children[i].max(); ++j)
            parents[j] = parent == -1 ? -1 : objects[parent];
    }

    /* Populate the rest */
    std::size_t transformationOffset = 0;
    std::size_t trsOffset = 0;
    std::size_t meshMaterialOffset = 0;
    std::size_t lightOffset = 0;
    std::size_t cameraOffset = 0;
    std::size_t skinOffset = 0;
    for(const UnsignedInt i: objects) {
        const cgltf_node& node = _d->data->nodes[i];

        /* Parse TRS */
        Vector3 translation;
        if(node.has_translation) translation = Vector3::from(node.translation);

        Quaternion rotation;
        if(node.has_rotation) {
            rotation = Quaternion{Vector3::from(node.rotation), node.rotation[3]};
            if(!rotation.isNormalized() && configuration().value<bool>("normalizeQuaternions")) {
                rotation = rotation.normalized();
                Warning{} << "Trade::CgltfImporter::scene(): rotation quaternion of node" << i << "was renormalized";
            }
        }

        Vector3 scaling{1.0f};
        if(node.has_scale) scaling = Vector3::from(node.scale);

        /* Parse transformation, or combine it from TRS if not present */
        Matrix4 transformation;
        if(node.has_matrix) transformation = Matrix4::from(node.matrix);
        else transformation =
            Matrix4::translation(translation)*
            Matrix4{rotation.toMatrix()}*
            Matrix4::scaling(scaling);

        /* Populate the combined transformation and object mapping only if
           there's actually some transformation for this object and we want to
           store it -- if all objects have TRS anyway, the matrix is redundant */
        if((node.has_matrix ||
            node.has_translation ||
            node.has_rotation ||
            node.has_scale) && transformationCount)
        {
            transformations[transformationOffset] = transformation;
            transformationObjects[transformationOffset] = i;
            ++transformationOffset;
        }

        /* Store the TRS information and object mapping only if there was
           something */
        if(node.has_translation ||
           node.has_rotation ||
           node.has_scale)
        {
            if(hasTranslations) translations[trsOffset] = translation;
            if(hasRotations) rotations[trsOffset] = rotation;
            if(hasScalings) scalings[trsOffset] = scaling;
            trsObjects[trsOffset] = i;
            ++trsOffset;
        }

        /* Populate mesh references */
        if(node.mesh) {
            for(std::size_t j = 0; j != node.mesh->primitives_count; ++j) {
                meshMaterialObjects[meshMaterialOffset] = i;
                meshes[meshMaterialOffset] = _d->meshSizeOffsets[node.mesh - _d->data->meshes] + j;
                if(hasMeshMaterials) {
                    const cgltf_material* material = node.mesh->primitives[j].material;
                    meshMaterials[meshMaterialOffset] = material ? material - _d->data->materials : -1;
                }
                ++meshMaterialOffset;
            }
        }

        /* Populate light references */
        if(node.light) {
            lightObjects[lightOffset] = i;
            lights[lightOffset] = node.light - _d->data->lights;
            ++lightOffset;
        }

        /* Populate camera references */
        if(node.camera) {
            cameraObjects[cameraOffset] = i;
            cameras[cameraOffset] = node.camera - _d->data->cameras;
            ++cameraOffset;
        }

        /* Populate skin references */
        if(node.skin) {
            skinObjects[skinOffset] = i;
            skins[skinOffset] = node.skin - _d->data->skins;
            ++skinOffset;
        }
    }

    CORRADE_INTERNAL_ASSERT(
        transformationOffset == transformations.size() &&
        trsOffset == trsObjects.size() &&
        meshMaterialOffset == meshMaterialObjects.size() &&
        lightOffset == lightObjects.size() &&
        cameraOffset == cameraObjects.size() &&
        skinOffset == skinObjects.size());

    /* Put everything together. For simplicity the imported data could always
       have all fields present, with some being empty, but this gives less
       noise for asset introspection purposes. */
    Containers::Array<SceneFieldData> fields;
    arrayAppend(fields, {
        /** @todo once there's a flag to annotate implicit fields, omit the
            parent field if it's all -1s; or alternatively we could also have a
            stride of 0 for this case */
        SceneFieldData{SceneField::Parent, parentObjects, parents}
    });

    /* Transformations. If there's no such field, add an empty transformation
       to indicate it's a 3D scene. */
    if(transformationCount) arrayAppend(fields, SceneFieldData{
        SceneField::Transformation, transformationObjects, transformations
    });
    if(hasTranslations) arrayAppend(fields, SceneFieldData{
        SceneField::Translation, trsObjects, translations
    });
    if(hasRotations) arrayAppend(fields, SceneFieldData{
        SceneField::Rotation, trsObjects, rotations
    });
    if(hasScalings) arrayAppend(fields, SceneFieldData{
        SceneField::Scaling, trsObjects, scalings
    });
    if(!transformationCount && !trsCount) arrayAppend(fields, SceneFieldData{
        SceneField::Transformation, SceneMappingType::UnsignedInt, nullptr, SceneFieldType::Matrix4x4, nullptr
    });

    if(meshCount) arrayAppend(fields, SceneFieldData{
        SceneField::Mesh, meshMaterialObjects, meshes
    });
    if(hasMeshMaterials) arrayAppend(fields, SceneFieldData{
        SceneField::MeshMaterial, meshMaterialObjects, meshMaterials
    });
    if(lightCount) arrayAppend(fields, SceneFieldData{
        SceneField::Light, lightObjects, lights
    });
    if(cameraCount) arrayAppend(fields, SceneFieldData{
        SceneField::Camera, cameraObjects, cameras
    });
    if(skinCount) arrayAppend(fields, SceneFieldData{
        SceneField::Skin, skinObjects, skins
    });

    /* Convert back to the default deleter to avoid dangling deleter function
       pointer issues when unloading the plugin */
    arrayShrink(fields, DefaultInit);
    return SceneData{SceneMappingType::UnsignedInt, maxObjectIndexPlusOne, std::move(data), std::move(fields)};
}

UnsignedLong CgltfImporter::doObjectCount() const {
    return _d->data->nodes_count;
}

Long CgltfImporter::doObjectForName(const Containers::StringView name) {
    if(!_d->nodesForName) {
        _d->nodesForName.emplace();
        _d->nodesForName->reserve(_d->data->nodes_count);
        for(std::size_t i = 0; i != _d->data->nodes_count; ++i) {
            _d->nodesForName->emplace(_d->decodeCachedString(_d->data->nodes[i].name), i);
        }
    }

    const auto found = _d->nodesForName->find(name);
    return found == _d->nodesForName->end() ? -1 : found->second;
}

Containers::String CgltfImporter::doObjectName(UnsignedLong id) {
    return _d->decodeCachedString(_d->data->nodes[id].name);
}

UnsignedInt CgltfImporter::doSkin3DCount() const {
    return _d->data->skins_count;
}

Int CgltfImporter::doSkin3DForName(const Containers::StringView name) {
    if(!_d->skinsForName) {
        _d->skinsForName.emplace();
        _d->skinsForName->reserve(_d->data->skins_count);
        for(std::size_t i = 0; i != _d->data->skins_count; ++i)
            _d->skinsForName->emplace(_d->decodeCachedString(_d->data->skins[i].name), i);
    }

    const auto found = _d->skinsForName->find(name);
    return found == _d->skinsForName->end() ? -1 : found->second;
}

Containers::String CgltfImporter::doSkin3DName(const UnsignedInt id) {
    return _d->decodeCachedString(_d->data->skins[id].name);
}

Containers::Optional<SkinData3D> CgltfImporter::doSkin3D(const UnsignedInt id) {
    const cgltf_skin& skin = _d->data->skins[id];

    if(!skin.joints_count) {
        Error{} << "Trade::CgltfImporter::skin3D(): skin has no joints";
        return {};
    }

    /* Joint IDs */
    Containers::Array<UnsignedInt> joints{NoInit, skin.joints_count};
    for(std::size_t i = 0; i != joints.size(); ++i) {
        const UnsignedInt nodeId = skin.joints[i] - _d->data->nodes;
        joints[i] = nodeId;
    }

    /* Inverse bind matrices. If there are none, default is identities */
    Containers::Array<Matrix4> inverseBindMatrices{skin.joints_count};
    if(skin.inverse_bind_matrices) {
        const cgltf_accessor* accessor = skin.inverse_bind_matrices;
        Containers::Optional<Containers::StridedArrayView2D<const char>> view = accessorView("Trade::CgltfImporter::skin3D():", accessor);
        if(!view)
            return {};

        if(accessor->type != cgltf_type_mat4 || accessor->component_type != cgltf_component_type_r_32f || accessor->normalized) {
            Error{} << "Trade::CgltfImporter::skin3D(): inverse bind matrices have unexpected type"
                << (accessor->normalized ? "normalized " : "") << Debug::nospace
                << gltfTypeName(accessor->type) << "/" << gltfComponentTypeName(accessor->component_type);
            return {};
        }

        Containers::StridedArrayView1D<const Matrix4> matrices = Containers::arrayCast<1, const Matrix4>(*view);
        if(matrices.size() != inverseBindMatrices.size()) {
            Error{} << "Trade::CgltfImporter::skin3D(): invalid inverse bind matrix count, expected" << inverseBindMatrices.size() << "but got" << matrices.size();
            return {};
        }

        Utility::copy(matrices, inverseBindMatrices);
    }

    return SkinData3D{std::move(joints), std::move(inverseBindMatrices)};
}

UnsignedInt CgltfImporter::doMeshCount() const {
    return _d->meshMap.size();
}

Int CgltfImporter::doMeshForName(const Containers::StringView name) {
    if(!_d->meshesForName) {
        _d->meshesForName.emplace();
        _d->meshesForName->reserve(_d->data->meshes_count);
        for(std::size_t i = 0; i != _d->data->meshes_count; ++i) {
            /* The mesh can be duplicated for as many primitives as it has,
               point to the first mesh in the duplicate sequence */
            _d->meshesForName->emplace(_d->decodeCachedString(_d->data->meshes[i].name), _d->meshSizeOffsets[i]);
        }
    }

    const auto found = _d->meshesForName->find(name);
    return found == _d->meshesForName->end() ? -1 : found->second;
}

Containers::String CgltfImporter::doMeshName(const UnsignedInt id) {
    /* This returns the same name for all multi-primitive mesh duplicates */
    return _d->decodeCachedString(_d->data->meshes[_d->meshMap[id].first()].name);
}

Containers::Optional<MeshData> CgltfImporter::doMesh(const UnsignedInt id, UnsignedInt) {
    const cgltf_mesh& mesh = _d->data->meshes[_d->meshMap[id].first()];
    const cgltf_primitive& primitive = mesh.primitives[_d->meshMap[id].second()];

    MeshPrimitive meshPrimitive{};
    if(primitive.type == cgltf_primitive_type_points) {
        meshPrimitive = MeshPrimitive::Points;
    } else if(primitive.type == cgltf_primitive_type_lines) {
        meshPrimitive = MeshPrimitive::Lines;
    } else if(primitive.type == cgltf_primitive_type_line_loop) {
        meshPrimitive = MeshPrimitive::LineLoop;
    } else if(primitive.type == cgltf_primitive_type_line_strip) {
        meshPrimitive = MeshPrimitive::LineStrip;
    } else if(primitive.type == cgltf_primitive_type_triangles) {
        meshPrimitive = MeshPrimitive::Triangles;
    } else if(primitive.type == cgltf_primitive_type_triangle_fan) {
        meshPrimitive = MeshPrimitive::TriangleFan;
    } else if(primitive.type == cgltf_primitive_type_triangle_strip) {
        meshPrimitive = MeshPrimitive::TriangleStrip;
    } else {
        /* Cgltf parses an int and directly casts it to cgltf_primitive_type
           without checking for valid values */
        Error{} << "Trade::CgltfImporter::mesh(): unrecognized primitive" << primitive.type;
        return {};
    }

    /* Sort attributes by name so that we add attribute sets in the correct
       order and can warn if indices are not contiguous. Stable sort is needed
       to preserve declaration order for duplicate attributes, checked below. */
    Containers::Array<UnsignedInt> attributeOrder{primitive.attributes_count};
    for(UnsignedInt i = 0; i < attributeOrder.size(); ++i)
        attributeOrder[i] = i;

    std::stable_sort(attributeOrder.begin(), attributeOrder.end(), [&](UnsignedInt a, UnsignedInt b) {
        return std::strcmp(primitive.attributes[a].name, primitive.attributes[b].name) < 0;
    });

    /* Find and remove duplicate attributes. This mimics tinygltf behaviour
       which replaces the previous attribute of the same name. */
    std::size_t attributeCount = attributeOrder.size();
    for(UnsignedInt i = 0; i + 1 < attributeOrder.size(); ++i) {
        const cgltf_attribute& current = primitive.attributes[attributeOrder[i]];
        const cgltf_attribute& next = primitive.attributes[attributeOrder[i + 1]];
        if(std::strcmp(current.name, next.name) == 0) {
            --attributeCount;
            /* Mark for skipping later */
            attributeOrder[i] = ~0u;
        }
    }

    /* Gather all (whitelisted) attributes and the total buffer range spanning
       them */
    cgltf_buffer* buffer = nullptr;
    UnsignedInt vertexCount = 0;
    std::size_t attributeId = 0;
    cgltf_attribute lastAttribute{};
    Math::Range1D<std::size_t> bufferRange;
    Containers::Array<MeshAttributeData> attributeData{attributeCount};
    for(UnsignedInt a: attributeOrder) {
        /* Duplicate attribute, skip */
        if(a == ~0u)
            continue;

        const cgltf_attribute& attribute = primitive.attributes[a];

        const Containers::StringView nameString{attribute.name};
        /* See the comment in doOpenData() for why we do this */
        const Containers::StringView semantic = attribute.type != cgltf_attribute_type_invalid ?
            nameString.partition('_')[0] : nameString;

        /* Numbered attributes are expected to be contiguous (COLORS_0,
           COLORS_1...). If not, print a warning, because in the MeshData they
           will appear as contiguous. */
        if(attribute.type != cgltf_attribute_type_invalid) {
            if(attribute.type != lastAttribute.type)
                lastAttribute.index = -1;

            if(attribute.index != lastAttribute.index + 1)
                Warning{} << "Trade::CgltfImporter::mesh(): found attribute" << nameString << "but expected" << semantic << Debug::nospace << "_" << Debug::nospace << lastAttribute.index + 1;
        }
        lastAttribute = attribute;

        const cgltf_accessor* accessor = attribute.data;
        if(!checkAccessor("Trade::CgltfImporter::mesh():", accessor))
            return {};

        /* Convert to our vertex format */
        VertexFormat componentFormat;
        if(accessor->component_type == cgltf_component_type_r_8)
            componentFormat = VertexFormat::Byte;
        else if(accessor->component_type == cgltf_component_type_r_8u)
            componentFormat = VertexFormat::UnsignedByte;
        else if(accessor->component_type == cgltf_component_type_r_16)
            componentFormat = VertexFormat::Short;
        else if(accessor->component_type == cgltf_component_type_r_16u)
            componentFormat = VertexFormat::UnsignedShort;
        else if(accessor->component_type == cgltf_component_type_r_32u)
            componentFormat = VertexFormat::UnsignedInt;
        else if(accessor->component_type == cgltf_component_type_r_32f)
            componentFormat = VertexFormat::Float;
        else {
            CORRADE_INTERNAL_ASSERT(accessor->component_type == cgltf_component_type_invalid);
            Error{} << "Trade::CgltfImporter::mesh(): attribute" << nameString << "has an invalid component type";
            return {};
        }

        UnsignedInt componentCount;
        UnsignedInt vectorCount = 0;
        if(accessor->type == cgltf_type_scalar)
            componentCount = 1;
        else if(accessor->type == cgltf_type_vec2)
            componentCount = 2;
        else if(accessor->type == cgltf_type_vec3)
            componentCount = 3;
        else if(accessor->type == cgltf_type_vec4)
            componentCount = 4;
        else if(accessor->type == cgltf_type_mat2) {
            componentCount = 2;
            vectorCount = 2;
        } else if(accessor->type == cgltf_type_mat3) {
            componentCount = 3;
            vectorCount = 3;
        } else if(accessor->type == cgltf_type_mat4) {
            componentCount = 4;
            vectorCount = 4;
        } else {
            CORRADE_INTERNAL_ASSERT(accessor->type == cgltf_type_invalid);
            Error{} << "Trade::CgltfImporter::mesh(): attribute" << nameString << "has an invalid type";
            return {};
        }

        /* Check for illegal normalized types */
        if(accessor->normalized &&
            (componentFormat == VertexFormat::Float || componentFormat == VertexFormat::UnsignedInt)) {
            Error{} << "Trade::CgltfImporter::mesh(): attribute" << nameString << "component type" << gltfComponentTypeName(accessor->component_type) << "can't be normalized";
            return {};
        }

        /* Check that matrix type is legal */
        if(vectorCount &&
            componentFormat != VertexFormat::Float &&
            !(componentFormat == VertexFormat::Byte && accessor->normalized) &&
            !(componentFormat == VertexFormat::Short && accessor->normalized)) {
            Error{} << "Trade::CgltfImporter::mesh(): attribute" << nameString << "has an unsupported matrix component type"
                << (accessor->normalized ? "normalized" : "unnormalized")
                << gltfComponentTypeName(accessor->component_type);
            return {};
        }

        const VertexFormat format = vectorCount ?
            vertexFormat(componentFormat, vectorCount, componentCount, true) :
            vertexFormat(componentFormat, componentCount, accessor->normalized);

        /* Whitelist supported attribute and data type combinations */
        MeshAttribute name;
        if(attribute.type == cgltf_attribute_type_position) {
            name = MeshAttribute::Position;

            if(accessor->type != cgltf_type_vec3) {
                Error{} << "Trade::CgltfImporter::mesh(): unexpected" << semantic << "type" << gltfTypeName(accessor->type);
                return {};
            }

            if(!(componentFormat == VertexFormat::Float && !accessor->normalized) &&
               /* KHR_mesh_quantization. Both normalized and unnormalized
                  bytes/shorts are okay. */
               componentFormat != VertexFormat::UnsignedByte &&
               componentFormat != VertexFormat::Byte &&
               componentFormat != VertexFormat::UnsignedShort &&
               componentFormat != VertexFormat::Short) {
                Error{} << "Trade::CgltfImporter::mesh(): unsupported" << semantic << "component type"
                    << (accessor->normalized ? "normalized" : "unnormalized")
                    << gltfComponentTypeName(accessor->component_type);
                return {};
            }

        } else if(attribute.type == cgltf_attribute_type_normal) {
            name = MeshAttribute::Normal;

            if(accessor->type != cgltf_type_vec3) {
                Error{} << "Trade::CgltfImporter::mesh(): unexpected" << semantic << "type" << gltfTypeName(accessor->type);
                return {};
            }

            if(!(componentFormat == VertexFormat::Float && !accessor->normalized) &&
               /* KHR_mesh_quantization */
               !(componentFormat == VertexFormat::Byte && accessor->normalized) &&
               !(componentFormat == VertexFormat::Short && accessor->normalized)) {
                Error{} << "Trade::CgltfImporter::mesh(): unsupported" << semantic << "component type"
                    << (accessor->normalized ? "normalized" : "unnormalized")
                    << gltfComponentTypeName(accessor->component_type);
                return {};
            }

        } else if(attribute.type == cgltf_attribute_type_tangent) {
            name = MeshAttribute::Tangent;

            if(accessor->type != cgltf_type_vec4) {
                Error{} << "Trade::CgltfImporter::mesh(): unexpected" << semantic << "type" << gltfTypeName(accessor->type);
                return {};
            }

            if(!(componentFormat == VertexFormat::Float && !accessor->normalized) &&
               /* KHR_mesh_quantization */
               !(componentFormat == VertexFormat::Byte && accessor->normalized) &&
               !(componentFormat == VertexFormat::Short && accessor->normalized)) {
                Error{} << "Trade::CgltfImporter::mesh(): unsupported" << semantic << "component type"
                    << (accessor->normalized ? "normalized" : "unnormalized")
                    << gltfComponentTypeName(accessor->component_type);
                return {};
            }

        } else if(attribute.type == cgltf_attribute_type_texcoord) {
            name = MeshAttribute::TextureCoordinates;

            if(accessor->type != cgltf_type_vec2) {
                Error{} << "Trade::CgltfImporter::mesh(): unexpected" << semantic << "type" << gltfTypeName(accessor->type);
                return {};
            }

            /* Core spec only allows float and normalized unsigned bytes/shorts, the
               rest is added by KHR_mesh_quantization */
            if(!(componentFormat == VertexFormat::Float && !accessor->normalized) &&
               componentFormat != VertexFormat::UnsignedByte &&
               componentFormat != VertexFormat::Byte &&
               componentFormat != VertexFormat::UnsignedShort &&
               componentFormat != VertexFormat::Short) {
                Error{} << "Trade::CgltfImporter::mesh(): unsupported" << semantic << "component type"
                    << (accessor->normalized ? "normalized" : "unnormalized")
                    << gltfComponentTypeName(accessor->component_type);
                return {};
            }

        } else if(attribute.type == cgltf_attribute_type_color) {
            name = MeshAttribute::Color;

            if(accessor->type != cgltf_type_vec4 && accessor->type != cgltf_type_vec3) {
                Error{} << "Trade::CgltfImporter::mesh(): unexpected" << semantic << "type" << gltfTypeName(accessor->type);
                return {};
            }

            if(!(componentFormat == VertexFormat::Float && !accessor->normalized) &&
               !(componentFormat == VertexFormat::UnsignedByte && accessor->normalized) &&
               !(componentFormat == VertexFormat::UnsignedShort && accessor->normalized)) {
                Error{} << "Trade::CgltfImporter::mesh(): unsupported" << semantic << "component type"
                    << (accessor->normalized ? "normalized" : "unnormalized")
                    << gltfComponentTypeName(accessor->component_type);
                return {};
            }
        } else if(attribute.type == cgltf_attribute_type_joints) {
            name = _d->meshAttributesForName.at(semantic);

            if(accessor->type != cgltf_type_vec4) {
                Error{} << "Trade::CgltfImporter::mesh(): unexpected" << semantic << "type" << gltfTypeName(accessor->type);
                return {};
            }

            if(!(componentFormat == VertexFormat::UnsignedByte && !accessor->normalized) &&
               !(componentFormat == VertexFormat::UnsignedShort && !accessor->normalized)) {
                Error{} << "Trade::CgltfImporter::mesh(): unsupported" << semantic << "component type"
                    << (accessor->normalized ? "normalized" : "unnormalized")
                    << gltfComponentTypeName(accessor->component_type);
                return {};
            }
        } else if(attribute.type == cgltf_attribute_type_weights) {
            name = _d->meshAttributesForName.at(semantic);

            if(accessor->type != cgltf_type_vec4) {
                Error{} << "Trade::CgltfImporter::mesh(): unexpected" << semantic << "type" << gltfTypeName(accessor->type);
                return {};
            }

            if(!(componentFormat == VertexFormat::Float && !accessor->normalized) &&
               !(componentFormat == VertexFormat::UnsignedByte && accessor->normalized) &&
               !(componentFormat == VertexFormat::UnsignedShort && accessor->normalized)) {
                Error{} << "Trade::CgltfImporter::mesh(): unsupported" << semantic << "component type"
                    << (accessor->normalized ? "normalized" : "unnormalized")
                    << gltfComponentTypeName(accessor->component_type);
                return {};
            }
        /* Object ID, name user-configurable */
        } else if(nameString == configuration().value("objectIdAttribute")) {
            name = MeshAttribute::ObjectId;

            if(accessor->type != cgltf_type_scalar) {
                Error{} << "Trade::CgltfImporter::mesh(): unexpected object ID type" << gltfTypeName(accessor->type);
                return {};
            }

            /* The glTF spec says that "Application-specific attribute semantics
               MUST NOT use unsigned int component type" but I'm not sure what
               the point of enforcing that would be */
            if((componentFormat != VertexFormat::UnsignedInt &&
                componentFormat != VertexFormat::UnsignedShort &&
                componentFormat != VertexFormat::UnsignedByte) ||
                accessor->normalized) {
                Error{} << "Trade::CgltfImporter::mesh(): unsupported object ID component type"
                    << (accessor->normalized ? "normalized" : "unnormalized")
                    << gltfComponentTypeName(accessor->component_type);
                return {};
            }

        /* Custom or unrecognized attributes, map to an ID */
        } else {
            CORRADE_INTERNAL_ASSERT(attribute.type == cgltf_attribute_type_invalid);
            name = _d->meshAttributesForName.at(nameString);
        }

        /* Remember which buffer the attribute is in and the range, for
           consecutive attribs expand the range */
        const cgltf_buffer_view* bufferView = accessor->buffer_view;
        if(attributeId == 0) {
            buffer = bufferView->buffer;
            bufferRange = Math::Range1D<std::size_t>::fromSize(bufferView->offset, bufferView->size);
            vertexCount = accessor->count;
        } else {
            /* ... and probably never will be */
            if(bufferView->buffer != buffer) {
                Error{} << "Trade::CgltfImporter::mesh(): meshes spanning multiple buffers are not supported";
                return {};
            }

            bufferRange = Math::join(bufferRange, Math::Range1D<std::size_t>::fromSize(bufferView->offset, bufferView->size));

            if(accessor->count != vertexCount) {
                Error{} << "Trade::CgltfImporter::mesh(): mismatched vertex count for attribute" << semantic << Debug::nospace << ", expected" << vertexCount << "but got" << accessor->count;
                return {};
            }
        }

        /** @todo Check that accessor stride >= vertexFormatSize(format)? */

        /* Fill in an attribute. Offset-only, will be patched to be relative to
           the actual output buffer once we know how large it is and where it
           is allocated. */
        attributeData[attributeId++] = MeshAttributeData{name, format,
            UnsignedInt(accessor->offset + bufferView->offset), vertexCount,
            std::ptrdiff_t(accessor->stride)};
    }

    /* Verify we really filled all attributes */
    CORRADE_INTERNAL_ASSERT(attributeId == attributeData.size());

    /* Allocate & copy vertex data (if any) */
    Containers::Array<char> vertexData{NoInit, bufferRange.size()};
    if(vertexData.size()) {
        const UnsignedInt bufferId = buffer - _d->data->buffers;
        if(!loadBuffer("Trade::CgltfImporter::mesh():", bufferId))
            return {};

        Utility::copy(Containers::arrayView(static_cast<char*>(buffer->data), buffer->size)
            .slice(bufferRange.min(), bufferRange.max()),
        vertexData);
    }

    /* Convert the attributes from relative to absolute, copy them to a
       non-growable array and do additional patching */
    for(std::size_t i = 0; i != attributeData.size(); ++i) {
        Containers::StridedArrayView1D<char> data{vertexData,
            /* Offset is what with the range min subtracted, as we copied
               without the prefix */
            vertexData + attributeData[i].offset(vertexData) - bufferRange.min(),
            vertexCount, attributeData[i].stride()};

        attributeData[i] = MeshAttributeData{attributeData[i].name(),
            attributeData[i].format(), data};

        /* Flip Y axis of texture coordinates, unless it's done in the material
           instead */
        if(attributeData[i].name() == MeshAttribute::TextureCoordinates && !_d->textureCoordinateYFlipInMaterial) {
           if(attributeData[i].format() == VertexFormat::Vector2)
                for(auto& c: Containers::arrayCast<Vector2>(data))
                    c.y() = 1.0f - c.y();
            else if(attributeData[i].format() == VertexFormat::Vector2ubNormalized)
                for(auto& c: Containers::arrayCast<Vector2ub>(data))
                    c.y() = 255 - c.y();
            else if(attributeData[i].format() == VertexFormat::Vector2usNormalized)
                for(auto& c: Containers::arrayCast<Vector2us>(data))
                    c.y() = 65535 - c.y();
            /* For these it's always done in the material texture transform as
               we can't do a 1 - y flip like above. These are allowed only by
               the KHR_mesh_quantization formats and in that case the texture
               transform should be always present. */
            /* LCOV_EXCL_START */
            else if(attributeData[i].format() != VertexFormat::Vector2bNormalized &&
                    attributeData[i].format() != VertexFormat::Vector2sNormalized &&
                    attributeData[i].format() != VertexFormat::Vector2ub &&
                    attributeData[i].format() != VertexFormat::Vector2b &&
                    attributeData[i].format() != VertexFormat::Vector2us &&
                    attributeData[i].format() != VertexFormat::Vector2s)
                CORRADE_INTERNAL_ASSERT_UNREACHABLE();
            /* LCOV_EXCL_STOP */
        }
    }

    /* Indices */
    MeshIndexData indices;
    Containers::Array<char> indexData;
    if(primitive.indices) {
        const cgltf_accessor* accessor = primitive.indices;
        Containers::Optional<Containers::StridedArrayView2D<const char>> src = accessorView("Trade::CgltfImporter::mesh():", accessor);
        if(!src)
            return {};

        if(accessor->type != cgltf_type_scalar) {
            Error() << "Trade::CgltfImporter::mesh(): unexpected index type" << gltfTypeName(accessor->type);
            return {};
        }

        if(accessor->normalized) {
            Error() << "Trade::CgltfImporter::mesh(): index type can't be normalized";
            return {};
        }

        MeshIndexType type;
        if(accessor->component_type == cgltf_component_type_r_8u)
            type = MeshIndexType::UnsignedByte;
        else if(accessor->component_type == cgltf_component_type_r_16u)
            type = MeshIndexType::UnsignedShort;
        else if(accessor->component_type == cgltf_component_type_r_32u)
            type = MeshIndexType::UnsignedInt;
        else {
            Error{} << "Trade::CgltfImporter::mesh(): unexpected index component type" << gltfComponentTypeName(accessor->component_type);
            return {};
        }

        if(!src->isContiguous()) {
            Error{} << "Trade::CgltfImporter::mesh(): index buffer view is not contiguous";
            return {};
        }

        Containers::ArrayView<const char> srcContiguous = src->asContiguous();
        indexData = Containers::Array<char>{srcContiguous.size()};
        Utility::copy(srcContiguous, indexData);
        indices = MeshIndexData{type, indexData};
    }

    /* If we have an index-less attribute-less mesh, glTF has no way to supply
       a vertex count, so return 0 */
    if(!indices.data().size() && !attributeData.size())
        return MeshData{meshPrimitive, 0};

    return MeshData{meshPrimitive,
        std::move(indexData), indices,
        std::move(vertexData), std::move(attributeData),
        vertexCount};
}

MeshAttribute CgltfImporter::doMeshAttributeForName(const Containers::StringView name) {
    return _d ? _d->meshAttributesForName[name] : MeshAttribute{};
}

Containers::String CgltfImporter::doMeshAttributeName(UnsignedShort name) {
    return _d && name < _d->meshAttributeNames.size() ?
        _d->meshAttributeNames[name] : "";
}

UnsignedInt CgltfImporter::doMaterialCount() const {
    return _d->data->materials_count;
}

Int CgltfImporter::doMaterialForName(const Containers::StringView name) {
    if(!_d->materialsForName) {
        _d->materialsForName.emplace();
        _d->materialsForName->reserve(_d->data->materials_count);
        for(std::size_t i = 0; i != _d->data->materials_count; ++i)
            _d->materialsForName->emplace(_d->decodeCachedString(_d->data->materials[i].name), i);
    }

    const auto found = _d->materialsForName->find(name);
    return found == _d->materialsForName->end() ? -1 : found->second;
}

Containers::String CgltfImporter::doMaterialName(const UnsignedInt id) {
    return _d->decodeCachedString(_d->data->materials[id].name);
}

namespace {

/** @todo turn this into a helper API on MaterialAttributeData and then drop
    from here and AssimpImporter */
bool checkMaterialAttributeSize(const Containers::StringView name, const MaterialAttributeType type, const void* const value = nullptr) {
    std::size_t valueSize;
    if(type == MaterialAttributeType::String) {
        CORRADE_INTERNAL_ASSERT(value);
        /* +2 are null byte and size */
        valueSize = static_cast<const Containers::StringView*>(value)->size() + 2;
    } else
        valueSize = materialAttributeTypeSize(type);

    /* +1 is the key null byte */
    if(valueSize + name.size() + 1 + sizeof(MaterialAttributeType) > sizeof(MaterialAttributeData)) {
        Warning{} << "Trade::CgltfImporter::material(): property" << name <<
            "is too large with" << valueSize + name.size() << "bytes, skipping";
        return false;
    }

    return true;
}

Containers::Optional<MaterialAttributeData> parseMaterialAttribute(const Containers::StringView json, const Containers::ArrayView<const jsmntok_t> tokens) {
    std::size_t tokenIndex = 0;

    CORRADE_INTERNAL_ASSERT(tokens[tokenIndex].type == JSMN_STRING);

    Containers::StringView name = tokenString(json, tokens[tokenIndex]);
    if(name.isEmpty()) {
        Warning{} << "Trade::CgltfImporter::material(): property with an empty name, skipping";
        return {};
    }

    ++tokenIndex;
    const jsmntok_t& token = tokens[tokenIndex];

    /* We only need temporary storage for parsing primitive (arrays) as bool/
       Float/Vector[2/3/4]. Other types/sizes are either converted or ignored,
       so we know the upper limit on the data size. The alignas prevents
       unaligned reads for individual floats. For strings,
       MaterialAttributeData expects a pointer to StringView. */
    alignas(4) char attributeData[16];
    Containers::String attributeString;
    Containers::StringView attributeStringView;
    MaterialAttributeType type{};
    if(token.type == JSMN_OBJECT) {
        /* Not parsing textureInfo objects here because they're only needed by
           extensions but not by extras. They may also append more than one
           attribute, so this is handled directly in the extension parsing
           loop. */
        Warning{} << "Trade::CgltfImporter::material(): property" << name << "is an object, skipping";
        return {};

    /* A primitive is anything that's not a string, object or array. We ignore
       non-primitive arrays, so we can handle both in one place. */
    } else if(token.type == JSMN_PRIMITIVE || token.type == JSMN_ARRAY) {
        const UnsignedInt start = tokenIndex + UnsignedInt(token.type == JSMN_ARRAY);
        /* Primitive token size is 0, but we can't use max() because
            that would allow empty arrays */
        const UnsignedInt count = token.type == JSMN_PRIMITIVE ? 1 : token.size;

        /* No use importing arbitrarily-sized arrays of primitives, those are
           currently not used in any glTF extension */
        if(count >= 1 && count <= 4) {
            /* This still works for non-primitive array members (like objects
               or nested arrays), because we instantly abort when we find one
               of those, so we never need more than four tokens */
            for(const jsmntok_t& element: tokens.slice(start, start + count)) {
                if(element.type != JSMN_PRIMITIVE) {
                    type = MaterialAttributeType{};
                    break;
                }

                /* Jsmn only checks the first character, which allows some
                   invalid values like nnn. We perform basic type detection and
                   invalid values result in 0. This matches cgltf behaviour. */
                const Containers::StringView value = tokenString(json, element);
                if(value == "true"_s || value == "false"_s) {
                    /* MaterialAttributeType has no bool vectors, and
                       converting to a number needlessly complicates parsing
                       later. So far there is no glTF extension that uses bool
                       vectors. */
                    if(count > 1) {
                        type = MaterialAttributeType{};
                        break;
                    }
                    type = MaterialAttributeType::Bool;
                } else if(value != "null"_s) {
                    /* Always interpret numbers as floats because the type can
                       be ambiguous. E.g. integer attributes may use exponent
                       notation and decimal points, making correct type
                       detection depend on glTF exporter behaviour. */
                    type = MaterialAttributeType::Float;
                } else {
                    type = MaterialAttributeType{};
                    break;
                }
            }
        }

        if(type == MaterialAttributeType{}) {
            Warning{} << "Trade::CgltfImporter::material(): property" << name << "has unsupported type, skipping";
            return {};
        }

        if(type == MaterialAttributeType::Float) {
            constexpr MaterialAttributeType vectorType[4] {
                MaterialAttributeType::Float, MaterialAttributeType::Vector2,
                MaterialAttributeType::Vector3, MaterialAttributeType::Vector4
            };
            type = vectorType[count - 1];

            Vector4& data = *reinterpret_cast<Vector4*>(attributeData);
            for(UnsignedInt i = 0; i != count; ++i)
                data[i] = cgltf_json_to_float(&tokens[start + i], reinterpret_cast<const uint8_t*>(json.data()));

        } else if(type == MaterialAttributeType::Bool) {
            CORRADE_INTERNAL_ASSERT(count == 1);
            bool& data = *reinterpret_cast<bool*>(attributeData);
            data = cgltf_json_to_bool(&tokens[start], reinterpret_cast<const uint8_t*>(json.data()));

        } else CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */

    } else if(token.type == JSMN_STRING) {
        const Containers::StringView value = tokenString(json, token);
        Containers::Optional<Containers::String> decoded = decodeString(value);
        if(decoded) {
            attributeString = std::move(*decoded);
            attributeStringView = attributeString;
        } else
            attributeStringView = value;
        type = MaterialAttributeType::String;

    /* JSMN_UNDEFINED, should never happen for valid JSON files */
    } else CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */

    CORRADE_INTERNAL_ASSERT(type != MaterialAttributeType{});

    const void* const valuePointer = type == MaterialAttributeType::String ?
        static_cast<const void*>(&attributeStringView) : static_cast<const void*>(attributeData);
    if(!checkMaterialAttributeSize(name, type, valuePointer))
        return {};

    /* Uppercase attribute names are reserved. Standard glTF (extension)
       attributes should all be lowercase but we don't have this guarantee for
       extras attributes. Can't use String::nullTerminatedView() here because
       JSON tokens are not null-terminated. */
    Containers::String nameLowercase;
    if(!name.isEmpty() && std::isupper(static_cast<unsigned char>(name.front()))) {
        nameLowercase = name;
        nameLowercase[0] = std::tolower(static_cast<unsigned char>(name.front()));
        name = nameLowercase;
    }

    return MaterialAttributeData{name, type, valuePointer};
}

}

void CgltfImporter::Document::materialTexture(const cgltf_texture_view& texture, Containers::Array<MaterialAttributeData>& attributes, Containers::StringView attribute, Containers::StringView matrixAttribute, Containers::StringView coordinateAttribute) const {
    CORRADE_INTERNAL_ASSERT(texture.texture);

    UnsignedInt texCoord = texture.texcoord;

    /* Texture transform. Because texture coordinates were Y-flipped, we first
       unflip them back, apply the transform (which assumes origin at bottom
       left and Y down) and then flip the result again. Sanity of the following
       verified with https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/TextureTransformTest */
    if(texture.has_transform && checkMaterialAttributeSize(matrixAttribute, MaterialAttributeType::Matrix3x3)) {
        Matrix3 matrix;

        /* If material needs an Y-flip, the mesh doesn't have the texture
           coordinates flipped and thus we don't need to unflip them first */
        if(!textureCoordinateYFlipInMaterial)
            matrix = Matrix3::translation(Vector2::yAxis(1.0f))*
                     Matrix3::scaling(Vector2::yScale(-1.0f));

        /* The extension can override texture coordinate index (for example
           to have the unextended coordinates already transformed, and applying
           transformation to a different set) */
        if(texture.transform.has_texcoord)
            texCoord = texture.transform.texcoord;

        matrix = Matrix3::scaling(Vector2::from(texture.transform.scale))*matrix;

        /* Because we import images with Y flipped, counterclockwise rotation
           is now clockwise. This has to be done in addition to the Y
           flip/unflip. */
        matrix = Matrix3::rotation(-Rad(texture.transform.rotation))*matrix;

        matrix = Matrix3::translation(Vector2::from(texture.transform.offset))*matrix;

        matrix = Matrix3::translation(Vector2::yAxis(1.0f))*
                 Matrix3::scaling(Vector2::yScale(-1.0f))*matrix;

        arrayAppend(attributes, InPlaceInit, matrixAttribute, matrix);
    }

    /* In case the material had no texture transformation but still needs an
       Y-flip, put it there */
    if(!texture.has_transform && textureCoordinateYFlipInMaterial &&
       checkMaterialAttributeSize(matrixAttribute, MaterialAttributeType::Matrix3x3))
    {
        arrayAppend(attributes, InPlaceInit, matrixAttribute,
            Matrix3::translation(Vector2::yAxis(1.0f))*
            Matrix3::scaling(Vector2::yScale(-1.0f)));
    }

    /* Add texture coordinate set if non-zero. The KHR_texture_transform could
       be modifying it, so do that after */
    if(texCoord != 0 && checkMaterialAttributeSize(coordinateAttribute, MaterialAttributeType::UnsignedInt))
        arrayAppend(attributes, InPlaceInit, coordinateAttribute, texCoord);

    /* In some cases (when dealing with packed textures), we're parsing &
       adding texture coordinates and matrix multiple times, but adding the
       packed texture ID just once. In other cases the attribute is invalid. */
    if(!attribute.isEmpty() && checkMaterialAttributeSize(attribute, MaterialAttributeType::UnsignedInt)) {
        const UnsignedInt textureId = texture.texture - data->textures;
        arrayAppend(attributes, InPlaceInit, attribute, textureId);
    }
}

Containers::Optional<MaterialData> CgltfImporter::doMaterial(const UnsignedInt id) {
    const cgltf_material& material = _d->data->materials[id];

    Containers::Array<UnsignedInt> layers;
    Containers::Array<MaterialAttributeData> attributes;
    MaterialTypes types;

    /* Alpha mode and mask, double sided */
    if(material.alpha_mode == cgltf_alpha_mode_blend)
        arrayAppend(attributes, InPlaceInit, MaterialAttribute::AlphaBlend, true);
    else if(material.alpha_mode == cgltf_alpha_mode_mask)
        arrayAppend(attributes, InPlaceInit, MaterialAttribute::AlphaMask, material.alpha_cutoff);
    else if(material.alpha_mode != cgltf_alpha_mode_opaque) {
        /* This should never be reached, cgltf treats invalid alpha modes as
           opaque */
        CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
    }

    if(material.double_sided)
        arrayAppend(attributes, InPlaceInit, MaterialAttribute::DoubleSided, true);

    /* Core metallic/roughness material */
    if(material.has_pbr_metallic_roughness) {
        types |= MaterialType::PbrMetallicRoughness;

        const Vector4 baseColorFactor = Vector4::from(material.pbr_metallic_roughness.base_color_factor);
        if(baseColorFactor != Vector4{1.0f})
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::BaseColor,
                Color4{baseColorFactor});
        if(material.pbr_metallic_roughness.metallic_factor != 1.0f)
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::Metalness,
                material.pbr_metallic_roughness.metallic_factor);
        if(material.pbr_metallic_roughness.roughness_factor != 1.0f)
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::Roughness,
                material.pbr_metallic_roughness.roughness_factor);

        if(material.pbr_metallic_roughness.base_color_texture.texture) {
            _d->materialTexture(
                material.pbr_metallic_roughness.base_color_texture,
                attributes,
                "BaseColorTexture"_s,
                "BaseColorTextureMatrix"_s,
                "BaseColorTextureCoordinates"_s);
        }

        if(material.pbr_metallic_roughness.metallic_roughness_texture.texture) {
            _d->materialTexture(
                material.pbr_metallic_roughness.metallic_roughness_texture,
                attributes,
                "NoneRoughnessMetallicTexture"_s,
                "MetalnessTextureMatrix"_s,
                "MetalnessTextureCoordinates"_s);

            /* Add the matrix/coordinates attributes also for the roughness
               texture, but skip adding the texture ID again */
            _d->materialTexture(
                material.pbr_metallic_roughness.metallic_roughness_texture,
                attributes,
                {},
                "RoughnessTextureMatrix"_s,
                "RoughnessTextureCoordinates"_s);
        }

        /** @todo Support for KHR_materials_specular? This adds an explicit
            F0 (texture) and a scalar factor (texture) for the entire specular
            reflection to a metallic/roughness material. Currently imported as
            a custom layer below. */
    }

    /* Specular/glossiness material */
    if(material.has_pbr_specular_glossiness) {
        types |= MaterialType::PbrSpecularGlossiness;

        const Vector4 diffuseFactor = Vector4::from(material.pbr_specular_glossiness.diffuse_factor);
        if(diffuseFactor != Vector4{1.0f})
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::DiffuseColor,
                Color4{diffuseFactor});

        const Vector3 specularFactor = Vector3::from(material.pbr_specular_glossiness.specular_factor);
        if(specularFactor != Vector3{1.0f})
            arrayAppend(attributes, InPlaceInit,
                /* Specular is 3-component in glTF, alpha should be 0 to not
                   affect transparent materials */
                MaterialAttribute::SpecularColor,
                Color4{specularFactor, 0.0f});

        if(material.pbr_specular_glossiness.glossiness_factor != 1.0f)
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::Glossiness,
                material.pbr_specular_glossiness.glossiness_factor);

        if(material.pbr_specular_glossiness.diffuse_texture.texture) {
            _d->materialTexture(
                material.pbr_specular_glossiness.diffuse_texture,
                attributes,
                "DiffuseTexture"_s,
                "DiffuseTextureMatrix"_s,
                "DiffuseTextureCoordinates"_s);
        }

        if(material.pbr_specular_glossiness.specular_glossiness_texture.texture) {
           _d->materialTexture(
                material.pbr_specular_glossiness.specular_glossiness_texture,
                attributes,
                "SpecularGlossinessTexture"_s,
                "SpecularTextureMatrix"_s,
                "SpecularTextureCoordinates"_s);

            /* Add the matrix/coordinates attributes also for the glossiness
               texture, but skip adding the texture ID again */
            _d->materialTexture(
                material.pbr_specular_glossiness.specular_glossiness_texture,
                attributes,
                {},
                "GlossinessTextureMatrix"_s,
                "GlossinessTextureCoordinates"_s);
        }
    }

    /* Unlit material -- reset all types and add just Flat */
    if(material.unlit)
        types = MaterialType::Flat;

    /* Normal texture */
    if(material.normal_texture.texture) {
        _d->materialTexture(
            material.normal_texture,
            attributes,
            "NormalTexture"_s,
            "NormalTextureMatrix"_s,
            "NormalTextureCoordinates"_s);

        if(material.normal_texture.scale != 1.0f)
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::NormalTextureScale,
                material.normal_texture.scale);
    }

    /* Occlusion texture */
    if(material.occlusion_texture.texture) {
        _d->materialTexture(
            material.occlusion_texture,
            attributes,
            "OcclusionTexture"_s,
            "OcclusionTextureMatrix"_s,
            "OcclusionTextureCoordinates"_s);

        /* cgltf exposes the strength multiplier as scale */
        if(material.occlusion_texture.scale != 1.0f)
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::OcclusionTextureStrength,
                material.occlusion_texture.scale);
    }

    /* Emissive factor & texture */
    const Vector3 emissiveFactor = Vector3::from(material.emissive_factor);
    if(emissiveFactor != Vector3{0.0f})
        arrayAppend(attributes, InPlaceInit,
            MaterialAttribute::EmissiveColor,
            Color3{emissiveFactor});
    if(material.emissive_texture.texture) {
        _d->materialTexture(
            material.emissive_texture,
            attributes,
            "EmissiveTexture"_s,
            "EmissiveTextureMatrix"_s,
            "EmissiveTextureCoordinates"_s);
    }

    /* Phong material fallback for backwards compatibility */
    if(configuration().value<bool>("phongMaterialFallback")) {
        /* This adds a Phong type even to Flat materials because that's exactly
           how it behaved before */
        types |= MaterialType::Phong;

        /* Create Diffuse attributes from BaseColor */
        Containers::Optional<Color4> diffuseColor;
        Containers::Optional<UnsignedInt> diffuseTexture;
        Containers::Optional<Matrix3> diffuseTextureMatrix;
        Containers::Optional<UnsignedInt> diffuseTextureCoordinates;
        for(const MaterialAttributeData& attribute: attributes) {
            if(attribute.name() == "BaseColor")
                diffuseColor = attribute.value<Color4>();
            else if(attribute.name() == "BaseColorTexture")
                diffuseTexture = attribute.value<UnsignedInt>();
            else if(attribute.name() == "BaseColorTextureMatrix")
                diffuseTextureMatrix = attribute.value<Matrix3>();
            else if(attribute.name() == "BaseColorTextureCoordinates")
                diffuseTextureCoordinates = attribute.value<UnsignedInt>();
        }

        /* But if there already are those from the specular/glossiness
           material, don't add them again. Has to be done in a separate pass
           to avoid resetting too early. */
        for(const MaterialAttributeData& attribute: attributes) {
            if(attribute.name() == "DiffuseColor")
                diffuseColor = Containers::NullOpt;
            else if(attribute.name() == "DiffuseTexture")
                diffuseTexture = Containers::NullOpt;
            else if(attribute.name() == "DiffuseTextureMatrix")
                diffuseTextureMatrix = Containers::NullOpt;
            else if(attribute.name() == "DiffuseTextureCoordinates")
                diffuseTextureCoordinates = Containers::NullOpt;
        }

        if(diffuseColor)
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::DiffuseColor, *diffuseColor);
        if(diffuseTexture)
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::DiffuseTexture, *diffuseTexture);
        if(diffuseTextureMatrix)
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::DiffuseTextureMatrix, *diffuseTextureMatrix);
        if(diffuseTextureCoordinates)
            arrayAppend(attributes, InPlaceInit, MaterialAttribute::DiffuseTextureCoordinates, *diffuseTextureCoordinates);
    }

    /* Extras -- application-specific data, added to the base layer */
    if(material.extras.start_offset) {
        CORRADE_INTERNAL_ASSERT(material.extras.end_offset > material.extras.start_offset);
        const Containers::StringView json{
            _d->data->json + material.extras.start_offset,
            material.extras.end_offset - material.extras.start_offset};
        /* Theoretically extras can be any token type but the glTF spec
           recommends objects for interoperability, makes our life easier, too:
           https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#reference-extras
           Cgltf directly gives us the JSON value string instead of a token so
           we can only check the type like this. */
        if(json[0] == '{') {
            const auto tokens = parseJson(json);
            /* This is checked by jsmn */
            CORRADE_INTERNAL_ASSERT(!tokens.isEmpty() && tokens[0].type == JSMN_OBJECT);

            UnsignedInt numAttributes = tokens[0].size;
            Containers::Array<UnsignedInt> attributeTokens;
            arrayReserve(attributeTokens, numAttributes);
            for(UnsignedInt t = 1; t + 1 < tokens.size();) {
                /* This is checked by jsmn */
                CORRADE_INTERNAL_ASSERT(tokens[t].type == JSMN_STRING && tokens[t].size == 1);
                arrayAppend(attributeTokens, InPlaceInit, t);
                t = skipJson(tokens, t + 1);
            }

            /* Sort and mark duplicates, those will be skipped later. We don't
               need to cross-check for duplicates in the base layer because
               those are all internal uppercase names and we make all names
               lowercase. */
            std::stable_sort(attributeTokens.begin(), attributeTokens.end(), [&](UnsignedInt a, UnsignedInt b) {
                return tokenString(json, tokens[a]) < tokenString(json, tokens[b]);
            });

            for(std::size_t i = 0; i + 1 < attributeTokens.size(); ++i) {
                if(tokenString(json, tokens[attributeTokens[i]]) == tokenString(json, tokens[attributeTokens[i + 1]])) {
                    --numAttributes;
                    /* We can use 0 as an invalid token to mark attributes to
                       skip. Token 0 is always the extras object itself. */
                    attributeTokens[i] = 0u;
                }
            }

            arrayReserve(attributes, attributes.size() + numAttributes);
            for(UnsignedInt tokenIndex: attributeTokens) {
                if(tokenIndex == 0u) continue;

                const Containers::Optional<MaterialAttributeData> parsed = parseMaterialAttribute(
                    json, tokens.exceptPrefix(tokenIndex));
                if(parsed)
                    arrayAppend(attributes, *parsed);
            }

        } else Warning{} << "Trade::CgltfImporter::material(): extras property is not an object, skipping";
    }

    /* Clear coat layer -- needs to be after all base material attributes */
    if(material.has_clearcoat) {
        types |= MaterialType::PbrClearCoat;

        /* Add a new layer -- this works both if layers are empty and if
           there's something already */
        arrayAppend(layers, UnsignedInt(attributes.size()));
        arrayAppend(attributes, InPlaceInit, MaterialLayer::ClearCoat);

        arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::LayerFactor,
                material.clearcoat.clearcoat_factor);

        if(material.clearcoat.clearcoat_texture.texture) {
            _d->materialTexture(
                material.clearcoat.clearcoat_texture,
                attributes,
                "LayerFactorTexture"_s,
                "LayerFactorTextureMatrix"_s,
                "LayerFactorTextureCoordinates"_s);
        }

        arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::Roughness,
                material.clearcoat.clearcoat_roughness_factor);

        if(material.clearcoat.clearcoat_roughness_texture.texture) {
            _d->materialTexture(
                material.clearcoat.clearcoat_roughness_texture,
                attributes,
                "RoughnessTexture"_s,
                "RoughnessTextureMatrix"_s,
                "RoughnessTextureCoordinates"_s);

            /* The extension description doesn't mention it, but the schema
               says the clearcoat roughness is actually in the G channel:
               https://github.com/KhronosGroup/glTF/blob/dc5519b9ce9834f07c30ec4c957234a0cd6280a2/extensions/2.0/Khronos/KHR_materials_clearcoat/schema/glTF.KHR_materials_clearcoat.schema.json#L32 */
            arrayAppend(attributes, InPlaceInit,
                MaterialAttribute::RoughnessTextureSwizzle,
                MaterialTextureSwizzle::G);
        }

        if(material.clearcoat.clearcoat_normal_texture.texture) {
            _d->materialTexture(
                material.clearcoat.clearcoat_normal_texture,
                attributes,
                "NormalTexture"_s,
                "NormalTextureMatrix"_s,
                "NormalTextureCoordinates"_s);

            if(material.clearcoat.clearcoat_normal_texture.scale != 1.0f)
                arrayAppend(attributes, InPlaceInit,
                    MaterialAttribute::NormalTextureScale,
                    material.clearcoat.clearcoat_normal_texture.scale);
        }
    }

    /* Import extensions with non-standard layer/attribute types that are
       already parsed by cgltf and hence don't appear in the extension list
       anymore. We use the original attribute names as found in the extension
       specifications. To imitate actual unknown extension import below, all
       Int and UnsignedInt attributes must be converted to Float. */
    /** @todo If these are turned into standard layer types they will have to
        be duplicated for backwards compatibility, with the old names and type
        conversion. */
    if(material.has_ior) {
        arrayAppend(layers, UnsignedInt(attributes.size()));
        arrayAppend(attributes, InPlaceInit, MaterialAttribute::LayerName, "#KHR_materials_ior"_s);

        arrayAppend(attributes, InPlaceInit, "ior", material.ior.ior);
    }

    if(material.has_specular) {
        arrayAppend(layers, UnsignedInt(attributes.size()));
        arrayAppend(attributes, InPlaceInit, MaterialAttribute::LayerName, "#KHR_materials_specular"_s);

        arrayAppend(attributes, InPlaceInit,
            "specularFactor"_s,
            material.specular.specular_factor);

        if(material.specular.specular_texture.texture) {
            _d->materialTexture(
                material.specular.specular_texture,
                attributes,
                "specularTexture"_s,
                "specularTextureMatrix"_s,
                "specularTextureCoordinates"_s);

            /* Specular strength is stored in the alpha channel */
            arrayAppend(attributes, InPlaceInit,
                "specularTextureSwizzle"_s,
                MaterialTextureSwizzle::A);
        }

        const Vector3 specularColorFactor = Vector3::from(material.specular.specular_color_factor);
        arrayAppend(attributes, InPlaceInit,
            "specularColorFactor"_s,
            specularColorFactor);

        if(material.specular.specular_color_texture.texture)
            _d->materialTexture(
                material.specular.specular_color_texture,
                attributes,
                "specularColorTexture"_s,
                "specularColorTextureMatrix"_s,
                "specularColorTextureCoordinates"_s);
    }

    if(material.has_transmission) {
        arrayAppend(layers, UnsignedInt(attributes.size()));
        arrayAppend(attributes, InPlaceInit, MaterialAttribute::LayerName, "#KHR_materials_transmission"_s);

        arrayAppend(attributes, InPlaceInit,
            "transmissionFactor"_s,
            material.transmission.transmission_factor);

        if(material.transmission.transmission_texture.texture)
            _d->materialTexture(
                material.transmission.transmission_texture,
                attributes,
                "transmissionTexture"_s,
                "transmissionTextureMatrix"_s,
                "transmissionTextureCoordinates"_s);
    }

    if(material.has_volume) {
        arrayAppend(layers, UnsignedInt(attributes.size()));
        arrayAppend(attributes, InPlaceInit, MaterialAttribute::LayerName, "#KHR_materials_volume"_s);

        arrayAppend(attributes, InPlaceInit,
            "thicknessFactor"_s,
            material.volume.thickness_factor);

        if(material.volume.thickness_texture.texture) {
            _d->materialTexture(
                material.volume.thickness_texture,
                attributes,
                "thicknessTexture"_s,
                "thicknessTextureMatrix"_s,
                "thicknessTextureCoordinates"_s);

            /* Thickness is stored in the green channel */
            arrayAppend(attributes, InPlaceInit,
                "thicknessTextureSwizzle"_s,
                MaterialTextureSwizzle::G);
        }

        /* Default spec value is infinity but cgltf uses FLT_MAX, fix it */
        const Float attenuationDistance = material.volume.attenuation_distance == std::numeric_limits<Float>::max() ?
            Constants::inf() : material.volume.attenuation_distance;
        arrayAppend(attributes, InPlaceInit,
            "attenuationDistance"_s,
            attenuationDistance);

        const Vector3 attenuationColor = Vector3::from(material.volume.attenuation_color);
        arrayAppend(attributes, InPlaceInit,
            "attenuationColor"_s,
            attenuationColor);
    }

    if(material.has_sheen) {
        arrayAppend(layers, UnsignedInt(attributes.size()));
        arrayAppend(attributes, InPlaceInit, MaterialAttribute::LayerName, "#KHR_materials_sheen"_s);

        const Vector3 sheenColorFactor = Vector3::from(material.sheen.sheen_color_factor);
        arrayAppend(attributes, InPlaceInit,
            "sheenColorFactor"_s,
            sheenColorFactor);

        if(material.sheen.sheen_color_texture.texture)
            _d->materialTexture(
                material.sheen.sheen_color_texture,
                attributes,
                "sheenColorTexture"_s,
                "sheenColorTextureMatrix"_s,
                "sheenColorTextureCoordinates"_s);

        arrayAppend(attributes, InPlaceInit,
            "sheenRoughnessFactor"_s,
            material.sheen.sheen_roughness_factor);

        if(material.sheen.sheen_roughness_texture.texture) {
            _d->materialTexture(
                material.sheen.sheen_roughness_texture,
                attributes,
                "sheenRoughnessTexture"_s,
                "sheenRoughnessTextureMatrix"_s,
                "sheenRoughnessTextureCoordinates"_s);

            /* Sheen roughness is stored in the alpha channel */
            arrayAppend(attributes, InPlaceInit,
                "sheenRoughnessTextureSwizzle"_s,
                MaterialTextureSwizzle::A);
        }
    }

    /* Stable-sort extensions by name so that we can easily find duplicates and
       overwrite lexically preceding extensions. This matches cgltf parsing
       behaviour. */
    Containers::Array<UnsignedInt> extensionOrder{material.extensions_count};
    for(UnsignedInt i = 0; i < extensionOrder.size(); ++i)
        extensionOrder[i] = i;

    std::stable_sort(extensionOrder.begin(), extensionOrder.end(), [&](UnsignedInt a, UnsignedInt b) {
        return std::strcmp(material.extensions[a].name, material.extensions[b].name) < 0;
    });

    /* Mark duplicates, those will be skipped later */
    for(std::size_t i = 0; i + 1 < extensionOrder.size(); ++i) {
        const cgltf_extension& current = material.extensions[extensionOrder[i]];
        const cgltf_extension& next = material.extensions[extensionOrder[i + 1]];
        if(std::strcmp(current.name, next.name) == 0)
            extensionOrder[i] = ~0u;
    }

    /* Import unrecognized extension attributes as custom attributes, one
       layer per extension */
    for(UnsignedInt e: extensionOrder) {
        if(e == ~0u) continue;

        const cgltf_extension& extension = material.extensions[e];

        const Containers::StringView extensionName = extension.name;
        if(extensionName.isEmpty()) {
            Warning{} << "Trade::CgltfImporter::material(): extension with an empty name, skipping";
            continue;
        }

        /* +1 is the key null byte. +3 are the '#' layer prefix, the layer null
           byte and the length. */
        if(" LayerName"_s.size() + 1 + extensionName.size() + 3 + sizeof(MaterialAttributeType) > sizeof(MaterialAttributeData)) {
            Warning{} << "Trade::CgltfImporter::material(): extension name" << extensionName <<
                "is too long with" << extensionName.size() << "characters, skipping";
            continue;
        }

        const Containers::StringView json = extension.data;
        const auto tokens = parseJson(json);
        /* First token is the extension object. This is checked by cgltf. If
           the object is empty, tokens.size() is 1. */
        CORRADE_INTERNAL_ASSERT(!tokens.isEmpty() && tokens[0].type == JSMN_OBJECT);

        UnsignedInt numAttributes = tokens[0].size;
        Containers::Array<UnsignedInt> attributeTokens;
        arrayReserve(attributeTokens, numAttributes);
        for(UnsignedInt t = 1; t + 1 < tokens.size();) {
            /* This is checked by jsmn */
            CORRADE_INTERNAL_ASSERT(tokens[t].type == JSMN_STRING && tokens[t].size == 1);
            arrayAppend(attributeTokens, InPlaceInit, t);
            t = skipJson(tokens, t + 1);
        }

        /* Sort and mark duplicates, those will be skipped later */
        std::stable_sort(attributeTokens.begin(), attributeTokens.end(), [&](UnsignedInt a, UnsignedInt b) {
            return tokenString(json, tokens[a]) < tokenString(json, tokens[b]);
        });

        for(std::size_t i = 0; i + 1 < attributeTokens.size(); ++i) {
            if(tokenString(json, tokens[attributeTokens[i]]) == tokenString(json, tokens[attributeTokens[i + 1]])) {
                --numAttributes;
                /* We can use 0 as an invalid token to mark attributes to skip.
                   Token 0 is always the extension object itself. */
                attributeTokens[i] = 0u;
            }
        }

        Containers::Array<MaterialAttributeData> extensionAttributes;
        arrayReserve(extensionAttributes, numAttributes);
        for(UnsignedInt tokenIndex: attributeTokens) {
            if(tokenIndex == 0u) continue;

            const Containers::StringView name = tokenString(json, tokens[tokenIndex]);
            if(name.isEmpty()) {
                Warning{} << "Trade::CgltfImporter::material(): property with an empty name, skipping";
                continue;
            }

            if(tokens[tokenIndex + 1].type == JSMN_OBJECT) {
                /* Parse glTF textureInfo objects. Any objects without the
                   correct suffix and type are ignored. */
                if(name.size() < 8 || !name.hasSuffix("Texture")) {
                    Warning{} << "Trade::CgltfImporter::material(): property" << name << "has non-texture object type, skipping";
                    continue;
                }

                cgltf_texture_view textureView{};
                const bool valid = cgltf_parse_json_texture_view(&_d->options, tokens, tokenIndex + 1,
                    reinterpret_cast<const uint8_t*>(json.data()), &textureView) >= 0;
                /* Free memory allocated by cgltf. We're only interested in
                   KHR_texture_transform and that's already parsed into
                   textureView.transform. */
                cgltf_free_extensions(_d->data, textureView.extensions, textureView.extensions_count);

                /* cgltf_parse_json_texture_view() casts and saves index + 1 as
                   cgltf_texture*. 0 indicates there was no index property.
                   It's mandatory, so we check for it. */
                if(!valid || !textureView.texture) {
                    Warning{} << "Trade::CgltfImporter::material(): property" << name << "has invalid texture object type, skipping";
                    continue;
                }

                const std::size_t index = std::size_t(textureView.texture) - 1;
                if(index >= _d->data->images_count) {
                    Error{} << "Trade::CgltfImporter::material():" << name << "index" << index << "out of bounds for" << _d->data->textures_count << "textures";
                    return {};
                }

                /* materialTexture() expects a fixed up texture pointer in
                   cgltf_texture_view, normally done by cgltf */
                textureView.texture = &_d->data->textures[index];

                Containers::String nameBuffer{NoInit, name.size()*2 + 6 + 11};
                Utility::formatInto(nameBuffer, "{}Matrix{}Coordinates", name, name);
                _d->materialTexture(
                    textureView,
                    extensionAttributes,
                    name,
                    nameBuffer.prefix(name.size() + 6),
                    nameBuffer.exceptPrefix(name.size() + 6));

                /** @todo If there are ever extensions that reference texture
                    types other than textureInfo and normalTextureInfo, we
                    might have to be a bit smarter here, e.g. detect
                    occlusionTextureInfo and suffix with "Strength" instead.
                    cgltf parses both "strength" and "scale" into the same
                    variable. */
                if(textureView.scale != 1.0f) {
                    Utility::formatInto(nameBuffer, "{}Scale", name);
                    const Containers::StringView scaleName = nameBuffer.prefix(name.size() + 5);
                    if(checkMaterialAttributeSize(scaleName, MaterialAttributeType::Float))
                        arrayAppend(extensionAttributes, InPlaceInit,
                            scaleName, textureView.scale);
                }

            } else {
                /* All other attribute types: bool, numbers, strings */
                const Containers::Optional<MaterialAttributeData> parsed = parseMaterialAttribute(
                    json, tokens.exceptPrefix(tokenIndex));
                if(parsed)
                    arrayAppend(extensionAttributes, *parsed);
            }
        }

        /* Uppercase layer names are reserved. Since all extension names start
           with an uppercase vendor identifier, making the first character
           lowercase seems silly, so we use a unique prefix. */
        Containers::String layerName{NoInit, extensionName.size() + 1};
        Utility::formatInto(layerName, "#{}", extensionName);

        arrayAppend(layers, UnsignedInt(attributes.size()));
        arrayAppend(attributes, InPlaceInit, MaterialAttribute::LayerName, layerName);
        arrayAppend(attributes, Containers::arrayView<const MaterialAttributeData>(extensionAttributes));
    }

    /* If there's any layer, add the final attribute count */
    arrayAppend(layers, UnsignedInt(attributes.size()));

    /* Can't use growable deleters in a plugin, convert back to the default
       deleter */
    arrayShrink(layers);
    arrayShrink(attributes, DefaultInit);
    return MaterialData{types, std::move(attributes), std::move(layers)};
}

UnsignedInt CgltfImporter::doTextureCount() const {
    return _d->data->textures_count;
}

Int CgltfImporter::doTextureForName(const Containers::StringView name) {
    if(!_d->texturesForName) {
        _d->texturesForName.emplace();
        _d->texturesForName->reserve(_d->data->textures_count);
        for(std::size_t i = 0; i != _d->data->textures_count; ++i)
            _d->texturesForName->emplace(_d->decodeCachedString(_d->data->textures[i].name), i);
    }

    const auto found = _d->texturesForName->find(name);
    return found == _d->texturesForName->end() ? -1 : found->second;
}

Containers::String CgltfImporter::doTextureName(const UnsignedInt id) {
    return _d->decodeCachedString(_d->data->textures[id].name);
}

Containers::Optional<TextureData> CgltfImporter::doTexture(const UnsignedInt id) {
    const cgltf_texture& tex = _d->data->textures[id];

    UnsignedInt imageId = ~0u;

    /* Various extensions, they override the standard image */
    if(tex.has_basisu && tex.basisu_image) {
        /* KHR_texture_basisu. Allows the usage of mimeType image/ktx2 but only
           explicitly talks about KTX2 with Basis compression. We don't care
           since we delegate to AnyImageImporter and let it figure out the file
           type based on magic. Note: The core glTF spec only allows image/jpeg
           and image/png but we don't check that either. */
        imageId = tex.basisu_image - _d->data->images;
    } else {
        constexpr Containers::StringView extensions[]{
            /* GOOGLE_texture_basis is not a registered extension but can be found
               in some of the early Basis Universal examples. Basis files don't
               have a registered mimetype either, but as explained above we don't
               care about mimetype at all. */
            "GOOGLE_texture_basis"_s,
            "MSFT_texture_dds"_s
            /** @todo EXT_texture_webp once a plugin provides WebpImporter */
        };
        /* Use the first supported extension, assuming that extension order
           indicates a preference */
        /** @todo Figure out a better priority
            - extensionsRequired?
            - image importers available via manager()->aliasList()?
            - are there even files out there with more than one extension? */
        for(std::size_t i = 0; i != tex.extensions_count && imageId == ~0u; ++i) {
            for(std::size_t j = 0; j != Containers::arraySize(extensions) && imageId == ~0u; ++j) {
                if(tex.extensions[i].name == extensions[j]) {
                    const Containers::StringView json = tex.extensions[i].data;
                    const auto tokens = parseJson(tex.extensions[i].data);
                    /* This is checked by cgltf */
                    CORRADE_INTERNAL_ASSERT(!tokens.isEmpty() && tokens[0].type == JSMN_OBJECT);

                    Containers::Optional<Int> source;
                    std::size_t t = 1;
                    while(t + 1 < tokens.size()) {
                        /* This is checked by jsmn */
                        CORRADE_INTERNAL_ASSERT(tokens[t].type == JSMN_STRING && tokens[t].size == 1);

                        if(tokenString(json, tokens[t]) == "source" && tokens[t + 1].type == JSMN_PRIMITIVE) {
                            source = cgltf_json_to_int(&tokens[t + 1], reinterpret_cast<const uint8_t*>(json.data()));
                            t += 2;
                        } else
                            t = skipJson(tokens, t + 1);
                    }

                    /* Only check the index here because there can be multiple
                       JSON "source" keys and the last one wins. This matches
                       cgltf behaviour. */
                    if(source) {
                        if(*source < 0 || std::size_t(*source) >= _d->data->images_count) {
                            Error{} << "Trade::CgltfImporter::texture():" << extensions[j] << "image" <<
                                *source << "out of bounds for" << _d->data->images_count << "images";
                            return {};
                        }
                        imageId = *source;
                    }
                }
            }
        }
    }

    if(imageId == ~0u) {
        /* If not overwritten by an extension, use the standard 'source'
           attribute. It's not mandatory, so this can still fail. */
        if(tex.image)
            imageId = tex.image - _d->data->images;
        else {
            Error{} << "Trade::CgltfImporter::texture(): no image source found";
            return {};
        }
    }

    CORRADE_INTERNAL_ASSERT(imageId < _d->data->images_count);

    /* Sampler */
    if(!tex.sampler) {
        /* The specification instructs to use "auto sampling", i.e. it is left
           to the implementor to decide on the default values... */
        return TextureData{TextureType::Texture2D, SamplerFilter::Linear, SamplerFilter::Linear,
            SamplerMipmap::Linear, {SamplerWrapping::Repeat, SamplerWrapping::Repeat, SamplerWrapping::Repeat}, imageId};
    }

    /* GL filter enums */
    enum GltfTextureFilter: cgltf_int {
        Nearest = 9728,
        Linear = 9729,
        NearestMipmapNearest = 9984,
        LinearMipmapNearest = 9985,
        NearestMipmapLinear = 9986,
        LinearMipmapLinear = 9987
    };

    SamplerFilter minFilter;
    SamplerMipmap mipmap;
    switch(tex.sampler->min_filter) {
        case GltfTextureFilter::Nearest:
            minFilter = SamplerFilter::Nearest;
            mipmap = SamplerMipmap::Base;
            break;
        case GltfTextureFilter::Linear:
            minFilter = SamplerFilter::Linear;
            mipmap = SamplerMipmap::Base;
            break;
        case GltfTextureFilter::NearestMipmapNearest:
            minFilter = SamplerFilter::Nearest;
            mipmap = SamplerMipmap::Nearest;
            break;
        case GltfTextureFilter::NearestMipmapLinear:
            minFilter = SamplerFilter::Nearest;
            mipmap = SamplerMipmap::Linear;
            break;
        case GltfTextureFilter::LinearMipmapNearest:
            minFilter = SamplerFilter::Linear;
            mipmap = SamplerMipmap::Nearest;
            break;
        case GltfTextureFilter::LinearMipmapLinear:
        /* glTF 2.0 spec does not define a default value for 'minFilter' and
           'magFilter'. In this case cgltf sets it to 0. */
        case 0:
            minFilter = SamplerFilter::Linear;
            mipmap = SamplerMipmap::Linear;
            break;
        default:
            Error{} << "Trade::CgltfImporter::texture(): invalid minFilter" << tex.sampler->min_filter;
            return {};
    }

    SamplerFilter magFilter;
    switch(tex.sampler->mag_filter) {
        case GltfTextureFilter::Nearest:
            magFilter = SamplerFilter::Nearest;
            break;
        case GltfTextureFilter::Linear:
        /* glTF 2.0 spec does not define a default value for 'minFilter' and
           'magFilter'. In this case cgltf sets it to 0. */
        case 0:
            magFilter = SamplerFilter::Linear;
            break;
        default:
            Error{} << "Trade::CgltfImporter::texture(): invalid magFilter" << tex.sampler->mag_filter;
            return {};
    }

    /* GL wrap enums */
    enum GltfTextureWrap: cgltf_int {
        Repeat = 10497,
        ClampToEdge = 33071,
        MirroredRepeat = 33648
    };

    Math::Vector3<SamplerWrapping> wrapping;
    wrapping.z() = SamplerWrapping::Repeat;
    for(auto&& wrap: std::initializer_list<Containers::Pair<Int, Int>>{
        {tex.sampler->wrap_s, 0}, {tex.sampler->wrap_t, 1}})
    {
        switch(wrap.first()) {
            case GltfTextureWrap::Repeat:
                wrapping[wrap.second()] = SamplerWrapping::Repeat;
                break;
            case GltfTextureWrap::ClampToEdge:
                wrapping[wrap.second()] = SamplerWrapping::ClampToEdge;
                break;
            case GltfTextureWrap::MirroredRepeat:
                wrapping[wrap.second()] = SamplerWrapping::MirroredRepeat;
                break;
            default:
                Error{} << "Trade::CgltfImporter::texture(): invalid wrap mode" << wrap.first();
                return {};
        }
    }

    /* glTF supports only 2D textures */
    return TextureData{TextureType::Texture2D, minFilter, magFilter,
        mipmap, wrapping, imageId};
}

UnsignedInt CgltfImporter::doImage2DCount() const {
    return _d->data->images_count;
}

Int CgltfImporter::doImage2DForName(const Containers::StringView name) {
    if(!_d->imagesForName) {
        _d->imagesForName.emplace();
        _d->imagesForName->reserve(_d->data->images_count);
        for(std::size_t i = 0; i != _d->data->images_count; ++i)
            _d->imagesForName->emplace(_d->decodeCachedString(_d->data->images[i].name), i);
    }

    const auto found = _d->imagesForName->find(name);
    return found == _d->imagesForName->end() ? -1 : found->second;
}

Containers::String CgltfImporter::doImage2DName(const UnsignedInt id) {
    return _d->decodeCachedString(_d->data->images[id].name);
}

AbstractImporter* CgltfImporter::setupOrReuseImporterForImage(const UnsignedInt id, const char* const errorPrefix) {
    /* Looking for the same ID, so reuse an importer populated before. If the
       previous attempt failed, the importer is not set, so return nullptr in
       that case. Going through everything below again would not change the
       outcome anyway, only spam the output with redundant messages. */
    if(_d->imageImporterId == id)
        return _d->imageImporter ? &*_d->imageImporter : nullptr;

    /* Otherwise reset the importer and remember the new ID. If the import
       fails, the importer will stay unset, but the ID will be updated so the
       next round can again just return nullptr above instead of going through
       the doomed-to-fail process again. */
    _d->imageImporter = Containers::NullOpt;
    _d->imageImporterId = id;

    AnyImageImporter importer{*manager()};
    if(fileCallback()) importer.setFileCallback(fileCallback(), fileCallbackUserData());

    const cgltf_image& image = _d->data->images[id];

    /* Load embedded image. Can either be a buffer view or a base64 payload.
       Buffers are kept in memory until the importer closes but decoded base64
       data is freed after opening the image. */
    if(!image.uri || isDataUri(image.uri)) {
        Containers::Array<char> imageData;
        Containers::ArrayView<const char> imageView;

        if(image.uri) {
            const auto view = loadUri(errorPrefix, image.uri, imageData);
            if(!view)
                return nullptr;
            imageView = *view;
        } else {
            if(!image.buffer_view) {
                Error{} << errorPrefix << "image has neither a URI nor a buffer view";
                return nullptr;
            }

            if(!checkBufferView(errorPrefix, image.buffer_view))
                return nullptr;

            const cgltf_buffer* buffer = image.buffer_view->buffer;
            const UnsignedInt bufferId = buffer - _d->data->buffers;
            if(!loadBuffer(errorPrefix, bufferId))
                return nullptr;
            imageView = Containers::arrayView(static_cast<const char*>(buffer->data) + image.buffer_view->offset, image.buffer_view->size);
        }

        if(!importer.openData(imageView))
            return nullptr;
        return &_d->imageImporter.emplace(std::move(importer));
    }

    /* Load external image */
    if(!_d->filePath && !fileCallback()) {
        Error{} << errorPrefix << "external images can be imported only when opening files from the filesystem or if a file callback is present";
        return nullptr;
    }

    if(!importer.openFile(Utility::Path::join(_d->filePath ? *_d->filePath : "", decodeUri(_d->decodeCachedString(image.uri)))))
        return nullptr;
    return &_d->imageImporter.emplace(std::move(importer));
}

UnsignedInt CgltfImporter::doImage2DLevelCount(const UnsignedInt id) {
    CORRADE_ASSERT(manager(), "Trade::CgltfImporter::image2DLevelCount(): the plugin must be instantiated with access to plugin manager in order to open image files", {});

    AbstractImporter* importer = setupOrReuseImporterForImage(id, "Trade::CgltfImporter::image2DLevelCount():");
    /* image2DLevelCount() isn't supposed to fail (image2D() is, instead), so
       report 1 on failure and expect image2D() to fail later */
    if(!importer) return 1;

    return importer->image2DLevelCount(0);
}

Containers::Optional<ImageData2D> CgltfImporter::doImage2D(const UnsignedInt id, const UnsignedInt level) {
    CORRADE_ASSERT(manager(), "Trade::CgltfImporter::image2D(): the plugin must be instantiated with access to plugin manager in order to load images", {});

    AbstractImporter* importer = setupOrReuseImporterForImage(id, "Trade::CgltfImporter::image2D():");
    if(!importer) return {};

    Containers::Optional<ImageData2D> imageData = importer->image2D(0, level);
    if(!imageData) return {};
    return ImageData2D{std::move(*imageData)};
}

}}

CORRADE_PLUGIN_REGISTER(CgltfImporter, Magnum::Trade::CgltfImporter,
    "cz.mosra.magnum.Trade.AbstractImporter/0.5")

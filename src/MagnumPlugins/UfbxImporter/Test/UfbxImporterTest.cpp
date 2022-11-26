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

#include <sstream>
#include <string>
#include <Corrade/Containers/Optional.h>
#include <Corrade/Containers/ArrayView.h>
#include <Corrade/Containers/String.h>
#include <Corrade/Containers/StringStl.h>
#include <Corrade/Containers/StringView.h>
#include <Corrade/TestSuite/Tester.h>
#include <Corrade/TestSuite/Compare/Container.h>
#include <Corrade/TestSuite/Compare/Numeric.h>
#include <Corrade/TestSuite/Compare/String.h>
#include <Corrade/Utility/Path.h>
#include <Corrade/Utility/String.h>
#include <Magnum/Math/Vector3.h>
#include <Magnum/Math/Vector4.h>
#include <Magnum/Trade/AbstractImporter.h>
#include <Magnum/Trade/MeshData.h>
#include <Magnum/Trade/SceneData.h>

#include "configure.h"

namespace Magnum { namespace Trade { namespace Test { namespace {

using namespace Math::Literals;
using namespace Containers::Literals;

struct UfbxImporterTest: TestSuite::Tester {
    explicit UfbxImporterTest();

    void openFile();
    void openData();
    void openFileFailed();
    void openDataFailed();
    void mesh();

    /* Needs to load AnyImageImporter from a system-wide location */
    PluginManager::Manager<AbstractImporter> _manager;
};

UfbxImporterTest::UfbxImporterTest() {
    addTests({&UfbxImporterTest::openFile,
              &UfbxImporterTest::openData,
              &UfbxImporterTest::openFileFailed,
              &UfbxImporterTest::openDataFailed});

    addTests({&UfbxImporterTest::mesh});

    /* Load the plugin directly from the build tree. Otherwise it's static and
       already loaded. It also pulls in the AnyImageImporter dependency. */
    #ifdef UFBXIMPORTER_PLUGIN_FILENAME
    CORRADE_INTERNAL_ASSERT_OUTPUT(_manager.load(UFBXIMPORTER_PLUGIN_FILENAME) & PluginManager::LoadState::Loaded);
    #endif

    /* Reset the plugin dir after so it doesn't load anything else from the
       filesystem. Do this also in case of static plugins (no _FILENAME
       defined) so it doesn't attempt to load dynamic system-wide plugins. */
    #ifndef CORRADE_PLUGINMANAGER_NO_DYNAMIC_PLUGIN_SUPPORT
    _manager.setPluginDirectory({});
    #endif
}

void UfbxImporterTest::openFile() {
    Containers::Pointer<AbstractImporter> importer = _manager.instantiate("UfbxImporter");

    CORRADE_VERIFY(importer->openFile(Utility::Path::join(UFBXIMPORTER_TEST_DIR, "blender-default.fbx")));
    CORRADE_VERIFY(importer->isOpened());
    CORRADE_COMPARE(importer->sceneCount(), 1);
    CORRADE_COMPARE(importer->objectCount(), 3);
    CORRADE_COMPARE(importer->meshCount(), 1);
    CORRADE_COMPARE(importer->lightCount(), 1);
    CORRADE_COMPARE(importer->cameraCount(), 1);
    CORRADE_COMPARE(importer->animationCount(), 0);
    CORRADE_COMPARE(importer->skin3DCount(), 0);

    importer->close();
    CORRADE_VERIFY(!importer->isOpened());
}

void UfbxImporterTest::openData() {
    Containers::Pointer<AbstractImporter> importer = _manager.instantiate("UfbxImporter");

    Containers::Optional<Containers::Array<char>> data = Utility::Path::read(Utility::Path::join(UFBXIMPORTER_TEST_DIR, "blender-default.fbx"));
    CORRADE_VERIFY(importer->openData(*data));
    CORRADE_VERIFY(importer->isOpened());
    CORRADE_COMPARE(importer->sceneCount(), 1);
    CORRADE_COMPARE(importer->objectCount(), 3);
    CORRADE_COMPARE(importer->meshCount(), 1);
    CORRADE_COMPARE(importer->lightCount(), 1);
    CORRADE_COMPARE(importer->cameraCount(), 1);
    CORRADE_COMPARE(importer->animationCount(), 0);
    CORRADE_COMPARE(importer->skin3DCount(), 0);

    importer->close();
    CORRADE_VERIFY(!importer->isOpened());
}

void UfbxImporterTest::openFileFailed() {
    Containers::Pointer<AbstractImporter> importer = _manager.instantiate("UfbxImporter");

    std::ostringstream out;
    Error redirectError{&out};

    CORRADE_VERIFY(!importer->openFile("i-do-not-exist.foo"));
    CORRADE_COMPARE(out.str(), "Trade::UfbxImporter::openData(): loading failed: File not found: i-do-not-exist.foo\n"_s);
}

void UfbxImporterTest::openDataFailed() {
    Containers::Pointer<AbstractImporter> importer = _manager.instantiate("UfbxImporter");

    std::ostringstream out;
    Error redirectError{&out};

    constexpr const char data[] = "what";
    CORRADE_VERIFY(!importer->openData({data, sizeof(data)}));
    CORRADE_COMPARE(out.str(), "Trade::UfbxImporter::openData(): loading failed: Unrecognized file format\n"_s);
}

void UfbxImporterTest::mesh() {
    Containers::Pointer<AbstractImporter> importer = _manager.instantiate("UfbxImporter");
    CORRADE_VERIFY(importer->openFile(Utility::Path::join(UFBXIMPORTER_TEST_DIR, "triangle.fbx")));

    CORRADE_COMPARE(importer->meshCount(), 1);

    /* FBX files don't have reliable mesh names, so go by index */
    Containers::Optional<MeshData> mesh = importer->mesh(0);
    CORRADE_VERIFY(mesh);

    CORRADE_COMPARE(mesh->primitive(), MeshPrimitive::Triangles);

    CORRADE_VERIFY(mesh->isIndexed());
    CORRADE_COMPARE_AS(mesh->indices<UnsignedInt>(),
        Containers::arrayView<UnsignedInt>({0, 1, 2}),
        TestSuite::Compare::Container);

    CORRADE_COMPARE(mesh->attributeCount(), 6);
    CORRADE_COMPARE(mesh->attributeCount(MeshAttribute::Position), 1);
    CORRADE_COMPARE_AS(mesh->attribute<Vector3>(MeshAttribute::Position),
        Containers::arrayView<Vector3>({
            {1.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {-1.0f, -1.0f, 0.0f}
        }), TestSuite::Compare::Container);
    CORRADE_COMPARE(mesh->attributeCount(MeshAttribute::Normal), 1);
    CORRADE_COMPARE_AS(mesh->attribute<Vector3>(MeshAttribute::Normal),
        Containers::arrayView<Vector3>({
            {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}
        }), TestSuite::Compare::Container);
    CORRADE_COMPARE(mesh->attributeCount(MeshAttribute::Tangent), 1);
    CORRADE_COMPARE_AS(mesh->attribute<Vector3>(MeshAttribute::Tangent),
        Containers::arrayView<Vector3>({
            {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        }), TestSuite::Compare::Container);
    CORRADE_COMPARE(mesh->attributeCount(MeshAttribute::Bitangent), 1);
    CORRADE_COMPARE_AS(mesh->attribute<Vector3>(MeshAttribute::Bitangent),
        Containers::arrayView<Vector3>({
            {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        }), TestSuite::Compare::Container);
    CORRADE_COMPARE(mesh->attributeCount(MeshAttribute::TextureCoordinates), 1);
    CORRADE_COMPARE_AS(mesh->attribute<Vector2>(MeshAttribute::TextureCoordinates),
        Containers::arrayView<Vector2>({
            {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f},
        }), TestSuite::Compare::Container);
    CORRADE_COMPARE(mesh->attributeCount(MeshAttribute::Color), 1);
    CORRADE_COMPARE_AS(mesh->attribute<Vector4>(MeshAttribute::Color),
        Containers::arrayView<Vector4>({
            {0.0f, 0.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f},
        }), TestSuite::Compare::Container);

    CORRADE_COMPARE(importer->sceneCount(), 1);

    Containers::Optional<SceneData> scene = importer->scene(0);
    CORRADE_VERIFY(scene);
    CORRADE_COMPARE(scene->fieldCount(), 6);

    /* Fields we're not interested in */
    CORRADE_VERIFY(scene->hasField(SceneField::Parent));
    CORRADE_VERIFY(scene->hasField(SceneField::Translation));
    CORRADE_VERIFY(scene->hasField(SceneField::Rotation));
    CORRADE_VERIFY(scene->hasField(SceneField::Scaling));

    CORRADE_VERIFY(scene->hasField(SceneField::Mesh));
    CORRADE_VERIFY(scene->hasField(SceneField::MeshMaterial));
    CORRADE_COMPARE_AS(scene->mapping<UnsignedInt>(SceneField::Mesh), Containers::arrayView<UnsignedInt>({
        0,
    }), TestSuite::Compare::Container);
    CORRADE_COMPARE_AS(scene->field<UnsignedInt>(SceneField::Mesh), Containers::arrayView<UnsignedInt>({
        0,
    }), TestSuite::Compare::Container);
    CORRADE_COMPARE_AS(scene->field<Int>(SceneField::MeshMaterial), Containers::arrayView<Int>({
        0,
    }), TestSuite::Compare::Container);
}

}}}}

CORRADE_TEST_MAIN(Magnum::Trade::Test::UfbxImporterTest)

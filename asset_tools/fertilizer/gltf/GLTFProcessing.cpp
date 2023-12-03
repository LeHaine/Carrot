//
// Created by jglrxavpok on 05/11/2022.
//

#include <gltf/GLTFProcessing.h>
#include <core/utils/CarrotTinyGLTF.h>
#include <core/scene/LoadedScene.h>
#include <core/Macros.h>
#include <core/scene/GLTFLoader.h>
#include <gltf/GLTFWriter.h>
#include <unordered_set>
#include <core/io/Logging.hpp>
#include <glm/gtx/component_wise.hpp>
#include <core/tasks/Tasks.h>
#include <core/data/Hashes.h>

#include <gltf/MikkTSpaceInterface.h>

#include <meshoptimizer.h>

#define IDXTYPEWIDTH 64
#define REALTYPEWIDTH 64
#include <metis.h>

namespace Fertilizer {

    constexpr float Epsilon = 10e-16;
    constexpr const char* const KHR_TEXTURE_BASISU_EXTENSION_NAME = "KHR_texture_basisu";

    using fspath = std::filesystem::path;
    using namespace Carrot::IO;
    using namespace Carrot::Render;

    bool gltfReadWholeFile(std::vector<unsigned char>* out,
                           std::string* err, const std::string& filepath,
                           void* userData) {
        FILE* f;

        CLEANUP(fclose(f));
        if(fopen_s(&f, filepath.c_str(), "rb")) {
            *err = "Could not open file.";
            return false;
        }

        if(fseek(f, 0, SEEK_END)) {
            *err = "Could not seek in file.";
            return false;
        }

        std::size_t size = ftell(f);

        if(fseek(f, 0, SEEK_SET)) {
            *err = "Could not seek in file.";
            return false;
        }

        out->resize(size);
        std::size_t readSize = fread_s(out->data(), size, 1, size, f);
        if(readSize != size) {
            *err = "Could not read file.";
            return false;
        }
        return true;
    }

    std::string gltfExpandFilePath(const std::string& filepath, void* userData) {
        const fspath& basePath = *reinterpret_cast<fspath*>(userData);
        return (basePath / filepath).string();
    }

    bool gltfFileExists(const std::string& abs_filename, void* userData) {
        return std::filesystem::exists(abs_filename);
    }

    /**
     * "Expands" the vertex buffer: this is the exact opposite of indexing, we separate vertex info for each face
     *  otherwise keeping the same index buffer will provide incorrect results after attribute generation
     */
    static ExpandedMesh expandMesh(const LoadedPrimitive& primitive) {
        ExpandedMesh expanded;
        const bool isSkinned = primitive.isSkinned;
        const std::size_t vertexCount = isSkinned ? primitive.skinnedVertices.size() : primitive.vertices.size();
        const std::size_t indexCount = primitive.indices.size();
        expanded.vertices.resize(indexCount);
        expanded.duplicatedVertices.resize(vertexCount);
        for(std::size_t i = 0; i < indexCount; i++) {
            const std::uint32_t index = primitive.indices[i];
            if(isSkinned) {
                expanded.vertices[i].vertex = primitive.skinnedVertices[index];
            } else {
                expanded.vertices[i].vertex.pos = primitive.vertices[index].pos;
                expanded.vertices[i].vertex.normal = primitive.vertices[index].normal;
                expanded.vertices[i].vertex.tangent = primitive.vertices[index].tangent;
                expanded.vertices[i].vertex.color = primitive.vertices[index].color;
                expanded.vertices[i].vertex.uv = primitive.vertices[index].uv;
            }
            expanded.vertices[i].originalIndex = index;

            expanded.duplicatedVertices[index].push_back(i);
        }

        return std::move(expanded);
    }

    static bool areSameVertices(const Carrot::SkinnedVertex& a, const Carrot::SkinnedVertex& b) {
        auto similar2 = [](const glm::vec2& a, const glm::vec2& b) {
            return glm::compMax(glm::abs(a-b)) < 10e-6f;
        };
        auto similar3 = [](const glm::vec3& a, const glm::vec3& b) {
            return glm::compMax(glm::abs(a-b)) < 10e-6f;
        };
        auto similar4 = [](const glm::vec4& a, const glm::vec4& b) {
            return glm::compMax(glm::abs(a-b)) < 10e-6f;
        };

        return similar3(a.pos.xyz, b.pos.xyz)
            && similar3(a.normal, b.normal)
            && similar4(a.tangent, b.tangent)
            && similar2(a.uv, b.uv)
            && similar3(a.color, b.color)
            && similar3(a.boneWeights, b.boneWeights)
            && a.boneIDs == b.boneIDs;
    }


    /**
     * Generate indexed mesh into 'out' from a non-indexed mesh inside ExpandedMesh
     */
    static void collapseMesh(LoadedPrimitive& out, ExpandedMesh& mesh) {
        out.vertices.clear();
        out.skinnedVertices.clear();
        out.indices.clear();
        std::uint32_t nextIndex = 0;
        const bool isSkinned = out.isSkinned;
        for(std::size_t vertexIndex = 0; vertexIndex < mesh.vertices.size(); vertexIndex++) {
            auto& duplicatedVertex = mesh.vertices[vertexIndex];
            verify(!duplicatedVertex.newIndex.has_value(), "Programming error: duplicated vertex must not already have an index in the new mesh");
            const std::vector<std::uint32_t>& siblingIndices = mesh.duplicatedVertices[duplicatedVertex.originalIndex];

            // check if any sibling is still the same as our current vertex, and has already been written to vertex buffer
            std::optional<std::uint32_t> indexToReuse;
            for(std::uint32_t siblingIndex : siblingIndices) {
                const ExpandedVertex& sibling = mesh.vertices[siblingIndex];
                if(!sibling.newIndex.has_value()) {
                    continue; // not already in vertex buffer
                }

                if(areSameVertices(sibling.vertex, duplicatedVertex.vertex)) {
                    indexToReuse = sibling.newIndex.value();
                    break;
                }
            }

            if(indexToReuse.has_value()) {
                out.indices.push_back(indexToReuse.value());
            } else {
                // no index to reuse, create new index and write to vertex buffer

                duplicatedVertex.newIndex = nextIndex;
                out.indices.push_back(nextIndex);
                nextIndex++;
                if(isSkinned) {
                    out.skinnedVertices.emplace_back(duplicatedVertex.vertex);
                } else {
                    out.vertices.emplace_back(duplicatedVertex.vertex);
                }
            }
        }
    }

    static void generateFlatNormals(ExpandedMesh& mesh) {
        std::size_t faceCount = mesh.vertices.size() / 3;
        for(std::size_t face = 0; face < faceCount; face++) {
            Carrot::Vertex& a = mesh.vertices[face * 3 + 0].vertex;
            Carrot::Vertex& b = mesh.vertices[face * 3 + 1].vertex;
            Carrot::Vertex& c = mesh.vertices[face * 3 + 2].vertex;

            glm::vec3 ab = (b.pos - a.pos).xyz;
            glm::vec3 bc = (c.pos - b.pos).xyz;
            glm::vec3 ac = (c.pos - a.pos).xyz;

            if(glm::dot(ab, ab) <= Epsilon
            || glm::dot(bc, bc) <= Epsilon
            || glm::dot(ac, ac) <= Epsilon
            ) {
                Carrot::Log::warn("Degenerate triangle (face = %llu)", face);
            }

            a.normal = glm::normalize(glm::cross(ab, ac));
            b.normal = glm::normalize(glm::cross(bc, -ab));
            c.normal = glm::normalize(glm::cross(ac, -bc));
        }
    }

    static void generateMikkTSpaceTangents(ExpandedMesh& mesh) {
        bool r = generateTangents(mesh);
        if(!r) {
            Carrot::Log::error("Could not generate tangents for mesh");
        }
    }

    /**
     * If tangents are colinear with normals, make tangent follow an edge of the triangle. This case can happen when
     * applying Mikkt-Space with no UV mapping (either inside 'generateMikkTSpaceTangents' or other tools, eg Blender)
     * @param mesh the mesh to clean up
     */
    static void cleanupTangents(ExpandedMesh& mesh) {
        verify(mesh.vertices.size() % 3 == 0, "Only triangle meshs are supported");
        bool needsRegeneration = false;
        for (std::size_t i = 0; i < mesh.vertices.size(); i += 3) {
            auto isCloseToCollinear = [](const glm::vec3& a, const glm::vec3& b) {
                constexpr float epsilon = 10e-12f;
                const glm::vec3 rejected = b - glm::dot(a, b) * b;
                return glm::all(glm::lessThan(glm::abs(rejected), glm::vec3(epsilon)));
            };

            for(std::size_t j = 0; j < 3; j++) {
                const glm::vec3& normal = mesh.vertices[i + j].vertex.normal;
                const glm::vec3 tangent = mesh.vertices[i + j].vertex.tangent.xyz;
                needsRegeneration |= isCloseToCollinear(normal, tangent);
            }

            if(needsRegeneration) {
                break;
            }
        }

        if(needsRegeneration) {
            Carrot::Log::warn("Found collinear normals and tangents (maybe due to missing UV mapping), generating basic tangents");
            // regenerate all tangents for this mesh, we found collinear normal and tangents
            for (std::size_t i = 0; i < mesh.vertices.size(); i += 3) {
                const glm::vec3 edge = mesh.vertices[i + 1].vertex.pos.xyz - mesh.vertices[i + 0].vertex.pos.xyz;
                for (std::size_t j = 0; j < 3; j++) {
                    glm::vec4& tangentW = mesh.vertices[i + j].vertex.tangent;
                    tangentW = glm::vec4(glm::normalize(edge), 1.0f); // W=1.0f but no thought was put behind this value
                }
            }
        }
    }

    struct MeshletGroup {
        std::vector<std::size_t> meshlets;
    };
    static std::vector<MeshletGroup> groupMeshlets(LoadedPrimitive& primitive, std::span<Meshlet> meshlets) {
        // ===== Build meshlet connections
        auto groupWithAllMeshlets = [&]() {
            MeshletGroup group;
            for (int i = 0; i < meshlets.size(); ++i) {
                group.meshlets.push_back(i);
            }
            return std::vector { group };
        };
        if(meshlets.size() < 8) {
            return groupWithAllMeshlets();
        }

        /**
         * Connections betweens meshlets
         */
        struct MeshletEdge {
            explicit MeshletEdge(std::size_t a, std::size_t b): first(std::min(a, b)), second(std::max(a, b)) {}

            bool operator==(const MeshletEdge& other) const = default;

            const std::size_t first;
            const std::size_t second;
        };

        struct MeshletEdgeHasher {
            std::size_t operator()(const MeshletEdge& edge) const {
                std::size_t h = edge.first;
                Carrot::hash_combine(h, edge.second);
                return h;
            }
        };

        // meshlets represented by their index into 'previousLevelMeshlets'
        std::unordered_map<MeshletEdge, std::vector<std::size_t>, MeshletEdgeHasher> edges2Meshlets;
        std::unordered_map<std::size_t, std::vector<MeshletEdge>> meshlets2Edges;

        // for each meshlet
        for(std::size_t meshletIndex = 0; meshletIndex < meshlets.size(); meshletIndex++) {
            const auto& meshlet = meshlets[meshletIndex];
            auto getVertexIndex = [&](std::size_t index) {
                return primitive.meshletVertexIndices[primitive.meshletIndices[index + meshlet.indexOffset] + meshlet.vertexOffset];
            };

            const std::size_t triangleCount = meshlet.indexCount / 3;
            // for each triangle of the meshlet
            for(std::size_t triangleIndex = 0; triangleIndex < triangleCount; triangleIndex++) {
                // for each edge of the triangle
                for(std::size_t i = 0; i < 3; i++) {
                    MeshletEdge edge { getVertexIndex(i + triangleIndex * 3), getVertexIndex(((i+1) % 3) + triangleIndex * 3) };
                    edges2Meshlets[edge].push_back(meshletIndex);
                    meshlets2Edges[meshletIndex].emplace_back(edge);
                }
            }
        }

        // remove edges which are not connected to 2 different meshlets
        std::erase_if(edges2Meshlets, [&](const auto& pair) {
            return pair.second.size() <= 1;
        });

        if(edges2Meshlets.empty()) {
            return groupWithAllMeshlets();
        }

        // at this point, we have basically built a graph of meshlets, in which edges represent which meshlets are connected together

        std::vector<MeshletGroup> groups;

        idx_t vertexCount = meshlets.size();
        idx_t ncon = 1;
        idx_t nparts = meshlets.size()/4;
        verify(nparts > 1, "Must have at least 2 parts in partition for METIS");
        idx_t options[METIS_NOPTIONS];
        METIS_SetDefaultOptions(options);
        options[METIS_OPTION_OBJTYPE] = METIS_OBJTYPE_CUT;
        options[METIS_OPTION_DBGLVL] = METIS_DBG_INFO;

        std::vector<idx_t> partition;
        partition.resize(vertexCount);

        std::vector<idx_t> xadjacency;
        xadjacency.reserve(meshlets.size() + 1);

        std::vector<idx_t> edgeAdjacency;
        std::vector<idx_t> edgeWeights;

        for(std::size_t meshletIndex = 0; meshletIndex < meshlets.size(); meshletIndex++) {
            std::size_t startIndexInEdgeAdjacency = edgeAdjacency.size();
            for(const auto& edge : meshlets2Edges[meshletIndex]) {
                auto connectionsIter = edges2Meshlets.find(edge);
                if(connectionsIter == edges2Meshlets.end()) {
                    continue;
                }
                const auto& connections = connectionsIter->second;
                for(const auto& connectedMeshlet : connections) {
                    if(connectedMeshlet != meshletIndex) {
                        if(std::find(edgeAdjacency.begin(), edgeAdjacency.end(), connectedMeshlet) == edgeAdjacency.end()) {
                            edgeAdjacency.emplace_back(connectedMeshlet);
                            // TODO: edgeweights
                        }
                    }
                }
            }
            xadjacency.push_back(startIndexInEdgeAdjacency);
        }
        xadjacency.push_back(edgeAdjacency.size());
        verify(xadjacency.size() == meshlets.size() + 1, "unexpected count of vertices for METIS graph?");

        idx_t edgeCut;
        int result = METIS_PartGraphKway(&vertexCount,
                                         &ncon,
                                         xadjacency.data(),
                                         edgeAdjacency.data(),
                                         nullptr, /* vertex weights */
                                         nullptr, /* vertex size */
                                         nullptr,//edgeWeights.data(),
                                         &nparts,
                                         nullptr,
                                         nullptr,
                                         options,
                                         &edgeCut,
                                         partition.data()
                            );

        verify(result == METIS_OK, "Graph partitioning failed!");

        // ===== Group meshlets together
        groups.resize(nparts);
        for(std::size_t i = 0; i < meshlets.size(); i++) {
            idx_t partitionNumber = partition[i];
            groups[partitionNumber].meshlets.push_back(i);
        }
        return groups;
    }

    static void appendMeshlets(LoadedPrimitive& primitive, std::span<std::uint32_t> indexBuffer) {
        constexpr std::size_t maxVertices = 64;
        constexpr std::size_t maxTriangles = 128;
        const float coneWeight = 0.0f; // for occlusion culling, currently unused

        const std::size_t meshletOffset = primitive.meshlets.size();
        const std::size_t vertexOffset = primitive.meshletVertexIndices.size();
        const std::size_t indexOffset = primitive.meshletIndices.size();
        const std::size_t maxMeshlets = meshopt_buildMeshletsBound(indexBuffer.size(), maxVertices, maxTriangles);
        std::vector<meshopt_Meshlet> meshoptMeshlets;
        meshoptMeshlets.resize(maxMeshlets);

        std::vector<unsigned int> meshletVertexIndices;
        std::vector<unsigned char> meshletTriangles;
        meshletVertexIndices.resize(maxMeshlets * maxVertices);
        meshletTriangles.resize(maxMeshlets * maxVertices * 3);

        const std::size_t meshletCount = meshopt_buildMeshlets(meshoptMeshlets.data(), meshletVertexIndices.data(), meshletTriangles.data(), // meshlet outputs
                                                               indexBuffer.data(), indexBuffer.size(), // original index buffer
                                                               &primitive.vertices[0].pos.x, // pointer to position data
                                                               primitive.vertices.size(), // vertex count of original mesh
                                                               sizeof(Carrot::Vertex), // stride
                                                               maxVertices, maxTriangles, coneWeight);
        const meshopt_Meshlet& last = meshoptMeshlets[meshletCount - 1];
        const std::size_t vertexCount = last.vertex_offset + last.vertex_count;
        const std::size_t indexCount = last.triangle_offset + ((last.triangle_count * 3 + 3) & ~3);
        primitive.meshletVertexIndices.resize(vertexOffset + vertexCount);
        primitive.meshletIndices.resize(indexOffset + indexCount);
        primitive.meshlets.resize(meshletOffset + meshletCount); // remove over-allocated meshlets

        Carrot::Async::parallelFor(vertexCount, [&](std::size_t index) {
            primitive.meshletVertexIndices[vertexOffset + index] = meshletVertexIndices[index];
        }, 1024);
        Carrot::Async::parallelFor(indexCount, [&](std::size_t index) {
            primitive.meshletIndices[indexOffset + index] = meshletTriangles[index];
        }, 1024);


        // meshlets are ready, process them in the format used by Carrot:
        Carrot::Async::parallelFor(meshletCount, [&](std::size_t index) {
            auto& meshoptMeshlet = meshoptMeshlets[index];
            auto& carrotMeshlet = primitive.meshlets[meshletOffset + index];

            carrotMeshlet.vertexOffset = vertexOffset + meshoptMeshlet.vertex_offset;
            carrotMeshlet.vertexCount = meshoptMeshlet.vertex_count;

            carrotMeshlet.indexOffset = indexOffset + meshoptMeshlet.triangle_offset;
            carrotMeshlet.indexCount = meshoptMeshlet.triangle_count*3;
        }, 32);
    }

    /**
     * From this primitive's vertex & index buffer, generate meshlets/clusters
     */
    static void generateClusterHierarchy(LoadedPrimitive& primitive) {
        // level 0
        // tell meshoptimizer to generate meshlets
        auto& indexBuffer = primitive.indices;
        std::size_t previousMeshletsStart = 0;
        appendMeshlets(primitive, indexBuffer);

        // level n+1
        const int maxLOD = 25;
        for (int lod = 0; lod < maxLOD; ++lod) {
            float tLod = lod / (float)maxLOD;
            std::span<Meshlet> previousLevelMeshlets = std::span { primitive.meshlets.data() + previousMeshletsStart, primitive.meshlets.size() - previousMeshletsStart };
            if(previousLevelMeshlets.size() <= 1) {
                return; // we have reached the end
            }

            std::vector<MeshletGroup> groups = groupMeshlets(primitive, previousLevelMeshlets);

            // ===== Simplify groups
            const std::size_t newMeshletStart = primitive.meshlets.size();
            for(const auto& group : groups) {
                // meshlets vector is modified during the loop
                previousLevelMeshlets = std::span { primitive.meshlets.data() + previousMeshletsStart, primitive.meshlets.size() - previousMeshletsStart };
                std::vector<unsigned int> groupVertexIndices;

                // add cluster vertices to this group
                for(const auto& meshletIndex : group.meshlets) {
                    const auto& meshlet = previousLevelMeshlets[meshletIndex];
                    std::size_t start = groupVertexIndices.size();
                    groupVertexIndices.resize(start + meshlet.indexCount);
                    for(std::size_t j = 0; j < meshlet.indexCount; j++) {
                        groupVertexIndices[j + start] = primitive.meshletVertexIndices[primitive.meshletIndices[meshlet.indexOffset + j] + meshlet.vertexOffset];
                    }
                }

                // simplify this group
                const float threshold = 0.5f;
                std::size_t targetIndexCount = groupVertexIndices.size() * threshold;
                float targetError = 0.9f * tLod + 0.01f * (1-tLod);
                unsigned int options = meshopt_SimplifyLockBorder; // we want all group borders to be locked (because they are shared between groups)

                std::vector<unsigned int> simplifiedIndexBuffer;
                simplifiedIndexBuffer.resize(groupVertexIndices.size());
                float simplificationError = 0.f;

                std::size_t simplifiedIndexCount = meshopt_simplify(simplifiedIndexBuffer.data(), // output
                                                                    groupVertexIndices.data(), groupVertexIndices.size(), // index buffer
                                                                    &primitive.vertices[0].pos.x, primitive.vertices.size(), sizeof(Carrot::Vertex), // vertex buffer
                                                                    targetIndexCount, targetError, options, &simplificationError
                );
                simplifiedIndexBuffer.resize(simplifiedIndexCount);

                if(simplifiedIndexCount == groupVertexIndices.size()) {
                    continue; // could not simplify this group further
                }

                // ===== Generate meshlets for this group
                appendMeshlets(primitive, simplifiedIndexBuffer);
            }

            for(std::size_t i = newMeshletStart; i < primitive.meshlets.size(); i++) {
                primitive.meshlets[i].lod = lod + 1;
            }
            previousMeshletsStart = newMeshletStart;
        }
    }

    static void processModel(const std::string& modelName, tinygltf::Model& model) {
        GLTFLoader loader{};
        LoadedScene scene = loader.load(model, {});

        for(auto& primitive : scene.primitives) {
            ExpandedMesh expandedMesh = expandMesh(primitive);

            if(!primitive.hadTexCoords) {
                //TODO; // not supported yet
            }

            if(!primitive.hadNormals) {
                Carrot::Log::info("Mesh %s has no normals, generating flat normals...", primitive.name.c_str());
                generateFlatNormals(expandedMesh);
                Carrot::Log::info("Mesh %s, generated flat normals!", primitive.name.c_str());
            }

            if(!primitive.hadTangents) {
                Carrot::Log::info("Mesh %s has no tangents, generating tangents...", primitive.name.c_str());
                generateMikkTSpaceTangents(expandedMesh);
                Carrot::Log::info("Mesh %s, generated tangents!", primitive.name.c_str());
            }

            cleanupTangents(expandedMesh);

            collapseMesh(primitive, expandedMesh);
            generateClusterHierarchy(primitive);
        }

        // re-export model
        tinygltf::Model reexported = std::move(writeAsGLTF(modelName, scene));
        // keep copyright+author info
        model.asset.extras = std::move(model.asset.extras);
        model.asset.copyright = std::move(model.asset.copyright);

        model = std::move(reexported);
    }

    ConversionResult processGLTF(const std::filesystem::path& inputFile, const std::filesystem::path& outputFile) {
        using namespace tinygltf;

        tinygltf::TinyGLTF parser;
        tinygltf::FsCallbacks callbacks;

        callbacks.ReadWholeFile = gltfReadWholeFile;
        callbacks.ExpandFilePath = gltfExpandFilePath;
        callbacks.FileExists = gltfFileExists;
        callbacks.WriteWholeFile = nullptr;

        fspath parentPath = inputFile.parent_path();
        fspath outputParentPath = outputFile.parent_path();
        callbacks.user_data = (void*)&parentPath;
        parser.SetFsCallbacks(callbacks);

        tinygltf::Model model;
        std::string errors;
        std::string warnings;

        if(!parser.LoadASCIIFromFile(&model, &errors, &warnings, inputFile.string())) {
            return {
                .errorCode = ConversionResultError::GLTFCompressionError,
                .errorMessage = errors,
            };
        }

        // ----------

        // buffers are regenerated inside 'processModel' method too, so we don't copy the .bin file
        processModel(Carrot::toString(outputFile.stem().u8string()), model);

        // ----------

        if(!parser.WriteGltfSceneToFile(&model, outputFile.string(), false, false, true/* pretty-print */, false)) {
            return {
                .errorCode = ConversionResultError::GLTFCompressionError,
                .errorMessage = "Could not write GLTF",
            };
        }

        return {
            .errorCode = ConversionResultError::Success,
        };
    }
}
#include <includes/lighting_utils.glsl>
#include <includes/gbuffer_input.glsl>

DEFINE_GBUFFER_INPUTS(0)

#include <includes/gbuffer_unpack.glsl>

const uint LOCAL_SIZE = 22; // computed to fit 'sharedGBufferReads' inside maxComputeSharedMemorySize for RTX 3070 & avoid device remove??
layout (local_size_x = LOCAL_SIZE) in;
layout (local_size_y = LOCAL_SIZE) in;

layout(rgba32f, set = 1, binding = 0) uniform readonly image2D inputImage;
layout(rgba32f, set = 1, binding = 1) uniform writeonly image2D outputImage;
layout(rgba32f, set = 1, binding = 2) uniform readonly image2D lastFrameMomentHistoryHistoryLength;

layout(push_constant) uniform Push {
    int index;
} iterationData;

const int FILTER_RADIUS = 2;
const float KERNEL_WEIGHTS[FILTER_RADIUS*2+1] = {
    1.0f/16.0f,
    1.0f/4.0f,
    3.0f/8.0f,
    1.0f/4.0f,
    1.0f/16.0f,
};

// used to workaround maxComputeSharedMemorySize
struct ExtractedGBuffer {
    vec4 albedo;
    vec3 viewPosition;
    vec3 normal;
    uvec4 entityID;
};

ExtractedGBuffer extractUsefulInfo(in GBuffer g) {
    ExtractedGBuffer e;
    e.albedo = g.albedo;
    e.viewPosition = g.viewPosition;
    e.normal = g.viewTBN[2];
    e.entityID = g.entityID;
    return e;
}

shared ExtractedGBuffer sharedGBufferReads[LOCAL_SIZE][LOCAL_SIZE];

ExtractedGBuffer readGBuffer(ivec2 coordsOffset/* offset from current pixel */) {

    ivec2 localCoords = ivec2(gl_LocalInvocationID.xy) + coordsOffset;
    if(localCoords.x >= 0 && localCoords.x < LOCAL_SIZE
    && localCoords.y >= 0 && localCoords.y < LOCAL_SIZE) {
        return sharedGBufferReads[localCoords.x][localCoords.y];
    }

    const ivec2 size = imageSize(outputImage);
    const ivec2 coords = ivec2(gl_GlobalInvocationID) + coordsOffset;

    if(coords.x >= size.x
    || coords.y >= size.y
    || coords.x < 0
    || coords.y < 0
    ) {
        ExtractedGBuffer null;
        return null;
    }

    const vec2 filterUV = coords / vec2(size);
    return extractUsefulInfo(unpackGBuffer(filterUV));
}

void main() {
    // A-Trous filter
    const ivec2 coords = ivec2(gl_GlobalInvocationID);

    // from SVGF
    const float sigmaNormals = 128.0f;
    const float sigmaPositions = 1.0f;
    const float sigmaLuminance = 1.0f;

    const int STEP_SIZE = 1 << iterationData.index;

    const ivec2 size = imageSize(outputImage);

    if(coords.x >= size.x
    || coords.y >= size.y) {
        return;
    }
    vec4 finalPixel = imageLoad(inputImage, coords);

    const vec2 currentUV = coords/vec2(size);
    sharedGBufferReads[gl_LocalInvocationID.x][gl_LocalInvocationID.y] = extractUsefulInfo(unpackGBufferLight(currentUV));
    #define currentGBuffer (sharedGBufferReads[gl_LocalInvocationID.x][gl_LocalInvocationID.y])

    barrier();

    if(currentGBuffer.albedo.a <= 1.0f / 256.0f) {
        imageStore(outputImage, coords, finalPixel);
        return;
    }

    float baseLuminance = luminance(finalPixel.rgb);

    const vec3 currentPosition = currentGBuffer.viewPosition;
    float totalWeight = KERNEL_WEIGHTS[FILTER_RADIUS] * KERNEL_WEIGHTS[FILTER_RADIUS];
    finalPixel *= totalWeight;

    for(int dy = -FILTER_RADIUS; dy <= FILTER_RADIUS; dy++) {
        const float yKernelWeight = KERNEL_WEIGHTS[dy+FILTER_RADIUS];
        for(int dx = -FILTER_RADIUS; dx <= FILTER_RADIUS; dx++) {
            if(dx == 0 && dy == 0) {
                continue;
            }
            const ivec2 dCoord = ivec2(dx, dy) * STEP_SIZE;
            const ivec2 filterCoord = coords + dCoord;

            if(filterCoord.x < 0 || filterCoord.y < 0
            || filterCoord.x >= size.x || filterCoord.y >= size.y) {
                continue;
            }

            const float filterWeight = KERNEL_WEIGHTS[dx+FILTER_RADIUS] * yKernelWeight;
            const ExtractedGBuffer filterGBuffer = readGBuffer(dCoord);
            // TODO: use index for mesh on top of entity (for instance curtains of Sponza)
            vec4 filterPixel = imageLoad(inputImage, filterCoord);
            if(filterGBuffer.albedo.a <= 1.0f/256.0f) {
                continue;
            }
            const float sameMeshWeight = currentGBuffer.entityID == filterGBuffer.entityID ? 1.0f : 0.0f;
            const float normalWeight = pow(max(0, dot(filterGBuffer.normal, currentGBuffer.normal)), sigmaNormals);

            const vec3 filterPosition = filterGBuffer.viewPosition;
            const vec3 dPosition = filterPosition - currentPosition;
            const float distanceSquared = dot(dPosition, dPosition);
            const float positionWeight = min(1, exp(-distanceSquared/sigmaPositions));

            const float filterLuminance = luminance(filterPixel.rgb);
            const float luminanceWeight = exp(-abs(filterLuminance-baseLuminance)*sigmaLuminance);

            const float weight = sameMeshWeight * normalWeight * positionWeight * filterWeight * luminanceWeight;
            finalPixel += weight * filterPixel;
            totalWeight += weight;
        }
    }

    finalPixel /= totalWeight;
    imageStore(outputImage, coords, finalPixel);
}
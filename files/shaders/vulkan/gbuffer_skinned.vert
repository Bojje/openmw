#version 460

// The G-buffer vertex shader for skinned geometry. Identical to gbuffer.vert except that the vertex
// is posed by a weighted blend of bone matrices before the model matrix is applied.
//
// A separate shader rather than a branch in gbuffer.vert because the difference is not in the code
// but in the vertex input: this pipeline declares a second vertex binding, and a pipeline that
// declares one requires every draw to bind it. Rigid geometry has no second buffer to bind.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inColor;

// Binding 1, eight bytes a vertex. Indices are into this shape's own bone list, which the CPU has
// already compacted and offset by boneOffset; weights arrive as unorm bytes summing to one.
layout(location = 4) in uvec4 inBoneIndices;
layout(location = 5) in vec4 inBoneWeights;

// Must match Vk::GBufferPushConstants and the block in gbuffer.frag exactly.
// A push-constant mat3 occupies 48 bytes: three columns, each padded out to a vec4.
//   model         offset   0, 64 bytes
//   normalMatrix  offset  64, 48 bytes
//   materialBits  offset 112,  4 bytes -- sampler slot plus the authored alpha test, unpacked in
//                                         gbuffer.frag; see Vk::packMaterialBits
// 128 bytes total, exactly the guaranteed limit.
layout(push_constant) uniform PushConstants {
    layout(offset = 0)   mat4 model;
    layout(offset = 64)  mat3 normalMatrix;
    layout(offset = 112) uint materialBits;
    layout(offset = 116) float roughness;
    layout(offset = 120) float specularStrength;
    layout(offset = 124) uint boneOffset;
} push;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
} camera;

// One flat array for every skinned shape in the frame. boneOffset is where this shape's palette
// starts in it, which is why an eight-bit per-vertex index is enough however many actors are on
// screen.
layout(set = 0, binding = 2, std430) readonly buffer BoneMatrices {
    mat4 bones[];
} skin;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out vec4 fragColor;

void main() {
    // Renormalised rather than assumed to sum to one. A vertex the NIF gave no influences at all has
    // four zero weights, and blending those produces a zero matrix that collapses it onto the
    // skeleton's origin -- a spike running to the actor's feet, which is what this guards against.
    float totalWeight = dot(inBoneWeights, vec4(1.0));
    mat4 skinMatrix = mat4(1.0);
    if (totalWeight > 0.0)
    {
        skinMatrix = inBoneWeights.x * skin.bones[push.boneOffset + inBoneIndices.x]
                   + inBoneWeights.y * skin.bones[push.boneOffset + inBoneIndices.y]
                   + inBoneWeights.z * skin.bones[push.boneOffset + inBoneIndices.z]
                   + inBoneWeights.w * skin.bones[push.boneOffset + inBoneIndices.w];
        skinMatrix /= totalWeight;
    }

    vec4 posed = skinMatrix * vec4(inPosition, 1.0);
    vec4 worldPos = push.model * posed;
    fragWorldPos = worldPos.xyz;

    // The skin matrix goes in as a plain 3x3 rather than as its own inverse transpose. Bone matrices
    // in Morrowind's skeletons are rotations and translations with a uniform scale at most, and for
    // those the two agree up to a length the normalize below removes anyway.
    fragNormal = normalize(push.normalMatrix * (mat3(skinMatrix) * inNormal));
    fragTexCoord = inTexCoord;
    fragColor = inColor;
    gl_Position = camera.projection * camera.view * worldPos;
}
